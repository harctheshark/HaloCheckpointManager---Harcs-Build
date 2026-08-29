#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// A PER-FRAME CALLBACK THAT RUNS ON HALO CAMPAIGN EVOLVED'S *SIMULATION* GAME THREAD.
//
// WHY THIS EXISTS WHEN HCEGameThreadTick ALREADY DOES SOMETHING SIMILAR
// ---------------------------------------------------------------------
// HCEGameThreadTick rides APlayerCameraManager::DoUpdateCamera, which is UE5's game thread, inside
// HaloCampaignEvolved.exe. That is a DIFFERENT THREAD from the one the simulation runs on, and the difference
// is not academic: the sim's TLS block belongs to HaloSimulation_tag_release.dll and only the sim thread has
// the real one. On any other thread `NtCurrentTeb()->ThreadLocalStoragePointer[tls_index]` yields a
// plausible-looking pointer that is simply wrong - reads succeed, writes land somewhere harmless, and nothing
// errors. That silently broke the first version of Game Speed, which reported success on screen while writing
// a float into another thread's slot.
//
// Anything that must run ON the simulation thread needs this pump instead:
//   * executing HaloScript - every hs entry point dereferences the sim TLS with no null check, so the wrong
//     thread is a fault rather than a wrong answer;
//   * touching the observer / director / player-control globals with frame accuracy.
//
// THE SITE: sim RVA 0x1AF2BD, inside sub_1801AF290
// -----------------------------------------------
//   1801AF2AA  8B 0D 80 34 BC 00           mov ecx,[rip+..]      ; tls_index
//   1801AF2B0  65 48 8B 04 25 58 00 00 00  mov rax, gs:[0x58]
//   1801AF2B9  48 8B 3C C8                 mov rdi,[rax+rcx*8]   ; RDI = SIM TLS BASE
//   1801AF2BD  41 BF E8 00 00 00           mov r15d,0xE8         <-- HOOK HERE (6 bytes, one instruction)
//   1801AF2C3  4A 8B 04 3F                 mov rax,[rdi+r15]
//
// Chosen over main_game_time_update for one decisive reason: it hands us the sim TLS base live in RDI, which
// removes the TEB/thread walk and its staleness hazard entirely. It also sits after the simulation update for
// the frame, and it runs at the main menu and while the game is PAUSED - both verified.
//
// ⚠ FOUND BY SIGNATURE, NOT BY FIXED ADDRESS. HaloCER ships no version resource (every build reports 0.0.0.0),
// so a hard-coded RVA is a patch landing mid-instruction after the next update. The 16-byte pattern below has
// ZERO wildcards and exactly one match in the module. ⚠ Do NOT trim it toward the TLS-load idiom in front of
// it: that idiom alone matches 22 places, and uniqueness only arrives at the tail
// (`mov r15d,0xE8` / `mov rax,[rdi+r15]` / `test dword[rax],0x3FFF`).
//
// ⚠ CONTRACT FOR CALLBACKS. This runs on the game's critical path, once per simulation frame:
//   * noexcept, always. sub_1801AF290 carries an unwind-only handler and the trampoline has no .pdata entry,
//     so an exception escaping a callback is an unwalkable stack, not a catchable error.
//   * cheap, and non-blocking. Do nothing in the common case; act only when a flag says there is work.
//   * never touch ctx.rdi, ctx.rsp or ctx.rip. RDI is live and is consumed nine more times in this function;
//     RSP is the frame pointer and the spilled callee-saved registers live below it.
//   * registration is a plain function pointer - no allocation, no captured state, nothing to destroy under a
//     running callback.
// ================================================================================================================

namespace HCEGameThreadPump
{
	// Receives the SIM TLS BASE, live, straight out of RDI - no walk required.
	using PumpFn = void(*)(uintptr_t simTlsBase) noexcept;

	constexpr size_t kMaxPumps = 8;
	inline std::atomic<PumpFn> gPumps[kMaxPumps]{};

	inline bool add(PumpFn fn) noexcept
	{
		if (!fn) return false;
		for (auto& slot : gPumps)
			if (slot.load(std::memory_order_acquire) == fn) return true;
		for (auto& slot : gPumps)
		{
			PumpFn empty = nullptr;
			if (slot.compare_exchange_strong(empty, fn, std::memory_order_acq_rel)) return true;
		}
		return false;
	}

	// MUST be called before the owning object dies, or the midhook calls into freed memory.
	inline void remove(PumpFn fn) noexcept
	{
		for (auto& slot : gPumps)
		{
			PumpFn held = fn;
			if (slot.compare_exchange_strong(held, nullptr, std::memory_order_acq_rel)) return;
		}
	}

	inline void run(uintptr_t simTlsBase) noexcept
	{
		for (auto& slot : gPumps)
		{
			const PumpFn fn = slot.load(std::memory_order_acquire);
			if (fn) fn(simTlsBase);
		}
	}
}

// Owns the midhook. Resolve it as a dependent cheat to keep the pump alive for as long as you need it.
class HCEGameThreadPumpHost : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HCEGameThreadPumpHost(GameState game, IDIContainer& dicon);
	~HCEGameThreadPumpHost();
	virtual std::string_view getName() override { return nameof(HCEGameThreadPumpHost); }

	// True once the signature resolved and the hook is attached.
	bool isRunning() const;
};
