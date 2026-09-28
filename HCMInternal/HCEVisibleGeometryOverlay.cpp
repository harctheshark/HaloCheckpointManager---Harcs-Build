#include "pch.h"
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <set>
#include <unordered_map>
#include "HCEVisibleGeometryOverlay.h"
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

// See HCEVisibleGeometryOverlay.h for what this draws and how the budget works, and HCEInvisibleGeometryOverlay.h
// for the tag layout both overlays read. Addresses are byte-signature anchored exactly as the invisible overlay
// does it; only STRUCTURAL tag constants come from InternalPointerData.xml (the same entries, shared).
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

	// The SMALL collision bsp layout (instanced geometry never uses the large one). Surface 14 B, edge 12 B, vertex 16 B.
	constexpr size_t kSurfaceStride = 14, kEdgeStride = 12, kVertexStride = 16;
	constexpr size_t kSurfaceFirstEdge = 0x02, kSurfaceFlags = 0x0A;
	constexpr size_t kEdgeStart = 0x00, kEdgeEnd = 0x02, kEdgeForward = 0x04, kEdgeReverse = 0x06, kEdgeRight = 0x0A;
	constexpr uint32_t kSentinel = 0xFFFFu;

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
	// Per-frame upload budget in vertices, counted over every draw (each re-uploads its vertices) - the shared upload
	// ring drops a draw that does not fit, silently. 1.2M = 14.4 MB leaves room for the hidden-line pre-pass plus
	// fill plus wire at the 400k-triangle budget, and for the other overlays, under the ring's 32 MB per-buffer cap.
	constexpr size_t kFrameVertexBudget = 1200000;
	// Selected placements are merged into one batch per cube of this edge (world units): a frame issues a few dozen
	// draws instead of one per rock, and the render thread can still frustum-cull by batch.
	constexpr float kBatchCellSize = 32.f;

	struct DrawBatch
	{
		VertexCollection vertices;   // world space; shared by the triangles and the edges
		IndexCollection triangles;
		IndexCollection edges;
		SimpleMath::Vector3 center{};
		float radius = 0.f;
	};

	struct Snapshot
	{
		std::vector<DrawBatch> batches;
	};

	class BatchModel : public IModelTriangles, public IModelEdges
	{
	public:
		explicit BatchModel(const DrawBatch& b) : mBatch(b) {}
		const VertexCollection& getTriangleVertices() const override { return mBatch.vertices; }
		const IndexCollection& getTriangleIndices() const override { return mBatch.triangles; }
		const VertexCollection& getEdgeVertices() const override { return mBatch.vertices; }
		const IndexCollection& getEdgeIndices() const override { return mBatch.edges; }
	private:
		const DrawBatch& mBatch;
	};

	// Ear-clipping triangulation of one edge ring (the same algorithm as HCEBspOverlay and the invisible overlay -
	// collision rings are N-gons and may be concave, so a fan is wrong). Emits RING POSITIONS, three per triangle.
	void triangulateRing(const std::vector<SimpleMath::Vector3>& points, std::vector<uint32_t>& out)
	{
		const size_t n = points.size();
		if (n < 3) return;
		if (n == 3) { out.push_back(0); out.push_back(1); out.push_back(2); return; }
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
				out.push_back((uint32_t)ia); out.push_back((uint32_t)ib); out.push_back((uint32_t)ic);
				remaining.erase(remaining.begin() + i);
				clipped = true;
				break;
			}
			if (!clipped) break;   // malformed ring: fan the remainder rather than drop the face
		}
		for (size_t i = 1; i + 1 < remaining.size(); ++i)
		{
			out.push_back((uint32_t)remaining[0]);
			out.push_back((uint32_t)remaining[i]);
			out.push_back((uint32_t)remaining[i + 1]);
		}
	}

	// Squared distance from p to triangle abc - the closest-point test from Ericson, Real-Time Collision Detection
	// 5.1.5. Exact, so a big floor triangle directly under the camera counts even when all three corners are far.
	float distanceSquaredToTriangle(const SimpleMath::Vector3& p, const SimpleMath::Vector3& a, const SimpleMath::Vector3& b,
		const SimpleMath::Vector3& c)
	{
		const SimpleMath::Vector3 ab = b - a, ac = c - a, ap = p - a;
		const float d1 = ab.Dot(ap), d2 = ac.Dot(ap);
		if (d1 <= 0.f && d2 <= 0.f) return ap.LengthSquared();
		const SimpleMath::Vector3 bp = p - b;
		const float d3 = ab.Dot(bp), d4 = ac.Dot(bp);
		if (d3 >= 0.f && d4 <= d3) return bp.LengthSquared();
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return (p - (a + (d1 / (d1 - d3)) * ab)).LengthSquared();
		const SimpleMath::Vector3 cp = p - c;
		const float d5 = ab.Dot(cp), d6 = ac.Dot(cp);
		if (d6 >= 0.f && d5 <= d6) return cp.LengthSquared();
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return (p - (a + (d2 / (d2 - d6)) * ac)).LengthSquared();
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
			return (p - (b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b))).LengthSquared();
		const float sum = va + vb + vc;
		if (!(sum > 0.f))   // degenerate triangle: fall back to the nearest corner
			return std::min({ ap.LengthSquared(), bp.LengthSquared(), cp.LengthSquared() });
		return (p - (a + (vb / sum) * ab + (vc / sum) * ac)).LengthSquared();
	}

	float distanceSquaredToSegment(const SimpleMath::Vector3& p, const SimpleMath::Vector3& a, const SimpleMath::Vector3& b)
	{
		const SimpleMath::Vector3 ab = b - a;
		const float lengthSquared = ab.LengthSquared();
		const float t = lengthSquared > 0.f ? std::clamp((p - a).Dot(ab) / lengthSquared, 0.f, 1.f) : 0.f;
		return (p - (a + t * ab)).LengthSquared();
	}

	// One visible definition's collision in LOCAL space, compacted to the vertices its faces use.
	struct DefinitionMesh
	{
		std::vector<SimpleMath::Vector3> vertices;
		std::vector<uint32_t> triangles;   // 3 per triangle, into vertices
		std::vector<uint32_t> edges;       // 2 per de-duplicated edge, into vertices
	};

	struct DefinitionEntry
	{
		uintptr_t collision = 0;           // tag address of the definition's inline collision bsp
		bool visible = false;
		bool decodeTried = false;          // decoded lazily, the first time one of its placements is in range
		std::unique_ptr<DefinitionMesh> mesh;
	};

	// One SOLID placement of a VISIBLE definition, copied out of the tag once per level.
	struct PlacementEntry
	{
		SimpleMath::Vector3 sphereCentre{};
		float sphereRadius = 0.f;
		SimpleMath::Vector3 position{}, forward{}, left{}, up{};
		float scale = 1.f;
		uint32_t definition = 0;           // into LevelCache::definitions
		bool rejected = false;             // failed the in-sphere guard; never selected again
	};

	struct LevelCache
	{
		uint64_t key = 0;
		std::vector<DefinitionEntry> definitions;   // every loaded bsp's, concatenated
		std::vector<PlacementEntry> placements;
		size_t decoded = 0, decodedEmpty = 0, decodedTooLarge = 0, outsideSphere = 0;
	};

	struct SelectionStats
	{
		size_t candidates = 0, placements = 0, triangles = 0, overWholeBudget = 0, trianglesOutsideRadius = 0;
		bool budgetHit = false;
	};
}


