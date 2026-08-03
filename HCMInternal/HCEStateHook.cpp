#include "pch.h"
#include "HCEStateHook.h"
#include "GlobalKill.h"
#include "imgui.h"   // for the software-cursor toggle in pollCursor()

namespace
{
	// RVAs into HaloSimulation_tag_release.dll (HCM_Evolved addresses.json)
	constexpr uintptr_t kRvaTickCounter = 0x12944C8;
	constexpr uintptr_t kRvaCurrentLevel = 0xCA2F00;

	constexpr const wchar_t* kSimModuleName = L"HaloSimulation_tag_release.dll";

	// Halo CE scenario tag names -> the matching halo1 LevelID. HCE is a remake of the CE campaign so the
	// classic a10/a30/... scenario names carry over. Matched as a substring so a full tag path
	// ("levels\\a10\\a10") or a bare name both work. Unknown//MP maps fall through to no_map_loaded.
	struct HceLevelName { const char* tag; LevelID id; };
	constexpr HceLevelName kHceLevels[] = {
		{ "a10", LevelID::_map_id_halo1_pillar_of_autumn },
		{ "a30", LevelID::_map_id_halo1_halo },
		{ "a50", LevelID::_map_id_halo1_truth_and_reconciliation },
		{ "b30", LevelID::_map_id_halo1_silent_cartographer },
		{ "b40", LevelID::_map_id_halo1_assault_on_the_control_room },
		{ "c10", LevelID::_map_id_halo1_343_guilty_spark },
		{ "c20", LevelID::_map_id_halo1_the_library },
		{ "c40", LevelID::_map_id_halo1_two_betrayals },
		{ "d20", LevelID::_map_id_halo1_keyes },
		{ "d40", LevelID::_map_id_halo1_the_maw },
	};

	LevelID levelIdFromHceName(const char* name)
	{
		if (!name || !*name) return LevelID::no_map_loaded;
		for (const auto& e : kHceLevels)
			if (std::strstr(name, e.tag) != nullptr) return e.id;
		return LevelID::no_map_loaded;
	}

