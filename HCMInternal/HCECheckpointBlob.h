#pragma once
#include "pch.h"
#include "GameState.h"
#include "DIContainer.h"
#include "MultilevelPointer.h"
#include <chrono>
#include <string>
#include <vector>

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) checkpoint blob - the half that Dump Checkpoint and Inject Checkpoint SHARE.
//
// This is NOT an IOptionalCheat. It is a plain object that each feature constructs for itself (the same way
// InjectCheckpoint holds a CheckpointInjectionLogger by value); constructing one is nothing but PointerDataStore
// lookups, so there is no reason to route it through the DI cheat cache and no OptionalCheatEnum entry for it.
//
// ⚠⚠ READ THIS FIRST: THERE ARE TWO STORAGE PATHS AND THE SHIPPED GAME TAKES THE SECOND ONE.
//
// save_checkpoint (sub_18019D400) branches on the engine shell (qword_182C40028) and [shell + 0x10]:
//
//   LOCAL    - either null. The two in-process ring buffers, control block and asynchronous SHA-1 worker
//              described below. Everything in this class from readSnapshot() to writeCheckpoint() serves it.
//   PROVIDER - both non null. The blob is handed straight to an external storage provider at 0x19D51A and there
//              is NO in-process checkpoint buffer to read or write at all: qword_181151240 / qword_181151250 are
//              left NULL and the per-slot valid bytes are hardcoded to 1. THIS IS WHAT THE SHIPPED GAME DOES.
//
// The provider path is therefore not served by anything in this class - it is served by intercepting the hand-off
// itself, which lives in HCECheckpointDetours (the passive shadow for dumping, injectCheckpointAtHandoff for the
// write side). What this
// class contributes there is the three things that are common to both paths: detectStorageMode(), the state
// length, and the LIVE HEADER that a candidate file is validated against.
//
// Both Dump and Inject branch on detectStorageMode() and keep working on either path.
//
// WHAT A HALOCER CHECKPOINT IS
// sub_180009E60 allocates ONE 0xC10000-byte game state and TWO checkpoint buffers of the same size. A checkpoint
// is a bit-exact copy of the whole game state with a 0x1ED38-byte header prefixed onto it. The buffers live in a
// 2-entry ring:
//
//     control block  (rva 0x1294590, 32 bytes)
//         +0x00 BYTE  checkpoint system initialised
//         +0x04 DWORD ring size (2)
//         +0x08 BYTE  a checkpoint exists            <- revert aborts if this is 0
//         +0x09 BYTE  the PREVIOUS checkpoint also exists (the double-revert guard)
//         +0x0C DWORD THE RING INDEX                 <- also shipped as hceDoubleRevertFlag
//         +0x10       game time of the last checkpoint
//
//     slot table     (rva 0x1151240, stride 0x10, 2 entries)
//         +0x00 QWORD buffer pointer
//         +0x08 BYTE  valid - 0 when the copy STARTS, 1 when the SHA-1 lands
//         +0x0C DWORD slot ordinal
//
//     length         (rva 0x142AA1C, DWORD)   the size of the state AND of each checkpoint buffer
//     save in flight (rva 0x142AA30, DWORD)   2 while a save is running, 0 when it has been published
//
// ⚠⚠ THE DIGEST IS COMPUTED ON A WORKER TASK, NOT THE GAME THREAD. save_checkpoint memcpys the state on the game
// thread, sets the in-flight dword to 2 and queues sub_180327440 -> sub_18019E5E0; only when THAT finishes does it
// set slot+8 = 1 and the in-flight dword back to 0. So `slot.valid == 1 && inFlight == 0` is the engine's own
// publication barrier, and it is the only thing that distinguishes "a finished checkpoint" from "12 MB that is
// being overwritten right now, whose digest field is still stale". readCurrentCheckpoint gates on that pair
// BEFORE the copy and re-checks it AFTER, retrying if anything moved - a torn dump would be a file that always
// fails its SHA, and on HaloCER a rejected revert does not fail softly, IT RESTARTS THE LEVEL (every failure path
// in sub_18019D730 falls to LABEL_95, which the checkpoint host at 0x1ADB83 turns into sub_18020C910).
//
// ⚠ NOTHING MAY BE STAMPED INTO THE BLOB. The SHA-1 at blob+0x1ED20 covers the WHOLE buffer, so MCC's habit of
// writing a 10-byte version string over the last 10 bytes of a dump would invalidate every file. HaloCER also has
// no version resource to stamp (every build reports 0.0.0.0), so the practice would be meaningless here anyway.
//
// ⚠ THE LENGTH IS READ, NOT ASSUMED. 0xC10000 is what this build allocates, but the engine keeps the number in a
// global and the header/hash both cover exactly that many bytes, so read it.
//
// ⚠ THE LIVE HEADER IS ALWAYS AVAILABLE, ON BOTH PATHS. qword_18142AA10 is the game state block, and the session
// setup at 0x19D21B writes every field the revert compares into its first 0x1ED38 bytes at level load: the state
// signature CRC at +0x000, the scenario path at +0x008, the engine build string at +0x108, the state identity at
// +0x128, the whole 0x1EBB0-byte game options struct at +0x130 and the session block at +0x1ECE0. So
// readLiveHeader() is a cheap read of ~126 KB that needs no checkpoint to exist and no checkpoint to be forced,
// and it is literally the `live` side the engine passes to sub_18019EB50. That makes it the right thing to
// validate an injection candidate against on both paths.
//
// Every method throws HCMRuntimeException with a user-facing message; that is the normal case at a menu, mid-load,
// and before the first checkpoint of a level.
// ================================================================================================================
class HCECheckpointBlob
{
public:
	// Pure PointerDataStore (XML) lookups - touches NO game memory and resolves NO address, so this is safe to
	// build inside a cheat constructor, which runs on a detached thread long before
	// HaloSimulation_tag_release.dll is guaranteed to be loaded (see the SAFETY note in HCECheckpointDetours.cpp).
	// Throws HCMInitException, which is the only thing OptionalCheatConstructor::createCheats catches.
	HCECheckpointBlob(GameState game, IDIContainer& dicon);

