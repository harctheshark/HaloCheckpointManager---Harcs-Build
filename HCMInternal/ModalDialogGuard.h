#pragma once
#include <atomic>
#include <cstdint>
#include <Windows.h>

// Global "one modal file dialog at a time" guard, shared by every feature that spawns a Win32 file dialog off a
// button event (PresetManager, H2ArmorColour, ...).
//
// Why it must be global/static: the DI builds one cheat instance PER SUPPORTED GAME, and every instance subscribes
// to the same button ActionEvent. One click therefore fires all N handlers synchronously, back-to-back. A
// per-instance guard lets each of the N instances open its own dialog (the "cancel it 6 times" bug). A single
// shared guard means only the first handler opens a dialog and the rest no-op. The short post-close cool-down also
// swallows the click that closes a dialog (it can leak back into ImGui as another button press) and the
// immediately-following sibling-instance handlers.
//
// Usage:
//   auto claim = ModalDialogGuard::tryClaim();
//   if (!claim) return;             // a dialog is already open / just closed -> ignore this stray/sibling fire
//   ... open your modal dialog ...   // guard auto-releases + stamps the close time when 'claim' leaves scope
class ModalDialogGuard
{
private:
	static inline std::atomic_bool s_open{ false };
	static inline std::atomic_uint64_t s_lastCloseTick{ 0 };
	static constexpr uint64_t kCooldownMs = 500;

public:
	// Move-only RAII token. Truthy only if it actually claimed the dialog slot; on destruction the owner stamps the
	// close time then clears the open flag (in that order, so a leaked close-click still sees a fresh cool-down).
	class Claim
	{
		bool mOwns;
	public:
		explicit Claim(bool owns) : mOwns(owns) {}
		Claim(const Claim&) = delete;
		Claim& operator=(const Claim&) = delete;
		Claim(Claim&& o) noexcept : mOwns(o.mOwns) { o.mOwns = false; }
		Claim& operator=(Claim&&) = delete;
		explicit operator bool() const { return mOwns; }
		~Claim()
		{
			if (mOwns)
			{
				s_lastCloseTick.store(GetTickCount64(), std::memory_order_release);
				s_open.store(false, std::memory_order_release);
			}
		}
	};

	static Claim tryClaim()
	{
		if (s_open.load(std::memory_order_acquire)) return Claim(false);
		if (GetTickCount64() - s_lastCloseTick.load(std::memory_order_acquire) < kCooldownMs) return Claim(false);
		return Claim(!s_open.exchange(true, std::memory_order_acq_rel)); // exchange serialises concurrent claimers
	}
};
