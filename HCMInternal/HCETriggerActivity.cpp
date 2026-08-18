#include "pch.h"
#include "HCEAnchors.h"
#include "HCETriggerActivity.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include <array>
#include <atomic>
#include <mutex>

// ================================================================================================================
// Halo Campaign Evolved scenario trigger-volume activity tracker.
//
// ---------------------------------------------------------------------------------------------------------------
// THE FUNCTION
// ---------------------------------------------------------------------------------------------------------------
// HaloSimulation_tag_release.dll RVA 0x2E45F0 (IDA VA 0x1802E45F0) is trigger_volume_test_point:
//
//     bool trigger_volume_test_point(int volumeIndex, int objectIndexOrMinusOne, const float* point)
//
// Identified by its consumption of the volume index:
//
//     0x2E4609  48 63 D9              movsxd rbx, ecx            ; arg1 = scenario trigger-volume block index
//     0x2E4634  4C 6B FB 1F           imul   r15, rbx, 1Fh       ; 31 dwords == the 124-byte scenario
//     0x2E4638  8B 88 7C 02 00 00     mov    ecx, [rax+27Ch]     ;   trigger-volume stride
//
// so ECX at every call site IS the scenario trigger-volume block index - there is nothing to convert. The
// address was NOT taken on trust: hceTriggerVolumeTestPointOriginalBytes below is the first 0x48 bytes of the
// function and that (wildcarding the four rip-relative displacements) matches EXACTLY ONCE in the whole .text
// section, and the bytes were re-read straight out of the shipped DLL file (RVA 0x2E45F0 -> file offset
// 0x2E39F0; .text VA 0x1000, raw 0x400) rather than out of a stale disassembly database.
//
// ---------------------------------------------------------------------------------------------------------------
// WHY THE CALL SITES AND NEVER THE PROLOGUE - THIS IS THE ENTIRE FEATURE
// ---------------------------------------------------------------------------------------------------------------
// trigger_volume_test_point has 23 call sites. Only EIGHT of them are HaloScript. The other fifteen are AI,
// object placement, damage and safe-zone code that poll volumes every tick no matter what the mission script is
// doing. A detour on the prologue therefore reports EVERY volume as live and destroys the green/red distinction
// completely. The call-site filter is not an optimisation, it is the point.
//
// The split is not a guess. HaloCER's HaloScript function table lives in .rdata as 0x50-byte records
//     +0x00 const char* name   +0x10 parse fn (always sub_180350490)   +0x18 evaluate fn
// and walking every record in .rdata that has sub_180350490 at +0x10 yields 1660 script functions. Cross
// referencing the 23 callers against that table gives, exactly:
//
//   RVA 0x1B275A  in sub_1801B2630  = evaluate fn of "volume_teleport_players_not_inside"
//   RVA 0x1B2884  in sub_1801B2820  = evaluate fn of "volume_test_object"
//   RVA 0x1B29A9  in sub_1801B28B0  = evaluate fn of "volume_test_objects"
//   RVA 0x1B2B09  in sub_1801B2A10  = evaluate fn of "volume_test_objects_all"
//   RVA 0x1B2C61  in sub_1801B2B70  = evaluate fn of "volume_test_players"
//   RVA 0x1B2DC5  in sub_1801B2CD0  = evaluate fn of "volume_test_players_all"
//   RVA 0x1B3178  in sub_1801B2ED0  = evaluate fn of "volume_return_objects_by_campaign_type"
//   RVA 0x2E50D6  in sub_1802E4E90  = a helper whose ONLY two code callers are the evaluate fns of
//                                     "volume_return_objects" (sub_1801B2E30) and
//                                     "volume_return_objects_by_type" (sub_1801B2E80)
//
// Every other caller is absent from that table and is reached from engine systems, not from script:
//   sub_180133610, sub_18014CBE0, sub_18018F870 (2 sites, called from AI), sub_18019CDB0, sub_180304B80,
//   sub_1803050C0, sub_1803525D0, sub_1803526A0 (2 sites), sub_1803A0CC0 (2 sites),
//   sub_18067FC30 (11 callers, object/damage code), sub_18069C590 (2 sites).
// None of them is patched.
//
// ---------------------------------------------------------------------------------------------------------------
// THE PATCH
// ---------------------------------------------------------------------------------------------------------------
// We rewrite the 4-byte rel32 displacement of each existing `E8` call so it lands on our stub instead. That
// touches no prologue, so .pdata / UNWIND_INFO stay exactly valid. A trampoline over the prologue would instead
// leave a ~5-instruction window covered by a RUNTIME_FUNCTION that no longer describes the code there, and any
// SEH unwind crossing it would walk garbage. None of the eight displacements straddles a 64-byte cache line
// (checked: (site+1) & 63 is 27, 5, 42, 10, 34, 6, 57 and 23), so each 4-byte store is atomic against a
// concurrently fetching core, and every other thread is suspended around the write anyway.
//
// FAIL CLOSED. Before a single byte is written we require, for the whole set:
//   * the function's first 0x48 bytes match hceTriggerVolumeTestPointOriginalBytes exactly, AND
//   * every call site's first byte is 0xE8, AND
//   * every call site's own rel32 already resolves to the function we verified.
// If any one of those fails, nothing at all is patched. A stale offset is inert, never catastrophic.
//
// ---------------------------------------------------------------------------------------------------------------
// THE STUB IS SELF CONTAINED MACHINE CODE, ON PURPOSE
// ---------------------------------------------------------------------------------------------------------------
// The stub calls nothing inside HCMInternal.dll. It reads the clock through kernel32!GetTickCount and writes
// into a VirtualAlloc'd table, neither of which lives in our image. If HCM is unloaded while a game thread is
// mid-stub, there is nothing dangling to return into. The stub page and the table are deliberately never freed
// (~24 KB for the process lifetime) for exactly the same reason - a thread can be inside the stub at any
// instant and there is no cheap way to prove otherwise.
//
// It also cannot throw, allocate, lock or log. It runs once per (volume, candidate object) on the game thread.
//
//   sub  rsp, 48h                      ; entry rsp%16==8 -> 0 here, 20h shadow + 20h saves + 8 pad
//   mov  [rsp+20h], rcx                ; arg1 volumeIndex (also our own scratch copy)
//   mov  [rsp+28h], rdx                ; arg2
//   mov  [rsp+30h], r8                 ; arg3
//   mov  [rsp+38h], r9                 ; not an argument of the target, saved anyway
//   call qword ptr [rip+GetTickCount]  ; eax = ms since boot
//   mov  ecx, [rsp+20h]                ; volumeIndex, zero extended - a negative index fails the unsigned cmp
//   cmp  ecx, kMaxTrackedVolumes
//   jae  skip
//   mov  rdx, qword ptr [rip+table]
//   or   eax, 1                        ; 0 is the "never tested" sentinel, so never store it (costs <=1ms)
//   mov  [rdx+rcx*4], eax
// skip:
//   mov  rcx, [rsp+20h] ... mov r9, [rsp+38h]
//   add  rsp, 48h
//   jmp  qword ptr [rip+realFunction]  ; TAIL jump: [rsp] is still the original return address, so the real
//                                      ; function returns straight to the caller and rax is untouched by us
//
// ---------------------------------------------------------------------------------------------------------------
// WALL CLOCK, NOT TICKS
// ---------------------------------------------------------------------------------------------------------------
// The window is 3000 ms of GetTickCount, with unsigned (wrap safe) subtraction. It is deliberately NOT counted
// in simulation ticks: HaloScript's sleep_until re-evaluates its condition only every N ticks, so a script that
// is genuinely watching a volume tests it in bursts. A tick-counted window makes those live triggers flicker
// red between their own polls.
// ================================================================================================================

