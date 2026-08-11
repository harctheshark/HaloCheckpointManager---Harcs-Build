#include "pch.h"
#include "HCECheckpointBlob.h"
#include "PointerDataStore.h"
#include "HCEGetPlayerState.h"
#include <chrono>
#include <cstring>
#include <climits>
#include <algorithm>
#include <bcrypt.h>

// See HCECheckpointBlob.h for the layout, the publication barrier, and why the PROVIDER path has to be handled.

namespace
{
	// BCryptFinishHash is the only place a raw NTSTATUS reaches us; ntdef.h's NT_SUCCESS is not pulled in by
	// windows.h, and redefining it would collide wherever it is.
	inline bool bcryptOk(NTSTATUS status) { return status >= 0; }

	// RAII for the two BCrypt handles, so a throw between open and close cannot leak them. Plain C++ destructors,
	// no SEH anywhere in this file, so unwinding is fine (see the C2712 note in HCEGetPlayerState.cpp).
	// ⚠ The "Scoped" prefix is load-bearing: bcrypt.h declares a FUNCTION called BCryptHash (the one-shot hash
	// API), so a type of that name is an ambiguous symbol even inside an anonymous namespace (C2872).
	struct ScopedBCryptAlgorithm
	{
		BCRYPT_ALG_HANDLE handle = nullptr;
		~ScopedBCryptAlgorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
	};
	struct ScopedBCryptHash
	{
		BCRYPT_HASH_HANDLE handle = nullptr;
		~ScopedBCryptHash() { if (handle) BCryptDestroyHash(handle); }
	};
}

uintptr_t HCECheckpointBlob::resolveOrThrow(const std::shared_ptr<MultilevelPointer>& mlp, const char* what)
{
	if (!mlp) throw HCMRuntimeException(std::format("No HaloCER pointer data for {}", what));
	uintptr_t address = 0;
	if (!mlp->resolve(&address) || address == 0)
		throw HCMRuntimeException(std::format("Could not resolve HaloCER {}: {}", what, MultilevelPointer::GetLastError()));
	return address;
}


HCECheckpointBlob::HCECheckpointBlob(GameState game, IDIContainer& dicon)
	: mGame(game)
{
	// explicit cast: GameState has both operator==(GameState) and operator Value(), so comparing a GameState
	// straight against a Value is ambiguous (C2666).
	if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
		throw HCMInitException("HCECheckpointBlob only supports Halo Campaign Evolved");

	auto ptr = dicon.Resolve<PointerDataStore>().lock();
	if (!ptr) throw HCMInitException("HCECheckpointBlob could not resolve the PointerDataStore");

	mControlBlock = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointControlBlock), mGame);
	mSlotTable = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointSlotTable), mGame);
	mStateLength = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointStateLength), mGame);
	mSaveInFlight = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointSaveInFlight), mGame);
	mShell = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointShell), mGame);
	mStateBlock = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointStateBlock), mGame);

	mSHAOffset = *ptr->getData<std::shared_ptr<int64_t>>(nameof(hceCheckpointSHAOffset), mGame);
	mSHALength = *ptr->getData<std::shared_ptr<int64_t>>(nameof(hceCheckpointSHALength), mGame);
	mHeaderLength = *ptr->getData<std::shared_ptr<int64_t>>(nameof(hceCheckpointHeaderLength), mGame);
}


uintptr_t HCECheckpointBlob::getSimModuleBase()
{
	HMODULE sim = GetModuleHandleW(mGame.toModuleName().c_str());
	if (!sim) throw HCMRuntimeException("HaloSimulation_tag_release.dll is not loaded");
	return (uintptr_t)sim;
}


