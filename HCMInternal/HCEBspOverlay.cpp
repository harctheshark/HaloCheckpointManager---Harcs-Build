#include "pch.h"
#include <shared_mutex>   // pch.h carries <mutex> only; the render-path gate is a shared_timed_mutex
#include "HCEBspOverlay.h"
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
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cmath>

// See HCEBspOverlay.h for what this draws and why the geometry is where it is.
//
// WHY THE THREE ADDRESSES ARE BYTE-SIGNATURE ANCHORED AND NOT IN InternalPointerData.xml
// --------------------------------------------------------------------------------------
// Identical reasoning to HCETriggerOverlay: HaloSimulation_tag_release.dll has no version resource, so every
// build reports 0.0.0.0 and the XML Version attribute cannot distinguish them. A stale global would not fail
// loudly, it would resolve to whatever now occupies the address and draw confident nonsense. Zero matches or
// more than one DISABLES the feature; it never falls back to a remembered address. Only the STRUCTURAL
// constants (block offsets and record strides, which are tag-format facts rather than build addresses) live
// in the XML.
namespace
{
	// ---------------------------------------------------------------------------------------------------------
	// Guarded reads, same mechanism as the trigger overlay - HCEGetPlayerState::tryReadRaw is SEH-wrapped, so a
	// moved offset degrades to a zero rather than a crash.
	// ---------------------------------------------------------------------------------------------------------
	uint32_t  readU32(uintptr_t a) { uint32_t v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	int32_t   readI32(uintptr_t a) { int32_t v = 0;  HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	uintptr_t readPtr(uintptr_t a) { uintptr_t v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }

	bool plausiblePointer(uintptr_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull; }

	// ---------------------------------------------------------------------------------------------------------
	// The two collision-BSP record variants. Both are real in this build and a BSP uses exactly one of them
	// (whichever block has a non-zero count) - the small form indexes with u16, the large form with u32.
	// Every offset here is read out of the tag struct definitions in the binary, and every record layout was
	// checked by walking its fields and requiring them to sum EXACTLY to the struct's declared size.
	// ---------------------------------------------------------------------------------------------------------
	struct CollisionLayout
	{
		bool large;
		uint32_t sentinel;                       // "no such index"
		size_t surfaceStride, edgeStride, vertexStride;
		size_t surfaceFirstEdge, surfaceFlags;   // firstEdge is index-width; flags is u16 in BOTH variants
		size_t edgeStart, edgeEnd, edgeForward, edgeReverse, edgeLeft, edgeRight;
	};

	constexpr CollisionLayout kSmallLayout
	{
		false, 0xFFFFu,
		14, 12, 16,
		0x02, 0x0A,
		0x00, 0x02, 0x04, 0x06, 0x08, 0x0A,
	};

	constexpr CollisionLayout kLargeLayout
	{
		true, 0xFFFFFFFFu,
		20, 24, 20,
		0x04, 0x0E,
		0x00, 0x04, 0x08, 0x0C, 0x10, 0x14,
	};

	// bit1 of surface_flags. A REAL definition in this build (9 named options), not a name heuristic.
	constexpr uint16_t kSurfaceInvisible = 0x0002;
	// bit4. An invalid surface is engine garbage and is never drawn, in either mode.
	constexpr uint16_t kSurfaceInvalid = 0x0010;

	// A ring longer than this is a malformed walk, not a real polygon. Collision surfaces are BSP-split convex
	// faces; the classic Blam engines cap them far below this.
	constexpr size_t kMaxRingLength = 64;

	// u16 indices in IndexCollection, so a batch can never exceed 65535 vertices. Chunking also gives the
	// frustum cull something smaller than "the entire level" to reject.
	constexpr size_t kMaxBatchVertices = 60000;

	// structure_bsp_resource_struct, definition record rva 0x9C9450, declared size 0x24 (the walk closes
	// exactly). Its fields are the three collision/geometry blocks at +0x00 (small), +0x0C (large) and
	// +0x18 (instanced, never read).
	constexpr size_t kResourceItemStride = 0x24;

	// Face-shading quantisation. Each bin becomes one extra draw per side, so this trades draw calls for
	// tonal resolution; 8 is plenty to separate a floor from a wall from a ceiling and still only ever 16
	// draws on a batch of ~1000 triangles.
	constexpr int kShadeBins = 8;

	// Per-SURFACE tint variation. Each collision surface (one wall piece, not one triangle) gets a small
	// fixed offset from its shade, so a big flat area built from several surfaces stops reading as one
	// undifferentiated slab. Derived from the surface's ordinal, so it is stable frame to frame - a jitter
	// that changed per frame would shimmer.
	constexpr int kVariationBins = 3;
	constexpr int kBucketsPerSide = kShadeBins * kVariationBins;
	constexpr int kTotalBuckets = 2 * kBucketsPerSide;

	// ⚠ EVERY BUCKET IS A DRAW, AND EVERY DRAW RE-UPLOADS THE WHOLE BATCH VERTEX ARRAY (drawIndexed takes a
	// vertex span, not a shared buffer). The renderer's per-frame upload ring is finite and a draw that does
	// not fit is dropped SILENTLY, which would look like the overlay losing random surfaces. So variation is
	// disabled on batches large enough for 48 copies to matter, rather than risking that.
	constexpr size_t kMaxVerticesForVariation = 8000;
	// A fixed WORLD-space light, deliberately not the view direction: view-relative shading slides around as
	// you turn and gives no stable sense of which way a wall faces. Off-axis on all three so no axis-aligned
	// surface (and Halo levels are full of them) lands exactly on a bin boundary.
	const SimpleMath::Vector3 kShadeLightDirection = SimpleMath::Vector3(0.30f, 0.45f, 0.84f);

	// Reads an index of the variant's width out of an already-copied local buffer.
	inline uint32_t readIndex(const uint8_t* base, size_t offset, bool large)
	{
		if (large)
		{
			uint32_t v; memcpy(&v, base + offset, sizeof(v)); return v;
		}
		uint16_t v; memcpy(&v, base + offset, sizeof(v)); return v;
	}

	inline uint16_t readFlags(const uint8_t* base, size_t offset)
	{
		uint16_t v; memcpy(&v, base + offset, sizeof(v)); return v;
	}

	// ---------------------------------------------------------------------------------------------------------
	// One drawable chunk of structure BSP, already expanded into world space so the render thread does no tag
	// reading, no triangulation and no allocation.
	// ---------------------------------------------------------------------------------------------------------
	// One collision surface's plane plus the slice of triangleIndices it produced. Recorded at build time so
	// the render thread can sort surfaces into "camera in front of this" and "camera behind this" without
	// touching tag data or re-triangulating.
	//
	// WHY THIS EXISTS: a filled shell drawn in one colour reads completely differently depending on which side
	// you are on, and not because of culling. From OUTSIDE, a view ray crosses the near wall, the far wall, the
	// floor and the ceiling, so four translucent layers stack up and the shell looks solid. From INSIDE a room
	// it crosses ONE surface, so the same wall is a single faint wash and barely reads as a wall at all.
	// Giving the two sides their own colour and opacity is what makes an interior legible.
	struct SurfaceSpan
	{
		SimpleMath::Vector3 normal{};
		float planeD = 0.f;          // plane is dot(normal, p) + planeD == 0
		uint32_t firstIndex = 0;     // into BspBatch::triangleIndices
		uint32_t indexCount = 0;
		uint32_t firstEdge = 0;      // into BspBatch::edgeIndices
		uint32_t edgeCount = 0;
		// ⚠ PER-SURFACE CENTRE, because the render-distance cull has to be per SURFACE.
		// It used to be tested per BATCH - and a whole level is ONE batch, so the test was "is the entire
		// level further away than the slider", i.e. all-or-nothing, i.e. the slider did nothing at any usable
		// value. Culling here is what makes it actually mean "how far away can a surface be and still draw".
		SimpleMath::Vector3 centre{};
	};

	struct BspBatch
	{
		VertexCollection renderVertices;
		IndexCollection triangleIndices;
		IndexCollection edgeIndices;
		std::vector<SurfaceSpan> surfaces;
		SimpleMath::Vector3 center{};
		float radius = 0.f;
		// Axis-aligned bounds, for the "is the camera inside this" test the interior style needs. The sphere
		// above is the frustum/distance cull; a sphere would report "inside" far too generously for this.
		DirectX::BoundingBox bounds{};
	};

	class BspBatchModel : public IModelTriangles, public IModelEdges
	{
	public:
		explicit BspBatchModel(const BspBatch& batch) : mBatch(batch) {}
		const VertexCollection& getTriangleVertices() const override { return mBatch.renderVertices; }
		const IndexCollection& getTriangleIndices() const override { return mBatch.triangleIndices; }
		const VertexCollection& getEdgeVertices() const override { return mBatch.renderVertices; }
		const IndexCollection& getEdgeIndices() const override { return mBatch.edgeIndices; }
	private:
		const BspBatch& mBatch;
	};

	// Same vertices, a caller-supplied index subset. Used to draw the camera-facing and camera-behind halves of
	// a batch in different colours without duplicating the vertex buffer.
	class BspSubsetModel : public IModelTriangles
	{
	public:
		BspSubsetModel(const VertexCollection& vertices, const IndexCollection& indices)
			: mVertices(vertices), mIndices(indices) {}
		const VertexCollection& getTriangleVertices() const override { return mVertices; }
		const IndexCollection& getTriangleIndices() const override { return mIndices; }
	private:
		const VertexCollection& mVertices;
		const IndexCollection& mIndices;
	};

	// The same, for line lists.
	class BspEdgeSubsetModel : public IModelEdges
	{
	public:
		BspEdgeSubsetModel(const VertexCollection& vertices, const IndexCollection& indices)
			: mVertices(vertices), mIndices(indices) {}
		const VertexCollection& getEdgeVertices() const override { return mVertices; }
		const IndexCollection& getEdgeIndices() const override { return mIndices; }
	private:
		const VertexCollection& mVertices;
		const IndexCollection& mIndices;
	};

	// ------------------------------------------------------------------------------------------------------
	// Ear-clipping triangulation of one collision surface's edge ring.
	//
	// ⚠ THIS REPLACED A TRIANGLE FAN, and the fan was WRONG. The fan rested on "BSP-split surfaces are convex
	// by construction", which is not true of this content: a collision surface is an edge-ring N-gon and may be
	// concave. Fanning a concave polygon from vertex 0 emits triangles that spill OUTSIDE the polygon and
	// overlap each other, so the fill comes out with holes and stray slivers - visible as a shell that looks
	// solid from one direction and broken from another. Ear clipping makes no convexity assumption.
	//
	// Winding is irrelevant to the output (the caller draws CullNone), so this only has to produce a correct
	// TESSELLATION, not a correct facing.
	//
	// Same method HCETriggerOverlay uses for sector caps, which are concave for the same kind of reason.
	// ------------------------------------------------------------------------------------------------------
	// Returns TRUE if every triangle came from a real ear, FALSE if the ear search gave up and the remainder
	// was fanned.
	//
	// ⚠ WHY A RETURN VALUE AND NOT A TRIANGLE COUNT. The obvious check - "did this emit fewer than n-2
	// triangles?" - is STRUCTURALLY DEAD. Ear clipping emits exactly one triangle per ear and the trailing fan
	// emits remaining.size()-2, so the total is ALWAYS n-2 whether or not a single ear was ever found. A
	// counter built on that comparison can never fire, which is precisely how an earlier build reported
	// "0 partially triangulated" while walls were visibly unfilled. The only honest signal is whether the ear
	// loop bailed, so that is what gets reported.
	bool triangulateRing(const std::vector<SimpleMath::Vector3>& points, uint16_t baseIndex,
		IndexCollection& out)
	{
		const size_t n = points.size();
		if (n < 3) return true;
		if (n == 3)
		{
			out.push_back(baseIndex);
			out.push_back((uint16_t)(baseIndex + 1));
			out.push_back((uint16_t)(baseIndex + 2));
			return true;
		}

		// Newell's method: a plane normal that is correct for any polygon, convex or not.
		SimpleMath::Vector3 normal(0.f, 0.f, 0.f);
		for (size_t i = 0; i < n; ++i)
		{
			const SimpleMath::Vector3& a = points[i];
			const SimpleMath::Vector3& b = points[(i + 1) % n];
			normal.x += (a.y - b.y) * (a.z + b.z);
			normal.y += (a.z - b.z) * (a.x + b.x);
			normal.z += (a.x - b.x) * (a.y + b.y);
		}
		if (normal.LengthSquared() < 1e-20f) return true;   // degenerate surface, nothing to fill
		normal.Normalize();

		SimpleMath::Vector3 axisU = (std::abs(normal.z) < 0.9f)
			? SimpleMath::Vector3::UnitZ.Cross(normal)
			: SimpleMath::Vector3::UnitX.Cross(normal);
		if (axisU.LengthSquared() < 1e-20f) return true;
		axisU.Normalize();
		const SimpleMath::Vector3 axisV = normal.Cross(axisU);

		std::vector<SimpleMath::Vector2> flat(n);
		for (size_t i = 0; i < n; ++i)
			flat[i] = SimpleMath::Vector2(points[i].Dot(axisU), points[i].Dot(axisV));

		float signedArea2 = 0.f;
		for (size_t i = 0; i < n; ++i)
			signedArea2 += flat[i].x * flat[(i + 1) % n].y - flat[(i + 1) % n].x * flat[i].y;

		std::vector<size_t> remaining(n);
		for (size_t i = 0; i < n; ++i) remaining[i] = i;
		if (signedArea2 < 0.f) std::reverse(remaining.begin(), remaining.end());   // ear clipping wants CCW

		auto cross2 = [](const SimpleMath::Vector2& o, const SimpleMath::Vector2& a, const SimpleMath::Vector2& b)
			{ return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };

		bool allEars = true;
		size_t guard = 0;
		const size_t guardLimit = n * n + 16;   // a malformed ring must not spin the rebuild
		while (remaining.size() > 3 && guard++ < guardLimit)
		{
			bool clipped = false;
			const size_t m = remaining.size();
			for (size_t i = 0; i < m; ++i)
			{
				const size_t ia = remaining[(i + m - 1) % m];
				const size_t ib = remaining[i];
				const size_t ic = remaining[(i + 1) % m];
				if (cross2(flat[ia], flat[ib], flat[ic]) <= 0.f) continue;   // reflex, not an ear

				// An ear may not contain any other vertex of the polygon.
				bool contains = false;
				for (size_t k = 0; k < m && !contains; ++k)
				{
					const size_t ip = remaining[k];
					if (ip == ia || ip == ib || ip == ic) continue;
					contains = cross2(flat[ia], flat[ib], flat[ip]) >= 0.f
						&& cross2(flat[ib], flat[ic], flat[ip]) >= 0.f
						&& cross2(flat[ic], flat[ia], flat[ip]) >= 0.f;
				}
				if (contains) continue;

				out.push_back((uint16_t)(baseIndex + ia));
				out.push_back((uint16_t)(baseIndex + ib));
				out.push_back((uint16_t)(baseIndex + ic));
				remaining.erase(remaining.begin() + i);
				clipped = true;
				break;
			}
			// No ear found: the ring is self-intersecting or otherwise malformed. Fan the remainder rather than
			// dropping the surface - a slightly wrong fill beats an invisible one, and the ring guard already
			// bounded how bad it can be. Reported, because a fanned concave remainder is exactly the wrong
			// geometry this function exists to avoid.
			if (!clipped) { allEars = false; break; }
		}
		if (guard >= guardLimit) allEars = false;

		for (size_t i = 1; i + 1 < remaining.size(); ++i)
		{
			out.push_back((uint16_t)(baseIndex + remaining[0]));
			out.push_back((uint16_t)(baseIndex + remaining[i]));
			out.push_back((uint16_t)(baseIndex + remaining[i + 1]));
		}
		return allEars;
	}

	void finaliseBatchBounds(BspBatch& batch)
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
		DirectX::BoundingBox::CreateFromPoints(batch.bounds,
			DirectX::XMVectorSet(lo.x, lo.y, lo.z, 0.f), DirectX::XMVectorSet(hi.x, hi.y, hi.z, 0.f));
	}
}


class HCEBspOverlay::HCEBspOverlayImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;
	std::optional<std::weak_ptr<Render3DEventProvider>> mRender3DProviderOptionalWeak;
	// The UE render camera. NOT optional in practice: Render3DEventProvider only gets a correct camera basis
	// and field of view while HCEGetCameraData's midhook is live. Without it the renderer falls back to the
	// sim's player-aim camera and a default FOV, which draws everything at visibly the wrong scale - the exact
	// symptom the trigger overlay had before this hook was added.
	std::optional<std::weak_ptr<HCEGetCameraData>> mCameraDataOptionalWeak;
	bool mCameraHookRequested = false;

