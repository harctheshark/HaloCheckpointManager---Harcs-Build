#include "pch.h"
#include "BlockGameInput.h"
#include "ModuleHook.h"
#include "MidhookFlagInterpreter.h"
#include "MultilevelPointer.h"
#include "ImGuiManager.h"   // HCE keeps the WndProc swallow as a SECOND layer, on top of the exe hook
#include <atomic>
#include <mutex>
#include <vector>
#include <string>




class BlockGameInputImpl : public TokenSharedRequestProvider
{
private:


	std::shared_ptr<ModulePatch> blockGameKeyboardInputHook;
	std::shared_ptr<ModulePatch> blockGameMouseMoveInputPatch;
	std::shared_ptr<ModulePatch> blockGameMouseClickInputPatch;
	std::shared_ptr<ModulePatch> blockGameControllerInputPatch;


public:
	BlockGameInputImpl(std::shared_ptr<PointerDataStore> ptr)
	{

		auto blockGameKeyboardInputFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(blockGameKeyboardInputFunction));
		auto blockGameKeyboardInputCode = ptr->getVectorData<byte>(nameof(blockGameKeyboardInputCode));
		blockGameKeyboardInputHook = ModulePatch::make(L"main", blockGameKeyboardInputFunction, *blockGameKeyboardInputCode.get());

		auto blockGameMouseMoveInputFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(blockGameMouseMoveInputFunction));
		auto blockGameMouseMoveInputCode = ptr->getVectorData<byte>(nameof(blockGameMouseMoveInputCode));
		blockGameMouseMoveInputPatch = ModulePatch::make(L"main", blockGameMouseMoveInputFunction, *blockGameMouseMoveInputCode.get());

		auto blockGameMouseClickInputFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(blockGameMouseClickInputFunction));
		auto blockGameMouseClickInputCode = ptr->getVectorData<byte>(nameof(blockGameMouseClickInputCode));
		blockGameMouseClickInputPatch = ModulePatch::make(L"main", blockGameMouseClickInputFunction, *blockGameMouseClickInputCode.get());

		auto blockGameControllerInputFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(blockGameControllerInputFunction));
		auto blockGameControllerInputCode = ptr->getVectorData<byte>(nameof(blockGameControllerInputCode));
		blockGameControllerInputPatch = ModulePatch::make(L"main", blockGameControllerInputFunction, *blockGameControllerInputCode.get());
	}

	~BlockGameInputImpl()
	{
		PLOG_DEBUG << "~" << nameof(BlockGameInputImpl);
	}

	virtual void updateService() override
	{
		bool requested = serviceIsRequested();
		PLOG_INFO << "BlockGameInput service is turning " << (requested ? "ON!" : "OFF!");
		blockGameKeyboardInputHook->setWantsToBeAttached(requested);
		blockGameMouseMoveInputPatch->setWantsToBeAttached(requested);
		blockGameMouseClickInputPatch->setWantsToBeAttached(requested);
		blockGameControllerInputPatch->setWantsToBeAttached(requested);
		PLOG_DEBUG << "BlockGameInput hook state has been updated.";
	}


};




