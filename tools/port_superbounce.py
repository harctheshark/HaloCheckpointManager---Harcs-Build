# Generates HCMInternal/H2CV_Superbounce.cpp (MCC Halo 2) from Cartographer's superbounce viewer section.
# The core (geometry scans, prediction, verdicts, seam ratings, recorder) is copied as is; the platform pieces are replaced.
# Every replacement asserts it matched exactly once, so a Cartographer change that moves one of them stops the script
# with the snippet it could not find instead of producing a half-ported file.
#
#   python port_superbounce.py <cartographer>/xlive/H2MOD/Modules/GhostViewer/GhostViewer.cpp HCMInternal/H2CV_Superbounce.cpp
import sys
if len(sys.argv) != 3:
    sys.exit('usage: port_superbounce.py <Cartographer GhostViewer.cpp> <output H2CV_Superbounce.cpp>')
SRC, OUT = sys.argv[1], sys.argv[2]
g = open(SRC, encoding='utf-8').read()
sec = g[g.index('\t/* ---- Superbounce viewer ----'):g.index('\t// Maintain charge state on checkpoints and reverts.')]

def match_end(s, i):
    """index just past the brace block starting at the first '{' at or after i (skips strings, chars, comments)"""
    j = s.index('{', i); depth = 0; k = j
    while k < len(s):
        c = s[k]
        if s.startswith('//', k): k = s.index('\n', k); continue
        if s.startswith('/*', k): k = s.index('*/', k) + 2; continue
        if c == '"':
            k += 1
            while s[k] != '"': k += 2 if s[k] == '\\' else 1
            k += 1; continue
        if c == "'":
            k += 1
            while s[k] != "'": k += 2 if s[k] == '\\' else 1
            k += 1; continue
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return k + 1
        k += 1
    raise ValueError('unbalanced')

def replace_function(sig, new):
    global sec
    assert sec.count(sig) == 1, ('sig', sig)
    i = sec.index(sig); j = match_end(sec, i)
    sec = sec[:i] + new + sec[j:]

def rep(old, new, n=1):
    global sec
    assert sec.count(old) == n, (sec.count(old), old[:100]); sec = sec.replace(old, new)

# ---- the geometry cache: HCM's parsed world / instanced geometry, through the collision_bsp adapters
replace_function('\t\tvoid sb_bsp_update()\n', r'''		void sb_bsp_update()
		{
			// HCM: the resident structure bsp's collision (CollisionWorld) and its instanced geometry, rebuilt on every bsp load
			static uintptr_t s_id = 0;
			if (!g_world || !g_world->valid()) { g_sbb = s_sb_bsp{}; s_id = 0; return; }
			if (g_world->sbspId() == s_id && g_sbb.bsp) return;
			s_id = g_world->sbspId();
			g_sbb = s_sb_bsp{};
			g_sb_shown.clear(); g_sb_plan_tick = 0xFFFFFFFF;
			g_bsp_copies.clear();
			const collision_bsp* w = sb_adapt(g_world->worldBsp());
			g_sbb.bsp = w; g_sbb.surface_count = w->surfaces.count; g_sbb.edge_count = w->edges.count;
			sb_add_bsp(w, nullptr, 1.f, v3{ 0.f, 0.f, 0.f });
			// instanced geometry: each instance's definition collision, into world space (forward / left / up, scale, position)
			const auto& defs = g_world->igDefs();
			const auto& inst = g_world->igInstances();
			g_sbb.instance_count = (int32)inst.size();
			g_sbb.inst_base.assign(inst.size(), 0xFFFFFFFFu);
			std::vector<const collision_bsp*> adapted(defs.size(), nullptr);
			for (size_t i = 0; i < inst.size(); ++i)
			{
				const int d = inst[i].def;
				if (d < 0 || (size_t)d >= defs.size() || defs[(size_t)d].surfaces.empty()) continue;
				if (!adapted[(size_t)d]) adapted[(size_t)d] = sb_adapt(defs[(size_t)d]);
				g_sbb.inst_base[i] = (uint32)g_sbb.surf.size();
				const float* m = inst[i].m;           // [0] scale, [1..3] forward, [4..6] left, [7..9] up, [10..12] position
				sb_add_bsp(adapted[(size_t)d], m + 1, m[0], v3{ m[10], m[11], m[12] });
			}
			sb_find_gaps();
			sb_find_fins();
			sb_find_junctions();
			sb_mark_slots();
			g_sb_fins_logged = false;
			sb_spots_on_map();                    // this map's remembered monster / dreadnought spots
			g_sbb.obj_surf = g_sbb.surf.size(); g_sbb.obj_seam = g_sbb.seams.size(); g_sb_obj_sig = 0;   // objects come after
		}''')