	// Structural tag constants (NOT addresses) - see the header comment on why these are the only XML entries.
	int64_t mScenarioStructureBspBlock = 0;      // scenario -> 'structure bsps'
	int64_t mStructureBspReferenceStride = 0;    // scenario_structure_bsp_reference
	int64_t mStructureBspTagIndexOffset = 0;     // 'structure bsp' tag_reference, index field
	int64_t mStructureBspLocalTagIndexOffset = 0;// 'local structure bsp' fallback
	int64_t mTagInstanceTableOffset = 0;         // tagGlobals -> instance array
	int64_t mTagInstanceStride = 0;
	int64_t mTagInstanceDataOffset = 0;          // instance -> encoded tag data address
	int64_t mStructureBspResourceBlock = 0;      // structure_bsp -> raw resource
	int64_t mResourceSmallCollisionBsp = 0;
	int64_t mResourceLargeCollisionBsp = 0;
	int64_t mCollisionSurfacesBlock = 0;
	int64_t mCollisionEdgesBlock = 0;
	int64_t mCollisionVerticesBlock = 0;

	// Byte-signature anchored, never XML'd.
	uintptr_t mScenarioSlot = 0;
	uintptr_t mTagAddressTable = 0;
	uintptr_t mTagGlobalsSlot = 0;
	bool mAnchorsTried = false;
	bool mAnchorsGood = false;
	std::string mAnchorFailure;

	std::atomic_bool mReady{ false };
	bool mIsActive = false;
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRender3DEventCallback;

	// Render-path lifetime gate. Replaces a ScopedAtomicBool + `mCurrentlyRendering3D.wait(true)` pair that had
	// two defects. ScopedAtomicBool is `wait(true); atom = true;` - a TOCTOU, not a lock; D3D12Hook.h documents
	// and rejects the same flaw. And the waiter had NO timeout, so if the render thread ever stalled inside
	// onRender3DEvent, whoever called set3DRenderingEnabled(false) blocked forever at 0% CPU - including the
	// state-hook thread, which reaches it on every game state change, and therefore on every revert.
	//
	// The render path now never blocks (try_to_lock, skip the frame), so our teardown cannot stall the game's
	// render thread, and the drain side is bounded. See dropRender3DSubscription.
	std::shared_timed_mutex mRender3DGuard;
	std::atomic_bool mRender3DDraining{ false };

