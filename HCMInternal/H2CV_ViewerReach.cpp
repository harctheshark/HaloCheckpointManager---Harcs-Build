#include "pch.h"
// Halo: Reach collision viewer path (MCC 1.3528).
//  * Geometry, best source first:
//    1. Poorboy's world file <HCM folder>\h3sb\h3sb_world_<map>.bin (Poorboys Checkpoint Manager, MIT; written by his
//       h3sb.dll): every collision polygon of the map - structure AND instanced geometry - already in world space, tagged
//       with its BSP and its instance (-1 = the structure). Format "H3WD" v1/v2, read like his WorldLinesScene.cpp. When a map
//       has none yet, his mod is asked to run in observe mode (the game untouched) so it writes one.
//    2. Until then: the structure from HCM's Havok debugger (gather-only, the engine's own getChildShape on the engine
//       thread; HavokDebuggerBridge). Its instanced-geometry decode is parked, so this source has no instanced geometry.
//  * Drawing: at Present on the back buffer, i.e. over the finished, bloomed frame - none of the game's bloom on the lines.
//  * Depth: our own collision depth (the polygons near the camera) hides lines behind walls; the game's own depth buffer
//    (surface 9, reversed float) copied each frame is Poorboy's gun mask (PoorboyWorldLines 1.4.0): something of the game's
//    within 2 world units AND clearly in front of a line hides it (the first-person gun and arms).
//  * Only the BSPs the game has loaded are drawn (currentBSPSet).
#include "H2CV_Viewer.h"
#include "H2CV_Reach.h"
#include "H2CV_Engine.h"
#include "H2CV_Log.h"
#include "H2CV_ViewerUtil.h"
#include "HavokDebuggerBridge.h"
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace h2cv
{
	using namespace vu;

	namespace
	{
		constexpr int kMaxReachTris = 2000000;      // = the Havok debugger's MAX_BSP_TRI
		constexpr float kGunMaskRange = 2.f;        // world units (Poorboy's sceneRange)
		constexpr float kOccCell = 64.f;            // occluder cells (the buffer is sorted by cell, drawn by range)
		constexpr uint8_t kAnyBsp = 0xFF;           // drawn whatever BSPs are loaded (the Havok source has no BSP index)

		Vec3 sub3(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		uint64_t ms() { return GetTickCount64(); }

		int cellOf(float v) { return (int)std::floor(v / kOccCell); }
		uint64_t cellKey(int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 0x100000) << 42) | ((uint64_t)(uint32_t)(y + 0x100000) << 21) | (uint64_t)(uint32_t)(z + 0x100000); }

		// polygons in, one flat list: ring[start[i] .. start[i+1]) are polygon i's vertices
		struct PolySoup
		{
			std::vector<Vec3> v;
			std::vector<uint32_t> start;            // size = polygons + 1
			std::vector<uint8_t> cat;               // Cat per polygon
			std::vector<uint8_t> bsp;               // BSP index per polygon (kAnyBsp = always)
			// edges given as they are (the direct read: a collision_bsp's edge table, already unique) - when set, buildScene
			// takes these as the lines and only fans the polygons into occluders (no welding / edge reconstruction)
			bool haveLines = false;
			std::vector<Line> lines;
			std::vector<uint8_t> lineCat, lineBsp;
			size_t count() const { return start.empty() ? 0 : start.size() - 1; }
		};

		// Poorboy's world file -> polygons (WorldLinesScene.cpp's reader: "H3WD", version, polygons, BSPs, instances, map name
		// @20 [32]; from 52: per polygon u32 surface ref, i32 instance (-1 = structure), u8 bsp, u8 flags, i16 material,
		// u16 vertex count, u16 plane, 3 floats normal, (v2: u32 surface index), vertex count x 3 floats)
		bool readWorldFile(const std::filesystem::path& path, PolySoup& out, std::string& err)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f) { err = "cannot open the world file"; return false; }
			std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (d.size() < 52 || std::memcmp(d.data(), "H3WD", 4) != 0) { err = "not a world file of Poorboy's mod"; return false; }
			uint32_t version, npoly;
			std::memcpy(&version, d.data() + 4, 4);
			std::memcpy(&npoly, d.data() + 8, 4);
			if (version != 1 && version != 2) { err = "world file of an unknown version " + std::to_string(version); return false; }
			if (npoly > 50000000) { err = "world file is damaged"; return false; }
			size_t p = 52;
			auto need = [&](size_t n) { return p + n <= d.size(); };
			out.v.reserve((size_t)npoly * 4);
			out.start.reserve((size_t)npoly + 1);
			out.cat.reserve(npoly); out.bsp.reserve(npoly);
			for (uint32_t i = 0; i < npoly; ++i)
			{
				const size_t head = version >= 2 ? 32 : 28;
				if (!need(head)) { err = "world file is still being written (ends early)"; return false; }
				int32_t inst; uint8_t bsp; uint16_t nv;
				std::memcpy(&inst, d.data() + p + 4, 4);
				bsp = (uint8_t)d[p + 8];
				std::memcpy(&nv, d.data() + p + 12, 2);
				p += head;
				if (!need((size_t)nv * 12)) { err = "world file is still being written (ends early)"; return false; }
				// instance indices from 0x4000 up are the map's objects (scenery, machines, crates, Forge) as they stood when
				// the file was written: drawn live from the game instead (pumpReachObjects), so they follow when they move
				if (nv >= 3 && inst < 0x4000)
				{
					out.start.push_back((uint32_t)out.v.size());
					for (uint16_t k = 0; k < nv; ++k)
					{
						float xyz[3];
						std::memcpy(xyz, d.data() + p + (size_t)k * 12, 12);
						out.v.push_back({ xyz[0], xyz[1], xyz[2] });
					}
					out.cat.push_back((uint8_t)(inst >= 0 ? Cat::Instanced : Cat::Bsp));
					out.bsp.push_back(bsp);
				}
				p += (size_t)nv * 12;
			}
			out.start.push_back((uint32_t)out.v.size());
			return true;
		}
	}

	struct Viewer::ReachScene
	{
		std::string map, source;
		bool fromFile = false;
		std::filesystem::file_time_type fileTime{};
		unsigned havokVer = 0;
		std::vector<Line> lines;
		std::vector<uint8_t> lineCat, lineBsp;
		LineGrid grid;
		std::vector<Tri> tris;                  // sorted by (occluder cell, BSP): uploaded ONCE, drawn by range
		std::vector<uint8_t> triBsp;
		struct OccRange { uint32_t first, count; uint8_t bsp; };
		std::vector<OccRange> occRanges;        // runs of one cell and one BSP
		std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> occCells;   // cell -> [first, last) in occRanges
		uint32_t bspMaskAtLoad = 0;             // the game's loaded BSPs when this scene was asked for
		size_t polys = 0, instPolys = 0;
		uint32_t buildMs = 0;
	};

	struct Viewer::ReachLoad
	{
		std::mutex m;
		std::shared_ptr<ReachScene> result;
		std::string err;
		std::atomic<bool> done{ false };
		std::string map;
		bool fromFile = false;
	};

	namespace
	{
		// polygons -> outline edges (an edge is drawn when one polygon has it or when its polygons are not coplanar: the
		// diagonals inside a flat face are left out), fan triangles as occluders, buckets. Vertices welded on 1/256 WU.
		void buildScene(PolySoup& in, Viewer::ReachScene& sc)
		{
			const auto t0 = std::chrono::steady_clock::now();
			const size_t n = in.count();
			if (in.haveLines)
			{
				// Halo 2's way: the lines are the edge table; the occluders are the polygons fanned as they are
				sc.lines.swap(in.lines); sc.lineCat.swap(in.lineCat); sc.lineBsp.swap(in.lineBsp);
				for (size_t i = 0; i < n; ++i)
				{
					const uint32_t a = in.start[i], b = in.start[i + 1];
					for (uint32_t k = a + 1; k + 1 < b; ++k) { sc.tris.push_back({ in.v[a], in.v[k], in.v[k + 1] }); sc.triBsp.push_back(in.bsp[i]); }
					++sc.polys;
					if (in.cat[i] == (uint8_t)Cat::Instanced) ++sc.instPolys;
				}
			}
			else
			{
			std::unordered_map<uint64_t, uint32_t> weld; weld.reserve(in.v.size());
			std::vector<Vec3> verts; verts.reserve(in.v.size() / 2);
			auto vid = [&](const Vec3& p) -> uint32_t
			{
				const int64_t qx = (int64_t)std::llround(p.x * 256.0), qy = (int64_t)std::llround(p.y * 256.0), qz = (int64_t)std::llround(p.z * 256.0);
				const uint64_t key = ((uint64_t)(qx & 0x1FFFFF) << 42) | ((uint64_t)(qy & 0x1FFFFF) << 21) | (uint64_t)(qz & 0x1FFFFF);
				auto [it, added] = weld.try_emplace(key, (uint32_t)verts.size());
				if (added) verts.push_back(p);
				return it->second;
			};
			struct Edge { uint64_t key; uint32_t poly; };
			std::vector<Edge> edges; edges.reserve(in.v.size());
			std::vector<Vec3> normals(n);
			std::vector<uint32_t> ids;
			for (size_t i = 0; i < n; ++i)
			{
				const uint32_t a = in.start[i], b = in.start[i + 1];
				ids.clear();
				for (uint32_t k = a; k < b; ++k)
				{
					const uint32_t id = vid(in.v[k]);
					if (ids.empty() || ids.back() != id) ids.push_back(id);
				}
				while (ids.size() > 1 && ids.front() == ids.back()) ids.pop_back();
				if (ids.size() < 3) continue;
				// Newell normal (any convex or nearly-convex polygon, either winding)
				Vec3 nn{ 0, 0, 0 };
				for (size_t k = 0; k < ids.size(); ++k)
				{
					const Vec3& p = verts[ids[k]]; const Vec3& q = verts[ids[(k + 1) % ids.size()]];
					nn.x += (p.y - q.y) * (p.z + q.z); nn.y += (p.z - q.z) * (p.x + q.x); nn.z += (p.x - q.x) * (p.y + q.y);
				}
				const float l = std::sqrt(dot3(nn, nn));
				if (!(l > 1e-9f)) continue;
				normals[i] = { nn.x / l, nn.y / l, nn.z / l };
				for (size_t k = 1; k + 1 < ids.size(); ++k)
				{
					sc.tris.push_back({ verts[ids[0]], verts[ids[k]], verts[ids[k + 1]] });
					sc.triBsp.push_back(in.bsp[i]);
				}
				for (size_t k = 0; k < ids.size(); ++k)
				{
					uint32_t p = ids[k], q = ids[(k + 1) % ids.size()];
					if (p > q) std::swap(p, q);
					edges.push_back({ ((uint64_t)p << 32) | q, (uint32_t)i });
				}
				++sc.polys;
				if (in.cat[i] == (uint8_t)Cat::Instanced) ++sc.instPolys;
			}
			std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) { return x.key < y.key; });
			for (size_t i = 0; i < edges.size();)
			{
				size_t j = i + 1;
				while (j < edges.size() && edges[j].key == edges[i].key) ++j;
				bool draw = (j - i) == 1;
				const Vec3& n0 = normals[edges[i].poly];
				for (size_t k = i + 1; k < j && !draw; ++k)
					if (std::fabs(dot3(n0, normals[edges[k].poly])) < 0.9995f) draw = true;
				if (draw)
				{
					const uint32_t p = (uint32_t)(edges[i].key >> 32), q = (uint32_t)(edges[i].key & 0xFFFFFFFFu);
					sc.lines.push_back({ verts[p], verts[q] });
					// the structure's colour wins on a shared edge; the BSP is the first polygon's
					uint8_t cat = in.cat[edges[i].poly];
					for (size_t k = i; k < j; ++k) if (in.cat[edges[k].poly] == (uint8_t)Cat::Bsp) cat = (uint8_t)Cat::Bsp;
					sc.lineCat.push_back(cat);
					sc.lineBsp.push_back(in.bsp[edges[i].poly]);
				}
				i = j;
			}
			}
			// occluders sorted by (cell, BSP) so the GPU buffer is uploaded once and drawn by the ranges near the camera
			{
				const size_t nt = sc.tris.size();
				std::vector<uint64_t> key(nt);
				std::vector<uint32_t> order(nt);
				for (uint32_t t = 0; t < (uint32_t)nt; ++t)
				{
					const Tri& tr = sc.tris[t];
					const Vec3 c{ (tr.a.x + tr.b.x + tr.c.x) / 3.f, (tr.a.y + tr.b.y + tr.c.y) / 3.f, (tr.a.z + tr.b.z + tr.c.z) / 3.f };
					key[t] = cellKey(cellOf(c.x), cellOf(c.y), cellOf(c.z));     // 63 bits: the BSP is compared separately (it used to be
					order[t] = t;                                                  // shifted in on top, which cut the key's high bits off
				}                                                                  // and no cell lookup ever matched: no occluders)
				std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key[a] != key[b] ? key[a] < key[b] : sc.triBsp[a] < sc.triBsp[b]; });
				std::vector<Tri> tris(nt);
				std::vector<uint8_t> bsps(nt);
				for (size_t i = 0; i < nt; ++i) { tris[i] = sc.tris[order[i]]; bsps[i] = sc.triBsp[order[i]]; }
				for (size_t i = 0; i < nt;)
				{
					const uint64_t cell = key[order[i]];
					const uint8_t bsp = sc.triBsp[order[i]];
					size_t j = i + 1;
					while (j < nt && key[order[j]] == cell && sc.triBsp[order[j]] == bsp) ++j;
					auto [it, added] = sc.occCells.try_emplace(cell, (uint32_t)sc.occRanges.size(), (uint32_t)sc.occRanges.size());
					sc.occRanges.push_back({ (uint32_t)i, (uint32_t)(j - i), bsp });
					it->second.second = (uint32_t)sc.occRanges.size();
					i = j;
				}
				sc.tris.swap(tris);
				sc.triBsp.swap(bsps);
			}
			sc.grid.build(sc.lines, 16.f);
			sc.buildMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		}

		std::shared_ptr<Viewer::ReachLoad> startLoad(std::function<bool(PolySoup&, std::string&)> source, std::string map, bool fromFile,
			std::filesystem::file_time_type fileTime, unsigned havokVer, std::string label, bool quiet = false)
		{
			auto job = std::make_shared<Viewer::ReachLoad>();
			job->map = map; job->fromFile = fromFile;
			std::thread([job, source = std::move(source), map, fromFile, fileTime, havokVer, label, quiet]() {
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				auto sc = std::make_shared<Viewer::ReachScene>();
				sc->map = map; sc->fromFile = fromFile; sc->fileTime = fileTime; sc->havokVer = havokVer; sc->source = label;
				std::string err;
				try
				{
					PolySoup soup;
					if (source(soup, err)) buildScene(soup, *sc);
					else sc.reset();
				}
				catch (const std::exception& e) { err = e.what(); sc.reset(); }
				{
					std::scoped_lock lk(job->m);
					job->result = sc;
					job->err = err;
				}
				if (quiet && sc) {}
				else if (sc) LOGF("Reach: %s for %s: %zu polygons (%zu instanced) -> %zu outline edges, %zu occluder triangles (%u ms)",
					label.c_str(), map.c_str(), sc->polys, sc->instPolys, sc->lines.size(), sc->tris.size(), sc->buildMs);
				else LOGF("Reach: %s for %s failed: %s", label.c_str(), map.c_str(), err.c_str());
				job->done = true;
				}).detach();
			return job;
		}
	}

	void Viewer::wantReachWorld(bool on)
	{
		if (on == mRxWantOn) return;
		mRxWantOn = on;
		if (mRxWantWorld) mRxWantWorld(on);
		LOGF("Reach: %s Poorboy's mod (observe mode) for the world file", on ? "asking for" : "letting go of");
	}

	void Viewer::stopReach()
	{
		if (mRxGather) HavokDebuggerBridge::stopGatherOnly();
		mRxGather = false;
		wantReachWorld(false);
	}

	// which source, and (re)build on a worker thread when it changed
	void Viewer::pumpReachScene(const std::string& map)
	{
		// a finished build
		if (mRxLoad && mRxLoad->done)
		{
			std::shared_ptr<ReachScene> got;
			{ std::scoped_lock lk(mRxLoad->m); got = mRxLoad->result; }
			if (got) got->bspMaskAtLoad = mRxLoadMask;
			if (got && got->map == map) mRxScene = got;
			if (got && mRxLoad->fromFile) mRxDirectFailed.clear();   // read directly after all: the Havok fallback goes off
			if (!got && mRxLoad->fromFile) { mRxDirectFailed = mRxLoadKey; mRxDirectRetryMs = ms() + 5000; }   // resources may still be loading
			mRxLoad.reset();
		}
		// the map's collision READ DIRECTLY from tag / resource memory (reach::readCollision; REPORT_REACH_DIRECT_READ.md):
		// structure + instanced geometry, no world file and no Havok decode. Rebuilt on a worker for a new map or a BSP change.
		// It needs a TLS block whose resource view holds the level: this (render = main game) thread's; the Havok engine-step
		// thread's view is empty on campaign maps (see reach::resource).
		// `fromFile` marks this authoritative scene (the name is kept from the world-file days).
		const bool fileScene = mRxScene && mRxScene->fromFile && mRxScene->map == map;
		const uint32_t maskNow = reach::activeBspMask();
		uintptr_t gameTls = reach::thisThreadTls();
		if (!gameTls) gameTls = (uintptr_t)HavokDebuggerBridge::reachEngineTls();
		const bool bspsChanged = fileScene && maskNow != 0 && maskNow != mRxScene->bspMaskAtLoad;
		const bool failedHere = mRxDirectFailed == map + "/" + std::to_string(maskNow) && ms() < mRxDirectRetryMs;
		if (!mRxLoad && maskNow != 0 && gameTls && (!fileScene || bspsChanged) && !failedHere && ms() >= mRxNextCheckMs)
		{
			mRxNextCheckMs = ms() + 500;
			const std::string failKey = map + "/" + std::to_string(maskNow);
			mRxLoad = startLoad([gameTls, maskNow](PolySoup& s, std::string& e)
				{
					reach::Polys p;
					std::string log;
					const bool ok = reach::readCollision(gameTls, maskNow, p, log);
					LOGF("Reach direct read:\n%s", log.c_str());
					if (!ok) { e = "nothing readable (see the lines above)"; return false; }
					s.v = std::move(p.v); s.start = std::move(p.start); s.bsp = std::move(p.bsp);
					s.cat.resize(p.instance.size());
					for (size_t k = 0; k < p.instance.size(); ++k) s.cat[k] = (uint8_t)(p.instance[k] ? Cat::Instanced : Cat::Bsp);
					// the edges with Halo 2's colour codes: structure / instanced, invisible (red), breakable (orange)
					s.haveLines = true;
					const size_t ne = p.lineKind.size();
					s.lines.reserve(ne); s.lineCat.reserve(ne); s.lineBsp.reserve(ne);
					for (size_t k = 0; k < ne; ++k)
					{
						Cat c = p.lineInstance[k] ? Cat::Instanced : Cat::Bsp;
						if (p.lineKind[k] == reach::Invisible) c = p.lineInstance[k] ? Cat::InstancedInvisible : Cat::BspInvisible;
						else if (p.lineKind[k] == reach::Breakable) c = Cat::Breakable;
						s.lines.push_back({ p.lineV[2 * k], p.lineV[2 * k + 1] });
						s.lineCat.push_back((uint8_t)c); s.lineBsp.push_back(p.lineBsp[k]);
					}
					return true;
				}, map, true, {}, 0, "direct read");
			mRxLoadMask = maskNow;
			mRxLoadKey = failKey;
		}
		// the Havok gather (an engine step hook on the GAME thread, every tick) runs only when something needs it: the live
		// objects (scenery / crates / machines shown), or the fallbacks - the Havok structure after a failed direct read, the
		// game thread's TLS when this thread has none. World + instanced only: no hook at all.
		const bool needGather = wantReachObjects() || (!fileScene && (!mRxDirectFailed.empty() || !reach::thisThreadTls()));
		if (needGather && !mRxGather)
		{
			mRxGather = HavokDebuggerBridge::startGatherOnly(3);
			LOGF("Reach: Havok collision gather %s", mRxGather ? "started (engine step hook)" : "could not start (no Havok SDK in this build, or the Havok debugger is attached to another game)");
		}
		else if (!needGather && mRxGather)
		{
			HavokDebuggerBridge::stopGatherOnly();
			mRxGather = false;
			LOGF("Reach: Havok collision gather stopped (no objects shown, the map read directly)");
		}
		if (mRxGather) HavokDebuggerBridge::setGatherStructure(!fileScene && !mRxDirectFailed.empty());   // the Havok structure: only the fallback after a failed direct read
		if (fileScene) return;
		const unsigned ver = HavokDebuggerBridge::bspVersion();
		const bool haveHavok = mRxScene && !mRxScene->fromFile && mRxScene->map == map && mRxScene->havokVer == ver;
		if (mRxGather && ver != 0 && !haveHavok && !mRxLoad)
		{
			auto raw = std::make_shared<std::vector<float>>((size_t)kMaxReachTris * 9);
			int count = 0; unsigned got = 0;
			if (HavokDebuggerBridge::copyBspTris(raw->data(), kMaxReachTris, &count, &got) && count > 0)
			{
				raw->resize((size_t)count * 9);
				mRxLoad = startLoad([raw](PolySoup& s, std::string&)
					{
						const size_t n = raw->size() / 9;
						s.v.reserve(n * 3); s.start.reserve(n + 1); s.cat.assign(n, (uint8_t)Cat::Bsp); s.bsp.assign(n, kAnyBsp);
						for (size_t i = 0; i < n; ++i)
						{
							s.start.push_back((uint32_t)s.v.size());
							const float* t = raw->data() + i * 9;
							s.v.push_back({ t[0], t[1], t[2] }); s.v.push_back({ t[3], t[4], t[5] }); s.v.push_back({ t[6], t[7], t[8] });
						}
						s.start.push_back((uint32_t)s.v.size());
						return true;
					}, map, false, {}, got, "Havok structure gather");
			}
		}
	}

	// the live objects: copy the engine-thread gather's triangles when it published new ones and rebuild their outlines.
	// The gather publishes every other tick; the triangles are copied only when its version moved, rebuilt only when they
	// differ from the last build (still scenery and crates: never), and the rebuild runs on a worker - it used to run
	// here, on the render thread, 30 times a second (Boneyard: 240 fps -> 80).
	void Viewer::pumpReachObjects()
	{
		constexpr int kMaxObjTris = 400000;     // = the gather's MAX_RX_OBJ_TRI
		if (mRxObjLoad && mRxObjLoad->done)
		{
			std::shared_ptr<ReachScene> got;
			{ std::scoped_lock lk(mRxObjLoad->m); got = mRxObjLoad->result; }
			mRxObjLoad.reset();
			if (got && mRxGather && wantReachObjects()) { mRxObjScene = got; ++mRxObjSceneGen; }
		}
		if (!mRxGather || !wantReachObjects()) { mRxObjScene.reset(); mRxObjPrev.clear(); mRxObjPrevTags.clear(); mRxObjVer = ~0u; return; }
		if (mRxObjLoad || HavokDebuggerBridge::objectTrisVersion() == mRxObjVer) return;   // a build in flight, or nothing new
		if (mRxObjBuf.size() != (size_t)kMaxObjTris * 9) { mRxObjBuf.resize((size_t)kMaxObjTris * 9); mRxObjTags.resize(kMaxObjTris); }
		int count = 0; unsigned ver = 0;
		if (!HavokDebuggerBridge::copyObjectTris(mRxObjBuf.data(), mRxObjTags.data(), kMaxObjTris, &count, &ver)) return;
		if (ver == mRxObjVer) return;
		mRxObjVer = ver;
		const size_t nf = (size_t)count * 9;
		if (mRxObjScene && mRxObjPrevTags.size() == (size_t)count
			&& std::memcmp(mRxObjPrev.data(), mRxObjBuf.data(), nf * sizeof(float)) == 0
			&& std::memcmp(mRxObjPrevTags.data(), mRxObjTags.data(), (size_t)count) == 0)
			return;                                                         // nothing moved
		mRxObjPrev.assign(mRxObjBuf.begin(), mRxObjBuf.begin() + nf);
		mRxObjPrevTags.assign(mRxObjTags.begin(), mRxObjTags.begin() + count);
		auto tris = std::make_shared<std::vector<float>>(mRxObjPrev);
		auto tags = std::make_shared<std::vector<uint8_t>>(mRxObjPrevTags);
		mRxObjLoad = startLoad([tris, tags](PolySoup& soup, std::string&)
			{
				const size_t n = tags->size();
				soup.v.reserve(n * 3); soup.start.reserve(n + 1);
				for (size_t i = 0; i < n; ++i)
				{
					const uint8_t cat = (*tags)[i];
					if (cat >= kCatCount) continue;
					const float* t = tris->data() + i * 9;
					soup.start.push_back((uint32_t)soup.v.size());
					soup.v.push_back({ t[0], t[1], t[2] }); soup.v.push_back({ t[3], t[4], t[5] }); soup.v.push_back({ t[6], t[7], t[8] });
					soup.cat.push_back(cat); soup.bsp.push_back(kAnyBsp);
				}
				soup.start.push_back((uint32_t)soup.v.size());
				return true;
			}, mRxMap, false, {}, 0, "live objects", true);
	}

	void Viewer::renderReach()
	{
		const auto tStart = std::chrono::steady_clock::now();
		mRxNote.clear();
		mRxMaskUsed = false;
		// the Collision Viewer's own drawing runs only while it is on; the superbounce viewer (below) draws without it
		const bool cvOn = settings.collision;
		const bool gameOk = reach::base() && reach::buildOk();
		std::string map;
		if (!reach::base()) mRxNote = "haloreach.dll is not loaded";
		else if (!reach::buildOk()) mRxNote = "unsupported Halo: Reach build (needs MCC 1.3528)";
		else if (cvOn)
		{
			map = reach::mapName();
			if (map != mRxMap) { mRxMap = map; mRxNextCheckMs = 0; }
			const std::string mapLog = map.empty() ? "map name unreadable - " + reach::mapNameDiag() : "map " + map;
			if (mapLog != mRxMapLogged) { mRxMapLogged = mapLog; LOGF("Reach: %s", mapLog.c_str()); }
			if (map.empty()) mRxNote = "the map name could not be read (see the HCM log)";
			pumpReachScene(map);
			pumpReachObjects();
		}

		const h2::Camera cam = reach::readCamera();
		const std::shared_ptr<ReachScene> sc = (mRxScene && mRxScene->map == map) ? mRxScene : nullptr;
		if (mRxNote.empty() && !cam.ok) mRxNote = "no game camera (menu or loading)";
		if (cvOn && mRxNote.empty() && !sc) mRxNote = mRxLoad ? "building the collision ..." : "waiting for the collision (gathered once the game runs)";

		if (cvOn && cam.ok && sc && !sc->lines.empty())
		{
			const int occ = settings.occlusion.load();
			const bool own = occ != (int)Occlusion::XRay;
			const uint32_t mask = reach::activeBspMask();
			auto bspOn = [mask](uint8_t b) { return b == kAnyBsp || mask == 0 || b >= 32 || (mask & (1u << b)); };
			const float R = settings.nearRadius.load(), R2 = R * R;

			// occluders near the camera: re-gathered from the buckets when it has moved, the radius or the BSPs changed
			if (own)
			{
				const float occR = R + kOccCell;
				const Vec3 d = sub3(cam.pos, mRxOccAt);
				// the scene's occluders go to the GPU ONCE (a new scene: map load / BSP change); moving only picks other ranges
				if (mRxOccScene != sc.get()) { ++mRxOccVer; mRxOccScene = sc.get(); mRxOccMask = ~mask; }
				if (mRxOccMask != mask || std::fabs(mRxOccR - occR) > 0.5f || dot3(d, d) > 16.f * 16.f)
				{
					mRxOccFirst.clear(); mRxOccCount.clear(); mRxOccTris = 0;
					const int r = (int)std::ceil(occR / kOccCell);
					const int cx = cellOf(cam.pos.x), cy = cellOf(cam.pos.y), cz = cellOf(cam.pos.z);
					std::vector<uint64_t> nearCells;
					const double cube = std::pow(2.0 * r + 1.0, 3.0);
					if (cube <= (double)sc->occCells.size())
					{
						for (int x = cx - r; x <= cx + r; ++x)
							for (int y = cy - r; y <= cy + r; ++y)
								for (int z = cz - r; z <= cz + r; ++z)
									if (sc->occCells.count(cellKey(x, y, z))) nearCells.push_back(cellKey(x, y, z));
					}
					else
					{
						// a big radius: walk the occupied cells instead of the cube around the camera
						for (const auto& kv : sc->occCells)
						{
							const int x = (int)((kv.first >> 42) & 0x1FFFFF) - 0x100000, y = (int)((kv.first >> 21) & 0x1FFFFF) - 0x100000, z = (int)(kv.first & 0x1FFFFF) - 0x100000;
							if (std::abs(x - cx) <= r && std::abs(y - cy) <= r && std::abs(z - cz) <= r) nearCells.push_back(kv.first);
						}
					}
					std::sort(nearCells.begin(), nearCells.end());                     // ascending cell = ascending buffer ranges (merges)
					for (uint64_t cell : nearCells)
					{
						const auto& span = sc->occCells.at(cell);
						for (uint32_t k = span.first; k < span.second; ++k)
						{
							const auto& rg = sc->occRanges[k];
							if (!bspOn(rg.bsp)) continue;
							if (!mRxOccFirst.empty() && mRxOccFirst.back() + mRxOccCount.back() == rg.first) mRxOccCount.back() += rg.count;
							else { mRxOccFirst.push_back(rg.first); mRxOccCount.push_back(rg.count); }
							mRxOccTris += rg.count;
						}
					}
					mRxOccAt = cam.pos; mRxOccR = occR; mRxOccMask = mask;
				}
				// Poorboy's gun mask: the game's own depth (surface 9) copied now, at Present, when it holds the gun and arms
				ID3D11Texture2D* depth = reach::mainDepth(mDev, &mRxDepthWhy);
				if (mRxDepthWhy != mRxDepthWhyLogged) { mRxDepthWhyLogged = mRxDepthWhy; LOGF("Reach: game depth %s: %s", depth ? "found" : "NOT found", mRxDepthWhy.c_str()); }
				if (depth)
				{
					mRxMaskUsed = mR.copyGameDepthForMask(mCtx, depth, 2 /* reversed finite */, cam.nearClip, cam.farClip, kGunMaskRange);
					depth->Release();
				}
			}
			if (mR.begin(mCtx, mRtv, mW, mH, cam.pos, cam.fwd, cam.up, cam.vfov, 0.01f))
			{
				if (own)
				{
					mR.clearOwnDepth();
					mR.setStaticOccluders(sc->tris, mRxOccVer);                 // a no-op unless the scene changed
					mR.drawStaticOccluderRanges(mRxOccFirst.data(), mRxOccCount.data(), mRxOccFirst.size());
					if (mRxObjScene && !mRxObjScene->tris.empty())
					{
						mR.setObjectOccluders(mRxObjScene->tris, mRxObjSceneGen);   // uploaded once per object scene
						mR.drawObjectOccluders();
					}
				}
				// the lines within the radius: gathered again only when the camera moved a quarter unit or something they
				// depend on changed (turning the camera changes nothing - the cut is by distance), uploaded only then
				uint32_t showBits = 0;
				for (int c = 0; c < kCatCount; ++c) if (settings.show[c]) showBits |= 1u << c;
				const uintptr_t objGen = mRxObjScene ? mRxObjSceneGen : 0;
				const Vec3 moved = sub3(cam.pos, mRxLinesAt);
				if (mRxLinesScene != sc.get() || mRxLinesObjGen != objGen || mRxLinesMask != mask || mRxLinesShow != showBits
					|| mRxLinesR != R || dot3(moved, moved) > 0.25f * 0.25f)
				{
					mRxLines.clear();
					sc->grid.query(sc->lines, cam.pos, R, [&](const Line& l)
					{
						const size_t i = (size_t)(&l - sc->lines.data());
						const uint8_t cat = sc->lineCat[i];
						if (!(showBits & (1u << cat)) || !bspOn(sc->lineBsp[i])) return;
						if (pointSegDist2(cam.pos, l.a, l.b) > R2) return;
						addLine(mRxLines, l.a, l.b, kCatColor[cat]);
					});
					if (mRxObjScene)
						for (size_t i = 0; i < mRxObjScene->lines.size(); ++i)
						{
							const uint8_t cat = mRxObjScene->lineCat[i];
							const Line& l = mRxObjScene->lines[i];
							if (!(showBits & (1u << cat)) || pointSegDist2(cam.pos, l.a, l.b) > R2) continue;
							addLine(mRxLines, l.a, l.b, kCatColor[cat]);
						}
					mRxLinesAt = cam.pos; mRxLinesScene = sc.get(); mRxLinesObjGen = objGen; mRxLinesMask = mask; mRxLinesShow = showBits; mRxLinesR = R;
					++mRxLinesKey;
				}
				mR.drawLinesCached(mRxLines.data(), mRxLines.size(), mRxLinesKey, settings.lineWidth, settings.pull, own, mRxMaskUsed, settings.hiddenAlpha);
				mR.end();
			}
			mR.clearGameDepth();
		}
		// superbounce viewer (2026-11-04): the Halo 2 body of Poorboy's h3sb.dll, the bsps read from tag memory. Reach has no render
		// hook, so it is drawn here at Present with the camera record's pos / fwd / up / vfov (as the collision lines are), unoccluded
		// like the Halo 2 / Halo 3 bounce lines; the game's depth is not used. The labels are projected with the same camera.
		if (gameOk && superbounceActive() && cam.ok)
		{
			mLabelScaleX = mLabelScaleY = 1.f;
			if (mR.begin(mCtx, mRtv, mW, mH, cam.pos, cam.fwd, cam.up, cam.vfov, 0.01f))
			{
				drawSuperbounce();
				mR.end();
			}
		}
		else { mSbLabels.clear(); mSb = sb::Output{}; }   // menus / loading: no stale CHARGED, last bounce or panel lines
		drawSuperbounceOverlay(ImGui::GetBackgroundDrawList(), gameOk);
		const float cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tStart).count();
		mRxCpuMs = mRxCpuMs <= 0.f ? cpuMs : mRxCpuMs * 0.95f + cpuMs * 0.05f;
		if (settings.hud && cvOn) drawHudReach(cam);
		drawReachBouncePanel();
	}

	// the superbounce Info-panel section on Reach (its loading state, the charge, the last launch and its verdict): its own small
	// window on the left, below the Collision Viewer's panel while that is up
	void Viewer::drawReachBouncePanel()
	{
		if (!settings.hud || !settings.superbounce || !settings.infoSuperbounce || mSb.panel.empty()) return;
		ImGui::SetNextWindowPos(ImVec2(12.f, settings.collision ? ImGui::GetIO().DisplaySize.y * 0.5f : 12.f), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (ImGui::Begin("Superbounce viewer##reachsb", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
		{
			for (const auto& l : mSb.panel) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(l.rgba), "%s", l.text.c_str());
		}
		ImGui::End();
	}

	void Viewer::drawHudReach(const h2::Camera& cam)
	{
		ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (ImGui::Begin("Collision Viewer##reachcv", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
		{
			const bool xray = settings.occlusion.load() == (int)Occlusion::XRay;
			ImGui::Text("Halo: Reach  %s   occlusion: %s", mRxMap.c_str(), xray ? "X-ray (no occlusion)" : "own collision depth + the game's depth as gun mask (no bloom)");
			if (!mRxNote.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "%s", mRxNote.c_str());
			if (mRxScene && mRxScene->map == mRxMap)
			{
				const ReachScene& sc = *mRxScene;
				ImGui::Text("source: %s  -  %zu polygons (%zu instanced), %zu edges, built in %u ms", sc.source.c_str(), sc.polys, sc.instPolys, sc.lines.size(), sc.buildMs);
				if (!sc.fromFile)
					ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), mRxDirectFailed.empty() ? "reading the map's collision directly ..."
					                                                                     : "the direct read failed (see the HCM log): Havok structure only, no instanced geometry");
			}
			if (mRxLoad) ImGui::TextDisabled("building %s ...", mRxLoad->fromFile ? "from the map's collision (direct read)" : "from the Havok gather");
			if (!xray) ImGui::Text("gun mask: %s   occluders near you: %zu   BSPs loaded: %08X", mRxMaskUsed ? "on (game depth, surface 9)" : "off (the game's depth was not found)",
				mRxOccTris, reach::activeBspMask());
			if (!xray && !mRxMaskUsed && !mRxDepthWhy.empty()) ImGui::TextDisabled("  depth lookup: %s", mRxDepthWhy.c_str());
			if (mRxMap.empty()) ImGui::TextDisabled("  map name bytes: %s", reach::mapNameDiag().c_str());
			if (cam.ok) ImGui::Text("camera %.2f %.2f %.2f  vfov %.1f  clip %.4f .. %.0f", cam.pos.x, cam.pos.y, cam.pos.z, cam.vfov * 57.29578f, cam.nearClip, cam.farClip);
			if (mRxObjScene) ImGui::Text("live objects (scenery / crates / machines): %zu triangles, %zu edges   game-thread pass %.2f ms", mRxObjScene->tris.size(), mRxObjScene->lines.size(),
				HavokDebuggerBridge::objectGatherMicros() / 1000.f);
			ImGui::Text("viewer: %.2f ms CPU per frame, %zu lines drawn%s", mRxCpuMs, mRxLines.size() / 2, mRxObjLoad ? "  (rebuilding the live objects)" : "");
			ImGui::TextDisabled("soft ceilings and kill volumes are not shown on Reach yet");
		}
		ImGui::End();
	}
}