# ---- live contact constraints: the 64-bit layout (slots, pairing flag) is not mapped on MCC yet
# ---- live contact constraints, MCC halo2.dll 1.3528 (64-bit) - mapped against Cartographer's 32-bit halo2.exe:
#   hkSimpleContactConstraint (RTTI) primary vtable RVA 0xB73720 (32-bit 0x3F3BD0: entry 9 calls the schema builder in both);
#   subclasses built by the same constructor sub_1809E5D80: hkMassChangerConstraint 0xB72B78, hkMovingSurfaceConstraint 0xB72C00
#   entity +0xD8 / +0xE0: its constraints (hkEntity::addConstraint sub_1809CE4E0)            32-bit +0x88 / +0x8C
#   constraint +0x18 / +0x20: entity A / B (setters sub_1809E5C50 / sub_1809E5CD0)           32-bit +0x0C / +0x10
#   constraint +0x50 / +0x58: contact points, 0x20 each (position, normal + distance)        32-bit +0x34 / +0x38
#   constraint +0xB0 / +0xB8: solver slots, 0x20 each (in-place, 4 inline at +0xC0)         32-bit +0x90 / +0x94, 0x1C each
#   slot +0x1C (u16): coupled with the previous point - the schema builder sub_1809EBF70 tests it before writing 0x30402
#   (single) or 0x50803 (pair); 32-bit slot +0x18. Slot floats: +0x00 / +0x04 / +0x08 as 32-bit; +0x18 = 32-bit +0x14.
replace_function('\t\tvoid sb_read_pairs(uintptr_t entity, v3 center, std::vector<s_sb_pair>& out)', r'''		void sb_read_pairs(uintptr_t entity, v3 center, std::vector<s_sb_pair>& out)
		{
			out.clear();
			const uintptr_t list = mem::q(entity + k_mcc_entity_constraints);
			const int32 nc = mem::i32(entity + k_mcc_entity_constraints + 8);
			if (!mem::okp(list) || nc <= 0 || nc > 256) return;   // removeConstraint (sub_1809CE6A0) nulls entries, it does not compact
			for (int32 k = 0; k < nc; ++k)
			{
				const uintptr_t c = mem::q(list + 8 * (uintptr_t)k);
				if (!sb_is_contact_constraint(c)) continue;
				const uintptr_t pts = mem::q(c + 0x50), slots = mem::q(c + 0xB0);
				const int32 np = mem::i32(c + 0x58), nslots = mem::i32(c + 0xB8);
				if (!mem::okp(pts) || !mem::okp(slots) || np < 2 || np > 16 || nslots < np) continue;
				for (int32 i = 1; i < np; ++i)
				{
					uint16 flag = 0;
					if (!mem::copy(&flag, slots + (uintptr_t)i * 0x20 + 0x1C, sizeof(flag)) || flag == 0) continue;   // coupled with point i - 1
					float pa[8], pb[8];
					if (!mem::copy(pa, pts + (uintptr_t)(i - 1) * 0x20, sizeof(pa)) || !mem::copy(pb, pts + (uintptr_t)i * 0x20, sizeof(pb))) continue;
					s_sb_pair pr;
					pr.p0 = { pa[0], pa[1], pa[2] }; pr.n0 = { pa[4], pa[5], pa[6] };
					pr.p1 = { pb[0], pb[1], pb[2] }; pr.n1 = { pb[4], pb[5], pb[6] };
					if (dot(center - pr.p0, pr.n0) < 0.f) pr.n0 = pr.n0 * -1.f;    // as pushes on the pill
					if (dot(center - pr.p1, pr.n1) < 0.f) pr.n1 = pr.n1 * -1.f;
					const float cc = 0.99f * dot(pr.n0, pr.n1);
					pr.factor = (1.f + cc) > 1e-4f ? len(pr.n0 + pr.n1) / (1.f + cc) : 0.f;
					out.push_back(pr);
				}
			}
		}''')