	// Serialises set3DRenderingEnabled as a whole, so mRender3DEventCallback has ONE owner rather than being
	// guarded on the drop side only. Three unsynchronised threads reach that function: the state-hook poll
	// thread (onGameStateChanged), and a thread GUISimpleToggle spawns and DETACHES per checkbox click
	// (onToggleEvent). BinarySetting's own mutex serialises toggle-against-toggle but not toggle-against-state-
	// change, so without this an enable and a disable can race on the same unique_ptr: one resets (deletes) it
	// while the other stores into it, and a lost update leaves a live subscription that the destructor's drain
	// never sees - a callback into freed memory every frame. A revert reaches the disable side on the poll
	// thread, which is exactly when a click is likely to be in flight.
	//
	// NOT mRender3DGuard: dropRender3DSubscription takes that one exclusively, and shared_timed_mutex is not
	// recursive. Lock order is mRender3DSubscriptionMutex -> mRender3DGuard, one way only. The render path takes
	// neither by blocking - it try_locks the guard and bails - so it cannot close a cycle.
	std::mutex mRender3DSubscriptionMutex;

	std::mutex mBatchesMutex;
	std::vector<BspBatch> mBatches;
	// Scratch for the render path's two-pass draw. A member, not a local, so the render thread never allocates.
	// Only ever touched under mBatchesMutex.
	std::vector<const BspBatch*> mVisibleBatches;
	// Per-frame index partitions for the front/back split. Members, not locals, so the render thread never
	// allocates after the first few frames. Only touched under mBatchesMutex.
	// [0 .. kShadeBins-1] = surfaces facing the camera, [kShadeBins .. 2*kShadeBins-1] = facing away.
	// Indexed [side][shadeBin][variationBin], flattened. Side 0 = facing the camera.
	std::array<IndexCollection, kTotalBuckets> mShadeBuckets;
	// Per-frame, distance-culled wireframe indices. A member so the render thread never allocates.
	IndexCollection mFrontEdges;
	// Rebuild key. This is STATIC tag data, so unlike the trigger overlay there is no reason to re-read it on a
	// timer - only when the loaded BSP set actually changes.
	uint64_t mCacheKey = 0;
	bool mCacheKeyValid = false;
	bool mLastBuildInvisibleOnly = true;
	size_t mLastSurfaceTotal = 0;
	size_t mLastSurfaceDrawn = 0;
	size_t mLastRingFailures = 0;
	size_t mLastZeroTriangleSurfaces = 0;   // ring walked, but the triangulator emitted nothing
	size_t mLastFannedSurfaces = 0;         // ear search gave up; remainder fanned (possibly wrong geometry)
	size_t mLastDegenerateNormals = 0;      // no usable plane normal - shaded flat rather than dropped
	size_t mLastResourceItems = 0;          // raw-resource items walked across all structure bsps
	size_t mLastBspsSeen = 0;               // structure bsps whose tag data resolved
	size_t mLastMaxRing = 0;
	// How many surfaces carry each surface_flags bit, tallied per rebuild and logged.
	// WHY THIS EXISTS: on the first real level tested, 575 of 575 structure surfaces had bit1 (INVISIBLE)
	// CLEAR, so the invisible-only filter drew nothing while the surfaces plainly are not rendered in game.
	// Without a histogram that is unfalsifiable guesswork ("maybe they are sky?"); with one, the log says
	// exactly which bits the level actually uses and the filter can be pointed at the right one.
	size_t mFlagHistogram[16]{};
	size_t mFlagsNone = 0;   // surfaces with no bits set at all