namespace
{
	// Scenario block maximum is MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO == 0x600 (1536) for
	// scenario_trigger_volume_block. Rounded up so an out-of-spec map still cannot walk off the table; anything
	// at or above this is silently ignored by both the stub and the reader.
	constexpr uint32_t kMaxTrackedVolumes = 4096;

	constexpr uint32_t kDefaultWindowMilliseconds = 3000;

	// Never freed - see the header comment. Written by the stub (plain 32-bit stores from the game thread),
	// read here.
	std::atomic<uint32_t*> gLastTestedTable{ nullptr };

	std::string bytesToString(const std::vector<byte>& bytes)
	{
		std::string out;
		for (auto b : bytes)
		{
			if (!out.empty()) out += ' ';
			out += std::format("{:02X}", (uint32_t)(unsigned char)b);
		}
		return out;
	}
}


class HCETriggerActivity::HCETriggerActivityImpl
{
private:
	GameState mGame;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// All pure PointerDataStore (XML) lookups - constructing these touches no game memory. The sim dll is not
	// guaranteed to be loaded when cheats are constructed, so nothing here is resolved until setEnabled(true).
	std::shared_ptr<MultilevelPointer> mFunction;
	std::shared_ptr<std::vector<byte>> mExpectedBytes;
	std::vector<std::shared_ptr<MultilevelPointer>> mCallSites;