	// Safe read of a dword inside the sim module; false if the module went away or the address is bad.
	bool tryReadDword(uintptr_t addr, uint32_t& out)
	{
		if (!addr) return false;
		if (IsBadReadPtr((void*)addr, sizeof(uint32_t))) return false;
		__try { out = *(volatile uint32_t*)addr; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool tryReadPointer(uintptr_t addr, uintptr_t& out)
	{
		if (!addr) return false;
		if (IsBadReadPtr((void*)addr, sizeof(uintptr_t))) return false;
		__try { out = *(volatile uintptr_t*)addr; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Reads a NUL-terminated ASCII string (bounded). The level name is stored as a string, not an index.
	bool tryReadString(uintptr_t addr, char* out, size_t cap)
	{
		if (!addr || cap == 0) return false;
		if (IsBadReadPtr((void*)addr, 1)) return false;
		__try
		{
			size_t i = 0;
			for (; i + 1 < cap; ++i)
			{
				char c = *(volatile char*)(addr + i);
				if (c == '\0') break;
				if (c < 0x20 || c > 0x7E) return false;   // not a printable name -> treat as unloaded
				out[i] = c;
			}
			out[i] = '\0';
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

class HCEStateHook::HCEStateHookImpl
{
private:
	MCCState mCurrentState{ GameState::Value::HaloCER, PlayState::MainMenu, LevelID::no_map_loaded };
	std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> mStateChangedEvent
		= std::make_shared<eventpp::CallbackList<void(const MCCState&)>>();

	std::mutex mStateMutex;
	std::atomic<bool> mRunning{ true };
	std::thread mPollThread;

	uint32_t mLastTick = 0;
	int mStillCount = 0;   // consecutive polls with an unchanged tick counter
	bool mCursorShowing = true;    // handed to HCMInternalGUI as a bool*; see pollCursor()
	bool mOsCursorVisible = false; // real WIN32 cursor state (drives ImGui's software cursor)

	// Cursor handling for the overlay.
	//
	// HCMInternalGUI applies ImGuiWindowFlags_NoInputs whenever its cursor flag is false, so that flag decides
	// whether the user can click the overlay AT ALL - including whether they can expand the collapsed HCM window.
	// On MCC there is a dedicated "free cursor" cheat that midhooks an MCC function to make MCC show its cursor;
	// that is MCC-specific and has no HCE equivalent.
	//
	// HCE hides the OS cursor during gameplay, so keying off the OS cursor alone would make the overlay
	// permanently un-clickable while playing (and unexpandable, so there'd be no way back). We therefore always
	// report the overlay as clickable on HCE, and instead ask ImGui to draw its own software cursor whenever the
	// OS one is hidden - so the user can see what they're pointing at during gameplay. In menus, where the OS
	// cursor is already visible, we leave it alone so there aren't two cursors on screen.
	void pollCursor()
	{
		CURSORINFO ci{};
		ci.cbSize = sizeof(ci);
		bool osCursorVisible = false;
		if (GetCursorInfo(&ci))
			osCursorVisible = (ci.flags & CURSOR_SHOWING) != 0;

		mOsCursorVisible = osCursorVisible;
		mCursorShowing = true;   // overlay input is always permitted on HCE (see comment above)
		// NOTE: we deliberately do NOT touch ImGui's software cursor here. Doing it from cursor state alone drew a
		// cursor for the whole of gameplay (the game hides the OS cursor while playing). HCMInternalGUI now enables
		// it only while the HCM window is actually open - the same rule MCC uses for its free-cursor service.
	}

	void poll()
	{
		pollCursor();
		HMODULE sim = GetModuleHandleW(kSimModuleName);
		PlayState newPlay = PlayState::MainMenu;
		LevelID newLevel = LevelID::no_map_loaded;

		if (sim)
		{
			uintptr_t base = (uintptr_t)sim;
			// tick_counter is a POINTER to the counter, not the counter itself (the reference tool does
			// read_pointer(base+rva) then read_int(that)). Reading the dword in place gave a value that never
			// changed, so we reported "Loading" forever - which also gated every feature off.
			uint32_t tick = 0;
			uintptr_t tickPtr = 0;
			if (tryReadPointer(base + kRvaTickCounter, tickPtr) && tryReadDword(tickPtr, tick))
			{
				// A frozen tick counter means the sim exists but isn't simulating -> loading (or paused at a
				// transition). Require a few consecutive still polls so a single dropped frame isn't "Loading".
				if (tick != mLastTick) { mStillCount = 0; mLastTick = tick; }
				else if (mStillCount < 100) ++mStillCount;

				newPlay = (mStillCount >= 3) ? PlayState::Loading : PlayState::Ingame;

				// current_level is a STRING (the reference tool does read_string(addr, 40)), not an index.
				// HCE remakes the Halo CE campaign, so map its scenario name onto the matching halo1 LevelID -
				// those are contiguous from _map_id_halo1_pillar_of_autumn in campaign order.
				char levelName[48]{};
				if (tryReadString(base + kRvaCurrentLevel, levelName, sizeof(levelName)))
					newLevel = levelIdFromHceName(levelName);
			}
			else
			{
				newPlay = PlayState::Loading; // module present but not readable yet (still initialising)
			}
		}
		else
		{
			mLastTick = 0; mStillCount = 0;
		}

		bool changed = false;
		MCCState snapshot{ GameState::Value::HaloCER, newPlay, newLevel };
		{
			std::scoped_lock lock(mStateMutex);
			if (mCurrentState.currentPlayState != newPlay || mCurrentState.currentLevelID != newLevel)
			{
				mCurrentState = snapshot;
				changed = true;
			}
		}

		if (changed)
		{
			PLOG_DEBUG << "HCE state changed: play=" << (int)newPlay << " level=" << (int)newLevel;
			try { (*mStateChangedEvent)(snapshot); }
			catch (...) { PLOG_ERROR << "exception in HCE state changed event"; }
		}
	}

public:
	HCEStateHookImpl()
	{
		mPollThread = std::thread([this]()
			{
				while (mRunning.load() && !GlobalKill::isKillSet())
				{
					try { poll(); }
					catch (...) {}
					// 40ms: fast enough that the cursor-showing flag (which gates overlay mouse input)
					// reacts promptly when the user opens a menu / frees the cursor.
					std::this_thread::sleep_for(std::chrono::milliseconds(40));
				}
			});
	}

	~HCEStateHookImpl()
	{
		mRunning.store(false);
		if (mPollThread.joinable()) mPollThread.join();
	}

	const MCCState& getCurrentMCCState()
	{
		std::scoped_lock lock(mStateMutex);
		return mCurrentState;
	}

	bool isGameCurrentlyPlaying(GameState gameToCheck)
	{
		std::scoped_lock lock(mStateMutex);
		// explicit cast: GameState has both operator==(GameState) and operator Value(), so comparing a
		// GameState directly against a Value is ambiguous (C2666).
		return static_cast<GameState::Value>(gameToCheck) == GameState::Value::HaloCER
			&& mCurrentState.currentPlayState == PlayState::Ingame;
	}

	std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> getMCCStateChangedEvent() { return mStateChangedEvent; }

	bool* getCursorShowingFlag() { return &mCursorShowing; }
};


HCEStateHook::HCEStateHook() : pimpl(std::make_unique<HCEStateHookImpl>()) {}
HCEStateHook::~HCEStateHook() { PLOG_VERBOSE << "~HCEStateHook"; }

const MCCState& HCEStateHook::getCurrentMCCState() { return pimpl->getCurrentMCCState(); }
bool HCEStateHook::isGameCurrentlyPlaying(GameState gameToCheck) { return pimpl->isGameCurrentlyPlaying(gameToCheck); }
std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> HCEStateHook::getMCCStateChangedEvent() { return pimpl->getMCCStateChangedEvent(); }
bool* HCEStateHook::getCursorShowingFlag() { return pimpl->getCursorShowingFlag(); }