	// One coherent look at the checkpoint ring.
	struct Snapshot
	{
		bool      systemInitialised = false; // control + 0x00
		bool      anyCheckpointExists = false; // control + 0x08
		int32_t   ringSize = 0;              // control + 0x04
		int32_t   ringIndex = -1;            // control + 0x0C
		uintptr_t slotAddress = 0;           // slotTable + slotStride*ringIndex
		uintptr_t buffer = 0;                // *(void**)slotAddress
		bool      slotValid = false;         // *(uint8*)(slotAddress + 8)
		bool      saveIdle = false;          // *(int32*)(simBase + 0x142AA30) == 0
		int32_t   length = 0;                // *(int32*)(simBase + 0x142AA1C)

		// The engine's own publication barrier, plus the sanity checks that make the buffer safe to touch.
		bool published() const
		{
			return systemInitialised && anyCheckpointExists && slotValid && saveIdle && buffer != 0 && length > 0;
		}
	};

	uintptr_t getSimModuleBase();   // HaloSimulation_tag_release.dll base. Throws if it is not loaded.


	// ============================================================================================================
	// COMMON TO BOTH STORAGE PATHS
	// ============================================================================================================
	enum class StorageMode
	{
		Local,      // in-process ring buffers - everything from readSnapshot() down applies
		Provider,   // external provider - only the hand-off in HCECheckpointDetours can reach a checkpoint
	};

	// The engine's OWN discriminator, read from the same two words it branches on at 0x19D4DD / 0x19D4E6:
	// shell = *(void**)(simBase + 0x2C40028), then shell->0x10. Either null means Local.
	// Deliberately not inferred from a null slot pointer any more - that could not tell "provider" apart from
	// "the pointer data moved". Throws only if the sim or its pointer data is unavailable.
	StorageMode detectStorageMode();

	// *(int32*)(simBase + 0x142AA1C). The size of the game state, of every checkpoint, and of the blob handed to
	// the provider. Validated against kMaxPlausibleLength. Throws with a user-facing message.
	int32_t readStateLength();

	// *(void**)(simBase + 0x142AA10) - the live game state block.
	// ⚠ HCM NEVER WRITES THROUGH THIS. It is the buffer the running simulation lives in and the one a revert
	// restores into; injection redirects the provider's argument instead. See HCECheckpointDetours.cpp.
	uintptr_t readStateBlockAddress();