// ====================================================================================================================
// COMMON TO BOTH STORAGE PATHS
// ====================================================================================================================
HCECheckpointBlob::StorageMode HCECheckpointBlob::detectStorageMode()
{
	// The engine's own branch, transcribed:
	//     19D4CF  mov rax, [0x2C40028]     ; the shell
	//     19D4DD  test rax, rax
	//     19D4E0  je   local
	//     19D4E2  mov rsi, [rax + 0x10]    ; the provider
	//     19D4E6  test rsi, rsi
	//     19D4E9  je   local
	const uintptr_t shellSlot = resolveOrThrow(mShell, nameof(hceCheckpointShell));

	uintptr_t shell = 0;
	if (!HCEGetPlayerState::tryReadRaw(shellSlot, &shell, sizeof(shell)))
		throw HCMRuntimeException("Could not read Halo Campaign Evolved's engine shell pointer.");
	if (shell == 0) return StorageMode::Local;

	uintptr_t provider = 0;
	if (!HCEGetPlayerState::tryReadRaw(shell + kShellProvider, &provider, sizeof(provider)))
		throw HCMRuntimeException("Could not read Halo Campaign Evolved's checkpoint storage provider pointer.");

	return provider == 0 ? StorageMode::Local : StorageMode::Provider;
}


int32_t HCECheckpointBlob::readStateLength()
{
	const uintptr_t lengthAddress = resolveOrThrow(mStateLength, nameof(hceCheckpointStateLength));

	int32_t length = 0;
	if (!HCEGetPlayerState::tryReadRaw(lengthAddress, &length, sizeof(length)))
		throw HCMRuntimeException("Could not read the HaloCER game state length");

	if (length <= 0 || length > kMaxPlausibleLength)
		throw HCMRuntimeException(std::format(
			"Halo Campaign Evolved reported an implausible game state length (0x{:X}) - this build of the game may not be supported.",
			(uint32_t)length));

	return length;
}


uintptr_t HCECheckpointBlob::readStateBlockAddress()
{
	const uintptr_t slot = resolveOrThrow(mStateBlock, nameof(hceCheckpointStateBlock));

	uintptr_t block = 0;
	if (!HCEGetPlayerState::tryReadRaw(slot, &block, sizeof(block)) || block == 0)
		throw HCMRuntimeException(
			"Halo Campaign Evolved has no game state loaded right now - load a level first.");

	return block;
}


std::vector<byte> HCECheckpointBlob::readLiveHeader()
{
	if (mHeaderLength <= 0 || mHeaderLength > kMaxPlausibleLength)
		throw HCMRuntimeException(std::format(
			"HCM has an implausible HaloCER checkpoint header length (0x{:X}).", (uint64_t)mHeaderLength));

	// The header can never be longer than the state it lives at the front of.
	const int32_t length = readStateLength();
	if ((int64_t)length < mHeaderLength)
		throw HCMRuntimeException(std::format(
			"Halo Campaign Evolved's game state (0x{:X} bytes) is smaller than one checkpoint header (0x{:X}).",
			(uint32_t)length, (uint64_t)mHeaderLength));

	const uintptr_t block = readStateBlockAddress();

	std::vector<byte> header;
	header.resize((size_t)mHeaderLength);
	if (!HCEGetPlayerState::tryReadRaw(block, header.data(), header.size()))
		throw HCMRuntimeException(std::format(
			"Could not read Halo Campaign Evolved's live session header (0x{:X}, 0x{:X} bytes).",
			(uint64_t)block, (uint64_t)header.size()));

	return header;
}


// ====================================================================================================================
// LOCAL PATH ONLY
// ====================================================================================================================
bool HCECheckpointBlob::isLocalStorage()
{
	// The runtime discriminator: on the PROVIDER path the engine never allocates the two in-process buffers, so
	// slot 0's pointer stays null (and it hardcodes the valid bytes to 1, which is exactly why the valid byte on
	// its own is not enough to trust). Deliberately tests SLOT 0 rather than the current slot - the current slot
	// can legitimately be null-ish garbage if the ring index is nonsense, and this question is about the whole
	// storage mode, not about one entry.
	uintptr_t slotTable = 0;
	try { slotTable = resolveOrThrow(mSlotTable, nameof(hceCheckpointSlotTable)); }
	catch (HCMRuntimeException) { return false; }

	uintptr_t firstBuffer = 0;
	if (!HCEGetPlayerState::tryReadRaw(slotTable, &firstBuffer, sizeof(firstBuffer))) return false;
	return firstBuffer != 0;
}


