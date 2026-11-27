#include "pch.h"
#include "H2TAAObjects.h"
#include "H2CV_Halo2.h"
#include "H2CV_Collision.h"
#include "H2CV_Mem.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// See H2TAAObjects.h. halo2.dll 1.3528 (PE stamp 0x68A0F0F2); RE 2026-11-23 (idalib on the MCC halo2.dll.i64):
//   sub_1807E4970 (object render: node matrices) -> object_node_transform_interpolate_array 0x722CB0, else the object's own
//   node block; the validity test is sub_180723230 (interpolation enabled byte +0xDFCBF8, not a cinematic, both tick buffers
//   filled, index < 0x800, slot back-reference = the object datum).
namespace h2taa
{
	namespace
	{
		constexpr uintptr_t kInterpNodesRva = 0x722CB0;
		constexpr uint8_t kInterpNodesBytes[] = { 0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0xF2, 0x49, 0x8B, 0xE8, 0x48, 0x8D, 0x54, 0x24, 0x68, 0x8B, 0xD9, 0xE8 };
		constexpr uint8_t kHdrBeingDeleted = 0x10;
		constexpr int kMaxNodes = 255;
		constexpr size_t kMaxBoxesPerDef = 96;
		constexpr float kTeleport = 3.f;          // wu in one frame: not a motion
		constexpr float kCameraMargin = 0.25f;    // wu: the camera this close to a box = inside the object
		// An object that moved stays sent for this many frames after its last motion: a hovering vehicle moves less than the
		// threshold on some frames, and objects without interpolated poses only change on game ticks - sent / not sent / sent
		// would flip its pixels between its own motion and the camera's (user: "it blanks in and out", 2026-11-24).
		constexpr uint64_t kKeepFrames = 90;

		// object types (the header byte): biped 0, vehicle 1, weapon 2, equipment 3, garbage 4, projectile 5, scenery 6,
		// machine 7, control 8, light fixture 9, sound scenery 10, crate 11, creature 12
		bool trackedType(uint8_t t)
		{
			switch (t)
			{
			case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 11: case 12: return true;
			default: return false;
			}
		}