sig_contacts = [l for l in sec.split('\n') if l.startswith('\t\tvoid sb_read_contacts(')][0]
replace_function(sig_contacts, sig_contacts + r'''
		{
			out.clear();
			const uintptr_t list = mem::q(entity + k_mcc_entity_constraints);
			const int32 nc = mem::i32(entity + k_mcc_entity_constraints + 8);
			if (!mem::okp(list) || nc <= 0 || nc > 256) return;   // removeConstraint (sub_1809CE6A0) nulls entries, it does not compact
			for (int32 k = 0; k < nc; ++k)
			{
				const uintptr_t c = mem::q(list + 8 * (uintptr_t)k);
				if (!sb_is_contact_constraint(c)) continue;
				const uintptr_t pts = mem::q(c + 0x50), slots = mem::q(c + 0xB0);
				const int32 np = mem::i32(c + 0x58), nslots = mem::i32(c + 0xB8);
				if (!mem::okp(pts) || !mem::okp(slots) || np < 1 || np > 16 || nslots < np) continue;
				const uintptr_t ea = mem::q(c + 0x18), eb = mem::q(c + 0x20);
				for (int32 i = 0; i < np; ++i)
				{
					s_sb_contact sc;
					sc.con = (int)k; sc.pt = (int)i;
					sc.partner = (uint32)((ea == entity) ? eb : ea);     // low 32 bits, as the 32-bit log printed it
					uint8 raw[0x20] = {};
					if (!mem::copy(sc.p, pts + (uintptr_t)i * 0x20, sizeof(sc.p)) || !mem::copy(raw, slots + (uintptr_t)i * 0x20, sizeof(raw))) continue;
					// the 32-bit recorder's six slot dwords: +0x00 +0x04 +0x08 +0x0C, the low half of the 8-byte field at +0x10, +0x18
					static const int k_slot_at[6] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x18 };
					for (int f = 0; f < 6; ++f) memcpy(&sc.slot[f], raw + k_slot_at[f], 4);
					memcpy(&sc.flag, raw + 0x1C, 4);
					out.push_back(sc);
				}
			}
		}''')
# ---- crouch charge: HCM's (body, level) pair and its penetration level
replace_function('\t\tbool sb_charged(uintptr_t own, const std::vector<s_pair>& pairs)', r'''		bool sb_charged(uintptr_t ent)
		{
			// the body's pair with the level held at penetration level -3 (MCC pair entry: 32 bytes, level at +0x18), with a
			// live agent - at rest the island sleeps, its pair keeps a stale -3 and the charge is gone
			const h2::PairInfo p = h2::findBspPair(ent);
			return sb_live(p) && p.S == -3;
		}''')
rep('const bool charged = own && sb_charged(own, pairs);', 'const bool charged = own && sb_charged(own);')
# ---- recorder: helper lines from HCM's welder children
rep('uintptr_t own, float dvz, v3 dvh, float best_factor, const std::vector<s_pair>& pairs)', 'uintptr_t own, float dvz, v3 dvh, float best_factor)')
i = sec.index('\t\t\t// the collision helpers of your pairs (one per nearby triangle)')
j = match_end(sec, sec.index('for (size_t pi = 0; pi < pairs.size(); ++pi)', i))
sec = sec[:i] + r'''			// the collision helpers of your pair with the level (one per nearby triangle): in contact, or within 0.5 wu by their TIM
			{
				const h2::PairInfo pr = h2::findBspPair(own);
				std::vector<h2::Child> kids;
				if (pr.found && h2::readChildren(pr.agent, kids))
					for (const h2::Child& k : kids)
						if (k.manifold > 0 || std::fabs(k.tim) < 0.5f)
						{
							v3 fn{ 0.f, 0.f, 0.f };
							sb_face_normal(k.key, fn);
							fprintf(g_sb_log, "H,%u,%d,%08x,%d,%.4f,%.4f,%.4f,%.5f,%d,%.4f,%.4f,%.4f\n", (unsigned)tick, 0, (unsigned)k.key, (int)k.manifold,
								k.normal.x, k.normal.y, k.normal.z, k.tim, (int)pr.S, fn.x, fn.y, fn.z);
						}
			}''' + sec[j:]