void HCECheckpointBlob::requireLocalStorage()
{
	if (isLocalStorage()) return;
	throw HCMRuntimeException(
		"Halo Campaign Evolved is not keeping its checkpoints in this process, so HCM cannot read or write them.\n"
		"(The engine was started with an external checkpoint provider - there is no in-process checkpoint buffer.)");
}


HCECheckpointBlob::Snapshot HCECheckpointBlob::readSnapshot()
{
	// Resolved every call rather than cached: a MultilevelPointer resolution is a module-cache lookup plus an add,
	// and caching it would mean a module reload could leave us reading a stale address.
	const uintptr_t control = resolveOrThrow(mControlBlock, nameof(hceCheckpointControlBlock));
	const uintptr_t slotTable = resolveOrThrow(mSlotTable, nameof(hceCheckpointSlotTable));
	const uintptr_t lengthAddress = resolveOrThrow(mStateLength, nameof(hceCheckpointStateLength));
	const uintptr_t inFlightAddress = resolveOrThrow(mSaveInFlight, nameof(hceCheckpointSaveInFlight));

	Snapshot out;

	// Read the in-flight dword FIRST and the slot's valid byte LAST. The engine publishes in the order
	// (slot.valid = 1, then inFlight = 0), so sampling in the reverse order cannot manufacture a "published"
	// reading out of a save that is still running.
	int32_t inFlight = 0;
	if (!HCEGetPlayerState::tryReadRaw(inFlightAddress, &inFlight, sizeof(inFlight)))
		throw HCMRuntimeException("Could not read the HaloCER checkpoint save state");
	out.saveIdle = (inFlight == 0);

	uint8_t initialised = 0;
	uint8_t exists = 0;
	if (!HCEGetPlayerState::tryReadRaw(control + kControlInitialised, &initialised, sizeof(initialised))
		|| !HCEGetPlayerState::tryReadRaw(control + kControlExists, &exists, sizeof(exists))
		|| !HCEGetPlayerState::tryReadRaw(control + kControlRingSize, &out.ringSize, sizeof(out.ringSize))
		|| !HCEGetPlayerState::tryReadRaw(control + kControlRingIndex, &out.ringIndex, sizeof(out.ringIndex)))
		throw HCMRuntimeException("Could not read the HaloCER checkpoint control block");

	out.systemInitialised = (initialised != 0);
	out.anyCheckpointExists = (exists != 0);

	if (!HCEGetPlayerState::tryReadRaw(lengthAddress, &out.length, sizeof(out.length)))
		throw HCMRuntimeException("Could not read the HaloCER game state length");

	// A ring index outside the engine's own ring size would index past the slot table, so refuse it rather than
	// reading whatever follows. ringSize is 2 on this build; the upper bound is defensive, not a constraint.
	if (out.ringSize <= 0 || out.ringSize > 8) return out;
	if (out.ringIndex < 0 || out.ringIndex >= out.ringSize) return out;

	out.slotAddress = slotTable + (uintptr_t)out.ringIndex * kSlotStride;

	if (!HCEGetPlayerState::tryReadRaw(out.slotAddress, &out.buffer, sizeof(out.buffer)))
	{
		out.buffer = 0;
		return out;
	}

	uint8_t valid = 0;
	if (HCEGetPlayerState::tryReadRaw(out.slotAddress + kSlotValid, &valid, sizeof(valid)))
		out.slotValid = (valid != 0);

	return out;
}


