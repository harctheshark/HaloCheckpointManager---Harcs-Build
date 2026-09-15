#include "pch.h"
#include "H5GetHavokData.h"
#include "H5GetPlayerState.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5GetHavokData.h for the pointer chain and the evidence behind every offset here.

namespace
{
	constexpr uintptr_t kRvaPhysicsWorld = 0x05FB1CD8;

	constexpr uintptr_t kWorldBodies = 0x20;   // hkArray<hknpBody>
	constexpr uintptr_t kBodyStride  = 0x90;
	constexpr uintptr_t kBodyCol0 = 0x00, kBodyCol1 = 0x10, kBodyCol2 = 0x20, kBodyTrans = 0x30;
	constexpr uintptr_t kBodyFlags = 0x40, kBodyFilter = 0x44, kBodyShape = 0x48, kBodyMotionId = 0x68;

	// hknpCompoundShape
	constexpr uintptr_t kCompoundInstances = 0x60;   // hkFreeListArray: {ptr, size, capFlags, i32 firstFree}
	constexpr uintptr_t kInstanceStride = 0x80;
	constexpr uintptr_t kInstCol0 = 0x00, kInstCol1 = 0x10, kInstCol2 = 0x20, kInstTrans = 0x30;
	constexpr uintptr_t kInstScale = 0x40, kInstShape = 0x50;

	// hknpCompressedMeshShape -> hknpCompressedMeshShapeData -> hkcdStaticMeshTree
	constexpr uintptr_t kMeshData = 0x60, kMeshNumTriangles = 0x98;
	constexpr uintptr_t kDataToTree = 0x10;
	constexpr uintptr_t kTreeDomainMin = 0x10, kTreeDomainMax = 0x20;
	constexpr uintptr_t kTreeSections = 0x40, kTreePrimitives = 0x50, kTreeSharedIdx = 0x60;
	constexpr uintptr_t kTreePackedVerts = 0x70, kTreeSharedVerts = 0x80;
	constexpr uintptr_t kSectionStride = 0x60;
	constexpr uintptr_t kSecDomainMin = 0x10, kSecDomainMax = 0x20, kSecCodec = 0x30;
	constexpr uintptr_t kSecFirstPacked = 0x48, kSecSharedRel = 0x4C, kSecPrimRel = 0x50;
	constexpr uintptr_t kSecNumPacked = 0x58, kSecNumShared = 0x59, kSecPage = 0x5C;

	// hknpConvexShape / hknpConvexPolytopeShape, via hkRelArray {u16 size; u16 offset}
	constexpr uintptr_t kConvexVertices = 0x30;
	constexpr uintptr_t kPolytopePlanes = 0x40, kPolytopeFaces = 0x44, kPolytopeIndices = 0x48;
	constexpr uintptr_t kShapeConvexRadius = 0x14;
	constexpr uintptr_t kCapsuleA = 0x60, kCapsuleB = 0x70;

	// ⚠ THE FILLER IS 0xADDEADDE, NOT 0xDEADDEAD. The bytes are DE AD DE AD, which as a little-endian u32
	// reads 0xADDEADDE. Using the intuitive-looking constant lets 494 filler primitives through and emits
	// 988 junk triangles built from uninitialised local vertex slots.
	constexpr uint32_t kPrimitiveFiller = 0xADDEADDEu;

	// Shape vtable RVAs. hknpShape carries no type field, so the vtable IS the discriminator. Built from
	// intact MSVC RTTI (1329 type descriptors -> 1186 vtables) and each re-read live.
	constexpr uintptr_t kVtCompressedMesh      = 0x0347DD88;
	constexpr uintptr_t kVtStaticCompound      = 0x0347D968;
	constexpr uintptr_t kVtDynamicCompound     = 0x0347DB78;
	constexpr uintptr_t kVtConvexPolytope      = 0x0347A728;
	constexpr uintptr_t kVtSphere              = 0x0347AB98;
	constexpr uintptr_t kVtCapsule             = 0x0347FA38;

	constexpr uint32_t kMaxBodies = 65536;