rep('sb_log_tick(tick, tps, feet, vel, charged, airborne, own, dvz, dvh, live_best, pairs);', 'sb_log_tick(tick, tps, feet, vel, charged, airborne, own, dvz, dvh, live_best);')
# ---- the overlay is drawn at Havok's pose: no render offset to ride on
replace_function('\t\tv3 sb_render_shift(datum unit)', r'''		v3 sb_render_shift(datum unit)
		{
			(void)unit;
			return v3{ 0.f, 0.f, 0.f };
		}''')
rep('sb_render_shift(pl->unit_index)', 'sb_render_shift(NONE)')
# ---- the local player's state: from HCM (g_sbs, filled by frame())
i = sec.index('\t\tvoid draw_superbounce(int32 user, const std::vector<s_pair>& pairs)\n')
j = sec.index('\t\t\tconst v3 center = feet + v3{ 0.f, 0.f, 0.36f };\n', i) + len('\t\t\tconst v3 center = feet + v3{ 0.f, 0.f, 0.36f };\n')
sec = sec[:i] + r'''		void draw_superbounce(int32 user)
		{
			if (user != 0 || !g_sbs.ok) return;                 // the local player only
			sb_bsp_update();
			const v3 feet = g_sbs.feet;
			const v3 vel = g_sbs.vel;
			const int tps = g_sbs.tps;
			const float dt = 1.f / (float)tps, vsweep = k_sb_sweep_per_tick * (float)tps;
			g_sb_tick_scale = (float)tps / 30.f;                     // launches (and the limits naming them) scale with the tick rate
			const bool airborne = g_sbs.airborne;
			const uintptr_t own = g_sbs.ent;
			const bool charged = own && sb_charged(own);
			const v3 center = feet + v3{ 0.f, 0.f, 0.36f };
''' + sec[j:]
rep('if (own) sb_read_pairs(owner_of(own), center, live);', 'if (own) sb_read_pairs(own, center, live);')
rep('\t\t\tconst uint32 tick = okp(time_globals) ? u32(time_globals + 8) : 0;\n', '\t\t\tconst uint32 tick = g_sbs.tick;\n')
rep('sb_read_contacts(owner_of(own), cs);', 'sb_read_contacts(own, cs);', 2)
rep('''				xform bx; s_capsule lc;
				if (own && body_pose(own, bx) && read_capsule(u32(own - 0x10), lc) && lc.r > 0.05f && lc.r < 1.f)
				{
					ca = xf_point(bx, lc.a); cb = xf_point(bx, lc.b); cr = lc.r;
				}''', '''				const h2::Capsule hc = own ? h2::entityCapsule(own) : h2::Capsule{};
				if (hc.ok && hc.r > 0.05f && hc.r < 1.f)
				{
					ca = v3{ hc.a.x, hc.a.y, hc.a.z }; cb = v3{ hc.b.x, hc.b.y, hc.b.z }; cr = hc.r;
				}''')
assert 's_pair' not in sec.replace('s_pair_', ''), [l for l in sec.split('\n') if 's_pair' in l and 's_sb_pair' not in l][:5]