	mutable std::mutex mStateMutex;   // guards everything below
	bool mEnabled = false;
	uint8_t* mStubPage = nullptr;     // never freed
	uintptr_t mStubEntry = 0;
	struct PatchedSite { uintptr_t site; int32_t originalRel32; };
	std::vector<PatchedSite> mPatched;

	uint32_t mWindowMilliseconds = kDefaultWindowMilliseconds;

	// ------------------------------------------------------------------------------------------------------
	// Stub page layout. Data first so the code's rip-relative slots are a fixed, computable distance away.
	// ------------------------------------------------------------------------------------------------------
	static constexpr uint32_t kSlotGetTickCount = 0x00;
	static constexpr uint32_t kSlotTable = 0x08;
	static constexpr uint32_t kSlotRealFunction = 0x10;
	static constexpr uint32_t kCodeOffset = 0x20;

	// Offsets within the code block of the three rip-relative disp32 fields, and of the one imm32.
	// ------------------------------------------------------------------------------------------------------
	// WHY THIS IS A WRAPPER (call + ret) AND NO LONGER A TAIL JUMP
	// ------------------------------------------------------------------------------------------------------
	// The original stub tail-jumped into the real function, which meant it never saw the RESULT. That gave one
	// piece of information - "a script tested this volume recently" - and the viewer had to infer everything
	// else from a decay window. The visible consequence: after you actually entered a volume the script stopped
	// testing it, and the overlay kept showing it live for the whole 3000 ms window before it went dormant. It
	// could never say "you just hit this one", only "something tested this a moment ago".
	//
	// The result IS the answer: trigger_volume_test_point returns whether the point was inside. So the stub now
	// CALLS the real function, records the return value, and returns - giving two tables, lastTested and
	// lastHit, which is exactly the check-hit / check-miss split the MCC trigger overlay already exposes.
	//
	// ⚠ THE COST, STATED PLAINLY: the stub is now ON THE STACK while the real function runs, where before it
	// was not. A stub page has no .pdata, so an SEH unwind crossing it would walk an undescribed frame. That is
	// why buildStub registers a RUNTIME_FUNCTION for this code via RtlAddFunctionTable - see registerUnwind().
	// Do not remove that registration on the grounds that "it has never thrown".
	//
	// Frame (0x48 bytes, which also keeps rsp 16-byte aligned for the two calls we make - at entry rsp%16==8,
	// 0x48%16==8, so after the sub rsp%16==0 and the call pushes it back to 8, which is what a callee expects):
	//     [rsp+00h..1Fh]  shadow space for the functions WE call
	//     [rsp+20h]       rcx  - volumeIndex, and our own scratch copy of it
	//     [rsp+28h]       rdx
	//     [rsp+30h]       r8
	//     [rsp+38h]       r9
	//     [rsp+40h]       the real function's return value, saved across the GetTickCount call
	// rcx/rdx/r8/r9 are NOT restored before returning: they are volatile in the Win64 ABI, and unlike the old
	// tail jump nothing downstream of us reads them.
	// ------------------------------------------------------------------------------------------------------
	static constexpr uint32_t kDispRealCall = 0x1A;   // FF 15 <disp32>, next instruction at code+0x1E
	static constexpr uint32_t kNextRealCall = 0x1E;
	static constexpr uint32_t kDispTickCall = 0x24;   // FF 15 <disp32>, next instruction at code+0x28
	static constexpr uint32_t kNextTickCall = 0x28;
	static constexpr uint32_t kImmMax = 0x2E;   // 81 F9 <imm32>
	static constexpr uint32_t kDispTable = 0x37;   // 48 8B 15 <disp32>, next instruction at code+0x3B
	static constexpr uint32_t kNextTable = 0x3B;

