#include "pch.h"
#include "H2CV_ReachOob.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>

using namespace mem;

namespace hr
{
	namespace
	{
		constexpr uint32_t kPeStamp = 0x68A0EFE1u, kPeSize = 0x4EDA000u;
		constexpr uint32_t kRvaTlsIndex = 0xC17B18;     // dword: implicit TLS index of haloreach.dll
		constexpr uint32_t kRvaLimit = 0xA8AD74;        // float 0.5: seconds out of bounds before the kill
		constexpr uint32_t kTlsPlayers = 0x18, kTlsGameTime = 0xA0;
		constexpr uint32_t kPlayerStride = 0x490, kPlayerUnit = 0x28, kPlayerCounter = 0x85;

		typedef LONG(NTAPI* t_NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);
		struct ThreadBasicInfo { LONG ExitStatus; PVOID TebBaseAddress; PVOID UniqueProcess; PVOID UniqueThread; ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority; };

		bool plausibleTls(uintptr_t t)
		{
			if (!okp(t)) return false;
			const uintptr_t hdr = q(t + kTlsPlayers), tt = q(t + kTlsGameTime);
			if (!okp(hdr) || !okp(tt) || !okp(q(hdr + 0x50))) return false;
			const int16_t tps = (int16_t)u16(tt + 6, 0);
			return u8(tt, 0) != 0 && tps > 0 && tps <= 120;
		}

		// the game thread's TLS block, found from any thread by walking the process's threads (see H2CV_Halo3 findGameTls)
		uintptr_t findGameTls()
		{
			static uintptr_t sCached = 0; static ULONGLONG sNext = 0;
			if (plausibleTls(sCached)) return sCached;
			const ULONGLONG now = GetTickCount64();
			if (now < sNext) return 0;
			sNext = now + 1000;
			sCached = 0;
			const uint32_t idx = u32(base() + kRvaTlsIndex, 0xFFFFFFFFu);
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
				if (NtQIT(th, 0, &tbi, sizeof(tbi), nullptr) == 0 && tbi.TebBaseAddress)
				{
					const uintptr_t arr = q((uintptr_t)tbi.TebBaseAddress + 0x58);
					const uintptr_t blk = okp(arr) ? q(arr + 8 * (uintptr_t)idx) : 0;
					if (plausibleTls(blk)) sCached = blk;
				}
				CloseHandle(th);
			}
			CloseHandle(snap);
			if (sCached) LOGF("Reach death bar: game-thread TLS block found: 0x%llX", (unsigned long long)sCached);
			return sCached;
		}
	}

	uintptr_t base() { return (uintptr_t)GetModuleHandleW(L"haloreach.dll"); }

	bool buildOk()
	{
		static uintptr_t sBase = 0; static bool sOk = false;
		const uintptr_t b = base();
		if (!b) return false;
		if (b == sBase) return sOk;
		sBase = b; sOk = false;
		IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
		if (copy(&dos, b, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && copy(&nt, b + dos.e_lfanew, sizeof(nt)))
			sOk = nt.FileHeader.TimeDateStamp == kPeStamp && nt.OptionalHeader.SizeOfImage == kPeSize;
		LOGF("haloreach.dll build check %s", sOk ? "OK" : "FAILED (needs MCC 1.3528)");
		return sOk;
	}

	bool readDeath(Death& out)
	{
		out = Death{};
		if (!base()) { out.note = "haloreach.dll not loaded"; return false; }
		if (!buildOk()) { out.note = "unsupported haloreach.dll build (needs MCC 1.3528)"; return false; }
		const uintptr_t t = findGameTls();
		if (!okp(t)) { out.note = "game-thread TLS block not found yet"; return false; }
		const uintptr_t data = q(q(t + kTlsPlayers) + 0x50);
		const uintptr_t tt = q(t + kTlsGameTime);
		if (!okp(data) || !okp(tt)) { out.note = "players / game time not readable"; return false; }
		// the local player: the first player that has a unit (campaign)
		uintptr_t player = 0; uint32_t index = 0;
		for (uint32_t i = 0; i < 16; ++i)
		{
			const uintptr_t e = data + (uintptr_t)i * kPlayerStride;
			const uint32_t unit = u32(e + kPlayerUnit, 0xFFFFFFFFu);
			if (unit != 0xFFFFFFFFu && unit != 0) { player = e; index = i; break; }
		}
		if (!player) { out.note = "no player has a unit (dead / respawning)"; return false; }
		int tps = (int)(int16_t)u16(tt + 6, 30);
		if (tps <= 0 || tps > 120) tps = 30;
		float sec = 0.5f;
		if (!copy(&sec, base() + kRvaLimit, 4) || !(sec > 0.f && sec < 10.f)) sec = 0.5f;
		const uint32_t tick = u32(tt + 0xC, 0);
		out.ticksPerSec = tps;
		out.limit = (int)(tps * sec + 0.5f);
		out.counter = (int)u8(player + kPlayerCounter, 0);
		out.playerIndex = index;
		out.nextCheck = (int)((16u - ((tick + index) & 15u)) & 15u);
		out.valid = true;
		return true;
	}
}