prelude = r'''#include "pch.h"
// Superbounce viewer for Halo 2 (MCC 1.3528) - generated by port_superbounce.py from Cartographer's GhostViewer.cpp
// superbounce section; edit there and regenerate. The core (geometry scans, landing prediction, verdicts, seam ratings,
// recorder) is Cartographer's; the platform pieces come from HCM: the collision geometry (CollisionWorld's parsed
// collision_bsps through Cartographer-shaped adapters), the local player (h2::), the crouch charge (the (body, level)
// pair's penetration level), the welder's children (recorder H lines), the live contact constraints (coupled pairs,
// their factors, the recorder's P lines; 64-bit layout mapped against the 32-bit build, see sb_read_pairs).
#include "H2CV_Superbounce.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include "imgui.h"
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace h2cv::sb
{
	namespace
	{
		typedef int8_t int8; typedef uint8_t uint8; typedef int16_t int16; typedef uint16_t uint16; typedef int32_t int32; typedef uint32_t uint32;
		typedef int32_t datum;
		constexpr int32 NONE = -1;

		struct v3 { float x, y, z; };
		inline v3 operator+(v3 a, v3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		inline v3 operator-(v3 a, v3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		inline v3 operator*(v3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline v3 cross(v3 a, v3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		inline float len(v3 a) { return std::sqrt(dot(a, a)); }
		inline v3 norm(v3 a) { const float l = len(a); return l > 1e-12f ? a * (1.f / l) : v3{ 0.f, 0.f, 0.f }; }

		// this frame's output and settings (frame() points / fills them)
		Output* g_out = nullptr;
		struct s_settings_sb { bool superbounce = true, superbounce_record = false, superbounce_gaps = false, superbounce_seam_text = true, superbounce_debounce = true, superbounce_spots = true; } g_settings;
		void line(v3 a, v3 b, ImU32 c, float thick = 1.5f)
		{
			if (!g_out) return;
			auto& v = thick >= 2.5f ? g_out->thick : g_out->thin;
			v.push_back({ a.x, a.y, a.z, c }); v.push_back({ b.x, b.y, b.z, c });
		}
		void text(v3 p, ImU32 c, const char* t) { if (g_out) g_out->labels.push_back({ h2::Vec3{ p.x, p.y, p.z }, c, t }); }
		// the Info panel's superbounce section and the HUD's charge state
		struct s_sb_line { ImVec4 c; std::string t; };
		std::vector<s_sb_line> g_sb_panel;
		int g_sb_hud = -1;
		float g_sb_speed_now = -1.f;     // your speed this frame (u/s) for the HUD; -1 = none

		// XInput, loaded at run time (the stick, for air control)
		typedef DWORD(WINAPI* t_xinput_get_state)(DWORD, XINPUT_STATE*);
		t_xinput_get_state xinput_get_state()
		{
			static t_xinput_get_state fn = nullptr; static bool tried = false;
			if (!tried)
			{
				tried = true;
				for (const char* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
					if (HMODULE m = LoadLibraryA(dll)) { fn = (t_xinput_get_state)GetProcAddress(m, "XInputGetState"); if (fn) break; }
			}
			return fn;
		}

		// Cartographer's collision_bsp shapes over HCM's parsed CollBsp (Halo 2 collision bsps use 16-bit indices)
		struct real_point3d { float x, y, z; };
		struct real_plane3d { float n[3]; float d; };
		struct collision_surface { int16 plane; uint32 first_edge; };
		struct collision_edge { uint16 start_vertex, end_vertex, forward_edge, reverse_edge, left_surface, right_surface; };
		struct collision_vertex { real_point3d point; };
		template <class T> struct sb_block { int32 count = 0; const T* data = nullptr; const T* operator[](int64_t i) const { return data + i; } };
		struct collision_bsp { sb_block<real_plane3d> planes; sb_block<collision_surface> surfaces; sb_block<collision_edge> edges; sb_block<collision_vertex> vertices; };
		struct sb_bsp_copy { std::vector<real_plane3d> P; std::vector<collision_surface> S; std::vector<collision_edge> E; std::vector<collision_vertex> V; collision_bsp bsp; };
		std::vector<std::unique_ptr<sb_bsp_copy>> g_bsp_copies;
		const CollisionWorld* g_world = nullptr;
		const collision_bsp* sb_adapt(const CollBsp& c)
		{
			auto k = std::make_unique<sb_bsp_copy>();
			for (size_t i = 0; i + 3 < c.planes.size(); i += 4) k->P.push_back({ { c.planes[i], c.planes[i + 1], c.planes[i + 2] }, c.planes[i + 3] });
			for (const auto& s : c.surfaces) k->S.push_back({ (int16)(uint16)((s.plane & 0x7FFF) | (s.flip ? 0x8000 : 0)), s.firstEdge });
			for (const auto& e : c.edges) k->E.push_back({ (uint16)e.v0, (uint16)e.v1, (uint16)e.fwd, (uint16)e.rev, (uint16)e.left, (uint16)e.right });
			for (const auto& v : c.verts) k->V.push_back({ { v.x, v.y, v.z } });
			k->bsp.planes = { (int32)k->P.size(), k->P.data() }; k->bsp.surfaces = { (int32)k->S.size(), k->S.data() };
			k->bsp.edges = { (int32)k->E.size(), k->E.data() }; k->bsp.vertices = { (int32)k->V.size(), k->V.data() };
			g_bsp_copies.push_back(std::move(k));
			return &g_bsp_copies.back()->bsp;
		}

		// MCC halo2.dll 1.3528: the contact constraints Havok solves (see sb_read_pairs for how each offset was found)
		constexpr uintptr_t k_mcc_entity_constraints = 0xD8;                   // hkEntity: constraint pointers, count at +8
		constexpr uintptr_t k_mcc_vt_contact[] = { 0xB73720, 0xB72B78, 0xB72C00 };   // hkSimpleContactConstraint (+ mass changer, moving surface)
		bool sb_is_contact_constraint(uintptr_t c)
		{
			if (!mem::okp(c)) return false;
			const uintptr_t vt = mem::q(c), base = h2::base();
			if (!base || vt < base) return false;
			for (uintptr_t v : k_mcc_vt_contact) if (vt - base == v) return true;
			return false;
		}

		// a pair that is really there: a sleeping island keeps its pair entries with the null agent and a stale level
		// (that is where a charge is lost at rest) - halo2.dll + 0xE1A510, h2::vt::NULL_AGENT_INST
		bool sb_live(const h2::PairInfo& p) { return p.found && mem::okp(p.agent) && p.agent != h2::base() + h2::vt::NULL_AGENT_INST; }

		// the local player this frame, read through h2:: by frame()
		struct s_sb_state { bool ok = false; v3 feet{}, vel{}; bool airborne = false; uintptr_t ent = 0; int tps = 30; uint32 tick = 0; };
		s_sb_state g_sbs;
	}
'''