	// HCEGetCameraData owns the byte verification and the reference count; this only makes sure THIS overlay's
	// request is added and released exactly once. Throws on an unrecognised build.
	void attachCameraHook()
	{
		if (!mCameraDataOptionalWeak.has_value())
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the BSP "
				"overlay cannot read the render camera");
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData)
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the BSP "
				"overlay cannot read the render camera");

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

	uintptr_t resolveTagBlock(uint32_t encoded) const
	{
		if (encoded == 0 || encoded == 0xFFFFFFFFu) return 0;
		const uintptr_t regionBase = readPtr(mTagAddressTable + 8ull * (encoded >> 28));
		if (!plausiblePointer(regionBase)) return 0;
		return regionBase + 4ull * encoded;
	}

	// Copies a whole tag block into a local buffer with ONE guarded read. Reading a BSP element-by-element
	// through tryReadRaw would be tens of thousands of SEH frames per rebuild; this is three reads total.
	bool copyBlock(uintptr_t blockField, size_t stride, std::vector<uint8_t>& out, int32_t& outCount) const
	{
		out.clear();
		outCount = readI32(blockField);
		const uint32_t encoded = readU32(blockField + 4);
		if (outCount <= 0 || encoded == 0 || encoded == 0xFFFFFFFFu) return false;
		// Loose upper bound. The engine's own maxima are 32767/65535/131072 (small) and far higher for large;
		// this only has to reject "we are not looking at a collision bsp".
		if ((size_t)outCount > 4000000u) return false;

		const uintptr_t base = resolveTagBlock(encoded);
		if (!plausiblePointer(base)) return false;

		const size_t bytes = (size_t)outCount * stride;
		out.resize(bytes);
		if (!HCEGetPlayerState::tryReadRaw(base, out.data(), bytes)) { out.clear(); return false; }
		return true;
	}

	void resolveAnchors(uintptr_t simBase) // never throws
	{
		mAnchorsTried = true;
		mAnchorsGood = false;
		mAnchorFailure.clear();

		int hits = 0;

		// Shared with HCETriggerOverlay - the "78 02" inside the pattern is the trigger-volume count compare, so
		// the anchor is literally the engine's own scenario access.
		const uintptr_t scenarioInsn = HCESignatureScan::resolveUnique(
			simBase, "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00", hits);
		if (!scenarioInsn) { mAnchorFailure = std::format("scenario data pointer ({} matches)", hits); return; }
		mScenarioSlot = HCESignatureScan::ripTarget(scenarioInsn, 3, 7);

		const uintptr_t tableInsn = HCESignatureScan::resolveUnique(simBase,
			"48 8B D9 4C 8D 05 ?? ?? ?? ?? 8B 09 8B 53 08 8B C2 48 C1 E8 1C 49 8B 04 C0 3B 4C 90 10 7D", hits);
		if (!tableInsn) { mAnchorFailure = std::format("tag address table ({} matches)", hits); return; }
		mTagAddressTable = HCESignatureScan::ripTarget(tableInsn, 6, 10);

		// tag_get_data's prologue. The `mov rax,[rip+X]` sits at +0x13 (disp32 at +0x16, next insn at +0x1A),
		// and the tail `48 C1 E7 04 / 48 03 78 50` is the instance-stride shift (rax+rax*2 then <<4 == 48) plus
		// the +0x50 load of the instance array - so this pattern IS the tag instance lookup rather than
		// something that merely resembles it.
		const uintptr_t tagGetData = HCESignatureScan::resolveUnique(simBase,
			"48 89 5C 24 08 57 48 83 EC 20 0F B7 C2 8B DA 48 8D 3C 40 48 8B 05 ?? ?? ?? ?? 48 C1 E7 04 48 03 78 50", hits);
		if (!tagGetData) { mAnchorFailure = std::format("tag instance table ({} matches)", hits); return; }
		mTagGlobalsSlot = HCESignatureScan::ripTarget(tagGetData + 0x13, 3, 7);

		if (!mScenarioSlot || !mTagAddressTable || !mTagGlobalsSlot)
		{
			mAnchorFailure = "rip-relative operand read failed";
			return;
		}

		mAnchorsGood = true;
	}

	// scenario -> i'th structure bsp tag data. Deliberately inlines what the engine's bsp_from_index does
	// instead of calling it: that function takes a lock, and this runs on HCM's render thread.
	uintptr_t resolveStructureBsp(uintptr_t scenario, int index) const
	{
		const uintptr_t blockField = scenario + mScenarioStructureBspBlock;
		const uint32_t encoded = readU32(blockField + 4);
		const uintptr_t base = resolveTagBlock(encoded);
		if (!plausiblePointer(base)) return 0;

		const uintptr_t element = base + (uintptr_t)mStructureBspReferenceStride * index;
		uint32_t tagIndex = readU32(element + mStructureBspTagIndexOffset);
		// The engine picks the local BSP only when the main reference is -1; match that exactly.
		if (tagIndex == 0xFFFFFFFFu) tagIndex = readU32(element + mStructureBspLocalTagIndexOffset);
		if (tagIndex == 0xFFFFFFFFu) return 0;

		const uintptr_t tagGlobals = readPtr(mTagGlobalsSlot);
		if (!plausiblePointer(tagGlobals)) return 0;
		const uintptr_t instances = readPtr(tagGlobals + mTagInstanceTableOffset);
		if (!plausiblePointer(instances)) return 0;

		const uintptr_t instance = instances + (uintptr_t)mTagInstanceStride * (tagIndex & 0xFFFFu);
		return resolveTagBlock(readU32(instance + mTagInstanceDataOffset));
	}

	// Walks one surface's edge ring. Returns false if the ring does not close within kMaxRingLength, in which
	// case the caller falls back to drawing that surface's edges individually.
	// ⚠ The convention below (take start_vertex + forward_edge when the surface is on the edge's LEFT, else
	// end_vertex + reverse_edge) is the classic Blam one and is UNVERIFIED on this build. That is exactly why
	// the failure path exists and why wireframe is the default render style.
	static bool walkSurfaceRing(const std::vector<uint8_t>& edges, int32_t edgeCount,
		const CollisionLayout& layout, uint32_t surfaceIndex, uint32_t firstEdge,
		std::vector<uint32_t>& outVertices)
	{
		outVertices.clear();
		if (firstEdge == layout.sentinel || (int64_t)firstEdge >= edgeCount) return false;

		uint32_t edge = firstEdge;
		for (size_t step = 0; step < kMaxRingLength; ++step)
		{
			if ((int64_t)edge >= edgeCount) return false;
			const uint8_t* record = edges.data() + (size_t)edge * layout.edgeStride;

			const uint32_t left = readIndex(record, layout.edgeLeft, layout.large);
			const uint32_t vertex = (left == surfaceIndex)
				? readIndex(record, layout.edgeStart, layout.large)
				: readIndex(record, layout.edgeEnd, layout.large);
			const uint32_t next = (left == surfaceIndex)
				? readIndex(record, layout.edgeForward, layout.large)
				: readIndex(record, layout.edgeReverse, layout.large);

			if (vertex == layout.sentinel) return false;
			outVertices.push_back(vertex);

			if (next == layout.sentinel) return false;
			if (next == firstEdge) return outVertices.size() >= 3;   // closed
			edge = next;
		}
		return false;   // ran past the guard - malformed, treat as a failure
	}

	// Rebuilds mBatches from the scenario tag. CALLER MUST HOLD mBatchesMutex.
	void rebuild(bool invisibleOnly)
	{
		mVisibleBatches.clear();   // holds pointers INTO mBatches; must not outlive it
		mBatches.clear();
		mLastSurfaceTotal = 0;
		mLastSurfaceDrawn = 0;
		mLastRingFailures = 0;
		mLastZeroTriangleSurfaces = 0;
		mLastFannedSurfaces = 0;
		mLastDegenerateNormals = 0;
		mLastResourceItems = 0;
		mLastBspsSeen = 0;
		mLastMaxRing = 0;
		mFlagsNone = 0;
		for (size_t& bucket : mFlagHistogram) bucket = 0;

		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return;

		const int32_t bspCount = readI32(scenario + mScenarioStructureBspBlock);
		if (bspCount <= 0 || bspCount > 256) return;

		BspBatch current;
		std::vector<uint8_t> surfaces, edges, vertices;
		std::vector<uint32_t> ring;
		std::vector<SimpleMath::Vector3> ringPoints;   // world-space ring, for the triangulator
		uint32_t pendingEdgeFirst = 0, pendingEdgeCount = 0;   // this surface's slice of edgeIndices
		std::vector<uint8_t> wanted;      // per-surface: is this one being drawn
		std::vector<uint8_t> ringFailed;  // per-surface: ring walk failed, needs the edge fallback

		auto flushBatch = [&]()
			{
				if (current.renderVertices.empty()) return;
				finaliseBatchBounds(current);
				mBatches.push_back(std::move(current));
				current = BspBatch{};
				// ⚠ These are declared ONCE for the whole rebuild, so without this they keep pointing into the
				// batch we just moved away - and every span in the NEW batch would carry an edge slice from the
				// old one. `current = BspBatch{}` empties edgeIndices but cannot know about these.
				pendingEdgeFirst = 0;
				pendingEdgeCount = 0;
			};

		for (int b = 0; b < bspCount; ++b)
		{
			const uintptr_t sbsp = resolveStructureBsp(scenario, b);
			// A null here is normal, not an error: it is how an unloaded zone-set BSP presents.
			if (!plausiblePointer(sbsp)) continue;
			++mLastBspsSeen;

			// ⚠ THE RESOURCE BLOCK IS A BLOCK, WITH A COUNT. This used to resolve the encoded address and read
			// the FIRST element only, silently ignoring the count - so a BSP carrying more than one raw
			// resource item contributed exactly one item's worth of collision and the rest simply never
			// existed. That is indistinguishable from "the level has gaps". Read the count and walk them all.
			const int32_t resourceCount = readI32(sbsp + mStructureBspResourceBlock);
			const uintptr_t resourceBase = resolveTagBlock(readU32(sbsp + mStructureBspResourceBlock + 4));
			if (!plausiblePointer(resourceBase)) continue;
			if (resourceCount <= 0 || resourceCount > 256)
			{
				PLOG_WARNING << "HCEBspOverlay: structure bsp " << b << " has an implausible raw-resource count ("
					<< resourceCount << "); reading the first item only";
			}

			const int32_t resourcesToWalk = (resourceCount > 0 && resourceCount <= 256) ? resourceCount : 1;
			mLastResourceItems += (size_t)resourcesToWalk;

			for (int32_t r = 0; r < resourcesToWalk; ++r)
			{
			const uintptr_t resource = resourceBase + (uintptr_t)kResourceItemStride * r;

			// Pick the variant by whichever collision block is populated. ⚠ +0x18 of this same resource is the
			// INSTANCED geometry and is deliberately never read - that omission IS the "true BSP only" filter.
			uintptr_t collisionBsp = 0;
			CollisionLayout layout = kSmallLayout;
			const int32_t smallCount = readI32(resource + mResourceSmallCollisionBsp);
			const int32_t largeCount = readI32(resource + mResourceLargeCollisionBsp);
			if (smallCount > 0)
			{
				collisionBsp = resolveTagBlock(readU32(resource + mResourceSmallCollisionBsp + 4));
				layout = kSmallLayout;
			}
			else if (largeCount > 0)
			{
				collisionBsp = resolveTagBlock(readU32(resource + mResourceLargeCollisionBsp + 4));
				layout = kLargeLayout;
			}
			// Both populated would mean the variant choice is losing geometry; say so rather than guessing.
			if (smallCount > 0 && largeCount > 0)
				PLOG_WARNING << "HCEBspOverlay: bsp " << b << " resource " << r
					<< " has BOTH small (" << smallCount << ") and large (" << largeCount
					<< ") collision bsps; only the small one is being read";
			if (!plausiblePointer(collisionBsp)) continue;

			int32_t surfaceCount = 0, edgeCount = 0, vertexCount = 0;
			if (!copyBlock(collisionBsp + mCollisionSurfacesBlock, layout.surfaceStride, surfaces, surfaceCount)) continue;
			if (!copyBlock(collisionBsp + mCollisionEdgesBlock, layout.edgeStride, edges, edgeCount)) continue;
			if (!copyBlock(collisionBsp + mCollisionVerticesBlock, layout.vertexStride, vertices, vertexCount)) continue;

			mLastSurfaceTotal += (size_t)surfaceCount;

			auto vertexAt = [&](uint32_t index) -> SimpleMath::Vector3
				{
					float p[3]{};
					memcpy(p, vertices.data() + (size_t)index * layout.vertexStride, sizeof(p));
					return SimpleMath::Vector3(p[0], p[1], p[2]);
				};

			wanted.assign((size_t)surfaceCount, 0);
			ringFailed.assign((size_t)surfaceCount, 0);

			for (int32_t s = 0; s < surfaceCount; ++s)
			{
				const uint8_t* record = surfaces.data() + (size_t)s * layout.surfaceStride;
				const uint16_t flags = readFlags(record, layout.surfaceFlags);

				// Tallied BEFORE any filtering, so the histogram describes the level rather than the filter.
				if (flags == 0) ++mFlagsNone;
				for (int bit = 0; bit < 16; ++bit) if (flags & (1u << bit)) ++mFlagHistogram[bit];

				if (flags & kSurfaceInvalid) continue;                          // engine garbage, never drawn
				if (invisibleOnly && !(flags & kSurfaceInvisible)) continue;
				wanted[s] = 1;

				const uint32_t firstEdge = readIndex(record, layout.surfaceFirstEdge, layout.large);
				if (!walkSurfaceRing(edges, edgeCount, layout, (uint32_t)s, firstEdge, ring))
				{
					ringFailed[s] = 1;
					++mLastRingFailures;
					continue;
				}

				bool ringInRange = true;
				for (uint32_t v : ring) if ((int64_t)v >= vertexCount) { ringInRange = false; break; }
				if (!ringInRange) { ringFailed[s] = 1; ++mLastRingFailures; continue; }

				if (current.renderVertices.size() + ring.size() > kMaxBatchVertices) flushBatch();

				const uint16_t base = (uint16_t)current.renderVertices.size();
				ringPoints.clear();
				ringPoints.reserve(ring.size());
				for (uint32_t v : ring)
				{
					const SimpleMath::Vector3 p = vertexAt(v);
					ringPoints.push_back(p);
					current.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(p.x, p.y, p.z)));
				}

				// Ear clipping, NOT a fan - collision surfaces are edge-ring N-gons and are not guaranteed
				// convex. See triangulateRing.
				const size_t trianglesBefore = current.triangleIndices.size();
				if (!triangulateRing(ringPoints, base, current.triangleIndices))
					++mLastFannedSurfaces;

				// ⚠ DIAGNOSTIC. "surfaces drawn" counts surfaces whose RING WALKED, which is NOT the same as
				// surfaces that produced fill - a surface can walk a perfect ring and still emit zero triangles
				// if the triangulator bails (degenerate normal, no ear found). That gap is exactly what made
				// "575 of 575 drawn, 0 ring failures" coexist with visibly unfilled walls. Count it separately.
				const size_t emitted = (current.triangleIndices.size() - trianglesBefore) / 3;
				if (emitted == 0) ++mLastZeroTriangleSurfaces;
				mLastMaxRing = std::max(mLastMaxRing, ring.size());

				// ⚠ THE DOUBLE-WINDING EXPERIMENT WAS REMOVED HERE, DELIBERATELY. DO NOT REINSTATE IT.
				//
				// It emitted every triangle a second time reversed, to test whether something was culling by
				// facing despite CullNone. It changed nothing, which usefully PROVED facing is not the
				// mechanism - but it is not free. Under CULL_MODE_NONE both copies rasterise the SAME pixels at
				// the SAME depth, and with alpha blending that means every surface blends TWICE: an alpha of
				// 0.35 lands as 1-(1-0.35)^2 = 0.58. So the experiment silently doubled the opacity of the
				// whole overlay and contributed to the "everything is a green mist" symptom it was meant to
				// help diagnose. Coverage-neutral is not blend-neutral.

				// Record this surface's plane and the slice of indices it owns, so the render thread can pick a
				// colour per side. Newell's normal again - correct for concave rings, unlike a cross product of
				// the first three points, which is degenerate whenever those happen to be collinear (common
				// where a BSP split leaves collinear vertices on an edge).
				if (current.triangleIndices.size() > trianglesBefore)
				{
					SurfaceSpan span;
					span.firstIndex = (uint32_t)trianglesBefore;
					span.indexCount = (uint32_t)(current.triangleIndices.size() - trianglesBefore);
					span.firstEdge = pendingEdgeFirst;
					span.edgeCount = pendingEdgeCount;

					SimpleMath::Vector3 centreAccumulator(0.f, 0.f, 0.f);
					for (const SimpleMath::Vector3& p : ringPoints) centreAccumulator += p;
					span.centre = centreAccumulator / (float)std::max<size_t>(ringPoints.size(), 1);

					SimpleMath::Vector3 n(0.f, 0.f, 0.f);
					for (size_t i = 0; i < ringPoints.size(); ++i)
					{
						const SimpleMath::Vector3& a = ringPoints[i];
						const SimpleMath::Vector3& b = ringPoints[(i + 1) % ringPoints.size()];
						n.x += (a.y - b.y) * (a.z + b.z);
						n.y += (a.z - b.z) * (a.x + b.x);
						n.z += (a.x - b.x) * (a.y + b.y);
					}
					// ⚠ THE SPAN IS PUSHED UNCONDITIONALLY. It used to be pushed only when the normal was
					// non-degenerate, which meant a surface with an unusable normal owned NO span - and since
					// the colour passes iterate SPANS while the depth pre-pass draws the whole index buffer,
					// those triangles wrote depth and were then never coloured. They rendered as HOLES punched
					// through the overlay, showing the game underneath, which is exactly what was reported.
					// A degenerate normal is a fine reason to shade a surface flatly; it is not a reason to
					// stop drawing it. Zero normal => dot() == 0 => it lands in the facing bucket at the
					// darkest shade bin, which is a sane, visible default.
					if (n.LengthSquared() > 1e-20f)
					{
						n.Normalize();
						span.normal = n;
						span.planeD = -n.Dot(ringPoints[0]);
					}
					else
					{
						++mLastDegenerateNormals;
					}
					current.surfaces.push_back(span);
				}
				// Perimeter, from the same vertices - so wireframe and fill can never disagree.
				const size_t edgesBefore = current.edgeIndices.size();
				for (size_t i = 0; i < ring.size(); ++i)
				{
					current.edgeIndices.push_back((uint16_t)(base + i));
					current.edgeIndices.push_back((uint16_t)(base + (i + 1) % ring.size()));
				}
				pendingEdgeFirst = (uint32_t)edgesBefore;
				pendingEdgeCount = (uint32_t)(current.edgeIndices.size() - edgesBefore);

				++mLastSurfaceDrawn;
			}

			// Fallback for surfaces whose ring did not close: draw their edges straight out of the edge block,
			// so a wrong winding convention costs the FILL of those faces and nothing else.
			if (mLastRingFailures > 0)
			{
				for (int32_t e = 0; e < edgeCount; ++e)
				{
					const uint8_t* record = edges.data() + (size_t)e * layout.edgeStride;
					const uint32_t left = readIndex(record, layout.edgeLeft, layout.large);
					const uint32_t right = readIndex(record, layout.edgeRight, layout.large);

					const bool leftWants = left != layout.sentinel && (int64_t)left < surfaceCount
						&& wanted[left] && ringFailed[left];
					const bool rightWants = right != layout.sentinel && (int64_t)right < surfaceCount
						&& wanted[right] && ringFailed[right];
					if (!leftWants && !rightWants) continue;

					const uint32_t va = readIndex(record, layout.edgeStart, layout.large);
					const uint32_t vb = readIndex(record, layout.edgeEnd, layout.large);
					if ((int64_t)va >= vertexCount || (int64_t)vb >= vertexCount) continue;

					if (current.renderVertices.size() + 2 > kMaxBatchVertices) flushBatch();

					const uint16_t base = (uint16_t)current.renderVertices.size();
					const SimpleMath::Vector3 pa = vertexAt(va), pb = vertexAt(vb);
					current.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(pa.x, pa.y, pa.z)));
					current.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(pb.x, pb.y, pb.z)));
					current.edgeIndices.push_back(base);
					current.edgeIndices.push_back((uint16_t)(base + 1));
				}
			}
			}   // resource item loop
		}

		flushBatch();
		mLastBuildInvisibleOnly = invisibleOnly;
	}

	// The rebuild key. Static tag data, so this only has to change when the loaded BSP set does.
	uint64_t computeCacheKey() const
	{
		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) return 0;
		const uint64_t count = (uint64_t)(uint32_t)readI32(scenario + mScenarioStructureBspBlock);
		const uint64_t encoded = (uint64_t)readU32(scenario + mScenarioStructureBspBlock + 4);
		return (count << 32) ^ encoded ^ ((uint64_t)scenario << 1);
	}

	void onRender3DEvent(GameState game, IRenderer3D* renderer)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;
		if (!renderer) return;
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER) return;

		// Never blocks: if a teardown is draining the render path, skip this frame rather than stall the game's
		// render thread. try_to_lock also means a waiting drain can never park us behind it.
		if (mRender3DDraining.load(std::memory_order_acquire)) return;
		std::shared_lock<std::shared_timed_mutex> renderLock(mRender3DGuard, std::try_to_lock);
		if (!renderLock.owns_lock()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (!mAnchorsGood) return;

			const bool invisibleOnly = settings->hceBspOverlayInvisibleOnly->GetValue();

			std::scoped_lock batchesLock(mBatchesMutex);

			// Rebuild only when the BSP set changes or the filter is flipped - not on a timer. This is static
			// tag data; re-reading a whole BSP every 250ms would be pure waste.
			const uint64_t key = computeCacheKey();
			if (!mCacheKeyValid || key != mCacheKey || invisibleOnly != mLastBuildInvisibleOnly)
			{
				mCacheKey = key;
				mCacheKeyValid = true;
				rebuild(invisibleOnly);
				logBuildResult();
			}
			if (mBatches.empty()) return;

			const SettingsEnums::TriggerRenderStyle renderStyle = settings->hceBspOverlayRenderStyle->GetValue();
			if (renderStyle == SettingsEnums::TriggerRenderStyle::None) return;
			const bool wantSolid = renderStyle == SettingsEnums::TriggerRenderStyle::Solid
				|| renderStyle == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;
			const bool wantWire = renderStyle == SettingsEnums::TriggerRenderStyle::Wireframe
				|| renderStyle == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;

			const float renderDistance = settings->hceBspOverlayRenderDistance->GetValue();
			const SimpleMath::Vector4 base = settings->hceBspOverlayColor->GetValue();
			// Read before facingColour is built - it is applied to that alpha a few lines below.
			const float layerCompensation = std::clamp(settings->hceBspOverlayLayerCompensation->GetValue(), 1.f, 8.f);
			const float solidAlpha = std::clamp(settings->hceBspOverlayAlpha->GetValue(), 0.f, 1.f);
			const float wireAlpha = std::clamp(settings->hceBspOverlayWireframeAlpha->GetValue(), 0.f, 1.f);

			// FACES-YOU vs FACES-AWAY, not "inside" vs "outside".
			//
			// ⚠ THE OLD NAMES WERE THE BUG. The test is dot(planeNormal, camera) + d >= 0, which asks "does this
			// surface face me", NOT "am I inside this room" - there is no per-surface notion of a room. Standing
			// in a corridor the walls around you face you; standing outside the building its exterior faces you
			// too. Labelling those two answers "inside" and "outside" made the setting read as inverted
			// depending on where the user was standing, which is exactly what was reported. The mapping is
			// unchanged; the names now describe what is actually computed.
			const SimpleMath::Vector4 awayBase = settings->hceBspOverlayInsideColor->GetValue();
			const float awayAlpha = std::clamp(settings->hceBspOverlayInsideAlpha->GetValue(), 0.f, 1.f);

			SimpleMath::Vector4 facingColour = base;
			facingColour.w = std::clamp(base.w * solidAlpha, 0.f, 1.f);

			// LAYER COMPENSATION - why a wall seen from inside looks weaker than the same shell seen from
			// outside, and how to make them match.
			//
			// Nothing is rendering differently: it is alpha compositing doing exactly what it should. Looking
			// INTO the shell from outside, a view ray crosses the near wall, the far wall, the floor and the
			// ceiling, and those layers composite to 1-(1-a)^N - four layers at 0.35 land at 0.82. Standing
			// INSIDE a room the same ray crosses ONE surface and you get 0.35 flat. Raising the opacity slider
			// cannot fix it, because it raises BOTH cases: the single layer gets stronger, and so does the
			// stack it is being compared against.
			//
			// So pre-multiply the single layer to the value N layers would have reached. This is the exact
			// compositing identity, not an approximation: a' = 1-(1-a)^N.
			//
			// Applied to the FACING half only. Those are the surfaces pointing at the camera - the wall you are
			// looking at from inside a room - which is precisely the case that comes up one layer short.
			if (layerCompensation > 1.f)
				facingColour.w = std::clamp(1.f - std::pow(1.f - facingColour.w, layerCompensation), 0.f, 1.f);
			SimpleMath::Vector4 awayColour = awayBase;
			awayColour.w = std::clamp(awayBase.w * awayAlpha, 0.f, 1.f);

			// ONE wireframe colour, deliberately. It was briefly split per side like the fill, but the two
			// colours did not discriminate usefully in practice - an edge is a thin line, and which of its two
			// adjoining faces you happen to be in front of is not information worth reading off a hairline.
			// What the wireframe IS worth is being a different colour from the FILL: an outline that matches
			// the face it outlines is invisible, which was most of why an interior read as fog.
			SimpleMath::Vector4 wireColour = settings->hceBspOverlayWireframeColor->GetValue();
			wireColour.w = std::clamp(wireColour.w * wireAlpha, 0.f, 1.f);
			const bool shadingEnabled = settings->hceBspOverlayFaceShading->GetValue();
			const float shadingStrength = std::clamp(settings->hceBspOverlayShadingStrength->GetValue(), 0.f, 0.8f);
			const float variationAmount = std::clamp(settings->hceBspOverlaySurfaceVariation->GetValue(), 0.f, 0.5f);
			const bool variationEnabled = variationAmount > 0.f;
			const bool occludeFarSurfaces = settings->hceBspOverlayOccludeFarSurfaces->GetValue();
			const float patternContrast = std::clamp(settings->hceBspOverlayPatternContrast->GetValue(), 0.f, 1.f);
			const float patternCellSize = std::max(settings->hceBspOverlayPatternScale->GetValue(), 0.f);

			const SimpleMath::Vector3 cameraPosition = renderer->getCameraPosition();
			const DirectX::BoundingFrustum& cameraFrustum = renderer->getCameraFrustum();

			// Visible once, reused by both passes, so the cull work is not done twice.
			mVisibleBatches.clear();
			for (const BspBatch& batch : mBatches)
			{
				if (batch.renderVertices.empty()) continue;
				const float distance = (batch.center - cameraPosition).Length();
				if (distance - batch.radius > renderDistance) continue;
				if (!cameraFrustum.Intersects(DirectX::BoundingSphere(batch.center, batch.radius))) continue;
				mVisibleBatches.push_back(&batch);
			}

			// TWO PASSES, all solids before any wireframe. An opaque fill WRITES depth, so interleaving them
			// per batch lets a later batch's fill bury an earlier batch's lines. Lines are depth-tested
			// LESS_EQUAL and do not write, so drawing them after every fill puts them exactly on the surfaces
			// they belong to and nowhere else.
			//
			// CullNone throughout: a collision surface has to read the same from both sides, and the tag data
			// guarantees no winding convention worth culling on.
			// Same three-way option the MCC trigger overlay exposes, and the same containment test: is the
			// camera inside this batch's bounds. ⚠ For a structure BSP that is almost always TRUE - you stand
			// inside the level's shell - which is exactly why this defaults to Normal. DontRender is the
			// behaviour trigger volumes want (walls from outside, nothing from within) and would blank the BSP.
			// ⚠ NO TextureEnum HERE ANY MORE. This used to ask for SplotchyPattern, which the D3D12 path does
			// not implement - it logged "textured draws are not implemented" and drew untextured, so Patterned
			// was a no-op on this game. The pattern is now generated procedurally in the pixel shader
			// (setSurfacePattern), which needs no texture, no descriptor heap and no SRV.
			const SettingsEnums::TriggerInteriorStyle interiorStyle = settings->hceBspOverlayInteriorStyle->GetValue();

			if (wantSolid)
				for (const BspBatch* batch : mVisibleBatches)
				{
					if (batch->triangleIndices.empty()) continue;

					const bool cameraInside =
						batch->bounds.Contains(cameraPosition) == DirectX::ContainmentType::CONTAINS;
					if (cameraInside && interiorStyle == SettingsEnums::TriggerInteriorStyle::DontRender)
						continue;

					// Split by which SIDE of each surface's plane the camera is on, and draw the two halves in
					// their own colour and opacity. This is the fix for "a wall barely reads as a wall from
					// inside": the two sides genuinely need different settings, because the number of
					// translucent layers between you and the geometry differs by side, not because anything is
					// culled. Partitioning ~575 surfaces per frame is trivial CPU work and needs no rebuild.
					// ---------------------------------------------------------------------------------------
					// FACE SHADING - the thing that makes an interior readable.
					//
					// ⚠ THE PROBLEM THIS SOLVES. Geometrically the fill only ever covers FACES, never the space
					// between them. But standing inside a closed room, every view ray terminates on a wall, so
					// every pixel is covered and - because the pixel shader is a flat `return input.color`
					// with no lighting - the whole screen becomes one uniform block of colour with NO boundary
					// between one wall and the next. It reads as "the entire box and its innards are filled".
					// From OUTSIDE the same geometry is obvious, because the silhouette against the skybox
					// gives you the shape for free. Inside there is no silhouette, so shape has to come from
					// shading.
					//
					// Each surface is shaded by how much its (camera-oriented) normal faces a fixed world
					// light, quantised into a few bins, and each bin is drawn with its own colour constant.
					// That needs NO shader change - the colour is already a per-draw root constant - and costs
					// at most 2 sides x kShadeBins draws of a batch that is ~1000 triangles total.
					// ---------------------------------------------------------------------------------------
					for (auto& bucket : mShadeBuckets) bucket.clear();

					// See kMaxVerticesForVariation: fall back to no variation rather than risk silently
					// dropped draws on a big batch.
					const bool variationHere = variationEnabled
						&& batch->renderVertices.size() <= kMaxVerticesForVariation;

					if (batch->surfaces.empty())
					{
						mShadeBuckets[0] = batch->triangleIndices;   // no plane data - one flat bucket
					}
					else
					{
						uint32_t ordinal = 0;
						for (const SurfaceSpan& span : batch->surfaces)
						{
							const uint32_t thisOrdinal = ordinal++;
							// PER-SURFACE distance cull - see SurfaceSpan::centre for why this is not done at
							// batch level.
							if ((span.centre - cameraPosition).Length() > renderDistance) continue;
							const bool facing = span.normal.Dot(cameraPosition) + span.planeD >= 0.f;

							// ⚠ ABS OF THE RAW NORMAL - NOT the camera-oriented one. This was the per-side
							// brightness bug.
							//
							// Orienting the normal towards the viewer (n = facing ? normal : -normal) and then
							// taking dot(n, light) means the SAME SURFACE lands in a different shade bin
							// depending on which side you stand on: a wall that faces the light from outside
							// faces away from it from inside, so it drops to bin 0. At any real shading
							// strength that is dark enough to read as "this face is not rendering from in
							// here", which is exactly what it looked like.
							//
							// abs() makes the shade a property of the SURFACE rather than of the viewer: a
							// plane and its opposite side get the same tone, so the two sides are equally
							// strong. The facing/away distinction is still available - it picks the COLOUR
							// above - it just no longer leaks into brightness.
							const float ndl = std::clamp(std::abs(span.normal.Dot(kShadeLightDirection)), 0.f, 1.f);
							// ⚠ THE CLAMP MUST BE TWO-SIDED, and std::clamp above does NOT make it so: a NaN
							// survives std::clamp untouched (NaN < lo and hi < NaN are both false), cvttss2si
							// then yields INT_MIN, and `INT_MIN >= kShadeBins` is FALSE - so the old one-sided
							// guard let a NaN index mShadeBuckets ~48 GB below its base. Test NaN, clamp both.
							int bin = (ndl == ndl) ? (int)(ndl * (float)kShadeBins) : 0;   // NaN -> 0
							bin = std::clamp(bin, 0, kShadeBins - 1);

							// Stable per-surface variation: a cheap integer hash of the surface ordinal, NOT
							// anything derived from the camera, so a wall keeps the same tint as you move.
							int variation = 0;
							if (variationHere)
							{
								uint32_t h = thisOrdinal * 2654435761u;   // Knuth multiplicative
								h ^= h >> 15;
								variation = (int)(h % (uint32_t)kVariationBins);
							}

							// ⚠ The span's slice is trusted from tag data that copyBlock read speculatively, so
							// bound it against the buffer it indexes rather than assuming it is in range.
							if ((size_t)span.firstIndex + span.indexCount > batch->triangleIndices.size()) continue;

							const int index = (facing ? 0 : kBucketsPerSide) + bin * kVariationBins + variation;
							IndexCollection& target = mShadeBuckets[index];
							target.insert(target.end(),
								batch->triangleIndices.begin() + span.firstIndex,
								batch->triangleIndices.begin() + span.firstIndex + span.indexCount);
						}
					}

					// DEPTH PRE-PASS, then colour. This is what makes a closed shell readable.
					//
					// Pass 1 draws the WHOLE shell writing depth and no colour. Pass 2 draws the two sides
					// blending with depth writes OFF, so only fragments at the nearest depth survive: exactly
					// ONE translucent layer per pixel, and the result does not depend on submission order.
					//
					// ⚠ The previous attempt - blending AND writing depth in a single pass - was wrong. Whichever
					// translucent surface happened to be submitted first won the depth test while the ones behind
					// it still blended in, so the picture depended on arbitrary tag order and a room came out as
					// a uniform wash. Two passes is the correct construction; do not collapse it back to one.
					// ⚠ THE PRE-PASS IS WHAT MAKES SURFACES OPAQUE TO EACH OTHER. It lays depth for the whole
					// shell so only the NEAREST surface gets coloured - which is what stops an interior being
					// a wash, but also means you cannot see a far wall through a near one. Those are the same
					// property, so it has to be the user's choice, not a hard-coded one.
					if (occludeFarSurfaces)
					{
						// Colour is irrelevant here - this pass writes depth only, with the render target write
						// mask at 0 - but the signature wants one.
						const BspBatchModel whole(*batch);
						renderer->drawTriangleCollection(&whole, facingColour, CullingOption::CullNone,
							std::nullopt, DepthMode::DepthOnlyPrepass);
					}


					// ⚠ THE PATTERN IS GATED ON "Render Solid Interior as: Patterned". It used to be driven by
					// its contrast slider alone, which meant selecting Normal still gave you a checker - the
					// dropdown appeared to do nothing and the pattern appeared to be stuck on. The dropdown is
					// the switch; the two sliders only shape it.
					const bool wantPattern = (interiorStyle == SettingsEnums::TriggerInteriorStyle::Patterned);
					// Turned on here and off after the loop - it is sticky renderer state, so leaving it set
					// would pattern the trigger overlay too.
					renderer->setSurfacePattern(wantPattern ? patternCellSize : 0.f,
						wantPattern ? patternContrast : 0.f);

					// ⚠ AWAY-FACING SURFACES ARE DRAWN FIRST, FACING ONES SECOND. The order is the whole
					// difference between the two sides reading equally and one looking washed out.
					//
					// Without a depth pre-pass every surface blends, so the LAST thing drawn dominates. The
					// buckets are laid out facing-then-away, and iterating them in order therefore drew the
					// surfaces pointing AT you first and then blended every far wall over the top of them.
					// Standing in a room, that is precisely backwards: the walls around you are the ones you
					// are looking at, and they were being tinted by everything behind them. Painting away-faces
					// first and facing-faces last makes the surface you are actually looking at win, on both
					// sides of the shell, which is what "strong on both sides" means.
					//
					// This is a DRAW-ORDER fix, not an opacity one - the two sides can now use the same alpha
					// and still read the same, which they could not before.
					for (size_t pass = 0; pass < 2; ++pass)
					for (size_t b = (pass == 0 ? (size_t)kBucketsPerSide : 0);
					     b < (pass == 0 ? mShadeBuckets.size() : (size_t)kBucketsPerSide);
					     ++b)
					{
						if (mShadeBuckets[b].empty()) continue;

						const bool facing = b < (size_t)kBucketsPerSide;
						const SimpleMath::Vector4& base = facing ? facingColour : awayColour;
						const size_t within = b % (size_t)kBucketsPerSide;
						const size_t shadeBin = within / (size_t)kVariationBins;
						const size_t variationBin = within % (size_t)kVariationBins;

						SimpleMath::Vector4 shaded = base;
						if (shadingEnabled && shadingStrength > 0.f)
						{
							// Bin centre, so a bin never maps to either extreme exactly.
							const float t = ((float)shadeBin + 0.5f) / (float)kShadeBins;

							// ⚠ THE BRIGHTEST FACE IS THE COLOUR YOU PICKED, and shading only ever darkens
							// FROM there by at most `strength`. The first version instead scaled everything
							// down from a fixed 0.45 ambient, so even a fully-lit face lost half its colour and
							// the darkest bin sat at 0.48. That is bad on its own, but it compounds with alpha:
							// the colour is then blended at the opacity slider, so a 0.48 shade at 0.125 alpha
							// lands near 6% effective visibility - which does not read as "a dark wall", it
							// reads as A HOLE. Shading must never be able to make a surface invisible; that is
							// what the opacity slider is for.
							const float shade = 1.f - shadingStrength * (1.f - t);
							shaded.x *= shade;
							shaded.y *= shade;
							shaded.z *= shade;
						}

						// Per-surface tint, centred on zero so variation brightens as often as it darkens and
						// the average look of the overlay is unchanged by turning it on.
						if (variationAmount > 0.f && kVariationBins > 1)
						{
							const float v = ((float)variationBin / (float)(kVariationBins - 1)) - 0.5f;
							const float tint = 1.f + variationAmount * 2.f * v;
							shaded.x = std::clamp(shaded.x * tint, 0.f, 1.f);
							shaded.y = std::clamp(shaded.y * tint, 0.f, 1.f);
							shaded.z = std::clamp(shaded.z * tint, 0.f, 1.f);
						}

						const BspSubsetModel model(batch->renderVertices, mShadeBuckets[b]);
						renderer->drawTriangleCollection(&model, shaded, CullingOption::CullNone, std::nullopt);
					}
				}

			// Clear the pattern before ANY other draw. It is sticky renderer state and the wireframe below -
			// and the trigger overlay, which shares this renderer - must not inherit it.
			renderer->setSurfacePattern(0.f, 0.f);

			// WIREFRAME, one colour, one draw. Every surface emits its own copy of its perimeter, so shared
			// edges are drawn twice - harmless and invisible in a single colour, which is another reason not to
			// split it (two colours would blend the duplicates into a third).
			if (wantWire)
				for (const BspBatch* batch : mVisibleBatches)
				{
					if (batch->edgeIndices.empty()) continue;

					// The wireframe honours the SAME per-surface distance cull as the fill. Culling only the
					// fill would leave a lattice of lines floating where the surfaces had been removed, which
					// is worse than not culling at all.
					if (batch->surfaces.empty())
					{
						const BspBatchModel model(*batch);
						renderer->drawEdgeCollection(&model, wireColour);
						continue;
					}

					mFrontEdges.clear();
					for (const SurfaceSpan& span : batch->surfaces)
					{
						if (span.edgeCount == 0) continue;
						if ((span.centre - cameraPosition).Length() > renderDistance) continue;
						// Same bound as the triangle path: never index past the buffer the slice refers to.
						if ((size_t)span.firstEdge + span.edgeCount > batch->edgeIndices.size()) continue;
						mFrontEdges.insert(mFrontEdges.end(),
							batch->edgeIndices.begin() + span.firstEdge,
							batch->edgeIndices.begin() + span.firstEdge + span.edgeCount);
					}
					if (mFrontEdges.empty()) continue;

					const BspEdgeSubsetModel model(batch->renderVertices, mFrontEdges);
					renderer->drawEdgeCollection(&model, wireColour);
				}
		}
		catch (HCMRuntimeException)
		{
			// Expected constantly (no level loaded, mid-load).
		}
		catch (...)
		{
			LOG_ONCE(PLOG_ERROR << "HCEBspOverlay's 3D render path threw an unknown exception; suppressing further reports");
		}
	}

	// Logged on every rebuild, deliberately. If a level's collision turns out to be overwhelmingly INSTANCED,
	// this overlay legitimately draws almost nothing - and without these numbers that is indistinguishable
	// from a broken feature. Ring failures are logged for the same reason: a nonzero count means the winding
	// convention is wrong on this build and the fill is degraded to wireframe.
	void logBuildResult() const
	{
		PLOG_INFO << "HCEBspOverlay rebuilt: " << mLastSurfaceDrawn << " surfaces drawn of "
			<< mLastSurfaceTotal << " total structure surfaces, " << mBatches.size() << " batches, "
			<< mLastRingFailures << " ring-walk failures ("
			<< (mLastBuildInvisibleOnly ? "invisible only" : "all surfaces") << ")";

		PLOG_INFO << "HCEBspOverlay triangulation: " << mLastZeroTriangleSurfaces
			<< " surfaces emitted NO triangles, " << mLastFannedSurfaces
			<< " fell back to a FAN (ear search failed - geometry may be wrong on those), "
			<< mLastDegenerateNormals << " had no usable normal (shaded flat, NOT dropped), largest ring "
			<< mLastMaxRing << " verts, "
			<< (mBatches.empty() ? 0 : mBatches[0].triangleIndices.size() / 3)
			<< " triangles in batch 0 (no mirror - the double-winding experiment was removed)";

		// The named bits, so the log is readable without cross-referencing the flag definition.
		static const char* const kFlagNames[9] =
		{
			"two-sided", "INVISIBLE", "climbable", "breakable", "invalid",
			"conveyor", "slip", "plane-negated", "pathfinding-only"
		};
		std::string histogram;
		for (int bit = 0; bit < 9; ++bit)
			if (mFlagHistogram[bit])
				histogram += std::format("{}={} ", kFlagNames[bit], mFlagHistogram[bit]);
		for (int bit = 9; bit < 16; ++bit)
			if (mFlagHistogram[bit])
				histogram += std::format("bit{}={} ", bit, mFlagHistogram[bit]);   // undefined in this build
		if (histogram.empty()) histogram = "(no flag bits set on any surface) ";

		PLOG_INFO << "HCEBspOverlay surface flags: " << histogram << "| no-flags-at-all=" << mFlagsNone;
		PLOG_INFO << "HCEBspOverlay source walk: " << mLastBspsSeen << " structure bsps resolved, "
			<< mLastResourceItems << " raw-resource items walked (this used to read only the FIRST item of "
			"the first bsp)";

		// COVERAGE CHECK: does every triangle in the index buffer belong to some surface span?
		//
		// The colour passes iterate SPANS while the depth pre-pass draws the WHOLE index buffer, so any
		// triangle not covered by a span writes depth and is never coloured - a hole punched through the
		// overlay. This proves or disproves that outright instead of inferring it from a screenshot.
		// If coverage is exact and holes remain, they are NOT a rendering bug: they are real gaps in the
		// structure BSP, i.e. places where the collision is INSTANCED geometry, which this overlay excludes
		// by design.
		for (size_t b = 0; b < mBatches.size(); ++b)
		{
			size_t spanned = 0;
			for (const SurfaceSpan& s : mBatches[b].surfaces) spanned += s.indexCount;
			const size_t total = mBatches[b].triangleIndices.size();
			PLOG_INFO << "HCEBspOverlay batch " << b << " span coverage: " << spanned << " of " << total
				<< " indices covered by " << mBatches[b].surfaces.size() << " spans"
				<< (spanned == total ? "  [EXACT - orphaning is not the cause of any holes]"
					: "  [!! ORPHANED INDICES - these draw depth but no colour]");
		}
	}

	// How long a drain waits for an in-flight render callback. A frame is milliseconds, so this is generous for a
	// slow one and still finite - which is the entire point, since the wait it replaces had no bound at all.
	static constexpr std::chrono::milliseconds kRender3DDrainTimeout{ 1000 };

	// Stops new entries to onRender3DEvent, waits for any in-flight one, and drops the subscription - all before
	// releasing the guard, so a callback cannot slip in between the wait and the reset.
	//
	// mustSucceed distinguishes the two callers, and they need genuinely different answers:
	//
	//  - false (a toggle or a game state change, i.e. every revert): give up after the timeout and leave the
	//    subscription IN PLACE. Dropping it while a callback is still inside would free state that callback is
	//    using. Staying subscribed is harmless here because THIS OBJECT STILL EXISTS - mReady and mIsActive make
	//    the callback a no-op, and the destructor will drain it properly later. This is the path that used to
	//    block the state-hook thread forever.
	//
	//  - true (the destructor): there is no safe way to give up. Every member is about to be freed while the
	//    provider still holds a lambda capturing `this`, so returning early would turn a hang into a
	//    use-after-free. Keep waiting, but log each attempt so a stall is visible rather than silent.
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
					break;
				}
			}

			if (!mustSucceed)
			{
				mRender3DDraining.store(false, std::memory_order_release);
				PLOG_ERROR << "HCEBspOverlay: timed out draining the 3D render path; keeping the subscription "
					"rather than freeing state it may still be using";
				return false;
			}

			PLOG_ERROR << "HCEBspOverlay: still waiting for the 3D render path to finish before teardown; "
				"cannot give up here without freeing state a live callback is using";
		}
		mRender3DDraining.store(false, std::memory_order_release);
		return true;
	}

	// forTeardown is set only by the destructor - see dropRender3DSubscription for why it cannot give up there.
	void set3DRenderingEnabled(bool enabled, bool forTeardown = false)
	{
		// Covers BOTH branches: every read and write of mRender3DEventCallback happens under it.
		std::scoped_lock subscriptionLock(mRender3DSubscriptionMutex);

		if (!enabled)
		{
			if (mRender3DEventCallback)
				dropRender3DSubscription(forTeardown);
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
		mVisibleBatches.clear();   // holds pointers INTO mBatches; must not outlive it
		mBatches.clear();
		mCacheKeyValid = false;
		mCacheKey = 0;
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
				messagesGUI->addMessage("Disabling BSP Overlay.");
				return;
			}

			lockOrThrow(playerStateWeak, playerState);

			// BEFORE anything draws: without the render camera the overlay is drawn with the sim's player-aim
			// basis and a default field of view, which looks like a wrong-scale/wrong-FOV overlay rather than
			// like a failure. Surfacing it is better than silently drawing something subtly wrong.
			attachCameraHook();
			set3DRenderingEnabled(true);

			// Resolve on the TOGGLE, never on the render thread: the scan walks the whole executable image.
			if (!mAnchorsTried || !mAnchorsGood)
				resolveAnchors(playerState->getSimModuleBase());

			if (!mAnchorsGood)
			{
				mIsActive = false;
				throw HCMRuntimeException(std::format(
					"BSP Overlay can't run on this game build: could not locate the {} by byte signature. "
					"This usually means Halo Campaign Evolved updated. (Halo Campaign Evolved reports no version "
					"number at all, so HCM cannot detect an update any other way, and would rather refuse than "
					"draw geometry read from the wrong address.)", mAnchorFailure));
			}

			// The D3D12 renderer carries this feature alone - there is no ImGui fallback, unlike the trigger
			// overlay. Say so rather than sitting there drawing nothing.
			if (!mRender3DEventCallback)
			{
				mIsActive = false;
				throw HCMRuntimeException("BSP Overlay needs HCM's 3D renderer, which is not available "
					"(the D3D12 hook has not initialised). Try toggling it again once you are in-game.");
			}

			size_t drawn = 0, total = 0, batches = 0;
			{
				std::scoped_lock batchesLock(mBatchesMutex);
				lockOrThrow(settingsWeak, settings);
				mCacheKey = computeCacheKey();
				mCacheKeyValid = true;
				rebuild(settings->hceBspOverlayInvisibleOnly->GetValue());
				logBuildResult();
				drawn = mLastSurfaceDrawn;
				total = mLastSurfaceTotal;
				batches = mBatches.size();
			}

			mIsActive = true;
			if (batches == 0)
				messagesGUI->addMessage(std::format(
					"BSP Overlay on, but nothing to draw: {} structure surfaces in this level, none matched. "
					"This level's collision may be almost entirely INSTANCED geometry, which this overlay "
					"deliberately excludes.", total));
			else
				messagesGUI->addMessage(std::format("BSP Overlay on: {} of {} structure surfaces.", drawn, total));
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

			const bool want = settings->hceBspOverlayToggle->GetValue()
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

			// The toggle may already have been on when this level loaded, so the camera hook has to follow the
			// level and not just the toggle. Silent on failure - the toggle path is where the user gets told.
			try
			{
				if (want) attachCameraHook();
				else detachCameraHook();
			}
			catch (HCMRuntimeException) {}

			// Self-heal, same as the trigger overlay: attachCameraHook is a no-op once this overlay's request
			// is registered, so if anything detached the midhook behind our back it would stay detached for the
			// rest of the session. This runs on the state-hook thread, never the render thread.
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

	// Declared LAST - the callbacks must be destroyed before anything they touch.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	HCEBspOverlayImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceBspOverlayToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEBspOverlay only supports Halo Campaign Evolved");

		// Pure PointerDataStore lookups. NOTHING here touches game memory - the sim dll is not guaranteed to
		// be loaded when cheats are constructed.
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
		hceOffset(mResourceSmallCollisionBsp, hceResourceSmallCollisionBsp);
		hceOffset(mResourceLargeCollisionBsp, hceResourceLargeCollisionBsp);
		hceOffset(mCollisionSurfacesBlock, hceCollisionSurfacesBlock);
		hceOffset(mCollisionEdgesBlock, hceCollisionEdgesBlock);
		hceOffset(mCollisionVerticesBlock, hceCollisionVerticesBlock);
#undef hceOffset

		// The 3D renderer. Unlike the trigger overlay this is NOT optional in effect - there is no ImGui
		// fallback for arbitrary BSP geometry - but a failure to resolve must still not throw out of the
		// constructor and take down cheat registration. onToggleEvent reports it instead.
		try
		{
			mRender3DProviderOptionalWeak = resolveDependentCheat(Render3DEventProvider);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCEBspOverlay could not resolve Render3DEventProvider; the BSP overlay will be "
				"unavailable (it has no ImGui fallback)";
		}

		// The UE render camera. Without it the overlay still draws, but with the sim's player-aim basis and a
		// default FOV - i.e. at visibly the wrong scale. onToggleEvent reports that rather than throwing here.
		try
		{
			mCameraDataOptionalWeak = resolveDependentCheat(HCEGetCameraData);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCEBspOverlay could not resolve HCEGetCameraData; the BSP overlay would draw with "
				"the sim's player camera and a default field of view";
		}

		mReady.store(true, std::memory_order_release);
	}

	~HCEBspOverlayImpl()
	{
		mReady.store(false, std::memory_order_release);
		mIsActive = false;
		// forTeardown = true: giving up here would free state underneath a live render callback.
		set3DRenderingEnabled(false, true);
		detachCameraHook();
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCEBspOverlay::HCEBspOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEBspOverlayImpl>(game, dicon))
{
}

HCEBspOverlay::~HCEBspOverlay()
{
	PLOG_VERBOSE << "~" << getName();
}
