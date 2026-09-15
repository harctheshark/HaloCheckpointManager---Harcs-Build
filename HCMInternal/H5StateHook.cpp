#include "pch.h"
#include "H5StateHook.h"
#include <TlHelp32.h>

// See H5StateHook.h for what each state means and why a -1 player datum is deliberately NOT "Loading".

namespace
{
	typedef LONG(NTAPI* H5SNtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	constexpr ULONG kThreadBasicInformation = 0;

	struct H5SThreadBasicInformation
	{
		LONG      ExitStatus;
		PVOID     TebBaseAddress;
		ULONG_PTR UniqueProcessId;
		ULONG_PTR UniqueThreadId;
		ULONG_PTR AffinityMask;
		LONG      Priority;
		LONG      BasePriority;
	};

	// SEH only - no C++ objects with destructors (MSVC C2712).
	bool sehRead(void* dest, const void* src, size_t size)
	{
		__try { memcpy(dest, src, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template<typename T>
	bool readAt(uintptr_t addr, T& out)
	{
		if (!addr) return false;
		return sehRead(&out, (const void*)addr, sizeof(T));
	}

	constexpr uintptr_t kRvaTlsIndex    = 0x05F1D56C;
	constexpr uintptr_t kTlsPlayerGlobals = 0x1560;
	constexpr uintptr_t kTlsObjectGlobals = 0x4B68;
}


class H5StateHook::H5StateHookImpl
{
public:
	MCCState mState{ GameState::Value::NoGame, PlayState::MainMenu, (LevelID)0 };
	std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> mStateChangedEvent
		= std::make_shared<eventpp::CallbackList<void(const MCCState&)>>();

	std::atomic<bool> mRun{ true };
	std::thread mPollThread;
	std::mutex mStateMutex;
	bool mCursorShowing = false;

	H5StateHookImpl()
	{
		mPollThread = std::thread([this]() { pollLoop(); });
	}

	~H5StateHookImpl()
	{
		mRun.store(false, std::memory_order_release);
		if (mPollThread.joinable()) mPollThread.join();
	}

	// True when some thread has an addressable simulation TLS block.
	bool simulationAddressable()
	{
		const uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
		if (!exeBase) return false;

		uint32_t tlsIndex = 0;
		if (!readAt(exeBase + kRvaTlsIndex, tlsIndex)) return false;

		static H5SNtQueryInformationThread_t ntQuery = nullptr;
		if (!ntQuery)
		{
			HMODULE nt = GetModuleHandleW(L"ntdll.dll");
			if (nt) ntQuery = (H5SNtQueryInformationThread_t)GetProcAddress(nt, "NtQueryInformationThread");
			if (!ntQuery) return false;
		}

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return false;

		const DWORD pid = GetCurrentProcessId();
		bool found = false;
		THREADENTRY32 te{}; te.dwSize = sizeof(te);
		if (Thread32First(snap, &te))
		{
			do
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!th) continue;
				H5SThreadBasicInformation tbi{};
				const LONG st = ntQuery(th, kThreadBasicInformation, &tbi, sizeof(tbi), nullptr);
				CloseHandle(th);
				if (st != 0 || !tbi.TebBaseAddress) continue;

				uintptr_t tlsArray = 0;
				if (!readAt((uintptr_t)tbi.TebBaseAddress + 0x58, tlsArray) || !tlsArray) continue;
				uintptr_t tlsBase = 0;
				if (!readAt(tlsArray + 8ull * tlsIndex, tlsBase) || !tlsBase) continue;

				uintptr_t playerGlobals = 0, objectGlobals = 0;
				if (!readAt(tlsBase + kTlsPlayerGlobals, playerGlobals) || !playerGlobals) continue;
				if (!readAt(tlsBase + kTlsObjectGlobals, objectGlobals) || !objectGlobals) continue;

				found = true;
				break;
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return found;
	}

	void pollLoop()
	{
		while (mRun.load(std::memory_order_acquire))
		{
			const bool live = simulationAddressable();

			// ⚠⚠⚠ DO NOT ASK WIN32 WHETHER THE CURSOR IS SHOWING ON THIS TITLE.
			// HCE's approach (GetCursorInfo + CURSOR_SHOWING) is right for a game that shows the OS cursor
			// when its menu is up. Halo 5 never does: it HIDES the OS cursor and clips it to 1x1 for mouse
			// look, so this read was false essentially always.
			//
			// That is not cosmetic - this flag gates ALL mouse input to the overlay. HCMInternalGUI turns it
			// into ImGuiWindowFlags_NoMouseInputs on its own window, so a false reading meant every click on
			// the HCM menu was discarded by ImGui before it reached a button. The give-away was that the
			// service-failures MODAL could be clicked (separate window, no such flag) while the menu could
			// not.
			//
			// In windowless mode HCM draws its OWN cursor (io.MouseDrawCursor), so "is the OS cursor
			// visible" is simply the wrong question - ours is always visible when the overlay is up.
			const bool cursor = true;

			MCCState next = live
				? MCCState{ GameState::Value::Halo5Forge, PlayState::Ingame, (LevelID)0 }
				: MCCState{ GameState::Value::NoGame,     PlayState::MainMenu, (LevelID)0 };

			bool changed = false;
			{
				std::scoped_lock lk(mStateMutex);
				mCursorShowing = cursor;
				if (next.currentGameState != mState.currentGameState
					|| next.currentPlayState != mState.currentPlayState)
				{
					mState = next;
					changed = true;
				}
			}
			if (changed)
			{
				PLOG_INFO << "H5StateHook: state -> "
					<< (live ? "Halo5Forge/Ingame" : "NoGame/MainMenu");
				(*mStateChangedEvent)(mState);
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}
};


H5StateHook::H5StateHook() : pimpl(std::make_unique<H5StateHookImpl>()) {}
H5StateHook::~H5StateHook() { PLOG_VERBOSE << "~H5StateHook"; }

const MCCState& H5StateHook::getCurrentMCCState() { return pimpl->mState; }

bool H5StateHook::isGameCurrentlyPlaying(GameState gameToCheck)
{
	std::scoped_lock lk(pimpl->mStateMutex);
	return pimpl->mState.currentGameState == gameToCheck
		&& pimpl->mState.currentPlayState == PlayState::Ingame;
}

std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> H5StateHook::getMCCStateChangedEvent()
{
	return pimpl->mStateChangedEvent;
}

bool* H5StateHook::getCursorShowingFlag() { return &pimpl->mCursorShowing; }