	// IndexCollection is std::vector<uint16_t>, so 65535 is the hard ceiling on one piece's vertices.
	// Spill to a new piece below this, leaving room for the 4 vertices a single quad can add.
	constexpr size_t kPieceVertexCap = 60000;

	bool sehCopy(void* dest, const void* src, size_t size)
	{
		__try { memcpy(dest, src, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template<typename T>
	bool readAt(uintptr_t addr, T& out)
	{
		if (!addr) return false;
		return sehCopy(&out, (const void*)addr, sizeof(T));
	}

	SimpleMath::Vector3 readV3(const uint8_t* p, uintptr_t off)
	{
		float v[3]{};
		memcpy(v, p + off, sizeof(v));
		return { v[0], v[1], v[2] };
	}

	bool finite3(const SimpleMath::Vector3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	// A column-major hkTransform plus an optional per-axis scale.
	struct Xform
	{
		SimpleMath::Vector3 c0{ 1,0,0 }, c1{ 0,1,0 }, c2{ 0,0,1 }, t{ 0,0,0 };
		SimpleMath::Vector3 scale{ 1,1,1 };

		SimpleMath::Vector3 apply(const SimpleMath::Vector3& v) const
		{
			return c0 * (v.x * scale.x) + c1 * (v.y * scale.y) + c2 * (v.z * scale.z) + t;
		}
		// Compose: this applied AFTER inner.
		Xform compose(const Xform& inner) const
		{
			Xform r;
			r.c0 = c0 * (inner.c0.x * inner.scale.x) + c1 * (inner.c0.y * inner.scale.x) + c2 * (inner.c0.z * inner.scale.x);
			r.c1 = c0 * (inner.c1.x * inner.scale.y) + c1 * (inner.c1.y * inner.scale.y) + c2 * (inner.c1.z * inner.scale.y);
			r.c2 = c0 * (inner.c2.x * inner.scale.z) + c1 * (inner.c2.y * inner.scale.z) + c2 * (inner.c2.z * inner.scale.z);
			r.t = apply(inner.t);
			r.scale = { 1.f, 1.f, 1.f };
			return r;
		}
	};

	Xform readXform(const uint8_t* p, uintptr_t c0, uintptr_t c1, uintptr_t c2, uintptr_t tr)
	{
		Xform x;
		x.c0 = readV3(p, c0);
		x.c1 = readV3(p, c1);
		x.c2 = readV3(p, c2);
		x.t = readV3(p, tr);
		return x;
	}
}

class H5GetHavokData::Impl
{
private:
	GameState mGame;
	std::weak_ptr<H5GetPlayerState> mPlayerStateWeak;

	// Scratch buffers, reused across calls so a per-frame collect allocates nothing steady-state.
	std::vector<uint8_t> mSecBuf, mPrimBuf, mPackedBuf, mSharedIdxBuf;

	uintptr_t getWorld()
	{
		lockOrThrow(mPlayerStateWeak, playerState);
		uintptr_t w = 0;
		if (!readAt(playerState->getExeBase() + kRvaPhysicsWorld, w) || !w)
			throw HCMRuntimeException("The Halo 5 physics world is not loaded");
		return w;
	}

	uintptr_t exeBase()
	{
		lockOrThrow(mPlayerStateWeak, playerState);
		return playerState->getExeBase();
	}

	ShapeKind classify(uintptr_t shape, uintptr_t base)
	{
		uintptr_t vt = 0;
		if (!readAt(shape, vt) || vt < base) return ShapeKind::Unknown;
		const uintptr_t rva = vt - base;
		switch (rva)
		{
		case kVtCompressedMesh:  return ShapeKind::CompressedMesh;
		case kVtStaticCompound:
		case kVtDynamicCompound: return ShapeKind::Compound;
		case kVtConvexPolytope:  return ShapeKind::ConvexPolytope;
		case kVtSphere:          return ShapeKind::Sphere;
		case kVtCapsule:         return ShapeKind::Capsule;
		default:                 return ShapeKind::Unknown;
		}
	}

	// ---- the compressed static mesh ------------------------------------------------------------------
	// Decodes only the sections whose own AABB intersects the cull sphere. See the header for why that is
	// the whole performance story.
	void decodeCompressedMesh(uintptr_t shape, const Xform& toWorld,
		const SimpleMath::Vector3& around, float radius, int& budget,
		std::vector<Piece>& out, Piece& piece)
	{
		uintptr_t dataP = 0;
		if (!readAt(shape + kMeshData, dataP) || !dataP) return;
		const uintptr_t tree = dataP + kDataToTree;

		uint8_t th[0xA0]{};
		if (!sehCopy(th, (const void*)tree, sizeof(th))) return;

		const SimpleMath::Vector3 dmin = readV3(th, kTreeDomainMin);
		const SimpleMath::Vector3 dmax = readV3(th, kTreeDomainMax);
		if (!finite3(dmin) || !finite3(dmax)) return;
		const SimpleMath::Vector3 ext = dmax - dmin;

		uintptr_t secP = 0, primP = 0, sidxP = 0, packP = 0, shrP = 0;
		uint32_t secN = 0, primN = 0, sidxN = 0, packN = 0, shrN = 0;
		memcpy(&secP, th + kTreeSections, 8);      memcpy(&secN, th + kTreeSections + 8, 4);
		memcpy(&primP, th + kTreePrimitives, 8);   memcpy(&primN, th + kTreePrimitives + 8, 4);
		memcpy(&sidxP, th + kTreeSharedIdx, 8);    memcpy(&sidxN, th + kTreeSharedIdx + 8, 4);
		memcpy(&packP, th + kTreePackedVerts, 8);  memcpy(&packN, th + kTreePackedVerts + 8, 4);
		memcpy(&shrP, th + kTreeSharedVerts, 8);   memcpy(&shrN, th + kTreeSharedVerts + 8, 4);
		if (!secP || !secN || secN > 1000000) return;

		mSecBuf.resize((size_t)secN * kSectionStride);
		if (!sehCopy(mSecBuf.data(), (const void*)secP, mSecBuf.size())) return;

		const float r2 = radius * radius;
		// Shared vertices are quantised over the WHOLE tree domain at 21/21/22 bits.
		const float sx = ext.x / (float)((1u << 21) - 1);
		const float sy = ext.y / (float)((1u << 21) - 1);
		const float sz = ext.z / (float)((1u << 22) - 1);

		for (uint32_t s = 0; s < secN && budget > 0; ++s)
		{
			const uint8_t* sec = mSecBuf.data() + (size_t)s * kSectionStride;
			const SimpleMath::Vector3 smin = readV3(sec, kSecDomainMin);
			const SimpleMath::Vector3 smax = readV3(sec, kSecDomainMax);
			if (!finite3(smin) || !finite3(smax)) continue;

			// Cull against the section AABB, transformed only by translation for the static world (whose
			// rotation is identity) - cheap and conservative enough at this granularity.
			const SimpleMath::Vector3 c = toWorld.apply((smin + smax) * 0.5f);
			const float half = (smax - smin).Length() * 0.5f + radius;
			if ((c - around).LengthSquared() > half * half) continue;

			float codec[6]{};
			memcpy(codec, sec + kSecCodec, sizeof(codec));

			uint32_t firstPacked = 0, sharedRel = 0, primRel = 0;
			memcpy(&firstPacked, sec + kSecFirstPacked, 4);
			memcpy(&sharedRel, sec + kSecSharedRel, 4);
			memcpy(&primRel, sec + kSecPrimRel, 4);
			const uint8_t nPacked = sec[kSecNumPacked];
			const uint8_t nShared = sec[kSecNumShared];
			const uint8_t page = sec[kSecPage];

			// ⚠ These three u32 are PACKED as (baseIndex << 8) | count, not plain indices. Verified
			// because the per-section primitive ranges come out strictly contiguous and the last
			// section's base+count equals the primitive array size exactly on all 16 meshes.
			const uint32_t sharedBase = sharedRel >> 8;
			const uint32_t primBase = primRel >> 8;
			const uint32_t primCount = primRel & 0xFF;
			if (!primCount) continue;
			if (primBase + primCount > primN) continue;

			// Local vertex table: packed first, then shared.
			SimpleMath::Vector3 lv[256];
			const int total = (int)nPacked + (int)nShared;
			if (total <= 0 || total > 256) continue;

			if (nPacked)
			{
				if ((uint64_t)firstPacked + nPacked > packN) continue;
				uint32_t pv[256]{};
				if (!sehCopy(pv, (const void*)(packP + (size_t)firstPacked * 4), (size_t)nPacked * 4)) continue;
				for (int i = 0; i < nPacked; ++i)
				{
					// 11 / 11 / 10 bits. The class name in the binary literally declares the shared
					// counts, and the quantiser at exe+0x00D14829 unpacks z with an UNMASKED shr 22 -
					// which is only correct if z is exactly the remaining bits.
					const uint32_t q = pv[i];
					lv[i] = { codec[0] + (float)(q & 0x7FFu) * codec[3],
							  codec[1] + (float)((q >> 11) & 0x7FFu) * codec[4],
							  codec[2] + (float)((q >> 22) & 0x3FFu) * codec[5] };
				}
			}
			if (nShared)
			{
				if ((uint64_t)sharedBase + nShared > sidxN) continue;
				uint16_t si[256]{};
				if (!sehCopy(si, (const void*)(sidxP + (size_t)sharedBase * 2), (size_t)nShared * 2)) continue;
				for (int i = 0; i < nShared; ++i)
				{
					// ⚠ THE SHARED INDEX IS PAGED: the section's page byte supplies the high 16 bits.
					// Without it only 1.4% of shared vertices land inside their own section AABB; with
					// it, 100.00%.
					const uint32_t idx = ((uint32_t)page << 16) | si[i];
					if (idx >= shrN) { lv[nPacked + i] = {}; continue; }
					uint64_t q = 0;
					if (!sehCopy(&q, (const void*)(shrP + (size_t)idx * 8), 8)) { lv[nPacked + i] = {}; continue; }
					lv[nPacked + i] = { dmin.x + (float)(q & 0x1FFFFFull) * sx,
										dmin.y + (float)((q >> 21) & 0x1FFFFFull) * sy,
										dmin.z + (float)(q >> 42) * sz };
				}
			}

			mPrimBuf.resize((size_t)primCount * 4);
			if (!sehCopy(mPrimBuf.data(), (const void*)(primP + (size_t)primBase * 4), mPrimBuf.size())) continue;

			for (uint32_t p = 0; p < primCount && budget > 0; ++p)
			{
				const uint8_t* q = mPrimBuf.data() + (size_t)p * 4;
				uint32_t raw = 0;
				memcpy(&raw, q, 4);
				if (raw == kPrimitiveFiller) continue;          // unused slot
				const int i0 = q[0], i1 = q[1], i2 = q[2], i3 = q[3];
				if (i0 >= total || i1 >= total || i2 >= total || i3 >= total) continue;

				// ⚠ IndexCollection is 16-bit, so one piece can never hold more than 65535 vertices.
				// SPILL into a fresh piece rather than stopping: this used to zero the shared budget,
				// which is passed by reference all the way up into collect()'s body loop - so the first
				// body that filled up silently killed every REMAINING body, instance and object. That is
				// why raising the cull radius past ~7 WU made most of the overlay disappear.
				if (piece.verts.size() + 4 > kPieceVertexCap)
				{
					Piece next;
					next.kind = piece.kind;
					next.bodyIndex = piece.bodyIndex;
					next.collisionFilter = piece.collisionFilter;
					next.isStaticWorld = piece.isStaticWorld;
					out.push_back(std::move(piece));
					piece = std::move(next);
				}
				const uint16_t b = (uint16_t)piece.verts.size();

				// Quad iff indices[3] != indices[2] - a literal compare in the engine at exe+0x00D0100C.
				// This convention is what reproduces the engine's own numTriangles EXACTLY on all 16
				// meshes (2,371,357); i3!=i0, i3!=i1 and i3!=0xFF are each off by ~67k.
				const bool quad = (i3 != i2);
				piece.verts.emplace_back(toWorld.apply(lv[i0]));
				piece.verts.emplace_back(toWorld.apply(lv[i1]));
				piece.verts.emplace_back(toWorld.apply(lv[i2]));
				piece.triangles.insert(piece.triangles.end(), { b, (uint16_t)(b + 1), (uint16_t)(b + 2) });
				piece.edges.insert(piece.edges.end(),
					{ b, (uint16_t)(b + 1), (uint16_t)(b + 1), (uint16_t)(b + 2), (uint16_t)(b + 2), b });
				--budget;
				if (quad)
				{
					piece.verts.emplace_back(toWorld.apply(lv[i3]));
					piece.triangles.insert(piece.triangles.end(),
						{ b, (uint16_t)(b + 2), (uint16_t)(b + 3) });
					piece.edges.insert(piece.edges.end(),
						{ (uint16_t)(b + 2), (uint16_t)(b + 3), (uint16_t)(b + 3), b });
					--budget;
				}
			}
		}
	}

	// ---- convex hulls / spheres / capsules -----------------------------------------------------------
	// hkRelArray<T> is {u16 size; u16 offset} and its data lives at (u8*)&field + offset.
	void decodeConvex(uintptr_t shape, ShapeKind kind, const Xform& toWorld, Piece& piece)
	{
		uint16_t vcount = 0, voff = 0;
		if (!readAt(shape + kConvexVertices, vcount)) return;
		if (!readAt(shape + kConvexVertices + 2, voff)) return;
		if (!vcount || vcount > 1024) return;

		const uintptr_t vdata = shape + kConvexVertices + voff;
		std::vector<SimpleMath::Vector3> vs;
		vs.reserve(vcount);
		for (uint16_t i = 0; i < vcount; ++i)
		{
			float v[4]{};
			if (!sehCopy(v, (const void*)(vdata + (size_t)i * 16), sizeof(v))) return;
			const SimpleMath::Vector3 p{ v[0], v[1], v[2] };
			if (!finite3(p)) return;
			vs.push_back(p);
		}

		if (kind == ShapeKind::Sphere)
		{
			// The relarray holds 4 identical copies for SIMD; radius is convexRadius.
			float rad = 0.f;
			readAt(shape + kShapeConvexRadius, rad);
			if (!std::isfinite(rad) || rad <= 0.f) return;
			const SimpleMath::Vector3 c = toWorld.apply(vs[0]);
			// A cheap 3-ring wire sphere - enough to see one, not worth a real mesh.
			const uint16_t base = (uint16_t)piece.verts.size();
			constexpr int kSeg = 16;
			for (int axis = 0; axis < 3; ++axis)
				for (int i = 0; i < kSeg; ++i)
				{
					const float a = (float)i / kSeg * 6.2831853f;
					SimpleMath::Vector3 o{};
					if (axis == 0) o = { 0.f, std::cos(a) * rad, std::sin(a) * rad };
					if (axis == 1) o = { std::cos(a) * rad, 0.f, std::sin(a) * rad };
					if (axis == 2) o = { std::cos(a) * rad, std::sin(a) * rad, 0.f };
					piece.verts.emplace_back(c + o);
				}
			for (int axis = 0; axis < 3; ++axis)
				for (int i = 0; i < kSeg; ++i)
					piece.edges.insert(piece.edges.end(),
						{ (uint16_t)(base + axis * kSeg + i),
						  (uint16_t)(base + axis * kSeg + (i + 1) % kSeg) });
			return;
		}

		// Polytope: walk the face list so the wireframe is the real silhouette rather than a vertex cloud.
		uint16_t fcount = 0, foff = 0, icount = 0, ioff = 0;
		const bool haveFaces = readAt(shape + kPolytopeFaces, fcount)
			&& readAt(shape + kPolytopeFaces + 2, foff)
			&& readAt(shape + kPolytopeIndices, icount)
			&& readAt(shape + kPolytopeIndices + 2, ioff);

		const uint16_t base = (uint16_t)piece.verts.size();
		if (base + vs.size() > 60000) return;
		for (const auto& p : vs) piece.verts.emplace_back(toWorld.apply(p));

		if (haveFaces && fcount && icount && fcount <= 1024 && icount <= 4096)
		{
			std::vector<uint8_t> idx(icount);
			if (!sehCopy(idx.data(), (const void*)(shape + kPolytopeIndices + ioff), idx.size())) return;
			for (uint16_t f = 0; f < fcount; ++f)
			{
				uint8_t rec[4]{};
				if (!sehCopy(rec, (const void*)(shape + kPolytopeFaces + foff + (size_t)f * 4), 4)) return;
				uint16_t first = 0;
				memcpy(&first, rec, 2);
				const uint8_t n = rec[2];
				if (!n || (size_t)first + n > idx.size()) continue;
				for (uint8_t k = 0; k < n; ++k)
				{
					const uint8_t a = idx[first + k];
					const uint8_t b = idx[first + (k + 1) % n];
					if (a >= vs.size() || b >= vs.size()) continue;
					piece.edges.insert(piece.edges.end(), { (uint16_t)(base + a), (uint16_t)(base + b) });
				}
			}
		}
	}

	void decodeShape(uintptr_t shape, const Xform& toWorld, uintptr_t base,
		const SimpleMath::Vector3& around, float radius, int& budget,
		bool includeStaticWorld, bool includeInstances, bool includeObjects,
		int depth, std::vector<Piece>& out, Piece& piece)
	{
		if (!shape || depth > 4 || budget <= 0) return;
		const ShapeKind kind = classify(shape, base);

		if (kind == ShapeKind::Compound)
		{
			uintptr_t instP = 0; uint32_t instN = 0;
			if (!readAt(shape + kCompoundInstances, instP) || !instP) return;
			if (!readAt(shape + kCompoundInstances + 8, instN) || !instN || instN > 65536) return;
			if (!includeInstances && depth > 0) return;

			std::vector<uint8_t> ib((size_t)instN * kInstanceStride);
			if (!sehCopy(ib.data(), (const void*)instP, ib.size())) return;
			for (uint32_t i = 0; i < instN && budget > 0; ++i)
			{
				const uint8_t* e = ib.data() + (size_t)i * kInstanceStride;
				uintptr_t child = 0;
				memcpy(&child, e + kInstShape, sizeof(child));
				if (!child) continue;   // a free-list slot
				Xform x = readXform(e, kInstCol0, kInstCol1, kInstCol2, kInstTrans);
				x.scale = readV3(e, kInstScale);
				if (!finite3(x.scale) || x.scale.LengthSquared() < 1e-12f) x.scale = { 1,1,1 };
				decodeShape(child, toWorld.compose(x), base, around, radius, budget,
					includeStaticWorld, includeInstances, includeObjects, depth + 1, out, piece);
			}
			return;
		}

		if (kind == ShapeKind::CompressedMesh)
		{
			if (!includeStaticWorld) return;
			// Set the kind BEFORE decoding - the decoder can spill full pieces into `out`, and those
			// need to carry the kind too.
			piece.kind = ShapeKind::CompressedMesh;
			decodeCompressedMesh(shape, toWorld, around, radius, budget, out, piece);
			return;
		}

		if (kind == ShapeKind::ConvexPolytope || kind == ShapeKind::Sphere || kind == ShapeKind::Capsule)
		{
			if (!includeObjects) return;
			decodeConvex(shape, kind, toWorld, piece);
			if (piece.kind == ShapeKind::Unknown) piece.kind = kind;
		}
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game), mPlayerStateWeak(resolveDependentCheat(H5GetPlayerState))
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5GetHavokData only supports Halo 5: Forge");
	}

	void collect(const SimpleMath::Vector3& around, float radius, int maxTriangles,
		bool includeStaticWorld, bool includeInstances, bool includeObjects,
		std::vector<Piece>& out)
	{
		out.clear();
		const uintptr_t world = getWorld();
		const uintptr_t base = exeBase();

		uintptr_t bodiesP = 0; uint32_t bodiesN = 0;
		if (!readAt(world + kWorldBodies, bodiesP) || !bodiesP)
			throw HCMRuntimeException("The Halo 5 rigid body array is null");
		if (!readAt(world + kWorldBodies + 8, bodiesN) || !bodiesN || bodiesN > kMaxBodies)
			throw HCMRuntimeException("The Halo 5 rigid body count looks wrong");

		std::vector<uint8_t> bb((size_t)bodiesN * kBodyStride);
		if (!sehCopy(bb.data(), (const void*)bodiesP, bb.size()))
			throw HCMRuntimeException("Could not read the Halo 5 rigid body array");

		int budget = maxTriangles;
		for (uint32_t i = 0; i < bodiesN && budget > 0; ++i)
		{
			const uint8_t* b = bb.data() + (size_t)i * kBodyStride;
			uintptr_t shape = 0;
			memcpy(&shape, b + kBodyShape, sizeof(shape));
			if (!shape) continue;   // ⚠ the ONLY valid emptiness test - unused slots are NOT zeroed

			uint32_t filter = 0, motionId = 0;
			memcpy(&filter, b + kBodyFilter, 4);
			memcpy(&motionId, b + kBodyMotionId, 4);

			Xform x = readXform(b, kBodyCol0, kBodyCol1, kBodyCol2, kBodyTrans);
			if (!finite3(x.c0) || !finite3(x.c1) || !finite3(x.c2) || !finite3(x.t)) continue;

			Piece piece;
			piece.bodyIndex = i;
			piece.collisionFilter = filter;
			piece.isStaticWorld = (motionId == 0);

			decodeShape(shape, x, base, around, radius, budget,
				includeStaticWorld, includeInstances, includeObjects, 0, out, piece);

			if (!piece.verts.empty()) out.push_back(std::move(piece));
		}
	}

	Summary summarise()
	{
		Summary s;
		const uintptr_t world = getWorld();
		const uintptr_t base = exeBase();
		uintptr_t bodiesP = 0; uint32_t bodiesN = 0;
		if (!readAt(world + kWorldBodies, bodiesP) || !bodiesP) return s;
		if (!readAt(world + kWorldBodies + 8, bodiesN) || bodiesN > kMaxBodies) return s;

		std::vector<uint8_t> bb((size_t)bodiesN * kBodyStride);
		if (!sehCopy(bb.data(), (const void*)bodiesP, bb.size())) return s;

		for (uint32_t i = 0; i < bodiesN; ++i)
		{
			uintptr_t shape = 0;
			memcpy(&shape, bb.data() + (size_t)i * kBodyStride + kBodyShape, sizeof(shape));
			if (!shape) continue;
			++s.bodies;

			// One level down through a compound to find the mesh, which is how the static world is shaped.
			uintptr_t mesh = 0;
			if (classify(shape, base) == ShapeKind::Compound)
			{
				uintptr_t instP = 0; uint32_t instN = 0;
				if (readAt(shape + kCompoundInstances, instP) && instP
					&& readAt(shape + kCompoundInstances + 8, instN) && instN)
				{
					uintptr_t child = 0;
					if (readAt(instP + kInstShape, child) && child
						&& classify(child, base) == ShapeKind::CompressedMesh)
						mesh = child;
				}
			}
			else if (classify(shape, base) == ShapeKind::CompressedMesh)
			{
				mesh = shape;
			}

			if (mesh)
			{
				++s.staticMeshes;
				int32_t n = 0;
				if (readAt(mesh + kMeshNumTriangles, n) && n > 0) s.declaredTriangles += n;
			}
		}
		return s;
	}
};


H5GetHavokData::H5GetHavokData(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5GetHavokData::~H5GetHavokData() { PLOG_VERBOSE << "~" << getName(); }

void H5GetHavokData::collect(const SimpleMath::Vector3& around, float radius, int maxTriangles,
	bool includeStaticWorld, bool includeInstances, bool includeObjects, std::vector<Piece>& out)
{
	pimpl->collect(around, radius, maxTriangles, includeStaticWorld, includeInstances, includeObjects, out);
}

H5GetHavokData::Summary H5GetHavokData::summarise() { return pimpl->summarise(); }