// ================================================================================================================
// Halo Campaign Evolved (HaloCER) implementation.
//
// WHY THE OBVIOUS APPROACH DOES NOT WORK
// HCM subclasses the SWAPCHAIN window (D3D12Hook -> swapDesc.OutputWindow -> ImGuiManager::mNewWndProc). HCE does
// not take gameplay input from that window at all. sub_1439FD050 (exe RVA 0x39FD050) creates a second, MESSAGE
// ONLY window - class L"UnrealWindowsRawInputHandler", title L"UnrealWindowsRawInputWindow", parent HWND_MESSAGE -
// and is the process's ONLY RegisterRawInputDevices caller. It registers exactly two devices, both with
// dwFlags = RIDEV_INPUTSINK (0x100) and hwndTarget = that message window:
//     usagePage 1 / usage 2  = mouse    (look)
//     usagePage 1 / usage 6  = keyboard (movement)
// So every gameplay input arrives as WM_INPUT at a window our subclass never sees. That is exactly why
// ImGuiManager::setSwallowGameInput alone was tested in game and did NOT stop look or movement.
//
// WHAT WE DO INSTEAD
// Inline-hook that window's WndProc (sub_1439FCDD0, exe RVA 0x39FCDD0, pointer data
// blockGameInputHCEWndProcFunction) and, while the service is requested, answer WM_INPUT ourselves with
// DefWindowProcW instead of letting the original body run. One message id covers both mouse and keyboard.
//
// WHY AN INLINE HOOK AT THE ENTRY AND NOT A MIDHOOK
// The first instruction is 4C 89 4C 24 20 - exactly 5 bytes and fully position independent, so safetyhook
// relocates it into its trampoline with no fixups. Our detour has the real WNDPROC signature, so we make no
// assumptions about register liveness or stack depth: suppression is a plain return and pass-through is
// getInlineHook().call(). A midhook at the entry could NOT be used to fake a return - the prologue
// (push rbp/rsi/rdi/r14 ; sub rsp,78h) has not run yet, so redirecting ctx.rip to the epilogue would execute
// add rsp,78h ; pop ... ; ret against an unprologued stack and corrupt the return address immediately.
//
// WHY DefWindowProcW AND NOT A BARE 0
// The original function already ends with return DefWindowProcW(...) on EVERY path including WM_INPUT.
// DefWindowProc routes WM_INPUT to DefRawInputProc, which is the documented cleanup for the message; skipping it
// on a RIDEV_INPUTSINK device risks leaking the RAWINPUT handle. The numeric result is 0 either way.
//
// WHAT WE MUST NOT TOUCH
//   - uMsg 0x81 (WM_NCCREATE). It is what stores the handler object into GWLP_USERDATA (-21), which the WM_INPUT
//     path later dereferences unconditionally, and DefWindowProcW must return TRUE there or CreateWindowExW
//     fails outright. The filter is a strict uMsg == WM_INPUT equality test, never a range.
//   - The game's own predicate globals (byte_14D48B231 / byte_14D48B269 / byte_14D48B230 / qword_14D4AB2B0).
//     Those are persistent game state; this hook is completely stateless.
//
// KNOWN GAP: only mouse + keyboard are registered here. A GAMEPAD does not come through this proc, so a
// controller can still move the player while the GUI is open. MCC's blockGameControllerInputPatch has no HCE
// counterpart yet.
//
// SAFETY: HCE ships no version resource, so every build reports version 0.0.0.0 and the pointer data's Version
// attribute cannot detect a game update (same problem as hceCheckpointDetours). The site is therefore verified
// against blockGameInputHCEWndProcOriginalBytes before anything is patched - hooking the middle of an
// instruction in a WndProc is an instant crash. Do not remove that check.
//
// The hook is installed LAZILY, the first time the user actually requests the service, and then left in place;
// toggling afterwards is a single atomic store. A user who never opens the GUI with the toggle on never has a
// byte of game code modified. Resolution and verification deliberately do NOT happen in the constructor, which
// runs very early in injection (ControlServiceContainer) - the same reasoning as HCECheckpointDetours.
// ================================================================================================================

namespace
{
	// Written by the UI thread, read by the game's raw-input message thread on every single input packet.
	std::atomic<bool> gHCEBlockRawInput{ false };

	// Published only once the hook is actually installed; null means "do nothing special".
	std::atomic<ModuleInlineHook*> gHCEWndProcHook{ nullptr };

	// How many threads are currently inside hceRawInputWndProcDetour. The destructor drains this before it lets
	// the ModuleInlineHook object be freed, so the detour can never dereference a dangling hook pointer.
	std::atomic<int> gHCEDetourInFlight{ 0 };

	// RAII so the counter is decremented even if the game's own WndProc body unwinds through us.
	struct HCEDetourInFlightGuard
	{
		HCEDetourInFlightGuard() { gHCEDetourInFlight.fetch_add(1, std::memory_order_acquire); }
		~HCEDetourInFlightGuard() { gHCEDetourInFlight.fetch_sub(1, std::memory_order_release); }
		HCEDetourInFlightGuard(const HCEDetourInFlightGuard&) = delete;
		HCEDetourInFlightGuard& operator=(const HCEDetourInFlightGuard&) = delete;
	};

