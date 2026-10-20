#include "pch.h"
#include "H2CV_Viewer.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include "H2CV_TimKeys.h"
#include "H2CV_ViewerUtil.h"
#include "H2CV_Engine.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace h2cv
{
	using h2::Vec3;
	using namespace vu;

	const char* occlusionName(Occlusion o)
	{
		switch (o)
		{
		case Occlusion::InFrame: return "in-frame, game depth - bloom OFF while shown (Cartographer style)";
		case Occlusion::PostBloom: return "after bloom, game depth copy + gun mask (bloom kept)";
		case Occlusion::Own: return "own collision depth (at Present)";
		case Occlusion::XRay: return "X-ray (no occlusion)";
		default: return "?";
		}
	}

	namespace
	{
		// what the ghost view is showing for one local user: the vehicle the unit is seated in, else the biped
		struct GhostSubject
		{
			bool ok = false, vehicle = false;
			uintptr_t obj = 0, ent = 0;     // object whose Havok body is shown (the vehicle, or the unit itself)
			std::string name, detail;
			h2::Motion m; h2::PairInfo pair; h2::BodyShape body; h2::Xf pose;
		};

		// Vehicle subject. Any seat counts (driver, passenger, gunner): the vehicle's body desyncs the same way whoever sits
		// in it. Picks the first DYNAMIC rigid body of the vehicle's havok component that has a (body, level) pair, trying the
		// primary record (comp+0x18) first; falls back to the primary dynamic body without a pair. sj.ok stays false (and
		// `why` says why) when anything is unreadable -- the caller then shows the biped.
		void buildVehicleSubject(uintptr_t unit, GhostSubject& sj, std::string& why)
		{
			uint32_t vdatum = 0;
			const uintptr_t veh = h2::vehicleOfUnit(unit, &vdatum);
			if (!veh) return;
			uint32_t grp = 0;
			const uint32_t tag = h2::objectDefinitionTag(veh, &grp);
			char fc[5] = { (char)(grp >> 24), (char)(grp >> 16), (char)(grp >> 8), (char)grp, 0 };
			for (int i = 0; i < 4; ++i) if (fc[i] < 32 || fc[i] > 126) fc[i] = '?';
			char nm[128];
			snprintf(nm, sizeof(nm), "vehicle [tag %04X '%s', object %08X]", tag & 0xFFFF, fc, vdatum);
			sj.name = nm;
			uintptr_t bodies[8];
			const int nb = h2::havokBodies(veh, bodies);
			if (nb <= 0) { why = "seated in a vehicle, but it has no Havok rigid body"; return; }
			int pick = -1, dyn = 0; h2::Motion pm; h2::PairInfo pp;
			for (int i = 0; i < nb; ++i)
			{
				h2::Motion m;
				if (!h2::readMotion(bodies[i], m, true)) continue;
				++dyn;
				const h2::PairInfo pr = h2::findBspPair(bodies[i]);
				if (pick < 0 || (pr.found && !pp.found)) { pick = i; pm = m; pp = pr; }
				if (pp.found) break;
			}
			if (pick < 0) { why = "seated in a vehicle, but none of its " + std::to_string(nb) + " Havok bodies has a live (non-fixed) motion"; return; }
			if (!h2::readBodyShape(bodies[pick], pp.found ? pp.agent : 0, sj.body)) { why = "seated in a vehicle, but its shape tree has no readable primitive (" + sj.body.desc + ")"; return; }
			sj.ok = true; sj.vehicle = true; sj.obj = veh; sj.ent = bodies[pick];
			sj.m = pm; sj.pair = pp; sj.pose = h2::motionPose(pm);
			char dt[200];
			snprintf(dt, sizeof(dt), "%s motion, body %d of %d (%d dynamic), %zu primitives; shape: %s", h2::motionKindName(pm.kind), pick, nb, dyn, sj.body.prims.size(), sj.body.desc.c_str());
			sj.detail = dt;
		}

		void buildBipedSubject(uintptr_t unit, GhostSubject& sj, std::string& note)
		{
			const uintptr_t ent = h2::havokEntityForObject(unit);
			if (!ent) { note = "no dynamic Havok body"; return; }
			h2::Motion m;
			if (!h2::readMotion(ent, m)) { note = "motion not a live stabilized-box (asleep / dead / seated in a vehicle that cannot be shown)"; return; }
			const h2::Capsule cap = h2::entityCapsuleLocal(ent);
			if (!cap.ok) { note = "capsule unreadable"; return; }
			const h2::PairInfo pair = h2::findBspPair(ent);
			h2::ShapePrim p; p.capsule = true; p.a = cap.a; p.b = cap.b; p.r = cap.r; p.hkType = 7; p.agent = pair.found ? pair.agent : 0;
			sj.body = h2::BodyShape{}; sj.body.prims.push_back(p);
			sj.ok = true; sj.vehicle = false; sj.obj = unit; sj.ent = ent; sj.name = "biped";
			sj.m = m; sj.pair = pair; sj.pose = h2::motionPose(m);
		}

		// ---- Believed poses - objects ---------------------------------------------------------------------------------
		// Gap between a subject primitive (body-local, at sPose) and an object primitive already in WORLD space.
		// Polytope objects: each face ring against the subject (primSurfaceDist), minus the object's convex radius; a subject
		// whose centre lies inside every face plane counts as touching. Capsule / sphere objects: exact against a capsule,
		// segment-vs-face against a polytope subject.
		float primPrimDist(const h2::ShapePrim& s, const h2::Xf& sPose, const h2::ShapePrim& o)
		{
			static const h2::Xf I{};
			std::vector<Vec3> ring;
			if (o.capsule)
			{
				if (s.capsule) return std::sqrt(segSegDist2(xfp(sPose, s.a), xfp(sPose, s.b), o.a, o.b)) - s.r - o.r;
				std::vector<Vec3> w(s.verts.size());
				for (size_t i = 0; i < w.size(); ++i) w[i] = xfp(sPose, s.verts[i]);
				float best = 1e9f;
				for (const auto& f : s.faces)
				{
					ring.clear();
					for (uint16_t vi : f.ring) if (vi < w.size()) ring.push_back(w[vi]);
					if (ring.size() >= 3) best = std::min(best, segPolyDist(o.a, o.b, ring));
				}
				return best - s.r - o.r;
			}
			const Vec3 sc = primCentre(s, sPose), oc = primCentre(o, I);
			float best = 1e9f; bool inside = !o.faces.empty();
			for (const auto& f : o.faces)
			{
				ring.clear();
				for (uint16_t vi : f.ring) if (vi < o.verts.size()) ring.push_back(o.verts[vi]);
				if (ring.size() < 3) continue;
				Vec3 n = norm(cross(sub(ring[1], ring[0]), sub(ring[2], ring[0])));
				if (dot(n, sub(ring[0], oc)) < 0) n = mul(n, -1.f);
				if (dot(sub(sc, ring[0]), n) > 0) inside = false;
				best = std::min(best, primSurfaceDist(s, sPose, ring));
			}
			if (inside) return -(s.r + o.r);
			return best - o.r;
		}

		// one skipped (TIM > 0, no contact) child against an object
		struct ObjHit
		{
			uint64_t id = 0;                 // stable per episode: object datum, source, leaf, key
			uint32_t datum = 0xFFFFFFFF; uint8_t type = 0xFF;
			int source = 0;                  // 0 rigid-body pair, 1 baked physics model (key cat 3), 2 baked collision model (cat 4)
			bool bspPoly = false;            // true: a collision_bsp polygon (poly); false: a convex primitive (prim)
			uint32_t key = 0;
			h2::Child k;
			float trueDist = 0, over = 0; bool stale = false, suspect = false;
			Vec3 dir{}, anchor{};
			std::vector<Vec3> poly;          // world
			h2::ShapePrim prim;              // world
			// travel region inputs
			Vec3 objMn{}, objMx{}; bool haveObjBox = false;     // whole object (all leaves), world
			Vec3 qmn{}, qmx{}; bool haveQ = false;              // baked: the LEVEL welder's stored query box at scan time
			uintptr_t innerWelder = 0; h2::Xf innerXf; Vec3 polyLocMn{}, polyLocMx{};   // collision-bsp leaf: its own welder
			uintptr_t body = 0;                                 // rigid-body pair: the object's rigid body (broadphase box)
		};
		struct ObjScan
		{
			int pairs = 0, baked = 0, bakedFail = 0, leaves = 0, tims = 0, skipped = 0, suspect = 0, unsupported = 0;
			std::string unsupportedWhat, bakedWhy;
			std::vector<ObjHit> hits;
			Vec3 qmn{}, qmx{}; bool haveQ = false;              // set by the caller before scanning baked scenery
		};
		inline const char* objSourceName(int s) { return s == 1 ? "baked_phmo" : s == 2 ? "baked_coll" : "rigid_body_pair"; }
		inline const char* objTypeName(uint8_t t)
		{
			switch (t)
			{
			case h2::Biped: return "biped"; case h2::Vehicle: return "vehicle"; case h2::Weapon: return "weapon"; case h2::Equipment: return "equipment";
			case h2::Scenery: return "scenery"; case h2::Machine: return "machine"; case h2::Crate: return "crate"; default: return "object";
			}
		}

		// Every TIM-carrying leaf of an object's shape tree (walked in parallel with the agent tree, prims / bsps in WORLD space)
		// measured against the subject primitives `subj` (body-local, at `pose`). Only skipped children produce hits; that is
		// the state the believed pose describes (same rule and over-credit formula as the level children in drawGhost).
		void scanObjectTree(const h2::BodyShape& tree, const std::vector<const h2::ShapePrim*>& subj, const h2::Xf& pose, const Vec3& mid,
			uint32_t datum, uint8_t type, int source, float overMin, std::unordered_map<uintptr_t, CollBsp>& bspCache, ObjScan& sc)
		{
			constexpr float kTol = 0.0328f;
			static const h2::Xf I{};
			if (subj.empty()) return;
			auto finish = [&](ObjHit& h, float gap)
			{
				h.trueDist = gap - 0.01f;
				h.over = h.trueDist > -0.5f ? h.k.tim - (h.trueDist - 1.1f * kTol) : 0.f;
				h.stale = h.trueDist < kTol;
				h.suspect = h.over > overMin || h.stale;
				if (h.suspect) ++sc.suspect;
				h.datum = datum; h.type = type; h.source = source;
			};
			auto makeId = [&](uint32_t leaf, uint32_t key)
			{
				return ((uint64_t)(datum & 0xFFFF) << 22) | ((uint64_t)(source & 3) << 20) | ((uint64_t)(leaf & 0xF) << 16) | (uint64_t)(key & 0xFFFF);
			};
			// whole-object bounds (world), for the travel region's lifetime box
			Vec3 omn{ 1e30f, 1e30f, 1e30f }, omx{ -1e30f, -1e30f, -1e30f };
			auto grow = [&](const Vec3& p, float r)
			{
				omn = { std::min(omn.x, p.x - r), std::min(omn.y, p.y - r), std::min(omn.z, p.z - r) };
				omx = { std::max(omx.x, p.x + r), std::max(omx.y, p.y + r), std::max(omx.z, p.z + r) };
			};
			for (const auto& pr : tree.prims)
			{
				if (pr.capsule) { grow(pr.a, pr.r); grow(pr.b, pr.r); }
				else for (const Vec3& v : pr.wire) grow(v, 0.f);
			}
			for (const auto& bl : tree.bsps)
			{
				auto it = bspCache.find(bl.bsp);
				if (it == bspCache.end())
				{
					CollBsp cb; h2::TagPools p;
					if (!(h2::tagPools(p) && cb.parse(bl.bsp, p))) cb = CollBsp{};
					it = bspCache.emplace(bl.bsp, std::move(cb)).first;
				}
				for (const Vec3& v : it->second.verts) grow(h2::xfPoint(bl.xf, v), 0.f);
			}
			const bool haveObjBox = omx.x >= omn.x && omx.x - omn.x < 500.f && omx.y - omn.y < 500.f && omx.z - omn.z < 500.f;
			auto stamp = [&](ObjHit& h)
			{
				h.objMn = omn; h.objMx = omx; h.haveObjBox = haveObjBox;
				h.qmn = sc.qmn; h.qmx = sc.qmx; h.haveQ = sc.haveQ && source != 0;
			};
			// convex leaves (sweep / GSK agents), primitives that share an agent (multi-sphere) measured together
			std::vector<uintptr_t> done;
			for (size_t i = 0; i < tree.prims.size(); ++i)
			{
				const h2::ShapePrim& pr = tree.prims[i];
				if (!pr.agent || pr.agent == h2::base() + h2::vt::NULL_AGENT_INST) continue;   // no agent / filtered-out pair
				if (h2::agentKind(pr.agent) != h2::AgentKind::Convex)
				{
					++sc.unsupported;
					const char* nm = h2::agentClassName(pr.agent);
					if (sc.unsupportedWhat.find(nm) == std::string::npos && sc.unsupportedWhat.size() < 120)
						sc.unsupportedWhat += (sc.unsupportedWhat.empty() ? "" : ", ") + std::string(nm);
					continue;
				}
				if (std::find(done.begin(), done.end(), pr.agent) != done.end()) continue;
				done.push_back(pr.agent);
				++sc.leaves;
				h2::Child k;
				if (!h2::readConvexAgent(pr.agent, k)) continue;
				++sc.tims;
				if (!(k.tim > 0 && k.manifold == 0)) continue;
				++sc.skipped;
				float gap = 1e9f; const h2::ShapePrim* best = &pr;
				for (size_t j = i; j < tree.prims.size(); ++j)
				{
					if (tree.prims[j].agent != pr.agent) continue;
					for (const h2::ShapePrim* s : subj)
					{
						const float d = primPrimDist(*s, pose, tree.prims[j]);
						if (d < gap) { gap = d; best = &tree.prims[j]; }
					}
				}
				if (!(gap < 1e8f)) continue;
				ObjHit h; h.k = k; h.key = (uint32_t)i; h.prim = *best; h.bspPoly = false;
				finish(h, gap);
				h.anchor = primCentre(*best, I);
				const Vec3 away = sub(mid, h.anchor);
				Vec3 n = len(k.normal) > 0.5f ? norm(k.normal) : norm(away);
				if (dot(n, away) < 0) n = mul(n, -1.f);                     // away from the object, on the subject's side
				h.dir = n;
				h.id = makeId(0xF, (uint32_t)i);
				stamp(h);
				sc.hits.push_back(std::move(h));
			}
			// collision-bsp leaves: a welder whose children are keyed by surface index of that bsp
			std::vector<Vec3> local;
			for (size_t li = 0; li < tree.bsps.size(); ++li)
			{
				const h2::BspLeaf& bl = tree.bsps[li];
				++sc.leaves;
				auto it = bspCache.find(bl.bsp);
				if (it == bspCache.end())
				{
					CollBsp cb; h2::TagPools p;
					if (!(h2::tagPools(p) && cb.parse(bl.bsp, p))) cb = CollBsp{};   // empty = unreadable, not retried until the bsp changes
					it = bspCache.emplace(bl.bsp, std::move(cb)).first;
				}
				const CollBsp& cb = it->second;
				if (cb.surfaces.empty()) { sc.bakedWhy = "object collision bsp unreadable"; continue; }
				std::vector<h2::Child> kids;
				if (!h2::readChildren(bl.agent, kids)) continue;
				for (const auto& k : kids)
				{
					if (k.kind != h2::Child::Sweep && k.kind != h2::Child::Gsk) continue;
					++sc.tims;
					if (!(k.tim > 0 && k.manifold == 0)) continue;
					++sc.skipped;
					if (!cb.polygon(k.key, local) || local.size() < 3) continue;
					ObjHit h; h.k = k; h.key = k.key; h.bspPoly = true;
					h.poly.reserve(local.size());
					for (const Vec3& v : local) h.poly.push_back(h2::xfPoint(bl.xf, v));
					float gap = 1e9f;
					for (const h2::ShapePrim* s : subj) gap = std::min(gap, primSurfaceDist(*s, pose, h.poly));
					finish(h, gap);
					Vec3 c{ 0, 0, 0 }; for (const Vec3& v : h.poly) c = add(c, v);
					h.anchor = mul(c, 1.f / (float)h.poly.size());
					Vec3 n = norm(cross(sub(h.poly[1], h.poly[0]), sub(h.poly[2], h.poly[0])));
					const float side = dot(sub(mid, h.poly[0]), n);
					if (std::fabs(side) < 1e-6f) n = norm(k.normal); else if (side < 0) n = mul(n, -1.f);
					h.dir = n;
					h.id = makeId((uint32_t)li, k.key);
					stamp(h);
					h.innerWelder = bl.agent; h.innerXf = bl.xf;
					h.polyLocMn = local[0]; h.polyLocMx = local[0];
					for (const Vec3& v : local)
					{
						h.polyLocMn = { std::min(h.polyLocMn.x, v.x), std::min(h.polyLocMn.y, v.y), std::min(h.polyLocMn.z, v.z) };
						h.polyLocMx = { std::max(h.polyLocMx.x, v.x), std::max(h.polyLocMx.y, v.y), std::max(h.polyLocMx.z, v.z) };
					}
					sc.hits.push_back(std::move(h));
				}
			}
		}

	}

	bool Viewer::initDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx)
	{
		// HCM owns the device, context and ImGui; the viewer only borrows them
		if (!dev || !ctx) return false;
		if (!mR.ready() && !mR.init(dev)) return false;
		mDev = dev; mCtx = ctx;
		LOGF("device %p ready", (void*)mDev);
		return true;
	}

	void Viewer::beforeResize() { mRtv = nullptr; }

	void Viewer::shutdown()
	{
		restoreBloom();
		mR.shutdown();
		mRtv = nullptr; mCtx = nullptr; mDev = nullptr;
	}

	void Viewer::drawCollision(const h2::Vec3& camPos, bool depthTest)
	{
		const float R = settings.nearRadius.load(), R2 = R * R;
		const bool own = depthTest, game = false;
		const h2::Camera cam = [&] { h2::Camera c; c.pos = camPos; return c; }();
		// translucent faces first (Cartographer's barrier / kill-trigger fill), depth-tested like the lines
		if (settings.fills)
		{
			auto fill = [&](const std::vector<Tri>& tris, uint32_t rgb, float opacity)
			{
				if (tris.empty() || opacity <= 0.f) return;
				const uint32_t col = (rgb & 0x00FFFFFFu) | ((uint32_t)(std::clamp(opacity, 0.f, 1.f) * 255.f) << 24);
				mScratch.clear();
				for (const auto& t : tris)
				{
					if (pointSegDist2(cam.pos, t.a, t.b) > R2 && pointSegDist2(cam.pos, t.b, t.c) > R2) continue;
					mScratch.push_back({ t.a.x, t.a.y, t.a.z, col });
					mScratch.push_back({ t.b.x, t.b.y, t.b.z, col });
					mScratch.push_back({ t.c.x, t.c.y, t.c.z, col });
				}
				mR.drawTris(mScratch.data(), mScratch.size(), own);
			};
			if (settings.show[(int)Cat::BspInvisible] || settings.show[(int)Cat::InstancedInvisible])
				fill(mWorld.barrierFills(), kCatColor[(int)Cat::BspInvisible], settings.barrierOpacity);
			if (settings.show[(int)Cat::KillTrigger])
				fill(mWorld.killTriggerFills(), kCatColor[(int)Cat::KillTrigger], settings.killOpacity);
		}
		mScratch.clear();
		for (int c = 0; c < kCatCount; ++c)
		{
			if (!settings.show[c]) continue;
			mWorld.forNear((Cat)c, cam.pos, R, [&](const Line& l)
			{
				if (pointSegDist2(cam.pos, l.a, l.b) > R2) return;
				addLine(mScratch, l.a, l.b, kCatColor[c]);
			});
		}
		mR.drawLines(mScratch.data(), mScratch.size(), settings.lineWidth, settings.pull, own, game, settings.hiddenAlpha);

		// desyncable creases: concave edges whose two faces meet at < 90 degrees (yellow = just under 90, red = very
		// acute), thick, over the normal lines; optional thin outline of both faces
		if (settings.creases)
		{
			auto heat = [](float sharp, uint8_t a) {
				const float t = std::clamp(sharp / 0.7f, 0.f, 1.f);              // nA.nB = -0.7 (~135 deg between normals) and sharper = red
				return rgba(255, (uint8_t)(230 - 200 * t), 0, a); };
			if (settings.creaseFaces)
			{
				mScratch.clear();
				for (const auto& l : mWorld.creaseFaces())
				{
					if (pointSegDist2(cam.pos, l.a, l.b) > R2) continue;
					addLine(mScratch, l.a, l.b, rgba(255, 200, 0, 150));
				}
				mR.drawLines(mScratch.data(), mScratch.size(), settings.lineWidth + 0.5f, settings.pull, own, game, settings.hiddenAlpha);
			}
			mScratch.clear();
			for (const auto& c : mWorld.creases())
			{
				if (pointSegDist2(cam.pos, c.edge.a, c.edge.b) > R2) continue;
				addLine(mScratch, c.edge.a, c.edge.b, heat(c.sharp, 255));
			}
			mR.drawLines(mScratch.data(), mScratch.size(), settings.lineWidth + 3.0f, settings.pull, own, game, settings.hiddenAlpha);
		}
	}

	void Viewer::drawPills()
	{
		std::vector<h2::ObjectRef> objs;
		h2::enumerateObjects(objs);
		const uintptr_t me = h2::localPlayerUnitObject();
		uintptr_t locals[4] = {};
		if (settings.ghost) h2::localPlayerUnits(locals);   // drawn (state-coloured) by drawGhost instead
		mScratch.clear();
		for (const auto& o : objs)
		{
			if (o.type != h2::Biped) continue;
			if (settings.ghost && (o.obj == locals[0] || o.obj == locals[1] || o.obj == locals[2] || o.obj == locals[3])) continue;
			const uintptr_t ent = h2::havokEntityForObject(o.obj);
			if (!ent) continue;
			const h2::Capsule c = h2::entityCapsule(ent);
			if (!c.ok) continue;
			capsuleWire(mScratch, c, o.obj == me ? rgba(255, 230, 0) : rgba(255, 255, 255, 200));
		}
		// pills are drawn through walls (like Cartographer's), thin
		mR.drawLines(mScratch.data(), mScratch.size(), 1.5f, 0.f, false, false, 0.f);
	}

	// ------------------------------------------------------------------------------------------------
	// GHOSTING DESYNC DEBUG VIEW (notes: Halo2_Ghosting_Analysis.md; read-only).
	// The subject per local user is the VEHICLE the unit is seated in (any seat) when its Havok body can be read, else the biped.
	//   pink/lime/red body = Havok's current pose, coloured by backstep state (capsule for a biped, the vehicle's shape
	//                        primitives -- boxes, convex hulls, spheres, capsules -- for a vehicle)
	//   thin state-coloured body = Havok step-start pose (motion PREV; Havok keeps no previous rotation, so current rotation)
	//   white body = where HALO's object position puts it (only when it differs from Havok)
	//   fading trail = Havok poses of the last N game ticks (with their own rotation)
	//   yellow body + red-orange pre-rewind body = last backstep rewind (target / where it was drawn)
	//   thin lime bodies + line = "believed" poses: where each over-credited TIM thinks the body is
	//   lime/red thick polygon edges = welder children currently skipped (TIM>0) / over-credited
	// ------------------------------------------------------------------------------------------------
	// ---- ghost desync logger ---------------------------------------------------------------------------------------
	// One JSON object per line, written next to the module that contains this code (HCMInternal.dll / h2cv.dll):
	// GhostDesyncLog_<yyyymmdd_hhmmss>.jsonl. Events: backstep (S 0 -> >0), rewind (detected tick rewind),
	// overcredit_start / overcredit_end (a polygon's TIM over-credit episode). Raw data for finding the ground truth.
	namespace
	{
	}

	bool Viewer::logEnsureOpen()
	{
		if (mLogFile) return true;
		if (!mLogPath.empty()) return false;          // opening failed once this session: don't retry every frame
		HMODULE self = nullptr;
		wchar_t dir[MAX_PATH] = {};
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)&vu::jv, &self) && GetModuleFileNameW(self, dir, MAX_PATH))
		{
			if (wchar_t* slash = wcsrchr(dir, L'\\')) slash[1] = 0;
		}
		SYSTEMTIME st; GetLocalTime(&st);
		wchar_t path[MAX_PATH + 64];
		swprintf(path, MAX_PATH + 64, L"%sGhostDesyncLog_%04d%02d%02d_%02d%02d%02d.jsonl", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		char narrow[MAX_PATH * 2] = {};
		WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
		mLogPath = narrow;
		if (_wfopen_s(&mLogFile, path, L"ab") != 0) { mLogFile = nullptr; LOGF("ghost logger: could not open %s", narrow); return false; }
		LOGF("ghost logger: writing %s", narrow);
		mLogLines = 0;
		return true;
	}

	void Viewer::logClose()
	{
		if (mLogFile)
		{
			// close every open episode so the file never ends mid-episode
			for (auto& e : mLogActive)
			{
				std::string r = "{\"event\":\"overcredit_end\",\"reason\":\"logger_off\",\"key64\":" + std::to_string(e.first) +
					",\"peak_over\":" + jf(e.second.peakOver) + "}\n";
				fwrite(r.data(), 1, r.size(), mLogFile);
			}
			fclose(mLogFile);
			LOGF("ghost logger: closed %s (%u lines)", mLogPath.c_str(), mLogLines);
		}
		mLogFile = nullptr; mLogPath.clear(); mLogActive.clear();
	}

	void Viewer::logEvent(const std::string& json)
	{
		if (!mLogFile) return;
		fwrite(json.data(), 1, json.size(), mLogFile);
		fputc('\n', mLogFile);
		fflush(mLogFile);
		++mLogLines;
	}

	void Viewer::drawGhost()
	{
		const uint32_t kLime = rgba(170, 255, 0), kRed = rgba(255, 40, 40);
		// user-selectable capsule colours (state colours of the real capsule + trail, and the believed poses)
		const uint32_t kPink = settings.ghostColNormal.load(), kDesync = settings.ghostColDesync.load(), kBelieved = settings.ghostColBelieved.load();
		const uint32_t kHalo = rgba(255, 255, 255), kRewindTo = rgba(255, 230, 0), kRewindFrom = rgba(255, 90, 0);
		const uint32_t kBelievedObj = rgba(255, 160, 0);   // (f2) object believed poses: orange, so they never read as level ones
		constexpr float kTol = 0.0328f;
		// object collision bsps live in tag data, which moves on every map / bsp load
		if (mObjBspSbsp != mWorld.sbspId()) { mObjBsp.clear(); mObjBspSbsp = mWorld.sbspId(); }
		// rigid body -> owning object, built at most once per frame and only when an object pair needs it
		bool entObjBuilt = false;
		std::vector<std::pair<uintptr_t, h2::ObjectRef>> entObj;
		auto objectOf = [&](uintptr_t ent, h2::ObjectRef& out) -> bool
		{
			if (!entObjBuilt)
			{
				entObjBuilt = true;
				std::vector<h2::ObjectRef> objs;
				h2::enumerateObjects(objs);
				for (const auto& o : objs)
				{
					if (o.hdrFlags & 0x10) continue;
					uintptr_t bodies[8];
					const int nb = h2::havokBodies(o.obj, bodies);
					for (int i = 0; i < nb; ++i) entObj.push_back({ bodies[i], o });
				}
			}
			for (const auto& e : entObj) if (e.first == ent) { out = e.second; return true; }
			return false;
		};

		const uint32_t tick = h2::gameTick();
		uintptr_t units[4];
		h2::localPlayerUnits(units);
		const int trailLen = std::clamp(settings.ghostTrailLen.load(), 0, 60);
		const uint32_t hold = (uint32_t)std::clamp(settings.ghostHold.load(), 0, 120);
		const int belMax = std::clamp(settings.ghostBelievedMax.load(), 0, 32);
		const float overMin = settings.ghostOverMin, prevMin = settings.ghostPrevMinDelta, haloMin = settings.ghostHaloMinDelta;

		std::vector<LineVertex> capsV, thinV, edgeLime, edgeRed, travelFill, passV;
		// pass-through prediction: flashing green
		const float passPulse = 0.35f + 0.65f * (0.5f + 0.5f * std::sin((float)(GetTickCount64() % 100000ull) * 0.009f));
		const uint32_t kPass = rgba(32, 255, 64, (uint8_t)(255.f * passPulse));
		std::vector<Vec3> poly;
		auto withA = [](uint32_t c, uint8_t a) { return (c & 0x00FFFFFFu) | ((uint32_t)a << 24); };
		const Vec3 zero{ 0, 0, 0 };
		++mLogFrame;
		mPassSide.erase(std::remove_if(mPassSide.begin(), mPassSide.end(), [&](const auto& e) { return mLogFrame - e.second.second > 2; }), mPassSide.end());
		if (!settings.ghostLog && mLogFile) logClose();
		if (!settings.ghostLog) mLogPath.clear();          // re-enabling opens a fresh file
		const bool logOn = settings.ghostLog && logEnsureOpen();
		const std::string lvl = logOn ? eng::levelCode() : std::string();

		for (int u = 0; u < 4; ++u)
		{
			GhostInfo& gi = mGI[u];
			gi = GhostInfo{}; gi.user = u;
			GhostPlayer& gp = mGP[u];
			const uintptr_t unit = units[u];
			if (!unit) { gp = GhostPlayer{}; continue; }
			gi.valid = true;

			// ---- the subject: the vehicle the unit is riding in, else its biped -------------------------------
			GhostSubject sj;
			std::string vehWhy;
			if (settings.ghostVehicles) buildVehicleSubject(unit, sj, vehWhy);
			if (!sj.ok)
			{
				sj = GhostSubject{};
				std::string bipedWhy;
				buildBipedSubject(unit, sj, bipedWhy);
				if (!sj.ok)
				{
					gp = GhostPlayer{};
					gi.subject = "biped";
					gi.note = vehWhy.empty() ? bipedWhy : (vehWhy + "; biped: " + bipedWhy);
					continue;
				}
				gi.detail = vehWhy.empty() ? std::string() : (vehWhy + " -- showing the biped");
			}
			else gi.detail = sj.detail;
			gi.subject = sj.name; gi.vehicle = sj.vehicle; gi.nPrims = (int)sj.body.prims.size();
			const h2::Motion& m = sj.m;
			const h2::PairInfo& pair = sj.pair;
			const h2::BodyShape& body = sj.body;
			const h2::Xf& pose = sj.pose;
			bool newRewind = false;
			std::vector<std::pair<uint32_t, Vec3>> contacts;          // touching children (manifold > 0): key, cached normal
			std::vector<std::pair<uint64_t, std::string>> pendStart;  // over-credit episodes that started this frame
			auto head = [&](const char* ev) {
				return std::string("{\"event\":\"") + ev + "\",\"engine\":\"halo2\",\"tick\":" + std::to_string(tick) + ",\"map\":\"" + lvl +
					"\",\"user\":" + std::to_string(u) + ",\"subject\":\"" + (sj.vehicle ? "vehicle" : "biped") + "\",\"cur\":" + jv(m.cur) +
					",\"prev\":" + jv(m.prev) + ",\"v\":" + jv(m.v) + ",\"vint\":" + jv(m.vint) + ",\"S\":" + std::to_string(pair.found ? pair.S : 0); };
			if (gp.obj != sj.obj) { gp = GhostPlayer{}; gp.obj = sj.obj; }
			// anchor of the link lines and of the "which side of the polygon" test: the biped's capsule midpoint, a vehicle's centre of mass
			const Vec3 mid = sj.vehicle ? m.cur : primCentre(body.prims[0], pose);
			gi.pairFound = pair.found; gi.welder = pair.agentIsWelder; gi.S = pair.found ? pair.S : 0;
			if (pair.found && pair.S > 0) { gp.lastBackstepTick = tick; gp.everBackstep = true; }

			// ---- once per game tick: trail sample + rewind detection --------------------------------------
			// motion time (+0x8C) is 1.0 after integrate (sub_1809E05F0 end: `a1[8].m128_i32[3] = 1065353216`); anything else = mid-step
			if (std::fabs(m.time - 1.f) < 1e-4f && tick != 0 && tick != gp.lastTick)
			{
				if (gp.have && tick < gp.lastTick) { gp.trail.clear(); gp.have = false; gp.haveRewind = false; }
				GhostSample sm; sm.tick = tick; sm.cur = m.cur; sm.prev = m.prev; sm.S = gi.S; sm.pose = pose;
				if (gp.have && tick == gp.lastTick + 1 && !gp.trail.empty())
				{
					// integrate does PREV := CUR, so after a clean tick PREV_k == CUR_(k-1) exactly. A backstep setTime
					// (sub_1809DFEB0) moves CUR to P(0)+t*dt*vInt before that copy -> PREV_k = rewind target M.
					const GhostSample& p = gp.trail.back();
					const Vec3 d = sub(m.prev, p.cur);
					const float dist = len(d);
					if (dist > 1e-4f && dist < 1.0f)   // > 1 wu is a teleport, not a rewind
					{
						const Vec3 D = sub(p.cur, p.prev);   // the step the previous tick integrated: A -> B
						const float D2 = dot(D, D);
						gp.haveRewind = true; gp.rewindTick = tick; gp.rewindFrom = p.cur; gp.rewindTo = m.prev; gp.rewindDist = dist;
						gp.tStar = D2 > 1e-10f ? dot(sub(m.prev, p.prev), D) / D2 : 0.f;
						newRewind = true;
						gp.lastBackstepTick = tick; gp.everBackstep = true;
					}
				}
				gp.trail.push_back(sm);
				while (gp.trail.size() > 64) gp.trail.pop_front();
				gp.lastTick = tick; gp.have = true;
			}
			const bool holdActive = gp.everBackstep && tick >= gp.lastBackstepTick && tick - gp.lastBackstepTick <= hold;
			const bool rewindActive = gp.haveRewind && tick >= gp.rewindTick && tick - gp.rewindTick <= hold;
			gi.hold = holdActive;
			if (gp.haveRewind) { gi.rewindDist = gp.rewindDist; gi.tStar = gp.tStar; gi.rewindAge = (int)(tick - gp.rewindTick); }

			// ---- welder children of the (body, level) pair ---------------------------------------------------
			// One welder agent per primitive (a vehicle's list / transform agents fan out to one welder per convex leaf; the
			// biped's pair agent IS the welder). Primitives that share one agent (the spheres of a multi-sphere) are one group
			// and the true gap is the smallest of theirs.
			struct Bel { float over; Vec3 dir; };
			std::vector<Bel> bel, belObj;
			struct Travel { uint32_t key = 0; Vec3 n{}, ref{}, omn{}, omx{}, pmn{}, pmx{}; float tim = 0, over = 0; bool haveZone = false; std::vector<Vec3> poly; };
			std::vector<Travel> travel;
			struct Group { uintptr_t agent; std::vector<const h2::ShapePrim*> prims; };
			std::vector<Group> groups;
			for (const auto& pr : body.prims)
			{
				if (h2::agentKind(pr.agent) != h2::AgentKind::Welder) continue;
				bool found = false;
				for (auto& g : groups) if (g.agent == pr.agent) { g.prims.push_back(&pr); found = true; break; }
				if (!found) groups.push_back({ pr.agent, { &pr } });
			}
			gi.nWelders = (int)groups.size();
			if (!pair.found) gi.note = std::string("no (") + (sj.vehicle ? "vehicle" : "player") + ", level) pair";
			else if (groups.empty())
			{
				if (!sj.vehicle) gi.note = "pair agent is not the welder (inactive / rebuilt)";
				else if (pair.kind == h2::AgentKind::Unknown) gi.note = "vehicle pair agent is not a list / transform / MOPP / welder agent (rebuilt or unsupported?): no TIM data";
				else gi.note = "vehicle pair agent tree has no welder leaf: no TIM data";
			}
			for (const auto& grp : groups)
			{
				std::vector<h2::Child> kids;
				if (!h2::readChildren(grp.agent, kids)) { gi.note = "children unreadable"; continue; }
				const Vec3 gMid = primCentre(*grp.prims[0], pose);
				Vec3 qmn{}, qmx{};
				const bool haveQ = settings.ghostTravel && h2::readWelderQueryBox(grp.agent, qmn, qmx);
				for (const auto& k : kids)
				{
					if (k.kind != h2::Child::Sweep && k.kind != h2::Child::Gsk) continue;
					++gi.nChildren;
					const bool skipping = k.tim > 0 && k.manifold == 0;
					if (logOn && k.manifold > 0) contacts.push_back({ k.key, k.normal });
					if (!skipping) continue;   // only skipped children matter below (and the polygon lookup is the costly part)
					++gi.nSkipped;
					const KeyInfo ki = decodeKey(k.key);
					bool have = false;
					if (ki.kind == KeyInfo::WorldPolygon) have = mWorld.worldPolygon(ki.surface, poly);
					else if (ki.kind == KeyInfo::IgPolygon) have = mWorld.igPolygon(ki.instance, ki.surface, poly);
					if (!have || poly.size() < 3) continue;
					// same measure as drawTim(): true gap between the primitive's surface and the polygon surface
					float gap = 1e9f;
					for (const h2::ShapePrim* pr : grp.prims) gap = std::min(gap, primSurfaceDist(*pr, pose, poly));
					const float trueDist = gap - 0.01f;
					const float over = trueDist > -0.5f ? k.tim - (trueDist - 1.1f * kTol) : 0.f;
					const bool stale = trueDist < kTol;                 // GSK would report a contact, the cache still skips it
					const bool suspect = over > overMin || stale;
					if (suspect) ++gi.nSuspect;
					gi.maxOver = std::max(gi.maxOver, over);
					if (logOn && over > overMin)
					{
						const uint64_t key64 = ((uint64_t)u << 40) | ((uint64_t)(sj.vehicle ? 1 : 0) << 39) | (uint64_t)k.key;
						auto it = std::find_if(mLogActive.begin(), mLogActive.end(), [&](const auto& e) { return e.first == key64; });
						if (it != mLogActive.end()) { it->second.peakOver = std::max(it->second.peakOver, over); it->second.lastFrame = mLogFrame; }
						else
						{
							Vec3 n = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
							const float side = dot(sub(sj.vehicle ? gMid : mid, poly[0]), n);
							if (side < 0) n = mul(n, -1.f);                          // the believed shift direction: away from the polygon, on the body's side
							Vec3 c{}; for (const auto& v : poly) c = add(c, v); c = mul(c, 1.f / (float)poly.size());
							std::string pv = "["; for (size_t i = 0; i < poly.size(); ++i) { if (i) pv += ","; pv += jv(poly[i]); } pv += "]";
							std::string sf;
							if (ki.kind == KeyInfo::WorldPolygon && ki.surface < mWorld.worldBsp().surfaces.size())
							{ const auto& su = mWorld.worldBsp().surfaces[ki.surface]; sf = ",\"flags\":" + std::to_string(su.flags) + ",\"material\":" + std::to_string(su.material) + ",\"breakable\":" + std::to_string(su.breakable); }
							const Vec3 step = sub(m.cur, m.prev);
							std::string rec = head("overcredit_start") + ",\"key\":" + std::to_string(k.key) + ",\"key64\":" + std::to_string(key64) +
								",\"poly_kind\":\"" + (ki.kind == KeyInfo::WorldPolygon ? "world" : ki.kind == KeyInfo::IgPolygon ? "instanced" : "other") +
								"\",\"instance\":" + std::to_string(ki.instance) + ",\"surface\":" + std::to_string(ki.surface) + sf +
								",\"child_kind\":\"" + (k.kind == h2::Child::Sweep ? "sweep" : "gsk") + "\",\"poly\":" + pv + ",\"centroid\":" + jv(c) +
								",\"normal\":" + jv(n) + ",\"cached_normal\":" + jv(k.normal) + ",\"tim\":" + jf(k.tim) + ",\"true_dist\":" + jf(trueDist) +
								",\"over\":" + jf(over) + ",\"believed_shift\":" + jv(mul(n, over)) + ",\"n_dot_vint\":" + jf(dot(n, m.vint)) +
								",\"n_dot_v\":" + jf(dot(n, m.v)) + ",\"n_dot_step\":" + jf(dot(n, step)) + ",\"stale\":" + (stale ? "true" : "false");
							pendStart.push_back({ key64, rec });
							LogState ls; ls.startTick = tick; ls.lastFrame = mLogFrame; ls.peakOver = over;
							mLogActive.push_back({ key64, ls });
						}
					}
					if (settings.ghostEdges && (suspect || !settings.ghostEdgesSuspectOnly))
					{
						auto& dst = suspect ? edgeRed : edgeLime;
						for (size_t i = 0; i < poly.size(); ++i) addLine(dst, poly[i], poly[(i + 1) % poly.size()], suspect ? kRed : kLime);
					}
					// ---- pass-through prediction ----
					// The TIM is spent linearly: moving the centre by dp costs n . dp, whatever the path. Passing through
					// needs the capsule fully over this polygon (its closest point q pulled inside by the radius, so the
					// polygon you stand on / next to no longer holds you) and fully past its plane (moved through by the
					// capsule's half-extent along the polygon normal), with TIM still left:
					//     target = q_in - pn * (r + |core . pn| / 2)    needed = n . (centre - target)    margin = TIM - needed
					if (settings.ghostPass && !sj.vehicle && over > overMin && !body.prims.empty() && body.prims[0].capsule)
					{
						const h2::ShapePrim& cp = body.prims[0];
						const Vec3 ca = xfp(pose, cp.a), cb = xfp(pose, cp.b);
						Vec3 c{}; for (const auto& v : poly) c = add(c, v); c = mul(c, 1.f / (float)poly.size());
						Vec3 pn = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
						// side decided once per TIM and kept while it lives (re-deciding while passing through would flip it)
						if (passSide(((uint64_t)u << 40) | (uint64_t)k.key, dot(pn, sub(mid, c)) >= 0) < 0) pn = mul(pn, -1.f);
						Vec3 n = len(k.normal) > 0.5f ? norm(k.normal) : pn;
						if (dot(n, pn) < 0) n = mul(n, -1.f);
						const Vec3 q = closestPointPoly(mid, poly);
						Vec3 inward = sub(c, q); inward = sub(inward, mul(pn, dot(inward, pn)));
						const float il = len(inward);
						const Vec3 qIn = il > 1e-4f ? add(q, mul(inward, std::min(cp.r, il) / il)) : q;
						const float half = cp.r + 0.5f * std::fabs(dot(sub(ca, cb), pn));
						const Vec3 target = sub(qIn, mul(pn, half));
						const float needed = dot(n, sub(mid, target));
						const float margin = k.tim - needed;
						if (margin > 0.f)
						{
							++gi.nPassLevel; gi.passBest = std::max(gi.passBest, margin);
							for (size_t i = 0; i < poly.size(); ++i) addLine(passV, poly[i], poly[(i + 1) % poly.size()], kPass);
						}
						if (suspect && gi.passRows.size() < 6)
						{
							char row[220];
							snprintf(row, sizeof(row), "surf %u: TIM %.3f  needed %.3f (over-credit needed %.3f, have %.3f)  n.pn %.2f  margin %+.3f  -> %s",
								ki.surface, k.tim, needed, needed - (k.tim - over), over, dot(n, pn), margin, margin > 0.f ? "PASS" : "short");
							gi.passRows.push_back(row);
						}
					}
					if (settings.ghostTravel && over > overMin && travel.size() < 6)
					{
						// GSK processCollision (sub_18098E3A0) while TIM > 0:  tim -= n.(dB - dA) + wA + wB  (agent +0x30 normal,
						// +0x3C tim; dA/dB = each body's integrated step from its motion state, w = their per-step extra terms),
						// then skips the exact test while tim stays > 0. The polygon does not move (dB = 0), so apart from the w
						// terms the TIM is tim0 + n.(x - x0): it only drains when the body moves toward the polygon along n, and
						// the limit is a PLANE, path-independent. The child (and its TIM) also only lives while the welder query
						// box overlaps the polygon's bounds (sub_1809A9AC0), which is the zone box.
						const Vec3 ref = sj.vehicle ? gMid : mid;
						Vec3 pn = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
						if (dot(sub(ref, poly[0]), pn) < 0) pn = mul(pn, -1.f);
						Vec3 n = len(k.normal) > 0.5f ? norm(k.normal) : pn;
						if (dot(n, pn) < 0) n = mul(n, -1.f);
						Travel tr; tr.key = k.key; tr.n = n; tr.tim = k.tim; tr.over = over; tr.ref = ref; tr.poly = poly;
						if (haveQ)
						{
							// the polygon's box (fixed to it) and the welder's stored query box (moves with the body)
							Vec3 pmn = poly[0], pmx = poly[0];
							for (const Vec3& v : poly) { pmn = { std::min(pmn.x, v.x), std::min(pmn.y, v.y), std::min(pmn.z, v.z) }; pmx = { std::max(pmx.x, v.x), std::max(pmx.y, v.y), std::max(pmx.z, v.z) }; }
							tr.omn = pmn; tr.omx = pmx; tr.pmn = qmn; tr.pmx = qmx; tr.haveZone = true;
						}
						travel.push_back(std::move(tr));
					}
					if (over > overMin && belMax > 0)
					{
						Vec3 n = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
						const float side = dot(sub(sj.vehicle ? gMid : mid, poly[0]), n);
						if (std::fabs(side) < 1e-6f) n = norm(k.normal); else if (side < 0) n = mul(n, -1.f);   // away from the polygon, on the body's side
						bel.push_back({ over, n });
					}
				}
			}
			// ---- objects: baked scenery children of the level welder(s) + pairs against other rigid bodies ----------------
			ObjScan objScan;
			if (settings.ghostBelievedObjects || settings.ghostTravelObjects)
			{
				ObjScan& sc = objScan;
				gi.objScanned = true;
				// (1) key categories 3 / 4 in the subject's level welder: the child is a transform agent around the scenery's
				//     own shape (physics model rigid body or collision-model welder), see h2::bakedSceneryShape
				for (const auto& grp : groups)
				{
					std::vector<h2::Child> kids;
					if (!h2::readChildren(grp.agent, kids)) continue;
					const Vec3 gMid = sj.vehicle ? primCentre(*grp.prims[0], pose) : mid;
					sc.haveQ = h2::readWelderQueryBox(grp.agent, sc.qmn, sc.qmx);
					for (const auto& k : kids)
					{
						const int cat = (int)(k.key >> 29);
						if ((cat != 3 && cat != 4) || k.kind == h2::Child::Null) continue;   // null agent = pair filtered out, nothing to collide
						++sc.baked;
						h2::BakedShape bs;
						if (!h2::bakedSceneryShape(k.key, bs)) { ++sc.bakedFail; sc.bakedWhy = "baked scenery: " + bs.why; continue; }
						const uintptr_t rootAgent = h2::agentKind(k.agent) == h2::AgentKind::Transform ? mem::q(k.agent + 0x18) : k.agent;
						h2::BodyShape tree;
						if (!h2::readShapeTree(bs.shape, rootAgent, bs.xf, tree)) { ++sc.bakedFail; sc.bakedWhy = "baked scenery shape unreadable (" + tree.desc + ")"; continue; }
						scanObjectTree(tree, grp.prims, pose, gMid, bs.datum, bs.type, cat == 3 ? 1 : 2, overMin, mObjBsp, sc);
					}
				}
				// (2) the biped's pairs against other rigid bodies. A vehicle subject is skipped: when both bodies are compound
				//     the agent tree nests the two shape trees and cannot be matched to the object's shape alone.
				if (!sj.vehicle)
				{
					std::vector<h2::ObjPair> pairs;
					h2::findObjectPairs(sj.ent, pairs);
					std::vector<const h2::ShapePrim*> subjPrims;
					for (const auto& pr : body.prims) subjPrims.push_back(&pr);
					for (const auto& op : pairs)
					{
						h2::ObjectRef o{};
						const bool haveObj = objectOf(op.other, o);
						if (haveObj && o.obj == sj.obj) continue;            // another body of the subject itself
						++sc.pairs;
						// one diagnostic row per pair: what it is, its agent, its shape, and how many TIMs came out of it
						char drow[240];
						auto pairRow = [&](const char* state, const h2::BodyShape* tree, int tims)
						{
							if (gi.objPairRows.size() >= 8) return;
							snprintf(drow, sizeof(drow), "%s %08X: agent %s, shape %s -> %s%s",
								haveObj ? objTypeName(o.type) : "body", haveObj ? o.datum : 0u, h2::agentClassName(op.agent),
								tree ? tree->desc.c_str() : "?", state, tims >= 0 ? (" " + std::to_string(tims) + " TIMs").c_str() : "");
							gi.objPairRows.push_back(drow);
						};
						h2::Xf opose;
						if (!h2::entityPose(op.other, opose)) { pairRow("pose unreadable", nullptr, -1); continue; }
						h2::BodyShape tree;
						if (!h2::readShapeTree(mem::q(op.other + 0x18), op.agent, opose, tree)) { pairRow("shape walk found nothing", &tree, -1); continue; }
						const size_t before = sc.hits.size();
						const int timsBefore = sc.tims;
						scanObjectTree(tree, subjPrims, pose, mid, haveObj ? o.datum : 0xFFFFFFFF, haveObj ? o.type : 0xFF, 0, overMin, mObjBsp, sc);
						for (size_t hi = before; hi < sc.hits.size(); ++hi) sc.hits[hi].body = op.other;
						pairRow("ok,", &tree, sc.tims - timsBefore);
					}
				}
				// machines near you: do they have Havok bodies at all, and is one of them paired with you? (a tram / elevator
				// that never shows up in the pair rows above is explained here)
				{
					std::vector<h2::ObjectRef> all;
					h2::enumerateObjects(all);
					std::vector<h2::ObjPair> myPairs;
					if (!sj.vehicle) h2::findObjectPairs(sj.ent, myPairs);
					struct Near { float d; std::string row; };
					std::vector<Near> nearMach;   // (not "near": windows.h defines near / far as empty macros)
					for (const auto& o : all)
					{
						if (o.type != h2::Machine || (o.hdrFlags & 0x10)) continue;
						float m13[13];
						if (!h2::objectNodeMatrix(o.obj, 0, m13)) continue;
						const float d = len(sub(Vec3{ m13[10], m13[11], m13[12] }, mid));
						if (d > 25.f) continue;
						uintptr_t bodies[8];
						const int nb = h2::havokBodies(o.obj, bodies);
						int paired = 0;
						for (int i = 0; i < nb; ++i)
							for (const auto& p : myPairs) if (p.other == bodies[i]) { ++paired; break; }
						char r[200];
						snprintf(r, sizeof(r), "machine %08X at %.1f wu: %d Havok bod%s, %d paired with you%s", o.datum, d, nb, nb == 1 ? "y" : "ies", paired,
							nb == 0 ? "  (no rigid body: its collision is not Havok's, so it has no TIMs)" : "");
						nearMach.push_back({ d, r });
					}
					std::sort(nearMach.begin(), nearMach.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
					for (size_t i = 0; i < nearMach.size() && i < 6; ++i) gi.machineRows.push_back(nearMach[i].row);
				}
				gi.nObjPairs = sc.pairs; gi.nObjBaked = sc.baked; gi.nObjBakedFail = sc.bakedFail; gi.nObjLeaves = sc.leaves;
				gi.nObjTims = sc.tims; gi.nObjSkipped = sc.skipped; gi.nObjSuspect = sc.suspect; gi.nObjUnsupported = sc.unsupported;
				if (!sc.unsupportedWhat.empty()) gi.objNote = std::to_string(sc.unsupported) + " leaf agent(s) without readable TIMs: " + sc.unsupportedWhat;
				if (!sc.bakedWhy.empty()) gi.objNote += (gi.objNote.empty() ? "" : "; ") + sc.bakedWhy;
				for (const auto& h : sc.hits)
				{
					gi.maxObjOver = std::max(gi.maxObjOver, h.over);
					if (settings.ghostEdges && (h.suspect || !settings.ghostEdgesSuspectOnly))
					{
						auto& dst = h.suspect ? edgeRed : edgeLime;
						const uint32_t col = h.suspect ? kRed : kLime;
						if (h.bspPoly) { for (size_t i = 0; i < h.poly.size(); ++i) addLine(dst, h.poly[i], h.poly[(i + 1) % h.poly.size()], col); }
						else { static const h2::Xf I{}; primWire(dst, h.prim, I, zero, col); }
					}
					if (h.over > overMin && belMax > 0) belObj.push_back({ h.over, h.dir });
					// pass-through prediction against this object piece: a collision_bsp polygon uses the polygon rule;
					// a convex piece needs the centre past the piece's middle along the TIM normal (TIM is spent only along n)
					if (settings.ghostPass && !sj.vehicle && h.over > overMin && h.k.tim > 0.f && h.k.manifold == 0 && !body.prims.empty() && body.prims[0].capsule)
					{
						const h2::ShapePrim& cp = body.prims[0];
						const Vec3 ca = xfp(pose, cp.a), cb = xfp(pose, cp.b);
						float margin = -1.f;
						if (h.bspPoly && h.poly.size() >= 3)
						{
							Vec3 c{}; for (const auto& v : h.poly) c = add(c, v); c = mul(c, 1.f / (float)h.poly.size());
							Vec3 pn = norm(cross(sub(h.poly[1], h.poly[0]), sub(h.poly[2], h.poly[0])));
							if (passSide(((uint64_t)u << 40) | (1ull << 39) | (h.id & ((1ull << 39) - 1)), dot(pn, sub(mid, c)) >= 0) < 0) pn = mul(pn, -1.f);
							Vec3 n = len(h.k.normal) > 0.5f ? norm(h.k.normal) : pn;
							if (dot(n, pn) < 0) n = mul(n, -1.f);
							const Vec3 q = closestPointPoly(mid, h.poly);
							Vec3 inward = sub(c, q); inward = sub(inward, mul(pn, dot(inward, pn)));
							const float il = len(inward);
							const Vec3 qIn = il > 1e-4f ? add(q, mul(inward, std::min(cp.r, il) / il)) : q;
							const float half = cp.r + 0.5f * std::fabs(dot(sub(ca, cb), pn));
							margin = h.k.tim - dot(n, sub(mid, sub(qIn, mul(pn, half))));
						}
						else
						{
							Vec3 n = len(h.k.normal) > 0.5f ? norm(h.k.normal) : h.dir;
							if (passSide(((uint64_t)u << 40) | (1ull << 39) | (h.id & ((1ull << 39) - 1)), dot(n, h.dir) >= 0) < 0) n = mul(n, -1.f);
							float lo = 1e30f, hi = -1e30f;
							if (h.prim.capsule) { for (const Vec3& e : { h.prim.a, h.prim.b }) { lo = std::min(lo, dot(e, n) - h.prim.r); hi = std::max(hi, dot(e, n) + h.prim.r); } }
							else for (const Vec3& v : h.prim.verts) { lo = std::min(lo, dot(v, n)); hi = std::max(hi, dot(v, n)); }
							if (hi >= lo) margin = h.k.tim - (dot(mid, n) - 0.5f * (lo + hi));
						}
						if (margin > 0.f)
						{
							++gi.nPassObj; gi.passBest = std::max(gi.passBest, margin);
							if (h.bspPoly) { for (size_t i = 0; i < h.poly.size(); ++i) addLine(passV, h.poly[i], h.poly[(i + 1) % h.poly.size()], kPass); }
							else { static const h2::Xf I{}; primWire(passV, h.prim, I, zero, kPass); }
						}
					}
					if (logOn && h.over > overMin)
					{
						const uint64_t key64 = ((uint64_t)u << 40) | (1ull << 38) | (h.id & ((1ull << 38) - 1));
						auto it = std::find_if(mLogActive.begin(), mLogActive.end(), [&](const auto& e) { return e.first == key64; });
						if (it != mLogActive.end()) { it->second.peakOver = std::max(it->second.peakOver, h.over); it->second.lastFrame = mLogFrame; }
						else
						{
							std::string geo;
							if (h.bspPoly)
							{
								geo = ",\"leaf\":\"bsp_polygon\",\"surface\":" + std::to_string(h.key) + ",\"poly\":[";
								for (size_t i = 0; i < h.poly.size(); ++i) { if (i) geo += ","; geo += jv(h.poly[i]); }
								geo += "]";
							}
							else geo = std::string(",\"leaf\":\"convex\",\"prim_index\":") + std::to_string(h.key) + ",\"hk_type\":" + std::to_string(h.prim.hkType) +
								",\"child_kind\":\"" + (h.k.kind == h2::Child::Sweep ? "sweep" : "gsk") + "\"";
							std::string rec = head("overcredit_start") + ",\"source\":\"object\",\"object_source\":\"" + objSourceName(h.source) +
								"\",\"object_datum\":" + std::to_string(h.datum) + ",\"object_type\":\"" + objTypeName(h.type) + "\",\"key\":" + std::to_string(h.key) +
								",\"key64\":" + std::to_string(key64) + geo + ",\"centroid\":" + jv(h.anchor) + ",\"normal\":" + jv(h.dir) +
								",\"cached_normal\":" + jv(h.k.normal) + ",\"tim\":" + jf(h.k.tim) + ",\"true_dist\":" + jf(h.trueDist) + ",\"over\":" + jf(h.over) +
								",\"believed_shift\":" + jv(mul(h.dir, h.over)) + ",\"n_dot_vint\":" + jf(dot(h.dir, m.vint)) + ",\"n_dot_v\":" + jf(dot(h.dir, m.v)) +
								",\"stale\":" + (h.stale ? "true" : "false");
							pendStart.push_back({ key64, rec });
							LogState ls; ls.startTick = tick; ls.lastFrame = mLogFrame; ls.peakOver = h.over;
							mLogActive.push_back({ key64, ls });
						}
					}
				}
			}
			if (logOn)
			{
				std::string cj = "[";
				for (size_t i = 0; i < contacts.size(); ++i)
				{
					const KeyInfo ck = decodeKey(contacts[i].first);
					if (i) cj += ",";
					cj += "{\"key\":" + std::to_string(contacts[i].first) + ",\"poly_kind\":\"" + (ck.kind == KeyInfo::WorldPolygon ? "world" : ck.kind == KeyInfo::IgPolygon ? "instanced" : "other") +
						"\",\"instance\":" + std::to_string(ck.instance) + ",\"surface\":" + std::to_string(ck.surface) + ",\"normal\":" + jv(contacts[i].second) + "}";
				}
				cj += "]";
				for (const auto& ps : pendStart) logEvent(ps.second + ",\"contacts\":" + cj + "}");
				if (pair.found && pair.S > 0 && gp.prevS <= 0) logEvent(head("backstep") + ",\"contacts\":" + cj + "}");
				if (newRewind)
					logEvent(head("rewind") + ",\"rewind_from\":" + jv(gp.rewindFrom) + ",\"rewind_to\":" + jv(gp.rewindTo) + ",\"rewind_dist\":" + jf(gp.rewindDist) + ",\"f\":" + jf(1.f - gp.tStar) + ",\"S_after\":" + std::to_string(gi.S) + ",\"step_len\":" + jf(gp.trail.size() >= 2 ? len(sub(gp.trail[gp.trail.size() - 2].cur, gp.trail[gp.trail.size() - 2].prev)) : 0.f) +
						",\"t_star\":" + jf(gp.tStar) + ",\"contacts\":" + cj + "}");
			}
			gp.prevS = pair.found ? pair.S : 0;
			gi.lime = gi.S > 0 || holdActive || gi.nSuspect > 0 || gi.nObjSuspect > 0 || (settings.ghostStrictSkip && (gi.nSkipped > 0 || gi.nObjSkipped > 0));
			const uint32_t st = gi.S == -3 ? kRed : (gi.lime ? kDesync : kPink);
			auto drawBody = [&](std::vector<LineVertex>& dst, const h2::Xf& ps, const Vec3& shift, uint32_t col)
			{
				for (const auto& pr : body.prims) primWire(dst, pr, ps, shift, col);
			};

			// (a) Havok current pose (optional: off shows only the desync poses, without the overlap)
			if (settings.ghostShowReal) drawBody(capsV, pose, zero, st);
			// (b) Havok step-start pose = motion PREV (+0x70) shifted from CUR (+0x80); the previous rotation is not stored, so the current one
			{
				const Vec3 d = sub(m.prev, m.cur);
				gi.prevDelta = len(d);
				if (settings.ghostPrev && gi.prevDelta > prevMin && gi.prevDelta < 5.f) drawBody(thinV, pose, d, withA(st, 120));
			}
			// (c) Halo's object position (obj+0x64); the reference point (body origin +0xC0 vs centre of mass +0x80) is whichever is nearer
			{
				float hp[3];
				if (mem::copy(hp, sj.obj + 0x64, 12) && std::isfinite(hp[0]) && std::isfinite(hp[1]) && std::isfinite(hp[2]))
				{
					const Vec3 P{ hp[0], hp[1], hp[2] };
					gi.dOrigin = len(sub(P, m.origin)); gi.dCom = len(sub(P, m.cur)); gi.comOff = len(m.com);
					gi.haloRefCom = gi.dCom < gi.dOrigin;
					const Vec3 d = sub(P, gi.haloRefCom ? m.cur : m.origin);
					// a vehicle's Halo position is its node-0 origin, which need not be the origin of the body Havok moves (a body
					// attached to another node): a constant offset of more than half a world unit is that mismatch, not a desync
					const float haloMax = sj.vehicle ? 0.5f : 50.f;
					if (settings.ghostHalo && len(d) > haloMin && len(d) < haloMax) drawBody(capsV, pose, d, kHalo);
				}
			}
			// (d) trail of the last N tick poses (the newest sample is the current pose: skipped when identical)
			if (settings.ghostTrail && trailLen > 0)
			{
				const int n = (int)gp.trail.size(), first = std::max(0, n - trailLen - 1);
				for (int i = first; i < n; ++i)
				{
					const GhostSample& sm = gp.trail[i];
					const Vec3 d = sub(sm.cur, m.cur);
					if (len(d) < 5e-4f || len(d) > 20.f) continue;
					const float f = (float)(i - first + 1) / (float)(n - first);
					drawBody(thinV, sm.pose, zero, withA(sm.S > 0 ? kDesync : kPink, (uint8_t)(35 + 150 * f)));
				}
			}
			// (e/g) the last backstep rewind: where it was drawn (B) -> the rewind target (M = PREV after the step)
			if (settings.ghostRewind && rewindActive)
			{
				drawBody(thinV, pose, sub(gp.rewindFrom, m.cur), withA(kRewindFrom, 170));
				drawBody(capsV, pose, sub(gp.rewindTo, m.cur), kRewindTo);
				addLine(thinV, add(gp.rewindFrom, sub(mid, m.cur)), add(gp.rewindTo, sub(mid, m.cur)), kRewindTo);
			}
			// link-line length label at the line's midpoint (the line runs mid -> mid + d, so its length is |d| = the over-credit)
			auto linkLabel = [&](const Vec3& d, uint32_t col)
			{
				if (!settings.ghostLinkLabels) return;
				Label L{}; L.col = col;
				if (!mR.project(add(mid, mul(d, 0.5f)), L.x, L.y)) return;
				L.x *= mLabelScaleX; L.y *= mLabelScaleY;
				snprintf(L.text, sizeof(L.text), "%.4f wu", len(d));
				gLabels.push_back(L);
			};
			// (f) believed poses: each over-credited child "thinks" the body is `over` farther along its normal
			if (settings.ghostBelieved && !bel.empty())
			{
				std::sort(bel.begin(), bel.end(), [](const Bel& x, const Bel& y) { return x.over > y.over; });
				if ((int)bel.size() > belMax) bel.resize((size_t)belMax);
				gi.nBelieved = (int)bel.size();
				for (const auto& b : bel)
				{
					const Vec3 d = mul(b.dir, b.over);
					// full-detail and solid like the real body (the user watches these); the link line stays thin
					drawBody(capsV, pose, d, kBelieved);
					addLine(thinV, add(mid, d), mid, withA(kBelieved, 130));
					linkLabel(d, kBelieved);
				}
			}
			// ---- ghost travel regions -----------------------------------------------------------------------------------
			// A region is where your BODY CENTRE (white cross, attached to your capsule) can be while the TIM survives,
			// fixed to the polygon / object:
			//   lifetime box = the polygon's / object's box grown by your capsule's half-size plus the margin the engine adds:
			//     level polygons and baked scenery: the level welder tests (capsule box + sweep) grown by 1.5 x tolerance;
			//     rigid-body pairs: the broadphase tests both bodies' broadphase boxes (the object's is read directly; yours
			//     is your capsule box plus its measured broadphase margin);
			//     collision-model pieces: also the object's own welder, same 1.5 x tolerance, in the object's frame.
			//   The engine's boxes also stretch along your step's motion (the sweep), which only ADDS reach in the direction
			//   you move; it is left out so the region does not flicker - the drawn region is the guaranteed part.
			//   reset sheet = the polygon pushed along its normal to the depth where your centre makes the TIM reach 0
			//     (n.x = n.ref - tim, sub_18098E3A0): fixed to the wall while you walk.
			auto tCorner = [](const Vec3& a, const Vec3& b, int i) { return Vec3{ (i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y, (i & 4) ? b.z : a.z }; };
			static const int kTE[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
			auto tBox = [&](std::vector<LineVertex>& dst, const Vec3& mn, const Vec3& mx, uint32_t col, const h2::Xf* X)
			{
				for (const auto& e : kTE)
				{
					Vec3 a = tCorner(mn, mx, e[0]), b = tCorner(mn, mx, e[1]);
					if (X) { a = h2::xfPoint(*X, a); b = h2::xfPoint(*X, b); }
					addLine(dst, a, b, col);
				}
			};
			auto tInside = [](const Vec3& p, const Vec3& mn, const Vec3& mx)
			{
				return std::min({ p.x - mn.x, mx.x - p.x, p.y - mn.y, mx.y - p.y, p.z - mn.z, mx.z - p.z });
			};
			auto tSheet = [&](const Vec3& n, float tim, const Vec3& ref, std::vector<Vec3> foot, const Vec3& anchor, float half)
			{
				const Vec3 p0 = sub(ref, mul(n, tim));
				if (foot.size() < 3)
				{
					const Vec3 hv = std::fabs(n.z) < 0.9f ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 };
					const Vec3 u1 = mul(norm(cross(n, hv)), half), u2 = mul(norm(cross(n, norm(cross(n, hv)))), half);
					foot = { add(add(anchor, u1), u2), add(sub(anchor, u1), u2), sub(sub(anchor, u1), u2), sub(add(anchor, u1), u2) };
				}
				for (Vec3& v : foot) v = sub(v, mul(n, dot(n, sub(v, p0))));        // onto the reset plane
				for (size_t j = 0; j < foot.size(); ++j) addLine(edgeRed, foot[j], foot[(j + 1) % foot.size()], rgba(255, 40, 40));
				for (size_t j = 1; j + 1 < foot.size(); ++j)
				{
					const uint32_t f = rgba(255, 40, 40, 45);
					travelFill.push_back({ foot[0].x, foot[0].y, foot[0].z, f });
					travelFill.push_back({ foot[j].x, foot[j].y, foot[j].z, f });
					travelFill.push_back({ foot[j + 1].x, foot[j + 1].y, foot[j + 1].z, f });
				}
				addLine(thinV, ref, p0, withA(rgba(255, 40, 40), 200));          // the remaining toward-the-surface budget
			};
			auto tMargin = [](float m, bool have) -> std::string
			{
				if (!have) return "?";
				if (m < 0) return "OUTSIDE";
				char b[24]; snprintf(b, sizeof(b), "%.4f wu", m); return b;
			};
			// your capsule's box (world) and centre; the cross marks the centre
			Vec3 capMn{ 1e30f, 1e30f, 1e30f }, capMx{ -1e30f, -1e30f, -1e30f };
			for (const auto& pr : body.prims)
			{
				if (pr.capsule)
					for (const Vec3& p : { xfp(pose, pr.a), xfp(pose, pr.b) })
					{
						capMn = { std::min(capMn.x, p.x - pr.r), std::min(capMn.y, p.y - pr.r), std::min(capMn.z, p.z - pr.r) };
						capMx = { std::max(capMx.x, p.x + pr.r), std::max(capMx.y, p.y + pr.r), std::max(capMx.z, p.z + pr.r) };
					}
				else
					for (const Vec3& w : pr.wire)
					{
						const Vec3 p = xfp(pose, w);
						capMn = { std::min(capMn.x, p.x), std::min(capMn.y, p.y), std::min(capMn.z, p.z) };
						capMx = { std::max(capMx.x, p.x), std::max(capMx.y, p.y), std::max(capMx.z, p.z) };
					}
			}
			const bool haveCap = capMx.x >= capMn.x;
			const Vec3 capC = haveCap ? mul(add(capMn, capMx), 0.5f) : mid, capHe = haveCap ? mul(sub(capMx, capMn), 0.5f) : Vec3{};
			const float tol = h2::collisionTolerance();
			gi.tolerance = tol;
			const float wm = 1.5f * tol;                                           // welder margin
			bool drewCross = false;
			auto cross3 = [&]()
			{
				if (drewCross) return;
				drewCross = true;
				const float cs = 0.06f;
				for (const Vec3& ax : { Vec3{ cs, 0, 0 }, Vec3{ 0, cs, 0 }, Vec3{ 0, 0, cs } }) addLine(edgeLime, sub(capC, ax), add(capC, ax), rgba(255, 255, 255));
			};

			// (h) level polygons
			if (settings.ghostTravel && !travel.empty() && haveCap)
			{
				static const uint32_t kHue[6] = { rgba(255, 220, 0), rgba(0, 220, 255), rgba(255, 120, 255), rgba(120, 255, 120), rgba(255, 150, 60), rgba(160, 160, 255) };
				cross3();
				const Vec3 g = add(capHe, Vec3{ wm, wm, wm });
				for (size_t i = 0; i < travel.size(); ++i)
				{
					const Travel& t = travel[i];
					const uint32_t hue = kHue[i % 6];
					float margin = -1.f;
					if (t.haveZone)
					{
						const Vec3 zmn = sub(t.omn, g), zmx = add(t.omx, g);
						tBox(thinV, zmn, zmx, withA(hue, 190), nullptr);
						margin = tInside(capC, zmn, zmx);
					}
					tSheet(t.n, t.tim, capC, t.poly, Vec3{}, 0.f);
					for (size_t j = 0; j < t.poly.size(); ++j) addLine(edgeRed, t.poly[j], t.poly[(j + 1) % t.poly.size()], hue);
					const KeyInfo ki = decodeKey(t.key);
					char row[200];
					snprintf(row, sizeof(row), "#%zu %s surf %u: toward-polygon budget %.4f wu (over-credit %.4f)   zone margin %s",
						i + 1, ki.kind == KeyInfo::IgPolygon ? "IG" : "world", ki.surface, t.tim, t.over, tMargin(margin, t.haveZone).c_str());
					gi.travel.push_back(row);
				}
			}

			// (h2) objects. Rigid-body pair: the pair agent (every leaf, every TIM) lives while the two broadphase boxes overlap,
			// and a moving object drains the TIM too (tim -= n.(dB - dA)). Baked scenery: a level-welder child. Collision-model
			// pieces: also their own welder.
			if (settings.ghostTravelObjects && !objScan.hits.empty() && haveCap)
			{
				static const uint32_t kHueO[6] = { rgba(255, 160, 0), rgba(255, 90, 200), rgba(200, 255, 80), rgba(80, 200, 255), rgba(255, 255, 120), rgba(180, 120, 255) };
				std::vector<const ObjHit*> picks;
				for (const auto& h : objScan.hits) if (h.over > overMin) picks.push_back(&h);
				std::sort(picks.begin(), picks.end(), [](const ObjHit* a, const ObjHit* b) { return a->over > b->over; });
				if (picks.size() > 6) picks.resize(6);
				const Vec3 bpg = add(capHe, sj.vehicle ? Vec3{ 0.5f * tol, 0.5f * tol, 0.5f * tol } : bpRestMargin(u, sj.ent, capMn, capMx));
				if (!picks.empty()) cross3();
				for (size_t i = 0; i < picks.size(); ++i)
				{
					const ObjHit& h = *picks[i];
					const uint32_t hue = kHueO[i % 6];
					float margin = 1e9f; bool have = false; const char* what = "?";
					if (h.source == 0)
					{
						Vec3 omn, omx;
						if (h.body && h2::broadphaseAabb(h.body, omn, omx))
						{
							const Vec3 zmn = sub(omn, bpg), zmx = add(omx, bpg);
							tBox(thinV, zmn, zmx, withA(hue, 190), nullptr);
							margin = std::min(margin, tInside(capC, zmn, zmx)); have = true; what = "broadphase";
						}
					}
					else if (h.haveObjBox)
					{
						const Vec3 g = add(capHe, Vec3{ wm, wm, wm });
						const Vec3 zmn = sub(h.objMn, g), zmx = add(h.objMx, g);
						tBox(thinV, zmn, zmx, withA(hue, 190), nullptr);
						margin = std::min(margin, tInside(capC, zmn, zmx)); have = true; what = "level welder";
					}
					// collision-model piece: its own welder, in the object's frame (your capsule box measured in that frame)
					bool inner = false;
					if (h.innerWelder)
					{
						const h2::Xf& X = h.innerXf;
						auto toLocal = [&](const Vec3& p) { const Vec3 d = sub(p, X.t); return Vec3{ dot(d, X.c[0]), dot(d, X.c[1]), dot(d, X.c[2]) }; };
						Vec3 lmn{ 1e30f, 1e30f, 1e30f }, lmx{ -1e30f, -1e30f, -1e30f };
						for (const auto& pr : body.prims)
						{
							if (!pr.capsule) continue;
							for (const Vec3& p : { toLocal(xfp(pose, pr.a)), toLocal(xfp(pose, pr.b)) })
							{
								lmn = { std::min(lmn.x, p.x - pr.r), std::min(lmn.y, p.y - pr.r), std::min(lmn.z, p.z - pr.r) };
								lmx = { std::max(lmx.x, p.x + pr.r), std::max(lmx.y, p.y + pr.r), std::max(lmx.z, p.z + pr.r) };
							}
						}
						if (lmx.x >= lmn.x)
						{
							const Vec3 lhe = add(mul(sub(lmx, lmn), 0.5f), Vec3{ wm, wm, wm }), lc = mul(add(lmn, lmx), 0.5f);
							const Vec3 zmn = sub(h.polyLocMn, lhe), zmx = add(h.polyLocMx, lhe);
							tBox(thinV, zmn, zmx, withA(hue, 120), &X);
							margin = std::min(margin, tInside(lc, zmn, zmx)); have = true; inner = true;
						}
					}
					tSheet(h.dir, h.k.tim, capC, h.bspPoly ? h.poly : std::vector<Vec3>{}, h.anchor, 0.35f);
					static const h2::Xf I{};
					if (h.bspPoly) { for (size_t j = 0; j < h.poly.size(); ++j) addLine(edgeRed, h.poly[j], h.poly[(j + 1) % h.poly.size()], hue); }
					else primWire(edgeRed, h.prim, I, Vec3{ 0, 0, 0 }, hue);
					char row[240];
					snprintf(row, sizeof(row), "#%zu %s %s %08X (%s): toward-object budget %.4f wu (over-credit %.4f)   zone margin %s [%s%s]",
						i + 1, objTypeName(h.type), h.source == 0 ? "pair" : h.source == 1 ? "baked phmo" : "baked coll", h.datum,
						h.bspPoly ? "polygon" : "convex", h.k.tim, h.over, tMargin(have ? margin : -1.f, have).c_str(), what, inner ? " + own welder" : "");
					gi.travelObj.push_back(row);
				}
			}
			// (f2) believed poses against objects (orange)
			if (settings.ghostBelievedObjects && !belObj.empty())
			{
				std::sort(belObj.begin(), belObj.end(), [](const Bel& x, const Bel& y) { return x.over > y.over; });
				if ((int)belObj.size() > belMax) belObj.resize((size_t)belMax);
				gi.nObjBelieved = (int)belObj.size();
				for (const auto& b : belObj)
				{
					const Vec3 d = mul(b.dir, b.over);
					drawBody(capsV, pose, d, kBelievedObj);
					addLine(thinV, add(mid, d), mid, withA(kBelievedObj, 130));
					linkLabel(d, kBelievedObj);
				}
			}
		}
		// drawn through walls (like the pills): thin/faded ones, then the solid capsules, then the thick polygon edges
		// over-credit episodes not seen this frame have ended (TIM dropped back / polygon left the welder / subject changed)
		if (logOn)
			for (auto it = mLogActive.begin(); it != mLogActive.end();)
			{
				if (it->second.lastFrame == mLogFrame) { ++it; continue; }
				logEvent("{\"event\":\"overcredit_end\",\"tick\":" + std::to_string(tick) + ",\"key64\":" + std::to_string(it->first) +
					",\"key\":" + std::to_string((uint32_t)(it->first & 0xFFFFFFFFu)) + ",\"user\":" + std::to_string((int)(it->first >> 40)) +
					",\"start_tick\":" + std::to_string(it->second.startTick) + ",\"ticks\":" + std::to_string((int64_t)tick - (int64_t)it->second.startTick) +
					",\"peak_over\":" + jf(it->second.peakOver) + "}");
				it = mLogActive.erase(it);
			}

		if (!travelFill.empty()) mR.drawTris(travelFill.data(), travelFill.size(), false);
		mR.drawLines(thinV.data(), thinV.size(), 1.3f, 0.f, false, false, 0.f);
		mR.drawLines(capsV.data(), capsV.size(), 2.5f, 0.f, false, false, 0.f);
		mR.drawLines(edgeLime.data(), edgeLime.size(), settings.lineWidth + 3.0f, 0.f, false, false, 0.f);
		mR.drawLines(edgeRed.data(), edgeRed.size(), settings.lineWidth + 4.5f, 0.f, false, false, 0.f);
		if (!passV.empty()) mR.drawLines(passV.data(), passV.size(), settings.lineWidth + 5.5f, 0.f, false, false, 0.f);
	}

	// ------------------------------------------------------------------------------------------------
	// WELD ZONES (Halo 2, biped). The level welder (hkConvexConvexWelderAgent) does not test every polygon: each step its
	// child update (sub_1809A9AC0) takes the capsule's AABB grown by 1.5 x the collision tolerance, keeps it at agent+0x30,
	// and asks the level MOPP for every polygon whose bounds overlap it. Overlap -> the polygon gets a child (and a TIM);
	// no overlap -> its child is deleted. The box is only re-queried once a face of the fresh box has moved more than
	// 0.5 x tolerance, so the stored box is the one the current children came from.
	// Drawn per polygon / object: its bounds grown by the stored box's half-extents. The player's box overlaps the
	// polygon's exactly when the box CENTRE (the white cross) is inside that zone, so a zone is "where you have to be".
	// Approximations: the MOPP tests its own quantized (slightly larger) bounds, not the exact polygon box, so a polygon can
	// be welded with the cross just outside its zone; rigid-body pairs come from the broadphase, whose boxes are close to
	// but not exactly this one (drawn the same way, marked as objects).
	// ------------------------------------------------------------------------------------------------
	// Your broadphase margin with you standing STILL. Havok stretches the broadphase box by the step's motion, so the margin
	// measured while moving grows with speed; sampling it only at rest gives one fixed number per axis (the largest rest
	// sample, which also covers the grid snapping), so every pair zone keeps one size. Re-measured after a bsp load.
	h2::Vec3 Viewer::bpRestMargin(int u, uintptr_t ent, const h2::Vec3& capMn, const h2::Vec3& capMx)
	{
		if (u < 0 || u > 3) return Vec3{};
		if (mBpMarginSbsp != mWorld.sbspId()) { for (int i = 0; i < 4; ++i) mBpMarginOk[i] = false; mBpMarginSbsp = mWorld.sbspId(); }
		h2::Motion m;
		Vec3 bmn, bmx;
		if (h2::readMotion(ent, m) && len(m.v) < 0.05f && len(sub(m.cur, m.prev)) < 1e-4f && h2::broadphaseAabb(ent, bmn, bmx))
		{
			const float s[3] = { std::min(capMn.x - bmn.x, bmx.x - capMx.x), std::min(capMn.y - bmn.y, bmx.y - capMx.y), std::min(capMn.z - bmn.z, bmx.z - capMx.z) };
			for (int a = 0; a < 3; ++a)
			{
				const float v = std::clamp(s[a], 0.f, 1.f);
				mBpMargin[u][a] = mBpMarginOk[u] ? std::max(mBpMargin[u][a], v) : v;
			}
			mBpMarginOk[u] = true;
		}
		if (!mBpMarginOk[u])
		{
			const float g = 0.5f * h2::collisionTolerance();
			return Vec3{ g, g, g };
		}
		return Vec3{ mBpMargin[u][0], mBpMargin[u][1], mBpMargin[u][2] };
	}

	void Viewer::drawWeldZones()
	{
		mWeld = WeldStats{};
		const bool wantNear = settings.weldZonesNear, wantWelded = settings.weldZonesWelded;
		if (!wantNear && !wantWelded) return;
		const float range = std::clamp(settings.weldZoneRange.load(), 0.f, 20.f);
		constexpr size_t kMaxZones = 600;      // drawn boxes per frame (nearest first)
		constexpr int kMaxNearBodies = 48;     // un-paired rigid bodies measured per frame
		static const h2::Xf I{};
		const uint32_t kWeldPoly = rgba(170, 255, 0), kWeldObj = rgba(255, 160, 0), kNearPoly = rgba(0, 200, 255), kNearObj = rgba(255, 210, 120);

		// every level polygon's box, once per bsp load (only the "near" mode needs the unwelded ones)
		if (wantNear && mWeldPolysSbsp != mWorld.sbspId())
		{
			mWeldPolys.clear();
			mWeldPolysSbsp = mWorld.sbspId();
			std::vector<Vec3> poly;
			auto addPoly = [&](uint32_t key)
			{
				if (poly.size() < 3) return;
				WeldPoly w{ poly[0], poly[0], key };
				for (const Vec3& v : poly)
				{
					w.mn = { std::min(w.mn.x, v.x), std::min(w.mn.y, v.y), std::min(w.mn.z, v.z) };
					w.mx = { std::max(w.mx.x, v.x), std::max(w.mx.y, v.y), std::max(w.mx.z, v.z) };
				}
				mWeldPolys.push_back(w);
			};
			const CollBsp& wb = mWorld.worldBsp();
			for (uint32_t s = 0; s < wb.surfaces.size() && s <= 0xFFFF; ++s)
				if (wb.polygon(s, poly)) addPoly((1u << 29) | s);
			for (uint32_t i = 0; i < mWorld.igInstanceCount() && i <= 0xFFFF; ++i)
			{
				const size_t ns = std::min<size_t>(mWorld.igSurfaceCount(i), 0x2000);
				for (uint32_t s = 0; s < ns; ++s)
					if (mWorld.igPolygon(i, s, poly)) addPoly((5u << 29) | (s << 16) | i);
			}
			LOGF("weld zones: %zu level polygon boxes built", mWeldPolys.size());
		}
		if (mObjBspSbsp != mWorld.sbspId()) { mObjBsp.clear(); mObjBspSbsp = mWorld.sbspId(); }

		auto grow = [](Vec3& mn, Vec3& mx, const Vec3& p, float r)
		{
			mn = { std::min(mn.x, p.x - r), std::min(mn.y, p.y - r), std::min(mn.z, p.z - r) };
			mx = { std::max(mx.x, p.x + r), std::max(mx.y, p.y + r), std::max(mx.z, p.z + r) };
		};
		// world bounds of a shape tree read with a world transform (prims are world-space; bsp leaves need their verts)
		auto treeBounds = [&](const h2::BodyShape& tree, Vec3& mn, Vec3& mx) -> bool
		{
			mn = { 1e30f, 1e30f, 1e30f }; mx = { -1e30f, -1e30f, -1e30f };
			bool any = false;
			for (const auto& pr : tree.prims)
			{
				if (pr.capsule) { grow(mn, mx, pr.a, pr.r); grow(mn, mx, pr.b, pr.r); any = true; }
				else for (const Vec3& v : pr.wire) { grow(mn, mx, v, 0.f); any = true; }
			}
			for (const auto& bl : tree.bsps)
			{
				auto it = mObjBsp.find(bl.bsp);
				if (it == mObjBsp.end())
				{
					CollBsp cb; h2::TagPools p;
					if (!(h2::tagPools(p) && cb.parse(bl.bsp, p))) cb = CollBsp{};
					it = mObjBsp.emplace(bl.bsp, std::move(cb)).first;
				}
				for (const Vec3& v : it->second.verts) { grow(mn, mx, h2::xfPoint(bl.xf, v), 0.f); any = true; }
			}
			return any && mx.x - mn.x < 500.f && mx.y - mn.y < 500.f && mx.z - mn.z < 500.f;
		};
		auto boxDist = [](const Vec3& p, const Vec3& mn, const Vec3& mx)
		{
			const float dx = std::max({ mn.x - p.x, 0.f, p.x - mx.x });
			const float dy = std::max({ mn.y - p.y, 0.f, p.y - mx.y });
			const float dz = std::max({ mn.z - p.z, 0.f, p.z - mx.z });
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};
		auto corner = [](const Vec3& a, const Vec3& b, int i) { return Vec3{ (i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y, (i & 4) ? b.z : a.z }; };
		static const int kEdges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
		static const int kFaces[6][4] = { {0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
		std::vector<LineVertex> fill, edges, marks;
		auto drawBox = [&](const Vec3& a, const Vec3& b, uint32_t col, uint8_t fillA, uint8_t edgeA)
		{
			const uint32_t f = (col & 0x00FFFFFFu) | ((uint32_t)fillA << 24), e = (col & 0x00FFFFFFu) | ((uint32_t)edgeA << 24);
			for (const auto& q : kFaces)
			{
				const Vec3 p0 = corner(a, b, q[0]), p1 = corner(a, b, q[1]), p2 = corner(a, b, q[2]), p3 = corner(a, b, q[3]);
				fill.push_back({ p0.x, p0.y, p0.z, f }); fill.push_back({ p1.x, p1.y, p1.z, f }); fill.push_back({ p2.x, p2.y, p2.z, f });
				fill.push_back({ p0.x, p0.y, p0.z, f }); fill.push_back({ p2.x, p2.y, p2.z, f }); fill.push_back({ p3.x, p3.y, p3.z, f });
			}
			for (const auto& ed : kEdges) addLine(edges, corner(a, b, ed[0]), corner(a, b, ed[1]), e);
		};

		uintptr_t units[4];
		h2::localPlayerUnits(units);
		std::vector<h2::ObjectRef> objs; bool objsRead = false;
		for (int u = 0; u < 4; ++u)
		{
			if (!units[u] || h2::vehicleOfUnit(units[u])) continue;   // biped only: a seated unit has no level welder of its own
			const uintptr_t ent = h2::havokEntityForObject(units[u]);
			if (!ent) { mWeld.note = "no dynamic Havok body"; continue; }
			const h2::PairInfo pair = h2::findBspPair(ent);
			if (!pair.found || pair.kind != h2::AgentKind::Welder) { mWeld.note = "no (player, level) welder right now"; continue; }
			Vec3 qmn, qmx;
			if (!h2::readWelderQueryBox(pair.agent, qmn, qmx)) { mWeld.note = "welder query box unreadable"; continue; }
			// Zones are anchored to the polygon / object and measured against your CAPSULE CENTRE (white cross): a zone is the
			// polygon's / object's box grown by your capsule's half-size plus the engine's margin (welder: 1.5 x tolerance;
			// broadphase: your measured broadphase margin). The engine's boxes also stretch along your motion (sweep), which
			// only adds reach in the direction you move; it is left out so the zones stay still.
			const h2::Capsule cap = h2::entityCapsule(ent);
			if (!cap.ok) { mWeld.note = "capsule unreadable"; continue; }
			const Vec3 capMn{ std::min(cap.a.x, cap.b.x) - cap.r, std::min(cap.a.y, cap.b.y) - cap.r, std::min(cap.a.z, cap.b.z) - cap.r };
			const Vec3 capMx{ std::max(cap.a.x, cap.b.x) + cap.r, std::max(cap.a.y, cap.b.y) + cap.r, std::max(cap.a.z, cap.b.z) + cap.r };
			const Vec3 c = mul(add(capMn, capMx), 0.5f), capHe = mul(sub(capMx, capMn), 0.5f);
			const float wm = 1.5f * h2::collisionTolerance();
			const Vec3 he = add(capHe, Vec3{ wm, wm, wm });
			if (!mWeld.valid) { mWeld.valid = true; mWeld.user = u; mWeld.boxSize = sub(qmx, qmn); }
			{
				const float cs = 0.08f;
				for (const Vec3& ax : { Vec3{ cs, 0, 0 }, Vec3{ 0, cs, 0 }, Vec3{ 0, 0, cs } }) addLine(marks, sub(c, ax), add(c, ax), rgba(255, 255, 255));
			}

			struct Zone { Vec3 mn, mx; float d; bool welded, object; };
			std::vector<Zone> zones;
			auto addZoneG = [&](const Vec3& mn, const Vec3& mx, const Vec3& grow, bool welded, bool object, bool force)
			{
				const Vec3 zmn = sub(mn, grow), zmx = add(mx, grow);
				const float d = boxDist(c, zmn, zmx);
				if (!force && d > range) return;
				zones.push_back({ zmn, zmx, d, welded, object });
				if (welded) { ++mWeld.welded; if (object) ++mWeld.weldedObjects; }
				else if (object) ++mWeld.nearObjects; else ++mWeld.nearPolys;
			};
			auto addZone = [&](const Vec3& mn, const Vec3& mx, bool welded, bool object, bool force) { addZoneG(mn, mx, he, welded, object, force); };

			// (1) children of the level welder: level polygons and baked scenery (key categories 3 / 4)
			std::vector<h2::Child> kids;
			h2::readChildren(pair.agent, kids);
			std::vector<uint32_t> keys;
			std::vector<Vec3> poly;
			for (const auto& k : kids)
			{
				keys.push_back(k.key);
				const KeyInfo ki = decodeKey(k.key);
				bool have = false;
				if (ki.kind == KeyInfo::WorldPolygon) have = mWorld.worldPolygon(ki.surface, poly);
				else if (ki.kind == KeyInfo::IgPolygon) have = mWorld.igPolygon(ki.instance, ki.surface, poly);
				if (have && poly.size() >= 3)
				{
					Vec3 mn = poly[0], mx = poly[0];
					for (const Vec3& v : poly) grow(mn, mx, v, 0.f);
					addZone(mn, mx, true, false, true);
					continue;
				}
				if (ki.category == 3 || ki.category == 4)
				{
					h2::BakedShape bs; h2::BodyShape tree; Vec3 mn, mx;
					if (h2::bakedSceneryShape(k.key, bs) && h2::readShapeTree(bs.shape, 0, bs.xf, tree) && treeBounds(tree, mn, mx))
						addZone(mn, mx, true, true, true);
				}
			}
			std::sort(keys.begin(), keys.end());

			// (2) every other level polygon whose zone is within range of the centre
			if (wantNear)
				for (const auto& w : mWeldPolys)
				{
					if (boxDist(c, sub(w.mn, he), add(w.mx, he)) > range) continue;
					if (std::binary_search(keys.begin(), keys.end(), w.key)) continue;   // already drawn as welded
					addZone(w.mn, w.mx, false, false, false);
				}

			// (3) rigid bodies: a broadphase pair with the biped counts as welded; others within range when "near" is on
			std::vector<h2::ObjPair> pairs;
			h2::findObjectPairs(ent, pairs);
			std::vector<uintptr_t> paired;
			// rigid bodies are paired by the broadphase, not the welder: their zone is the object's broadphase box grown by the
			// biped's broadphase half-extents, shifted into the cross's frame (cross - biped broadphase centre)
			Vec3 bpmn, bpmx;
			const bool haveBp = h2::broadphaseAabb(ent, bpmn, bpmx);
			const Vec3 bpg = add(capHe, bpRestMargin(u, ent, capMn, capMx));
			auto addBodyZone = [&](uintptr_t body, bool welded, bool force)
			{
				Vec3 omn, omx;
				if (haveBp && h2::broadphaseAabb(body, omn, omx))
				{
					addZoneG(omn, omx, bpg, welded, true, force);
					return true;
				}
				h2::Xf pose; h2::BodyShape tree; Vec3 mn, mx;
				if (h2::entityPose(body, pose) && h2::readShapeTree(mem::q(body + 0x18), 0, pose, tree) && treeBounds(tree, mn, mx))
				{
					addZone(mn, mx, welded, true, force);
					return true;
				}
				return false;
			};
			for (const auto& op : pairs)
			{
				paired.push_back(op.other);
				addBodyZone(op.other, true, true);
			}
			if (wantNear)
			{
				if (!objsRead) { h2::enumerateObjects(objs); objsRead = true; }
				int measured = 0;
				for (const auto& o : objs)
				{
					if (measured >= kMaxNearBodies) break;
					if ((o.hdrFlags & 0x10) || o.obj == units[u]) continue;
					uintptr_t bodies[8];
					const int nb = h2::havokBodies(o.obj, bodies);
					for (int i = 0; i < nb && measured < kMaxNearBodies; ++i)
					{
						if (bodies[i] == ent || std::find(paired.begin(), paired.end(), bodies[i]) != paired.end()) continue;
						h2::Xf pose;
						if (!h2::entityPose(bodies[i], pose)) continue;
						if (len(sub(pose.t, c)) > range + 30.f) continue;            // cheap reject before reading the shape
						++measured;
						addBodyZone(bodies[i], false, false);
					}
				}
			}

			// nearest first, capped; welded-only mode keeps just the welded ones
			if (!wantNear) zones.erase(std::remove_if(zones.begin(), zones.end(), [](const Zone& z) { return !z.welded; }), zones.end());
			std::sort(zones.begin(), zones.end(), [](const Zone& a, const Zone& b) { return a.d < b.d; });
			if (zones.size() > kMaxZones) { zones.resize(kMaxZones); mWeld.capped = true; }
			for (const auto& z : zones)
			{
				const uint32_t col = z.welded ? (z.object ? kWeldObj : kWeldPoly) : (z.object ? kNearObj : kNearPoly);
				drawBox(z.mn, z.mx, col, z.welded ? 22 : 12, z.welded ? 150 : 90);
			}
			mWeld.drawn += (int)zones.size();
		}
		// through walls, like the pills: faint fills, then edges, then the player's box on top
		mR.drawTris(fill.data(), fill.size(), false);
		mR.drawLines(edges.data(), edges.size(), 1.2f, 0.f, false, false, 0.f);
		mR.drawLines(marks.data(), marks.size(), 2.0f, 0.f, false, false, 0.f);
		(void)I;
	}

	void Viewer::drawTim()
	{
		mTimRows.clear(); mPairFound = false; mPairWelder = false;
		const uintptr_t obj = h2::localPlayerUnitObject();
		if (!obj) { mStatus = "no local player unit"; return; }
		const uintptr_t ent = h2::havokEntityForObject(obj);
		if (!ent) { mStatus = "player has no dynamic Havok body"; return; }
		const h2::PairInfo pair = h2::findBspPair(ent);
		mPairFound = pair.found; mPairS = pair.S; mPairWelder = pair.agentIsWelder;
		if (!pair.found) { mStatus = "no (player, level) pair"; return; }
		if (!pair.agentIsWelder) { mStatus = "pair agent is not the welder (inactive / rebuilt)"; return; }
		std::vector<h2::Child> kids;
		if (!h2::readChildren(pair.agent, kids)) { mStatus = "children unreadable"; return; }
		mStatus = "ok";
		const h2::Capsule cap = h2::entityCapsule(ent);

		mScratch.clear();
		std::vector<LineVertex> featLines;
		std::vector<Vec3> poly;
		for (const auto& k : kids)
		{
			TimRow row{ k.key, -1, 0, 0, k.tim, k.manifold, (uint8_t)k.kind, -1.f };
			const KeyInfo ki = decodeKey(k.key);
			row.cat = ki.category; row.inst = ki.instance; row.surf = ki.surface;
			bool have = false;
			if (ki.kind == KeyInfo::WorldPolygon) have = mWorld.worldPolygon(ki.surface, poly);
			else if (ki.kind == KeyInfo::IgPolygon) have = mWorld.igPolygon(ki.instance, ki.surface, poly);
			if (have && cap.ok) row.trueDist = segPolyDist(cap.a, cap.b, poly) - cap.r - 0.01f;
			mTimRows.push_back(row);
			if (!have || (k.kind != h2::Child::Sweep && k.kind != h2::Child::Gsk)) continue;

			const bool skipping = k.tim > 0 && k.manifold == 0;
			const float estTrueTim = row.trueDist - 1.1f * 0.0328f;
			const float over = (skipping && row.trueDist > -0.5f) ? k.tim - estTrueTim : 0.f;
			const uint32_t col = timColor(over, skipping, k.manifold);
			// which ring edge / vertex the last GSK run (the one that wrote the TIM) was closest to
			const int n = (int)poly.size();
			int featEdge = -1, featVert = -1;
			if (k.feature == 1 && k.featA >= 0 && k.featB >= 0 && k.featA < n && k.featB < n)
			{
				if (k.featB == (k.featA + 1) % n) featEdge = k.featA;
				else if (k.featA == (k.featB + 1) % n) featEdge = k.featB;
			}
			else if (k.feature == 0 && k.featA >= 0 && k.featA < n) featVert = k.featA;
			for (int i = 0; i < n; ++i)
			{
				const Vec3& a = poly[i]; const Vec3& b = poly[(i + 1) % n];
				addLine(mScratch, a, b, col);
				if (i == featEdge) addLine(featLines, a, b, rgba(255, 255, 255));
				if (!settings.timLabels) continue;
				const Vec3 mid = mul(add(a, b), 0.5f);
				Label L{}; L.col = col;
				if (mR.project(mid, L.x, L.y))
				{
					L.x *= mLabelScaleX; L.y *= mLabelScaleY;
					const char* mark = (i == featEdge) ? " <" : "";
					if (k.manifold > 0) snprintf(L.text, sizeof(L.text), "c%d%s", k.manifold, mark);
					else if (skipping) snprintf(L.text, sizeof(L.text), "%.3f%s%s", k.tim, over > 0.005f ? "!" : "", mark);
					else snprintf(L.text, sizeof(L.text), "%.3f%s", k.tim, mark);
					gLabels.push_back(L);
				}
			}
			if (featVert >= 0)
			{
				const Vec3& v = poly[featVert];
				const float s = 0.03f;
				addLine(featLines, Vec3{ v.x - s, v.y, v.z }, Vec3{ v.x + s, v.y, v.z }, rgba(255, 255, 255));
				addLine(featLines, Vec3{ v.x, v.y - s, v.z }, Vec3{ v.x, v.y + s, v.z }, rgba(255, 255, 255));
				addLine(featLines, Vec3{ v.x, v.y, v.z - s }, Vec3{ v.x, v.y, v.z + s }, rgba(255, 255, 255));
			}
		}
		// the touched / skipped polygons are the point of the overlay: draw them on top, thicker
		mR.drawLines(mScratch.data(), mScratch.size(), settings.lineWidth + 1.5f, 0.f, false, false, 0.f);
		mR.drawLines(featLines.data(), featLines.size(), settings.lineWidth + 4.0f, 0.f, false, false, 0.f);
	}

	void Viewer::drawHud(const h2::Camera& cam)
	{
		// top-right corner: HCM's own window lives at the top-left (10, 25), and this panel - which has grown a lot
		// with the ghost rows - used to sit right on top of its header and collapse arrow
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 12.f, 12.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (ImGui::Begin("Collision Viewer##h2cv", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
		{
			ImGui::Text("occlusion: %s", occlusionName((Occlusion)settings.occlusion.load()));
			if (!mInFrameNote.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "%s", mInFrameNote.c_str());
			ImGui::Text("width %.1fpx  pull %.4f  hidden %.2f  radius %.0f",
				settings.lineWidth.load(), settings.pull.load(), settings.hiddenAlpha.load(), settings.nearRadius.load());
			for (int c = 0; c < (int)Cat::SoftCeiling; ++c)   // soft ceilings are a Halo 3 / ODST category
			{
				const uint32_t col = kCatColor[c];
				ImGui::TextColored(ImVec4((col & 0xFF) / 255.f, ((col >> 8) & 0xFF) / 255.f, ((col >> 16) & 0xFF) / 255.f, 1),
					"%s %s  %zu", settings.show[c] ? "[x]" : "[ ]", catName((Cat)c), mWorld.lines((Cat)c).size());
				if (c % 3 != 2 && c != (int)Cat::SoftCeiling - 1) ImGui::SameLine();
			}
			ImGui::Separator();
			ImGui::Text("TIM overlay %s  labels %s  capsules %s", settings.tim ? "on" : "off",
				settings.timLabels ? "on" : "off", settings.pills ? "on" : "off");
			if (settings.tim)
			{
				ImGui::Text("player<->level pair: %s  S = %d  tick %u  (%s)", mPairFound ? "found" : "-", mPairS, mTick, mStatus.c_str());
				if (!mTimRows.empty() && ImGui::BeginTable("tim", 6, ImGuiTableFlags_SizingFixedFit))
				{
					ImGui::TableSetupColumn("key"); ImGui::TableSetupColumn("poly"); ImGui::TableSetupColumn("TIM");
					ImGui::TableSetupColumn("pts"); ImGui::TableSetupColumn("true d"); ImGui::TableSetupColumn("over");
					ImGui::TableHeadersRow();
					for (const auto& r : mTimRows)
					{
						const bool skip = r.tim > 0 && r.manifold == 0;
						const float over = (skip && r.trueDist > -0.5f) ? r.tim - (r.trueDist - 1.1f * 0.0328f) : 0.f;
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::Text("%08X", r.key);
						ImGui::TableNextColumn();
						if (r.cat == 1) ImGui::Text("bsp %u", r.surf);
						else if (r.cat == 5) ImGui::Text("ig %u/%u", r.inst, r.surf);
						else ImGui::Text("cat %d", r.cat);
						ImGui::TableNextColumn(); ImGui::Text(r.kind == h2::Child::Null ? "null" : "%.4f", r.tim);
						ImGui::TableNextColumn(); ImGui::Text("%d", r.manifold);
						ImGui::TableNextColumn(); if (r.trueDist > -0.5f) ImGui::Text("%.4f", r.trueDist); else ImGui::Text("-");
						ImGui::TableNextColumn();
						if (over > 0.005f) ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "+%.4f", over); else ImGui::Text(skip ? "skip" : "");
					}
					ImGui::EndTable();
				}
			}
			ImGui::Separator();
			ImGui::Text("GHOST debug [Home] %s  believed poses [Shift+Home] %s  objects %s  trail [End] %s  skipped edges [Shift+End] %s",
				settings.ghost ? "on" : "off", settings.ghostBelieved ? "on" : "off", settings.ghostBelievedObjects ? "on" : "off", settings.ghostTrail ? "on" : "off", settings.ghostEdges ? "on" : "off");
			if (settings.ghost)
			{
				auto objectsLine = [](const GhostInfo& g)
				{
					if (!g.objScanned) return;
					ImGui::TextColored(g.nObjSuspect > 0 ? ImVec4(1, 0.63f, 0, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1),
						"    objects: %d body pairs%s, %d baked scenery children (%d unresolved), %d TIM leaves  skipped %d/%d  suspect %d  max over-credit %.4f wu  believed %d",
						g.nObjPairs, g.vehicle ? " (biped only)" : "", g.nObjBaked, g.nObjBakedFail, g.nObjLeaves, g.nObjSkipped, g.nObjTims, g.nObjSuspect, g.maxObjOver, g.nObjBelieved);
					if (!g.objNote.empty()) ImGui::TextDisabled("    objects: %s", g.objNote.c_str());
					for (const auto& r : g.objPairRows) ImGui::TextDisabled("      pair: %s", r.c_str());
					for (const auto& r : g.machineRows) ImGui::TextDisabled("      %s", r.c_str());
				};
				for (const auto& g : mGI)
				{
					if (!g.valid) continue;
					const ImVec4 col = g.S == -3 ? ImVec4(1, 0.2f, 0.2f, 1) : (g.lime ? ImVec4(0.67f, 1, 0, 1) : ImVec4(1, 0.41f, 0.71f, 1));
					if (!g.note.empty() && !g.pairFound)
					{
						ImGui::TextColored(col, "P%d [%s]: %s", g.user, g.subject.c_str(), g.note.c_str());
						if (!g.detail.empty()) ImGui::TextDisabled("    %s", g.detail.c_str());
						objectsLine(g);
						continue;
					}
					ImGui::TextColored(col, "P%d [%s]  S=%d%s  skipped %d/%d  suspect %d  max over-credit %.4f wu  believed poses %d",
						g.user, g.subject.c_str(), g.S, g.hold && g.S <= 0 ? " (backstep hold)" : "", g.nSkipped, g.nChildren, g.nSuspect, g.maxOver, g.nBelieved);
					if (g.vehicle) ImGui::TextDisabled("    vehicle body: %d primitives, %d welder leaves with TIMs", g.nPrims, g.nWelders);
					objectsLine(g);
					ImGui::Text("    Havok-Halo: %.4f wu (vs %s; origin %.4f, COM %.4f, COM offset %.4f)   step-start delta %.4f wu",
						g.haloRefCom ? g.dCom : g.dOrigin, g.haloRefCom ? "COM" : "origin", g.dOrigin, g.dCom, g.comOff, g.prevDelta);
					if (g.rewindAge >= 0) ImGui::Text("    last rewind %.4f wu, t*=%.2f (f = %.2f of the step undone), %d ticks ago   (g = rewind target %s)", g.rewindDist, g.tStar, 1.f - g.tStar, g.rewindAge,
						g.rewindAge <= settings.ghostHold ? "shown" : "expired; g = step-start pose");
					else ImGui::Text("    no rewind seen yet   (g = step-start pose)");
					if (!g.detail.empty()) ImGui::TextDisabled("    %s", g.detail.c_str());
					if (!g.note.empty()) ImGui::TextDisabled("    %s", g.note.c_str());
					if (settings.ghostPass && (g.nPassLevel || g.nPassObj))
						ImGui::TextColored(ImVec4(0.13f, 1.f, 0.25f, 1.f), "    PASS-THROUGH: %d polygon(s), %d object piece(s) - TIM left over by up to %.3f wu", g.nPassLevel, g.nPassObj, g.passBest);
					if (settings.ghostPass) for (const auto& r : g.passRows) ImGui::TextDisabled("      pass test %s", r.c_str());
					if (!g.travel.empty())
					{
						ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "    TRAVEL REGION (level): keep the white cross (your centre) inside each coloured box and short of its red sheet");
						for (const auto& r : g.travel) ImGui::Text("      %s", r.c_str());
					}
					if (!g.travelObj.empty())
					{
						ImGui::TextColored(ImVec4(1, 0.63f, 0, 1), "    TRAVEL REGION (objects): same rules; pairs use the broadphase boxes, tolerance %.4f wu", g.tolerance);
						for (const auto& r : g.travelObj) ImGui::Text("      %s", r.c_str());
					}
				}
				ImGui::TextDisabled("pink = no backstep   LIME = S>0 / hold / stale or over-credited child   RED = S=-3   white = Halo position   yellow = rewind target   red-orange = pre-rewind   orange = believed vs object");
				ImGui::TextDisabled("seated in a vehicle (any seat) the view follows the VEHICLE's Havok body [%s]; on exit it returns to the biped", settings.ghostVehicles ? "on" : "off");
			}
			if (settings.ghost || settings.weldZonesNear || settings.weldZonesWelded)
			{
				h2::LevelGrids lg;
				if (h2::levelGrids(lg))
				{
					ImGui::Separator();
					ImGui::Text("LEVEL GRIDS (how far TIMs can outlive the real geometry on this level)");
					if (lg.tolerance > 0)
						ImGui::Text("    collision tolerance %.4f wu   welder margin (1.5x) %.4f wu   broadphase insert margin (0.5x) %.4f wu",
							lg.tolerance, 1.5f * lg.tolerance, 0.5f * lg.tolerance);
					if (lg.bpOk)
						ImGui::Text("    broadphase: boxes snap OUTWARD to %.4f / %.4f / %.4f wu (x/y/z), range %.0f..%.0f / %.0f..%.0f / %.0f..%.0f",
							lg.bpCell.x, lg.bpCell.y, lg.bpCell.z, lg.bpMin.x, lg.bpMax.x, lg.bpMin.y, lg.bpMax.y, lg.bpMin.z, lg.bpMax.z);
					h2::BackstepParams bp;
					if (h2::backstepParams(bp))
					{
						const float t0 = bp.base, t1 = bp.base + bp.perS, t2 = bp.base + 2.f * bp.perS;
						ImGui::Text("    backstep: a contact deeper than delta(S) = %.4f + S x %.4f wu triggers it  ->  S=0: %.4f  S=1: %.4f  S=2: %.4f wu",
							-bp.base, -bp.perS, -t0, -t1, -t2);
						ImGui::Text("              rewind f is found by halving the step down to %.4f of a step (so f comes in steps of that size)", bp.timeRes);
					}
					if (lg.moppOk)
						ImGui::Text("    level MOPP: scale %.2f per wu -> grid %.4f wu (coarsest) / %.5f wu / %.7f wu (finest), origin %.1f, %.1f, %.1f",
							lg.moppScale, 65536.f / lg.moppScale, 256.f / lg.moppScale, 1.f / lg.moppScale, lg.moppOrigin.x, lg.moppOrigin.y, lg.moppOrigin.z);
					else ImGui::TextDisabled("    level MOPP: not found");
				}
			}
			if (settings.weldZonesNear || settings.weldZonesWelded)
			{
				ImGui::Separator();
				if (!mWeld.valid) ImGui::TextDisabled("WELD ZONES: %s", mWeld.note.empty() ? "no biped on foot" : mWeld.note.c_str());
				else
				{
					ImGui::Text("WELD ZONES (P%d): welder query box %.3f x %.3f x %.3f wu - a polygon / object is welded while the white cross (your centre) is inside its zone", mWeld.user, mWeld.boxSize.x, mWeld.boxSize.y, mWeld.boxSize.z);
					ImGui::Text("    welded %d (%d objects)   near, not welded: %d polygons, %d bodies within %.2f wu   drawn %d%s",
						mWeld.welded, mWeld.weldedObjects, mWeld.nearPolys, mWeld.nearObjects, settings.weldZoneRange.load(), mWeld.drawn, mWeld.capped ? " (capped)" : "");
					ImGui::TextDisabled("    lime = welded polygon   orange = welded object / pair   cyan = near polygon   pale orange = near body");
				}
			}
			ImGui::Separator();
			ImGui::TextDisabled("options: HCM menu > Overlays > Collision Viewer");
		}
		ImGui::End();
		(void)cam;
	}

	namespace
	{
		DirectX::XMFLOAT4X4 gameViewProj(const h2::GameFrame& f, bool reversed)
		{
			// world_to_view = real_matrix4x3 {scale, forward, left, up, position}: view = s*(x*F + y*L + z*U) + P
			// (Cartographer matrix4x3_transform_point), then clip = view * projection (row-vector, standard z)
			const float s = f.w2v[0];
			const DirectX::XMMATRIX W(
				s * f.w2v[1], s * f.w2v[2], s * f.w2v[3], 0,
				s * f.w2v[4], s * f.w2v[5], s * f.w2v[6], 0,
				s * f.w2v[7], s * f.w2v[8], s * f.w2v[9], 0,
				f.w2v[10], f.w2v[11], f.w2v[12], 1);
			const DirectX::XMMATRIX M(f.proj);
			DirectX::XMFLOAT4X4 vp;
			DirectX::XMStoreFloat4x4(&vp, W * M);
			if (reversed) // the depth buffer stores 1 - z/w  ->  clip.z' = clip.w - clip.z
				for (int i = 0; i < 4; ++i) vp.m[i][2] = vp.m[i][3] - vp.m[i][2];
			return vp;
		}

		D3D11_VIEWPORT frameViewport(const h2::GameFrame& f)
		{
			// render_camera viewport_bounds: i16 top, left, bottom, right
			D3D11_VIEWPORT vp{};
			vp.TopLeftX = f.vp[1]; vp.TopLeftY = f.vp[0];
			vp.Width = (float)(f.vp[3] - f.vp[1]); vp.Height = (float)(f.vp[2] - f.vp[0]);
			vp.MinDepth = 0; vp.MaxDepth = 1;
			return vp;
		}

		bool comOk(IUnknown* p)
		{
			// the engine globals hold live COM pointers; make sure one is readable before calling through it
			if (!mem::okp(reinterpret_cast<uintptr_t>(p))) return false;
			const uintptr_t vt = mem::q(reinterpret_cast<uintptr_t>(p));
			return mem::okp(vt) && mem::okp(mem::q(vt));
		}
	}

	void Viewer::setBloomSuppressed(bool off)
	{
		if (isH3()) return;   // the bloom gate is a Halo 2 global
		const uintptr_t gate = h2::base() + 0xE19438; // dword_180E19438: sub_180951EC0 skips bloom (sub_18096FBF0) when non-zero
		if (!h2::base()) return;
		if (off)
		{
			if (!mBloomTouched) { mBloomOrig = mem::u32(gate, 0); mBloomTouched = true; }
			*reinterpret_cast<volatile uint32_t*>(gate) = 1;
		}
		else if (mBloomTouched)
		{
			*reinterpret_cast<volatile uint32_t*>(gate) = mBloomOrig;
			mBloomTouched = false;
		}
	}

	void Viewer::restoreBloom() { setBloomSuppressed(false); }

	bool Viewer::drawViewInto(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, const h2::GameFrame& f, const D3D11_VIEWPORT& vp)
	{
		const bool h3g = isH3();
		// Halo 2: the stored projection is standard-Z while the depth buffer holds 1 - z/w (flip applied in gameViewProj).
		// Halo 3 / ODST: the engine builds the projection for the depth convention in use (reversed when its byte is set) - used as stored.
		const bool reversed = h3g ? h3::depthReversed() : settings.gameZReversed.load();
		const bool wantObjects = settings.show[(int)Cat::Scenery] || settings.show[(int)Cat::Crate] || settings.show[(int)Cat::Machine];
		if (h3g) mWorld.setWantCreases(settings.creases);
		mWorld.update(wantObjects, settings.show[(int)Cat::Breakable]);
		if (!mWorld.valid()) return false;
		mR.setStandardZ(!reversed);
		if (!mR.beginExternal(mCtx, rtv, dsv, vp, gameViewProj(f, h3g ? false : reversed))) return false;
		mR.setStandardZ(!reversed);
		drawCollision(f.pos, true);
		if (h3g)
		{
			if (settings.tim) drawTim3();
			if (settings.pills) drawPills3();
			if (settings.ghost) drawGhost3();
		}
		else
		{
			if (settings.tim) drawTim();
			if (settings.pills) drawPills();
			if (settings.ghost) drawGhost();
			if (settings.weldZonesNear || settings.weldZonesWelded) drawWeldZones();
		}
		mR.end();
		return true;
	}

	void Viewer::onFirstPersonPass()
	{
		if (!mDev || !mCtx || !mR.ready()) return;
		const int occ = settings.occlusion.load();
		if (occ != (int)Occlusion::InFrame) setBloomSuppressed(false);
		if (occ != (int)Occlusion::InFrame && occ != (int)Occlusion::PostBloom) return;
		h2::GameFrame f;
		if (!h2::readFrame(f) || f.textureCamera) return;
		const uintptr_t b = h2::base();
		auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(mem::q(b + h2::rva::SCENE_RTV));
		auto* dsv = reinterpret_cast<ID3D11DepthStencilView*>(mem::q(b + h2::rva::SCENE_DSV));
		if (!comOk(rtv) || !comOk(dsv)) return;
		const D3D11_VIEWPORT vp = frameViewport(f);
		// labels are drawn on the back buffer at Present: scale from scene-target to back-buffer pixels
		{
			ID3D11Resource* res = nullptr; rtv->GetResource(&res);
			ID3D11Texture2D* t = nullptr;
			if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); res->Release(); }
			if (t)
			{
				D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
				mLabelScaleX = (d.Width && mW) ? (float)mW / (float)d.Width : 1.f;
				mLabelScaleY = (d.Height && mH) ? (float)mH / (float)d.Height : 1.f;
			}
		}
		if (occ == (int)Occlusion::InFrame)
		{
			// Cartographer's recipe: draw into the scene target against the live world depth BEFORE the first-person pass
			// (the gun then paints over the lines, the HUD comes later) and switch the bloom stage off while we do.
			setBloomSuppressed(true);
			if (drawViewInto(rtv, dsv, f, vp)) { mInFrameThisFrame = true; ++mInFrameCount; }
		}
		else
		{
			// post-bloom: keep a copy of the complete world depth now; draw after the resolve/bloom
			if (mR.copyWorldDepth(mCtx, dsv)) { mPbFrame = f; mPbVp = vp; mPbPending = true; }
		}
	}

	void Viewer::onResolveDone()
	{
		if (!mPbPending) return;
		mPbPending = false;
		if (settings.occlusion.load() != (int)Occlusion::PostBloom || !mDev || !mCtx) return;
		const uintptr_t b = h2::base();
		auto* dsv = reinterpret_cast<ID3D11DepthStencilView*>(mem::q(b + h2::rva::SCENE_DSV));   // now holds first-person-only depth
		auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(mem::q(b + 0x197EE58));           // swap-chain RTV (composited scene)
		if (!comOk(dsv)) return;
		if (!comOk(rtv)) rtv = mRtv;
		if (!rtv) return;
		if (!mR.copyFpDepthAndMerge(mCtx, dsv, mPbVp, settings.gameZReversed.load())) return;
		// back buffer may be larger than the scene target (resolution scaling): scale the viewport to match
		D3D11_VIEWPORT vp = mPbVp;
		vp.TopLeftX *= mLabelScaleX; vp.Width *= mLabelScaleX; vp.TopLeftY *= mLabelScaleY; vp.Height *= mLabelScaleY;
		if (std::fabs(mLabelScaleX - 1.f) > 0.01f || std::fabs(mLabelScaleY - 1.f) > 0.01f)
		{
			mInFrameNote = "post-bloom: render scale != 100% - depth copy and back buffer differ in size; set Occlusion to InFrame_NoBloom";
			return;
		}
		const float sx = mLabelScaleX, sy = mLabelScaleY;
		mLabelScaleX = mLabelScaleY = 1.f;
		if (drawViewInto(rtv, mR.worldCopyDsv(), mPbFrame, vp)) { mInFrameThisFrame = true; ++mInFrameCount; }
		mLabelScaleX = sx; mLabelScaleY = sy;
	}

	void Viewer::onRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h)
	{
		if (!mDev && !initDevice(dev, ctx)) return;
		if (!rtv || !w || !h) return;
		mRtv = rtv; mW = w; mH = h;

		++mPresentCount;
		mWantSim3 = isH3() && (settings.ghost || settings.tim);
		const bool gameOk = eng::base() && eng::buildOk();
		h2::Camera cam;
		if (gameOk) cam = isH3() ? camera3() : h2::readCamera();
		mTick = gameOk ? eng::gameTick() : 0;
		int occ = settings.occlusion.load();
		if (isH3() && occ == (int)Occlusion::PostBloom) occ = (int)Occlusion::Own;   // there is no post-bloom hook on Halo 3 / ODST
		const bool engineMode = occ == (int)Occlusion::InFrame || occ == (int)Occlusion::PostBloom;
		const bool engineDrew = mInFrameThisFrame;
		mInFrameThisFrame = false;
		if (engineDrew) mLastInFramePresent = mPresentCount;
		if (!engineMode) { mInFrameNote.clear(); setBloomSuppressed(false); }
		else if (!engineDrew && mPresentCount - mLastInFramePresent > 2)
		{
			// the hooks did not draw (remastered graphics, menu, cinematic, texture camera): fall back to own depth
			mInFrameNote = mInFrameCount == 0
				? "render hooks not drawing (remastered graphics? menu?) - showing own collision depth instead"
				: "render hooks idle this frame (cinematic / menu?) - showing own collision depth";
			setBloomSuppressed(false);
		}
		else mInFrameNote.clear();

		if (gameOk && cam.ok && !(engineMode && (engineDrew || mPresentCount - mLastInFramePresent <= 2)))
		{
			const bool own = occ != (int)Occlusion::XRay;
			const bool wantObjects = (own && !isH3()) || settings.show[(int)Cat::Scenery] || settings.show[(int)Cat::Crate] || settings.show[(int)Cat::Machine];
			if (isH3()) mWorld.setWantCreases(settings.creases);
			mWorld.update(wantObjects, settings.show[(int)Cat::Breakable]);
			mLabelScaleX = mLabelScaleY = 1.f;
			if (mWorld.valid() && mR.begin(mCtx, mRtv, mW, mH, cam.pos, cam.fwd, cam.up, cam.vfov, 0.01f))
			{
				if (own)
				{
					mR.clearOwnDepth();
					mR.setStaticOccluders(mWorld.staticOccluders(), mWorld.sbspId() ^ (mWorld.staticOccluders().size() << 1));
					mR.drawStaticOccluders();
					mR.drawOccluders(mWorld.objectOccluders());
					if (settings.bipedOccluders && isH3())
					{
						std::vector<Tri> caps;
						bipedOccluders3(caps, cam.pos);
						mR.drawOccluders(caps);
					}
					else if (settings.bipedOccluders)
					{
						// other bipeds (and the player's own capsule when the camera is outside it, e.g. third person)
						std::vector<Tri> caps;
						std::vector<h2::ObjectRef> objs;
						h2::enumerateObjects(objs);
						for (const auto& o : objs)
						{
							if (o.type != h2::Biped && o.type != h2::Vehicle) continue;
							const uintptr_t e = h2::havokEntityForObject(o.obj);
							if (!e) continue;
							const h2::Capsule c = h2::entityCapsule(e);
							if (!c.ok) continue;
							if (std::sqrt(pointSegDist2(cam.pos, c.a, c.b)) < c.r + 0.05f) continue; // camera inside it
							capsuleTris(caps, c);
						}
						mR.drawOccluders(caps);
					}
				}
				drawCollision(cam.pos, own);
				if (isH3())
				{
					if (settings.tim) drawTim3();
					if (settings.pills) drawPills3();
					if (settings.ghost) drawGhost3();
				}
				else
				{
					if (settings.tim) drawTim();
					if (settings.pills) drawPills();
					if (settings.ghost) drawGhost();
					if (settings.weldZonesNear || settings.weldZonesWelded) drawWeldZones();
				}
				mR.end();
			}
		}

		{
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			for (const auto& L : gLabels)
			{
				const ImVec2 sz = ImGui::CalcTextSize(L.text);
				const ImVec2 p(L.x - sz.x * 0.5f, L.y - sz.y * 0.5f);
				dl->AddRectFilled(ImVec2(p.x - 2, p.y - 1), ImVec2(p.x + sz.x + 2, p.y + sz.y + 1), IM_COL32(0, 0, 0, 160), 3.f);
				dl->AddText(p, L.col | 0xFF000000u, L.text);
			}
			gLabels.clear();
			if (settings.hud)
			{
				if (!eng::base()) { dl->AddText(ImVec2(12, 12), IM_COL32(255, 200, 0, 255), "Collision Viewer: game module not loaded"); }
				else if (!eng::buildOk()) { dl->AddText(ImVec2(12, 12), IM_COL32(255, 60, 60, 255), "Collision Viewer: unsupported game build (needs MCC 1.3528)"); }
				else if (isH3()) drawHud3();
				else drawHud(cam);
			}
		}
	}
}
