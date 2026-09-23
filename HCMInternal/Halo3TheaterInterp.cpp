#include "pch.h"
#include "Halo3TheaterInterp.h"
#include "Halo3TheaterInterpCave.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include "GlobalKill.h"
#include <Windows.h>
#include <Psapi.h>
#include <thread>
#include <stop_token>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cmath>

// ================================================================================================
// HALO 3 THEATER INTERPOLATION
//
// Halo 3 MCC renders Theater straight off the tick stream. The engine DOES interpolate object
// POSITION (sub_180184438), which is why walking and animations look smooth - but camera
// ORIENTATION is rebuilt from raw per-tick unit aiming, so the moment the watched player turns, the
// whole world steps at the tick rate. In gameplay the camera forward comes from live input at frame
// rate, so it never shows; this is Theater-only.
//
// Three sites fix the three visible symptoms. They were found over a long RE session (see
// Fixes/Halo3_Theater_Interp_FIXES.md) and are shipped here as the cave that session produced.
//
// THE CAVE PREFERS moduleBase + kCaveRva (there it is byte-identical to the recovered blob), but it
// is no longer pinned to it: kCaveRva is one page past halo3.dll's image end, i.e. unowned address
// space that another allocation can and did take. relocateCave() lifts the pin - see the header for
// why only 13 fixups are needed and why placement is bounded to +/-2GB.
//
// ⚠⚠ WHY THE ORIGINAL BYTES ARE VERIFIED. These are jmp-over-instruction patches. If an MCC update
// moves the code, a stale RVA lands the jmp in the MIDDLE of an instruction and the game executes
// nonsense - the same failure mode that shipped a reliable crash-on-pause on HaloCER. Every site
// carries the bytes it expects and the patch refuses on a mismatch.
//
// FIRST-PERSON LEGS (LegAttach, 2026-09-22). With the camera smoothed, the FP legs used to judder when the
// watched player turned. They were never the problem: on the ground the engine draws them as a straight
// copy of the body's world-space node array (sub_1802C5A38 gets r9 = NULL because the GROUND motor state's
// def has no +0x38 slot), so they sit at the tick-time pose while cave2 renders an interpolated camera. The
// two are from different instants, and it shows as a 30 Hz sawtooth - strobing into "several copies of the
// legs" at high refresh rates.
//
// ⚠ The fix SUBSTITUTES; it interpolates nothing. LegAttach rotates the legs about the eye by the yaw
// difference between the camera actually rendered and this frame's raw camera - which on screen is exactly
// the fpgate-OFF leg placement (where the legs were always fine), while the world keeps the smooth camera.
// Every earlier attempt re-lerped something that was already per-frame smooth, and that is the one thing
// this must never do: a tick-latched re-lerp of a smooth input lags by a tick and jitters by the frame
// phase. The 2026-09-06 LegAnchor attempt did exactly that, AND hooked a matrix the ground path discards.
// ================================================================================================
class Halo3TheaterInterp::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	uintptr_t mBase = 0;
	uint8_t*  mCave = nullptr;
	bool      mApplied = false;

	struct Save { uintptr_t addr; std::vector<uint8_t> orig; };
	std::vector<Save> mSaves;
	// A revert cannot free the cave immediately - a render frame may still be executing inside it.
	// Same reasoning as FPScaleFix's deferred free.
	std::vector<uint8_t*> mPendingFree;

	static uintptr_t moduleBase()
	{
		return (uintptr_t)GetModuleHandleW(L"halo3.dll");
	}

	// Records the bytes at addr so revert() can put them back. ⚠ ALLOCATES (the Save's vector, and mSaves itself),
	// so it must run BEFORE any ScopedThreadSuspender exists - see apply(). (See ScopedThreadSuspender.)
	void recordSave(uintptr_t addr, size_t len)
	{
		Save s; s.addr = addr; s.orig.resize(len);
		memcpy(s.orig.data(), (void*)addr, len);
		mSaves.push_back(std::move(s));
	}

	// Overwrites code in place. ⚠ Pure memory operations only: this is what runs while every other thread is frozen,
	// so it must never allocate, log or take a lock. VirtualProtect / FlushInstructionCache are kernel calls and are
	// safe there - a thread can only be suspended once it is back in user mode, so none can be holding a kernel lock.
	static void writeCode(uintptr_t addr, const uint8_t* data, size_t len)
	{
		DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
		memcpy((void*)addr, data, len);
		VirtualProtect((void*)addr, len, o, &o);
		FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
	}

	// ⚠ Reads through a __try: an RVA that a game update invalidated can land outside the image.
	static bool readBytes(uintptr_t addr, uint8_t* out, size_t len)
	{
		__try { memcpy(out, (const void*)addr, len); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool verifySites(std::string& why) const
	{
		for (size_t i = 0; i < Halo3TheaterInterp_Detail::kHookCount; ++i)
		{
			const auto& h = Halo3TheaterInterp_Detail::kHooks[i];
			uint8_t actual[16]{};
			if (!readBytes(mBase + h.rva, actual, h.stolen))
			{
				why = std::format("could not read the {} site at halo3.dll+0x{:X}", h.name, h.rva);
				return false;
			}
			if (memcmp(actual, h.original, h.stolen) != 0)
			{
				why = std::format("the {} site at halo3.dll+0x{:X} does not hold the bytes this build "
					"expects - Halo 3 has been updated, or another mod already patched it. Patching anyway "
					"would land a jump inside an instruction.", h.name, h.rva);
				return false;
			}
		}
		return true;
	}

	// Free pages (kCaveSize) within rel32 reach of the module, searched outward from the module base. The cave's
	// four branches back into halo3.dll are rel32, so 2GB is a hard wall - we stay well inside it.
	// (Same shape as MasterTickrate's allocNear; kept local so this TU stays self-contained.)
	static void* allocNearModule(uintptr_t moduleBase, size_t size)
	{
		SYSTEM_INFO si; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity;
		const uintptr_t reach = 0x60000000ULL;                       // 1.5GB, comfortably inside rel32
		const uintptr_t lo = moduleBase > reach ? moduleBase - reach : 0x10000ULL;
		const uintptr_t hi = moduleBase + reach;
		const uintptr_t start = moduleBase & ~(gran - 1);
		for (uintptr_t p = start; p > lo; p -= gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
		for (uintptr_t p = start + gran; p < hi; p += gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
		return nullptr;
	}

	// ── DIAGNOSTIC LOGGER ──────────────────────────────────────────────────────────────────────────
	// Samples the cave's LegAttach counters once a second while the fix is armed and writes the per-second
	// rates to the HCM log (prefix H3LEGS). This replaces reading addresses live in Cheat Engine: the cave
	// is routinely RELOCATED (its preferred page is one past halo3.dll's image end and is often taken), so
	// the address moves between game sessions, and a log records every second rather than one look.
	//
	// ⚠⚠ THE THREAD MUST NOT EXIST WHILE ScopedThreadSuspender IS ALIVE. The suspender forbids heap
	// allocation because a frozen thread may hold the heap lock, and this thread allocates constantly
	// (std::format, the logger). It is therefore stopped and JOINED before apply()/revert() suspend anything,
	// and started only after apply() has resumed every thread.
	//
	// Offsets are relative to the cave base and must match the diagnostics block in Halo3TheaterInterpCave.h.
	static constexpr size_t kOffMode        = 0x270;   // u32 LegAttach MODE
	static constexpr size_t kOffEntries     = 0x274;   // u32 LegAttach entries
	static constexpr size_t kOffDiag        = 0x780;   // start of the diagnostics block
	static constexpr size_t kDiagLen        = 0x2C;    // +00..+28 inclusive
	static constexpr size_t kOffSlot0Raw    = 0x64C;   // f32 x2 raw.xy captured by cave2 (user slot 0)
	static constexpr size_t kOffSlot0Smooth = 0x654;   // f32 x2 smoothed.xy cave2 wrote
	static constexpr size_t kOffRingDiag    = 0x23C;   // u32 x5: depth0, depth>0, STALE, depth sum, depth max
	static constexpr size_t kOffRingCount   = 0x260;   // u32 ring publishes (== captures)

	struct DiagSnap
	{
		bool ok = false;
		uint32_t mode = 0, entries = 0;
		uint32_t ground = 0, anchor = 0, nodesInterp = 0, nodesRaw = 0, noMatch = 0, match = 0;
		uint32_t groundFix = 0, anchorFix = 0, captures = 0;
		float sinD = 0, cosD = 0, rawX = 0, rawY = 0, smX = 0, smY = 0;
		uint32_t depth0 = 0, depthN = 0, stale = 0, depthSum = 0, depthMax = 0, ringCount = 0;
	};

	static DiagSnap readDiag(const uint8_t* cave)
	{
		DiagSnap s;
		uint8_t d[kDiagLen]{};
		uint8_t k[8]{};
		float raw[2]{}, sm[2]{};
		if (!readBytes((uintptr_t)cave + kOffDiag, d, sizeof(d))) return s;
		if (!readBytes((uintptr_t)cave + kOffMode, k, sizeof(k))) return s;
		if (!readBytes((uintptr_t)cave + kOffSlot0Raw, (uint8_t*)raw, sizeof(raw))) return s;
		if (!readBytes((uintptr_t)cave + kOffSlot0Smooth, (uint8_t*)sm, sizeof(sm))) return s;
		uint32_t rd[5]{}, rc = 0;
		if (!readBytes((uintptr_t)cave + kOffRingDiag, (uint8_t*)rd, sizeof(rd))) return s;
		if (!readBytes((uintptr_t)cave + kOffRingCount, (uint8_t*)&rc, sizeof(rc))) return s;
		s.depth0 = rd[0]; s.depthN = rd[1]; s.stale = rd[2]; s.depthSum = rd[3]; s.depthMax = rd[4]; s.ringCount = rc;
		auto u = [&](size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; };
		auto f = [&](size_t o) { float v; memcpy(&v, d + o, 4); return v; };
		memcpy(&s.mode, k, 4); memcpy(&s.entries, k + 4, 4);
		s.ground = u(0x00); s.anchor = u(0x04); s.nodesInterp = u(0x08); s.nodesRaw = u(0x0C);
		s.noMatch = u(0x10); s.match = u(0x14); s.sinD = f(0x18); s.cosD = f(0x1C);
		s.groundFix = u(0x20); s.anchorFix = u(0x24); s.captures = u(0x28);
		s.rawX = raw[0]; s.rawY = raw[1]; s.smX = sm[0]; s.smY = sm[1];
		s.ok = true;
		return s;
	}

	// ── v2 render re-timing diagnostics (cave+0x2110.., layout in rt_design\rt_layout.txt) ──────────────────
	static constexpr size_t kOffRtDiag  = 0x2110;   // u32 x28: hooks, retimed, skip[14], lastReason, resync, reset,
	                                                //  clampHi, clampLo, fpMat, posTele, dtApply, dtSkip, ovHit, histRec, histInTick
	static constexpr size_t kOffRtSums  = 0x2180;   // f64 x4: sum age, sum age^2, sum phase err, sum phase err^2
	static constexpr size_t kOffRtPeriod = 0x20A8;  // f64 locked display period (s)
	static constexpr size_t kOffRtCoast = 0x21F0;   // u32 x2: coasts, frame-rate changes (v2.1 re-measures)
	static constexpr size_t kOffEyeAim  = 0x3E04;   // u32 x3 (v2.2): eye-build aim reads, aims replaced, ring resets
	static constexpr size_t kOffLegPiv  = 0x3E30;   // u32 x2 (v2.3): eye shifts measured, leg corrections applied

	struct RtSnap
	{
		bool ok = false;
		uint32_t u[28]{};
		double sums[4]{};
		double period = 0;
		uint32_t coast = 0, subh = 0;
		uint32_t eyeAim[3]{};
		uint32_t legPivot[2]{};
	};

	static RtSnap readRt(const uint8_t* cave)
	{
		RtSnap s;
		uint32_t cs[2]{};
		if (!readBytes((uintptr_t)cave + kOffRtDiag, (uint8_t*)s.u, sizeof(s.u))) return s;
		if (!readBytes((uintptr_t)cave + kOffRtSums, (uint8_t*)s.sums, sizeof(s.sums))) return s;
		if (!readBytes((uintptr_t)cave + kOffRtPeriod, (uint8_t*)&s.period, sizeof(s.period))) return s;
		if (!readBytes((uintptr_t)cave + kOffRtCoast, (uint8_t*)cs, sizeof(cs))) return s;
		if (!readBytes((uintptr_t)cave + kOffEyeAim, (uint8_t*)s.eyeAim, sizeof(s.eyeAim))) return s;
		if (!readBytes((uintptr_t)cave + kOffLegPiv, (uint8_t*)s.legPivot, sizeof(s.legPivot))) return s;
		s.coast = cs[0]; s.subh = cs[1];
		s.ok = true;
		return s;
	}

	// One H3RT line: per-second counts, why frames were not re-timed, the clock lock and the frame age.
	static void logRt(const RtSnap& c, const RtSnap& p)
	{
		const uint32_t hooks = c.u[0] - p.u[0], retimed = c.u[1] - p.u[1];
		std::string skips;
		static const char* kReason[14] = { "", "busy", "timer", "frameLimit", "slot", "same", "cinematic", "time",
			"notTheater", "notFP", "history", "age", "degenerate", "interpOff" };
		for (int r = 1; r < 14; ++r)
			if (const uint32_t n = c.u[2 + r] - p.u[2 + r]) skips += std::format(" {}={}", kReason[r], n);
		const double n = (double)retimed;
		const double ageMean = n ? (c.sums[0] - p.sums[0]) / n : 0.0;
		const double ageRms = n ? std::sqrt(std::max(0.0, (c.sums[1] - p.sums[1]) / n - ageMean * ageMean)) : 0.0;
		const double perrMean = n ? (c.sums[2] - p.sums[2]) / n : 0.0;
		const double perrRms = n ? std::sqrt(std::max(0.0, (c.sums[3] - p.sums[3]) / n - perrMean * perrMean)) : 0.0;
		PLOG_INFO << std::format(
			"H3RT /s: frames={} retimed={} ({:.1f}%) | skipped:{} | clock {:.2f} Hz snap={} coast={} warmup={} rateChange={} "
			"hookJitter={:.3f}ms | age mean={:.3f}ms sd={:.3f}ms | legHandoff={} hist={} inTick={} fpMat={} "
			"dt ok/refused={}/{} clamp hi/lo={}/{} | eyeAim replaced/reads={}/{} ringResets={} | legPivot shifts/applied={}/{}",
			hooks, retimed, hooks ? 100.0 * retimed / hooks : 0.0, skips.empty() ? " none" : skips,
			c.period > 0 ? 1.0 / c.period : 0.0, c.u[17] - p.u[17], c.coast - p.coast, c.u[18] - p.u[18], c.subh - p.subh,
			1000.0 * perrRms, 1000.0 * ageMean, 1000.0 * ageRms, c.u[25] - p.u[25], c.u[26] - p.u[26], c.u[27] - p.u[27],
			c.u[21] - p.u[21], c.u[23] - p.u[23], c.u[24] - p.u[24], c.u[19] - p.u[19], c.u[20] - p.u[20],
			c.eyeAim[1] - p.eyeAim[1], c.eyeAim[0] - p.eyeAim[0], c.eyeAim[2] - p.eyeAim[2],
			c.legPivot[0] - p.legPivot[0], c.legPivot[1] - p.legPivot[1]);
	}

	static void diagLoop(std::stop_token st, const uint8_t* cave)
	{
		std::mutex m;
		std::condition_variable_any cv;
		DiagSnap prev = readDiag(cave);
		RtSnap rtPrev = readRt(cave);
		int quietSeconds = 0;
		while (!st.stop_requested())
		{
			{
				std::unique_lock lk(m);
				cv.wait_for(lk, st, std::chrono::seconds(1), [] { return false; });
			}
			if (st.stop_requested()) break;

			const DiagSnap cur = readDiag(cave);
			if (!cur.ok) continue;

			// Unsigned subtraction is correct across a counter wrap.
			const uint32_t dEntries = cur.entries - prev.entries;

			// v2 re-timing: one line per second while theater first person is on screen (legs drawn or frames
			// re-timed). The hook runs on every rendered frame, so without that gate the menus would log too.
			if (const RtSnap rt = readRt(cave); rt.ok)
			{
				if (dEntries || rt.u[1] != rtPrev.u[1]) logRt(rt, rtPrev);
				rtPrev = rt;
			}
			if (dEntries == 0)
			{
				// Nothing drawn this second: not in theater first person, legs off screen (looking up),
				// or a menu. Say so occasionally so an empty stretch of log is not mistaken for a dead logger.
				if (++quietSeconds % 15 == 0)
					PLOG_INFO << "H3LEGS idle " << quietSeconds << "s - fix armed, legs not drawn (not in theater FP, or looking up)";
				prev = cur;
				continue;
			}
			quietSeconds = 0;

			const uint32_t dMatch = cur.match - prev.match, dNo = cur.noMatch - prev.noMatch;
			const double noPct = (dMatch + dNo) ? 100.0 * dNo / (double)(dMatch + dNo) : 0.0;

			PLOG_INFO << std::format(
				"H3LEGS mode={} /s: entries={} ground={} anchor={} | nodes interp={} raw={} | match={} NOMATCH={} ({:.1f}%) "
				"| groundFix={} anchorFix={} captures={} | ring depth0={} deeper={} STALE={} avgDepth={:.2f} maxDepth={} "
				"| last sinD={:+.4f} cosD={:.4f} | slot0 raw=({:+.3f},{:+.3f}) smooth=({:+.3f},{:+.3f})",
				cur.mode, dEntries, cur.ground - prev.ground, cur.anchor - prev.anchor,
				cur.nodesInterp - prev.nodesInterp, cur.nodesRaw - prev.nodesRaw,
				dMatch, dNo, noPct,
				cur.groundFix - prev.groundFix, cur.anchorFix - prev.anchorFix, cur.captures - prev.captures,
				cur.depth0 - prev.depth0, cur.depthN - prev.depthN, cur.stale - prev.stale,
				(cur.depth0 - prev.depth0 + cur.depthN - prev.depthN)
					? (double)(cur.depthSum - prev.depthSum) / (double)(cur.depth0 - prev.depth0 + cur.depthN - prev.depthN) : 0.0,
				cur.depthMax,
				cur.sinD, cur.cosD, cur.rawX, cur.rawY, cur.smX, cur.smY);
			prev = cur;
		}
	}

	void startDiagLogger()
	{
		stopDiagLogger();
		if (!mCave) return;
		const uint8_t* cave = mCave;
		PLOG_INFO << std::format("H3LEGS logger started - cave at 0x{:X} (halo3.dll+0x{:X}); one line per second while "
			"the legs are drawn. Columns are per-second counts from the cave's diagnostics block.",
			(uintptr_t)cave, (uintptr_t)cave - mBase);
		mDiagThread = std::jthread([cave](std::stop_token st) { diagLoop(st, cave); });
	}

	// Blocks until the logger thread has fully exited. Safe to call when it is not running.
	void stopDiagLogger()
	{
		if (mDiagThread.joinable())
		{
			mDiagThread.request_stop();
			mDiagThread.join();
		}
	}

	std::jthread mDiagThread;

	bool apply()
	{
		if (mApplied) return true;

		// ⚠ Before anything below can construct a ScopedThreadSuspender - see the logger note above.
		stopDiagLogger();

		mBase = moduleBase();
		if (!mBase)
			throw HCMRuntimeException("halo3.dll is not loaded yet - load a level first");

		std::string why;
		if (!verifySites(why))
			throw HCMRuntimeException("Theater Interpolation Fix: " + why);

		// Free anything a previous revert deferred, BEFORE the suspender exists (see ScopedThreadSuspender:
		// no virtual-memory calls while threads are frozen).
		for (uint8_t* p : mPendingFree) VirtualFree(p, 0, MEM_RELEASE);
		mPendingFree.clear();

		// PREFER moduleBase + kCaveRva - there the cave is byte-identical to the recovered blob and no
		// fixups apply at all. But that page is one past halo3.dll's image end (SizeOfImage is exactly
		// 0x4768000), so it is unowned space anything can take, and something did. Fall back to any slot
		// within +/-2GB and relocate; only refuse if nothing in rel32 range is free.
		const uintptr_t want = mBase + Halo3TheaterInterp_Detail::kCaveRva;
		mCave = (uint8_t*)VirtualAlloc((void*)want, Halo3TheaterInterp_Detail::kCaveSize,
			MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

		bool relocated = false;
		if (!mCave)
		{
			mCave = (uint8_t*)allocNearModule(mBase, Halo3TheaterInterp_Detail::kCaveSize);
			if (!mCave)
				throw HCMRuntimeException(std::format(
					"Theater Interpolation Fix: could not reserve the {}KB cave anywhere within 2GB of "
					"halo3.dll (preferred halo3.dll+0x{:X} is taken). Restarting the game should clear it.",
					Halo3TheaterInterp_Detail::kCaveSize / 1024, Halo3TheaterInterp_Detail::kCaveRva));
			relocated = true;
		}

		memcpy(mCave, Halo3TheaterInterp_Detail::kCave, Halo3TheaterInterp_Detail::kCaveSize);

		if (relocated)
		{
			// A wrong fixup would send the cave's `jmp rax` into arbitrary memory, so prove it rather
			// than assume it: relocate, then re-derive every game address the way the cave will and
			// require it to match. Anything short of an exact match frees the page and refuses.
			if (!Halo3TheaterInterp_Detail::relocateCave(mCave, (uintptr_t)mCave, mBase)
				|| !Halo3TheaterInterp_Detail::verifyRelocation(mCave, (uintptr_t)mCave, mBase))
			{
				VirtualFree(mCave, 0, MEM_RELEASE); mCave = nullptr;
				throw HCMRuntimeException(
					"Theater Interpolation Fix: the cave was placed away from its preferred address and "
					"the relocation did not verify. Refusing to arm.");
			}
			PLOG_INFO << "Halo3TheaterInterp: cave relocated to " << (void*)mCave
				<< " (preferred halo3.dll+0x" << std::hex << Halo3TheaterInterp_Detail::kCaveRva
				<< " was taken); " << std::dec << Halo3TheaterInterp_Detail::kAbsFixupCount
				<< " absolute + " << Halo3TheaterInterp_Detail::kRelFixupCount << " rel32 fixups verified";
		}

		// Knob values. ⚠ Write ONLY what we mean to change - every knob already holds the value the
		// recovered (working) blob shipped, so a stray write is a behaviour change, not a no-op. HCM
		// used to write 3 into DESTGATE and 1 into OBSPOS through a mislabelled table; that second one
		// is what made the body detach. OBSPOS is written explicitly as 0 so the intent is on the record
		// rather than relying on the blob's contents.
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobFpCode - Halo3TheaterInterp_Detail::kCaveRva)) = 3;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobEnable - Halo3TheaterInterp_Detail::kCaveRva)) = 1;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobObsPos - Halo3TheaterInterp_Detail::kCaveRva)) = 0;
		// Our own knob (not from the blob): the LegAttach MODE. 1 = fix. Poke live to A/B without touching
		// the camera fix: 0 = diagnostics only (stock legs), 4 = the fix with its sign flipped, which MUST look
		// worse - if 1 and 4 look the same, the correction is not reaching the screen. kLegFixHits counts
		// entries, so a stuck-at-0 counter means the hook never fired, not that the fix did nothing. Full
		// readout table in Halo3TheaterInterpCave.h.
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobLegFix - Halo3TheaterInterp_Detail::kCaveRva)) = 1;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kLegFixHits - Halo3TheaterInterp_Detail::kCaveRva)) = 0;
		setCrouchKnob(currentCrouchSetting());

		FlushInstructionCache(GetCurrentProcess(), mCave, Halo3TheaterInterp_Detail::kCaveSize);

		// ⚠⚠ EVERYTHING THAT ALLOCATES HAPPENS HERE, BEFORE ANY THREAD IS FROZEN. ScopedThreadSuspender forbids heap
		// allocation while it is alive: a frozen thread may hold the heap lock, and the allocating thread then
		// deadlocks forever. This used to record each Save from INSIDE the suspended scope, which allocated twice per
		// hook - a rare but unbounded hang on every arm. So: build every patch into fixed storage and record every
		// original byte now; the suspended scope below only writes memory.
		// (It also fixes an ordering bug: an allocation failure mid-loop used to leave earlier hooks PATCHED with
		// mApplied still false, where revert() refuses to undo them. Now nothing is written until everything is
		// recorded.)
		struct Patch { uintptr_t addr; uint8_t bytes[16]; uint8_t len; };
		Patch patches[Halo3TheaterInterp_Detail::kHookCount]{};
		mSaves.reserve(mSaves.size() + Halo3TheaterInterp_Detail::kHookCount);
		for (size_t i = 0; i < Halo3TheaterInterp_Detail::kHookCount; ++i)
		{
			const auto& h = Halo3TheaterInterp_Detail::kHooks[i];
			Patch& p = patches[i];
			p.addr = mBase + h.rva;
			p.len = h.stolen;
			memset(p.bytes, 0x90, sizeof(p.bytes));            // NOP the remainder of the stolen bytes
			p.bytes[0] = 0xE9;
			// ⚠ Relative to the cave's ACTUAL base, not mBase + caveRva - the cave may have been
			// relocated above, and this jmp is what enters it.
			const uintptr_t entry = (uintptr_t)mCave + (h.caveRva - Halo3TheaterInterp_Detail::kCaveRva);
			const int32_t rel = (int32_t)((intptr_t)entry - (intptr_t)(p.addr + 5));
			memcpy(p.bytes + 1, &rel, 4);
			recordSave(p.addr, p.len);
		}

		// Arm the hooks with every other thread frozen: the render path runs per-frame and a torn instruction here
		// is an immediate crash. Memory writes ONLY in this scope.
		{
			ScopedThreadSuspender suspend;
			for (const Patch& p : patches)
				writeCode(p.addr, p.bytes, p.len);
		}

		mApplied = true;

		// Only now, with every thread resumed and the suspender destroyed.
		startDiagLogger();
		return true;
	}

	void revert()
	{
		// ⚠ Stop and JOIN the logger before the suspender below exists - see the logger note.
		stopDiagLogger();

		if (!mApplied) return;
		{
			ScopedThreadSuspender suspend;
			// Reverse order: the hooks come off first so nothing can enter the cave again.
			for (auto it = mSaves.rbegin(); it != mSaves.rend(); ++it)
			{
				DWORD o; VirtualProtect((void*)it->addr, it->orig.size(), PAGE_EXECUTE_READWRITE, &o);
				memcpy((void*)it->addr, it->orig.data(), it->orig.size());
				VirtualProtect((void*)it->addr, it->orig.size(), o, &o);
				FlushInstructionCache(GetCurrentProcess(), (void*)it->addr, it->orig.size());
			}
		}
		mSaves.clear();
		if (mCave) { mPendingFree.push_back(mCave); mCave = nullptr; }   // deferred - a frame may be inside it
		mApplied = false;
	}

	bool currentCrouchSetting() const
	{
		if (auto s = settingsWeak.lock()) return s->halo3TheaterInterpCrouch->GetValue();
		return true;
	}

	void setCrouchKnob(bool on)
	{
		if (!mCave) return;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobCrouch - Halo3TheaterInterp_Detail::kCaveRva))
			= on ? 1u : 0u;
	}

	void onToggle(bool& newValue)
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (newValue) apply(); else revert();

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Theater Interpolation Fix on."
					: "Theater Interpolation Fix off.");
		}
		catch (HCMRuntimeException ex)
		{
			try { revert(); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onCrouchToggle(bool& newValue)
	{
		// A live knob - no re-patching, the cave reads it every frame.
		setCrouchKnob(newValue);
	}

	// Declared LAST - ScopedCallbacks subscribe inside their own constructors.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<ToggleEvent> mCrouchCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->halo3TheaterInterpToggle->valueChangedEvent,
			[this](bool& n) { onToggle(n); }),
		mCrouchCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->halo3TheaterInterpCrouch->valueChangedEvent,
			[this](bool& n) { onCrouchToggle(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo3)
			throw HCMInitException("Theater Interpolation Fix is Halo 3 only");
	}

	~Impl()
	{
		mToggleCallback.removeCallback();
		mCrouchCallback.removeCallback();
		// Before revert(), and before the cave can be freed below: the logger thread reads it.
		stopDiagLogger();
		try { revert(); } catch (...) {}
		for (uint8_t* p : mPendingFree) VirtualFree(p, 0, MEM_RELEASE);
		if (mCave) VirtualFree(mCave, 0, MEM_RELEASE);
	}
};


Halo3TheaterInterp::Halo3TheaterInterp(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}

Halo3TheaterInterp::~Halo3TheaterInterp()
{
	PLOG_VERBOSE << "~" << getName();
}