	// THE HOT PATH. This runs on the game's raw-input message thread for every mouse packet (up to ~1000/s on a
	// high polling rate mouse). While IDLE it is exactly: one counter bump, one atomic load, one compare, one
	// tail call - i.e. free. The single GetRawInputData below is paid ONLY while blocking is actually on, which
	// is only while the HCM window is open. Absolutely NO logging (plog takes a mutex and does file IO), no
	// ImGui, no allocation, no MultilevelPointer::resolve, no locks of ours. Nothing we do here can throw; the
	// in-flight guard is RAII purely so that an exception unwinding out of the GAME's own WndProc body still
	// balances the counter.
	// x64 has a single calling convention, so the plain declaration matches WNDPROC exactly.
	LRESULT hceRawInputWndProcDetour(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		HCEDetourInFlightGuard guard;

		ModuleInlineHook* const hook = gHCEWndProcHook.load(std::memory_order_acquire);

		// No trampoline to call (we are being torn down) - behave like the original's default path.
		if (hook == nullptr)
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);

		// Suppressed: answer exactly as the original does on its "raw input disabled" path, without the buffer
		// alloc, without the critical-section fan-out, and without touching any game state.
		//
		// The ONE exception is a keyboard RELEASE (RI_KEY_BREAK), which is still let through. This handler is a
		// QUEUE - it memcpy's each RAWINPUT into per-consumer ring buffers for something else to parse later -
		// not a key-state table, so a dropped key-up is simply never seen at all. Without this, a key the player
		// was holding when the GUI opened would stay down forever after the GUI closes (hold W, open HCM,
		// release W -> chief keeps running). A key-up cannot move the player, so letting it through does not
		// weaken the block in the slightest. Deliberately keyboard only: a RAWMOUSE record carries lLastX/lLastY
		// in the SAME record as the button transition, so passing mouse button-ups through would leak look input.
		// GetRawInputData(RID_INPUT) does not consume the message - the original is free to read it again on the
		// paths where we pass it through - and on any failure we fall back to suppressing, which is the safe side.
		if (uMsg == WM_INPUT && gHCEBlockRawInput.load(std::memory_order_relaxed))
		{
			RAWINPUT raw{};
			UINT rawSize = sizeof(raw);
			const UINT read = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER));
			const bool isKeyRelease = read != (UINT)-1
				&& raw.header.dwType == RIM_TYPEKEYBOARD
				&& (raw.data.keyboard.Flags & RI_KEY_BREAK) != 0;

			if (!isKeyRelease)
				return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}

		// Everything else (very much including WM_NCCREATE) goes through untouched. InlineHook::call takes the
		// hook's own recursive_mutex, which is what stops the trampoline being freed underneath us.
		return hook->getInlineHook().call<LRESULT, HWND, UINT, WPARAM, LPARAM>(hWnd, uMsg, wParam, lParam);
	}

	std::string hceBytesToString(const std::vector<byte>& bytes)
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


class BlockGameInputHCEImpl : public TokenSharedRequestProvider
{
private:
	// Pure PointerDataStore (XML) lookups - building these touches no game memory.
	std::shared_ptr<MultilevelPointer> mWndProcFunction;
	std::shared_ptr<std::vector<byte>> mWndProcExpectedBytes;
	std::shared_ptr<ModuleInlineHook> mWndProcHook;

	std::mutex mAttachMutex;
	bool mVerified = false;      // guarded by mAttachMutex
	bool mHookAttached = false;  // guarded by mAttachMutex