// Everything that waiting cannot possibly fix: no level, no checkpoint yet, a nonsense ring index or length.
// Deliberately separate from the barrier test, so the wait loop below only ever spins on the one condition that
// genuinely resolves itself (the SHA-1 worker finishing).
void HCECheckpointBlob::throwIfUnusable(const Snapshot& snapshot)
{
	if (!snapshot.systemInitialised)
		throw HCMRuntimeException("Halo Campaign Evolved's checkpoint system is not running yet - load a level first.");
	if (!snapshot.anyCheckpointExists)
		throw HCMRuntimeException("Halo Campaign Evolved has no checkpoint yet - reach one (or use Force Checkpoint) first.");
	if (snapshot.length <= 0 || snapshot.length > kMaxPlausibleLength)
		throw HCMRuntimeException(std::format(
			"Halo Campaign Evolved reported an implausible game state length (0x{:X}) - this build of the game may not be supported.",
			(uint32_t)snapshot.length));
	if (snapshot.buffer == 0)
		throw HCMRuntimeException(std::format(
			"Halo Campaign Evolved's checkpoint slot {} has no buffer.", snapshot.ringIndex));
}


HCECheckpointBlob::Snapshot HCECheckpointBlob::requirePublishedSnapshot()
{
	requireLocalStorage();
	const Snapshot snapshot = readSnapshot();
	throwIfUnusable(snapshot);

	if (!snapshot.published())
		throw HCMRuntimeException("Halo Campaign Evolved is still writing a checkpoint - try again in a moment.");

	return snapshot;
}


std::vector<byte> HCECheckpointBlob::readCurrentCheckpoint(Snapshot& outSnapshot)
{
	requireLocalStorage();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBarrierWaitMilliseconds);
	std::vector<byte> data;

	for (;;)
	{
		const Snapshot before = waitForPublishedSnapshot(deadline);

		data.resize((size_t)before.length);
		if (!HCEGetPlayerState::tryReadRaw(before.buffer, data.data(), data.size()))
			throw HCMRuntimeException(std::format(
				"Could not read Halo Campaign Evolved's checkpoint buffer (0x{:X}, 0x{:X} bytes).",
				(uint64_t)before.buffer, (uint64_t)data.size()));

		// RE-READ THE BARRIER. If the engine started (or finished) a save while the copy was running, the bytes we
		// hold are a mix of two game states and their digest is meaningless - go round again.
		const Snapshot after = readSnapshot();
		if (after.ringIndex == before.ringIndex
			&& after.buffer == before.buffer
			&& after.length == before.length
			&& after.slotValid && after.saveIdle)
		{
			outSnapshot = after;
			return data;
		}

		if (std::chrono::steady_clock::now() > deadline)
			throw HCMRuntimeException("Halo Campaign Evolved kept saving over its checkpoint while HCM was reading it - try again.");
	}
}


// Waits out an in-flight save. The SHA-1 runs on a worker task, so "the game thread finished the memcpy" and
// "the checkpoint is usable" are different moments and the gap is genuinely worth waiting through rather than
// telling the user to press the key again.
HCECheckpointBlob::Snapshot HCECheckpointBlob::waitForPublishedSnapshot(std::chrono::steady_clock::time_point deadline)
{
	for (;;)
	{
		const Snapshot snapshot = readSnapshot();
		throwIfUnusable(snapshot);

		if (snapshot.published()) return snapshot;

		if (std::chrono::steady_clock::now() > deadline)
			throw HCMRuntimeException("Halo Campaign Evolved is still writing a checkpoint - try again in a moment.");

		Sleep(2);
	}
}


