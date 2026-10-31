#include "pch.h"
// Halo 3 / Halo 3: ODST side of the viewer: in-frame hook intake, the TIM overlay and the ghost debug view for Havok 4.5.
//
// HAVOK 4.5 vs the Halo 2 (Havok 2) ghost debug
//   Halo 3 / ODST run hkpContinuousSimulation: bodies move through a CONTINUOUS step with time-of-impact events, there is NO backstep, no
//   S state and no rewind - so there is no "believed pose from a rewind". The same ghosting mechanism exists in a different form: the
//   narrow phase caches, per (body, shape key) child of the environment MOPP, a separating normal N and a distance w (the "TIM").
//   Each step the cache is updated   w -= dot(N, relative linear motion) + angular term   and a child whose w stays >= the collision
//   tolerance is SKIPPED (no contact processing). When the cache is wrong - w larger than the real gap - a polygon the body is actually
//   touching is skipped: the body passes through / hangs on a seam ("ghosting"). A raw position write (HCM Force Teleport, or any engine
//   path that moves com1 without the setTransform / warp-reset path) leaves every cached child stale in exactly that way.
//   What the view shows:
//     capsule  pink  = nothing suspect      lime = a cached w over-credits the real gap (w - gap > over-credit threshold)
//              red   = a polygon within the tolerance is being SKIPPED (stale and touching)
//     thin capsule behind it = the swept interval start (centre of mass 0), thin faded capsules = trail, white = Halo's own object position,
//     lime thick edges = skipped polygons (w >= tolerance), red thick edges = over-credited / stale ones,
//     thin lime capsules = "believed" poses: the body shifted away from the polygon by the over-credit (where the cache thinks it is),
//     yellow capsule + orange line = last JUMP: com0 of a step does not equal com1 of the previous step (engine integrate sets
//     com0 := com1, so a difference means a raw write / unsafe reposition happened between two ticks).
#include "H2CV_Viewer.h"
#include "H2CV_Halo3.h"
#include "H2CV_Engine.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include "H2CV_TimKeys.h"
#include "H2CV_ViewerUtil.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace h2cv
{
	using h2::Vec3;
	using namespace vu;

	namespace
	{
		constexpr float kTolFallback = 0.0164f;     // 0.0328 * 30 / 60 (world ctor: tolerance = 0.0328 * (30 / ticks per second))
		constexpr float kJumpMin = 0.004f;          // wu: com0 vs the previous com1 beyond this = a jump

		h2::Capsule worldCapsule(const h2::Capsule& local, const h2::Xf& pose)
		{
			h2::Capsule c; c.a = h2::xfPoint(pose, local.a); c.b = h2::xfPoint(pose, local.b); c.r = local.r; c.ok = local.ok;
			return c;
		}

		bool comOk3(IUnknown* p)
		{
			if (!mem::okp(reinterpret_cast<uintptr_t>(p))) return false;
			const uintptr_t vt = mem::q(reinterpret_cast<uintptr_t>(p));
			return mem::okp(vt) && mem::okp(mem::q(vt));
		}

		const char* polyKindName(const KeyInfo& ki) { return ki.kind == KeyInfo::WorldPolygon ? "world" : ki.kind == KeyInfo::IgPolygon ? "instanced" : "other"; }
	}

	// ---- camera / in-frame hook ---------------------------------------------------------------------------------------
	h2::Camera Viewer::camera3()
	{
		if (mF3.have && mPresentCount - mF3.presentIdx <= 3)
		{
			h2::Camera c; c.pos = mF3.f.pos; c.fwd = mF3.f.fwd; c.up = mF3.f.up; c.vfov = mF3.f.vfov; c.nearClip = mF3.f.nearClip; c.farClip = mF3.f.farClip; c.ok = true;
			return c;
		}
		return h3::readObserverCamera();
	}

	void Viewer::onFirstPersonPass3(uintptr_t view, bool opaquePass)
	{
		if (!opaquePass || !isH3() || !mDev || !mCtx || !mR.ready()) return;
		h2::GameFrame f;
		if (!h3::readViewFrame(view, f))
		{
			if ((++mBad3 & 0x1FF) == 1) LOGF("first-person pass: view struct @0x%llX did not validate (%u times)", (unsigned long long)view, mBad3);
			return;
		}
		ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
		mCtx->OMGetRenderTargets(1, &rtv, &dsv);
		D3D11_VIEWPORT vp{}; UINT nvp = 1; mCtx->RSGetViewports(&nvp, &vp);
		struct Rel { ID3D11RenderTargetView*& r; ID3D11DepthStencilView*& d; ~Rel() { if (r) r->Release(); if (d) d->Release(); } } rel{ rtv, dsv };
		mF3.have = true; mF3.f = f; mF3.vp = vp; mF3.presentIdx = mPresentCount; mF3.user = (uint32_t)h3::viewUser(view);
		const int occ = settings.occlusion.load();
		if (occ != (int)Occlusion::InFrame && occ != (int)Occlusion::PostBloom) return;
		if (!rtv || !dsv || nvp < 1 || vp.Width < 16 || vp.Height < 16 || !comOk3(rtv) || !comOk3(dsv)) return;

		// the bound set must be the main scene target: single-sample colour + D32_FLOAT_S8X24 depth of the same size
		D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dsv->GetDesc(&dd);
		D3D11_RENDER_TARGET_VIEW_DESC rd{}; rtv->GetDesc(&rd);
		if (dd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return;
		D3D11_TEXTURE2D_DESC td{}, cd{};
		{
			ID3D11Resource* r1 = nullptr; ID3D11Resource* r2 = nullptr;
			dsv->GetResource(&r1); rtv->GetResource(&r2);
			ID3D11Texture2D* t1 = nullptr; ID3D11Texture2D* t2 = nullptr;
			if (r1) { r1->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t1); r1->Release(); }
			if (r2) { r2->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2); r2->Release(); }
			if (t1) { t1->GetDesc(&td); t1->Release(); }
			if (t2) { t2->GetDesc(&cd); t2->Release(); }
			if (!td.Width || !cd.Width) return;
		}
		if (td.Width != cd.Width || td.Height != cd.Height || td.SampleDesc.Count != 1 || cd.SampleDesc.Count != 1)
		{
			if (!mLogged3) { LOGF("first-person pass: bound targets rejected (depth %ux%u x%u, colour %ux%u x%u)", td.Width, td.Height, td.SampleDesc.Count, cd.Width, cd.Height, cd.SampleDesc.Count); }
			return;
		}
		if (!mLogged3)
		{
			mLogged3 = true;
			const Vec3 p = add(f.pos, mul(f.fwd, 10.f));
			float clip[4];
			for (int c = 0; c < 4; ++c)
			{
				const float s = f.w2v[0];
				const float x = s * (p.x * f.w2v[1] + p.y * f.w2v[4] + p.z * f.w2v[7]) + f.w2v[10];
				const float y = s * (p.x * f.w2v[2] + p.y * f.w2v[5] + p.z * f.w2v[8]) + f.w2v[11];
				const float z = s * (p.x * f.w2v[3] + p.y * f.w2v[6] + p.z * f.w2v[9]) + f.w2v[12];
				clip[c] = x * f.proj[c] + y * f.proj[4 + c] + z * f.proj[8 + c] + f.proj[12 + c];
			}
			LOGF("first-person pass #1: view 0x%llX user %u  RTV fmt %d %ux%u  DSV fmt %d  viewport %.0f,%.0f %.0fx%.0f  reversed-Z byte %d  vfov %.3f near %.4f far %.1f  clip(cam+10*fwd) = (%.3f %.3f %.3f %.3f)  [w must be > 0, z/w in 0..1]",
				(unsigned long long)view, mF3.user, (int)rd.Format, cd.Width, cd.Height, (int)dd.Format, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
				h3::depthReversed() ? 1 : 0, f.vfov, f.nearClip, f.farClip, clip[0], clip[1], clip[2], clip[3]);
		}
		// labels are drawn on the back buffer at Present: scale from scene-target to back-buffer pixels
		mLabelScaleX = (cd.Width && mW) ? (float)mW / (float)cd.Width : 1.f;
		mLabelScaleY = (cd.Height && mH) ? (float)mH / (float)cd.Height : 1.f;
		if (occ == (int)Occlusion::PostBloom || occ == (int)Occlusion::InFrame)
		{
			// after bloom (also for InFrame_NoBloom: an in-frame draw here gets the game's bloom - Halo 3 / ODST have no
			// bloom switch like Halo 2's): keep the depth view; Present copies it once the gun is in it and draws over the finished frame
			if (mPb3Dsv != dsv)
			{
				if (mPb3Dsv) mPb3Dsv->Release();
				mPb3Dsv = dsv;
				mPb3Dsv->AddRef();
			}
			mPb3W = cd.Width; mPb3H = cd.Height;
			return;
		}
		if (drawViewInto(rtv, dsv, f, vp)) { mInFrameThisFrame = true; ++mInFrameCount; }
	}

	// Halo 3 / ODST "after bloom" at Present: the game's depth surface now holds the world and the first-person gun (the gun
	// pass draws into the same surface 3 - Poorboy, GUN_DEPTH_REPORT.md), so a copy of it hides the lines behind walls and the
	// gun while they are drawn over the finished, bloomed frame (none of the bloom on the lines).
	bool Viewer::postBloom3()
	{
		mPb3Why.clear();
		if (!mPb3Dsv || !mF3.have || mPresentCount - mF3.presentIdx > 1) { mPb3Why = "after bloom: no first-person pass this frame (menu, cinematic?) - own collision depth instead"; return false; }
		if (mPb3W != mW || mPb3H != mH) { mPb3Why = "after bloom: the scene is not drawn at the window's size (render scale?) - own collision depth instead"; return false; }
		if (!mR.copyWorldDepth(mCtx, mPb3Dsv)) { mPb3Why = "after bloom: the game's depth could not be copied - own collision depth instead"; return false; }
		mLabelScaleX = mLabelScaleY = 1.f;
		if (!drawViewInto(mRtv, mR.worldCopyDsv(), mF3.f, mF3.vp)) { mPb3Why = "after bloom: nothing to draw yet"; return false; }
		mInFrameThisFrame = true; ++mInFrameCount;
		return true;
	}

	// ---- simulation-thread sampling -----------------------------------------------------------------------------------
	void Viewer::onSimTick(uint32_t tick)
	{
		if (!isH3()) return;
		try
		{
			h3::captureSimTls();
			mSimLastMs = GetTickCount64();
			// death bar: read on the sim thread, because the game's trigger volume test and the players / game-time / kill
			// volume tables live in the sim thread's TLS
			if (!mWantSim3.load()) return;
			uintptr_t units[4] = {};
			h3::Ghost3 g;
			if (h3::localPlayerUnits(units) && units[0]) h3::sampleGhost(units[0], g);
			else { g.note = "no local player unit"; g.tick = tick; }
			if (tick) g.tick = tick;
			std::lock_guard<std::mutex> lk(mSimMu);
			mSimQ.push_back(std::move(g));
			while (mSimQ.size() > 90) mSimQ.pop_front();
		}
		catch (...) {}
	}

	void Viewer::pumpGhost3()
	{
		if (mPumped3 == mPresentCount && mPresentCount) return;     // once per Present (the in-frame path can draw several views)
		mPumped3 = mPresentCount;
		std::deque<h3::Ghost3> batch;
		const uint64_t last = mSimLastMs.load();
		const bool simLive = last != 0 && GetTickCount64() - last < 400;
		if (simLive) { std::lock_guard<std::mutex> lk(mSimMu); batch.swap(mSimQ); }
		else
		{
			{ std::lock_guard<std::mutex> lk(mSimMu); mSimQ.clear(); }
			uintptr_t units[4] = {};
			h3::Ghost3 g;
			if (h3::localPlayerUnits(units) && units[0]) h3::sampleGhost(units[0], g);
			else { g.note = "no local player unit"; g.tick = h3::gameTick(); }
			if (!mGP3.haveLatest || g.tick != mGP3.latest.tick || g.ok != mGP3.latest.ok) batch.push_back(std::move(g));
		}
		const uint32_t tickNow = h3::gameTick();
		(void)tickNow;
		const bool logOn = settings.ghostLog && settings.ghost && logEnsureOpen();
		const std::string lvl = logOn ? eng::levelCode() : std::string();
		for (auto& g : batch)
		{
			GhostPlayer3& gp = mGP3;
			if (!g.ok || !g.m.ok) { gp.latest = std::move(g); gp.haveLatest = true; gp.have = false; continue; }
			if (gp.body != g.body) { gp.trail.clear(); gp.have = false; gp.haveJump = false; gp.body = g.body; gp.lastHaloOk = false; }
			if (gp.have && g.tick < gp.lastTick) { gp.trail.clear(); gp.have = false; gp.haveJump = false; }          // checkpoint load / revert
			if (!gp.have || g.tick != gp.lastTick)
			{
				if (gp.have && g.tick == gp.lastTick + 1)
				{
					// integrate sets com0 := com1, so com0 of this step must equal com1 of the previous one
					const Vec3 d = sub(g.m.com0, gp.lastCom1);
					const float dist = len(d);
					if (dist > kJumpMin && dist < 200.f)
					{
						bool haloJump = false;
						if (g.haloOk && gp.lastHaloOk) haloJump = len(sub(g.halo, gp.lastHalo)) > 0.5f * dist;
						gp.haveJump = true; gp.jumpTick = g.tick; gp.jumpFrom = gp.lastCom1; gp.jumpTo = g.m.com0; gp.jumpDist = dist; gp.jumpHalo = haloJump; ++gp.jumps;
						if (logOn)
							logEvent(std::string("{\"event\":\"jump\",\"engine\":\"") + gameName(game()) + "\",\"tick\":" + std::to_string(g.tick) + ",\"map\":\"" + lvl +
								"\",\"user\":0,\"subject\":\"biped\",\"cur\":" + jv(g.m.com1) + ",\"prev\":" + jv(g.m.com0) + ",\"v\":" + jv(g.m.v) + ",\"vint\":" + jv(g.m.v) +
								",\"S\":0,\"from\":" + jv(gp.lastCom1) + ",\"to\":" + jv(g.m.com0) + ",\"delta\":" + jv(d) + ",\"dist\":" + jf(dist) +
								",\"halo_moved_with\":" + (haloJump ? "true" : "false") + ",\"n_pairs\":" + std::to_string(g.pairs.size()) + "}");
					}
				}
				Sample3 sm; sm.tick = g.tick; sm.com0 = g.m.com0; sm.com1 = g.m.com1; sm.pose = g.m.xf;
				gp.trail.push_back(sm);
				while (gp.trail.size() > 64) gp.trail.pop_front();
				gp.lastTick = g.tick; gp.lastCom1 = g.m.com1; gp.have = true;
				gp.lastHalo = g.halo; gp.lastHaloOk = g.haloOk;
			}
			gp.latest = std::move(g); gp.haveLatest = true;
		}
		analyse3();
	}

	// ---- per-child analysis of the latest sample -----------------------------------------------------------------------
	void Viewer::analyse3()
	{
		mAn3.clear(); mRows3.clear();
		GhostInfo3 gi; gi.valid = true;
		gi.sims = mGP3.jumps;
		if (!mGP3.haveLatest) { gi.note = "waiting for the first simulation tick"; mGI3 = gi; return; }
		const h3::Ghost3& g = mGP3.latest;
		gi.note = g.note;
		if (!g.ok || !g.m.ok) { mGI3 = gi; return; }
		const float tol = g.t.ok ? g.t.tol : kTolFallback;
		const float dt = g.t.ok ? g.t.stepEnd - g.t.stepStart : 0.f;
		const float overMin = settings.ghostOverMin;
		gi.tol = tol; gi.pairs = (int)g.pairs.size();
		const h2::Capsule cap = worldCapsule(g.cap, g.m.xf);
		// The cache's reference pose is the END of the step (w is updated to the step end), but the sample may land before the body is integrated
		// to it: the over-credit is therefore measured against the LARGER of the gap at the current pose and at the step-start pose (com0), so a
		// legitimate cache never reads as over-credited. A real stale cache is over-credited against both.
		const Vec3 toStart = sub(g.m.com0, g.m.com1);
		const bool haveStart = len(toStart) > 1e-5f && len(toStart) < 1.f;
		h2::Capsule capStart = cap; capStart.a = add(cap.a, toStart); capStart.b = add(cap.b, toStart);
		gi.stepMove = len(sub(g.m.com1, g.m.com0));
		if (g.haloOk) { const float dO = len(sub(g.halo, g.m.xf.t)), dC = len(sub(g.halo, g.m.com1)); gi.dHalo = std::min(dO, dC); }
		std::vector<Vec3> poly;
		const auto& bsps = mWorld.bsps3();
		for (const h3::Pair3& p : g.pairs)
		{
			if (p.verified) gi.layoutOk = true;
			if (!p.note.empty() && gi.detail.empty()) gi.detail = p.note;
			int bsp = p.bsp;
			if (bsp < 0 && bsps.size() == 1) bsp = bsps[0].index;
			if (gi.bsp < 0) gi.bsp = bsp;
			for (const h3::Tim3& k : p.kids)
			{
				An3 a; a.pair = &p; a.k = &k; a.bsp = bsp;
				const KeyInfo ki = decodeKey3(k.key);
				a.cat = ki.category; a.inst = ki.instance; a.surf = ki.surface;
				if (bsp >= 0)
				{
					if (ki.kind == KeyInfo::WorldPolygon) a.havePoly = mWorld.worldPolygon3(bsp, ki.surface, poly);
					else if (ki.kind == KeyInfo::IgPolygon) a.havePoly = mWorld.igPolygon3(bsp, ki.instance, ki.surface, poly);
				}
				float gapRef = 1e9f;
				if (a.havePoly && poly.size() >= 3)
				{
					a.poly = poly; a.trueDist = segPolyDist(cap.a, cap.b, poly) - cap.r;
					gapRef = haveStart ? std::max(a.trueDist, segPolyDist(capStart.a, capStart.b, poly) - cap.r) : a.trueDist;
				}
				else a.havePoly = false;
				a.validTime = k.hasTim && std::isfinite(k.time) && k.time >= 0.f;
				a.skipped = a.validTime && k.w >= tol;
				a.touching = a.havePoly && a.trueDist < tol;
				if (a.validTime && a.havePoly) a.over = k.w - gapRef;
				a.overcredit = a.validTime && a.havePoly && a.over > overMin;
				a.stale = a.skipped && a.touching;
				if (a.validTime && dt > 1e-7f) a.age = (int)std::lround((g.t.stepEnd - k.time) / dt);
				++gi.kids;
				if (k.hasTim) ++gi.tims;
				if (a.skipped) ++gi.skipped;
				if (a.stale) ++gi.touchSkipped;
				if (a.overcredit) { ++gi.overcredited; gi.maxOver = std::max(gi.maxOver, a.over); }
				mAn3.push_back(std::move(a));
			}
		}
		gi.red = gi.touchSkipped > 0;
		gi.lime = gi.overcredited > 0;
		// HUD rows: stale first, then over-credited, then the closest polygons
		std::vector<const An3*> order;
		for (const auto& a : mAn3) if (a.havePoly) order.push_back(&a);
		std::sort(order.begin(), order.end(), [](const An3* x, const An3* y) {
			const int rx = x->stale ? 0 : (x->overcredit ? 1 : 2), ry = y->stale ? 0 : (y->overcredit ? 1 : 2);
			return rx != ry ? rx < ry : x->trueDist < y->trueDist; });
		for (size_t i = 0; i < order.size() && i < 14; ++i)
		{
			const An3& a = *order[i];
			mRows3.push_back({ a.k->key, a.cat, a.bsp, a.inst, a.surf, a.k->w, a.trueDist, a.over, a.age, a.k->hasTim, a.skipped, a.touching, a.stale });
		}
		mGI3 = gi;
	}

	// ---- TIM overlay (the polygons the player's capsule is being tested against) --------------------------------------
	void Viewer::drawTim3()
	{
		pumpGhost3();
		if (mAn3.empty()) return;
		mScratch.clear();
		for (const An3& a : mAn3)
		{
			if (!a.havePoly) continue;
			uint32_t col;
			if (a.stale) col = rgba(255, 30, 30);                  // skipped although touching: the ghosting state
			else if (a.overcredit) col = rgba(255, 170, 0);        // cache over-credits the gap
			else if (a.skipped) col = rgba(255, 255, 0);           // skipped, honestly (far enough)
			else if (a.k->hasTim) col = rgba(150, 150, 150);       // cached but within tolerance: narrow phase runs
			else col = rgba(255, 255, 255);                        // no cache: narrow phase runs every step
			const int n = (int)a.poly.size();
			for (int i = 0; i < n; ++i)
			{
				const Vec3& p0 = a.poly[i]; const Vec3& p1 = a.poly[(i + 1) % n];
				addLine(mScratch, p0, p1, col);
				if (!settings.timLabels) continue;
				const Vec3 mid = mul(add(p0, p1), 0.5f);
				Label L{}; L.col = col;
				if (mR.project(mid, L.x, L.y))
				{
					L.x *= mLabelScaleX; L.y *= mLabelScaleY;
					if (!a.k->hasTim) snprintf(L.text, sizeof(L.text), "n");
					else snprintf(L.text, sizeof(L.text), "%.3f%s", a.k->w, a.stale ? "!!" : (a.overcredit ? "!" : ""));
					gLabels.push_back(L);
				}
			}
		}
		mR.drawLines(mScratch.data(), mScratch.size(), settings.lineWidth + 1.5f, 0.f, false, false, 0.f);
	}

	// ---- Havok capsules of the bipeds (yours yellow), through walls ---------------------------------------------------
	void Viewer::drawPills3()
	{
		std::vector<h2::ObjectRef> objs;
		h3::enumerateObjects(objs);
		const uintptr_t me = h3::localPlayerUnitObject();
		mScratch.clear();
		for (const auto& o : objs)
		{
			if (o.type != h3::Biped) continue;
			if (settings.ghost && o.obj == me) continue;     // drawn (state-coloured) by drawGhost3 instead
			uintptr_t bodies[8];
			const int nb = h3::havokBodies(o.obj, bodies);
			for (int i = 0; i < nb; ++i)
			{
				const h2::Capsule lc = h3::bodyCapsuleLocal(bodies[i]);
				h3::MotionState m;
				if (!lc.ok || !h3::readMotion(bodies[i], m)) continue;
				capsuleWire(mScratch, worldCapsule(lc, m.xf), o.obj == me ? rgba(255, 230, 0) : rgba(255, 255, 255, 200));
				break;
			}
		}
		mR.drawLines(mScratch.data(), mScratch.size(), 1.5f, 0.f, false, false, 0.f);
	}

	void Viewer::bipedOccluders3(std::vector<Tri>& caps, const h2::Vec3& camPos)
	{
		std::vector<h2::ObjectRef> objs;
		h3::enumerateObjects(objs);
		for (const auto& o : objs)
		{
			if (o.type != h3::Biped) continue;
			uintptr_t bodies[8];
			const int nb = h3::havokBodies(o.obj, bodies);
			for (int i = 0; i < nb; ++i)
			{
				const h2::Capsule lc = h3::bodyCapsuleLocal(bodies[i]);
				h3::MotionState m;
				if (!lc.ok || !h3::readMotion(bodies[i], m)) continue;
				const h2::Capsule c = worldCapsule(lc, m.xf);
				if (std::sqrt(pointSegDist2(camPos, c.a, c.b)) < c.r + 0.05f) break;   // camera inside it
				capsuleTris(caps, c);
				break;
			}
		}
	}

	// ---- ghost debug --------------------------------------------------------------------------------------------------
	void Viewer::drawGhost3()
	{
		pumpGhost3();
		const uint32_t kLime = rgba(170, 255, 0), kRed = rgba(255, 40, 40);
		const uint32_t kPink = settings.ghostColNormal.load(), kDesync = settings.ghostColDesync.load(), kBelieved = settings.ghostColBelieved.load();
		const uint32_t kHalo = rgba(255, 255, 255), kJump = rgba(255, 230, 0), kJumpFrom = rgba(255, 90, 0);
		const Vec3 zero{ 0, 0, 0 };
		auto withA = [](uint32_t c, uint8_t a) { return (c & 0x00FFFFFFu) | ((uint32_t)a << 24); };
		GhostPlayer3& gp = mGP3;
		const GhostInfo3& gi = mGI3;
		++mLogFrame;
		if (!settings.ghostLog && mLogFile) logClose();
		if (!settings.ghostLog) mLogPath.clear();
		const bool logOn = settings.ghostLog && logEnsureOpen();

		if (!gp.haveLatest || !gp.latest.ok || !gp.latest.m.ok) return;
		const h3::Ghost3& g = gp.latest;
		const h2::Xf& pose = g.m.xf;
		const int trailLen = std::clamp(settings.ghostTrailLen.load(), 0, 60);
		const uint32_t hold = (uint32_t)std::clamp(settings.ghostHold.load(), 0, 120);
		const int belMax = std::clamp(settings.ghostBelievedMax.load(), 0, 32);
		const float prevMin = settings.ghostPrevMinDelta, haloMin = settings.ghostHaloMinDelta;
		const float overMin = settings.ghostOverMin;
		const uint32_t st = gi.red ? kRed : (gi.lime || (settings.ghostStrictSkip && gi.skipped > 0) ? kDesync : kPink);

		std::vector<LineVertex> capsV, thinV, edgeLime, edgeRed;
		auto drawCap = [&](std::vector<LineVertex>& dst, const h2::Xf& ps, const Vec3& shift, uint32_t col)
		{
			h2::Capsule c = worldCapsule(g.cap, ps);
			c.a = add(c.a, shift); c.b = add(c.b, shift);
			capsuleWire(dst, c, col);
		};
		const h2::Capsule real = worldCapsule(g.cap, pose);
		const Vec3 mid = mul(add(real.a, real.b), 0.5f);

		// (a) Havok's current pose
		if (settings.ghostShowReal) drawCap(capsV, pose, zero, st);
		// (b) swept-interval start: centre of mass 0 (step start) relative to centre of mass 1 (step end)
		{
			const Vec3 d = sub(g.m.com0, g.m.com1);
			if (settings.ghostPrev && len(d) > prevMin && len(d) < 5.f) drawCap(thinV, pose, d, withA(st, 120));
		}
		// (c) Halo's own object position; the reference point is whichever of the body origin / centre of mass it is nearer to
		if (g.haloOk)
		{
			const float dO = len(sub(g.halo, pose.t)), dC = len(sub(g.halo, g.m.com1));
			const Vec3 d = sub(g.halo, dC < dO ? g.m.com1 : pose.t);
			if (settings.ghostHalo && len(d) > haloMin && len(d) < 50.f) drawCap(capsV, pose, d, kHalo);
		}
		// (d) trail of the last N tick poses
		if (settings.ghostTrail && trailLen > 0)
		{
			const int n = (int)gp.trail.size(), first = std::max(0, n - trailLen - 1);
			for (int i = first; i < n; ++i)
			{
				const Sample3& sm = gp.trail[i];
				const Vec3 d = sub(sm.com1, g.m.com1);
				if (len(d) < 5e-4f || len(d) > 20.f) continue;
				const float f = (float)(i - first + 1) / (float)(n - first);
				drawCap(thinV, sm.pose, zero, withA(kPink, (uint8_t)(35 + 150 * f)));
			}
		}
		// (e) last jump: where the body was at the end of the previous step -> where the next step started
		if (settings.ghostJump && gp.haveJump && g.tick >= gp.jumpTick && g.tick - gp.jumpTick <= hold)
		{
			drawCap(thinV, pose, sub(gp.jumpFrom, g.m.com1), withA(kJumpFrom, 170));
			drawCap(capsV, pose, sub(gp.jumpTo, g.m.com1), kJump);
			addLine(thinV, add(gp.jumpFrom, sub(mid, g.m.com1)), add(gp.jumpTo, sub(mid, g.m.com1)), kJump);
		}

		// per-child: edges of skipped polygons, believed poses, logger episodes
		struct Bel { float over; Vec3 dir; };
		std::vector<Bel> bel;
		std::vector<std::pair<uint32_t, Vec3>> contacts;
		for (const An3& a : mAn3)
		{
			if (a.havePoly && !a.k->hasTim && a.touching) contacts.push_back({ a.k->key, norm(cross(sub(a.poly[1], a.poly[0]), sub(a.poly[2], a.poly[0]))) });
		}
		std::string cj = "[";
		if (logOn)
		{
			for (size_t i = 0; i < contacts.size(); ++i)
			{
				const KeyInfo ck = decodeKey3(contacts[i].first);
				if (i) cj += ",";
				cj += "{\"key\":" + std::to_string(contacts[i].first) + ",\"poly_kind\":\"" + polyKindName(ck) + "\",\"instance\":" + std::to_string(ck.instance) +
					",\"surface\":" + std::to_string(ck.surface) + ",\"normal\":" + jv(contacts[i].second) + "}";
			}
		}
		cj += "]";
		const std::string lvl = logOn ? eng::levelCode() : std::string();
		const uint32_t tick = g.tick;
		for (const An3& a : mAn3)
		{
			if (!a.havePoly || !a.k->hasTim) continue;
			const bool suspect = a.overcredit || a.stale;
			if (a.skipped && settings.ghostEdges && (suspect || !settings.ghostEdgesSuspectOnly))
			{
				auto& dst = suspect ? edgeRed : edgeLime;
				const int n = (int)a.poly.size();
				for (int i = 0; i < n; ++i) addLine(dst, a.poly[i], a.poly[(i + 1) % n], suspect ? kRed : kLime);
			}
			Vec3 n = norm(cross(sub(a.poly[1], a.poly[0]), sub(a.poly[2], a.poly[0])));
			const float side = dot(sub(mid, a.poly[0]), n);
			if (std::fabs(side) < 1e-6f) n = norm(a.k->n); else if (side < 0) n = mul(n, -1.f);   // away from the polygon, on the body's side
			if (a.overcredit && belMax > 0) bel.push_back({ a.over, n });
			if (logOn && (a.overcredit || a.stale))
			{
				const uint64_t key64 = ((uint64_t)(a.bsp + 1) << 44) | (1ull << 38) | (uint64_t)a.k->key;
				auto it = std::find_if(mLogActive.begin(), mLogActive.end(), [&](const auto& e) { return e.first == key64; });
				if (it != mLogActive.end()) { it->second.peakOver = std::max(it->second.peakOver, a.over); it->second.lastFrame = mLogFrame; }
				else
				{
					const KeyInfo ki = decodeKey3(a.k->key);
					Vec3 c{}; for (const auto& v : a.poly) c = add(c, v); c = mul(c, 1.f / (float)a.poly.size());
					std::string pv = "["; for (size_t i = 0; i < a.poly.size(); ++i) { if (i) pv += ","; pv += jv(a.poly[i]); } pv += "]";
					std::string sf;
					if (ki.kind == KeyInfo::WorldPolygon)
					{
						if (const auto* b = mWorld.bsp3(a.bsp); b && ki.surface < b->world.surfaces.size())
						{ const auto& su = b->world.surfaces[ki.surface]; sf = ",\"flags\":" + std::to_string(su.flags) + ",\"material\":" + std::to_string(su.material) + ",\"breakable\":" + std::to_string(su.breakBit); }
					}
					const Vec3 step = sub(g.m.com1, g.m.com0);
					std::string rec = std::string("{\"event\":\"overcredit_start\",\"engine\":\"") + gameName(game()) + "\",\"tick\":" + std::to_string(tick) + ",\"map\":\"" + lvl +
						"\",\"user\":0,\"subject\":\"biped\",\"cur\":" + jv(g.m.com1) + ",\"prev\":" + jv(g.m.com0) + ",\"v\":" + jv(g.m.v) + ",\"vint\":" + jv(g.m.v) + ",\"S\":0" +
						",\"key\":" + std::to_string(a.k->key) + ",\"key64\":" + std::to_string(key64) + ",\"poly_kind\":\"" + polyKindName(ki) + "\",\"bsp\":" + std::to_string(a.bsp) +
						",\"instance\":" + std::to_string(ki.instance) + ",\"surface\":" + std::to_string(ki.surface) + sf + ",\"child_kind\":\"tim1n\",\"poly\":" + pv +
						",\"centroid\":" + jv(c) + ",\"normal\":" + jv(n) + ",\"cached_normal\":" + jv(a.k->n) + ",\"tim\":" + jf(a.k->w) + ",\"true_dist\":" + jf(a.trueDist) +
						",\"over\":" + jf(a.over) + ",\"believed_shift\":" + jv(mul(n, a.over)) + ",\"n_dot_vint\":" + jf(dot(n, g.m.v)) + ",\"n_dot_v\":" + jf(dot(n, g.m.v)) +
						",\"n_dot_step\":" + jf(dot(n, step)) + ",\"stale\":" + (a.stale ? "true" : "false") + ",\"touching\":" + (a.touching ? "true" : "false") +
						",\"skipped\":" + (a.skipped ? "true" : "false") + ",\"time_stamp\":" + jf(a.k->time) + ",\"age_steps\":" + std::to_string(a.age) +
						",\"tol\":" + jf(gi.tol) + ",\"over_min\":" + jf(overMin) + ",\"contacts\":" + cj + "}";
					logEvent(rec);
					LogState ls; ls.startTick = tick; ls.lastFrame = mLogFrame; ls.peakOver = a.over;
					mLogActive.push_back({ key64, ls });
				}
			}
		}
		if (logOn)
		{
			if (!gp.pairDumped && !g.pairs.empty())
			{
				gp.pairDumped = true;
				std::string dump; h3::dumpPairBytes(g, dump);
				logEvent(std::string("{\"event\":\"pair_layout\",\"engine\":\"") + gameName(game()) + "\",\"tick\":" + std::to_string(tick) + ",\"map\":\"" + lvl + "\",\"verified\":" +
					(g.pairs[0].verified ? "true" : "false") + ",\"agent_type\":" + std::to_string(g.pairs[0].agentType) + ",\"cmd\":" + std::to_string(g.pairs[0].cmd) +
					",\"sectors\":" + std::to_string(g.pairs[0].sectors) + ",\"children\":" + std::to_string(g.pairs[0].kids.size()) + ",\"bsp\":" + std::to_string(g.pairs[0].bsp) +
					",\"note\":\"" + g.pairs[0].note + "\",\"dump\":\"" + dump + "\"}");
			}
			for (auto it = mLogActive.begin(); it != mLogActive.end();)
			{
				if (it->second.lastFrame == mLogFrame) { ++it; continue; }
				logEvent("{\"event\":\"overcredit_end\",\"engine\":\"" + std::string(gameName(game())) + "\",\"tick\":" + std::to_string(tick) + ",\"key64\":" + std::to_string(it->first) +
					",\"key\":" + std::to_string((uint32_t)(it->first & 0xFFFFFFFFu)) + ",\"user\":0,\"start_tick\":" + std::to_string(it->second.startTick) +
					",\"ticks\":" + std::to_string((int64_t)tick - (int64_t)it->second.startTick) + ",\"peak_over\":" + jf(it->second.peakOver) + "}");
				it = mLogActive.erase(it);
			}
		}
		// (f) believed poses
		if (settings.ghostBelieved && !bel.empty())
		{
			std::sort(bel.begin(), bel.end(), [](const Bel& x, const Bel& y) { return x.over > y.over; });
			if ((int)bel.size() > belMax) bel.resize((size_t)belMax);
			mGI3.believed = (int)bel.size();
			for (const auto& b : bel)
			{
				const Vec3 d = mul(b.dir, b.over);
				drawCap(capsV, pose, d, kBelieved);
				addLine(thinV, add(mid, d), mid, withA(kBelieved, 130));
			}
		}
		mR.drawLines(thinV.data(), thinV.size(), 1.3f, 0.f, false, false, 0.f);
		mR.drawLines(capsV.data(), capsV.size(), 2.5f, 0.f, false, false, 0.f);
		mR.drawLines(edgeLime.data(), edgeLime.size(), settings.lineWidth + 3.0f, 0.f, false, false, 0.f);
		mR.drawLines(edgeRed.data(), edgeRed.size(), settings.lineWidth + 4.5f, 0.f, false, false, 0.f);
	}

	// ---- HUD ----------------------------------------------------------------------------------------------------------
	void Viewer::drawDeath3()
	{
		mOobBar = -1.f; mOobBarText[0] = 0;
		h3::Death d;
		h3::readDeath(d);                    // finds the game thread's TLS block itself, no sim callback needed
		mDetectTicks = (d.valid && d.counter == 0) ? d.nextCheck : -1;
		if (!d.valid) { mDeath3 = d; return; }
		int best = INT32_MAX; float frac = -1.f;
		auto offer = [&](int left, int period, const char* what)
		{
			if (left < 0 || period <= 0 || left >= best) return;
			best = left; frac = std::clamp((float)left / (float)period, 0.f, 1.f);
			snprintf(mOobBarText, sizeof(mOobBarText), "%s - %d ticks", what, left);
		};
		// Halo 3 keeps the count for the whole life (it never drains), so the bar stays put while you are back in bounds
		if (d.counter > 0)   // can run past the limit when the kill is blocked (invulnerability): clamp to 0
			offer(std::max(0, d.limit + 1 - d.counter), d.limit + 1, d.counter > d.limit + 1 ? "OUT OF BOUNDS (kill blocked)" : "OUT OF BOUNDS");
		for (const auto& in : d.kvInside)
		{
			char what[32]; snprintf(what, sizeof(what), "KILL VOLUME #%d", in.first);
			offer(in.second, d.kvCount, what);
		}
		mOobBar = frac;
		if (frac < 0.f) mOobBarText[0] = 0;
		mDeath3 = std::move(d);
	}

	void Viewer::drawHud3()
	{
		ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (ImGui::Begin("Collision Viewer##h3cv", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
		{
			ImGui::Text("%s   occlusion: %s", game() == Game::Halo3ODST ? "Halo 3: ODST" : "Halo 3", occlusionName((Occlusion)settings.occlusion.load()));
			if (!mInFrameNote.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "%s", mInFrameNote.c_str());
			if (!mWorld.lastWhy().empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "collision: %s", mWorld.lastWhy().c_str());
			ImGui::Text("width %.1fpx  pull %.4f  hidden %.2f  radius %.0f",
				settings.lineWidth.load(), settings.pull.load(), settings.hiddenAlpha.load(), settings.nearRadius.load());
			for (int c = 0; c < kCatCount; ++c)
			{
				const uint32_t col = kCatColor[c];
				ImGui::TextColored(ImVec4((col & 0xFF) / 255.f, ((col >> 8) & 0xFF) / 255.f, ((col >> 16) & 0xFF) / 255.f, 1),
					"%s %s  %zu", settings.show[c] ? "[x]" : "[ ]", catName((Cat)c), mWorld.lines((Cat)c).size());
				if (c % 3 != 2 && c != kCatCount - 1) ImGui::SameLine();
			}
			for (const auto& b : mWorld.bsps3())
				ImGui::TextDisabled("bsp %d: %s  (%zu surfaces, %zu IG instances)", b.index, b.how.c_str(), b.world.surfaces.size(), b.igInst.size());
			if (mWorld.objectShapesSkipped() > 0) ImGui::TextDisabled("object shapes not drawn (mopp-backed): %d", mWorld.objectShapesSkipped());
			ImGui::Separator();
			const GhostInfo3& gi = mGI3;
			ImGui::Text("TIM overlay %s  labels %s  capsules %s   (Havok 4.5: w = cached separating distance; tolerance %.4f)", settings.tim ? "on" : "off",
				settings.timLabels ? "on" : "off", settings.pills ? "on" : "off", gi.tol);
			if (settings.tim || settings.ghost)
			{
				if (!gi.note.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "%s", gi.note.c_str());
				if (!gi.detail.empty()) ImGui::TextDisabled("%s", gi.detail.c_str());
				if (gi.pairs > 0)
				{
					ImGui::Text("pairs %d  bsp %d  children %d  with TIM %d  skipped %d  touching-but-skipped %d  over-credited %d (max %.4f wu)  entry layout %s",
						gi.pairs, gi.bsp, gi.kids, gi.tims, gi.skipped, gi.touchSkipped, gi.overcredited, gi.maxOver, gi.layoutOk ? "verified" : "UNVERIFIED");
					if (mGP3.haveLatest && !mGP3.latest.pairs.empty())
					{
						const h3::Pair3& p0 = mGP3.latest.pairs[0];
						ImGui::TextDisabled("    pair entry: cmd %u agent type %u, %d track sector(s), %zu children%s%s", p0.cmd, p0.agentType, p0.sectors, p0.kids.size(), p0.note.empty() ? "" : "  - ", p0.note.c_str());
					}
					if (!mRows3.empty() && ImGui::BeginTable("tim3", 6, ImGuiTableFlags_SizingFixedFit))
					{
						ImGui::TableSetupColumn("key"); ImGui::TableSetupColumn("poly"); ImGui::TableSetupColumn("w");
						ImGui::TableSetupColumn("true gap"); ImGui::TableSetupColumn("age"); ImGui::TableSetupColumn("state");
						ImGui::TableHeadersRow();
						for (const auto& r : mRows3)
						{
							ImGui::TableNextRow();
							ImGui::TableNextColumn(); ImGui::Text("%08X", r.key);
							ImGui::TableNextColumn();
							if (r.cat == 1) ImGui::Text("b%d s%u", r.bsp, r.surf);
							else if (r.cat == 3) ImGui::Text("b%d ig %u/%u", r.bsp, r.inst, r.surf);
							else ImGui::Text("cat %d", r.cat);
							ImGui::TableNextColumn(); if (r.hasTim) ImGui::Text("%.4f", r.w); else ImGui::Text("-");
							ImGui::TableNextColumn(); ImGui::Text("%.4f", r.trueDist);
							ImGui::TableNextColumn(); if (r.hasTim) ImGui::Text("%d", r.age); else ImGui::Text("-");
							ImGui::TableNextColumn();
							if (r.stale) ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "SKIPPED+TOUCHING");
							else if (r.over > settings.ghostOverMin && r.hasTim) ImGui::TextColored(ImVec4(1, 0.65f, 0, 1), "+%.4f", r.over);
							else if (r.skipped) ImGui::Text("skip");
							else ImGui::Text(r.hasTim ? "tested" : "contact");
						}
						ImGui::EndTable();
					}
				}
			}
			ImGui::Separator();
			ImGui::Text("GHOST debug %s  believed poses %s  trail %s  skipped edges %s", settings.ghost ? "on" : "off", settings.ghostBelieved ? "on" : "off",
				settings.ghostTrail ? "on" : "off", settings.ghostEdges ? "on" : "off");
			if (settings.ghost && mGP3.haveLatest && mGP3.latest.ok)
			{
				const ImVec4 col = gi.red ? ImVec4(1, 0.2f, 0.2f, 1) : (gi.lime ? ImVec4(0.67f, 1, 0, 1) : ImVec4(1, 0.41f, 0.71f, 1));
				const h3::Ghost3& g = mGP3.latest;
				ImGui::TextColored(col, "P0 [biped]  tick %u  step move %.4f wu  Havok-Halo %.4f wu  jumps %u%s", g.tick, gi.stepMove, gi.dHalo, mGP3.jumps,
					mGP3.haveJump && g.tick - mGP3.jumpTick <= (uint32_t)settings.ghostHold ? "  (JUMP hold)" : "");
				if (mGP3.haveJump) ImGui::Text("    last jump %.4f wu, %u ticks ago, Halo position %s", mGP3.jumpDist, g.tick - mGP3.jumpTick, mGP3.jumpHalo ? "moved with it (placement / teleport)" : "did NOT move (Havok-only reposition)");
				else ImGui::Text("    no jump seen yet");
				ImGui::TextDisabled("    motion type %u, shape vtable RVA 0x%X, v = (%.3f %.3f %.3f)", g.m.type, g.motionShapeVt < 0 ? 0 : g.motionShapeVt, g.m.v.x, g.m.v.y, g.m.v.z);
			}
			ImGui::TextDisabled("pink = nothing suspect   LIME = a cached w over-credits the gap   RED = polygon within tolerance is SKIPPED   white = Halo position   yellow = jump");
			if (settings.killVolumes && settings.infoKill)
			{
				ImGui::Separator();
				const h3::Death& d = mDeath3;
				if (!d.valid) ImGui::TextDisabled("OUT OF BOUNDS: %s", d.note.empty() ? "waiting for a game tick (is the game running?)" : d.note.c_str());
				else
				{
					if (d.counter > 0)
						ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.f, 1.f), "OUT OF BOUNDS: %d of %d ticks used - %d left. Halo 3 keeps this count until you die (it never drains back)",
							d.counter, d.limit + 1, std::max(0, d.limit + 1 - d.counter));
					else
						ImGui::TextDisabled("OUT OF BOUNDS: none used. The check starts only on ticks where (tick + player index %u) %% 16 == 0 - next in %d ticks; %d ticks inside solid kill",
							d.playerIndex, d.nextCheck, d.limit + 1);
					if (d.kvCount > 0)
					{
						ImGui::Text("KILL VOLUMES: %d (%d enabled)   one checked per tick: #%d this tick", d.kvCount, d.kvEnabled, d.kvChecking);
						if (d.kvInside.empty()) ImGui::TextDisabled("    you are not inside an enabled kill volume");
						for (const auto& in : d.kvInside)
							ImGui::TextColored(ImVec4(1.f, 0.9f, 0.f, 1.f), "    INSIDE kill volume #%d - checked in %d ticks; then every %d ticks", in.first, in.second, d.kvCount);
					}
					else ImGui::TextDisabled("KILL VOLUMES: none on this map");
				}
			}
			ImGui::Separator();
			ImGui::TextDisabled("options: HCM menu > Overlays > Collision Viewer");
		}
		ImGui::End();
	}
}