	static constexpr uint8_t kStubCode[] =
	{
		0x48, 0x83, 0xEC, 0x48,                   // 0x00 sub  rsp, 48h
		0x48, 0x89, 0x4C, 0x24, 0x20,             // 0x04 mov  [rsp+20h], rcx
		0x48, 0x89, 0x54, 0x24, 0x28,             // 0x09 mov  [rsp+28h], rdx
		0x4C, 0x89, 0x44, 0x24, 0x30,             // 0x0E mov  [rsp+30h], r8
		0x4C, 0x89, 0x4C, 0x24, 0x38,             // 0x13 mov  [rsp+38h], r9
		// args are still live in rcx/rdx/r8/r9 - the stores above did not touch them
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,       // 0x18 call qword ptr [rip+disp]  -> real function
		0x89, 0x44, 0x24, 0x40,                   // 0x1E mov  [rsp+40h], eax   ; save the result
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,       // 0x22 call qword ptr [rip+disp]  -> GetTickCount
		0x8B, 0x4C, 0x24, 0x20,                   // 0x28 mov  ecx, [rsp+20h]  ; zero extended, so a negative
		                                          //                           ; index fails the unsigned cmp
		0x81, 0xF9, 0x00, 0x00, 0x00, 0x00,       // 0x2C cmp  ecx, kMaxTrackedVolumes
		0x73, 0x1B,                               // 0x32 jae  short +0x1B -> 0x4F
		0x48, 0x8B, 0x15, 0x00, 0x00, 0x00, 0x00, // 0x34 mov  rdx, qword ptr [rip+disp] -> table
		0x83, 0xC8, 0x01,                         // 0x3B or   eax, 1          ; 0 is the never-tested sentinel
		0x89, 0x04, 0x8A,                         // 0x3E mov  [rdx+rcx*4], eax                ; lastTested
		// Test only AL. The function returns a bool, so the upper 24 bits of eax are not guaranteed clean, and
		// comparing the full dword would report a hit on garbage.
		0x80, 0x7C, 0x24, 0x40, 0x00,             // 0x41 cmp  byte ptr [rsp+40h], 0
		0x74, 0x07,                               // 0x46 je   short +0x07 -> 0x4F
		0x89, 0x84, 0x8A, 0x00, 0x40, 0x00, 0x00, // 0x48 mov  [rdx+rcx*4+4000h], eax          ; lastHit
		                                          // 0x4F skip:
		0x8B, 0x44, 0x24, 0x40,                   // 0x4F mov  eax, [rsp+40h]  ; the real return value, exactly
		0x48, 0x83, 0xC4, 0x48,                   // 0x53 add  rsp, 48h
		0xC3,                                     // 0x57 ret
	};

	// Byte offset of the lastHit half within the one allocation. Must match the disp32 baked into the stub's
	// `mov [rdx+rcx*4+4000h], eax` above - a static_assert below enforces it rather than trusting the comment.
	static constexpr uint32_t kHitTableByteOffset = kMaxTrackedVolumes * sizeof(uint32_t);
	static_assert(kHitTableByteOffset == 0x4000,
		"kStubCode hard-codes disp32 0x4000 for the lastHit half; change both together or the stub corrupts memory");

	// ------------------------------------------------------------------------------------------------------
	// UNWIND INFO FOR THE STUB PAGE.
	//
	// The stub is now a real frame on the stack while the game's own code runs beneath it (see the kStubCode
	// comment). Dynamically generated code has no .pdata, so an SEH unwind that crossed this frame would find
	// no RUNTIME_FUNCTION and treat it as a leaf - restoring rsp wrongly by our 0x48 bytes and returning to
	// garbage. RtlAddFunctionTable publishes the description instead.
	//
	// The prologue this describes is exactly one instruction: `sub rsp, 48h` at offset 0, which is
	// UWOP_ALLOC_SMALL with an encoded size of (0x48 / 8) - 1 == 8.
	// ------------------------------------------------------------------------------------------------------
	static constexpr uint32_t kSlotUnwind = 0x18;   // UNWIND_INFO, in the data area ahead of the code
	static constexpr uint32_t kSlotRuntimeFunction = 0x100;

	// Laid out by hand rather than via a struct: UNWIND_INFO's trailing UNWIND_CODE array is a flexible member,
	// so a struct declaration would not describe the object we actually need.
	static constexpr uint8_t kUnwindInfo[] =
	{
		0x01,       // Version 1, no flags (no exception handler of our own)
		0x04,       // SizeOfProlog: the sub rsp,48h is 4 bytes
		0x01,       // CountOfUnwindCodes
		0x00,       // FrameRegister 0 / FrameOffset 0 - we use rsp directly, no frame pointer
		0x04, 0x82, // UNWIND_CODE: offset in prolog 0x04, op UWOP_ALLOC_SMALL (2) | info 8 -> (8+1)*8 = 0x48
	};