// ====================================================================================================================
// THE WRITE PATH
// ====================================================================================================================
void HCECheckpointBlob::writeCheckpoint(const Snapshot& expected, const std::vector<byte>& data, const std::vector<byte>& previousContents)
{
	requireLocalStorage();

	if (expected.buffer == 0 || expected.length <= 0)
		throw HCMRuntimeException("HCM does not have a valid Halo Campaign Evolved checkpoint slot to inject into.");

	// Belt and braces - the caller checks this against the game's own length global too, but a mismatch here would
	// be a partial overwrite of a live checkpoint, which is exactly the failure this whole class exists to avoid.
	if (data.size() != (size_t)expected.length)
		throw HCMRuntimeException(std::format(
			"Refusing to inject: the data is 0x{:X} bytes but the checkpoint slot is 0x{:X}.",
			(uint64_t)data.size(), (uint32_t)expected.length));

	// RE-READ THE BARRIER BEFORE TOUCHING ANYTHING. Validation and the digest take a few tens of milliseconds, and
	// the game may have checkpointed in that window. Refusing here costs the user one keypress; writing into a slot
	// the engine has moved on from, or is in the middle of, costs them the level.
	const Snapshot before = readSnapshot();
	throwIfUnusable(before);
	if (!before.published()
		|| before.ringIndex != expected.ringIndex
		|| before.buffer != expected.buffer
		|| before.length != expected.length)
		throw HCMRuntimeException(
			"Halo Campaign Evolved saved a checkpoint while HCM was preparing the injection.\n"
			"NOTHING was written - press inject again.");

	const bool canRollBack = (previousContents.size() == (size_t)expected.length);
	const bool wrote = HCEGetPlayerState::tryWriteRaw(expected.buffer, data.data(), data.size());

	// Either of these means the slot may now hold a mix of two blobs, whose digest is nobody's - and a checkpoint
	// that fails verification does not fail softly on this game, it RESTARTS THE LEVEL. Put the bytes we found
	// there back; they are a checkpoint the engine wrote itself, so they are self-consistent by construction.
	const Snapshot after = readSnapshot();
	const bool ringHeld = (after.ringIndex == before.ringIndex
		&& after.buffer == before.buffer
		&& after.length == before.length
		&& after.slotValid && after.saveIdle);

	if (wrote && ringHeld) return;

	bool rolledBack = false;
	if (canRollBack)
		rolledBack = HCEGetPlayerState::tryWriteRaw(before.buffer, previousContents.data(), previousContents.size());

	PLOG_ERROR << std::format(
		"HaloCER checkpoint injection aborted: wrote={} ringHeld={} rolledBack={} (slot {} buffer 0x{:X})",
		wrote, ringHeld, rolledBack, before.ringIndex, (uint64_t)before.buffer);

	if (!wrote)
		throw HCMRuntimeException(std::format(
			"Could not write to Halo Campaign Evolved's checkpoint buffer (0x{:X}, 0x{:X} bytes).\n{}",
			(uint64_t)expected.buffer, (uint64_t)data.size(),
			rolledBack ? "The previous checkpoint was put back."
			: "WARNING: THE PREVIOUS CHECKPOINT COULD NOT BE PUT BACK - make a new checkpoint before you revert."));

	throw HCMRuntimeException(std::format(
		"Halo Campaign Evolved saved a checkpoint while HCM was writing the injected one, so the injection was abandoned.\n{}",
		rolledBack ? "The previous checkpoint was put back - press inject again."
		: "WARNING: THE PREVIOUS CHECKPOINT COULD NOT BE PUT BACK - make a new checkpoint before you revert."));
}


// ====================================================================================================================
// THE DIGEST - see the comment on applySHA1 in the header for the recipe and for MCC's reserve()/resize() bug.
// ====================================================================================================================
void HCECheckpointBlob::applySHA1(std::vector<byte>& blob) const
{
	if (mSHALength <= 0 || mSHAOffset < 0 || (uint64_t)(mSHAOffset + mSHALength) > blob.size())
		throw HCMRuntimeException(std::format(
			"Checkpoint is too small to hold its own checksum (0x{:X} bytes, checksum wanted at 0x{:X}).",
			(uint64_t)blob.size(), (uint64_t)mSHAOffset));

	// BCryptHashData takes a ULONG. Every plausible length fits, but assert it rather than truncate silently.
	if (blob.size() > (size_t)ULONG_MAX)
		throw HCMRuntimeException("Checkpoint is far too large to hash.");

	// 1. Zero EXACTLY mSHALength bytes. (MCC's loop runs `it <= begin + offset + length`, which clears 21.)
	std::memset(blob.data() + mSHAOffset, 0, (size_t)mSHALength);

	ScopedBCryptAlgorithm algorithm;
	if (!bcryptOk(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA1_ALGORITHM, nullptr, 0)))
		throw HCMRuntimeException("Could not open a SHA-1 provider to checksum the checkpoint.");

	ScopedBCryptHash hash;
	if (!bcryptOk(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0)))
		throw HCMRuntimeException("Could not create a SHA-1 hash to checksum the checkpoint.");

	// 2. Hash the WHOLE blob, digest field (now zeroes) and salt included.
	if (!bcryptOk(BCryptHashData(hash.handle, (PUCHAR)blob.data(), (ULONG)blob.size(), 0)))
		throw HCMRuntimeException("Failed hashing the checkpoint.");

	// 3. resize(), NOT reserve() - this is the exact line MCC gets wrong, and getting it wrong produces an
	//    all-zero digest that the game rejects, i.e. a level restart.
	std::vector<byte> digest;
	digest.resize((size_t)mSHALength);
	if (!bcryptOk(BCryptFinishHash(hash.handle, (PUCHAR)digest.data(), (ULONG)digest.size(), 0)))
		throw HCMRuntimeException("Failed finishing the checkpoint checksum.");

	// 4. Paste it back.
	std::memcpy(blob.data() + mSHAOffset, digest.data(), digest.size());
}