	// headerLength() bytes copied out of the state block: the live session header, which is what the engine
	// compares an incoming checkpoint against. Available on both paths, any time a level is loaded, without
	// needing a checkpoint to exist. Feed it to compareHeaders() as the `live` argument.
	std::vector<byte> readLiveHeader();


	// ============================================================================================================
	// LOCAL PATH ONLY. Every one of these dereferences the in-process ring and will throw on a provider process,
	// so call detectStorageMode() first.
	// ============================================================================================================

	// false = the PROVIDER path, i.e. there is no in-process checkpoint buffer at all. Never throws.
	bool isLocalStorage();
	// Same test, but throws the user-facing explanation.
	void requireLocalStorage();

	Snapshot readSnapshot();              // throws only if the sim/pointer data is unavailable
	Snapshot requirePublishedSnapshot();  // additionally throws unless snapshot.published()

	// THE shared operation. Waits (briefly) for the engine to publish, copies the whole blob, then re-reads the
	// barrier and retries if the engine published a new checkpoint underneath us. outSnapshot describes the
	// checkpoint that was actually captured. Throws rather than ever returning a torn buffer.
	std::vector<byte> readCurrentCheckpoint(Snapshot& outSnapshot);

	// The write half, for Inject. Deliberately NOT a retry loop: it re-reads the barrier and refuses (WITHOUT
	// writing a single byte) unless the ring is exactly where `expected` left it, so a checkpoint that arrived
	// while the caller was validating can never be half-overwritten. If the engine starts a save while the write
	// is actually in flight, `previousContents` - the bytes the caller read out of this very buffer - is written
	// back as a rollback, because leaving a torn blob in the slot would mean the user's next revert RESTARTS THE
	// LEVEL. previousContents may be empty, in which case no rollback is attempted.
	void writeCheckpoint(const Snapshot& expected, const std::vector<byte>& data, const std::vector<byte>& previousContents);

	// Blob layout. Dump deliberately uses none of these - it writes the bytes verbatim - but Inject has to zero
	// the digest field, hash the whole buffer and write the result back, so the numbers live here with the rest of
	// the format rather than being rediscovered on that side.
	int64_t shaOffset() const { return mSHAOffset; }       // blob + 0x1ED20, where the 20-byte SHA-1 goes
	int64_t shaLength() const { return mSHALength; }       // 20
	int64_t headerLength() const { return mHeaderLength; } // 0x1ED38 - the game state proper starts here

	// A blob bigger than this is not a checkpoint, it is a bad read. 0xC10000 is what this build uses.
	static constexpr int32_t kMaxPlausibleLength = 0x4000000; // 64 MiB


	// ============================================================================================================
	// THE DIGEST
	// The engine's recipe, and it must be reproduced exactly on both sides: zero the mSHALength bytes at
	// mSHAOffset, hash the WHOLE blob (every byte, digest field included, i.e. the zeroes), and write the 20-byte
	// result back over them. Writer sub_18019E5E0, verifier sub_18019EC00, both via BCrypt SHA-1 (sub_1801A6D70).
	// It is deterministic, so re-running it over an untouched dump reproduces that dump's digest byte for byte -
	// which is why Inject always recomputes rather than trying to decide whether it needs to.
	//
	// ⚠ The 20-byte salt at blob+0x1ED0C is INSIDE the hashed range and is deliberately left alone: it is what
	// makes two saves of an identical state hash differently, and the verifier hashes whatever is there.
	//
	// ⚠ MCC's InjectCheckpoint gets this wrong in a way that silently produces an all-zero digest - it
	// `reserve`s the output vector instead of `resize`ing it, so the memcpy_s that pastes the digest back is
	// handed size() == 0 and copies nothing. Do not "simplify" this back towards that shape.
	void applySHA1(std::vector<byte>& blob) const; // throws HCMRuntimeException