	// Registered for the process lifetime alongside the never-freed stub page, for the same reason: a game
	// thread can be inside the stub at any instant.
	void registerUnwind(uint8_t* page, uint32_t codeOffset, uint32_t codeSize)
	{
		memcpy(page + kSlotUnwind, kUnwindInfo, sizeof(kUnwindInfo));

		RUNTIME_FUNCTION* rf = (RUNTIME_FUNCTION*)(page + kSlotRuntimeFunction);
		rf->BeginAddress = codeOffset;
		rf->EndAddress = codeOffset + codeSize;
		rf->UnwindInfoAddress = kSlotUnwind;

		// Non-fatal on failure: the stub still works, it is only an unwind crossing it that would misbehave,
		// and that is strictly better than refusing to track triggers at all.
		if (!RtlAddFunctionTable(rf, 1, (DWORD64)page))
			PLOG_ERROR << "HCETriggerActivity: RtlAddFunctionTable failed; an SEH unwind through the trigger "
			"stub would be undefined. Trigger tracking still works.";
	}

	// VirtualAlloc within +/-1.75 GB of `target` so an E8 rel32 from any call site can reach the stub. Same
	// approach as FPScaleFix::allocNear. Returns nullptr if no nearby page could be taken.
	static void* allocNear(uintptr_t target, size_t size)
	{
		SYSTEM_INFO si{}; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
		const uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL;
		const uintptr_t hi = target + 0x70000000ULL;
		const uintptr_t startp = target & ~(gran - 1);
		for (uintptr_t p = startp; p > lo; p -= gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
		for (uintptr_t p = startp + gran; p < hi; p += gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
		return nullptr;
	}

	// Throws HCMRuntimeException on any failure. Called with mStateMutex held, and never while threads are
	// suspended (it allocates).
	void buildStub(uintptr_t realFunction)
	{
		if (mStubPage) return;

		// ONE allocation, two halves: lastTested at [0], lastHit at [kHitTableByteOffset]. One base pointer means
		// the stub loads it once and reaches both halves with a displacement, which is why the hit store is a
		// plain disp32 rather than a second rip-relative load.
		uint32_t* table = (uint32_t*)VirtualAlloc(nullptr, 2ull * kMaxTrackedVolumes * sizeof(uint32_t),
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!table) throw HCMRuntimeException("HCETriggerActivity: could not allocate the timestamp table");
		// VirtualAlloc already zeroes; zero == "never tested", which is why the stub ORs 1 into what it stores.

		HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
		FARPROC pGetTickCount = kernel32 ? GetProcAddress(kernel32, "GetTickCount") : nullptr;
		if (!pGetTickCount)
		{
			VirtualFree(table, 0, MEM_RELEASE);
			throw HCMRuntimeException("HCETriggerActivity: could not resolve kernel32!GetTickCount");
		}

		uint8_t* page = (uint8_t*)allocNear(realFunction, 0x1000);
		if (!page)
		{
			VirtualFree(table, 0, MEM_RELEASE);
			throw HCMRuntimeException("HCETriggerActivity: could not allocate a code page near the sim dll");
		}

		memset(page, 0xCC, 0x1000);
		*(uintptr_t*)(page + kSlotGetTickCount) = (uintptr_t)pGetTickCount;
		*(uintptr_t*)(page + kSlotTable) = (uintptr_t)table;
		*(uintptr_t*)(page + kSlotRealFunction) = realFunction;

		uint8_t* code = page + kCodeOffset;
		memcpy(code, kStubCode, sizeof(kStubCode));

		const auto putDisp = [&](uint32_t fieldOff, uint32_t nextOff, uint32_t slotOff)
			{
				const int64_t d = (int64_t)(page + slotOff) - (int64_t)(code + nextOff);
				const int32_t d32 = (int32_t)d;
				memcpy(code + fieldOff, &d32, sizeof(d32));
			};
		putDisp(kDispRealCall, kNextRealCall, kSlotRealFunction);
		putDisp(kDispTickCall, kNextTickCall, kSlotGetTickCount);
		putDisp(kDispTable, kNextTable, kSlotTable);

		const uint32_t maxVolumes = kMaxTrackedVolumes;
		memcpy(code + kImmMax, &maxVolumes, sizeof(maxVolumes));

		// Before the page is made executable, so nothing can be running in it un-described.
		registerUnwind(page, kCodeOffset, (uint32_t)sizeof(kStubCode));

		DWORD oldProtect = 0;
		if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &oldProtect))
		{
			VirtualFree(page, 0, MEM_RELEASE);
			VirtualFree(table, 0, MEM_RELEASE);
			throw HCMRuntimeException("HCETriggerActivity: could not make the stub page executable");
		}
		FlushInstructionCache(GetCurrentProcess(), page, 0x1000);

		// Publish the table only once the stub that writes it is fully built.
		gLastTestedTable.store(table, std::memory_order_release);
		mStubPage = page;
		mStubEntry = (uintptr_t)code;

		PLOG_INFO << "HCETriggerActivity: stub at " << std::format("0x{:X}", mStubEntry)
			<< ", table at " << std::format("0x{:X}", (uintptr_t)table)
			<< ", real function at " << std::format("0x{:X}", realFunction);
	}