		bool bytesAt(uintptr_t a, const uint8_t* expect, size_t n)
		{
			__try { return memcmp((const void*)a, expect, n) == 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// the engine's own interpolated node matrices: 1 = got them, 0 = this object has none (use its own block), -1 = fault
		int sehInterpolatedNodes(uintptr_t fn, uint32_t datum, uintptr_t* matrices, int* count)
		{
			__try
			{
				int64_t p = 0; int n = 0;
				const char ok = reinterpret_cast<char(__fastcall*)(uint32_t, int64_t*, int*)>(fn)(datum, &p, &n);
				*matrices = (uintptr_t)p; *count = n;
				return ok ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// ---- affine maps, 3 rows of (x y z 1) coefficients, double ----
		struct Aff { double m[12]; };

		// real_matrix4x3 {scale, forward, left, up, position}: world = scale * (x F + y L + z U) + P
		Aff fromNode(const float* n)
		{
			const double s = n[0];
			return { { s * n[1], s * n[4], s * n[7], n[10],
			           s * n[2], s * n[5], s * n[8], n[11],
			           s * n[3], s * n[6], s * n[9], n[12] } };
		}

		bool invert(const Aff& a, Aff& out)
		{
			const double* m = a.m;
			const double c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
			const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
			if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
			const double id = 1.0 / det;
			double r[9];
			r[0] = c00 * id; r[1] = (m[2] * m[9] - m[1] * m[10]) * id; r[2] = (m[1] * m[6] - m[2] * m[5]) * id;
			r[3] = c01 * id; r[4] = (m[0] * m[10] - m[2] * m[8]) * id; r[5] = (m[2] * m[4] - m[0] * m[6]) * id;
			r[6] = c02 * id; r[7] = (m[1] * m[8] - m[0] * m[9]) * id; r[8] = (m[0] * m[5] - m[1] * m[4]) * id;
			for (int i = 0; i < 3; ++i)
			{
				out.m[i * 4 + 0] = r[i * 3 + 0]; out.m[i * 4 + 1] = r[i * 3 + 1]; out.m[i * 4 + 2] = r[i * 3 + 2];
				out.m[i * 4 + 3] = -(r[i * 3 + 0] * m[3] + r[i * 3 + 1] * m[7] + r[i * 3 + 2] * m[11]);
			}
			return true;
		}

		// a after b: (a . b)(p) = a(b(p))
		Aff compose(const Aff& a, const Aff& b)
		{
			Aff r;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 4; ++j)
					r.m[i * 4 + j] = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j] + a.m[i * 4 + 2] * b.m[2 * 4 + j];
				r.m[i * 4 + 3] += a.m[i * 4 + 3];
			}
			return r;
		}

		void apply(const Aff& a, const double p[3], double out[3])
		{
			for (int i = 0; i < 3; ++i) out[i] = a.m[i * 4 + 0] * p[0] + a.m[i * 4 + 1] * p[1] + a.m[i * 4 + 2] * p[2] + a.m[i * 4 + 3];
		}

		bool saneNode(const float* n)
		{
			for (int i = 0; i < 13; ++i) if (!std::isfinite(n[i])) return false;
			if (!(n[0] > 1e-4f && n[0] < 1e3f)) return false;
			return std::fabs(n[10]) < 1e5f && std::fabs(n[11]) < 1e5f && std::fabs(n[12]) < 1e5f;
		}

		struct Bounds
		{
			float lo[3]{ 1e30f, 1e30f, 1e30f }, hi[3]{ -1e30f, -1e30f, -1e30f };
			bool any = false;
			void add(float x, float y, float z)
			{
				const float v[3] = { x, y, z };
				for (int i = 0; i < 3; ++i) { lo[i] = (std::min)(lo[i], v[i]); hi[i] = (std::max)(hi[i], v[i]); }
				any = true;
			}
			bool sane() const
			{
				if (!any) return false;
				for (int i = 0; i < 3; ++i)
					if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || hi[i] < lo[i] || hi[i] - lo[i] > 400.f || std::fabs(lo[i]) > 1000.f || std::fabs(hi[i]) > 1000.f) return false;
				return true;
			}
		};
	}

	namespace
	{
		constexpr size_t kMaxTriVertsPerDef = 90000;   // 30000 triangles: far above any Halo 2 collision model
		// "this node jumped: its pixels have no history" (HCM_TAAProxy.hlsl tests the first row's w)
		const float kNoHistoryRows[12] = { 0, 0, 0, 1e30f, 0, 0, 0, 0, 0, 0, 0, 0 };

		// one node of a collision model while it is gathered
		struct NodeGeo
		{
			Bounds bounds;
			std::vector<float> tris;   // x y z per vertex, 3 vertices per triangle, node-local
		};
	}

