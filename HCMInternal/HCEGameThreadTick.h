#pragma once
#include "pch.h"

// ================================================================================================================
// A PER-FRAME CALLBACK THAT RUNS ON HALO CAMPAIGN EVOLVED'S GAME THREAD.
//
// WHY THIS EXISTS
// ---------------
// Some engine calls may only be made on the game thread - anything touching UWorld, the World Partition subsystem
// or data layers. HCM's own threads cannot make them. Until now the only game-thread code HCM ran on this title
// was inside individual midhooks, so a cheat could only act at the moments its own hook happened to fire.
//
// That is a real limitation, not a stylistic one. HCESkyFix needs to call the engine's RemoveOccupant to hand an
// area back properly, but its only game-thread site is RemoveOccupant itself - which by definition is not being
// called while the player is stood outside every area, which is exactly when the fix needs to act.
//
// APlayerCameraManager::DoUpdateCamera runs every frame on the game thread, and HCEGetCameraData already midhooks
// it. This turns that hook into a shared tick so any cheat can borrow it.
//
// ⚠ WHAT RUNS HERE IS ON THE GAME'S CRITICAL PATH. Every registered callback runs inside the camera midhook,
// once per frame, before the game gets its frame back. Callbacks must be cheap, must never block, and must never
// throw - a throwing midhook is a crash. They should do nothing at all in the common case and only act when a
// flag says there is work, which is why registration is a plain function pointer rather than a std::function: no
// allocation, no captured state, nothing to destroy underneath a running callback.
//
// ⚠ THE TICK IS NOT GUARANTEED TO RUN. The camera hook is attached on demand (see HCEGetCameraData::setHookWanted),
// and may be unavailable on an unsupported build. Anything queued here must have a fallback for "the tick never
// came" - never make correctness depend on it.
// ================================================================================================================
namespace HCEGameThreadTick
{
	using TickFn = void(*)() noexcept;

	// Small and fixed: this is a handful of cheats, not an open registry, and a fixed array keeps run() free of
	// locks and allocation on the game thread.
	constexpr size_t kMaxTicks = 8;
	inline std::atomic<TickFn> gTicks[kMaxTicks]{};

	// Registers a callback. Idempotent. False means the table is full and the caller must fall back.
	inline bool add(TickFn fn) noexcept
	{
		if (!fn) return false;

		for (auto& slot : gTicks)
			if (slot.load(std::memory_order_acquire) == fn) return true;

		for (auto& slot : gTicks)
		{
			TickFn empty = nullptr;
			if (slot.compare_exchange_strong(empty, fn, std::memory_order_acq_rel)) return true;
		}
		return false;
	}

	// Unregisters. MUST be called before the owning cheat is destroyed, or the tick calls into a dead object.
	inline void remove(TickFn fn) noexcept
	{
		for (auto& slot : gTicks)
		{
			TickFn held = fn;
			if (slot.compare_exchange_strong(held, nullptr, std::memory_order_acq_rel)) return;
		}
	}

	// Called from the camera midhook. Game thread, every frame.
	inline void run() noexcept
	{
		for (auto& slot : gTicks)
		{
			const TickFn fn = slot.load(std::memory_order_acquire);
			if (fn) fn();
		}
	}
}