	// Resolves and FULLY validates the function plus all eight call sites before returning. Throws on any
	// mismatch, having changed nothing.
	void resolveAndVerify(uintptr_t& functionOut, std::vector<uintptr_t>& sitesOut) const
	{
		if (!mFunction) throw HCMRuntimeException("HCETriggerActivity: no pointer data for trigger_volume_test_point");
		if (!mExpectedBytes || mExpectedBytes->empty()) throw HCMRuntimeException("HCETriggerActivity: no expected original bytes for trigger_volume_test_point");
		if (mCallSites.size() != 8) throw HCMRuntimeException(std::format("HCETriggerActivity: expected 8 HaloScript call sites, have {}", mCallSites.size()));

		uintptr_t function = 0;
		if (!mFunction->resolve(&function))
			throw HCMRuntimeException(std::format("HCETriggerActivity: could not resolve trigger_volume_test_point: {}", MultilevelPointer::GetLastError()));

		// Cross-check the address BEFORE comparing the fingerprint. If the function has merely relocated, the
		// signature finds it and this throws with both numbers - which is a far better diagnostic than the
		// fingerprint's "did not match its expected original bytes", whose failure text cannot distinguish
		// "moved" from "changed". Throws only on disagreement; see HCEAnchors.h.
		HCEAnchors::crossCheck(HCEAnchors::Anchor::TriggerVolumeTestPoint, function, "Trigger Volume live activity");

		const std::vector<byte>& expected = *mExpectedBytes;
		std::vector<byte> actual(expected.size());
		if (!mFunction->readArrayData(actual.data(), actual.size()))
			throw HCMRuntimeException(std::format("HCETriggerActivity: could not read trigger_volume_test_point: {}", MultilevelPointer::GetLastError()));

		if (actual != expected)
			throw HCMRuntimeException(std::format(
				"HCETriggerActivity: trigger_volume_test_point did not match its expected original bytes - this build of Halo Campaign Evolved is not supported. Expected [{}], found [{}]",
				bytesToString(expected), bytesToString(actual)));

		std::vector<uintptr_t> sites;
		sites.reserve(mCallSites.size());
		for (size_t i = 0; i < mCallSites.size(); i++)
		{
			uintptr_t site = 0;
			if (!mCallSites[i] || !mCallSites[i]->resolve(&site))
				throw HCMRuntimeException(std::format("HCETriggerActivity: could not resolve HaloScript call site {}: {}", i + 1, MultilevelPointer::GetLastError()));

			if (IsBadReadPtr((void*)site, 5))
				throw HCMRuntimeException(std::format("HCETriggerActivity: HaloScript call site {} at 0x{:X} is unreadable", i + 1, site));

			const uint8_t opcode = *(const uint8_t*)site;
			if (opcode != 0xE8)
				throw HCMRuntimeException(std::format("HCETriggerActivity: HaloScript call site {} at 0x{:X} is not a direct call (found opcode 0x{:02X})", i + 1, site, opcode));

			int32_t rel32 = 0;
			memcpy(&rel32, (const void*)(site + 1), sizeof(rel32));
			const uintptr_t callTarget = site + 5 + (intptr_t)rel32;
			if (callTarget != function)
				throw HCMRuntimeException(std::format("HCETriggerActivity: HaloScript call site {} at 0x{:X} calls 0x{:X}, not trigger_volume_test_point at 0x{:X}", i + 1, site, callTarget, function));

			sites.push_back(site);
		}

		functionOut = function;
		sitesOut = std::move(sites);
	}

