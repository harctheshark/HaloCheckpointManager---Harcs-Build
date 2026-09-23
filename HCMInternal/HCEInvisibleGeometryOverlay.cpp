#include "pch.h"
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <set>
#include "HCEInvisibleGeometryOverlay.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"
#include "HCESignatureScan.h"
#include "PointerDataStore.h"
#include "GlobalKill.h"
#include "Render3DEventProvider.h"
#include "IRenderer3D.h"
#include "IModel.h"
#include <algorithm>
#include <atomic>
#include <cmath>

// See HCEInvisibleGeometryOverlay.h for what this draws, where the data lives and why "invisible" means what it does.
//
// Addresses (scenario slot, tag address table, tag globals, string_id text) are BYTE-SIGNATURE anchored exactly as
// HCEBspOverlay does it - HaloSimulation_tag_release.dll reports no version, so a stale remembered address would
// draw confident nonsense. Only STRUCTURAL tag constants live in InternalPointerData.xml.
namespace
{
	uint32_t  readU32(uintptr_t a) { uint32_t v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	int32_t   readI32(uintptr_t a) { int32_t v = 0;  HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	uintptr_t readPtr(uintptr_t a) { uintptr_t v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	bool plausiblePointer(uintptr_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull; }

	template <typename T> T readLocal(const std::vector<uint8_t>& buffer, size_t offset)
	{
		T v{};
		if (offset + sizeof(T) <= buffer.size()) memcpy(&v, buffer.data() + offset, sizeof(T));
		return v;
	}

	// The SMALL collision bsp layout - instanced geometry never uses the large one (walked: the definition's
	// 'collision info' is an inline global_collision_bsp_struct, 108 B). Surface 14 B, edge 12 B, vertex 16 B.
	constexpr size_t kSurfaceStride = 14, kEdgeStride = 12, kVertexStride = 16;
	constexpr size_t kSurfaceFirstEdge = 0x02, kSurfaceFlags = 0x0A;
	constexpr size_t kEdgeStart = 0x00, kEdgeEnd = 0x02, kEdgeForward = 0x04, kEdgeReverse = 0x06, kEdgeRight = 0x0A;
	constexpr uint32_t kSentinel = 0xFFFFu;

	// surface_flags (a real 9-option definition in this build)
	constexpr uint16_t kSurfaceInvisible = 0x0002;
	constexpr uint16_t kSurfaceInvalid = 0x0010;
	constexpr uint16_t kSurfacePathfindingOnly = 0x0100;
	// instance flags bit0 'render only' - drawn by UE, NOT solid; the engine skips it for collision and Havok.
	constexpr uint16_t kInstanceRenderOnly = 0x0001;

	constexpr size_t kMaxRingLength = 64;          // the engine's own ring guard
	constexpr size_t kMaxBatchVertices = 60000;    // u16 indices
	constexpr int32_t kMaxBsps = 256;
	constexpr int32_t kMaxDefinitions = 3072;      // block maximum from the definition
	constexpr int32_t kMaxInstances = 16384;       // block maximum from the definition
	constexpr int32_t kMaxSurfacesPerDefinition = 65535;
	// Per-frame upload budget in vertices. Every draw re-uploads its whole vertex array into ONE upload ring that
	// every HCM overlay shares, and a draw that does not fit is dropped silently - so stop early, nearest first.
	constexpr size_t kFrameVertexBudget = 400000;

	// One placement, already in world space, so the render thread reads no tag data and allocates nothing.
	struct PlacementBatch
	{
		VertexCollection vertices;
		IndexCollection triangles;   // indices into vertices (the per-face ring copies)
		IndexCollection edges;       // indices into vertices (the de-duplicated edge endpoints)
		SimpleMath::Vector3 center{};
		float radius = 0.f;
	};

	struct Snapshot
	{
		std::vector<PlacementBatch> batches;
	};

	class PlacementModel : public IModelTriangles, public IModelEdges
	{
	public:
		explicit PlacementModel(const PlacementBatch& b) : mBatch(b) {}
		const VertexCollection& getTriangleVertices() const override { return mBatch.vertices; }
		const IndexCollection& getTriangleIndices() const override { return mBatch.triangles; }
		const VertexCollection& getEdgeVertices() const override { return mBatch.vertices; }
		const IndexCollection& getEdgeIndices() const override { return mBatch.edges; }
	private:
		const PlacementBatch& mBatch;
	};

	// Ear-clipping triangulation of one edge ring (same algorithm as HCEBspOverlay - collision rings are N-gons and
	// may be concave, so a fan is wrong). Emits indices baseIndex + i for ring position i.
	void triangulateRing(const std::vector<SimpleMath::Vector3>& points, uint16_t baseIndex, IndexCollection& out)
	{
		const size_t n = points.size();
		if (n < 3) return;
		if (n == 3)
		{
			out.push_back(baseIndex); out.push_back((uint16_t)(baseIndex + 1)); out.push_back((uint16_t)(baseIndex + 2));
			return;
		}
		SimpleMath::Vector3 normal(0.f, 0.f, 0.f);   // Newell
		for (size_t i = 0; i < n; ++i)
		{
			const SimpleMath::Vector3& a = points[i];
			const SimpleMath::Vector3& b = points[(i + 1) % n];
			normal.x += (a.y - b.y) * (a.z + b.z);
			normal.y += (a.z - b.z) * (a.x + b.x);
			normal.z += (a.x - b.x) * (a.y + b.y);
		}
		if (normal.LengthSquared() < 1e-20f) return;
		normal.Normalize();
		SimpleMath::Vector3 axisU = (std::abs(normal.z) < 0.9f) ? SimpleMath::Vector3::UnitZ.Cross(normal)
			: SimpleMath::Vector3::UnitX.Cross(normal);
		if (axisU.LengthSquared() < 1e-20f) return;
		axisU.Normalize();
		const SimpleMath::Vector3 axisV = normal.Cross(axisU);

		std::vector<SimpleMath::Vector2> flat(n);
		for (size_t i = 0; i < n; ++i) flat[i] = SimpleMath::Vector2(points[i].Dot(axisU), points[i].Dot(axisV));
		float signedArea2 = 0.f;
		for (size_t i = 0; i < n; ++i) signedArea2 += flat[i].x * flat[(i + 1) % n].y - flat[(i + 1) % n].x * flat[i].y;

		std::vector<size_t> remaining(n);
		for (size_t i = 0; i < n; ++i) remaining[i] = i;
		if (signedArea2 < 0.f) std::reverse(remaining.begin(), remaining.end());
		auto cross2 = [](const SimpleMath::Vector2& o, const SimpleMath::Vector2& a, const SimpleMath::Vector2& b)
			{ return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };

		size_t guard = 0;
		const size_t guardLimit = n * n + 16;
		while (remaining.size() > 3 && guard++ < guardLimit)
		{
			bool clipped = false;
			const size_t m = remaining.size();
			for (size_t i = 0; i < m; ++i)
			{
				const size_t ia = remaining[(i + m - 1) % m], ib = remaining[i], ic = remaining[(i + 1) % m];
				if (cross2(flat[ia], flat[ib], flat[ic]) <= 0.f) continue;
				bool contains = false;
				for (size_t k = 0; k < m && !contains; ++k)
				{
					const size_t ip = remaining[k];
					if (ip == ia || ip == ib || ip == ic) continue;
					contains = cross2(flat[ia], flat[ib], flat[ip]) >= 0.f && cross2(flat[ib], flat[ic], flat[ip]) >= 0.f
						&& cross2(flat[ic], flat[ia], flat[ip]) >= 0.f;
				}
				if (contains) continue;
				out.push_back((uint16_t)(baseIndex + ia)); out.push_back((uint16_t)(baseIndex + ib)); out.push_back((uint16_t)(baseIndex + ic));
				remaining.erase(remaining.begin() + i);
				clipped = true;
				break;
			}
			if (!clipped) break;   // malformed ring: fan the remainder rather than drop the face
		}
		for (size_t i = 1; i + 1 < remaining.size(); ++i)
		{
			out.push_back((uint16_t)(baseIndex + remaining[0]));
			out.push_back((uint16_t)(baseIndex + remaining[i]));
			out.push_back((uint16_t)(baseIndex + remaining[i + 1]));
		}
	}

	// string_id -> text through the engine's own function (hash-table lookup, no TLS - see HCESoftCeilingOverlay).
	// POD-only because of __try (C2712).
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
				if (c == '\0' || c < 0x20 || c > 0x7E) break;
				out[i] = c;
			}
			out[i] = '\0';
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
	}

	// A definition's selected collision surfaces, de-duplicated, in LOCAL space. Built once per definition and
	// transformed per placement.
	struct DefinitionMesh
	{
		std::vector<SimpleMath::Vector3> localVertices;       // by collision vertex index (only used ones are read)
		std::vector<std::vector<uint32_t>> faces;             // rings of collision vertex indices
		std::vector<std::pair<uint32_t, uint32_t>> edges;     // unordered pairs of collision vertex indices
		std::vector<uint32_t> usedVertices;                   // collision vertex indices the edges reference
	};

	enum class DefinitionClass : uint8_t { Visible, Invisible, NoRenderMesh, Navmesh };
	const char* className(DefinitionClass c)
	{
		switch (c)
		{
		case DefinitionClass::Invisible: return "INV";
		case DefinitionClass::NoRenderMesh: return "ZERO-UNFLAGGED";
		case DefinitionClass::Navmesh: return "NAV";
		default: return "VIS";
		}
	}
}


class HCEInvisibleGeometryOverlay::HCEInvisibleGeometryOverlayImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;
	std::optional<std::weak_ptr<Render3DEventProvider>> mRender3DProviderOptionalWeak;
	std::optional<std::weak_ptr<HCEGetCameraData>> mCameraDataOptionalWeak;
	bool mCameraHookRequested = false;