glue = r'''
	namespace
	{

		// Maintain charge state on checkpoints and reverts. Both rebuild the physics: the (body, level) pair comes back at
		// penetration level 0 and the charge is gone. MCC halo2.dll 1.3528:
		//   every checkpoint save (sub_1806AF600, sub_1806B0260 - the forced / natural one -, sub_1806B03C0) stamps the game
		//   tick into +0x15A3D68 (nothing else writes it) and flips the slot word +0x15A0B04 (sub_1806B0260 can save into both
		//   slots: the word ends where it started); a double revert (sub_1806AFDE0) flips the slot without saving.
		// Saving while charged gives the rebuilt pair its -3 back and remembers the slot as charged; a revert restores the charge
		// if you were charged as you reverted, or when the checkpoint you went back to was made.
		constexpr uintptr_t k_mcc_save_tick = 0x15A3D68, k_mcc_save_slot = 0x15A0B04;
		struct s_keep
		{
			bool init = false, charged = false, pending = false; uint32 tick = 0, until = 0; const char* why = "";
			uint32 save_tick = 0; uint16 slot = 0; bool slot_charged[2] = {}; uintptr_t sbsp = 0;
		} g_keep;
		std::atomic<bool> g_keep_forget{ false };   // HCM injected a checkpoint: what the slots held is unknown now
		void keep_charge(Output& out)
		{
			const h2::PairInfo pr = h2::findBspPair(g_sbs.ent);
			const bool live = sb_live(pr), charged = live && pr.S == -3;
			const uintptr_t b = h2::base();
			const uint32 save_tick = mem::u32(b + k_mcc_save_tick, 0);
			const uint16 slot = (uint16)(mem::u16(b + k_mcc_save_slot, 0) & 1);
			const uintptr_t sbsp = g_world ? g_world->sbspId() : 0;
			if (g_keep_forget.exchange(false)) g_keep.slot_charged[0] = g_keep.slot_charged[1] = false;
			if (!g_keep.init || sbsp != g_keep.sbsp)
			{
				// first look, or a new level / bsp: the old pair is gone for good - start over, restore nothing
				g_keep = s_keep{};
				g_keep.init = true; g_keep.charged = charged; g_keep.tick = g_sbs.tick; g_keep.save_tick = save_tick; g_keep.slot = slot; g_keep.sbsp = sbsp;
				return;
			}
			if (g_keep.tick != 0 && g_sbs.tick + 2 < g_keep.tick)
			{
				// time went back: a revert (a double revert has flipped the slot already)
				if (g_keep.charged || g_keep.slot_charged[slot]) { g_keep.pending = true; g_keep.until = g_sbs.tick + 90; g_keep.why = "revert"; }
			}
			else if (save_tick != g_keep.save_tick)
			{
				// a checkpoint was just saved (yours or the game's): it carries the charge you had the frame before it
				const bool was = g_keep.charged;
				if (slot == g_keep.slot) g_keep.slot_charged[0] = g_keep.slot_charged[1] = was;   // saved into both slots
				else g_keep.slot_charged[slot] = was;
				if (was) { g_keep.pending = true; g_keep.until = g_sbs.tick + 90; g_keep.why = "checkpoint"; }
			}
			g_keep.save_tick = save_tick; g_keep.slot = slot;
			if (g_keep.pending)
			{
				// the rebuilt pair can take a moment to come back (and a sleeping island has none): retried until it is live
				if (charged) { g_keep.pending = false; out.message = std::string("Charge kept through the ") + g_keep.why; }
				else if (g_sbs.tick > g_keep.until) { g_keep.pending = false; out.message = std::string("Could not restore the charge after the ") + g_keep.why; }
				else if (live) { const int32_t level = -3; mem::put(pr.entry + 0x18, &level, sizeof(level)); }
			}
			else g_keep.charged = charged;
			g_keep.tick = g_sbs.tick;
		}
		// Force Charged state: the live pair with the level is held at -3 (a sleeping island has none: charged again the moment
		// you move)
		void force_charge()
		{
			const h2::PairInfo pr = h2::findBspPair(g_sbs.ent);
			if (sb_live(pr) && pr.S != -3) { const int32_t level = -3; mem::put(pr.entry + 0x18, &level, sizeof(level)); }
		}
	}

	void frame(const CollisionWorld& world, const Options& opt, Output& out)
	{
		out = Output{};
		g_out = &out;
		g_world = &world;
		g_settings.superbounce = opt.on;
		g_settings.superbounce_record = opt.record;
		g_settings.superbounce_gaps = opt.highlight;
		g_settings.superbounce_seam_text = opt.seamText;
		g_settings.superbounce_debounce = opt.debounce;
		g_settings.superbounce_spots = opt.spots;
		g_sb_panel.clear();
		g_sb_hud = -1; g_sb_speed_now = -1.f;
		g_sbs = s_sb_state{};
		uintptr_t units[4] = {};
		h2::localPlayerUnits(units);
		if (units[0] && !h2::vehicleOfUnit(units[0]))
		{
			const uintptr_t ent = h2::havokEntityForObject(units[0]);
			const h2::Capsule cap = ent ? h2::entityCapsule(ent) : h2::Capsule{};
			h2::Motion m;
			if (ent && cap.ok && h2::readMotion(ent, m))
			{
				const h2::Vec3 lo = cap.a.z < cap.b.z ? cap.a : cap.b;
				g_sbs.feet = v3{ lo.x, lo.y, lo.z - cap.r };                     // the bottom of the pill
				g_sbs.vel = v3{ m.v.x, m.v.y, m.v.z };
				g_sbs.ent = ent;
				g_sbs.tick = h2::gameTick();
				// on the ground when the pair with the level has a contact on a floor-like face
				bool ground = false;
				const h2::PairInfo pr = h2::findBspPair(ent);
				std::vector<h2::Child> kids;
				if (pr.found && h2::readChildren(pr.agent, kids))
					for (const h2::Child& k : kids)
						if (k.manifold > 0 && std::fabs(k.normal.z) > 0.5f) ground = true;
				// at rest for two ticks counts too: a sleeping island has no collision agents, so no contacts to read
				static uint32 s_rest_tick = 0; static int s_rest = 0;
				if (g_sbs.tick != s_rest_tick) { s_rest = len(g_sbs.vel) < 0.02f ? s_rest + 1 : 0; s_rest_tick = g_sbs.tick; }
				if (s_rest >= 2) ground = true;
				g_sbs.airborne = !ground;
				// ticks per second: the game's own (time globals +2, i16); inferring it from gravity misread now and then
				const uintptr_t tgl = mem::q(h2::base() + h2::rva::GAME_TIME);
				const int game_tps = mem::okp(tgl) ? (int)(int16_t)mem::u16(tgl + 2, 0) : 0;
				g_sbs.tps = (game_tps >= 10 && game_tps <= 240) ? game_tps : 30;
				g_sbs.ok = true;
			}
		}
		if (opt.on && g_sbs.ok)
		{
			// scenery / crates / machines: their collision triangles (CollisionWorld's, world space) into the cache - after
			// the level, rebuilt only when one of them moves or the set changes (positions quantised to 1 cm)
			sb_bsp_update();
			const auto& ot = world.objectOccluders();
			std::vector<sb_tri> tris;
			tris.reserve(ot.size());
			uint64_t sig = 1469598103934665603ull ^ (uint64_t)ot.size();
			for (const auto& t : ot)
			{
				tris.push_back({ v3{ t.a.x, t.a.y, t.a.z }, v3{ t.b.x, t.b.y, t.b.z }, v3{ t.c.x, t.c.y, t.c.z } });
				for (const h2::Vec3* pt : { &t.a, &t.b, &t.c })
				{
					sig ^= (uint64_t)std::llround(pt->x * 100.f) * 31u + (uint64_t)std::llround(pt->y * 100.f) * 17u + (uint64_t)std::llround(pt->z * 100.f);
					sig *= 1099511628211ull;
				}
			}
			sb_objects_set(tris, sig == 0 ? 1 : sig);
		}
		if (opt.on) draw_superbounce(0);
		if (opt.forceCharge && g_sbs.ok) force_charge();
		else if (opt.keepCharge && g_sbs.ok) keep_charge(out);
		for (const auto& l : g_sb_panel) out.panel.push_back({ ImGui::ColorConvertFloat4ToU32(l.c), l.t });
		out.hud = g_sb_hud;
		out.speed = g_sb_speed_now;
		out.streak = g_sb_streak; out.record = g_sb_record;
		g_out = nullptr;
	}

	void forgetCheckpointCharges() { g_keep_forget.store(true); }

	void reset()
	{
		g_sbb = s_sb_bsp{};
		g_bsp_copies.clear();
		g_sb_shown.clear();
		sb_log_close();
	}
}
'''
# MCC catches falls of 40+ u/s a tick before impact and sets them down at 4.08 u/s (measured on Cairo Station)
assert sec.count('constexpr float k_sb_brake_speed = 0.f;') == 1
sec = sec.replace('constexpr float k_sb_brake_speed = 0.f;', 'constexpr float k_sb_brake_speed = 40.f;   // MCC: measured on Cairo Station (37.6 never, 43.5+ always)')
open(OUT, 'w', encoding='utf-8', newline='\r\n' if False else None).write(prelude + sec + glue)
print('wrote', OUT, len((prelude + sec + glue).split('\n')), 'lines')