	// Plain memory writes only - no allocation, no logging, no locking. Safe to run with other threads
	// suspended. Returns false if the 4-byte store could not be unprotected.
	static bool writeRel32(uintptr_t site, int32_t rel32)
	{
		DWORD oldProtect = 0;
		if (!VirtualProtect((void*)(site + 1), sizeof(int32_t), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
		*(volatile int32_t*)(site + 1) = rel32;   // does not straddle a cache line, so this store is atomic
		VirtualProtect((void*)(site + 1), sizeof(int32_t), oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
		return true;
	}

	void enableInternal()
	{
		if (mEnabled) return;

		uintptr_t function = 0;
		std::vector<uintptr_t> sites;
		resolveAndVerify(function, sites);   // throws before anything is touched

		buildStub(function);                 // allocates; must happen before the suspender exists

		// Every displacement must actually fit. allocNear guarantees this, but a caller of setEnabled deserves
		// a clean refusal rather than a silently truncated jump.
		std::vector<PatchedSite> planned;
		planned.reserve(sites.size());
		for (uintptr_t site : sites)
		{
			const int64_t newRel = (int64_t)mStubEntry - (int64_t)(site + 5);
			if (newRel < -0x80000000LL || newRel > 0x7FFFFFFFLL)
				throw HCMRuntimeException(std::format("HCETriggerActivity: stub at 0x{:X} is out of rel32 range of call site 0x{:X}", mStubEntry, site));

			int32_t original = 0;
			memcpy(&original, (const void*)(site + 1), sizeof(original));
			planned.push_back({ site, original });
		}

		// Everything below the suspender is plain memory writes. ScopedThreadSuspender forbids any heap or
		// virtual allocation while it is alive (a suspended thread may hold the loader/heap lock), which is why
		// `planned` is fully built above and mPatched is only assigned after the scope closes.
		size_t applied = 0;
		{
			ScopedThreadSuspender suspend;
			for (; applied < planned.size(); applied++)
			{
				const int32_t newRel = (int32_t)((int64_t)mStubEntry - (int64_t)(planned[applied].site + 5));
				if (!writeRel32(planned[applied].site, newRel)) break;
			}
			if (applied != planned.size())
				for (size_t i = 0; i < applied; i++)
					writeRel32(planned[i].site, planned[i].originalRel32);   // all-or-nothing
		}

		if (applied != planned.size())
			throw HCMRuntimeException("HCETriggerActivity: failed to redirect the HaloScript call sites (VirtualProtect refused); nothing was left patched");

		mPatched = std::move(planned);
		mEnabled = true;
		PLOG_INFO << "HCETriggerActivity: redirected " << mPatched.size() << " HaloScript trigger-volume test call sites";
	}

	void disableInternal()
	{
		if (!mEnabled && mPatched.empty()) return;

		{
			ScopedThreadSuspender suspend;
			for (const PatchedSite& p : mPatched) writeRel32(p.site, p.originalRel32);
		}
		mPatched.clear();
		mEnabled = false;

		// The stub page and the table are intentionally NOT freed - a game thread may still be inside the stub
		// at this instant, and after the restore above nothing new can enter it. See the header comment.
		PLOG_INFO << "HCETriggerActivity: restored the HaloScript trigger-volume test call sites";
	}

public:
	HCETriggerActivityImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		// explicit cast - GameState has operator==(GameState) AND operator Value(), so a direct compare is C2666
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCETriggerActivity only supports Halo Campaign Evolved");

		// Everything below is an XML lookup. No game memory is touched and no address is resolved here: the sim
		// dll is not guaranteed to be loaded when cheats are constructed.
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		mFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointFunction), mGame);
		mExpectedBytes = ptr->getVectorData<byte>(nameof(hceTriggerVolumeTestPointOriginalBytes), mGame);

		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite1), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite2), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite3), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite4), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite5), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite6), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite7), mGame));
		mCallSites.push_back(ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTriggerVolumeTestPointScriptCallSite8), mGame));

		// Optional - the 3000 ms default stands if the entry is ever removed.
		try
		{
			auto window = ptr->getData<std::shared_ptr<int64_t>>(nameof(hceTriggerActivityWindowMilliseconds), mGame);
			if (window && *window > 0) mWindowMilliseconds = (uint32_t)*window;
		}
		catch (HCMInitException&) {}
	}

	~HCETriggerActivityImpl()
	{
		try
		{
			std::scoped_lock lock(mStateMutex);
			disableInternal();
		}
		catch (...) {}
	}

	void setEnabled(bool enabled)
	{
		try
		{
			std::scoped_lock lock(mStateMutex);
			if (enabled) enableInternal(); else disableInternal();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
		catch (...)
		{
			HCMRuntimeException converted("HCETriggerActivity: unknown failure while toggling the call-site patches");
			runtimeExceptions->handleMessage(converted);
		}
	}

	bool isEnabled() const
	{
		std::scoped_lock lock(mStateMutex);
		return mEnabled;
	}

	bool isVolumeActive(uint32_t volumeIndex) const
	{
		if (volumeIndex >= kMaxTrackedVolumes) return false;

		uint32_t* table = gLastTestedTable.load(std::memory_order_acquire);
		if (!table) return false;

		// volatile, not atomic: the stub writes this slot with a plain 32-bit store from the game thread and
		// there is nothing to synchronise with - a naturally aligned 32-bit load can never tear.
		const uint32_t last = *(volatile uint32_t*)&table[volumeIndex];
		if (last == 0) return false;   // never tested; the stub never stores 0

		// ⚠ CHECKED BEFORE ANY "recently tested" SHORTCUT BELOW. A VOLUME WHOSE MOST RECENT TEST PASSED IS NOT
		// "ACTIVE" - this is what made a trigger you had just crossed keep showing as live for seconds after.
		//
		// "Active" means a script is WAITING on this volume. The instant the test returns true, the
		// sleep_until it was feeding is satisfied and the script moves on - the volume is done, not waiting.
		// But the tested-timestamp was just refreshed by that very test, so the decay window restarted and the
		// overlay kept it green for the full window. The hit flash would appear, expire, and hand back to a
		// stale "active" colour, which read as the change taking three to five seconds.
		//
		// The stub writes both tables from the SAME GetTickCount value on a passing test, so equality here is
		// exact and means precisely "the last thing that happened to this volume was a pass".
		const uint32_t lastHit = *(volatile uint32_t*)&table[kMaxTrackedVolumes + volumeIndex];
		if (lastHit == last) return false;

		// Unsigned subtraction, so a GetTickCount wrap (every 49.7 days) is handled for free.
		const uint32_t elapsed = GetTickCount() - last;

		// The stub ORs 1 into the timestamp, so on an even tick the stored value can be exactly 1 ms in the
		// "future" and elapsed underflows to a huge number. That only ever means "tested right now".
		if (elapsed > 0x80000000u) return true;

		return elapsed < mWindowMilliseconds;
	}

	// "The script tested this volume and the answer was YES" - within a short flash window.
	//
	// This is deliberately MUCH shorter than the active window. The active window is long because a script that
	// is genuinely watching a volume polls it in bursts (sleep_until re-evaluates every N ticks), so a short
	// window makes live triggers flicker. A HIT is not a poll, it is an event: it wants to appear the instant
	// it happens and then get out of the way, so a hit that was still being reported three seconds later would
	// be the very lag this feature exists to remove.
	bool isVolumeHit(uint32_t volumeIndex, uint32_t flashMilliseconds) const
	{
		if (volumeIndex >= kMaxTrackedVolumes) return false;

		uint32_t* table = gLastTestedTable.load(std::memory_order_acquire);
		if (!table) return false;

		const uint32_t last = *(volatile uint32_t*)&table[kMaxTrackedVolumes + volumeIndex];
		if (last == 0) return false;

		const uint32_t elapsed = GetTickCount() - last;
		if (elapsed > 0x80000000u) return true;   // see isVolumeActive: the OR-1 can look 1 ms into the future
		return elapsed < flashMilliseconds;
	}

	// Raw timestamp, 0 == never hit. This is what a "volumes I have hit" list is built from: the caller keeps
	// its own copy per volume and reports the ones that CHANGED, which makes the report edge-triggered without
	// this class having to own any per-scenario state (it has no idea when a level changed).
	uint32_t getLastHitTick(uint32_t volumeIndex) const
	{
		if (volumeIndex >= kMaxTrackedVolumes) return 0;
		uint32_t* table = gLastTestedTable.load(std::memory_order_acquire);
		if (!table) return 0;
		return *(volatile uint32_t*)&table[kMaxTrackedVolumes + volumeIndex];
	}
};

HCETriggerActivity::HCETriggerActivity(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCETriggerActivityImpl>(game, dicon))
{
}

HCETriggerActivity::~HCETriggerActivity()
{
	PLOG_VERBOSE << "~" << getName();
}

bool HCETriggerActivity::isVolumeActive(uint32_t volumeIndex) const
{
	return pimpl ? pimpl->isVolumeActive(volumeIndex) : false;
}

bool HCETriggerActivity::isVolumeHit(uint32_t volumeIndex, uint32_t flashMilliseconds) const
{
	return pimpl ? pimpl->isVolumeHit(volumeIndex, flashMilliseconds) : false;
}

uint32_t HCETriggerActivity::getLastHitTick(uint32_t volumeIndex) const
{
	return pimpl ? pimpl->getLastHitTick(volumeIndex) : 0;
}

void HCETriggerActivity::setEnabled(bool enabled)
{
	if (pimpl) pimpl->setEnabled(enabled);
}

bool HCETriggerActivity::isEnabled() const
{
	return pimpl ? pimpl->isEnabled() : false;
}