	// Structural tag constants from InternalPointerData.xml (see the header comment for what each one is).
	int64_t mScenarioStructureBspBlock = 0, mStructureBspReferenceStride = 0, mStructureBspTagIndexOffset = 0,
		mStructureBspLocalTagIndexOffset = 0, mTagInstanceTableOffset = 0, mTagInstanceStride = 0, mTagInstanceDataOffset = 0,
		mStructureBspResourceBlock = 0, mCollisionSurfacesBlock = 0, mCollisionEdgesBlock = 0, mCollisionVerticesBlock = 0;
	int64_t mInstancesBlock = 0, mInstanceStride = 0, mInstanceScale = 0, mInstanceForward = 0, mInstanceLeft = 0,
		mInstanceUp = 0, mInstancePosition = 0, mInstanceDefinitionIndex = 0, mInstanceFlags = 0, mInstanceSphereCenter = 0,
		mInstanceSphereRadius = 0, mInstanceName = 0, mInstancePhysicsBlock = 0, mUseResourceItems = 0,
		mResourceInstancedGeometryBlock = 0, mDefinitionStride = 0, mDefinitionCollisionBsp = 0, mDefinitionMeshIndex = 0,
		mMeshesBlock = 0, mMeshStride = 0, mMeshPartsBlock = 0;

	// Byte-signature anchored.
	uintptr_t mScenarioSlot = 0, mTagAddressTable = 0, mTagGlobalsSlot = 0;
	FnStringIdText mStringIdText = nullptr;
	uintptr_t mStringIdTableSlot = 0;
	std::atomic_bool mAnchorsGood{ false };
	bool mAnchorsTried = false;
	std::string mAnchorFailure;

	std::atomic_bool mReady{ false };
	std::atomic_bool mIsActive{ false };

	// ---- render path (same lifetime gate as HCEBspOverlay: the render thread never blocks) ----
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRender3DEventCallback;
	std::shared_timed_mutex mRender3DGuard;
	std::atomic_bool mRender3DDraining{ false };
	std::mutex mRender3DSubscriptionMutex;

	// ---- snapshot hand-off: worker publishes, render thread try_locks and caches ----
	std::mutex mSnapshotMutex;
	std::shared_ptr<const Snapshot> mSnapshot;        // current, guarded by mSnapshotMutex
	std::shared_ptr<const Snapshot> mPrevSnapshot;    // one generation longer, so big frees land on the worker
	std::shared_ptr<const Snapshot> mRenderSnapshot;  // render thread only (and teardown after the drain)
	std::vector<const PlacementBatch*> mVisible;      // render thread scratch
	std::vector<std::pair<float, const PlacementBatch*>> mVisibleSorted;

	// Last camera position, written by the render thread, read by the worker for the "nearest placements" log.
	std::atomic<float> mCamX{ 0.f }, mCamY{ 0.f }, mCamZ{ 0.f };

	// ---- worker ----
	std::mutex mWorkerStartMutex;
	std::mutex mWorkerWaitMutex;
	std::condition_variable_any mWorkerCv;
	std::atomic_bool mRebuildRequested{ false };
	std::atomic_bool mAnnounceNextBuild{ false };
	uint64_t mBuiltKey = 0;   // worker thread only
	std::jthread mWorker;

