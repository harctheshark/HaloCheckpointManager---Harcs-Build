#include "pch.h"
#include "H2CV_Halo3.h"
#include "H2CV_Engine.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace mem;

namespace h3
{
	namespace
	{
		// ---- per-game layout ------------------------------------------------------------------------------------------
		// All RVAs are module RVAs (IDA EA - 0x180000000). ODST column values come from the research report's function transfer
		// (fn-hash map) and were re-checked in the ODST decompilation where noted in NOTES.md.
		struct Layout
		{
			const wchar_t* module;
			uint32_t peStamp, peSize;
			uint32_t rvaScenario;      // qword: scenario tag data
			uint32_t rvaHeap;          // qword: tag heap base ("magicAddress")
			uint32_t rvaTagInst;       // qword: tag instance array (8-byte elements, offset dword @+4)
			uint32_t rvaGameState;     // qword: game-state base G (HCM: playerDatum / objectMetaDataTable / cameraDataPointer are G + const)
			uint32_t rvaActiveMask;    // dword: active structure-bsp bitmask
			uint32_t rvaReversedZ;     // byte: reversed-Z depth in use
			uint32_t rvaHavokComps;    // qword: "havok components" data array header*
			uint32_t rvaWorld;         // qword: hkpWorld*
			uint32_t rvaTlsIndex;      // dword: implicit TLS index of the module
			uint32_t rvaResCtx;        // qword: resource manager context R (collision bsp resources)
			uint32_t rvaLevel;         // char[]: level code
			uint32_t rvaTickBase;      // 0 = tick lives at G + offTick, else qword at RVA is the base of the tick dword
			uint32_t offTick, offPlayer, offObjHdr, offCamera;
			uint32_t resDisp, resConst;      // resource table address = R + 0x2B440 + resDisp (resConst when that sum overflows to 0)
			uint32_t tlsPool;                // TLS-block offset of the breakable-surface pool pointer
			uint32_t holderOff;              // sbsp offset of the collision-bsp resource holder
			uint32_t igCountOff, igOffOff;   // sbsp instanced-geometry instance block {count, offset}
			uint32_t igDefStride;            // resource IG-definition record size
			uint32_t compStride, compRb;     // havok component record size; rigid-body array pointer offset (count @+8)
			uint32_t scnTrig, trigStride, trigFwd, trigUp, trigPos, trigExt;   // scenario trigger volume block + record fields
			uint32_t scnKill;                // scenario kill-volume index block {count, offset} (u16 trigger volume indices)
			uint32_t vtCapsule, vtMopp, vtInstMopp, vtBox, vtSphere, vtConvex, vtCxform, vtCxform2, vtCtranslate, vtList, vtHkList,
			         vtCollection, vtTransform, vtSurf;
		};

		const Layout kH3 = {
			L"halo3.dll", 0x68A0EDFCu, 0x4768000u,
			0xA46A08, 0x1FCF4C8, 0xA49018, 0x204FD18, 0x46B70A8, 0x46BB86C, 0x46B7710, 0x46B7688, 0xA39F9C, 0x8F4028, 0x20A9144,
			0, 0x77C8F0, 0x4806FC, 0x50749C, 0x72A4A8,
			0x578, 0xB8, 184,
			0x380, 0x1C0, 0x1C4, 196,
			0xC0, 0x20,
			0x258, 0x44, 0x0C, 0x18, 0x24, 0x30, 0x484,
			0x794260, 0x7FBC00, 0x794FA0, 0x7953B0, 0x7941E8, 0x795208, 0x7954A0, 0x7955F8, 0x794170, 0x817668, 0x795280, 0,
			0x8176E0, 0x7FBC58 };

		const Layout kOdst = {
			L"halo3odst.dll", 0x68A0F232u, 0x4797000u,
			0xA9C9B8, 0x2022AA8, 0xA9F0A8, 0x2094D48, 0x46E261C, 0x46E5B94, 0x46E29E0, 0x46E29D0, 0xA8FB1C, 0x932848, 0x20EF155,
			0x1FE9630, 0x10C, 0x48068C, 0x50A364, 0x7475E8,
			0x5A8, 0x88, 192,
			0x374, 0x1B4, 0x1B8, 184,
			0xB0, 0x18,
			0x28C, 0x7C, 0x10, 0x1C, 0x28, 0x34, 0x4C4,
			0x7F33C0, 0x83EA78, 0x7F2DD0, 0x7F3168, 0x7F3348, 0x7F3078, 0x7F3258, 0x7F3590, 0x7F32D0, 0x859970, 0x83EA38, 0x7F3000,
			0x859A20, 0x83EB08 };

		inline const Layout& L() { return h2cv::game() == h2cv::Game::Halo3ODST ? kOdst : kH3; }

		std::atomic<uintptr_t> gSimTls{ 0 };

		uintptr_t curTls()
		{
			const uint32_t idx = u32(base() + L().rvaTlsIndex, 0xFFFFFFFFu);
			if (idx == 0xFFFFFFFFu || idx > 1088) return 0;
			const uintptr_t arr = (uintptr_t)__readgsqword(0x58);   // TEB.ThreadLocalStoragePointer
			return okp(arr) ? q(arr + 8 * (uintptr_t)idx) : 0;
		}
		// block used for TLS-gated reads: the captured sim-thread block, else this thread's
		uintptr_t tlsBlock()
		{
			const uintptr_t s = gSimTls.load();
			return s ? s : curTls();
		}

		inline uintptr_t G() { return q(base() + L().rvaGameState); }
		inline bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

		constexpr uint32_t kDataArraySig = 0x64407440u;   // 'd@t@' @ +0x2C of every engine data array header
	}

	bool odst() { return h2cv::game() == h2cv::Game::Halo3ODST; }

	uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(L().module)); }

	bool buildOk()
	{
		static uintptr_t sBase = 0; static bool sOk = false; static bool sOdst = false;
		const uintptr_t b = base();
		if (!b) return false;
		if (b == sBase && sOdst == odst()) return sOk;
		sBase = b; sOdst = odst();
		IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
		sOk = false;
		if (copy(&dos, b, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && copy(&nt, b + dos.e_lfanew, sizeof(nt)))
		{
			sOk = nt.FileHeader.TimeDateStamp == L().peStamp && nt.OptionalHeader.SizeOfImage == L().peSize;
			LOGF("%s @ 0x%llX  PE timestamp 0x%08X size 0x%X  -> build check %s (expects 1.3528: 0x%08X / 0x%X)",
				odst() ? "halo3odst.dll" : "halo3.dll", (unsigned long long)b, nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage,
				sOk ? "OK" : "FAILED", L().peStamp, L().peSize);
		}
		return sOk;
	}

	uint32_t gameTick()
	{
		const Layout& l = L();
		uintptr_t at;
		if (l.rvaTickBase) { const uintptr_t p = q(base() + l.rvaTickBase); at = okp(p) ? p + l.offTick : 0; }
		else { const uintptr_t g = G(); at = okp(g) ? g + l.offTick : 0; }
		return at ? u32(at, 0) : 0;
	}

	std::string levelCode()
	{
		char s[33] = {};
		if (!copy(s, base() + L().rvaLevel, 32)) return "";
		std::string r;
		for (char c : s) { if (!c) break; r += (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '?'; }
		return r;
	}

	// ---- cameras ------------------------------------------------------------------------------------------------------
	h2::Camera readObserverCamera()
	{
		h2::Camera c;
		const uintptr_t g = G();
		if (!okp(g)) return c;
		float f[20];
		if (!copy(f, g + L().offCamera, sizeof(f))) return c;
		c.pos = { f[0], f[1], f[2] };
		c.fwd = { f[10], f[11], f[12] };   // +0x28
		c.up = { f[13], f[14], f[15] };    // +0x34
		c.vfov = f[16];                    // +0x40 (assumed vertical)
		c.nearClip = 0.01f; c.farClip = 2000.f;
		const float lf = c.fwd.x * c.fwd.x + c.fwd.y * c.fwd.y + c.fwd.z * c.fwd.z;
		const float lu = c.up.x * c.up.x + c.up.y * c.up.y + c.up.z * c.up.z;
		c.ok = std::isfinite(f[0]) && lf > 0.5f && lf < 1.5f && lu > 0.5f && lu < 1.5f && c.vfov > 0.05f && c.vfov < 3.0f;
		return c;
	}

	bool depthReversed() { return u8(base() + L().rvaReversedZ, 0) != 0; }

	uintptr_t viewUser(uintptr_t view) { return u32(view + (odst() ? 10236 : 10228), 0xFFFFFFFFu); }

	bool readViewFrame(uintptr_t view, h2::GameFrame& f)
	{
		f = h2::GameFrame{};
		if (!okp(view)) return false;
		float cam[28];                                      // view+8 .. +0x78: pos, forward, up, ..., vfov @+40, near/far @+100/+104
		if (!copy(cam, view + 8, sizeof(cam))) return false;
		float rp[46];                                       // view+152: w2v (13 floats), ..., projection @+120 (16 floats)
		if (!copy(rp, view + 152, sizeof(rp))) return false;
		f.pos = { cam[0], cam[1], cam[2] }; f.fwd = { cam[3], cam[4], cam[5] }; f.up = { cam[6], cam[7], cam[8] };
		f.vfov = cam[10];
		f.nearClip = cam[25]; f.farClip = cam[26];
		memcpy(f.w2v, rp, 13 * sizeof(float));
		memcpy(f.proj, rp + 30, 16 * sizeof(float));
		f.textureCamera = false;
		const float lf = f.fwd.x * f.fwd.x + f.fwd.y * f.fwd.y + f.fwd.z * f.fwd.z;
		const float lu = f.up.x * f.up.x + f.up.y * f.up.y + f.up.z * f.up.z;
		f.ok = std::isfinite(f.pos.x) && std::isfinite(f.proj[0]) && std::isfinite(f.proj[10]) && std::isfinite(f.w2v[0]) &&
		       lf > 0.5f && lf < 1.5f && lu > 0.5f && lu < 1.5f && f.vfov > 0.05f && f.vfov < 3.0f &&
		       f.nearClip > 0 && f.farClip > f.nearClip && std::fabs(f.proj[15]) < 1e6f;
		return f.ok;
	}

	// ---- threads ------------------------------------------------------------------------------------------------------
	void captureSimTls()
	{
		const uintptr_t t = curTls();
		if (t && gSimTls.load() != t) { gSimTls = t; LOGF("sim-thread TLS block captured: 0x%llX (slot dword %d)", (unsigned long long)t, i32(t + 0x18, -1)); }
	}
	bool haveSimTls() { return gSimTls.load() != 0; }

	// ---- tags ---------------------------------------------------------------------------------------------------------
	uintptr_t heap() { return q(base() + L().rvaHeap); }
	uintptr_t scenario() { return q(base() + L().rvaScenario); }

	uintptr_t tagData(uint32_t tagIndex)
	{
		const uintptr_t inst = q(base() + L().rvaTagInst), h = heap();
		if (!okp(inst) || !okp(h)) return 0;
		const uint32_t off = u32(inst + 8 * (uintptr_t)(tagIndex & 0xFFFF) + 4, 0);
		return off ? h + 4 * (uintptr_t)off : 0;
	}

	bool block(uintptr_t blockAddr, uintptr_t& first, uint32_t& count)
	{
		first = 0; count = 0;
		uint32_t hdr[2];
		if (!copy(hdr, blockAddr, 8)) return false;
		count = hdr[0];
		if (count == 0) return true;
		const uintptr_t h = heap();
		if (!okp(h) || !hdr[1]) return false;
		first = h + 4 * (uintptr_t)hdr[1];
		return okp(first);
	}

	// ---- structure bsps -----------------------------------------------------------------------------------------------
	uint32_t activeBspMask() { return u32(base() + L().rvaActiveMask, 0); }

	namespace
	{
		uintptr_t sbspOf(int index)
		{
			const uintptr_t scn = scenario();
			if (!okp(scn) || index < 0 || index > 31) return 0;
			uintptr_t first; uint32_t n;
			if (!block(scn + 0x18, first, n) || (uint32_t)index >= n || n > 64) return 0;
			return tagData(u16(first + 0x6C * (uintptr_t)index + 0x0C, 0xFFFF));
		}

		// the engine's resource-or-inline resolve of the holder (sub_18009B6C0 / ODST sub_1800A8238). `slot` = the dword the engine
		// reads from TLS+0x18 (index into the resource table); candidates are tried by the caller.
		uintptr_t holderData(uintptr_t holder, int slot, bool& isResource)
		{
			isResource = u32(holder + 0x1C, 0) != 0;
			const Layout& l = L();
			if (!isResource)
			{
				const uint32_t off = u32(holder + 4, 0);
				return off ? heap() + 4 * (uintptr_t)off : 0;
			}
			const uintptr_t r = q(base() + l.rvaResCtx);
			if (!okp(r)) return 0;
			uintptr_t r8 = r + 0x2B440;
			const bool zf = r8 == 0;
			r8 += l.resDisp;
			if (zf) r8 = l.resConst;
			const uintptr_t tbl = q(r8);
			if (!okp(tbl)) return 0;
			const uintptr_t ent = q(tbl + 8 * (uintptr_t)(int64_t)slot);
			if (!okp(ent)) return 0;
			const uintptr_t arr = q(ent + 0x18);
			if (!okp(arr)) return 0;
			return q(arr + 8 * (uintptr_t)u16(holder + 0x0C, 0));
		}

		bool plausibleBsp(uintptr_t bsp)
		{
			if (!okp(bsp)) return false;
			const uint32_t cs = u32(bsp + 0x3C, 0), ce = u32(bsp + 0x48, 0), cv = u32(bsp + 0x54, 0), cp = u32(bsp + 0x0C, 0);
			if (!cs || !ce || !cv || !cp || cs > 3000000 || ce > 6000000 || cv > 3000000 || cp > 3000000) return false;
			return u32(bsp + 0x40, 0) && u32(bsp + 0x4C, 0) && u32(bsp + 0x58, 0) && u32(bsp + 0x10, 0);
		}
	}

	bool resolveBsp(int index, BspRes& out, std::string* why)
	{
		out = BspRes{};
		auto fail = [&](const char* m) { if (why) *why = m; return false; };
		const uintptr_t sbsp = sbspOf(index);
		if (!okp(sbsp)) return fail("scenario structure bsp / tag instance unreadable");
		const Layout& l = L();
		const uintptr_t holder = sbsp + l.holderOff;
		bool isRes = false;
		int slots[8]; int ns = 0;
		auto add = [&](int s) { for (int i = 0; i < ns; ++i) if (slots[i] == s) return; if (ns < 8) slots[ns++] = s; };
		if (const uintptr_t s = gSimTls.load()) add(i32(s + 0x18, 0));
		if (const uintptr_t c = curTls()) add(i32(c + 0x18, 0));
		for (int s = 0; s < 4; ++s) add(s);
		for (int k = 0; k < ns; ++k)
		{
			const uintptr_t data = holderData(holder, slots[k], isRes);
			if (!okp(data)) { if (!isRes) break; continue; }
			const bool is16 = u32(data, 0) == 1;
			const uint32_t off = u32(data + (is16 ? 4 : 0x10), 0);
			if (!off) continue;
			const uintptr_t bsp = heap() + 4 * (uintptr_t)off;
			if (!plausibleBsp(bsp)) continue;
			out.index = index; out.sbsp = sbsp; out.data = data; out.worldBsp = bsp; out.large = !is16;
			char how[128];
			snprintf(how, sizeof(how), "%s%s, %s collision bsp", isRes ? "resource slot " : "inline", isRes ? std::to_string(slots[k]).c_str() : "", is16 ? "16-bit" : "32-bit");
			out.how = how;
			// instanced geometry: definitions live in the same resource (offset dword @+0x1C), instances in the sbsp
			const uint32_t igOff = u32(data + 0x1C, 0);
			if (igOff) { out.igDefs = heap() + 4 * (uintptr_t)igOff; out.igDefStride = l.igDefStride; }
			const uint32_t ic = u32(sbsp + l.igCountOff, 0), io = u32(sbsp + l.igOffOff, 0);
			if (ic && io && ic < 40000) { out.igInstances = heap() + 4 * (uintptr_t)io; out.igInstanceCount = ic; }
			return true;
		}
		return fail(isRes ? "collision bsp resource did not resolve (no TLS slot gave a plausible bsp)" : "inline collision bsp implausible");
	}

	uintptr_t igDefBsp(const BspRes& r, uint32_t defIndex)
	{
		if (!okp(r.igDefs) || !r.igDefStride || defIndex > 20000) return 0;
		return r.igDefs + (uintptr_t)r.igDefStride * defIndex + 20;
	}

	bool breakableRow(int bsp, uint32_t group, uint32_t out[12])
	{
		if (bsp < 0 || bsp > 15 || group > 63) return false;
		const uintptr_t tls = tlsBlock();
		if (!tls) return false;
		const uintptr_t pool = q(tls + L().tlsPool);
		if (!okp(pool)) return false;
		if (!((u32(pool + 8, 0) >> bsp) & 1u)) return false;
		const int16_t idx = (int16_t)u16(pool + 2 * (uintptr_t)bsp + 118264, 0xFFFF);
		if (idx < 0 || idx > 4000) return false;
		return copy(out, pool + 3576 + 56 * (uintptr_t)idx + 56 * (uintptr_t)group + 8, 48);
	}

	bool killVolumes(std::vector<VolumeBox>& out)
	{
		out.clear();
		const Layout& l = L();
		const uintptr_t scn = scenario();
		if (!okp(scn)) return false;
		uintptr_t tvF, kF; uint32_t tvN, kN;
		if (!block(scn + l.scnTrig, tvF, tvN) || !block(scn + l.scnKill, kF, kN)) return false;
		if (tvN > 512 || kN > 512) return false;
		for (uint32_t k = 0; k < kN; ++k)
		{
			const uint16_t idx = u16(kF + 2 * (uintptr_t)k, 0xFFFF);
			if (idx == 0xFFFF || idx >= tvN) continue;
			const uintptr_t tv = tvF + (uintptr_t)idx * l.trigStride;
			float fwd[3], up[3], pos[3], ext[3];
			if (!copy(fwd, tv + l.trigFwd, 12) || !copy(up, tv + l.trigUp, 12) || !copy(pos, tv + l.trigPos, 12) || !copy(ext, tv + l.trigExt, 12)) continue;
			VolumeBox b; b.fwd = { fwd[0], fwd[1], fwd[2] }; b.up = { up[0], up[1], up[2] }; b.pos = { pos[0], pos[1], pos[2] }; b.ext = { ext[0], ext[1], ext[2] };
			if (!finite3(b.fwd) || !finite3(b.up) || !finite3(b.pos) || !finite3(b.ext)) continue;
			if (!(ext[0] > 0 && ext[1] > 0 && ext[2] > 0 && ext[0] < 1e5f && ext[1] < 1e5f && ext[2] < 1e5f)) continue;
			out.push_back(b);
		}
		return true;
	}

	namespace
	{
		// The game keeps the players table, game time and kill volume bits in its game thread's implicit-TLS block.
		// Without waiting for a sim-thread callback, find that block from any thread: walk the process's threads, read
		// each TEB's TLS array at the module's TLS index, and keep the block whose players table pointer (+0x10) and game
		// time pointer (+0xC8) look right. Cached; re-found when it stops looking right.
		typedef LONG(NTAPI* t_NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);
		struct ThreadBasicInfo { LONG ExitStatus; PVOID TebBaseAddress; PVOID UniqueProcess; PVOID UniqueThread; ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority; };

		bool plausibleGameTls(uintptr_t t)
		{
			if (!okp(t)) return false;
			const uintptr_t hdr = q(t + 0x10), tt = q(t + 0xC8);
			if (!okp(hdr) || !okp(tt)) return false;
			const uint32_t mx = u32(hdr + 0x20, 0), sz = u32(hdr + 0x24, 0);
			const int16_t tps = (int16_t)u16(tt + 4, 0);
			return okp(q(hdr + 0x48)) && mx > 0 && mx <= 256 && sz >= 0x48 && sz <= 0x4000 && tps > 0 && tps <= 120;
		}

		uintptr_t findGameTls()
		{
			static uintptr_t sCached = 0; static ULONGLONG sNext = 0;
			if (plausibleGameTls(gSimTls.load())) return gSimTls.load();
			if (plausibleGameTls(sCached)) return sCached;
			const ULONGLONG now = GetTickCount64();
			if (now < sNext) return 0;
			sNext = now + 1000;                                  // rescan at most once a second
			sCached = 0;
			const uint32_t idx = u32(base() + L().rvaTlsIndex, 0xFFFFFFFFu);
			if (idx == 0xFFFFFFFFu || idx > 1088) return 0;
			static t_NtQueryInformationThread NtQIT = (t_NtQueryInformationThread)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
			if (!NtQIT) return 0;
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) return 0;
			THREADENTRY32 te{}; te.dwSize = sizeof(te);
			const DWORD pid = GetCurrentProcessId();
			for (BOOL ok = Thread32First(snap, &te); ok && !sCached; ok = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
				if (!th) continue;
				ThreadBasicInfo tbi{};
				if (NtQIT(th, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), nullptr) == 0 && tbi.TebBaseAddress)
				{
					const uintptr_t arr = q((uintptr_t)tbi.TebBaseAddress + 0x58);   // TEB.ThreadLocalStoragePointer
					const uintptr_t blk = okp(arr) ? q(arr + 8 * (uintptr_t)idx) : 0;
					if (plausibleGameTls(blk)) sCached = blk;
				}
				CloseHandle(th);
			}
			CloseHandle(snap);
			if (sCached) LOGF("death bar: game-thread TLS block found by thread walk: 0x%llX", (unsigned long long)sCached);
			return sCached;
		}
	}

	bool readDeath(Death& out)
	{
		out = Death{};
		// ODST (player update sub_1800FF514, same counter +0x45 / unit +0x28 / players TLS+0x10 / game time TLS+0xC8; kill
		// volume bits at TLS+0x438 instead of +0x418; trigger volumes carry a type @+0xC - only boxes (0) are tested here)
		const uintptr_t t = findGameTls();
		if (!okp(t)) { out.note = "game-thread TLS block not found yet"; return false; }
		// players data array (TLS + 0x10, as the player update itself reads it)
		const uintptr_t hdr = q(t + 0x10);
		if (!okp(hdr)) { out.note = "players table pointer unreadable"; return false; }
		const uint32_t mx = u32(hdr + 0x20, 0), sz = u32(hdr + 0x24, 0);
		const uintptr_t data = q(hdr + 0x48);
		char buf[160];
		if (!okp(data) || mx == 0 || mx > 256 || sz < 0x48 || sz > 0x4000)
		{
			snprintf(buf, sizeof(buf), "players table not recognised (max %u, size 0x%X, data 0x%llX, sig 0x%08X)", mx, sz, (unsigned long long)data, u32(hdr + 0x2C, 0));
			out.note = buf; return false;
		}
		// the local player: the one whose unit (+0x28) is HCM's player unit; else the first player that has a unit
		const uintptr_t g = G();
		const uint32_t hcmUnit = okp(g) ? u32(g + L().offPlayer, 0xFFFFFFFFu) : 0xFFFFFFFFu;
		uintptr_t player = 0, first = 0; uint32_t index = 0, firstIndex = 0;
		for (uint32_t i = 0; i < mx; ++i)
		{
			const uintptr_t e = data + (uintptr_t)i * sz;
			const uint32_t pu = u32(e + 0x28, 0xFFFFFFFFu);
			if (pu == 0xFFFFFFFFu || pu == 0) continue;
			if (!first) { first = e; firstIndex = i; }
			if (hcmUnit != 0xFFFFFFFFu && pu == hcmUnit) { player = e; index = i; break; }
		}
		if (!player) { player = first; index = firstIndex; }
		if (!player) { out.note = "no player has a unit (dead / respawning)"; return false; }
		const uint32_t unit = u32(player + 0x28, 0xFFFFFFFFu);
		// game time (TLS + 0xC8): ticks per second @+4 (i16), tick @+0xC
		const uintptr_t tt = q(t + 0xC8);
		const uint32_t tick = okp(tt) ? u32(tt + 0xC, 0) : gameTick();
		int tps = okp(tt) ? (int)(int16_t)u16(tt + 4, 30) : 30;
		if (tps <= 0 || tps > 120) tps = 30;
		out.ticksPerSec = tps;
		out.limit = (int)(tps * 0.5f + 0.5f);
		out.counter = (int)u8(player + 0x45, 0);
		out.playerIndex = index;
		out.nextCheck = (int)((16u - ((tick + index) & 15u)) & 15u);
		out.valid = true;

		// kill volume rotation. Inside test as the game does it (sub_18016F55C): the unit's point object+0x1C in the trigger
		// volume's own frame (forward, left = up x forward, up from its corner), 0 < local < extents on all three axes.
		const uintptr_t scn = scenario();
		const uintptr_t uobj = objectFromDatum(unit);
		float pp[3] = {};
		const bool haveP = okp(uobj) && copy(pp, uobj + 0x1C, 12) && std::isfinite(pp[0]) && std::isfinite(pp[1]) && std::isfinite(pp[2]);
		uintptr_t kF, tvF; uint32_t kN, tvN;
		if (okp(scn) && block(scn + L().scnKill, kF, kN) && block(scn + L().scnTrig, tvF, tvN) && kN > 0 && kN < 4096)
		{
			out.kvCount = (int)kN;
			out.kvChecking = (int)((tick + (unit & 0xFFFF)) % kN);
			const uintptr_t bits = q(t + (odst() ? 0x438 : 0x418));
			for (uint32_t k = 0; k < kN; ++k)
			{
				const bool enabled = !okp(bits) || ((u32(bits + 4 * (uintptr_t)(k >> 5), 0) >> (k & 31)) & 1) == 0;
				if (!enabled) continue;
				++out.kvEnabled;
				const int16_t trig = (int16_t)u16(kF + 2 * (uintptr_t)k, 0xFFFF);
				if (trig < 0 || (uint32_t)trig >= tvN || !haveP) continue;
				const uintptr_t tv = tvF + (uintptr_t)trig * L().trigStride;
				if ((int16_t)u16(tv + 4, 0xFFFF) != -1) continue;    // follows an object: not handled
				if (odst() && (int16_t)u16(tv + 0xC, 0) != 0) continue;   // ODST sector volume (polygon prism): not handled
				float f[3], up[3], pos[3], ext[3];
				if (!copy(f, tv + L().trigFwd, 12) || !copy(up, tv + L().trigUp, 12) || !copy(pos, tv + L().trigPos, 12) || !copy(ext, tv + L().trigExt, 12)) continue;
				const float lf[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
				const float d[3] = { pp[0] - pos[0], pp[1] - pos[1], pp[2] - pos[2] };
				const float x = d[0] * f[0] + d[1] * f[1] + d[2] * f[2], y = d[0] * lf[0] + d[1] * lf[1] + d[2] * lf[2], z = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
				if (!(x > 0 && y > 0 && z > 0 && x < ext[0] && y < ext[1] && z < ext[2])) continue;
				const int64_t r = ((int64_t)k - (int64_t)tick - (int64_t)(unit & 0xFFFF)) % (int64_t)kN;
				out.kvInside.push_back({ (int)k, (int)(r < 0 ? r + kN : r) });
			}
		}
		return true;
	}

	// scenario structure bsp entry (stride 0x6C) +0x1C = structure design tag datum; sddt +0x10 = soft ceilings {count, offset}, stride 0x14:
	// type word @+4, triangles {count @+8, offset @+0xC}; triangle stride 0x44, vertices @+0x20 / +0x2C / +0x38 (HCM softCeilingData fields; same in ODST)
	bool softCeilings(int bspIndex, std::vector<SoftTri>& out)
	{
		const uintptr_t scn = scenario();
		uintptr_t bF; uint32_t bN;
		if (!okp(scn) || !block(scn + 0x18, bF, bN) || bspIndex < 0 || (uint32_t)bspIndex >= bN || bN > 64) return false;
		const uint32_t datum = u32(bF + 0x6C * (uintptr_t)bspIndex + 0x1C, 0xFFFFFFFFu);
		if (datum == 0xFFFFFFFFu) return false;
		const uintptr_t sd = tagData(datum);
		uintptr_t cF; uint32_t cN;
		if (!okp(sd) || !block(sd + 0x10, cF, cN) || cN > 4096) return false;
		for (uint32_t j = 0; j < cN; ++j)
		{
			const uintptr_t sc = cF + 0x14 * (uintptr_t)j;
			uintptr_t tF; uint32_t tN;
			if (!block(sc + 8, tF, tN) || tN > 65536) continue;
			const uint16_t type = u16(sc + 4, 0);
			for (uint32_t k = 0; k < tN && out.size() < 200000; ++k)
			{
				float v[3][3];
				const uintptr_t t = tF + 0x44 * (uintptr_t)k;
				if (!copy(v[0], t + 0x20, 12) || !copy(v[1], t + 0x2C, 12) || !copy(v[2], t + 0x38, 12)) continue;
				SoftTri s; s.type = type;
				for (int i = 0; i < 3; ++i) s.v[i] = { v[i][0], v[i][1], v[i][2] };
				if (!finite3(s.v[0]) || !finite3(s.v[1]) || !finite3(s.v[2])) continue;
				out.push_back(s);
			}
		}
		return true;
	}

	// ---- objects ------------------------------------------------------------------------------------------------------
	namespace
	{
		struct ObjTable { uintptr_t entries = 0; uint32_t maxCount = 0; };
		ObjTable objTable()
		{
			ObjTable t;
			const uintptr_t g = G();
			if (!okp(g)) return t;
			t.entries = g + L().offObjHdr;
			// the data array header sits 0x60..0xC0 bytes in front of the first record (the dword @hdr+0x58 is the record offset):
			// 'd@t@' @+0x2C, record size @+0x24, max @+0x20, absolute data pointer @+0x48
			for (uintptr_t d = 0x58; d <= 0xC8; d += 8)
			{
				const uintptr_t hdr = t.entries - d;
				if (u32(hdr + 0x2C, 0) == kDataArraySig && u32(hdr + 0x24, 0) == 24 && q(hdr + 0x48) == t.entries)
				{
					const uint32_t mx = u32(hdr + 0x20, 0);
					if (mx > 0 && mx <= 16384) { t.maxCount = mx; return t; }
				}
			}
			t.maxCount = 2048;   // H3 object limit when the header could not be located
			return t;
		}
	}

	void enumerateObjects(std::vector<h2::ObjectRef>& out)
	{
		out.clear();
		const ObjTable t = objTable();
		if (!okp(t.entries) || !t.maxCount) return;
		std::vector<uint8_t> raw((size_t)t.maxCount * 24);
		if (!copy(raw.data(), t.entries, raw.size())) return;
		for (uint32_t i = 0; i < t.maxCount; ++i)
		{
			const uint8_t* e = &raw[(size_t)i * 24];
			const uint16_t salt = *(const uint16_t*)e;
			const uintptr_t obj = *(const uintptr_t*)(e + 0x10);
			if (!salt || !okp(obj)) continue;
			out.push_back({ obj, ((uint32_t)salt << 16) | i, e[3], 0 });
		}
	}

	uintptr_t objectFromDatum(uint32_t datum)
	{
		if (datum == 0xFFFFFFFFu) return 0;
		const ObjTable t = objTable();
		const uint32_t idx = datum & 0xFFFF;
		if (!okp(t.entries) || idx >= t.maxCount) return 0;
		uint8_t e[24];
		if (!copy(e, t.entries + 24 * (uintptr_t)idx, 24)) return 0;
		if (*(uint16_t*)e != (datum >> 16)) return 0;
		const uintptr_t obj = *(uintptr_t*)(e + 0x10);
		return okp(obj) ? obj : 0;
	}

	uintptr_t localPlayerUnitObject()
	{
		const uintptr_t g = G();
		if (!okp(g)) return 0;
		return objectFromDatum(u32(g + L().offPlayer, 0xFFFFFFFFu));
	}

	int localPlayerUnits(uintptr_t out[4])
	{
		for (int i = 0; i < 4; ++i) out[i] = 0;
		out[0] = localPlayerUnitObject();
		return out[0] ? 1 : 0;
	}

	uint32_t objectDefinitionTag(uintptr_t obj) { return u32(obj, 0xFFFFFFFFu); }
	bool objectHidden(uintptr_t obj) { return (u32(obj + 4, 0) & 1u) != 0; }

	// ---- Havok 4.5 ----------------------------------------------------------------------------------------------------
	uintptr_t world() { return q(base() + L().rvaWorld); }

	WorldTime worldTime()
	{
		WorldTime t;
		const uintptr_t w = world();
		if (!okp(w)) return t;
		const uintptr_t ci = q(w + 168);                    // hkpWorld::m_collisionInput (ctor sub_18040E370: *(a1+168))
		if (!okp(ci)) return t;
		float f[14];
		if (!copy(f, ci, sizeof(f))) return t;
		t.tol = f[2]; t.stepStart = f[12]; t.stepEnd = f[13];
		t.ok = t.tol > 0.f && t.tol < 1.f && std::isfinite(t.stepStart) && std::isfinite(t.stepEnd);
		return t;
	}

	int havokBodies(uintptr_t obj, uintptr_t out[8])
	{
		const uint32_t hd = u32(obj + 0x9C, 0xFFFFFFFFu);
		if (hd == 0xFFFFFFFFu || hd == 0) return 0;
		const uintptr_t hdr = q(base() + L().rvaHavokComps);
		if (!okp(hdr)) return 0;
		uintptr_t data = q(hdr + 0x48);                      // absolute data pointer; HCM reads hdr + dword@+0x58 (the same address)
		if (!okp(data)) data = hdr + u32(hdr + 0x58, 0);
		const uintptr_t comp = data + (uintptr_t)L().compStride * (hd & 0xFFFF);
		if (u16(comp, 0) == 0) return 0;
		const uintptr_t rb = q(comp + L().compRb);
		const int32_t n = i32(comp + L().compRb + 8, 0);
		if (!okp(rb) || n <= 0 || n > 64) return 0;
		const uint8_t primary = u8(comp + 12, 0xFF);
		int cnt = 0;
		auto add = [&](int k)
		{
			const uintptr_t b = q(rb + 0x50 + 0x60 * (uintptr_t)k);
			if (!okp(b)) return;
			for (int i = 0; i < cnt; ++i) if (out[i] == b) return;
			if (cnt < 8) out[cnt++] = b;
		};
		if (primary < n) add(primary);
		for (int k = 0; k < n && k < 8; ++k) add(k);
		return cnt;
	}

	bool readMotion(uintptr_t body, MotionState& m)
	{
		m = MotionState{};
		uint8_t raw[0x100];
		if (!copy(raw, body + 0x100, sizeof(raw))) return false;
		auto F = [&](uintptr_t off) { return reinterpret_cast<const float*>(raw + (off - 0x100)); };
		m.type = raw[0];
		const float* c0 = F(0x110); const float* c1 = F(0x120); const float* c2 = F(0x130); const float* t = F(0x140);
		m.xf.c[0] = { c0[0], c0[1], c0[2] }; m.xf.c[1] = { c1[0], c1[1], c1[2] }; m.xf.c[2] = { c2[0], c2[1], c2[2] };
		m.xf.t = { t[0], t[1], t[2] };
		const float* a = F(0x150); const float* b = F(0x160); const float* v = F(0x1E0); const float* w = F(0x1F0);
		m.com0 = { a[0], a[1], a[2] }; m.com1 = { b[0], b[1], b[2] }; m.v = { v[0], v[1], v[2] }; m.w = { w[0], w[1], w[2] };
		bool ok = finite3(m.xf.t) && finite3(m.com0) && finite3(m.com1) && finite3(m.v);
		for (const Vec3& c : m.xf.c)
		{
			const float l2 = c.x * c.x + c.y * c.y + c.z * c.z;
			if (!std::isfinite(l2) || l2 < 0.8f || l2 > 1.2f) ok = false;
		}
		m.ok = ok;
		return ok;
	}

	namespace
	{
		// hkpShape::getType() is vtable slot 5, a `mov eax, imm32 / ret` leaf (sphere 0x28, box 0x40, capsule 0x50, convex vertices 0x80 in both
		// games). Reading it identifies COMDAT-cloned vtables (a rebuilt biped pill, per-tag shapes) that a plain vtable compare would miss.
		int shapeTypeOf(uintptr_t shape)
		{
			if (!okp(shape)) return -1;
			const uintptr_t b = base(), v = q(shape);
			if (v < b || v - b > 0x6000000) return -1;
			const uintptr_t fn = q(v + 0x28);
			if (fn < b || fn - b > 0x6000000) return -1;
			uint8_t c[6];
			if (!copy(c, fn, 6) || c[0] != 0xB8 || c[5] != 0xC3) return -1;
			int32_t t; memcpy(&t, c + 1, 4);
			return t;
		}
		constexpr int kTypeSphere = 0x28, kTypeBox = 0x40, kTypeCapsule = 0x50, kTypeConvex = 0x80;
	}

	int shapeVtRva(uintptr_t body)
	{
		const uintptr_t shape = q(body + 0x20);
		if (!okp(shape)) return -1;
		const uintptr_t vt = q(shape), b = base();
		return (vt >= b && vt - b < 0x6000000) ? (int)(vt - b) : -1;
	}

	h2::Capsule bodyCapsuleLocal(uintptr_t body)
	{
		h2::Capsule c;
		const uintptr_t shape = q(body + 0x20);
		if (!okp(shape)) return c;
		if (shapeVtRva(body) != (int)L().vtCapsule && shapeTypeOf(shape) != kTypeCapsule) return c;
		float r = f32(shape + 0x20, 0.f);
		float a[3], bb[3];
		if (!copy(a, shape + 0x30, 12) || !copy(bb, shape + 0x40, 12)) return c;
		if (!(r > 0.02f && r < 2.f)) return c;
		c.a = { a[0], a[1], a[2] }; c.b = { bb[0], bb[1], bb[2] }; c.r = r;
		c.ok = finite3(c.a) && finite3(c.b);
		return c;
	}

	// ---- object shapes ------------------------------------------------------------------------------------------------
	// Shape layouts (HavokDebugger.cpp, verified live there): the hkpShape vtable identifies the class;
	//   sphere radius @+0x20 | capsule radius @+0x20, A @+0x30, B @+0x40 | box half extents @+0x30 | convex vertices: SoA vertex blocks
	//   (48 B: x[4] y[4] z[4]) at *(+0x50), count @+0x60, planes {n.xyz, d} 16 B at *(+0x68), count @+0x70 | list / collection: child
	//   array *(+0x30), count @+0x38, 0x20 per child, shape pointer first | convex-transform: child @+0x30 | transform: child @+0x28,
	//   both with the hkTransform columns @+0x40/+0x50/+0x60 and translation @+0x70 | convex-translate: child @+0x30, translation @+0x40.
	namespace
	{
		struct ShapeWalker
		{
			ShapeLines& out; int skipped = 0;
			explicit ShapeWalker(ShapeLines& o) : out(o) {}
			void line(const Vec3& a, const Vec3& b) { if (out.a.size() < 60000) { out.a.push_back(a); out.b.push_back(b); } }
			void ring(const h2::Xf& xf, const Vec3& c, const Vec3& u, const Vec3& v, float r, int n = 16)
			{
				Vec3 prev{};
				for (int i = 0; i <= n; ++i)
				{
					const float a = 6.2831853f * i / n, ca = std::cos(a) * r, sa = std::sin(a) * r;
					const Vec3 p = h2::xfPoint(xf, { c.x + u.x * ca + v.x * sa, c.y + u.y * ca + v.y * sa, c.z + u.z * ca + v.z * sa });
					if (i) line(prev, p);
					prev = p;
				}
			}
			static bool readXfAt(uintptr_t shape, h2::Xf& t)
			{
				float m[16];
				if (!copy(m, shape + 0x40, sizeof(m))) return false;
				for (float f : m) if (!std::isfinite(f) || std::fabs(f) > 1e5f) return false;
				t.c[0] = { m[0], m[1], m[2] }; t.c[1] = { m[4], m[5], m[6] }; t.c[2] = { m[8], m[9], m[10] }; t.t = { m[12], m[13], m[14] };
				return true;
			}
			void box(const h2::Xf& xf, const Vec3& he, const Vec3& c = { 0, 0, 0 })
			{
				Vec3 p[8];
				for (int i = 0; i < 8; ++i) p[i] = h2::xfPoint(xf, { c.x + ((i & 1) ? he.x : -he.x), c.y + ((i & 2) ? he.y : -he.y), c.z + ((i & 4) ? he.z : -he.z) });
				static const int e[12][2] = { {0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7} };
				for (const auto& k : e) line(p[k[0]], p[k[1]]);
			}
			void convex(uintptr_t shape, const h2::Xf& xf)
			{
				const uintptr_t vdata = q(shape + 0x50), pdata = q(shape + 0x68);
				const int nv = i32(shape + 0x60, -1), np = i32(shape + 0x70, -1);
				if (!okp(vdata) || nv < 3 || nv > 256) { ++skipped; return; }
				std::vector<float> raw((size_t)((nv + 3) / 4) * 12);
				if (!copy(raw.data(), vdata, raw.size() * 4)) { ++skipped; return; }
				std::vector<Vec3> lv((size_t)nv);
				float ext = 1e-4f;
				for (int k = 0; k < nv; ++k)
				{
					const float* blk = &raw[(size_t)(k / 4) * 12];
					lv[(size_t)k] = { blk[k & 3], blk[4 + (k & 3)], blk[8 + (k & 3)] };
					ext = std::max({ ext, std::fabs(lv[(size_t)k].x), std::fabs(lv[(size_t)k].y), std::fabs(lv[(size_t)k].z) });
				}
				if (np < 1 || np > 256 || !okp(pdata))
				{
					Vec3 mn = lv[0], mx = lv[0];   // no planes: the vertex bounding box
					for (const Vec3& v : lv) { mn = { std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z) }; mx = { std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z) }; }
					box(xf, { (mx.x - mn.x) * .5f, (mx.y - mn.y) * .5f, (mx.z - mn.z) * .5f }, { (mx.x + mn.x) * .5f, (mx.y + mn.y) * .5f, (mx.z + mn.z) * .5f });
					return;
				}
				std::vector<float> pl((size_t)np * 4);
				if (!copy(pl.data(), pdata, pl.size() * 4)) { ++skipped; return; }
				const float eps = ext * 0.002f + 0.004f;
				for (int f = 0; f < np; ++f)
				{
					const Vec3 n{ pl[(size_t)f * 4], pl[(size_t)f * 4 + 1], pl[(size_t)f * 4 + 2] }; const float d = pl[(size_t)f * 4 + 3];
					std::vector<int> idx; Vec3 cen{ 0, 0, 0 };
					for (int k = 0; k < nv; ++k)
					{
						const float dist = n.x * lv[(size_t)k].x + n.y * lv[(size_t)k].y + n.z * lv[(size_t)k].z + d;
						if (dist < eps && dist > -eps) { idx.push_back(k); cen.x += lv[(size_t)k].x; cen.y += lv[(size_t)k].y; cen.z += lv[(size_t)k].z; }
					}
					if (idx.size() < 3) continue;
					const float inv = 1.f / (float)idx.size(); cen = { cen.x * inv, cen.y * inv, cen.z * inv };
					const Vec3 h = std::fabs(n.x) < 0.9f ? Vec3{ 1, 0, 0 } : Vec3{ 0, 1, 0 };
					Vec3 u{ n.y * h.z - n.z * h.y, n.z * h.x - n.x * h.z, n.x * h.y - n.y * h.x };
					const float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
					if (ul < 1e-6f) continue;
					u = { u.x / ul, u.y / ul, u.z / ul };
					const Vec3 w{ n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x };
					std::vector<std::pair<float, int>> ang;
					for (int k : idx)
					{
						const Vec3 dv{ lv[(size_t)k].x - cen.x, lv[(size_t)k].y - cen.y, lv[(size_t)k].z - cen.z };
						ang.push_back({ std::atan2(dv.x * w.x + dv.y * w.y + dv.z * w.z, dv.x * u.x + dv.y * u.y + dv.z * u.z), k });
					}
					std::sort(ang.begin(), ang.end());
					for (size_t i = 0; i < ang.size(); ++i)
						line(h2::xfPoint(xf, lv[(size_t)ang[i].second]), h2::xfPoint(xf, lv[(size_t)ang[(i + 1) % ang.size()].second]));
				}
			}
			void walk(uintptr_t shape, const h2::Xf& xf, int depth)
			{
				if (!okp(shape) || depth > 8) return;
				const Layout& l = L();
				const uintptr_t vt = q(shape), b = base();
				if (vt < b) { ++skipped; return; }
				const uint32_t r = (uint32_t)(vt - b);
				const int ty = shapeTypeOf(shape);
				if (r == l.vtBox || (ty == kTypeBox && r != l.vtSphere && r != l.vtCapsule && r != l.vtConvex))
				{
					float he[3];
					if (!copy(he, shape + 0x30, 12) || !(he[0] > 0 && he[1] > 0 && he[2] > 0 && he[0] < 1e4f && he[1] < 1e4f && he[2] < 1e4f)) { ++skipped; return; }
					box(xf, { he[0], he[1], he[2] });
				}
				else if (r == l.vtSphere || ty == kTypeSphere)
				{
					const float rad = f32(shape + 0x20, 0.f);
					if (!(rad > 0.f && rad < 1e4f)) { ++skipped; return; }
					ring(xf, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, rad); ring(xf, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, rad); ring(xf, { 0, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, rad);
				}
				else if (r == l.vtCapsule || ty == kTypeCapsule)
				{
					const float rad = f32(shape + 0x20, 0.f); float a[3], c[3];
					if (!(rad > 0.f && rad < 1e4f) || !copy(a, shape + 0x30, 12) || !copy(c, shape + 0x40, 12)) { ++skipped; return; }
					const Vec3 A{ a[0], a[1], a[2] }, B{ c[0], c[1], c[2] };
					Vec3 ax{ B.x - A.x, B.y - A.y, B.z - A.z };
					const float al = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);
					if (al < 1e-5f)
					{
						ring(xf, A, { 1, 0, 0 }, { 0, 1, 0 }, rad); ring(xf, A, { 1, 0, 0 }, { 0, 0, 1 }, rad); ring(xf, A, { 0, 1, 0 }, { 0, 0, 1 }, rad);
						return;
					}
					ax = { ax.x / al, ax.y / al, ax.z / al };
					const Vec3 h = std::fabs(ax.z) < 0.9f ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 };
					Vec3 u{ ax.y * h.z - ax.z * h.y, ax.z * h.x - ax.x * h.z, ax.x * h.y - ax.y * h.x };
					const float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
					u = { u.x / ul, u.y / ul, u.z / ul };
					const Vec3 v{ ax.y * u.z - ax.z * u.y, ax.z * u.x - ax.x * u.z, ax.x * u.y - ax.y * u.x };
					ring(xf, A, u, v, rad); ring(xf, B, u, v, rad);
					for (int k = 0; k < 4; ++k)
					{
						const float t = 1.5707963f * k;
						const Vec3 d{ u.x * std::cos(t) + v.x * std::sin(t), u.y * std::cos(t) + v.y * std::sin(t), u.z * std::cos(t) + v.z * std::sin(t) };
						line(h2::xfPoint(xf, { A.x + d.x * rad, A.y + d.y * rad, A.z + d.z * rad }), h2::xfPoint(xf, { B.x + d.x * rad, B.y + d.y * rad, B.z + d.z * rad }));
					}
					ring(xf, A, ax, u, rad); ring(xf, B, ax, u, rad);   // pole rings (half of each is the hemisphere cap)
				}
				else if (r == l.vtConvex || ty == kTypeConvex) convex(shape, xf);
				else if (r == l.vtList || r == l.vtHkList || (l.vtCollection && r == l.vtCollection))
				{
					const uintptr_t data = q(shape + 0x30); const int n = i32(shape + 0x38, -1);
					if (!okp(data) || n < 0 || n > 4096) { ++skipped; return; }
					for (int k = 0; k < n; ++k) walk(q(data + 0x20 * (uintptr_t)k), xf, depth + 1);
				}
				else if (r == l.vtCxform || r == l.vtCxform2 || r == l.vtTransform)
				{
					h2::Xf t;
					if (!readXfAt(shape, t)) { ++skipped; return; }
					walk(q(shape + (r == l.vtTransform ? 0x28 : 0x30)), h2::xfCompose(xf, t), depth + 1);
				}
				else if (r == l.vtCtranslate)
				{
					float t[3];
					if (!copy(t, shape + 0x40, 12)) { ++skipped; return; }
					h2::Xf T; T.t = { t[0], t[1], t[2] };
					walk(q(shape + 0x30), h2::xfCompose(xf, T), depth + 1);
				}
				else ++skipped;   // mopp / instance mopp / unknown
			}
		};
	}

	int bodyShapeLines(uintptr_t body, ShapeLines& out)
	{
		MotionState m;
		if (!readMotion(body, m)) return 0;
		ShapeWalker w(out);
		w.walk(q(body + 0x20), m.xf, 0);
		return w.skipped;
	}

	// ---- TIM / ghost sampling -----------------------------------------------------------------------------------------
	namespace
	{
		constexpr int kMaxKids = 768, kMaxSectors = 64;

		// Walk the hkpAgent1nTrack at `track`: {char** sectors; i32 count; i32 capacity}; sector = 512 B, {u32 bytesUsed, 12 B pad,
		// entries...}; entry: cmd @0, agent type @1, size @3, shape key @0x10, [cmd&0xE==4: time stamp @0x18, N @0x20, w @0x2C, agent data @0x30]
		// (sub_18044D500, the 1n machine process loop).
		bool readTrack(uintptr_t track, Pair3& p)
		{
			const uintptr_t sectors = q(track);
			const int32_t ns = i32(track + 8, -1);
			if (!okp(sectors) || ns < 1 || ns > kMaxSectors) { p.note = "agent track header implausible (sector array / count)"; return false; }
			p.sectors = ns;
			for (int s = 0; s < ns && (int)p.kids.size() < kMaxKids; ++s)
			{
				const uintptr_t sec = q(sectors + 8 * (uintptr_t)s);
				if (!okp(sec)) { p.note = "sector pointer unreadable"; return !p.kids.empty(); }
				uint8_t buf[512];
				if (!copy(buf, sec, sizeof(buf))) { p.note = "sector unreadable"; return !p.kids.empty(); }
				const uint32_t used = *(uint32_t*)buf;
				if (used > 496) { p.note = "sector header implausible (bytes used)"; return !p.kids.empty(); }
				uint32_t off = 16;
				while (off < used + 16 && (int)p.kids.size() < kMaxKids)
				{
					const uint8_t* e = buf + off;
					const uint8_t size = e[3];
					if (size < 0x20 || off + size > used + 16 || off + 0x20 > 512) { if (off + 0x20 <= 512 && size == 0) { p.note = "zero-size entry in track"; } break; }
					const uint8_t kind = e[0] & 0xE;                   // 0 = dead slot, 2 = plain child, 4 = child with a TIM, 6 = nested sub-track
					if (kind == 2 || kind == 4)
					{
						Tim3 k;
						k.cmd = e[0]; k.agent = e[1]; k.key = *(const uint32_t*)(e + 0x10);
						k.hasTim = kind == 4 && size >= 0x30;
						if (k.hasTim)
						{
							k.time = *(const float*)(e + 0x18);
							k.n = { *(const float*)(e + 0x20), *(const float*)(e + 0x24), *(const float*)(e + 0x28) };
							k.w = *(const float*)(e + 0x2C);
						}
						p.kids.push_back(k);
					}
					off += size;
				}
			}
			return true;
		}

		// (body, environment mopp) pair entry -> track. Layout from the engine's own entry processor (sub_180457A00 / sub_18045A9A0, H3):
		//   +0x00 cmd (2 = no TIM, 4 = TIM, 6 = nested), +0x01 agent type, +0x03 entry size, +0x18 contact manager / agent object (has a vtable),
		//   +0x20 collision quality index, +0x28 / +0x30 the two collidables, +0x38 TIM time stamp, +0x40 separating normal (xyz), +0x4C w,
		//   +0x50 agent data = the hkpAgent1nTrack for a TIM entry (+0x40 for a plain entry).
		//   cmd 6 (the bv-tree / mopp agent the player capsule gets against the level): the track is at +0x78 - verified live on
		//   120_halo 2026-10-15 (2 sectors, 9 TIM children with unit normals and the step time stamp, terminated by a cmd-1 entry).
		inline uintptr_t pairTrackOffset(uint8_t cmd)
		{
			switch (cmd & 0xE) { case 6: return 0x78; case 4: return 0x50; default: return 0x40; }
		}
		// Verified at runtime against the collidable pointers; if they are not where expected the entry is searched for them and the base
		// shifts accordingly (logged via Pair3::verified / note).
		void readPairEntry(uintptr_t entry, uintptr_t bodyColl, uintptr_t partnerColl, Pair3& p)
		{
			uintptr_t eb = entry;
			uint8_t raw[0x90];
			if (!copy(raw, entry, sizeof(raw))) { p.note = "pair entry unreadable"; return; }
			auto Q = [&](size_t o) { return *(const uintptr_t*)(raw + o); };
			if ((Q(0x28) == bodyColl && Q(0x30) == partnerColl) || (Q(0x28) == partnerColl && Q(0x30) == bodyColl)) p.verified = true;
			else
			{
				for (size_t o = 0; o + 16 <= sizeof(raw); o += 8)
					if ((Q(o) == bodyColl && Q(o + 8) == partnerColl) || (Q(o) == partnerColl && Q(o + 8) == bodyColl))
					{
						eb = entry + o - 0x28;
						p.note = "pair entry layout shifted: collidables found at +0x" + std::to_string(o) + " (expected +0x28)";
						break;
					}
			}
			p.cmd = u8(eb, 0); p.agentType = u8(eb + 1, 0);
			readTrack(eb + pairTrackOffset(p.cmd), p);
		}
	}

	bool sampleGhost(uintptr_t unit, Ghost3& g)
	{
		g = Ghost3{};
		g.tick = gameTick();
		g.obj = unit;
		g.t = worldTime();
		uintptr_t bodies[8];
		const int nb = havokBodies(unit, bodies);
		if (nb <= 0) { g.note = "unit has no Havok rigid body (component datum unset)"; return false; }
		uintptr_t pick = 0; MotionState pm; h2::Capsule pc; int firstVt = -1;
		for (int i = 0; i < nb; ++i)
		{
			if (firstVt < 0) firstVt = shapeVtRva(bodies[i]);
			MotionState m;
			if (!readMotion(bodies[i], m) || m.type == 7) continue;
			const h2::Capsule c = bodyCapsuleLocal(bodies[i]);
			if (!c.ok) continue;
			pick = bodies[i]; pm = m; pc = c; break;
		}
		if (!pick)
		{
			char b[160]; snprintf(b, sizeof(b), "no dynamic capsule body among the unit's %d Havok bodies (first shape vtable RVA 0x%X, expected capsule 0x%X)", nb, firstVt < 0 ? 0 : firstVt, L().vtCapsule);
			g.note = b; g.motionShapeVt = firstVt;
			return false;
		}
		g.body = pick; g.m = pm; g.cap = pc; g.motionShapeVt = shapeVtRva(pick);
		float hp[3];
		if (copy(hp, unit + 0x50, 12) && std::isfinite(hp[0]) && std::isfinite(hp[1]) && std::isfinite(hp[2])) { g.halo = { hp[0], hp[1], hp[2] }; g.haloOk = true; }

		// collision entries of the body: array of 16 B {agent entry*, partner collidable*} at body+0x58, count @+0x60
		const uintptr_t arr = q(pick + 0x58);
		const int32_t cnt = i32(pick + 0x60, -1);
		if (!okp(arr) || cnt < 0 || cnt > 4096) { g.note = "body has no readable collision-entry array"; g.ok = true; return true; }
		const uintptr_t bodyColl = pick + 0x20;
		int others = 0;
		for (int32_t i = 0; i < cnt && g.pairs.size() < 16; ++i)
		{
			const uintptr_t entry = q(arr + 16 * (uintptr_t)i), partner = q(arr + 16 * (uintptr_t)i + 8);
			if (!okp(entry) || !okp(partner)) continue;
			const int8_t ownerOff = (int8_t)u8(partner + 0x20, 0);
			const uintptr_t pent = partner + (intptr_t)ownerOff;
			if (!okp(pent) || u8(pent + 0x100, 0) != 7) { ++others; continue; }           // not a fixed body (object / other dynamic)
			const uintptr_t pshape = q(pent + 0x20);
			if (!okp(pshape)) { ++others; continue; }
			const uintptr_t vt = q(pshape);
			if (vt != base() + L().vtMopp) { ++others; continue; }                         // not the structure bsp's environment mopp
			Pair3 p; p.found = true; p.entry = entry; p.partner = partner; p.partnerEntity = pent;
			// environment mopp -> +0x28 -> collection; its shape-container subobject (+0x20) keeps the structure bsp index @+0x30
			// (sub_180143B6C builds it: collection object -> MOPP; the collection is held at mopp+0x28 (child) and mopp+0x38 (reference); its
			// shape-container subobject at collection+0x20 has the bsp index at +0x30, i.e. collection+0x50 = sub_18021C864's *(this+80))
			for (uintptr_t co : { 0x28u, 0x38u })
			{
				const uintptr_t coll = q(pshape + co);
				if (!okp(coll)) continue;
				const int32_t bi = i32(coll + 0x50, -1);
				if (bi >= 0 && bi < 16) { p.bsp = bi; break; }
			}
			readPairEntry(entry, bodyColl, partner, p);
			g.pairs.push_back(std::move(p));
		}
		if (g.pairs.empty()) g.note = "no (body, structure-bsp environment) pair among " + std::to_string(cnt) + " collision entries (" + std::to_string(others) + " non-environment)";
		g.ok = true;
		return true;
	}

	void dumpPairBytes(const Ghost3& g, std::string& out)
	{
		out.clear();
		if (g.pairs.empty()) return;
		const Pair3& p = g.pairs[0];
		char hex[4];
		auto dump = [&](const char* label, uintptr_t at, size_t n)
		{
			uint8_t buf[0x100]; n = std::min(n, sizeof(buf));
			out += label; out += ":";
			if (!copy(buf, at, n)) { out += " unreadable"; return; }
			for (size_t i = 0; i < n; ++i) { snprintf(hex, sizeof(hex), "%02X", buf[i]); if (i % 16 == 0) out += ' '; out += hex; }
		};
		dump("pair-entry", p.entry, 0x90);
		const uintptr_t track = p.entry + pairTrackOffset(p.cmd);
		const uintptr_t sec = q(q(track));
		out += " | ";
		dump("sector0", sec, 0x80);
	}
}