class HCEVisibleGeometryOverlay::HCEVisibleGeometryOverlayImpl
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

	// Structural tag constants from InternalPointerData.xml - the invisible overlay's entries, shared.
	int64_t mScenarioStructureBspBlock = 0, mStructureBspReferenceStride = 0, mStructureBspTagIndexOffset = 0,
		mStructureBspLocalTagIndexOffset = 0, mTagInstanceTableOffset = 0, mTagInstanceStride = 0, mTagInstanceDataOffset = 0,
		mStructureBspResourceBlock = 0, mCollisionSurfacesBlock = 0, mCollisionEdgesBlock = 0, mCollisionVerticesBlock = 0;
	int64_t mInstancesBlock = 0, mInstanceStride = 0, mInstanceScale = 0, mInstanceForward = 0, mInstanceLeft = 0,
		mInstanceUp = 0, mInstancePosition = 0, mInstanceDefinitionIndex = 0, mInstanceFlags = 0, mInstanceSphereCenter = 0,
		mInstanceSphereRadius = 0, mUseResourceItems = 0, mResourceInstancedGeometryBlock = 0, mDefinitionStride = 0,
		mDefinitionCollisionBsp = 0, mDefinitionMeshIndex = 0, mMeshesBlock = 0, mMeshStride = 0, mMeshPartsBlock = 0;

	// Byte-signature anchored.
	uintptr_t mScenarioSlot = 0, mTagAddressTable = 0, mTagGlobalsSlot = 0;
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
	std::vector<std::pair<float, const DrawBatch*>> mVisibleSorted;   // render thread scratch

	// Written by the render thread every frame, read by the worker: the camera and the budget settings, so the
	// worker never touches the settings objects.
	std::atomic<float> mCamX{ 0.f }, mCamY{ 0.f }, mCamZ{ 0.f };
	std::atomic_bool mCameraKnown{ false };
	std::atomic<float> mRadius{ 25.f }, mTriangleBudget{ 40000.f }, mRefreshMs{ 250.f };

	// ---- worker ----
	std::mutex mWorkerStartMutex;
	std::mutex mWorkerWaitMutex;
	std::condition_variable_any mWorkerCv;
	std::atomic_bool mRebuildRequested{ false };
	std::atomic_bool mAnnounceNextBuild{ false };
	std::jthread mWorker;

	// ------------------------------------------------------------------------------------------------------------
	void attachCameraHook()
	{
		if (!mCameraDataOptionalWeak.has_value())
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the Visible "
				"Geometry overlay cannot read the render camera");
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData)
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the Visible "
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
		mAnchorsGood = true;
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

	// Rebuild key: every loaded bsp's tag address plus its resource and instance block addresses (see the
	// invisible overlay - a zone switch changes the set of resolved bsps, so this changes with it).
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
	// PER-LEVEL CACHE (worker thread): classify every definition, copy out every solid placement of a visible one.
	// Nothing is decoded here - that happens per definition, the first time it comes within range.
	// ------------------------------------------------------------------------------------------------------------
	std::unique_ptr<LevelCache> buildCache(std::stop_token st, uint64_t key)
	{
		auto cache = std::make_unique<LevelCache>();
		cache->key = key;
		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return cache;
		const int32_t bspCount = readI32(scenario + mScenarioStructureBspBlock);
		if (bspCount <= 0 || bspCount > kMaxBsps) return cache;

		std::vector<uint8_t> definitions, instances, meshes, surfaces;
		int bspsLoaded = 0;
		size_t totalVisibleDefinitions = 0;

		for (int b = 0; b < bspCount; ++b)
		{
			if (st.stop_requested()) return nullptr;
			const uintptr_t sbsp = resolveStructureBsp(scenario, b);
			if (!plausiblePointer(sbsp)) { PLOG_INFO << std::format("HCEVisibleGeometry bsp {}: not loaded", b); continue; }
			++bspsLoaded;

			const int32_t useResourceItems = readI32(sbsp + mUseResourceItems);
			const int32_t rawCount = readI32(sbsp + mStructureBspResourceBlock);
			if (useResourceItems != 0 || rawCount < 1)
			{
				PLOG_INFO << std::format("HCEVisibleGeometry bsp {}: resource path not supported (use resource items {}, "
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
				PLOG_INFO << std::format("HCEVisibleGeometry bsp {}: {} definitions, {} placements - nothing instanced",
					b, definitionCount, instanceCount);
				continue;
			}
			const uintptr_t definitionsBase = resolveTagBlock(readU32(resource + mResourceInstancedGeometryBlock + 4));
			if (!plausiblePointer(definitionsBase)) continue;

			// ---- classify, exactly as the invisible overlay does, so every solid definition lands in ONE of the two ----
			enum class Cls : uint8_t { Visible, Invisible, NoRenderMesh, Navmesh, Empty };
			std::vector<Cls> cls((size_t)definitionCount, Cls::Empty);
			size_t unflaggedZeroParts = 0;
			for (int32_t k = 0; k < definitionCount; ++k)
			{
				const int16_t meshIndex = readLocal<int16_t>(definitions, (size_t)k * (size_t)mDefinitionStride + (size_t)mDefinitionMeshIndex);
				const int32_t parts = (meshIndex >= 0 && meshIndex < meshCount)
					? readLocal<int32_t>(meshes, (size_t)meshIndex * (size_t)mMeshStride + (size_t)mMeshPartsBlock) : -1;
				int32_t surfaceCount = 0;
				copyBlock(definitionsBase + (uintptr_t)mDefinitionStride * k + mDefinitionCollisionBsp + mCollisionSurfacesBlock,
					kSurfaceStride, kMaxSurfacesPerDefinition, surfaces, surfaceCount);
				size_t invisible = 0, pathfinding = 0, valid = 0;
				for (int32_t s = 0; s < surfaceCount; ++s)
				{
					const uint16_t f = readLocal<uint16_t>(surfaces, (size_t)s * kSurfaceStride + kSurfaceFlags);
					if (f & kSurfaceInvalid) continue;
					++valid;
					if (f & kSurfaceInvisible) ++invisible;
					if (f & kSurfacePathfindingOnly) ++pathfinding;
				}
				if (valid == 0) cls[k] = Cls::Empty;
				else if (pathfinding == valid) cls[k] = Cls::Navmesh;
				else if (invisible > 0) cls[k] = Cls::Invisible;
				else if (parts <= 0) { cls[k] = Cls::NoRenderMesh; ++unflaggedZeroParts; }
				else cls[k] = Cls::Visible;
			}
			// The invisible overlay draws the zero-part definitions only where this holds; where it does not (render
			// data looks stripped), they are drawn HERE instead, so none falls between the two overlays.
			const bool fallbackAllowed = meshCount > 0 && unflaggedZeroParts * 2 <= (size_t)definitionCount;

			const size_t firstDefinition = cache->definitions.size();
			size_t visibleDefinitions = 0, invisibleDefinitions = 0, navmeshDefinitions = 0, emptyDefinitions = 0,
				zeroPartsKept = 0;
			for (int32_t k = 0; k < definitionCount; ++k)
			{
				DefinitionEntry entry;
				entry.collision = definitionsBase + (uintptr_t)mDefinitionStride * k + mDefinitionCollisionBsp;
				entry.visible = cls[k] == Cls::Visible || (cls[k] == Cls::NoRenderMesh && !fallbackAllowed);
				if (entry.visible) ++visibleDefinitions;
				if (cls[k] == Cls::NoRenderMesh && !fallbackAllowed) ++zeroPartsKept;
				if (cls[k] == Cls::Invisible || (cls[k] == Cls::NoRenderMesh && fallbackAllowed)) ++invisibleDefinitions;
				if (cls[k] == Cls::Navmesh) ++navmeshDefinitions;
				if (cls[k] == Cls::Empty) ++emptyDefinitions;
				cache->definitions.push_back(std::move(entry));
			}
			totalVisibleDefinitions += visibleDefinitions;

			size_t solid = 0, notVisible = 0, renderOnly = 0, badDefinitionIndex = 0, badBasis = 0;
			for (int32_t i = 0; i < instanceCount; ++i)
			{
				const size_t base = (size_t)i * (size_t)mInstanceStride;
				const int16_t k = readLocal<int16_t>(instances, base + (size_t)mInstanceDefinitionIndex);
				if (k < 0 || k >= definitionCount) { ++badDefinitionIndex; continue; }
				if (!cache->definitions[firstDefinition + k].visible) { ++notVisible; continue; }
				if (readLocal<uint16_t>(instances, base + (size_t)mInstanceFlags) & kInstanceRenderOnly) { ++renderOnly; continue; }   // not solid

				PlacementEntry p;
				p.scale = readLocal<float>(instances, base + (size_t)mInstanceScale);
				p.forward = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceForward);
				p.left = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceLeft);
				p.up = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceUp);
				p.position = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstancePosition);
				p.sphereCentre = readLocal<SimpleMath::Vector3>(instances, base + (size_t)mInstanceSphereCenter);
				p.sphereRadius = readLocal<float>(instances, base + (size_t)mInstanceSphereRadius);
				const bool orthonormal = std::abs(p.forward.Length() - 1.f) < 1e-3f && std::abs(p.left.Length() - 1.f) < 1e-3f
					&& std::abs(p.up.Length() - 1.f) < 1e-3f && std::abs(p.forward.Dot(p.left)) < 1e-3f
					&& std::abs(p.forward.Dot(p.up)) < 1e-3f && std::abs(p.left.Dot(p.up)) < 1e-3f
					&& std::isfinite(p.scale) && p.scale > 0.f && std::isfinite(p.sphereRadius) && p.sphereRadius >= 0.f;
				if (!orthonormal) { ++badBasis; continue; }
				p.definition = (uint32_t)(firstDefinition + k);
				cache->placements.push_back(p);
				++solid;
			}

			PLOG_INFO << std::format("HCEVisibleGeometry bsp {}: {} placements / {} definitions / {} render meshes | "
				"visible defs {} (incl. {} zero-part defs kept because the invisible overlay's fallback is off here), "
				"invisible defs {}, navmesh defs {}, defs without valid surfaces {} | solid visible placements {} | "
				"skipped: not visible {}, renderOnly {}, badDef {}, badBasis {}",
				b, instanceCount, definitionCount, meshCount, visibleDefinitions, zeroPartsKept, invisibleDefinitions,
				navmeshDefinitions, emptyDefinitions, solid, notVisible, renderOnly, badDefinitionIndex, badBasis);
		}

		PLOG_INFO << std::format("HCEVisibleGeometry cache rebuilt: {} of {} bsps loaded, {} visible definitions, "
			"{} solid visible placements", bspsLoaded, bspCount, totalVisibleDefinitions, cache->placements.size());
		return cache;
	}

	// A definition's drawable surfaces, de-duplicated and triangulated once, in local space.
	void decodeDefinition(LevelCache& cache, DefinitionEntry& entry) const
	{
		entry.decodeTried = true;
		++cache.decoded;
		std::vector<uint8_t> surfaces, edges, vertices;
		int32_t surfaceCount = 0, edgeCount = 0, vertexCount = 0;
		if (!copyBlock(entry.collision + mCollisionSurfacesBlock, kSurfaceStride, kMaxSurfacesPerDefinition, surfaces, surfaceCount)
			|| !copyBlock(entry.collision + mCollisionEdgesBlock, kEdgeStride, 1 << 20, edges, edgeCount)
			|| !copyBlock(entry.collision + mCollisionVerticesBlock, kVertexStride, 1 << 20, vertices, vertexCount))
		{
			++cache.decodedEmpty;
			return;
		}

		std::vector<SimpleMath::Vector3> local((size_t)vertexCount);
		for (int32_t v = 0; v < vertexCount; ++v)
		{
			float p[3]{};
			memcpy(p, vertices.data() + (size_t)v * kVertexStride, sizeof(p));
			local[v] = SimpleMath::Vector3(p[0], p[1], p[2]);
		}

		auto mesh = std::make_unique<DefinitionMesh>();
		std::vector<int32_t> remap((size_t)vertexCount, -1);
		auto compact = [&](uint32_t v) -> uint32_t
			{
				if (remap[v] < 0) { remap[v] = (int32_t)mesh->vertices.size(); mesh->vertices.push_back(local[v]); }
				return (uint32_t)remap[v];
			};

		std::set<std::vector<uint32_t>> faceKeys;
		std::set<std::pair<uint32_t, uint32_t>> edgeKeys;
		std::vector<uint32_t> ring, ringTriangles;
		std::vector<SimpleMath::Vector3> ringPoints;
		for (int32_t s = 0; s < surfaceCount; ++s)
		{
			const size_t r = (size_t)s * kSurfaceStride;
			const uint16_t f = readLocal<uint16_t>(surfaces, r + kSurfaceFlags);
			if (f & (kSurfaceInvalid | kSurfaceInvisible | kSurfacePathfindingOnly)) continue;
			if (!walkRing(edges, edgeCount, (uint32_t)s, readLocal<uint16_t>(surfaces, r + kSurfaceFirstEdge), ring)) continue;
			bool inRange = true;
			for (uint32_t v : ring) if ((int64_t)v >= vertexCount) { inRange = false; break; }
			if (!inRange) continue;

			// Two-sided surfaces can be stored as a face plus its plane-negated twin - draw it once.
			std::vector<uint32_t> faceKey = ring;
			std::sort(faceKey.begin(), faceKey.end());
			if (!faceKeys.insert(faceKey).second) continue;

			ringPoints.clear();
			for (uint32_t v : ring) ringPoints.push_back(local[v]);
			ringTriangles.clear();
			triangulateRing(ringPoints, ringTriangles);
			for (uint32_t position : ringTriangles) mesh->triangles.push_back(compact(ring[position]));
			for (size_t i = 0; i < ring.size(); ++i)
			{
				uint32_t a = ring[i], c = ring[(i + 1) % ring.size()];
				if (a > c) std::swap(a, c);
				if (edgeKeys.insert({ a, c }).second) { mesh->edges.push_back(compact(a)); mesh->edges.push_back(compact(c)); }
			}
		}

		if (mesh->triangles.empty() && mesh->edges.empty()) { ++cache.decodedEmpty; return; }
		if (mesh->vertices.size() > kMaxBatchVertices) { ++cache.decodedTooLarge; return; }
		entry.mesh = std::move(mesh);
	}

	// ------------------------------------------------------------------------------------------------------------
	// SELECTION (worker thread): the nearest solid placements within the radius, until the triangle budget.
	// Returns nullptr if interrupted by a stop request.
	// ------------------------------------------------------------------------------------------------------------
	std::shared_ptr<Snapshot> selectAroundCamera(LevelCache& cache, const SimpleMath::Vector3& camera, float radius, size_t budget,
		std::stop_token st, SelectionStats& stats) const
	{
		auto snapshot = std::make_shared<Snapshot>();

		std::vector<std::pair<float, uint32_t>> candidates;
		for (uint32_t i = 0; i < (uint32_t)cache.placements.size(); ++i)
		{
			const PlacementEntry& p = cache.placements[i];
			if (p.rejected) continue;
			const float distance = (p.sphereCentre - camera).Length() - p.sphereRadius;
			if (distance > radius) continue;
			candidates.push_back({ std::max(distance, 0.f), i });
		}
		std::sort(candidates.begin(), candidates.end());
		stats.candidates = candidates.size();

		std::unordered_map<uint64_t, size_t> openBatch;   // cell -> index of the batch still being filled there
		std::vector<SimpleMath::Vector3> world;
		std::vector<uint32_t> keptTriangles, keptEdges, keptVertices;
		std::vector<int32_t> remap;
		const float radiusSquared = radius * radius;
		size_t remaining = budget;
		for (const auto& [distance, index] : candidates)
		{
			if (st.stop_requested()) return nullptr;
			PlacementEntry& p = cache.placements[index];
			DefinitionEntry& definition = cache.definitions[p.definition];
			if (!definition.decodeTried) decodeDefinition(cache, definition);
			const DefinitionMesh* mesh = definition.mesh.get();
			if (!mesh) continue;

			// World transform, with the invisible overlay's guard: every vertex inside the placement's own sphere.
			world.clear();
			const float limit = p.sphereRadius * 1.05f + 0.05f;
			bool inside = true;
			for (const SimpleMath::Vector3& v : mesh->vertices)
			{
				const SimpleMath::Vector3 w = p.position + p.scale * (v.x * p.forward + v.y * p.left + v.z * p.up);
				if ((w - p.sphereCentre).Length() > limit) { inside = false; break; }
				world.push_back(w);
			}
			if (!inside) { p.rejected = true; ++cache.outsideSphere; continue; }

			// The radius cuts pieces, not just selects them: a rock whose sphere touches a 1 wu radius must not be drawn
			// whole out to 10 wu. Whole pieces pass untested only when the entire sphere is inside the radius.
			const bool wholePiece = (p.sphereCentre - camera).Length() + limit <= radius;
			keptTriangles.clear();
			keptEdges.clear();
			if (!wholePiece)
			{
				for (size_t t = 0; t + 2 < mesh->triangles.size(); t += 3)
				{
					const uint32_t a = mesh->triangles[t], b = mesh->triangles[t + 1], c = mesh->triangles[t + 2];
					if (distanceSquaredToTriangle(camera, world[a], world[b], world[c]) > radiusSquared) continue;
					keptTriangles.push_back(a); keptTriangles.push_back(b); keptTriangles.push_back(c);
				}
				for (size_t e = 0; e + 1 < mesh->edges.size(); e += 2)
				{
					const uint32_t a = mesh->edges[e], b = mesh->edges[e + 1];
					if (distanceSquaredToSegment(camera, world[a], world[b]) > radiusSquared) continue;
					keptEdges.push_back(a); keptEdges.push_back(b);
				}
			}
			const std::vector<uint32_t>& tris = wholePiece ? mesh->triangles : keptTriangles;
			const std::vector<uint32_t>& lines = wholePiece ? mesh->edges : keptEdges;
			if (tris.empty() && lines.empty()) continue;   // the sphere touched the radius, the collision does not

			const size_t triangles = tris.size() / 3;
			if (triangles > budget) { ++stats.overWholeBudget; continue; }   // could never fit; do not let it block the rest
			if (triangles > remaining) { stats.budgetHit = true; break; }     // nearest first: stop, draw nothing farther
			stats.trianglesOutsideRadius += mesh->triangles.size() / 3 - triangles;

			// Only the vertices the kept primitives use go into the batch.
			remap.assign(world.size(), -1);
			keptVertices.clear();
			auto keep = [&](uint32_t v) { if (remap[v] < 0) { remap[v] = (int32_t)keptVertices.size(); keptVertices.push_back(v); } };
			for (uint32_t v : tris) keep(v);
			for (uint32_t v : lines) keep(v);

			const int64_t ix = (int64_t)std::floor(p.sphereCentre.x / kBatchCellSize);
			const int64_t iy = (int64_t)std::floor(p.sphereCentre.y / kBatchCellSize);
			const int64_t iz = (int64_t)std::floor(p.sphereCentre.z / kBatchCellSize);
			const uint64_t cell = ((uint64_t)ix & 0x1FFFFFull) | (((uint64_t)iy & 0x1FFFFFull) << 21) | (((uint64_t)iz & 0x1FFFFFull) << 42);
			auto found = openBatch.find(cell);
			if (found == openBatch.end() || snapshot->batches[found->second].vertices.size() + keptVertices.size() > kMaxBatchVertices)
			{
				snapshot->batches.emplace_back();
				openBatch[cell] = snapshot->batches.size() - 1;
				found = openBatch.find(cell);
			}
			DrawBatch& batch = snapshot->batches[found->second];
			const uint16_t first = (uint16_t)batch.vertices.size();
			for (uint32_t v : keptVertices)
				batch.vertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(world[v].x, world[v].y, world[v].z)));
			for (uint32_t v : tris) batch.triangles.push_back((uint16_t)(first + remap[v]));
			for (uint32_t v : lines) batch.edges.push_back((uint16_t)(first + remap[v]));

			remaining -= triangles;
			stats.triangles += triangles;
			++stats.placements;
		}

		for (DrawBatch& batch : snapshot->batches)
		{
			SimpleMath::Vector3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			for (const auto& v : batch.vertices)
			{
				lo.x = std::min(lo.x, v.position.x); hi.x = std::max(hi.x, v.position.x);
				lo.y = std::min(lo.y, v.position.y); hi.y = std::max(hi.y, v.position.y);
				lo.z = std::min(lo.z, v.position.z); hi.z = std::max(hi.z, v.position.z);
			}
			batch.center = (lo + hi) * 0.5f;
			batch.radius = (hi - batch.center).Length();
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
		std::unique_ptr<LevelCache> cache;   // worker only; freed here, never on the render thread
		bool haveSelection = false;
		SimpleMath::Vector3 selectedCamera{};
		float selectedRadius = -1.f;
		size_t selectedBudget = 0;
		bool logNextSelection = true, lastBudgetHit = false;

		while (!st.stop_requested())
		{
			{
				const auto interval = std::chrono::milliseconds((int)std::clamp(mRefreshMs.load(), 16.f, 5000.f));
				std::unique_lock<std::mutex> lk(mWorkerWaitMutex);
				mWorkerCv.wait_for(lk, st, interval, [this] { return mRebuildRequested.load(); });
			}
			if (st.stop_requested()) break;
			const bool forced = mRebuildRequested.exchange(false);
			try
			{
				if (!mAnchorsGood.load() || !mIsActive.load()) continue;
				const uint64_t key = computeKey();
				if (key == 0) { publish(nullptr); cache.reset(); haveSelection = false; continue; }

				if (forced || !cache || cache->key != key)
				{
					auto rebuilt = buildCache(st, key);
					if (!rebuilt) break;                                            // stop requested mid-build
					if (computeKey() != key) { mRebuildRequested = true; continue; }   // torn by a zone switch; redo
					cache = std::move(rebuilt);
					haveSelection = false;
					logNextSelection = true;
				}

				if (!mCameraKnown.load()) continue;
				const SimpleMath::Vector3 camera(mCamX.load(), mCamY.load(), mCamZ.load());
				const float radius = mRadius.load();
				const size_t budget = (size_t)std::max(mTriangleBudget.load(), 0.f);
				// Collision does not move: re-select only when the camera or a budget setting did.
				if (haveSelection && (camera - selectedCamera).LengthSquared() < 1e-6f && radius == selectedRadius
					&& budget == selectedBudget) continue;
				if (radius != selectedRadius || budget != selectedBudget) logNextSelection = true;

				SelectionStats stats;
				auto snapshot = selectAroundCamera(*cache, camera, radius, budget, st, stats);
				if (!snapshot) break;
				// Definitions decode lazily from live tag memory; if the level changed underneath, none of it can be trusted.
				if (computeKey() != key) { cache.reset(); mRebuildRequested = true; continue; }
				const size_t batches = snapshot->batches.size();
				publish(std::move(snapshot));
				haveSelection = true;
				selectedCamera = camera; selectedRadius = radius; selectedBudget = budget;

				if (logNextSelection || stats.budgetHit != lastBudgetHit)
				{
					PLOG_INFO << std::format("HCEVisibleGeometry selection at ({:.2f},{:.2f},{:.2f}): {} of {} placements in range "
						"drawn, {} triangles in {} batches, {} triangles of those pieces cut by the radius (radius {:.1f} wu, budget {}{}) "
						"| definitions decoded {}, empty {}, too large {} | placements failing the sphere guard {}, bigger than the "
						"whole budget {}",
						camera.x, camera.y, camera.z, stats.placements, stats.candidates, stats.triangles, batches,
						stats.trianglesOutsideRadius, radius, budget, stats.budgetHit ? ", BUDGET REACHED" : "", cache->decoded,
						cache->decodedEmpty, cache->decodedTooLarge, cache->outsideSphere, stats.overWholeBudget);
					logNextSelection = false;
					lastBudgetHit = stats.budgetHit;
				}

				if (mAnnounceNextBuild.exchange(false))
					if (auto messagesGUI = messagesGUIWeak.lock())
						messagesGUI->addMessage(cache->placements.empty()
							? std::string("Visible Geometry Overlay on: no solid visible instanced geometry in the loaded area "
								"(the log has a per-BSP census).")
							: std::format("Visible Geometry Overlay on: {} solid pieces in the loaded area, drawing the {} within "
								"{:.0f} wu ({} triangles{}).", cache->placements.size(), stats.placements, radius, stats.triangles,
								stats.budgetHit ? " - triangle budget reached" : ""));
			}
			catch (...)
			{
				// A throw out of a thread terminates the game. Transient failures (level load, zone switch) are normal.
				LOG_ONCE(PLOG_ERROR << "HCEVisibleGeometryOverlay rebuild threw; it will retry on the next change");
				cache.reset();
				haveSelection = false;
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

			// Hand the worker what it selects with, before anything below can return early.
			const SimpleMath::Vector3 cameraPosition = renderer->getCameraPosition();
			mCamX.store(cameraPosition.x); mCamY.store(cameraPosition.y); mCamZ.store(cameraPosition.z);
			mCameraKnown.store(true);
			mRadius.store(settings->hceVisibleGeometryOverlayRadius->GetValue());
			mTriangleBudget.store(settings->hceVisibleGeometryOverlayTriangleBudget->GetValue());
			mRefreshMs.store(settings->hceVisibleGeometryOverlayRefreshMs->GetValue());

			{
				std::unique_lock<std::mutex> lk(mSnapshotMutex, std::try_to_lock);
				if (lk.owns_lock()) mRenderSnapshot = mSnapshot;   // else keep last frame's
			}
			if (!mRenderSnapshot || mRenderSnapshot->batches.empty()) return;

			const float wireAlpha = settings->hceVisibleGeometryOverlayWireAlpha->GetValue();
			const float fillAlpha = settings->hceVisibleGeometryOverlayFillAlpha->GetValue();
			const bool wantWire = wireAlpha > 0.f, wantSolid = fillAlpha > 0.f;
			if (!wantWire && !wantSolid) return;
			// Hidden lines: lay the collision's depth down first (biased, so edges ON a surface still pass), and only
			// the nearest collision surface's lines and fill survive. Without it every far-side edge of every rock is
			// drawn too, which reads as a wireframe floating off the geometry.
			const bool hideHiddenLines = settings->hceVisibleGeometryOverlayHideHiddenLines->GetValue();
			const SimpleMath::Vector4 colour = settings->hceVisibleGeometryOverlayColor->GetValue();
			SimpleMath::Vector4 wireColour = colour, fillColour = colour;
			wireColour.w = std::clamp(colour.w * wireAlpha, 0.f, 1.f);
			fillColour.w = std::clamp(colour.w * fillAlpha, 0.f, 1.f);

			const DirectX::BoundingFrustum& frustum = renderer->getCameraFrustum();
			mVisibleSorted.clear();
			for (const DrawBatch& batch : mRenderSnapshot->batches)
			{
				if (!frustum.Intersects(DirectX::BoundingSphere(batch.center, batch.radius))) continue;
				mVisibleSorted.push_back({ (batch.center - cameraPosition).Length() - batch.radius, &batch });
			}
			std::sort(mVisibleSorted.begin(), mVisibleSorted.end(),
				[](const auto& a, const auto& c) { return a.first < c.first; });

			// Nearest first, within the shared upload ring's budget (each draw re-uploads its vertices).
			const size_t drawsPerBatch = (wantSolid ? 1 : 0) + (wantWire ? 1 : 0) + (hideHiddenLines ? 1 : 0);
			size_t budget = kFrameVertexBudget, drawable = 0;
			for (const auto& entry : mVisibleSorted)
			{
				const size_t cost = entry.second->vertices.size() * drawsPerBatch;
				if (cost > budget) break;
				budget -= cost;
				++drawable;
			}

			renderer->setSurfacePattern(0.f, 0.f);   // sticky renderer state - never inherit a pattern
			// Depth is shared by every overlay this frame: start from a clean buffer so another overlay's pre-pass
			// cannot hide or dash our lines depending on toggle order.
			renderer->clearDepth();
			// ALL pre-passes before ANY edge: a batch's lines drawn before a nearer batch's depth would not be hidden.
			if (hideHiddenLines)
				for (size_t i = 0; i < drawable; ++i)
				{
					const DrawBatch* batch = mVisibleSorted[i].second;
					if (batch->triangles.empty()) continue;
					const BatchModel model(*batch);
					renderer->drawTriangleCollection(&model, fillColour, CullingOption::CullNone, std::nullopt,
						DepthMode::DepthOnlyPrepassBiased);
				}
			// All fills before any wireframe, so a later fill cannot bury an earlier batch's lines.
			if (wantSolid)
				for (size_t i = 0; i < drawable; ++i)
				{
					const DrawBatch* batch = mVisibleSorted[i].second;
					if (batch->triangles.empty()) continue;
					const BatchModel model(*batch);
					renderer->drawTriangleCollection(&model, fillColour, CullingOption::CullNone, std::nullopt);
				}
			if (wantWire)
				for (size_t i = 0; i < drawable; ++i)
				{
					const DrawBatch* batch = mVisibleSorted[i].second;
					if (batch->edges.empty()) continue;
					const BatchModel model(*batch);
					renderer->drawEdgeCollection(&model, wireColour);
				}
			if (hideHiddenLines) renderer->clearDepth();   // do not leak our depth into overlays drawn after us
		}
		catch (HCMRuntimeException) {}
		catch (...)
		{
			LOG_ONCE(PLOG_ERROR << "HCEVisibleGeometryOverlay's 3D render path threw an unknown exception; suppressing further reports");
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
				PLOG_ERROR << "HCEVisibleGeometryOverlay: timed out draining the 3D render path; keeping the subscription";
				return false;
			}
			PLOG_ERROR << "HCEVisibleGeometryOverlay: still waiting for the 3D render path to finish before teardown";
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
				messagesGUI->addMessage("Disabling Visible Geometry Overlay.");
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
					"Visible Geometry Overlay can't run on this game build: could not locate the {} by byte signature. "
					"This usually means Halo Campaign Evolved updated (it reports no version number, so HCM refuses rather "
					"than read geometry from the wrong address).", mAnchorFailure));
			}
			if (!mRender3DEventCallback)
			{
				mIsActive = false;
				throw HCMRuntimeException("Visible Geometry Overlay needs HCM's 3D renderer, which is not available "
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
			const bool want = settings->hceVisibleGeometryOverlayToggle->GetValue()
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
			if (mIsActive) { publish(nullptr); startWorker(); }   // a level change invalidates the cache
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
	HCEVisibleGeometryOverlayImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceVisibleGeometryOverlayToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEVisibleGeometryOverlay only supports Halo Campaign Evolved");

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
		catch (HCMInitException) { PLOG_ERROR << "HCEVisibleGeometryOverlay could not resolve Render3DEventProvider"; }
		try { mCameraDataOptionalWeak = resolveDependentCheat(HCEGetCameraData); }
		catch (HCMInitException) { PLOG_ERROR << "HCEVisibleGeometryOverlay could not resolve HCEGetCameraData"; }

		mReady.store(true, std::memory_order_release);
	}

	~HCEVisibleGeometryOverlayImpl()
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


HCEVisibleGeometryOverlay::HCEVisibleGeometryOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEVisibleGeometryOverlayImpl>(game, dicon))
{
}

HCEVisibleGeometryOverlay::~HCEVisibleGeometryOverlay()
{
	PLOG_VERBOSE << "~" << getName();
}