	// The shape of one object definition (see the header): the collision model's triangles and a box per node, else one
	// box from the render model's bounds.
	bool ObjectMotion::buildDefinition(uintptr_t def, Def& out)
	{
		out.boxes.clear();
		out.tris.clear();
		h2::TagPools pools;
		if (!mem::okp(def) || !h2::tagPools(pools)) return false;
		const uint16_t hlmtIndex = mem::u16(def + 0x38, 0xFFFF);
		const uintptr_t hlmt = hlmtIndex != 0xFFFF ? h2::tagData(hlmtIndex, pools) : 0;
		if (!mem::okp(hlmt)) return true;   // no model: nothing to draw, nothing to track

		// the collision model: per node, every surface of every bsp of that node (all regions and permutations - a
		// permutation that is not the one on screen has nothing drawn near its triangles, so they claim nothing)
		std::unordered_map<uint16_t, NodeGeo> perNode;
		size_t totalVerts = 0;
		const uint16_t collIndex = mem::u16(hlmt + 0x0C, 0xFFFF);
		const uintptr_t coll = collIndex != 0xFFFF ? h2::tagData(collIndex, pools) : 0;
		if (mem::okp(coll))
		{
			uintptr_t rF = 0; uint32_t rN = 0;
			if (h2::resolveBlock(coll + 0x1C, pools, rF, rN) && rN <= 64)
				for (uint32_t r = 0; r < rN; ++r)
				{
					uintptr_t pF = 0; uint32_t pN = 0;
					if (!h2::resolveBlock(rF + (uintptr_t)r * 12 + 4, pools, pF, pN) || pN > 256) continue;
					for (uint32_t pi = 0; pi < pN; ++pi)
					{
						uintptr_t bF = 0; uint32_t bN = 0;
						if (!h2::resolveBlock(pF + (uintptr_t)pi * 20 + 4, pools, bF, bN) || bN > 64) continue;
						for (uint32_t k = 0; k < bN; ++k)
						{
							const uintptr_t el = bF + (uintptr_t)k * 0x44;
							h2cv::CollBsp cb;
							if (!cb.parse(el + 4, pools) || cb.verts.empty()) continue;
							NodeGeo& g = perNode[mem::u16(el, 0)];
							for (const auto& v : cb.verts)
								if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) g.bounds.add(v.x, v.y, v.z);
							decltype(cb.verts) poly;
							for (uint32_t s = 0; s < (uint32_t)cb.surfaces.size(); ++s)
							{
								if (!cb.polygon(s, poly) || poly.size() < 3) continue;
								bool finite = true;
								for (const auto& v : poly) finite = finite && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
								if (!finite || totalVerts + (poly.size() - 2) * 3 > kMaxTriVertsPerDef) continue;
								for (size_t i = 1; i + 1 < poly.size(); ++i)
								{
									const size_t idx[3] = { 0, i, i + 1 };
									for (size_t c : idx) { g.tris.push_back(poly[c].x); g.tris.push_back(poly[c].y); g.tris.push_back(poly[c].z); }
									totalVerts += 3;
								}
							}
						}
					}
				}
		}
		for (const auto& [node, g] : perNode)
		{
			const Bounds& b = g.bounds;
			if (!b.sane() || node > kMaxNodes || out.boxes.size() >= kMaxBoxesPerDef) continue;
			Box x; x.node = node;
			for (int i = 0; i < 3; ++i) { x.c[i] = 0.5f * (b.lo[i] + b.hi[i]); x.h[i] = 0.5f * (b.hi[i] - b.lo[i]) * 1.10f + 0.04f; }
			const uint32_t slot = (uint32_t)out.boxes.size();
			out.boxes.push_back(x);
			for (size_t i = 0; i + 2 < g.tris.size(); i += 3)
				out.tris.push_back(TriVertex{ { g.tris[i], g.tris[i + 1], g.tris[i + 2] }, slot });
		}
		if (!out.boxes.empty()) return true;   // (a collision model with boxes but no triangle at all falls back to its boxes)
		out.tris.clear();