	// Refuse to hook a build we don't recognise. See the SAFETY note above. Runs BEFORE anything is patched -
	// after attaching, the site reads back as our jmp rather than the original instructions, hence the sticky
	// mVerified flag rather than a per-toggle check.
	void verifyOriginalBytes()
	{
		if (!mWndProcFunction) throw HCMRuntimeException("No pointer data for the HaloCER raw-input WndProc");
		if (!mWndProcExpectedBytes || mWndProcExpectedBytes->empty()) throw HCMRuntimeException("No expected original bytes for the HaloCER raw-input WndProc");

		const std::vector<byte>& expected = *mWndProcExpectedBytes;
		std::vector<byte> actual(expected.size());
		if (!mWndProcFunction->readArrayData(actual.data(), actual.size()))
			throw HCMRuntimeException(std::format("Could not read the HaloCER raw-input WndProc: {}", MultilevelPointer::GetLastError()));

		if (actual != expected)
			throw HCMRuntimeException(std::format(
				"The HaloCER raw-input WndProc did not match its expected original bytes - this build of Halo Campaign Evolved is not supported by \"Block input when GUI open\". Expected [{}], found [{}]",
				hceBytesToString(expected), hceBytesToString(actual)));
	}

	// Installed on first request and then left in place (the detour is a single atomic load when idle).
	// Detaching on every toggle is avoided on purpose: it would mean repeatedly destroying a trampoline that the
	// message thread may be standing in, for no benefit.
	void ensureHookAttached()
	{
		std::scoped_lock lock(mAttachMutex);
		if (mHookAttached) return;

		if (!mVerified)
		{
			verifyOriginalBytes();
			mVerified = true;
			PLOG_INFO << "HaloCER raw-input WndProc matched its expected original bytes";
		}

		// Publish BEFORE patching. The detour is unreachable until the jmp is written, so this ordering means
		// there is no window where an input message can arrive at a detour that has no trampoline to call.
		gHCEWndProcHook.store(mWndProcHook.get(), std::memory_order_release);

		mWndProcHook->setWantsToBeAttached(true);
		if (!mWndProcHook->isHookInstalled())
		{
			gHCEWndProcHook.store(nullptr, std::memory_order_release);
			mWndProcHook->setWantsToBeAttached(false);
			throw HCMRuntimeException("Failed to install the Halo Campaign Evolved raw-input block hook");
		}

		mHookAttached = true;
		PLOG_INFO << "HaloCER raw-input WndProc hook installed (exe RVA 0x39FCDD0)";
	}

public:
	BlockGameInputHCEImpl(std::shared_ptr<PointerDataStore> ptr)
	{
		PLOG_INFO << "BlockGameInput: using Halo Campaign Evolved raw-input WndProc implementation";

		// Construction may ONLY ever throw HCMInitException - ControlServiceContainer catches nothing else, and
		// an HCMInitException there simply disables the service instead of taking HCM down. getData/getVectorData
		// already throw exactly that when an entry is missing.
		//
		// The game MUST be passed explicitly: PointerDataStore's key is (name, optional<GameState>) and it only
		// falls back from (name, game) to (name, nullopt), never the other way. These entries carry
		// Game="HaloCER", so the no-game form used by the MCC implementation above would throw "pointerData was
		// null". GameState(Value) is a non-explicit constexpr ctor, so constructing one here is fine - it is only
		// gameToCheck == GameState::Value::X *comparisons* that are C2666-ambiguous.
		const GameState hce = GameState(GameState::Value::HaloCER);
		mWndProcFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(blockGameInputHCEWndProcFunction), hce);
		mWndProcExpectedBytes = ptr->getVectorData<byte>(nameof(blockGameInputHCEWndProcOriginalBytes), hce);