// ====================================================================================================================
// THE HEADER COMPARISON
// ====================================================================================================================
std::string HCECheckpointBlob::readBoundedString(const std::vector<byte>& blob, int64_t offset, size_t maxLength)
{
	if (offset < 0 || (uint64_t)offset >= blob.size()) return {};
	const size_t available = (std::min)(maxLength, blob.size() - (size_t)offset);
	const char* start = (const char*)blob.data() + offset;

	size_t length = 0;
	while (length < available && start[length] != '\0') ++length;

	return std::string(start, length);
}


// Only printable ASCII survives. Display only - a hand-edited file can put anything in these fields, and they end
// up in an ImGui dialog. NEVER compare these: two different pieces of garbage sanitise to the same '?' run.
std::string HCECheckpointBlob::printableCopy(const std::string& raw)
{
	std::string out;
	out.reserve(raw.size());
	for (unsigned char c : raw) out.push_back((c >= 0x20 && c < 0x7F) ? (char)c : '?');
	return out;
}


bool HCECheckpointBlob::compareGameOptions(const byte* a, const byte* b, size_t available, std::string& detail)
{
	// Offsets below are into the options struct itself, i.e. blob + 0x130. A direct transcription of
	// sub_1802096E0:
	//
	//     sub_180382100 canonicalises the string at +0x44 into a 0x100-byte scratch on each side, then those two
	//         scratches are compared - it is a map name, and the canonicaliser strips the directory prefix
	//     byte  +0x000    game type; also the switch selector for everything below
	//     word  +0x006
	//     byte  +0x1D4    only when the type byte is 1
	//     sub_180216460(+0x15E0) when the type byte is 1, 2 or 3; any other type returns "compatible" right here
	//
	// and sub_180216460(A, B) is: the dword at A+0 must equal B+0, then memcmp(A+0xC, B+0xC, n) with n selected
	// off that dword - 1 -> 0xF888, 2 -> 0xF858, 3 -> 0x830, 4 -> 0x9C8, 0 or >4 -> 0x730.
	//
	// ⚠ THAT TRAILING BLOCK IS *NOT* "campaign difficulty, skulls", which is what this comment used to claim
	// and what the dialog told users for months. DIFFICULTY IS THE BYTE AT +0x1D4 ABOVE, and it measured 0x00
	// in all eleven checkpoints ever examined. The trailing block is a GAME ENGINE VARIANT object whose base
	// is A+4 (so payload offset P == object offset P+8), and the one byte ever observed to differ inside it
	// was object +0x2F8 - the low byte of a five-bit flags field written by initialize_defaults
	// (sub_180395950 stores 0x00070000 at +0x2F6), read only by the encoder/decoder/copy and five boolean
	// property getters. Nothing anywhere in the sim compares it against 6 or 7 as an integer.
	//
	// ⚠ A+4..A+0xB is a VTABLE POINTER and differs on every launch through ASLR. That is precisely why the
	// engine's compare starts at +0xC. Never widen this range to include it.
	constexpr size_t kType = 0x000;
	constexpr size_t kWordField = 0x006;
	constexpr size_t kMapName = 0x044;
	constexpr size_t kMapNameMax = 0x100;
	constexpr size_t kTypeOneField = 0x1D4;
	constexpr size_t kSubBlock = 0x15E0;
	constexpr size_t kSubBlockPayload = 0x00C;

	detail.clear();

	// A field we cannot read is reported as matching. The caller has already hard-refused any blob that is not
	// exactly the engine's own state length, so this only fires on a build whose header shrank - in which case a
	// scary dialog about "skulls" would be a lie, and the real problem is elsewhere.
	auto readable = [available](size_t offset, size_t size) { return offset + size <= available; };

	// The RAW string, deliberately, rather than a reimplementation of sub_180382100. That canonicaliser has two
	// branches - it strips the directory prefix when the input contains a particular marker substring, and copies
	// the string verbatim when it does not - so a leaf-name comparison would be LOOSER than the engine on the
	// verbatim branch, and "looser than the engine" is precisely the failure that loses a run. A raw compare is at
	// least as strict as both branches. It can therefore warn where the engine would have accepted (two spellings
	// of the same map), which costs the user one "Continue" click; two blobs from the same build carry
	// byte-identical strings here anyway.
	auto field = [](const byte* p, size_t max)
		{
			size_t length = 0;
			while (length < max && p[length] != '\0') ++length;
			return std::string((const char*)p, length);
		};

	if (readable(kMapName, kMapNameMax)
		&& field(a + kMapName, kMapNameMax) != field(b + kMapName, kMapNameMax))
	{
		detail = "map";
		return false;
	}

	if (readable(kType, 1) && a[kType] != b[kType]) { detail = "game type"; return false; }

	if (readable(kWordField, 2)
		&& std::memcmp(a + kWordField, b + kWordField, 2) != 0) { detail = "session flags"; return false; }

	if (!readable(kType, 1)) return true;

	// movsx in the original, so the selector is a SIGNED byte.
	const int32_t type = (int32_t)(int8_t)a[kType];

	if (type == 1 && readable(kTypeOneField, 1) && a[kTypeOneField] != b[kTypeOneField])
	{
		detail = "difficulty";
		return false;
	}

	// sub_1802096E0 only reaches the sub-block for types 1, 2 and 3; everything else is accepted at this point.
	if (type < 1 || type > 3) return true;

	if (!readable(kSubBlock, 4)) return true;

	uint32_t mode = 0;
	std::memcpy(&mode, a + kSubBlock, sizeof(mode));
	uint32_t modeOther = 0;
	std::memcpy(&modeOther, b + kSubBlock, sizeof(modeOther));
	if (mode != modeOther) { detail = "game mode"; return false; }

	size_t payload = 0x730;                          // mode 0, or anything above 4
	if (mode == 1)      payload = 0xF888;
	else if (mode == 2) payload = 0xF858;
	else if (mode == 3) payload = 0x830;
	else if (mode == 4) payload = 0x9C8;

	const size_t payloadStart = kSubBlock + kSubBlockPayload;
	if (!readable(payloadStart, payload)) return true;

	// ⚠ NAME THE BYTE THAT DIFFERED. This used to be one blanket memcmp over the whole payload that set
	// detail = "difficulty/skulls" for any difference anywhere in up to 0xF888 bytes - and that label was
	// simply WRONG. The engine's actual difficulty test is the separate byte at kTypeOneField
	// (options + 0x1D4 = blob 0x304) handled above; it measured 0x00 in all eleven checkpoints examined,
	// i.e. difficulty has never once been the reason an injection was questioned.
	//
	// The cost of the blanket label was not cosmetic. It sent an entire investigation chasing a difficulty
	// setting that had never changed, and every explanation offered for a refusal was unfalsifiable, because
	// the one fact that would have settled it was discarded right here. A first-difference scan costs nothing
	// and makes the next report self-diagnosing.
	//
	// The offset reported is ABSOLUTE INTO THE BLOB: a and b point at the options struct, which begins at
	// blob + 0x130, so blob offset = 0x130 + payloadStart + i. That is the number to compare against a hex
	// dump of the file, which is what anyone debugging this actually has in front of them.
	//
	// ⚠ The first 8 bytes of the sub-block object (blob 0x1714..0x171B) are a VTABLE POINTER and differ on
	// every game launch through ASLR. They are deliberately outside this range - the engine's own compare
	// starts at +0xC for exactly that reason - so do not "widen" payloadStart to include them.
	for (size_t i = 0; i < payload; ++i)
	{
		if (a[payloadStart + i] == b[payloadStart + i]) continue;

		size_t differing = 0;
		for (size_t k = i; k < payload; ++k)
			if (a[payloadStart + k] != b[payloadStart + k]) ++differing;

		const uint64_t blobOffset = (uint64_t)(0x130 + payloadStart + i);
		detail = std::format("game options at blob offset 0x{:X} (file 0x{:02X}, current game 0x{:02X}){}",
			blobOffset,
			(uint32_t)(uint8_t)a[payloadStart + i],
			(uint32_t)(uint8_t)b[payloadStart + i],
			differing > 1 ? std::format(", and {} bytes differ in total", differing) : "");
		return false;
	}

	return true;
}