		// no collision model: the render model's bounds (model space) in node 0's space
		const uint16_t modeIndex = mem::u16(hlmt + 0x04, 0xFFFF);
		const uintptr_t mode = modeIndex != 0xFFFF ? h2::tagData(modeIndex, pools) : 0;
		if (!mem::okp(mode)) return true;
		uintptr_t cF = 0, nF = 0; uint32_t cN = 0, nN = 0;
		float range[6];   // x min, x max, y min, y max, z min, z max
		if (!h2::resolveBlock(mode + 0x14, pools, cF, cN) || cN < 1 || cN > 16 || !mem::copy(range, cF, sizeof(range))) return true;
		Bounds model;
		model.add(range[0], range[2], range[4]); model.add(range[1], range[3], range[5]);
		if (!model.sane()) return true;
		float inv[13] = { 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };   // identity when node 0's inverse bind is unreadable
		if (h2::resolveBlock(mode + 0x48, pools, nF, nN) && nN >= 1 && nN <= 255)
		{
			float t[13];
			if (mem::copy(t, nF + 0x28, sizeof(t)) && saneNode(t)) memcpy(inv, t, sizeof(inv));
		}
		const Aff toNode = fromNode(inv);
		Bounds local;
		for (int k = 0; k < 8; ++k)
		{
			const double p[3] = { (k & 1) ? model.hi[0] : model.lo[0], (k & 2) ? model.hi[1] : model.lo[1], (k & 4) ? model.hi[2] : model.lo[2] };
			double q[3];
			apply(toNode, p, q);
			local.add((float)q[0], (float)q[1], (float)q[2]);
		}
		if (!local.sane()) return true;
		Box x; x.node = 0;
		for (int i = 0; i < 3; ++i) { x.c[i] = 0.5f * (local.lo[i] + local.hi[i]); x.h[i] = 0.5f * (local.hi[i] - local.lo[i]) * 1.05f + 0.02f; }
		out.boxes.push_back(x);
		return true;
	}

	const ObjectMotion::Def& ObjectMotion::definition(uintptr_t obj, uint32_t& tag)
	{
		static const Def kNone;
		const uintptr_t b = h2::base();
		// another map: every tag address is new
		const uintptr_t scenario = mem::q(b + h2::rva::SCENARIO);
		if (scenario != mDefsScenario) { mDefs.clear(); mDefsScenario = scenario; mLoggedDefs = false; ++mGeneration; }
		tag = mem::u32(obj, 0xFFFFFFFFu);
		if (tag == 0xFFFFFFFFu) return kNone;
		auto it = mDefs.find(tag);
		if (it != mDefs.end()) return it->second;
		h2::TagPools pools;
		if (!h2::tagPools(pools)) return kNone;
		Def d;
		if (!buildDefinition(h2::tagData((uint16_t)(tag & 0xFFFF), pools), d)) return kNone;   // unreadable now: tried again next frame
		for (const Box& x : d.boxes) d.extent = (std::max)(d.extent, 2.f * std::sqrt(x.h[0] * x.h[0] + x.h[1] * x.h[1] + x.h[2] * x.h[2]));
		return mDefs.emplace(tag, std::move(d)).first->second;
	}

	const std::vector<TriVertex>* ObjectMotion::mesh(uint32_t def) const
	{
		const auto it = mDefs.find(def);
		return (it == mDefs.end() || it->second.tris.empty()) ? nullptr : &it->second.tris;
	}

	void ObjectMotion::capture(const double vp[16], const float cam[3], ObjectFrame& out, size_t maxBoxes, size_t maxNodes, size_t maxDraws)
	{
		out.clear();
		mStats = ObjectStats{};
		const uintptr_t b = h2::base();
		if (!b) return;
		if (mEngineState == 0)
		{
			mEngineState = bytesAt(b + kInterpNodesRva, kInterpNodesBytes, sizeof(kInterpNodesBytes)) ? 1 : -1;
			PLOG_INFO << "Halo 2 TAA objects: " << (mEngineState > 0 ? "poses from the engine's interpolated node matrices (halo2+0x722CB0)"
				: "halo2+0x722CB0 is not what this was made for - poses from the objects' own node blocks (tick rate)");
		}
		++mFrame;
		const int cur = (int)(mFrame & 1), prev = cur ^ 1;
		// the unit shown in first person this frame (sub_1807E5430: unit datum halo2+0x165C500, first-person models drawn +0x165C504)
		const uint32_t fpUnit = mem::i32(b + 0x165C504, 0) > 0 ? mem::u32(b + 0x165C500, 0xFFFFFFFFu) : 0xFFFFFFFFu;
		static std::vector<h2::ObjectRef> objs;   // render thread only
		h2::enumerateObjects(objs);
		for (const h2::ObjectRef& o : objs)
		{
			if ((o.hdrFlags & kHdrBeingDeleted) || !trackedType(o.type) || !mem::okp(o.obj) || h2::objectHidden(o.obj)) continue;
			++mStats.objects;
			// this frame's pose, as the object renderer takes it
			uintptr_t src = 0; int count = 0;
			if (mEngineState > 0)
			{
				const int r = sehInterpolatedNodes(b + kInterpNodesRva, o.datum, &src, &count);
				if (r < 0)
				{
					mEngineState = -1;
					PLOG_ERROR << "Halo 2 TAA objects: the engine's node interpolation faulted - poses from the objects' own node blocks from now on";
				}
				else if (r == 1 && mem::okp(src) && count > 0) ++mStats.interpolated;
				else { src = 0; count = 0; }
			}
			if (!src)
			{
				int16_t size = 0, off = 0;
				if (!mem::copy(&size, o.obj + 0x118, 2) || !mem::copy(&off, o.obj + 0x11A, 2) || off <= 0 || size < 52) continue;
				src = o.obj + (uintptr_t)off; count = size / 52;
			}
			if (count <= 0) continue;
			if (count > kMaxNodes) count = kMaxNodes;
			State& st = mStates[o.datum];
			st.seen = mFrame;
			st.m[cur].resize((size_t)count * 13);
			if (!mem::copy(st.m[cur].data(), src, (size_t)count * 52)) { st.frame[cur] = 0; continue; }
			st.count[cur] = count; st.frame[cur] = mFrame;

			if (st.frame[prev] != mFrame - 1 || st.count[prev] != count) continue;   // no pose from last frame
			// Not one bit of its pose changed and it has not moved for a while: done. Nearly all scenery never moves - this
			// keeps tracking it to a copy and a compare, and its definition (collision model) is never even read.
			if (memcmp(st.m[cur].data(), st.m[prev].data(), (size_t)count * 52) == 0 && !(st.moved != 0 && mFrame - st.moved < kKeepFrames)) continue;
			// The first-person player and everything attached to them (the gun in the hand, the one on the back): NOT drawn in
			// first person - the first-person arms and gun are drawn in their place. Their triangles sit right at the camera
			// and claimed the wall next to you (user's screenshot of h2_taa_75: a gun-shaped cyan block on the wall).
			if (fpUnit != 0xFFFFFFFFu)
			{
				bool own = false;
				uint32_t d = o.datum;
				uintptr_t at = o.obj;
				for (int up = 0; up < 6 && mem::okp(at); ++up)
				{
					if ((d & 0xFFFF) == (fpUnit & 0xFFFF)) { own = true; break; }
					d = mem::u32(at + 0x14, 0xFFFFFFFFu);   // the parent (object_attach: child +0x14)
					if (d == 0xFFFFFFFFu) break;
					at = h2::objectFromDatum(d);
				}
				if (own) { ++mStats.cameraInside; continue; }
			}
			uint32_t tag = 0xFFFFFFFFu;
			const Def& def = definition(o.obj, tag);
			if (def.boxes.empty()) continue;
			++mStats.withShape;
			const bool mesh = !def.tris.empty();
			// The camera inside an object: only a reason to leave it out for bipeds (the first-person player's own body is
			// not drawn - its triangles would claim the floor under the feet) and for boxes. A machine or a vehicle the
			// camera is inside IS drawn around it (riding an elevator - user, 2026-11-24) and must keep its motion.
			const bool cameraTest = !mesh || o.type == 0;
			if (mesh ? (out.nodes.size() + def.boxes.size() > maxNodes || out.draws.size() >= maxDraws) : (out.boxes.size() >= maxBoxes)) continue;

			// Per node of the definition: is the camera inside it (-> the whole object is left out), did it move, is it on
			// screen. An object is sent when ANY node moved - then with ALL its nodes, the still ones too (zero motion): a
			// still torso must claim its own pixels, or the arm moving next to it would.
			const size_t firstBox = out.boxes.size(), firstNode = out.nodes.size();
			bool cameraInside = false, anyMoved = false, anyOnScreen = false;
			for (const Box& bx : def.boxes)
			{
				GpuNode gn{};   // all zero: no usable pose, the node's triangles collapse to a point
				const float* nc = bx.node < count ? &st.m[cur][(size_t)bx.node * 13] : nullptr;
				const float* np = bx.node < count ? &st.m[prev][(size_t)bx.node * 13] : nullptr;
				Aff mc{}, inv{};
				if (nc && saneNode(nc) && saneNode(np) && invert(mc = fromNode(nc), inv))
				{
					const double camW[3] = { cam[0], cam[1], cam[2] };
					double camL[3];
					apply(inv, camW, camL);
					const double margin = kCameraMargin / (double)nc[0];
					if (cameraTest && std::fabs(camL[0] - bx.c[0]) < bx.h[0] + margin && std::fabs(camL[1] - bx.c[1]) < bx.h[1] + margin && std::fabs(camL[2] - bx.c[2]) < bx.h[2] + margin)
					{
						cameraInside = true;
						break;
					}
					const Aff delta = compose(fromNode(np), inv);   // now -> last frame
					// how far the box moved (its centre and two corners), and its screen rectangle
					double worst = 0, dist = 1e30;
					float rect[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
					bool behind = false;
					for (int k = 0; k < 9; ++k)
					{
						const double pl[3] = { bx.c[0] + (k == 8 ? 0.0 : ((k & 1) ? bx.h[0] : -bx.h[0])), bx.c[1] + (k == 8 ? 0.0 : ((k & 2) ? bx.h[1] : -bx.h[1])),
							bx.c[2] + (k == 8 ? 0.0 : ((k & 4) ? bx.h[2] : -bx.h[2])) };
						double pw[3], pp[3];
						apply(mc, pl, pw);
						if (k == 8 || k == 0 || k == 7)
						{
							apply(delta, pw, pp);
							const double d = std::sqrt((pp[0] - pw[0]) * (pp[0] - pw[0]) + (pp[1] - pw[1]) * (pp[1] - pw[1]) + (pp[2] - pw[2]) * (pp[2] - pw[2]));
							worst = (std::max)(worst, d);
						}
						if (k == 8)
						{
							dist = std::sqrt((pw[0] - cam[0]) * (pw[0] - cam[0]) + (pw[1] - cam[1]) * (pw[1] - cam[1]) + (pw[2] - cam[2]) * (pw[2] - cam[2]));
							continue;
						}
						const double cx = pw[0] * vp[0] + pw[1] * vp[4] + pw[2] * vp[8] + vp[12];
						const double cy = pw[0] * vp[1] + pw[1] * vp[5] + pw[2] * vp[9] + vp[13];
						const double cw = pw[0] * vp[3] + pw[1] * vp[7] + pw[2] * vp[11] + vp[15];
						if (!(cw > 1e-3)) { behind = true; continue; }
						const float x = (float)(cx / cw), y = (float)(cy / cw);
						rect[0] = (std::min)(rect[0], x); rect[1] = (std::min)(rect[1], y); rect[2] = (std::max)(rect[2], x); rect[3] = (std::max)(rect[3], y);
					}
					// A jump, not a motion (a teleport, a respawn): further in one frame than 3 wu AND than 1.5 x the object's own size
					// (a carrier 300 wu long doing 10 wu a frame is flying, not teleporting - user: "the Cairo Station supercarrier",
					// 2026-11-24). Its pixels have no history worth fetching: they are sent marked "no history".
					const double jump = (std::max)((double)kTeleport, 1.5 * (double)def.extent * (double)nc[0]);
					const bool teleport = !std::isfinite(worst) || worst > jump;
					if (teleport || worst >= (std::max)(2e-5, dist * 2e-5)) anyMoved = true;   // more than a few hundredths of a pixel
					bool onScreen = true;
					if (behind) { rect[0] = -1.f; rect[1] = -1.f; rect[2] = 1.f; rect[3] = 1.f; }   // crosses the camera plane: the whole view
					else if (rect[2] < -1.f || rect[0] > 1.f || rect[3] < -1.f || rect[1] > 1.f) onScreen = false;
					else
					{
						const float pad = 0.004f;
						rect[0] = (std::max)(-1.f, rect[0] - pad); rect[1] = (std::max)(-1.f, rect[1] - pad);
						rect[2] = (std::min)(1.f, rect[2] + pad); rect[3] = (std::min)(1.f, rect[3] + pad);
					}
					if (mesh)
					{
						anyOnScreen = anyOnScreen || onScreen;
						for (int i = 0; i < 12; ++i) { gn.l2w[i] = (float)mc.m[i]; gn.delta[i] = teleport ? kNoHistoryRows[i] : (float)delta.m[i]; }
					}
					else if (onScreen && out.boxes.size() < maxBoxes)
					{
						GpuProxy g{};
						memcpy(g.rect, rect, sizeof(rect));
						for (int i = 0; i < 12; ++i) { g.w2l[i] = (float)inv.m[i]; g.delta[i] = teleport ? kNoHistoryRows[i] : (float)delta.m[i]; }
						for (int i = 0; i < 3; ++i) { g.centre[i] = bx.c[i]; g.half[i] = bx.h[i]; }
						out.boxes.push_back(g);
					}
				}
				if (mesh) out.nodes.push_back(gn);
			}
			if (anyMoved && !cameraInside) st.moved = mFrame;
			if (cameraInside) { out.boxes.resize(firstBox); out.nodes.resize(firstNode); ++mStats.cameraInside; }
			// nothing of it moved (its pixels follow the camera's motion, which is right), or none of it is in view
			else if (!(anyMoved || (st.moved != 0 && mFrame - st.moved < kKeepFrames)) || (mesh && !anyOnScreen)) { out.boxes.resize(firstBox); out.nodes.resize(firstNode); }
			else if (mesh)
			{
				out.draws.push_back(TriDraw{ tag, (uint32_t)firstNode });
				++mStats.moving; ++mStats.meshObjects;
			}
			else if (out.boxes.size() > firstBox) ++mStats.moving;
		}
		mStats.nodes = (int)out.nodes.size();
		mStats.boxes = (int)out.boxes.size();
		// objects gone for a while: their poses go
		if ((mFrame & 1023) == 0)
			for (auto it = mStates.begin(); it != mStates.end();)
				it = (mFrame - it->second.seen > 600) ? mStates.erase(it) : std::next(it);
		if (!mLoggedDefs && mStats.objects > 0)
		{
			mLoggedDefs = true;
			size_t withTris = 0, boxOnly = 0, tris = 0;
			for (const auto& [t, d] : mDefs) { withTris += !d.tris.empty(); boxOnly += d.tris.empty() && !d.boxes.empty(); tris += d.tris.size() / 3; }
			PLOG_INFO << "Halo 2 TAA objects: first frame on this map - " << mStats.objects << " tracked objects (" << mStats.interpolated << " with interpolated poses), "
				<< mDefs.size() << " definitions of objects that moved so far (" << withTris << " with collision triangles, " << tris << " triangles in all; " << boxOnly << " with a box only)";
		}
	}

	bool nodeDelta(const float* cur, const float* prev, double out[12])
	{
		if (!cur || !prev || !saneNode(cur) || !saneNode(prev)) return false;
		Aff inv;
		if (!invert(fromNode(cur), inv)) return false;
		const Aff d = compose(fromNode(prev), inv);   // now -> last frame
		for (int i = 0; i < 12; ++i) out[i] = d.m[i];
		return true;
	}
}