		// startEnabled = false: nothing is resolved and nothing is patched here. L"main" is only the
		// ModuleHookManager bucket key - no module named "main" ever passes through
		// postModuleLoad_UpdateHooks, so this hook is never auto-attached or auto-detached and
		// setWantsToBeAttached(true) is the only thing that can ever install it. Same as the MCC
		// implementation's patches and FreeMCCCursorImpl's midhook.
		mWndProcHook = ModuleInlineHook::make(L"main", mWndProcFunction, reinterpret_cast<void*>(&hceRawInputWndProcDetour), false);
	}

	~BlockGameInputHCEImpl()
	{
		PLOG_DEBUG << "~" << nameof(BlockGameInputHCEImpl);

		// 1. Make the detour a pure pass-through before anything else moves.
		gHCEBlockRawInput.store(false, std::memory_order_release);
		ImGuiManager::setSwallowGameInput(false);

		// 2. Restore the original bytes. safetyhook freezes every other thread and fixes up their instruction
		//    pointers while it does this, so after it returns no thread can newly ENTER the detour.
		if (mWndProcHook) mWndProcHook->setWantsToBeAttached(false);

		// 3. Stop anyone still inside the detour from using the hook object.
		gHCEWndProcHook.store(nullptr, std::memory_order_release);

		// 4. Let any thread already inside the detour finish before the hook object is freed. Bounded, because a
		//    destructor must never wait forever (an unbounded spin-wait here is exactly what used to deadlock
		//    HCM's teardown - see the comment in SharedRequestProvider::~SharedRequestProvider). The detour body
		//    is microseconds long, so 200 ms is an enormous margin.
		for (int i = 0; i < 200 && gHCEDetourInFlight.load(std::memory_order_acquire) != 0; ++i)
			Sleep(1);

		if (gHCEDetourInFlight.load(std::memory_order_acquire) != 0)
		{
			// Someone is wedged inside the original WndProc body. Deliberately leak one reference rather than
			// free memory a live thread is about to dereference. 16 bytes, once, on a path that should never
			// happen - a use-after-free on the message thread would be a hard crash.
			PLOG_ERROR << "HaloCER raw-input detour still in flight at teardown; leaking the hook object rather than freeing it";
			auto* intentionallyLeaked = new std::shared_ptr<ModuleInlineHook>(mWndProcHook);
			(void)intentionallyLeaked;
		}
	}

	virtual void updateService() override
	{
		// MUST NOT THROW. SharedRequestProvider::makeScopedRequest installs a shared_ptr *deleter* that calls
		// updateService(), and a deleter runs in a noexcept destructor context - an escaping exception there is
		// std::terminate. Everything, including the logging, is inside the try.
		bool requested = false;
		try
		{
			requested = serviceIsRequested();
			PLOG_INFO << "BlockGameInput (HCE) service is turning " << (requested ? "ON!" : "OFF!");

			if (requested) ensureHookAttached(); // first request installs the hook; later ones are a no-op

			gHCEBlockRawInput.store(requested, std::memory_order_release);

			// Secondary layer, kept on purpose. It filters the SWAPCHAIN window (a different HWND entirely), so
			// it is orthogonal to the hook above: it stops ImGui-bound clicks and the Slate/UI message path
			// reaching the game. Harmless either way.
			ImGuiManager::setSwallowGameInput(requested);
		}
		// Both catches FAIL OPEN - every layer off. If we cannot install the hook (unsupported build) then we
		// cannot actually stop look/movement anyway, and a half-applied block that leaves the swallow latched on
		// would be far worse than no block: the player would be stuck unable to play.
		catch (const std::exception& ex)
		{
			gHCEBlockRawInput.store(false, std::memory_order_release);
			ImGuiManager::setSwallowGameInput(false);
			try { PLOG_ERROR << "BlockGameInput (HCE) failed to update: " << ex.what(); }
			catch (...) {}
		}
		catch (...)
		{
			gHCEBlockRawInput.store(false, std::memory_order_release);
			ImGuiManager::setSwallowGameInput(false);
			try { PLOG_ERROR << "BlockGameInput (HCE) failed to update: unknown exception"; }
			catch (...) {}
		}
	}
};

extern bool hostIsCampaignEvolved(); // defined in FreeMCCCursor.cpp

BlockGameInput::BlockGameInput(std::shared_ptr<PointerDataStore> ptr)
	: pimpl(hostIsCampaignEvolved()
		? std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<BlockGameInputHCEImpl>(ptr))
		: std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<BlockGameInputImpl>(ptr))) {}

BlockGameInput::~BlockGameInput() = default;
