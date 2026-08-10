#include "pch.h"
#include "HCESoftCeilingOverlay.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"
#include "HCESignatureScan.h"
#include "PointerDataStore.h"
#include "Render3DEventProvider.h"
#include "IRenderer3D.h"
#include "IModel.h"
#include "SoftCeilingData.h"
#include "GlobalKill.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

// The full derivation of every offset, enum option and flag bit used here is in HCESoftCeilingOverlay.h and
// InternalPointerData.xml. This file only implements it.

namespace
{
	// All guarded (SEH inside tryReadRaw), so a moved offset degrades to a zero rather than a crash.
	uint16_t  readU16(uintptr_t address) { uint16_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	uint32_t  readU32(uintptr_t address) { uint32_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	int32_t   readI32(uintptr_t address) { int32_t v = 0;  HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	uintptr_t readPtr(uintptr_t address) { uintptr_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	bool plausiblePointer(uintptr_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull; }

	// ---------------------------------------------------------------------------------------------------------
	// The engine's string_id -> text function. Its whole body is a hash-table lookup through one module global
	// plus a memcpy out of the matched record - no thread-local storage anywhere in it, which is what makes it
	// safe to call from a thread other than the game thread. Still wrapped in SEH and pre-gated on its table
	// pointer being non-null, because during a level load that table is being rebuilt underneath us.
	//
	// POD-only: MSVC rejects __try in a function that requires C++ object unwinding (C2712).
	// ---------------------------------------------------------------------------------------------------------
	typedef const char* (__fastcall* FnStringIdText)(uint32_t);

	bool callStringIdText(FnStringIdText function, uint32_t stringId, char* out, size_t capacity)
	{
		out[0] = '\0';
		__try
		{
			const char* text = function(stringId);
			if (!text) return false;
			size_t i = 0;
			for (; i + 1 < capacity; ++i)
			{
				const char c = text[i];
				if (c == '\0') break;
				if (c < 0x20 || c > 0x7E) break;   // stop at the first non-printable rather than showing garbage
				out[i] = c;
			}
			out[i] = '\0';
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out[0] = '\0';
			return false;
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// One drawable soft ceiling, already expanded into world-space geometry so the render path does no tag
	// reading and no allocation at all.
	//
	// ⚠ INDICES ARE 16 BIT (IndexCollection is std::vector<uint16_t>), so a batch can hold at most 65536
	// vertices = 21845 triangles. A single soft ceiling that exceeds that is SPLIT across several batches
	// rather than truncated - they share an identity and simply draw as more than one collection.
	// ---------------------------------------------------------------------------------------------------------
	constexpr size_t kMaxTrianglesPerBatch = 20000;   // 60000 vertices, comfortably inside the 16-bit index range

	struct SoftCeilingBatch : public IModelTriangles, public IModelEdges
	{
		std::string name;
		uint32_t nameStringId = 0;
		SoftCeilingType type = SoftCeilingType::Acceleration;

		// From the SCENARIO record, which is joined by name. Defaults are the permissive ones: an unmatched
		// soft ceiling applies to everything and is treated as enabled, so a failed join hides nothing.
		// ⚠ bitmask<T> declares a converting constructor and therefore has NO default constructor - the
		// initialiser has to name a value, `{}` does not compile.
		SoftCeilingObjectMask ignoreMask{ SoftCeilingIgnoresObjectType::None };
		int scenarioIndex = -1;          // == the bit index in the mask at *(tls + 0x430); -1 when unmatched

		VertexCollection renderVertices;
		IndexCollection triangleIndices;
		IndexCollection edgeIndices;

		SimpleMath::Vector3 center{};
		float radius = 0.f;

		const VertexCollection& getTriangleVertices() const override { return renderVertices; }
		const IndexCollection& getTriangleIndices() const override { return triangleIndices; }
		const VertexCollection& getEdgeVertices() const override { return renderVertices; }
		const IndexCollection& getEdgeIndices() const override { return edgeIndices; }

		// Same tests as MCC's SoftCeilingData, against the same flag bits (bit0 ignore bipeds, bit1 ignore
		// vehicles) - the flags definition in this build is byte-identical to MCC's.
		bool appliesToBiped() const { return bitmaskContains(ignoreMask, SoftCeilingIgnoresObjectType::Biped) == false; }
		bool appliesToVehicle() const { return bitmaskContains(ignoreMask, SoftCeilingIgnoresObjectType::Vehicle) == false; }
	};

	// soft_ceiling_type_enum has exactly three options (0 acceleration, 1 soft kill, 2 slip surface), read out
	// of the enum definition in the binary. Anything else means we are not reading a type field at all, so it
	// is counted and shown as Acceleration rather than silently colouring as "slip surface" - which is what a
	// naive `>= 2` fallthrough would do and would make a wrong read look like real data.
	bool decodeSoftCeilingType(uint16_t raw, SoftCeilingType& out)
	{
		if (raw > (uint16_t)SoftCeilingType::SlipSurface) { out = SoftCeilingType::Acceleration; return false; }
		out = (SoftCeilingType)raw;
		return true;
	}

	void finaliseBatchBounds(SoftCeilingBatch& batch)
	{
		if (batch.renderVertices.empty()) return;
		SimpleMath::Vector3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (const auto& v : batch.renderVertices)
		{
			lo.x = std::min(lo.x, v.position.x); hi.x = std::max(hi.x, v.position.x);
			lo.y = std::min(lo.y, v.position.y); hi.y = std::max(hi.y, v.position.y);
			lo.z = std::min(lo.z, v.position.z); hi.z = std::max(hi.z, v.position.z);
		}
		batch.center = (lo + hi) * 0.5f;
		batch.radius = (hi - batch.center).Length();
	}
}


class HCESoftCeilingOverlay::HCESoftCeilingOverlayImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;
	std::optional<std::weak_ptr<Render3DEventProvider>> mRender3DProviderOptionalWeak;
	// The UE render camera. Without it Render3DEventProvider falls back to the sim's player-aim camera and a
	// default field of view, which draws everything at visibly the wrong scale rather than not at all.
	std::optional<std::weak_ptr<HCEGetCameraData>> mCameraDataOptionalWeak;
	bool mCameraHookRequested = false;

	// ---- structural tag constants, from InternalPointerData.xml. NOT addresses; see the header. ----
	int64_t mScenarioSoftCeilingBlock = 0;
	int64_t mScenarioSoftCeilingStride = 0;
	int64_t mScenarioSoftCeilingFlagsOffset = 0;
	int64_t mScenarioSoftCeilingNameOffset = 0;
	int64_t mScenarioSoftCeilingTypeOffset = 0;
	int64_t mScenarioStructureDesignBlock = 0;
	int64_t mStructureDesignReferenceStride = 0;
	int64_t mStructureDesignTagIndexOffset = 0;
	int64_t mStructureDesignSoftCeilingBlock = 0;
	int64_t mStructureSoftCeilingStride = 0;
	int64_t mStructureSoftCeilingNameOffset = 0;
	int64_t mStructureSoftCeilingTypeOffset = 0;
	int64_t mStructureSoftCeilingTrianglesBlock = 0;
	int64_t mSoftCeilingTriangleStride = 0;
	int64_t mSoftCeilingTriangleSphereOffset = 0;
	int64_t mSoftCeilingTriangleVertex0Offset = 0;
	int64_t mTagInstanceTableOffset = 0;
	int64_t mTagInstanceStride = 0;
	int64_t mTagInstanceDataOffset = 0;

	// MAXIMUM_SCENARIO_SOFT_CEILINGS_PER_SCENARIO, read out of the block definition itself.
	static constexpr int32_t kMaxScenarioSoftCeilings = 0x80;
	// Where soft_ceiling_enable's runtime mask hangs off the game thread's TLS block. Decoded and owned by
	// HCEDisableBarriers; duplicated here as a read-only consumer.
	static constexpr int64_t kSoftCeilingMaskTlsOffset = 0x430;

	// ---- byte-signature anchored, never XML'd (this title has no version resource) ----
	uintptr_t mScenarioSlot = 0;
	uintptr_t mTagAddressTable = 0;
	uintptr_t mTagGlobalsSlot = 0;
	FnStringIdText mStringIdText = nullptr;
	uintptr_t mStringIdTableSlot = 0;     // the global mStringIdText loads; null while a level is loading
	bool mAnchorsTried = false;
	bool mAnchorsGood = false;
	std::string mAnchorFailure;

	std::atomic_bool mReady{ false };
	bool mIsActive = false;
	std::atomic_bool mCurrentlyRendering3D{ false };
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRender3DEventCallback;

	// GUARDED BY mBatchesMutex. Three threads reach it: the render thread every frame, the hotkey/GUI thread on
	// toggle, and the state-hook thread on a level change.
	std::mutex mBatchesMutex;
	std::vector<SoftCeilingBatch> mBatches;
	// Rebuild key - see computeCacheKey(). This is static tag data, so it is re-read only when that key
	// changes, and the key itself is only recomputed every kCacheCheckIntervalMs rather than per frame.
	uint64_t mCacheKey = 0;
	bool mCacheValid = false;
	std::chrono::steady_clock::time_point mLastCacheCheck{};
	static constexpr int64_t kCacheCheckIntervalMs = 500;

	// Per-rebuild diagnostics, logged once so a level that draws nothing is diagnosable rather than mysterious.
	size_t mLastScenarioRecords = 0;
	size_t mLastGeometryRecords = 0;
	size_t mLastTriangles = 0;
	size_t mLastRejectedTriangles = 0;   // failed the bounding-sphere cross-check
	size_t mLastUnmatched = 0;           // geometry record whose name is not in the scenario block
	size_t mLastBadTypes = 0;            // type field outside the 3-option enum - a read that should not happen

	void attachCameraHook()
	{
		if (!mCameraDataOptionalWeak.has_value())
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the soft "
				"ceiling overlay cannot read the render camera");
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData)
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the soft "
				"ceiling overlay cannot read the render camera");

		if (mCameraHookRequested) return;
		cameraData->setHookWanted(this, true);   // may throw; only recorded once it did not
		mCameraHookRequested = true;
	}

	void detachCameraHook()
	{
		if (!mCameraHookRequested) return;
		mCameraHookRequested = false;
		if (!mCameraDataOptionalWeak.has_value()) return;
		if (auto cameraData = mCameraDataOptionalWeak.value().lock())
		{
			try { cameraData->setHookWanted(this, false); }
			catch (HCMRuntimeException) {}   // releasing a request cannot meaningfully fail
		}
	}

	// enc -> real address. tagAddressTable[enc >> 28] + 4 * enc: the high nibble selects one of 16 region bases,
	// so this is NOT a single flat offset.
	uintptr_t resolveTagBlock(uint32_t encoded) const
	{
		if (encoded == 0 || encoded == 0xFFFFFFFFu) return 0;
		const uintptr_t regionBase = readPtr(mTagAddressTable + 8ull * (encoded >> 28));
		if (!plausiblePointer(regionBase)) return 0;
		return regionBase + 4ull * encoded;
	}

	// tag index -> tag data. Deliberately inlines what the engine's tag_get_data does rather than calling it,
	// so nothing here takes an engine lock from HCM's threads. Same walk HCEBspOverlay uses.
	uintptr_t resolveTagData(uint32_t tagIndex) const
	{
		if (tagIndex == 0xFFFFFFFFu || (tagIndex & 0xFFFFu) == 0xFFFFu) return 0;
		const uintptr_t tagGlobals = readPtr(mTagGlobalsSlot);
		if (!plausiblePointer(tagGlobals)) return 0;
		const uintptr_t instances = readPtr(tagGlobals + mTagInstanceTableOffset);
		if (!plausiblePointer(instances)) return 0;
		const uintptr_t instance = instances + (uintptr_t)mTagInstanceStride * (tagIndex & 0xFFFFu);
		return resolveTagBlock(readU32(instance + mTagInstanceDataOffset));
	}

	void resolveAnchors(uintptr_t simBase) // never throws
	{
		mAnchorsTried = true;
		mAnchorsGood = false;
		mAnchorFailure.clear();

		int hits = 0;

		// Shared with HCETriggerOverlay and HCEBspOverlay - the "78 02" inside the pattern is the engine's own
		// trigger-volume count compare, so the anchor is literally its own scenario access.
		const uintptr_t scenarioInsn = HCESignatureScan::resolveUnique(
			simBase, "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00", hits);
		if (!scenarioInsn) { mAnchorFailure = std::format("scenario data pointer ({} matches)", hits); return; }
		mScenarioSlot = HCESignatureScan::ripTarget(scenarioInsn, 3, 7);

		const uintptr_t tableInsn = HCESignatureScan::resolveUnique(simBase,
			"48 8B D9 4C 8D 05 ?? ?? ?? ?? 8B 09 8B 53 08 8B C2 48 C1 E8 1C 49 8B 04 C0 3B 4C 90 10 7D", hits);
		if (!tableInsn) { mAnchorFailure = std::format("tag address table ({} matches)", hits); return; }
		mTagAddressTable = HCESignatureScan::ripTarget(tableInsn, 6, 10);

		// tag_get_data's prologue. The tail `48 C1 E7 04 / 48 03 78 50` is the instance-stride shift and the
		// +0x50 load of the instance array, so this pattern IS the tag instance lookup rather than something
		// that merely resembles it.
		const uintptr_t tagGetData = HCESignatureScan::resolveUnique(simBase,
			"48 89 5C 24 08 57 48 83 EC 20 0F B7 C2 8B DA 48 8D 3C 40 48 8B 05 ?? ?? ?? ?? 48 C1 E7 04 48 03 78 50", hits);
		if (!tagGetData) { mAnchorFailure = std::format("tag instance table ({} matches)", hits); return; }
		mTagGlobalsSlot = HCESignatureScan::ripTarget(tagGetData + 0x13, 3, 7);

		if (!mScenarioSlot || !mTagAddressTable || !mTagGlobalsSlot)
		{
			mAnchorFailure = "rip-relative operand read failed";
			return;
		}

		// string_id -> text. OPTIONAL: without it every barrier still draws, they are just named by index.
		const uintptr_t sidFunction = HCESignatureScan::resolveUnique(simBase,
			"40 53 55 56 57 48 83 EC 28 48 8B 3D ?? ?? ?? ?? 48 63 E9", hits);
		if (sidFunction)
		{
			// The function's 6th instruction is `mov rdi, [rip+stringTable]`; verify those three opcode bytes
			// before trusting the operand, so a pattern that ever matched something else cannot make us call
			// through a bad pointer.
			const uintptr_t loadInsn = sidFunction + 9;
			uint8_t opcode[3]{};
			if (HCEGetPlayerState::tryReadRaw(loadInsn, opcode, sizeof(opcode))
				&& opcode[0] == 0x48 && opcode[1] == 0x8B && opcode[2] == 0x3D)
			{
				mStringIdTableSlot = HCESignatureScan::ripTarget(loadInsn, 3, 7);
				if (mStringIdTableSlot) mStringIdText = (FnStringIdText)sidFunction;
			}
		}

		mAnchorsGood = true;
	}

	// A soft ceiling's live enable bit. Returns TRUE when the mask cannot be read at all, because "we could not
	// tell" must not silently hide barriers - only a bit we actually read and found CLEAR means disabled.
	// Polarity is BIT SET = ENABLED; see HCEDisableBarriers.cpp, which decoded and owns this mask.
	bool softCeilingIsEnabled(uintptr_t maskBase, int scenarioIndex) const
	{
		if (!maskBase || scenarioIndex < 0 || scenarioIndex >= kMaxScenarioSoftCeilings) return true;
		const uint32_t word = readU32(maskBase + 4ull * (uint32_t)(scenarioIndex >> 5));
		return (word & (1u << (scenarioIndex & 31))) != 0;
	}

	// The game thread's mask, or 0 if unavailable. MUST go through HCEGetPlayerState's thread walk - reading
	// NtCurrentTeb() from the render thread would give a different, wrong TLS block.
	uintptr_t readEnableMaskBase() const
	{
		auto playerState = playerStateWeak.lock();
		if (!playerState) return 0;
		const uintptr_t mask = readPtr(playerState->getTlsBase() + kSoftCeilingMaskTlsOffset);
		return plausiblePointer(mask) ? mask : 0;
	}

	// The rebuild key: the scenario pointer plus every structure_design tag's RESOLVED data address.
	//
	// ⚠ THE SCENARIO POINTER ALONE IS NOT ENOUGH, which is the difference between this and HCEBspOverlay's
	// simpler key. Structure designs are per-BSP tags that stream in and out with the zone set, while the
	// scenario pointer sits still for the whole level - so keying on the scenario would leave the PREVIOUS
	// area's barriers on screen, and the new area's missing, until the level itself changed. Hashing the
	// resolved design addresses catches a design loading, unloading, or being relocated.
	//
	// Returns 0 when no level is loaded, which the caller treats as "clear and wait" rather than as a key.
	uint64_t computeCacheKey() const
	{
		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return 0;

		// FNV-1a, so the ORDER of the designs is part of the key and a reordered pair is a new key.
		uint64_t key = 0xcbf29ce484222325ull;
		auto mix = [&key](uint64_t v) { key = (key ^ v) * 0x100000001b3ull; };
		mix((uint64_t)scenario);

		const int32_t designCount = readI32(scenario + mScenarioStructureDesignBlock);
		mix((uint64_t)(uint32_t)designCount);
		if (designCount <= 0 || designCount > 256) return key;

		const uintptr_t designBase = resolveTagBlock(readU32(scenario + mScenarioStructureDesignBlock + 4));
		if (!plausiblePointer(designBase)) return key;

		for (int32_t d = 0; d < designCount; ++d)
			mix((uint64_t)resolveTagData(readU32(designBase
				+ (uintptr_t)mStructureDesignReferenceStride * d + mStructureDesignTagIndexOffset)));
		return key;
	}

	// Rebuilds mBatches from the scenario tag. CALLER MUST HOLD mBatchesMutex.
	void rebuild()
	{
		mBatches.clear();
		mLastScenarioRecords = 0;
		mLastGeometryRecords = 0;
		mLastTriangles = 0;
		mLastRejectedTriangles = 0;
		mLastUnmatched = 0;
		mLastBadTypes = 0;

		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return;

		// ---- pass 1: the scenario's own soft ceiling records - flags, type, and the enable bit index --------
		struct ScenarioRecord
		{
			// bitmask<T> has no default constructor - see the note on SoftCeilingBatch::ignoreMask.
			SoftCeilingObjectMask ignoreMask{ SoftCeilingIgnoresObjectType::None };
			SoftCeilingType type = SoftCeilingType::Acceleration;
			int index = -1;
		};
		std::unordered_map<uint32_t, ScenarioRecord> byName;   // key: name string_id

		{
			const int32_t count = readI32(scenario + mScenarioSoftCeilingBlock);
			const uintptr_t base = resolveTagBlock(readU32(scenario + mScenarioSoftCeilingBlock + 4));
			if (plausiblePointer(base) && count > 0 && count <= kMaxScenarioSoftCeilings)
			{
				mLastScenarioRecords = (size_t)count;
				for (int32_t i = 0; i < count; ++i)
				{
					const uintptr_t element = base + (uintptr_t)mScenarioSoftCeilingStride * i;
					ScenarioRecord record;
					record.ignoreMask = SoftCeilingObjectMask(
						(SoftCeilingIgnoresObjectType)readU16(element + mScenarioSoftCeilingFlagsOffset));
					if (!decodeSoftCeilingType(readU16(element + mScenarioSoftCeilingTypeOffset), record.type))
						++mLastBadTypes;
					record.index = i;
					byName.insert_or_assign(readU32(element + mScenarioSoftCeilingNameOffset), record);
				}
			}
		}

		// ---- pass 2: the geometry, from every structure_design ('sddt') tag the scenario references ---------
		const int32_t designCount = readI32(scenario + mScenarioStructureDesignBlock);
		const uintptr_t designBase = resolveTagBlock(readU32(scenario + mScenarioStructureDesignBlock + 4));
		if (!plausiblePointer(designBase) || designCount <= 0 || designCount > 256) return;

		const bool stringIdUsable = mStringIdText && readPtr(mStringIdTableSlot) != 0;
		char nameBuffer[64]{};

		for (int32_t d = 0; d < designCount; ++d)
		{
			const uint32_t designTagIndex =
				readU32(designBase + (uintptr_t)mStructureDesignReferenceStride * d + mStructureDesignTagIndexOffset);
			const uintptr_t design = resolveTagData(designTagIndex);
			if (!plausiblePointer(design)) continue;

			const int32_t ceilingCount = readI32(design + mStructureDesignSoftCeilingBlock);
			const uintptr_t ceilingBase = resolveTagBlock(readU32(design + mStructureDesignSoftCeilingBlock + 4));
			// Loose gate: it only has to reject "we are not looking at a structure_design".
			if (!plausiblePointer(ceilingBase) || ceilingCount <= 0 || ceilingCount > 4096) continue;

			for (int32_t c = 0; c < ceilingCount; ++c)
			{
				const uintptr_t element = ceilingBase + (uintptr_t)mStructureSoftCeilingStride * c;
				const uint32_t nameStringId = readU32(element + mStructureSoftCeilingNameOffset);

				const int32_t triangleCount = readI32(element + mStructureSoftCeilingTrianglesBlock);
				const uintptr_t triangleBase =
					resolveTagBlock(readU32(element + mStructureSoftCeilingTrianglesBlock + 4));
				if (!plausiblePointer(triangleBase) || triangleCount <= 0 || triangleCount > 65535) continue;

				++mLastGeometryRecords;

				SoftCeilingBatch batch;
				batch.nameStringId = nameStringId;
				// The GEOMETRY record's own type, so colouring never depends on the name join below.
				if (!decodeSoftCeilingType(readU16(element + mStructureSoftCeilingTypeOffset), batch.type))
					++mLastBadTypes;

				const auto match = byName.find(nameStringId);
				if (match != byName.end())
				{
					batch.ignoreMask = match->second.ignoreMask;
					batch.scenarioIndex = match->second.index;
					// The scenario record is the authoring one and the enable bit belongs to it, so prefer its
					// type when the two ever disagree.
					batch.type = match->second.type;
				}
				else
				{
					++mLastUnmatched;
				}

				if (stringIdUsable && callStringIdText(mStringIdText, nameStringId, nameBuffer, sizeof(nameBuffer)))
					batch.name = nameBuffer;
				else
					batch.name = std::format("soft_ceiling_{}_{}", d, c);   // (design, record) - stable across splits

				batch.renderVertices.reserve((size_t)triangleCount * 3);
				batch.triangleIndices.reserve((size_t)triangleCount * 3);
				batch.edgeIndices.reserve((size_t)triangleCount * 6);

				auto flushBatch = [&]()
					{
						if (batch.renderVertices.empty()) return;
						finaliseBatchBounds(batch);
						SoftCeilingBatch next;
						next.name = batch.name;
						next.nameStringId = batch.nameStringId;
						next.type = batch.type;
						next.ignoreMask = batch.ignoreMask;
						next.scenarioIndex = batch.scenarioIndex;
						mBatches.push_back(std::move(batch));
						batch = std::move(next);
					};

				for (int32_t t = 0; t < triangleCount; ++t)
				{
					const uintptr_t triangle = triangleBase + (uintptr_t)mSoftCeilingTriangleStride * t;

					float raw[9]{};
					if (!HCEGetPlayerState::tryReadRaw(triangle + mSoftCeilingTriangleVertex0Offset, raw, sizeof(raw)))
						continue;
					const SimpleMath::Vector3 v0(raw[0], raw[1], raw[2]);
					const SimpleMath::Vector3 v1(raw[3], raw[4], raw[5]);
					const SimpleMath::Vector3 v2(raw[6], raw[7], raw[8]);

					// CROSS-CHECK AGAINST THE TRIANGLE'S OWN BOUNDING SPHERE. No instruction in the engine reads
					// these three vertices (it uses only the plane and the sphere), so although the struct walk
					// closes exactly on 68 and pins them, this stays as a cheap independent confirmation: a
					// centroid far outside the record's own sphere means we are reading the wrong field, and
					// dropping it beats drawing a triangle stretched across the level.
					float sphere[4]{};
					if (HCEGetPlayerState::tryReadRaw(triangle + mSoftCeilingTriangleSphereOffset, sphere, sizeof(sphere)))
					{
						const float radius = sphere[3];
						if (radius > 0.f && radius < 1e5f)
						{
							const SimpleMath::Vector3 centroid = (v0 + v1 + v2) / 3.f;
							const SimpleMath::Vector3 delta = centroid - SimpleMath::Vector3(sphere[0], sphere[1], sphere[2]);
							if (delta.LengthSquared() > radius * radius * 4.f + 1.f) { ++mLastRejectedTriangles; continue; }
						}
					}

					if (batch.renderVertices.size() / 3 >= kMaxTrianglesPerBatch) flushBatch();

					const uint16_t first = (uint16_t)batch.renderVertices.size();
					batch.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(v0.x, v0.y, v0.z)));
					batch.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(v1.x, v1.y, v1.z)));
					batch.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(v2.x, v2.y, v2.z)));

