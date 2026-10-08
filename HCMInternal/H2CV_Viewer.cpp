#include "pch.h"
#include "H2CV_Viewer.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include "H2CV_TimKeys.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace h2cv
{
	using h2::Vec3;

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
		constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
		const uint32_t kCatColor[kCatCount] = {
			rgba(0, 230, 255),    // BSP: cyan
			rgba(255, 50, 50),    // BSP invisible: red
			rgba(255, 140, 0),    // breakable: orange
			rgba(70, 120, 255),   // instanced: blue
			rgba(180, 70, 255),   // instanced invisible: purple
			rgba(255, 0, 255),    // scenery: magenta
			rgba(0, 255, 90),     // crates: green
			rgba(255, 130, 190),  // machines: pink
			rgba(255, 90, 0),     // kill triggers: red-orange
		};

		inline Vec3 sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		inline Vec3 add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		inline Vec3 mul(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline Vec3 cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		inline float len(const Vec3& a) { return std::sqrt(dot(a, a)); }
		inline Vec3 norm(const Vec3& a) { const float l = len(a); return l > 1e-9f ? mul(a, 1.f / l) : Vec3{ 0, 0, 1 }; }

		float pointSegDist2(const Vec3& p, const Vec3& a, const Vec3& b)
		{
			const Vec3 ab = sub(b, a);
			const float l2 = dot(ab, ab);
			const float t = l2 > 0 ? std::clamp(dot(sub(p, a), ab) / l2, 0.f, 1.f) : 0.f;
			const Vec3 d = sub(p, add(a, mul(ab, t)));
			return dot(d, d);
		}

		// closest distance^2 between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9)
		float segSegDist2(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2)
		{
			const Vec3 d1 = sub(q1, p1), d2 = sub(q2, p2), r = sub(p1, p2);
			const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
			float s, t;
			if (a <= 1e-12f && e <= 1e-12f) return dot(r, r);
			if (a <= 1e-12f) { s = 0; t = std::clamp(f / e, 0.f, 1.f); }
			else
			{
				const float c = dot(d1, r);
				if (e <= 1e-12f) { t = 0; s = std::clamp(-c / a, 0.f, 1.f); }
				else
				{
					const float b = dot(d1, d2), den = a * e - b * b;
					s = den != 0 ? std::clamp((b * f - c * e) / den, 0.f, 1.f) : 0.f;
					t = (b * s + f) / e;
					if (t < 0) { t = 0; s = std::clamp(-c / a, 0.f, 1.f); }
					else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.f, 1.f); }
				}
			}
			const Vec3 d = sub(add(p1, mul(d1, s)), add(p2, mul(d2, t)));
			return dot(d, d);
		}

		// distance from segment AB to a planar convex polygon (0 if it crosses)
		float segPolyDist(const Vec3& A, const Vec3& B, const std::vector<Vec3>& poly)
		{
			if (poly.size() < 3) return 1e9f;
			const Vec3 n = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
			auto inside = [&](const Vec3& p) {
				for (size_t i = 0; i < poly.size(); ++i)
				{
					const Vec3& a = poly[i]; const Vec3& b = poly[(i + 1) % poly.size()];
					if (dot(cross(sub(b, a), sub(p, a)), n) < -1e-6f) return false;
				}
				return true; };
			const float da = dot(sub(A, poly[0]), n), db = dot(sub(B, poly[0]), n);
			if ((da > 0) != (db > 0) && std::fabs(da - db) > 1e-9f)
			{
				const Vec3 x = add(A, mul(sub(B, A), da / (da - db)));
				if (inside(x)) return 0.f;
			}
			float best = 1e18f;
			for (const Vec3* p : { &A, &B })
			{
				const float h = dot(sub(*p, poly[0]), n);
				const Vec3 proj = sub(*p, mul(n, h));
				if (inside(proj)) best = std::min(best, h * h);
			}
			for (size_t i = 0; i < poly.size(); ++i)
				best = std::min(best, segSegDist2(A, B, poly[i], poly[(i + 1) % poly.size()]));
			return std::sqrt(best);
		}

		struct Label { float x, y; uint32_t col; char text[40]; };
		std::vector<Label> gLabels;

		void addLine(std::vector<LineVertex>& v, const Vec3& a, const Vec3& b, uint32_t c)
		{
			v.push_back({ a.x, a.y, a.z, c });
			v.push_back({ b.x, b.y, b.z, c });
		}

		// capsule wireframe (rings + meridians) and triangles
		void capsuleWire(std::vector<LineVertex>& v, const h2::Capsule& c, uint32_t col)
		{
			const Vec3 axis = norm(sub(c.b, c.a));
			const Vec3 t1 = norm(std::fabs(axis.z) < 0.9f ? cross(axis, Vec3{ 0, 0, 1 }) : cross(axis, Vec3{ 1, 0, 0 }));
			const Vec3 t2 = cross(axis, t1);
			constexpr int N = 16;
			for (int i = 0; i < N; ++i)
			{
				const float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
				const Vec3 r0 = add(mul(t1, std::cos(a0) * c.r), mul(t2, std::sin(a0) * c.r));
				const Vec3 r1 = add(mul(t1, std::cos(a1) * c.r), mul(t2, std::sin(a1) * c.r));
				addLine(v, add(c.a, r0), add(c.a, r1), col);
				addLine(v, add(c.b, r0), add(c.b, r1), col);
			}
			for (int m = 0; m < 4; ++m)
			{
				const float ang = 1.5707963f * m;
				const Vec3 dir = add(mul(t1, std::cos(ang)), mul(t2, std::sin(ang)));
				addLine(v, add(c.a, mul(dir, c.r)), add(c.b, mul(dir, c.r)), col);
				for (int k = 0; k < 8; ++k)
				{
					const float b0 = 1.5707963f * k / 8, b1 = 1.5707963f * (k + 1) / 8;
					auto cap = [&](const Vec3& centre, float sgn, float bb) {
						return add(centre, add(mul(dir, std::cos(bb) * c.r), mul(axis, sgn * std::sin(bb) * c.r))); };
					addLine(v, cap(c.b, 1, b0), cap(c.b, 1, b1), col);
					addLine(v, cap(c.a, -1, b0), cap(c.a, -1, b1), col);
				}
			}
		}

		void capsuleTris(std::vector<Tri>& out, const h2::Capsule& c)
		{
			const Vec3 axis = norm(sub(c.b, c.a));
			const Vec3 t1 = norm(std::fabs(axis.z) < 0.9f ? cross(axis, Vec3{ 0, 0, 1 }) : cross(axis, Vec3{ 1, 0, 0 }));
			const Vec3 t2 = cross(axis, t1);
			constexpr int N = 12, M = 4;
			auto pt = [&](int i, int j) -> Vec3 {
				// j in [-M, M]: latitude rings; j<0 bottom hemisphere around A, j>=0 top around B
				const float ang = 6.2831853f * i / N;
				const float lat = 1.5707963f * (float)std::abs(j) / M;
				const Vec3 radial = add(mul(t1, std::cos(ang)), mul(t2, std::sin(ang)));
				const Vec3 centre = j < 0 ? c.a : c.b;
				const float s = j < 0 ? -1.f : 1.f;
				return add(centre, add(mul(radial, std::cos(lat) * c.r), mul(axis, s * std::sin(lat) * c.r)));
			};
			for (int j = -M; j < M; ++j)
				for (int i = 0; i < N; ++i)
				{
					int ja = j, jb = j + 1;
					Vec3 p00 = pt(i, ja), p10 = pt(i + 1, ja), p01 = pt(i, jb), p11 = pt(i + 1, jb);
					if (j == -1) { p01 = pt(i, 0); p11 = pt(i + 1, 0); } // cylinder band between the hemispheres
					out.push_back({ p00, p10, p11 });
					out.push_back({ p00, p11, p01 });
				}
		}

		uint32_t timColor(float over, bool skipping, int manifold)
		{
			if (manifold > 0) return rgba(255, 255, 255);
			if (!skipping) return rgba(150, 150, 150);
			if (over > 0.02f) return rgba(255, 30, 30);    // over-credited: Havok believes it is much farther than it is
			if (over > 0.005f) return rgba(255, 170, 0);
			return rgba(255, 255, 0);                       // skipping honestly
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
			for (const auto& l : mWorld.lines((Cat)c))
			{
				if (pointSegDist2(cam.pos, l.a, l.b) > R2) continue;
				addLine(mScratch, l.a, l.b, kCatColor[c]);
			}
		}
		mR.drawLines(mScratch.data(), mScratch.size(), settings.lineWidth, settings.pull, own, game, settings.hiddenAlpha);
	}

	void Viewer::drawPills()
	{
		std::vector<h2::ObjectRef> objs;
		h2::enumerateObjects(objs);
		const uintptr_t me = h2::localPlayerUnitObject();
		mScratch.clear();
		for (const auto& o : objs)
		{
			if (o.type != h2::Biped) continue;
			const uintptr_t ent = h2::havokEntityForObject(o.obj);
			if (!ent) continue;
			const h2::Capsule c = h2::entityCapsule(ent);
			if (!c.ok) continue;
			capsuleWire(mScratch, c, o.obj == me ? rgba(255, 230, 0) : rgba(255, 255, 255, 200));
		}
		// pills are drawn through walls (like Cartographer's), thin
		mR.drawLines(mScratch.data(), mScratch.size(), 1.5f, 0.f, false, false, 0.f);
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
		ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (ImGui::Begin("Collision Viewer##h2cv", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
		{
			ImGui::Text("occlusion: %s", occlusionName((Occlusion)settings.occlusion.load()));
			if (!mInFrameNote.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "%s", mInFrameNote.c_str());
			ImGui::Text("width %.1fpx  pull %.4f  hidden %.2f  radius %.0f",
				settings.lineWidth.load(), settings.pull.load(), settings.hiddenAlpha.load(), settings.nearRadius.load());
			for (int c = 0; c < kCatCount; ++c)
			{
				const uint32_t col = kCatColor[c];
				ImGui::TextColored(ImVec4((col & 0xFF) / 255.f, ((col >> 8) & 0xFF) / 255.f, ((col >> 16) & 0xFF) / 255.f, 1),
					"%s %s  %zu", settings.show[c] ? "[x]" : "[ ]", catName((Cat)c), mWorld.lines((Cat)c).size());
				if (c % 3 != 2 && c != kCatCount - 1) ImGui::SameLine();
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
		const bool reversed = settings.gameZReversed.load();
		const bool wantObjects = settings.show[(int)Cat::Scenery] || settings.show[(int)Cat::Crate] || settings.show[(int)Cat::Machine];
		mWorld.update(wantObjects, settings.show[(int)Cat::Breakable]);
		if (!mWorld.valid()) return false;
		mR.setStandardZ(!reversed);
		if (!mR.beginExternal(mCtx, rtv, dsv, vp, gameViewProj(f, reversed))) return false;
		mR.setStandardZ(!reversed);
		drawCollision(f.pos, true);
		if (settings.tim) drawTim();
		if (settings.pills) drawPills();
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
		const bool gameOk = h2::base() && h2::buildOk();
		h2::Camera cam;
		if (gameOk) cam = h2::readCamera();
		mTick = gameOk ? h2::gameTick() : 0;
		const int occ = settings.occlusion.load();
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
			const bool wantObjects = own || settings.show[(int)Cat::Scenery] || settings.show[(int)Cat::Crate] || settings.show[(int)Cat::Machine];
			mWorld.update(wantObjects, settings.show[(int)Cat::Breakable]);
			mLabelScaleX = mLabelScaleY = 1.f;
			if (mWorld.valid() && mR.begin(mCtx, mRtv, mW, mH, cam.pos, cam.fwd, cam.up, cam.vfov, 0.01f))
			{
				if (own)
				{
					mR.clearOwnDepth();
					mR.setStaticOccluders(mWorld.staticOccluders(), h2::structureBsp() ^ (mWorld.staticOccluders().size() << 1));
					mR.drawStaticOccluders();
					mR.drawOccluders(mWorld.objectOccluders());
					if (settings.bipedOccluders)
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
				if (settings.tim) drawTim();
				if (settings.pills) drawPills();
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
				if (!h2::base()) { dl->AddText(ImVec2(12, 12), IM_COL32(255, 200, 0, 255), "Collision Viewer: halo2.dll not loaded"); }
				else if (!h2::buildOk()) { dl->AddText(ImVec2(12, 12), IM_COL32(255, 60, 60, 255), "Collision Viewer: unsupported halo2.dll build (needs MCC 1.3528)"); }
				else drawHud(cam);
			}
		}
	}
}