	// ============================================================================================================
	// THE HEADER, AND THE ENGINE'S OWN ACCEPTANCE TEST
	//
	// A checkpoint's first 0x1ED38 bytes are a header describing the session that produced it. The revert
	// (sub_18019D730) hands it to sub_18019EB50, which is a straight field-by-field comparison against the live
	// session, and EVERY failure path in the revert falls to LABEL_95 -> sub_18020C910 = A LEVEL RESTART. So these
	// offsets are not documentation, they are the difference between an injected checkpoint and a lost run.
	//
	// The revert memcpys the checkpoint's 0x1ED38-byte header into a stack local (0x19D8D1: mov r8d, 1ED38h) and
	// runs the CHECKPOINT's copy through every test below, so all six of these read the FILE side:
	//
	//     0x19D8DC  strcmp(blob+0x108, *(char**)0x182CBE0)   - engine build string vs the live one. The alternate
	//               table at 0x7B04A8 that follows has its single entry NULL on this build, so this is a plain
	//               equality; the override flag at 0x97C5E0 is the only way past it.
	//     0x19D939  *(DWORD*)(blob+0x000) == *(DWORD*)0x12945C0  - the build's state-layout CRC32
	//     0x19D94B  the scenario path at blob+0x008 must be non-empty
	//     0x19D967  sub_180208CA0(blob+0x130)                - the options must be self-consistent
	//     0x19D981  sub_18019E960(blob, 0, 0)                - the options must match a live session entry
	//     0x19D99C  sub_18019EB50(live, blob), which is:
	//                   sub_18019E880(blob)                     - build string and CRC again
	//                   strcmp(live+0x008, blob+0x008) == 0     - scenario path
	//                   sub_18019F450() == *(DWORD*)(blob+0x128)- state identity
	//                   sub_1802096E0(live+0x130, blob+0x130)   - game options - see compareGameOptions
	//                   sub_18019CD70(live+0x1ECE0, blob+0x1ECE0) - seven dwords, an exact 28-byte compare
	//     0x19DBBA  sub_18019EC00(blob, 0)                   - the SHA-1. a2 = 0, so no bypass. see applySHA1.
	//
	// HCM compares a CANDIDATE FILE against THE LIVE CHECKPOINT rather than against the running session, and that
	// is what makes this safe to do from outside the engine WITHOUT resolving a single extra global or calling a
	// single engine function. The argument is just transitivity: every test above is an equality (strcmp, ==,
	// memcmp) between a field of the checkpoint and something derived from the live session. The live checkpoint
	// passes all of them - the game can revert to it right now - so if the file agrees with the live blob field
	// for field, it passes them too. This holds no matter what the live side of each comparison actually is.
	//
	// The corollary is the rule for every check in here: it may be STRICTER than the engine (that costs the user a
	// "Continue" click) but it must never be LOOSER (that costs them the level). See compareGameOptions, where
	// that rule decides how the map-name field is compared.
	// ============================================================================================================
	struct HeaderOffsets
	{
		static constexpr int64_t stateSignature = 0x000;    // DWORD - the build's state-layout CRC32
		static constexpr int64_t scenarioPath = 0x008;      // NUL-terminated ASCII
		static constexpr int64_t engineBuild = 0x108;       // NUL-terminated ASCII
		static constexpr int64_t stateIdentity = 0x128;     // DWORD - sub_18019F450()
		static constexpr int64_t gameOptions = 0x130;       // see compareGameOptions
		static constexpr int64_t bspIndex = 0x1ECA4;        // DWORD - mirrors dword_1809A14E0. Informational only.
		static constexpr int64_t sessionBlock = 0x1ECE0;    // 7 DWORDs compared exactly by sub_18019CD70
		static constexpr int64_t sessionBlockLength = 0x1C;

		static constexpr size_t scenarioPathMax = 0x100;    // +0x008 .. +0x108
		static constexpr size_t engineBuildMax = 0x20;      // +0x108 .. +0x128
	};

	// One field-by-field verdict, split into the three buckets the GUI exposes as toggles.
	struct HeaderComparison
	{
		bool scenarioMatches = true;       // +0x008
		bool sessionBlockMatches = true;   // +0x1ECE0
		bool optionsMatch = true;          // +0x130
		bool signatureMatches = true;      // +0x000
		bool engineBuildMatches = true;    // +0x108
		bool stateIdentityMatches = true;  // +0x128

		std::string fileScenario, liveScenario;
		std::string fileEngineBuild, liveEngineBuild;
		int32_t fileBSP = -1, liveBSP = -1;   // +0x1ECA4 - shown to the user, never on its own a reason to refuse
		std::string optionsDetail;            // which field inside the options struct disagreed