HCECheckpointBlob::HeaderComparison HCECheckpointBlob::compareHeaders(const std::vector<byte>& candidate, const std::vector<byte>& live) const
{
	HeaderComparison out;

	const size_t common = (std::min)(candidate.size(), live.size());
	auto readable = [common](int64_t offset, size_t size)
		{ return offset >= 0 && (uint64_t)offset + size <= common; };

	auto compareDword = [&](int64_t offset, bool& flag)
		{
			if (!readable(offset, sizeof(int32_t))) return;
			flag = std::memcmp(candidate.data() + offset, live.data() + offset, sizeof(int32_t)) == 0;
		};

	compareDword(HeaderOffsets::stateSignature, out.signatureMatches);
	compareDword(HeaderOffsets::stateIdentity, out.stateIdentityMatches);

	// Verdict on the RAW bytes (what the engine strcmp's), display from the sanitised copy.
	const std::string fileScenarioRaw = readBoundedString(candidate, HeaderOffsets::scenarioPath, HeaderOffsets::scenarioPathMax);
	const std::string liveScenarioRaw = readBoundedString(live, HeaderOffsets::scenarioPath, HeaderOffsets::scenarioPathMax);
	out.scenarioMatches = (fileScenarioRaw == liveScenarioRaw);
	out.fileScenario = printableCopy(fileScenarioRaw);
	out.liveScenario = printableCopy(liveScenarioRaw);

	const std::string fileBuildRaw = readBoundedString(candidate, HeaderOffsets::engineBuild, HeaderOffsets::engineBuildMax);
	const std::string liveBuildRaw = readBoundedString(live, HeaderOffsets::engineBuild, HeaderOffsets::engineBuildMax);
	out.engineBuildMatches = (fileBuildRaw == liveBuildRaw);
	out.fileEngineBuild = printableCopy(fileBuildRaw);
	out.liveEngineBuild = printableCopy(liveBuildRaw);

	if (readable(HeaderOffsets::bspIndex, sizeof(int32_t)))
	{
		std::memcpy(&out.fileBSP, candidate.data() + HeaderOffsets::bspIndex, sizeof(out.fileBSP));
		std::memcpy(&out.liveBSP, live.data() + HeaderOffsets::bspIndex, sizeof(out.liveBSP));
	}

	if (readable(HeaderOffsets::sessionBlock, (size_t)HeaderOffsets::sessionBlockLength))
		out.sessionBlockMatches = std::memcmp(
			candidate.data() + HeaderOffsets::sessionBlock,
			live.data() + HeaderOffsets::sessionBlock,
			(size_t)HeaderOffsets::sessionBlockLength) == 0;

	if (readable(HeaderOffsets::gameOptions, 1))
		out.optionsMatch = compareGameOptions(
			candidate.data() + HeaderOffsets::gameOptions,
			live.data() + HeaderOffsets::gameOptions,
			common - (size_t)HeaderOffsets::gameOptions,
			out.optionsDetail);

	return out;
}