	// ------------------------------------------------------------------------------------------------------------
	void attachCameraHook()
	{
		if (!mCameraDataOptionalWeak.has_value())
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the Invisible "
				"Geometry overlay cannot read the render camera");
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData)
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the Invisible "
				"Geometry overlay cannot read the render camera");
		if (mCameraHookRequested) return;
		cameraData->setHookWanted(this, true);
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
			catch (HCMRuntimeException) {}
		}
	}

	uintptr_t resolveTagBlock(uint32_t encoded) const
	{
		if (encoded == 0 || encoded == 0xFFFFFFFFu) return 0;
		const uintptr_t regionBase = readPtr(mTagAddressTable + 8ull * (encoded >> 28));
		if (!plausiblePointer(regionBase)) return 0;
		return regionBase + 4ull * encoded;
	}

	// One guarded read for a whole tag block.
	bool copyBlock(uintptr_t blockField, size_t stride, int32_t maxCount, std::vector<uint8_t>& out, int32_t& outCount) const
	{
		out.clear();
		outCount = readI32(blockField);
		const uint32_t encoded = readU32(blockField + 4);
		if (outCount <= 0 || outCount > maxCount || encoded == 0 || encoded == 0xFFFFFFFFu) { outCount = 0; return false; }
		const uintptr_t base = resolveTagBlock(encoded);
		if (!plausiblePointer(base)) { outCount = 0; return false; }
		out.resize((size_t)outCount * stride);
		if (!HCEGetPlayerState::tryReadRaw(base, out.data(), out.size())) { out.clear(); outCount = 0; return false; }
		return true;
	}

	void resolveAnchors(uintptr_t simBase)   // never throws
	{
		mAnchorsTried = true;
		mAnchorsGood = false;
		mAnchorFailure.clear();
		int hits = 0;

		const uintptr_t scenarioInsn = HCESignatureScan::resolveUnique(simBase, "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00", hits);
		if (!scenarioInsn) { mAnchorFailure = std::format("scenario data pointer ({} matches)", hits); return; }
		mScenarioSlot = HCESignatureScan::ripTarget(scenarioInsn, 3, 7);

		const uintptr_t tableInsn = HCESignatureScan::resolveUnique(simBase,
			"48 8B D9 4C 8D 05 ?? ?? ?? ?? 8B 09 8B 53 08 8B C2 48 C1 E8 1C 49 8B 04 C0 3B 4C 90 10 7D", hits);
		if (!tableInsn) { mAnchorFailure = std::format("tag address table ({} matches)", hits); return; }
		mTagAddressTable = HCESignatureScan::ripTarget(tableInsn, 6, 10);

		const uintptr_t tagGetData = HCESignatureScan::resolveUnique(simBase,
			"48 89 5C 24 08 57 48 83 EC 20 0F B7 C2 8B DA 48 8D 3C 40 48 8B 05 ?? ?? ?? ?? 48 C1 E7 04 48 03 78 50", hits);
		if (!tagGetData) { mAnchorFailure = std::format("tag instance table ({} matches)", hits); return; }
		mTagGlobalsSlot = HCESignatureScan::ripTarget(tagGetData + 0x13, 3, 7);

		if (!mScenarioSlot || !mTagAddressTable || !mTagGlobalsSlot) { mAnchorFailure = "rip-relative operand read failed"; return; }

		// OPTIONAL: placement names in the log. Everything still draws without it.
		const uintptr_t sidFunction = HCESignatureScan::resolveUnique(simBase, "40 53 55 56 57 48 83 EC 28 48 8B 3D ?? ?? ?? ?? 48 63 E9", hits);
		if (sidFunction)
		{
			const uintptr_t loadInsn = sidFunction + 9;
			uint8_t opcode[3]{};
			if (HCEGetPlayerState::tryReadRaw(loadInsn, opcode, sizeof(opcode)) && opcode[0] == 0x48 && opcode[1] == 0x8B && opcode[2] == 0x3D)
			{
				mStringIdTableSlot = HCESignatureScan::ripTarget(loadInsn, 3, 7);
				if (mStringIdTableSlot) mStringIdText = (FnStringIdText)sidFunction;
			}
		}
		mAnchorsGood = true;
	}

	std::string stringIdText(uint32_t sid) const
	{
		if (mStringIdText && mStringIdTableSlot && readPtr(mStringIdTableSlot) != 0)
		{
			char buf[128];
			if (callStringIdText(mStringIdText, sid, buf, sizeof(buf))) return std::string(buf);
		}
		return std::format("sid 0x{:08X}", sid);
	}

	// scenario -> i'th structure bsp tag data, inlining bsp_from_index (which takes a lock) exactly as HCEBspOverlay does.
	uintptr_t resolveStructureBsp(uintptr_t scenario, int index) const
	{
		const uintptr_t base = resolveTagBlock(readU32(scenario + mScenarioStructureBspBlock + 4));
		if (!plausiblePointer(base)) return 0;
		const uintptr_t element = base + (uintptr_t)mStructureBspReferenceStride * index;
		uint32_t tagIndex = readU32(element + mStructureBspTagIndexOffset);
		if (tagIndex == 0xFFFFFFFFu) tagIndex = readU32(element + mStructureBspLocalTagIndexOffset);
		if (tagIndex == 0xFFFFFFFFu) return 0;
		const uintptr_t tagGlobals = readPtr(mTagGlobalsSlot);
		if (!plausiblePointer(tagGlobals)) return 0;
		const uintptr_t instances = readPtr(tagGlobals + mTagInstanceTableOffset);
		if (!plausiblePointer(instances)) return 0;
		const uintptr_t instance = instances + (uintptr_t)mTagInstanceStride * (tagIndex & 0xFFFFu);
		return resolveTagBlock(readU32(instance + mTagInstanceDataOffset));
	}

	// Rebuild key: every loaded bsp's tag address plus its resource and instance block addresses. A zone switch
	// changes the set of resolved bsps, so this changes with it (the scenario pointer alone would not).
	uint64_t computeKey() const
	{
		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return 0;
		const int32_t bspCount = readI32(scenario + mScenarioStructureBspBlock);
		if (bspCount <= 0 || bspCount > kMaxBsps) return 0;
		uint64_t h = 1469598103934665603ull;
		auto mix = [&h](uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; } };
		mix(scenario);
		mix((uint64_t)bspCount);
		for (int b = 0; b < bspCount; ++b)
		{
			const uintptr_t sbsp = resolveStructureBsp(scenario, b);
			mix(sbsp);
			if (!plausiblePointer(sbsp)) continue;
			mix(readU32(sbsp + mStructureBspResourceBlock + 4));
			mix(readU32(sbsp + mInstancesBlock + 4));
			mix((uint64_t)(uint32_t)readI32(sbsp + mInstancesBlock));
		}
		return h;
	}

	// Walks one surface's edge ring in the ENGINE's form (sub_1802F0170 keys on the RIGHT surface).
	static bool walkRing(const std::vector<uint8_t>& edges, int32_t edgeCount, uint32_t surface, uint32_t firstEdge,
		std::vector<uint32_t>& out)
	{
		out.clear();
		if (firstEdge == kSentinel || (int64_t)firstEdge >= edgeCount) return false;
		uint32_t edge = firstEdge;
		for (size_t step = 0; step < kMaxRingLength; ++step)
		{
			if ((int64_t)edge >= edgeCount) return false;
			const size_t e = (size_t)edge * kEdgeStride;
			const uint32_t right = readLocal<uint16_t>(edges, e + kEdgeRight);
			const uint32_t vertex = (right == surface) ? readLocal<uint16_t>(edges, e + kEdgeEnd) : readLocal<uint16_t>(edges, e + kEdgeStart);
			const uint32_t next = (right == surface) ? readLocal<uint16_t>(edges, e + kEdgeReverse) : readLocal<uint16_t>(edges, e + kEdgeForward);
			if (vertex == kSentinel) return false;
			out.push_back(vertex);
			if (next == kSentinel) return false;
			if (next == firstEdge) return out.size() >= 3;
			edge = next;
		}
		return false;
	}

	// ------------------------------------------------------------------------------------------------------------
	// THE REBUILD (worker thread). Returns nullptr if interrupted by a stop request.
	// ------------------------------------------------------------------------------------------------------------
	std::shared_ptr<Snapshot> buildSnapshot(std::stop_token st)
	{
		auto snapshot = std::make_shared<Snapshot>();
		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return snapshot;
		const int32_t bspCount = readI32(scenario + mScenarioStructureBspBlock);
		if (bspCount <= 0 || bspCount > kMaxBsps) return snapshot;

		const SimpleMath::Vector3 camera(mCamX.load(), mCamY.load(), mCamZ.load());
		struct NearEntry { float distance; int bsp; int instance; int definition; DefinitionClass cls; uint32_t name;
			uint16_t flags; int32_t physics; int32_t parts; int32_t surfaces; SimpleMath::Vector3 centre; float radius; };
		std::vector<NearEntry> nearest;           // every placement's class and distance, trimmed to 12 at the end
		std::vector<std::string> invisibleLines;  // one per drawn placement
		size_t totalInvisiblePlacements = 0, totalFallbackPlacements = 0, totalDrawnSurfaces = 0;
		int bspsLoaded = 0;

		std::vector<uint8_t> definitions, instances, meshes, surfaces, edges, vertices;
		std::vector<uint32_t> ring;
		std::vector<SimpleMath::Vector3> ringPoints;

		for (int b = 0; b < bspCount; ++b)
		{
			if (st.stop_requested()) return nullptr;
			const uintptr_t sbsp = resolveStructureBsp(scenario, b);
			if (!plausiblePointer(sbsp)) { PLOG_INFO << std::format("HCEInvisibleGeometry bsp {}: not loaded", b); continue; }
			++bspsLoaded;

			const int32_t useResourceItems = readI32(sbsp + mUseResourceItems);
			const int32_t rawCount = readI32(sbsp + mStructureBspResourceBlock);
			if (useResourceItems != 0 || rawCount < 1)
			{
				PLOG_INFO << std::format("HCEInvisibleGeometry bsp {}: resource path not supported (use resource items {}, "
					"raw resources {}) - skipped", b, useResourceItems, rawCount);
				continue;
			}
			const uintptr_t resource = resolveTagBlock(readU32(sbsp + mStructureBspResourceBlock + 4));   // raw item 0
			if (!plausiblePointer(resource)) continue;

			int32_t definitionCount = 0, instanceCount = 0, meshCount = 0;
			copyBlock(resource + mResourceInstancedGeometryBlock, (size_t)mDefinitionStride, kMaxDefinitions, definitions, definitionCount);
			copyBlock(sbsp + mInstancesBlock, (size_t)mInstanceStride, kMaxInstances, instances, instanceCount);
			copyBlock(sbsp + mMeshesBlock, (size_t)mMeshStride, 1 << 20, meshes, meshCount);
			if (definitionCount <= 0 || instanceCount <= 0)
			{
				PLOG_INFO << std::format("HCEInvisibleGeometry bsp {}: {} definitions, {} placements - nothing instanced",
					b, definitionCount, instanceCount);
				continue;
			}

			// ---- pass 1: classify every definition from its surface flags and render mesh ----
			std::vector<DefinitionClass> cls((size_t)definitionCount, DefinitionClass::Visible);
			std::vector<int32_t> parts((size_t)definitionCount, -1), surfaceCounts((size_t)definitionCount, 0);
			size_t flagHistogram[9]{}, unflaggedZeroParts = 0, invisibleDefs = 0, partialDefs = 0, navmeshDefs = 0,
				navmeshSurfaces = 0, surfacesScanned = 0, invisibleAndZero = 0, invisibleWithParts = 0;
			for (int32_t k = 0; k < definitionCount; ++k)
			{
				const uintptr_t definitionAddress = resolveTagBlock(readU32(resource + mResourceInstancedGeometryBlock + 4))
					+ (uintptr_t)mDefinitionStride * k;
				const size_t d = (size_t)k * (size_t)mDefinitionStride;
				const int16_t meshIndex = readLocal<int16_t>(definitions, d + (size_t)mDefinitionMeshIndex);
				parts[k] = (meshIndex >= 0 && meshIndex < meshCount)
					? readLocal<int32_t>(meshes, (size_t)meshIndex * (size_t)mMeshStride + (size_t)mMeshPartsBlock) : -1;

				int32_t surfaceCount = 0;
				copyBlock(definitionAddress + mDefinitionCollisionBsp + mCollisionSurfacesBlock, kSurfaceStride,
					kMaxSurfacesPerDefinition, surfaces, surfaceCount);
				surfaceCounts[k] = surfaceCount;
				surfacesScanned += (size_t)surfaceCount;
				size_t invisible = 0, pathfinding = 0, valid = 0;
				for (int32_t s = 0; s < surfaceCount; ++s)
				{
					const uint16_t f = readLocal<uint16_t>(surfaces, (size_t)s * kSurfaceStride + kSurfaceFlags);
					for (int bit = 0; bit < 9; ++bit) if (f & (1u << bit)) ++flagHistogram[bit];
					if (f & kSurfaceInvalid) continue;
					++valid;
					if (f & kSurfaceInvisible) ++invisible;
					if (f & kSurfacePathfindingOnly) ++pathfinding;
				}
				const bool zeroParts = parts[k] <= 0;
				if (valid > 0 && pathfinding == valid) { cls[k] = DefinitionClass::Navmesh; ++navmeshDefs; navmeshSurfaces += valid; }
				else if (invisible > 0)
				{
					cls[k] = DefinitionClass::Invisible;
					++invisibleDefs;
					if (invisible < valid) ++partialDefs;
					if (zeroParts) ++invisibleAndZero; else ++invisibleWithParts;
				}
				else if (zeroParts && valid > 0) { cls[k] = DefinitionClass::NoRenderMesh; ++unflaggedZeroParts; }
			}

			// The zero-parts fallback is only trusted where render data was evidently kept.
			const bool fallbackAllowed = meshCount > 0 && unflaggedZeroParts * 2 <= (size_t)definitionCount;

			// ---- pass 2: build the selected definitions' meshes once, in local space ----
			std::vector<std::unique_ptr<DefinitionMesh>> built((size_t)definitionCount);
			size_t ringFailures = 0, vertexOutOfRange = 0;
			for (int32_t k = 0; k < definitionCount; ++k)
			{
				const bool selected = cls[k] == DefinitionClass::Invisible || (fallbackAllowed && cls[k] == DefinitionClass::NoRenderMesh);
				if (!selected) continue;
				if (st.stop_requested()) return nullptr;

				const uintptr_t definitionAddress = resolveTagBlock(readU32(resource + mResourceInstancedGeometryBlock + 4))
					+ (uintptr_t)mDefinitionStride * k;
				const uintptr_t collision = definitionAddress + mDefinitionCollisionBsp;
				int32_t surfaceCount = 0, edgeCount = 0, vertexCount = 0;
				if (!copyBlock(collision + mCollisionSurfacesBlock, kSurfaceStride, kMaxSurfacesPerDefinition, surfaces, surfaceCount)) continue;
				if (!copyBlock(collision + mCollisionEdgesBlock, kEdgeStride, 1 << 20, edges, edgeCount)) continue;
				if (!copyBlock(collision + mCollisionVerticesBlock, kVertexStride, 1 << 20, vertices, vertexCount)) continue;

				auto mesh = std::make_unique<DefinitionMesh>();
				mesh->localVertices.resize((size_t)vertexCount);
				for (int32_t v = 0; v < vertexCount; ++v)
				{
					float p[3]{};
					memcpy(p, vertices.data() + (size_t)v * kVertexStride, sizeof(p));
					mesh->localVertices[v] = SimpleMath::Vector3(p[0], p[1], p[2]);
				}

				std::set<std::vector<uint32_t>> faceKeys;
				std::set<std::pair<uint32_t, uint32_t>> edgeKeys;
				std::set<uint32_t> used;
				for (int32_t s = 0; s < surfaceCount; ++s)
				{
					const size_t r = (size_t)s * kSurfaceStride;
					const uint16_t f = readLocal<uint16_t>(surfaces, r + kSurfaceFlags);
					if (f & kSurfaceInvalid) continue;
					if (cls[k] == DefinitionClass::Invisible && !(f & kSurfaceInvisible)) continue;
					if (cls[k] == DefinitionClass::NoRenderMesh && (f & kSurfacePathfindingOnly)) continue;

					if (!walkRing(edges, edgeCount, (uint32_t)s, readLocal<uint16_t>(surfaces, r + kSurfaceFirstEdge), ring)) { ++ringFailures; continue; }
					bool inRange = true;
					for (uint32_t v : ring) if ((int64_t)v >= vertexCount) { inRange = false; break; }
					if (!inRange) { ++vertexOutOfRange; continue; }

					// The same face is stored twice on these definitions (a surface and its plane-negated twin).
					std::vector<uint32_t> key = ring;
					std::sort(key.begin(), key.end());
					if (!faceKeys.insert(key).second) continue;
					mesh->faces.push_back(ring);
					for (size_t i = 0; i < ring.size(); ++i)
					{
						uint32_t a = ring[i], c = ring[(i + 1) % ring.size()];
						if (a > c) std::swap(a, c);
						if (edgeKeys.insert({ a, c }).second) mesh->edges.push_back({ a, c });
						used.insert(a); used.insert(c);
					}
				}
				mesh->usedVertices.assign(used.begin(), used.end());
				if (!mesh->faces.empty()) built[k] = std::move(mesh);
			}

			// ---- pass 3: placements ----
			size_t invisiblePlacements = 0, fallbackPlacements = 0, renderOnlySkipped = 0, badDefinitionIndex = 0,
				badBasis = 0, outsideSphere = 0, tooLarge = 0, drawnSurfaces = 0;
			for (int32_t i = 0; i < instanceCount; ++i)
			{
				if (st.stop_requested()) return nullptr;
				const size_t base = (size_t)i * (size_t)mInstanceStride;
				const int16_t k = readLocal<int16_t>(instances, base + (size_t)mInstanceDefinitionIndex);
				const uint16_t instanceFlags = readLocal<uint16_t>(instances, base + (size_t)mInstanceFlags);
				if (k < 0 || k >= definitionCount) { ++badDefinitionIndex; continue; }
				const SimpleMath::Vector3 sphereCentre = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceSphereCenter);
				const float sphereRadius = readLocal<float>(instances, base + (size_t)mInstanceSphereRadius);

				// Every placement (any class) goes into the nearest-to-camera diagnostic.
				{
					NearEntry n{};
					n.distance = (sphereCentre - camera).Length() - sphereRadius;
					n.bsp = b; n.instance = i; n.definition = k; n.cls = cls[k];
					n.name = readLocal<uint32_t>(instances, base + (size_t)mInstanceName);
					n.flags = instanceFlags;
					n.physics = readLocal<int32_t>(instances, base + (size_t)mInstancePhysicsBlock);
					n.parts = parts[k]; n.surfaces = surfaceCounts[k]; n.centre = sphereCentre; n.radius = sphereRadius;
					nearest.push_back(n);
				}

				const DefinitionMesh* mesh = built[k].get();
				if (!mesh) continue;
				if (instanceFlags & kInstanceRenderOnly) { ++renderOnlySkipped; continue; }   // not solid

				const float scale = readLocal<float>(instances, base + (size_t)mInstanceScale);
				const SimpleMath::Vector3 F = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceForward);
				const SimpleMath::Vector3 L = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceLeft);
				const SimpleMath::Vector3 U = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceUp);
				const SimpleMath::Vector3 P = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstancePosition);
				const bool orthonormal = std::abs(F.Length() - 1.f) < 1e-3f && std::abs(L.Length() - 1.f) < 1e-3f
					&& std::abs(U.Length() - 1.f) < 1e-3f && std::abs(F.Dot(L)) < 1e-3f && std::abs(F.Dot(U)) < 1e-3f
					&& std::abs(L.Dot(U)) < 1e-3f && std::isfinite(scale) && scale > 0.f;
				if (!orthonormal) { ++badBasis; continue; }
				auto toWorld = [&](const SimpleMath::Vector3& v) { return P + scale * (v.x * F + v.y * L + v.z * U); };

				// Guard: every vertex must sit inside the placement's own bounding sphere (all 2,123 a50 placements do).
				bool inside = true;
				const float limit = sphereRadius * 1.05f + 0.05f;
				for (uint32_t v : mesh->usedVertices)
					if ((toWorld(mesh->localVertices[v]) - sphereCentre).Length() > limit) { inside = false; break; }
				if (!inside) { ++outsideSphere; continue; }

				size_t vertexTotal = mesh->usedVertices.size();
				for (const auto& face : mesh->faces) vertexTotal += face.size();
				if (vertexTotal > kMaxBatchVertices) { ++tooLarge; continue; }

				PlacementBatch batch;
				batch.vertices.reserve(vertexTotal);
				for (const auto& face : mesh->faces)
				{
					const uint16_t firstVertex = (uint16_t)batch.vertices.size();
					ringPoints.clear();
					for (uint32_t v : face)
					{
						const SimpleMath::Vector3 w = toWorld(mesh->localVertices[v]);
						ringPoints.push_back(w);
						batch.vertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(w.x, w.y, w.z)));
					}
					triangulateRing(ringPoints, firstVertex, batch.triangles);
				}
				// Edge endpoints: one vertex per used collision vertex, after the face copies.
				std::unordered_map<uint32_t, uint16_t> edgeVertex;
				for (uint32_t v : mesh->usedVertices)
				{
					edgeVertex[v] = (uint16_t)batch.vertices.size();
					const SimpleMath::Vector3 w = toWorld(mesh->localVertices[v]);
					batch.vertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(w.x, w.y, w.z)));
				}
				for (const auto& e : mesh->edges)
				{
					batch.edges.push_back(edgeVertex[e.first]);
					batch.edges.push_back(edgeVertex[e.second]);
				}

				SimpleMath::Vector3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
				for (const auto& v : batch.vertices)
				{
					lo.x = std::min(lo.x, v.position.x); hi.x = std::max(hi.x, v.position.x);
					lo.y = std::min(lo.y, v.position.y); hi.y = std::max(hi.y, v.position.y);
					lo.z = std::min(lo.z, v.position.z); hi.z = std::max(hi.z, v.position.z);
				}
				batch.center = (lo + hi) * 0.5f;
				batch.radius = (hi - batch.center).Length();

				drawnSurfaces += mesh->faces.size();
				if (cls[k] == DefinitionClass::Invisible) ++invisiblePlacements; else ++fallbackPlacements;
				if (invisibleLines.size() < 64)
					invisibleLines.push_back(std::format("bsp {} inst {} def {} [{}] '{}': {} faces, instFlags 0x{:X}, "
						"physics {}, parts {}, sphere ({:.2f},{:.2f},{:.2f}) r {:.2f}, AABB ({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f})",
						b, i, k, className(cls[k]), stringIdText(readLocal<uint32_t>(instances, base + (size_t)mInstanceName)),
						mesh->faces.size(), instanceFlags, readLocal<int32_t>(instances, base + (size_t)mInstancePhysicsBlock),
						parts[k], sphereCentre.x, sphereCentre.y, sphereCentre.z, sphereRadius, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z));
				snapshot->batches.push_back(std::move(batch));
			}

			totalInvisiblePlacements += invisiblePlacements;
			totalFallbackPlacements += fallbackPlacements;
			totalDrawnSurfaces += drawnSurfaces;

			std::string histogram;
			static const char* const kFlagNames[9] = { "two-sided", "INVISIBLE", "climbable", "breakable", "invalid",
				"conveyor", "slip", "plane-negated", "pathfinding-only" };
			for (int bit = 0; bit < 9; ++bit) if (flagHistogram[bit]) histogram += std::format("{}={} ", kFlagNames[bit], flagHistogram[bit]);
			PLOG_INFO << std::format("HCEInvisibleGeometry bsp {}: {} placements / {} definitions / {} render meshes | "
				"invisible defs {} (partial {}), navmesh defs {} ({} surfaces), unflagged zero-part defs {} (fallback {}) | "
				"cross-tab invisible&zeroParts {} invisible&hasParts {} | drawn: {} invisible + {} fallback placements, "
				"{} faces | rejects: renderOnly {} badDef {} badBasis {} outsideSphere {} tooLarge {} ringFail {} vertexRange {} | "
				"surfaces scanned {} flags: {}",
				b, instanceCount, definitionCount, meshCount, invisibleDefs, partialDefs, navmeshDefs, navmeshSurfaces,
				unflaggedZeroParts, fallbackAllowed ? "on" : "OFF (render data looks stripped)", invisibleAndZero, invisibleWithParts,
				invisiblePlacements, fallbackPlacements, drawnSurfaces, renderOnlySkipped, badDefinitionIndex, badBasis,
				outsideSphere, tooLarge, ringFailures, vertexOutOfRange, surfacesScanned, histogram.empty() ? "(none)" : histogram);
		}

		PLOG_INFO << std::format("HCEInvisibleGeometry rebuilt: {} of {} bsps loaded, {} invisible + {} fallback placements, "
			"{} faces, {} batches", bspsLoaded, bspCount, totalInvisiblePlacements, totalFallbackPlacements, totalDrawnSurfaces,
			snapshot->batches.size());
		for (const std::string& line : invisibleLines) PLOG_INFO << "HCEInvisibleGeometry   " << line;
		if (totalInvisiblePlacements + totalFallbackPlacements > invisibleLines.size())
			PLOG_INFO << std::format("HCEInvisibleGeometry   ... and {} more",
				totalInvisiblePlacements + totalFallbackPlacements - invisibleLines.size());

		// The 12 placements nearest the camera, of ANY class - catches an unflagged blocker by its position.
		std::sort(nearest.begin(), nearest.end(), [](const NearEntry& a, const NearEntry& c) { return a.distance < c.distance; });
		if (nearest.size() > 12) nearest.resize(12);
		PLOG_INFO << std::format("HCEInvisibleGeometry nearest placements to the camera ({:.2f},{:.2f},{:.2f}):", camera.x, camera.y, camera.z);
		for (const NearEntry& n : nearest)
		{
			const std::string name = stringIdText(n.name);
			std::string lower = name;
			for (char& c : lower) c = (char)std::tolower((unsigned char)c);
			const bool collisionNamed = lower.find("collision") != std::string::npos || lower.find("blocker") != std::string::npos;
			PLOG_INFO << std::format("HCEInvisibleGeometry   {:7.2f} wu  bsp {} inst {} def {} [{}{}] '{}' surfaces {} parts {} "
				"instFlags 0x{:X} physics {} centre ({:.2f},{:.2f},{:.2f}) r {:.2f}", n.distance, n.bsp, n.instance, n.definition,
				className(n.cls), (collisionNamed && n.cls == DefinitionClass::Visible) ? " COLLISION-NAMED" : "", name,
				n.surfaces, n.parts, n.flags, n.physics, n.centre.x, n.centre.y, n.centre.z, n.radius);
		}
		return snapshot;
	}

	void publish(std::shared_ptr<const Snapshot> snapshot)
	{
		std::scoped_lock lock(mSnapshotMutex);
		mPrevSnapshot = std::move(mSnapshot);
		mSnapshot = std::move(snapshot);
	}

	void workerLoop(std::stop_token st)
	{
		mBuiltKey = 0;
		while (!st.stop_requested())
		{
			{
				std::unique_lock<std::mutex> lk(mWorkerWaitMutex);
				mWorkerCv.wait_for(lk, st, std::chrono::milliseconds(500), [this] { return mRebuildRequested.load(); });
			}
			if (st.stop_requested()) break;
			const bool forced = mRebuildRequested.exchange(false);
			try
			{
				if (!mAnchorsGood.load() || !mIsActive.load()) continue;
				const uint64_t key = computeKey();
				if (!forced && key == mBuiltKey) continue;
				if (key == 0) { publish(nullptr); mBuiltKey = 0; continue; }

				auto snapshot = buildSnapshot(st);
				if (!snapshot) break;                          // stop requested mid-build
				if (computeKey() != key) { mRebuildRequested = true; continue; }   // torn by a zone switch; redo
				const size_t batches = snapshot->batches.size();
				publish(std::move(snapshot));
				mBuiltKey = key;

				if (mAnnounceNextBuild.exchange(false))
					if (auto messagesGUI = messagesGUIWeak.lock())
						messagesGUI->addMessage(batches == 0
							? std::string("Invisible Geometry Overlay on: no invisible instanced geometry in the loaded area "
								"(the log has a per-BSP census).")
							: std::format("Invisible Geometry Overlay on: {} invisible placements in the loaded area.", batches));
			}
			catch (...)
			{
				// A throw out of a thread terminates the game. Transient failures (level load, zone switch) are normal.
				LOG_ONCE(PLOG_ERROR << "HCEInvisibleGeometryOverlay rebuild threw; it will retry on the next change");
			}
		}
	}

	void startWorker()
	{
		std::scoped_lock lock(mWorkerStartMutex);
		mRebuildRequested = true;
		if (!mWorker.joinable())
			mWorker = std::jthread([this](std::stop_token st) { workerLoop(st); });
		mWorkerCv.notify_all();
	}

	void stopWorker()
	{
		std::scoped_lock lock(mWorkerStartMutex);
		if (mWorker.joinable())
		{
			mWorker.request_stop();
			mWorkerCv.notify_all();
			mWorker.join();
			mWorker = std::jthread();
		}
		publish(nullptr);
		std::scoped_lock snapLock(mSnapshotMutex);
		mPrevSnapshot.reset();
	}

	// ------------------------------------------------------------------------------------------------------------
	void onRender3DEvent(GameState game, IRenderer3D* renderer)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive.load()) return;
		if (GlobalKill::isKillSet()) return;
		if (!renderer) return;
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER) return;
		if (mRender3DDraining.load(std::memory_order_acquire)) return;
		std::shared_lock<std::shared_timed_mutex> renderLock(mRender3DGuard, std::try_to_lock);
		if (!renderLock.owns_lock()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);

			const SimpleMath::Vector3 cameraPosition = renderer->getCameraPosition();
			mCamX.store(cameraPosition.x); mCamY.store(cameraPosition.y); mCamZ.store(cameraPosition.z);

			{
				std::unique_lock<std::mutex> lk(mSnapshotMutex, std::try_to_lock);
				if (lk.owns_lock()) mRenderSnapshot = mSnapshot;   // else keep last frame's
			}
			if (!mRenderSnapshot || mRenderSnapshot->batches.empty()) return;

			const SettingsEnums::TriggerRenderStyle style = settings->hceInvisibleGeometryOverlayRenderStyle->GetValue();
			if (style == SettingsEnums::TriggerRenderStyle::None) return;
			const bool wantSolid = style == SettingsEnums::TriggerRenderStyle::Solid || style == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;
			const bool wantWire = style == SettingsEnums::TriggerRenderStyle::Wireframe || style == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;

			const float renderDistance = settings->hceInvisibleGeometryOverlayRenderDistance->GetValue();
			SimpleMath::Vector4 fillColour = settings->hceInvisibleGeometryOverlayColor->GetValue();
			fillColour.w = std::clamp(fillColour.w * settings->hceInvisibleGeometryOverlayAlpha->GetValue(), 0.f, 1.f);
			SimpleMath::Vector4 wireColour = settings->hceInvisibleGeometryOverlayWireframeColor->GetValue();
			wireColour.w = std::clamp(wireColour.w * settings->hceInvisibleGeometryOverlayWireframeAlpha->GetValue(), 0.f, 1.f);

			const DirectX::BoundingFrustum& frustum = renderer->getCameraFrustum();
			mVisibleSorted.clear();
			for (const PlacementBatch& batch : mRenderSnapshot->batches)
			{
				const float distance = (batch.center - cameraPosition).Length() - batch.radius;
				if (distance > renderDistance) continue;
				if (!frustum.Intersects(DirectX::BoundingSphere(batch.center, batch.radius))) continue;
				mVisibleSorted.push_back({ distance, &batch });
			}
			std::sort(mVisibleSorted.begin(), mVisibleSorted.end(),
				[](const auto& a, const auto& c) { return a.first < c.first; });

			// Nearest first, within the shared upload ring's budget (each draw re-uploads its vertices).
			const size_t drawsPerBatch = (wantSolid ? 1 : 0) + (wantWire ? 1 : 0);
			size_t budget = kFrameVertexBudget;
			mVisible.clear();
			for (const auto& entry : mVisibleSorted)
			{
				const size_t cost = entry.second->vertices.size() * drawsPerBatch;
				if (cost > budget) break;
				budget -= cost;
				mVisible.push_back(entry.second);
			}

			renderer->setSurfacePattern(0.f, 0.f);   // sticky renderer state - never inherit a pattern
			// All fills before any wireframe, so a later fill cannot bury an earlier batch's lines.
			if (wantSolid)
				for (const PlacementBatch* batch : mVisible)
				{
					if (batch->triangles.empty()) continue;
					const PlacementModel model(*batch);
					renderer->drawTriangleCollection(&model, fillColour, CullingOption::CullNone, std::nullopt);
				}
			if (wantWire)
				for (const PlacementBatch* batch : mVisible)
				{
					if (batch->edges.empty()) continue;
					const PlacementModel model(*batch);
					renderer->drawEdgeCollection(&model, wireColour);
				}
		}
		catch (HCMRuntimeException) {}
		catch (...)
		{
			LOG_ONCE(PLOG_ERROR << "HCEInvisibleGeometryOverlay's 3D render path threw an unknown exception; suppressing further reports");
		}
	}

	static constexpr std::chrono::milliseconds kRender3DDrainTimeout{ 1000 };

	bool dropRender3DSubscription(bool mustSucceed)
	{
		mRender3DDraining.store(true, std::memory_order_release);
		for (;;)
		{
			{
				std::unique_lock<std::shared_timed_mutex> drain(mRender3DGuard, kRender3DDrainTimeout);
				if (drain.owns_lock())
				{
					mRender3DEventCallback.reset();
					mRenderSnapshot.reset();   // the render callback is gone; nothing else touches this
					break;
				}
			}
			if (!mustSucceed)
			{
				mRender3DDraining.store(false, std::memory_order_release);
				PLOG_ERROR << "HCEInvisibleGeometryOverlay: timed out draining the 3D render path; keeping the subscription";
				return false;
			}
			PLOG_ERROR << "HCEInvisibleGeometryOverlay: still waiting for the 3D render path to finish before teardown";
		}
		mRender3DDraining.store(false, std::memory_order_release);
		return true;
	}

	void set3DRenderingEnabled(bool enabled, bool forTeardown = false)
	{
		std::scoped_lock subscriptionLock(mRender3DSubscriptionMutex);
		if (!enabled)
		{
			if (mRender3DEventCallback) dropRender3DSubscription(forTeardown);
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
				stopWorker();
				detachCameraHook();
				messagesGUI->addMessage("Disabling Invisible Geometry Overlay.");
				return;
			}

			lockOrThrow(playerStateWeak, playerState);
			attachCameraHook();
			set3DRenderingEnabled(true);
			if (!mAnchorsTried || !mAnchorsGood) resolveAnchors(playerState->getSimModuleBase());
			if (!mAnchorsGood)
			{
				mIsActive = false;
				throw HCMRuntimeException(std::format(
					"Invisible Geometry Overlay can't run on this game build: could not locate the {} by byte signature. "
					"This usually means Halo Campaign Evolved updated (it reports no version number, so HCM refuses rather "
					"than read geometry from the wrong address).", mAnchorFailure));
			}
			if (!mRender3DEventCallback)
			{
				mIsActive = false;
				throw HCMRuntimeException("Invisible Geometry Overlay needs HCM's 3D renderer, which is not available "
					"(the D3D12 hook has not initialised). Try toggling it again once you are in-game.");
			}
			mIsActive = true;
			mAnnounceNextBuild = true;
			startWorker();
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
			const bool want = settings->hceInvisibleGeometryOverlayToggle->GetValue()
				&& newState.currentGameState == mGame && newState.currentPlayState == PlayState::Ingame;

			if (want && !mAnchorsGood)
				if (auto playerState = playerStateWeak.lock())
				{
					try { resolveAnchors(playerState->getSimModuleBase()); }
					catch (HCMRuntimeException) {}
				}

			try
			{
				if (want) attachCameraHook();
				else detachCameraHook();
			}
			catch (HCMRuntimeException) {}
			if (want && mCameraDataOptionalWeak.has_value())
				if (auto cameraData = mCameraDataOptionalWeak.value().lock())
					cameraData->ensureHookLive();

			set3DRenderingEnabled(want);
			mIsActive = want && mAnchorsGood;
			if (mIsActive) { publish(nullptr); startWorker(); }   // a level change invalidates the baked geometry
			else stopWorker();
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - the callbacks must be destroyed before anything they touch.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	HCEInvisibleGeometryOverlayImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceInvisibleGeometryOverlayToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEInvisibleGeometryOverlay only supports Halo Campaign Evolved");

		// Pure PointerDataStore lookups - nothing touches game memory here.
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
#define hceOffset(member, name) member = *ptr->getData<std::shared_ptr<int64_t>>(nameof(name), mGame)
		hceOffset(mScenarioStructureBspBlock, hceScenarioStructureBspBlock);
		hceOffset(mStructureBspReferenceStride, hceStructureBspReferenceStride);
		hceOffset(mStructureBspTagIndexOffset, hceStructureBspTagIndexOffset);
		hceOffset(mStructureBspLocalTagIndexOffset, hceStructureBspLocalTagIndexOffset);
		hceOffset(mTagInstanceTableOffset, hceTagInstanceTableOffset);
		hceOffset(mTagInstanceStride, hceTagInstanceStride);
		hceOffset(mTagInstanceDataOffset, hceTagInstanceDataOffset);
		hceOffset(mStructureBspResourceBlock, hceStructureBspResourceBlock);
		hceOffset(mCollisionSurfacesBlock, hceCollisionSurfacesBlock);
		hceOffset(mCollisionEdgesBlock, hceCollisionEdgesBlock);
		hceOffset(mCollisionVerticesBlock, hceCollisionVerticesBlock);
		hceOffset(mInstancesBlock, hceStructureBspInstancedGeometryInstancesBlock);
		hceOffset(mInstanceStride, hceInstancedGeometryInstanceStride);
		hceOffset(mInstanceScale, hceInstancedGeometryInstanceScaleOffset);
		hceOffset(mInstanceForward, hceInstancedGeometryInstanceForwardOffset);
		hceOffset(mInstanceLeft, hceInstancedGeometryInstanceLeftOffset);
		hceOffset(mInstanceUp, hceInstancedGeometryInstanceUpOffset);
		hceOffset(mInstancePosition, hceInstancedGeometryInstancePositionOffset);
		hceOffset(mInstanceDefinitionIndex, hceInstancedGeometryInstanceDefinitionIndexOffset);
		hceOffset(mInstanceFlags, hceInstancedGeometryInstanceFlagsOffset);
		hceOffset(mInstanceSphereCenter, hceInstancedGeometryInstanceSphereCenterOffset);
		hceOffset(mInstanceSphereRadius, hceInstancedGeometryInstanceSphereRadiusOffset);
		hceOffset(mInstanceName, hceInstancedGeometryInstanceNameOffset);
		hceOffset(mInstancePhysicsBlock, hceInstancedGeometryInstancePhysicsBlock);
		hceOffset(mUseResourceItems, hceStructureBspUseResourceItemsOffset);
		hceOffset(mResourceInstancedGeometryBlock, hceResourceInstancedGeometryBlock);
		hceOffset(mDefinitionStride, hceInstancedGeometryDefinitionStride);
		hceOffset(mDefinitionCollisionBsp, hceInstancedGeometryDefinitionCollisionBsp);
		hceOffset(mDefinitionMeshIndex, hceInstancedGeometryDefinitionMeshIndexOffset);
		hceOffset(mMeshesBlock, hceStructureBspRenderGeometryMeshesBlock);
		hceOffset(mMeshStride, hceRenderGeometryMeshStride);
		hceOffset(mMeshPartsBlock, hceRenderGeometryMeshPartsBlock);
#undef hceOffset

		try { mRender3DProviderOptionalWeak = resolveDependentCheat(Render3DEventProvider); }
		catch (HCMInitException) { PLOG_ERROR << "HCEInvisibleGeometryOverlay could not resolve Render3DEventProvider"; }
		try { mCameraDataOptionalWeak = resolveDependentCheat(HCEGetCameraData); }
		catch (HCMInitException) { PLOG_ERROR << "HCEInvisibleGeometryOverlay could not resolve HCEGetCameraData"; }

		mReady.store(true, std::memory_order_release);
	}

	~HCEInvisibleGeometryOverlayImpl()
	{
		mReady.store(false, std::memory_order_release);
		mIsActive = false;
		set3DRenderingEnabled(false, true);   // must drain: a live callback captures `this`
		stopWorker();
		detachCameraHook();
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCEInvisibleGeometryOverlay::HCEInvisibleGeometryOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEInvisibleGeometryOverlayImpl>(game, dicon))
{
}

HCEInvisibleGeometryOverlay::~HCEInvisibleGeometryOverlay()
{
	PLOG_VERBOSE << "~" << getName();
}