		// "am I on the right level, in the right place" - the scenario path plus the session block, which sits
		// beside the BSP index and which sub_18019EB50 compares byte for byte.
		bool levelOK() const { return scenarioMatches && sessionBlockMatches; }
		// "was this made by this build of the game, in a session shaped like this one"
		bool buildOK() const { return signatureMatches && engineBuildMatches && stateIdentityMatches; }
		bool optionsOK() const { return optionsMatch; }
		bool allOK() const { return levelOK() && buildOK() && optionsOK(); }
	};

	// Never throws and never touches game memory - both arguments are already-copied blobs. A field whose offset
	// falls outside either blob is reported as MATCHING, so a short/odd buffer can never manufacture a scary
	// warning; length is validated separately and much earlier, and it is a hard refusal.
	HeaderComparison compareHeaders(const std::vector<byte>& candidate, const std::vector<byte>& live) const;

private:
	// Control block / slot table layout. Deliberately C++ constants rather than XML entries: they are two small
	// structs that came out of one function (sub_180009E60) and are meaningless individually, and HCM already
	// keeps this class of offset in code (see HCEDisableBarriers' soft-ceiling offsets). Only the RVAs, which are
	// what a game update actually moves, are in InternalPointerData.xml.
	static constexpr int64_t kControlInitialised = 0x00;
	static constexpr int64_t kControlRingSize = 0x04;
	static constexpr int64_t kControlExists = 0x08;
	static constexpr int64_t kControlRingIndex = 0x0C;
	static constexpr int64_t kSlotStride = 0x10;
	static constexpr int64_t kSlotValid = 0x08;

	// How long readCurrentCheckpoint will wait for the SHA-1 worker to publish before giving up. The hash is one
	// pass over ~12 MB on a task thread, so this is generous; it exists so a wedged save cannot hang the hotkey
	// thread forever.
	static constexpr int kBarrierWaitMilliseconds = 1000;

	GameState mGame;

	std::shared_ptr<MultilevelPointer> mControlBlock;
	std::shared_ptr<MultilevelPointer> mSlotTable;
	std::shared_ptr<MultilevelPointer> mStateLength;
	std::shared_ptr<MultilevelPointer> mSaveInFlight;
	std::shared_ptr<MultilevelPointer> mShell;       // ADDRESS of the engine shell pointer (qword_182C40028)
	std::shared_ptr<MultilevelPointer> mStateBlock;  // ADDRESS of the game state pointer (qword_18142AA10)

	// [shell + 0x10]. Non-null on both is what selects the provider path at 0x19D4E6.
	static constexpr int64_t kShellProvider = 0x10;

	int64_t mSHAOffset = 0x1ED20;
	int64_t mSHALength = 0x14;
	int64_t mHeaderLength = 0x1ED38;

	static uintptr_t resolveOrThrow(const std::shared_ptr<MultilevelPointer>& mlp, const char* what);

	// A direct port of sub_1802096E0, the engine's game-options comparison (difficulty, skulls, game type, ...).
	// Ported rather than called: calling into the sim from the hotkey thread would be a far bigger risk than
	// reimplementing five field compares, and this way it runs entirely on two byte buffers HCM already owns.
	// `detail` gets a human-readable name for the first field that disagreed. Returns true = compatible.
	static bool compareGameOptions(const byte* a, const byte* b, size_t available, std::string& detail);

	// Bounded read of a NUL-terminated field. Stops at the field's own size, so an unterminated string in a
	// hand-edited file cannot walk off the end of the buffer. Returns the RAW bytes, because that is what the
	// verdict has to be taken on - the engine strcmp's them, and sanitising first would make two different pieces
	// of garbage compare equal. printableCopy is for putting the value in front of the user, and nothing else.
	static std::string readBoundedString(const std::vector<byte>& blob, int64_t offset, size_t maxLength);
	static std::string printableCopy(const std::string& raw);

	// Throws for everything that waiting cannot fix (no level, no checkpoint yet, nonsense length/slot), so the
	// wait loop only ever spins on the one condition that resolves itself - the SHA-1 worker finishing.
	static void throwIfUnusable(const Snapshot& snapshot);
	Snapshot waitForPublishedSnapshot(std::chrono::steady_clock::time_point deadline);
};