					batch.triangleIndices.push_back(first);
					batch.triangleIndices.push_back((uint16_t)(first + 1));
					batch.triangleIndices.push_back((uint16_t)(first + 2));

					batch.edgeIndices.push_back(first);                  batch.edgeIndices.push_back((uint16_t)(first + 1));
					batch.edgeIndices.push_back((uint16_t)(first + 1));  batch.edgeIndices.push_back((uint16_t)(first + 2));
					batch.edgeIndices.push_back((uint16_t)(first + 2));  batch.edgeIndices.push_back(first);

					++mLastTriangles;
				}

				flushBatch();
			}
		}
	}

	void logBuildResult() const
	{
		PLOG_INFO << "HCESoftCeilingOverlay rebuilt: " << mLastTriangles << " triangles across "
			<< mBatches.size() << " batches, from " << mLastGeometryRecords << " structure_design soft ceiling "
			"records and " << mLastScenarioRecords << " scenario records"
			<< " (" << mLastUnmatched << " geometry records had no scenario counterpart, "
			<< mLastRejectedTriangles << " triangles rejected by the bounding-sphere cross-check, "
			<< mLastBadTypes << " type fields outside the 3-option enum)";
	}

	void set3DRenderingEnabled(bool enabled)
	{
		if (!enabled)
		{
			if (mRender3DEventCallback)
			{
				if (mCurrentlyRendering3D) mCurrentlyRendering3D.wait(true);
				mRender3DEventCallback.reset();
			}
			return;
		}

		if (mRender3DEventCallback) return;
		if (!mRender3DProviderOptionalWeak.has_value()) return;

		auto provider = mRender3DProviderOptionalWeak.value().lock();
		if (!provider) return;
		if (provider->d3d12RendererHasFailed()) return;

		mRender3DEventCallback = provider->getRender3DEvent()->subscribe(
			[this](GameState g, IRenderer3D* r) { onRender3DEvent(g, r); });
	}

	void invalidateCache()
	{
		std::scoped_lock batchesLock(mBatchesMutex);
		mBatches.clear();
		mCacheValid = false;
		mCacheKey = 0;
		mLastCacheCheck = {};
	}

	void onRender3DEvent(GameState game, IRenderer3D* renderer)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;
		if (!renderer) return;
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER) return;

		ScopedAtomicBool renderingLock(mCurrentlyRendering3D);

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (!mAnchorsGood) return;

			std::scoped_lock batchesLock(mBatchesMutex);

			// Static tag data. The KEY is checked on a slow timer (so a BSP transition is picked up) and the
			// rebuild only happens when it actually changed.
			const auto now = std::chrono::steady_clock::now();
			if (!mCacheValid || mLastCacheCheck.time_since_epoch().count() == 0
				|| (now - mLastCacheCheck) >= std::chrono::milliseconds(kCacheCheckIntervalMs))
			{
				mLastCacheCheck = now;
				const uint64_t key = computeCacheKey();
				if (key == 0) { mBatches.clear(); mCacheValid = false; return; }   // no level loaded
				if (!mCacheValid || key != mCacheKey)
				{
					mCacheKey = key;
					mCacheValid = true;
					rebuild();
					logBuildResult();
				}
			}
			if (mBatches.empty()) return;

			// The SAME settings objects MCC's soft ceiling overlay uses. HaloCER and MCC can never share a
			// process, so presets and the existing Soft Ceiling Overlay hotkey carry over unchanged - the same
			// decision the HaloCER trigger overlay made.
			const auto desiredSubjects = settings->softCeilingOverlayRenderTypes->GetValue();
			const bool wantsVehicles = desiredSubjects == SettingsEnums::SoftCeilingRenderTypes::Vehicles
				|| desiredSubjects == SettingsEnums::SoftCeilingRenderTypes::BipedsOrVehicles;
			const bool wantsBipeds = desiredSubjects == SettingsEnums::SoftCeilingRenderTypes::Bipeds
				|| desiredSubjects == SettingsEnums::SoftCeilingRenderTypes::BipedsOrVehicles;

			const auto renderDirection = settings->softCeilingOverlayRenderDirection->GetValue();
			const CullingOption cullingOption =
				renderDirection == SettingsEnums::SoftCeilingRenderDirection::Both ? CullingOption::CullNone :
				renderDirection == SettingsEnums::SoftCeilingRenderDirection::Front ? CullingOption::CullBack
				: CullingOption::CullFront;

			const float solidOpacity = std::clamp(settings->softCeilingOverlaySolidTransparency->GetValue(), 0.f, 1.f);
			const float wireOpacity = std::clamp(settings->softCeilingOverlayWireframeTransparency->GetValue(), 0.f, 1.f);
			const bool wantSolid = solidOpacity > 0.f;
			const bool wantWire = wireOpacity > 0.f;
			if (!wantSolid && !wantWire) return;

			SimpleMath::Vector4 colourAccel = settings->softCeilingOverlayColorAccel->GetValue();
			SimpleMath::Vector4 colourKill = settings->softCeilingOverlayColorKill->GetValue();
			SimpleMath::Vector4 colourSlippy = settings->softCeilingOverlayColorSlippy->GetValue();

			const bool showDisabled = settings->hceSoftCeilingOverlayShowDisabled->GetValue();
			SimpleMath::Vector4 colourDisabled = settings->hceSoftCeilingOverlayDisabledColor->GetValue();
			const float renderDistance = settings->hceSoftCeilingOverlayRenderDistance->GetValue();

			// One read of the whole mask pointer per frame rather than per batch.
			const uintptr_t maskBase = readEnableMaskBase();

			const SimpleMath::Vector3 cameraPosition = renderer->getCameraPosition();
			const DirectX::BoundingFrustum& cameraFrustum = renderer->getCameraFrustum();

			for (const SoftCeilingBatch& batch : mBatches)
			{
				if (batch.renderVertices.empty()) continue;

				const bool isDesiredSubject = (wantsVehicles && batch.appliesToVehicle())
					|| (wantsBipeds && batch.appliesToBiped());
				if (!isDesiredSubject) continue;

				const bool enabled = softCeilingIsEnabled(maskBase, batch.scenarioIndex);
				if (!enabled && !showDisabled) continue;

				if ((batch.center - cameraPosition).Length() - batch.radius > renderDistance) continue;
				if (!cameraFrustum.Intersects(DirectX::BoundingSphere(batch.center, batch.radius))) continue;

				SimpleMath::Vector4 colour = !enabled ? colourDisabled
					: batch.type == SoftCeilingType::Acceleration ? colourAccel
					: batch.type == SoftCeilingType::SoftKill ? colourKill
					: colourSlippy;

				if (wantSolid)
				{
					SimpleMath::Vector4 solid = colour;
					solid.w = std::clamp(colour.w * solidOpacity, 0.f, 1.f);
					renderer->drawTriangleCollection(&batch, solid, cullingOption);
				}
				if (wantWire)
				{
					SimpleMath::Vector4 wire = colour;
					wire.w = std::clamp(colour.w * wireOpacity, 0.f, 1.f);
					renderer->drawEdgeCollection(&batch, wire);
				}
			}
		}
		catch (HCMRuntimeException ex)
		{
			PLOG_ERROR << "HCESoftCeilingOverlay render error: " << std::endl << ex.what();
			runtimeExceptions->handleMessage(ex);
			mIsActive = false;   // do not spew one message per frame
		}
	}

	void onToggleEvent(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) { mIsActive = false; return; }

			if (!newValue)
			{
				mIsActive = false;
				set3DRenderingEnabled(false);
				detachCameraHook();
				messagesGUI->addMessage("Soft Ceiling Overlay disabled.");
				return;
			}

			lockOrThrow(playerStateWeak, playerState);

			attachCameraHook();
			set3DRenderingEnabled(true);

			// Resolve on the TOGGLE, never on the render thread: the scan walks the whole executable image.
			if (!mAnchorsTried || !mAnchorsGood)
				resolveAnchors(playerState->getSimModuleBase());

			if (!mAnchorsGood)
			{
				mIsActive = false;
				throw HCMRuntimeException(std::format(
					"Soft Ceiling Overlay can't run on this game build: could not locate the {} by byte "
					"signature. This usually means Halo Campaign Evolved updated. (It reports no version number "
					"at all, so HCM cannot detect an update any other way, and would rather refuse than read "
					"geometry from the wrong address.)", mAnchorFailure));
			}

			// The D3D12 renderer carries this feature alone - there is no ImGui fallback for a triangle soup.
			// Say so rather than sitting there drawing nothing.
			if (!mRender3DEventCallback)
			{
				mIsActive = false;
				throw HCMRuntimeException("Soft Ceiling Overlay needs HCM's 3D renderer, which is not available "
					"(the D3D12 hook has not initialised). Try toggling it again once you are in-game.");
			}

			size_t triangles = 0, records = 0;
			{
				std::scoped_lock batchesLock(mBatchesMutex);
				mCacheKey = computeCacheKey();
				mCacheValid = true;
				mLastCacheCheck = std::chrono::steady_clock::now();
				rebuild();
				logBuildResult();
				triangles = mLastTriangles;
				records = mLastGeometryRecords;
			}

			mIsActive = true;
			if (records == 0)
				messagesGUI->addMessage("Soft Ceiling Overlay on, but this level has no soft ceilings.");
			else
				messagesGUI->addMessage(std::format("Soft Ceiling Overlay on: {} barriers, {} triangles.",
					records, triangles));
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onGameStateChanged(const MCCState& newState)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);

			invalidateCache();   // a level transition invalidates every cached tag pointer

			const bool want = settings->softCeilingOverlayToggle->GetValue()
				&& newState.currentGameState == mGame
				&& newState.currentPlayState == PlayState::Ingame;

			// The toggle can already be on when a level loads (a persisted setting, or a preset). This runs on
			// the state-hook thread, not the render thread, so the image scan is not a frame hitch.
			if (want && !mAnchorsGood)
			{
				if (auto playerState = playerStateWeak.lock())
				{
					try { resolveAnchors(playerState->getSimModuleBase()); }
					catch (HCMRuntimeException) {}
				}
			}

			try
			{
				if (want) attachCameraHook();
				else detachCameraHook();
			}
			catch (HCMRuntimeException) {}

			// Self-heal, same as the other two HaloCER overlays: attachCameraHook is a no-op once this overlay's
			// request is registered, so if anything detached the midhook behind our back it would otherwise stay
			// detached for the rest of the session.
			if (want && mCameraDataOptionalWeak.has_value())
				if (auto cameraData = mCameraDataOptionalWeak.value().lock())
					cameraData->ensureHookLive();

			set3DRenderingEnabled(want);
			mIsActive = want && mAnchorsGood;
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes in its own constructor, so everything it touches must already
	// be initialised.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	HCESoftCeilingOverlayImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->softCeilingOverlayToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		// explicit cast - GameState has operator==(GameState) AND operator Value(), so a direct compare is C2666
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCESoftCeilingOverlay only supports Halo Campaign Evolved");

		// Pure PointerDataStore lookups. NOTHING here touches game memory - the sim dll is not guaranteed to be
		// loaded when cheats are constructed.
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
#define hceOffset(member, name) member = *ptr->getData<std::shared_ptr<int64_t>>(nameof(name), mGame)
		hceOffset(mScenarioSoftCeilingBlock, hceScenarioSoftCeilingBlock);
		hceOffset(mScenarioSoftCeilingStride, hceScenarioSoftCeilingStride);
		hceOffset(mScenarioSoftCeilingFlagsOffset, hceScenarioSoftCeilingFlagsOffset);
		hceOffset(mScenarioSoftCeilingNameOffset, hceScenarioSoftCeilingNameOffset);
		hceOffset(mScenarioSoftCeilingTypeOffset, hceScenarioSoftCeilingTypeOffset);
		hceOffset(mScenarioStructureDesignBlock, hceScenarioStructureDesignBlock);
		hceOffset(mStructureDesignReferenceStride, hceStructureDesignReferenceStride);
		hceOffset(mStructureDesignTagIndexOffset, hceStructureDesignTagIndexOffset);
		hceOffset(mStructureDesignSoftCeilingBlock, hceStructureDesignSoftCeilingBlock);
		hceOffset(mStructureSoftCeilingStride, hceStructureSoftCeilingStride);
		hceOffset(mStructureSoftCeilingNameOffset, hceStructureSoftCeilingNameOffset);
		hceOffset(mStructureSoftCeilingTypeOffset, hceStructureSoftCeilingTypeOffset);
		hceOffset(mStructureSoftCeilingTrianglesBlock, hceStructureSoftCeilingTrianglesBlock);
		hceOffset(mSoftCeilingTriangleStride, hceSoftCeilingTriangleStride);
		hceOffset(mSoftCeilingTriangleSphereOffset, hceSoftCeilingTriangleSphereOffset);
		hceOffset(mSoftCeilingTriangleVertex0Offset, hceSoftCeilingTriangleVertex0Offset);
		hceOffset(mTagInstanceTableOffset, hceTagInstanceTableOffset);
		hceOffset(mTagInstanceStride, hceTagInstanceStride);
		hceOffset(mTagInstanceDataOffset, hceTagInstanceDataOffset);
#undef hceOffset

		// Failing to resolve either of these must NOT throw out of the constructor and take down cheat
		// registration; onToggleEvent reports them instead.
		try
		{
			mRender3DProviderOptionalWeak = resolveDependentCheat(Render3DEventProvider);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCESoftCeilingOverlay could not resolve Render3DEventProvider; the soft ceiling "
				"overlay will be unavailable (it has no ImGui fallback)";
		}

		try
		{
			mCameraDataOptionalWeak = resolveDependentCheat(HCEGetCameraData);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCESoftCeilingOverlay could not resolve HCEGetCameraData; the soft ceiling overlay "
				"would draw with the sim's player camera and a default field of view";
		}

		mReady.store(true, std::memory_order_release);
	}

	~HCESoftCeilingOverlayImpl()
	{
		mReady.store(false, std::memory_order_release);
		mIsActive = false;
		set3DRenderingEnabled(false);
		detachCameraHook();
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCESoftCeilingOverlay::HCESoftCeilingOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCESoftCeilingOverlayImpl>(game, dicon))
{
}

HCESoftCeilingOverlay::~HCESoftCeilingOverlay()
{
	PLOG_VERBOSE << "~" << getName();
}
