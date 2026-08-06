#include "pch.h"
#include "HCEStateHook.h"
#include "HCESignatureScan.h"
#include "GlobalKill.h"
#include "imgui.h"   // for the software-cursor toggle in pollCursor()

namespace
{
	// RVAs into HaloSimulation_tag_release.dll (HCM_Evolved addresses.json)
	constexpr uintptr_t kRvaTickCounter = 0x12944C8;
	constexpr uintptr_t kRvaCurrentLevel = 0xCA2F00;

	// ---------------------------------------------------------------------------------------------------------
	// WHERE THE LEVEL NAME COMES FROM, AND WHY IT IS NO LONGER simBase + 0xCA2F00.
	//
	// 0xCA2F00 is a 128-byte string owned by the session/presence updater (sub_1804C62F0). That function composes
	// a name into a stack buffer and then, at 0x4C6E4E, `cmp byte ptr [buf], 0 / jz` - it only copies the result
	// in when the composed name is NON-EMPTY, and it never clears the global. So the buffer is a CACHE that the
	// engine is free to leave untouched, and it stays ALL ZEROES for a whole session whenever that updater never
	// resolves a name (it also bails outright on `if (!byte_18172C1C0) return;` at the top).
	//
	// That is the bug this replaced, and HCM's own logs proved it on ONE build within one hour: several sessions
	// logged "level=8" while playing d20, and others logged "level=255" for eighteen straight minutes of the same
	// level - while, in those same sessions, HCEBspOverlay was drawing 575 surfaces straight out of the scenario
	// tag and the checkpoint detours were armed. A level was unambiguously loaded; only 0xCA2F00 disagreed.
	//
	// The engine's OWN answer to "which level is this" is the live session header at the front of the game state
	// block: *(simBase + 0x142AA10) + 0x008 is the scenario path, NUL-terminated inside a 0x100-byte field. The
	// session setup at 0x19D21B writes it at level load; the revert strcmp's it, i.e. it is literally the field
	// that decides whether a checkpoint belongs to the level you are standing in; and the teardown
	// (sub_18019CFC0: `xor edi, edi` ... `mov cs:qword_18142AA10, rdi`) NULLS the pointer, so "no level loaded"
	// is representable rather than stale. HCECheckpointBlob.h documents the same header from the other side, and
	// every .bin HCM has dumped reads "levels\halo1\solo\d20\d20" at +0x008.
	//
	// 0xCA2F00 is kept ONLY as a fallback for the one thing the header cannot cover: the signature below failing
	// to resolve on a future build. That is not "falling back to a remembered address" for the header - it is the
	// source HCM already used, unchanged - and a stale string cannot invent a wrong level, because
	// levelIdFromHceName accepts nothing but the ten Halo CE scenario tags.
	// ---------------------------------------------------------------------------------------------------------
	constexpr int64_t kStateHeaderScenarioPath = 0x008;

	// The local save's memcpy inside save_checkpoint (sub_18019D400, rva 0x19D639):
	//     movsxd r8, [rip+stateLength] / mov rdx, [rip+stateBlock] / mov rcx, [rcx] / call memcpy
	// Every byte of this is opcode+modrm - there is no build-specific immediate in it - and it is the only match
	// in .text on this build. HaloCampaignEvolved has NO version resource, so a byte signature is the only update
	// detector this title admits; the contract in HCESignatureScan.h is that zero or more than one match resolves
	// NOTHING, never a remembered address.
	constexpr const char* kSigStateBlockSlot = "4C 63 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 8B 09 E8";
	constexpr int kSigStateBlockInsnOffset = 7;   // the `mov rdx, [rip+stateBlock]` inside the match
	constexpr int kSigStateBlockDispOffset = 3;   // its disp32
	constexpr int kSigStateBlockInsnLength = 7;   // rip is the address of the NEXT instruction

	constexpr const wchar_t* kSimModuleName = L"HaloSimulation_tag_release.dll";

	// Halo CE scenario tag names -> the matching halo1 LevelID. HCE is a remake of the CE campaign so the
	// classic a10/a30/... scenario names carry over. Matched as a substring so a full tag path
	// ("levels\\a10\\a10") or a bare name both work. Unknown//MP maps fall through to no_map_loaded.
	struct HceLevelName { const char* tag; LevelID id; };
	constexpr HceLevelName kHceLevels[] = {
		// ⚠ HCE RENAMES TWO OF THE TEN. Confirmed from the shipped tag tree (levels\halo1\solo\): the folders are
		//     a15  a30  a50  b30  b40  c10  c20  c45  d20  d40
		// so Pillar of Autumn is "a15" (not the classic a10) and Two Betrayals is "c45" (not c40). Everything
		// else carries the classic name. Both spellings are listed for each - a classic name costs nothing and a
		// future build could go either way.
		{ "a15", LevelID::_map_id_halo1_pillar_of_autumn },
		{ "a10", LevelID::_map_id_halo1_pillar_of_autumn },
		{ "a30", LevelID::_map_id_halo1_halo },
		{ "a50", LevelID::_map_id_halo1_truth_and_reconciliation },
		{ "b30", LevelID::_map_id_halo1_silent_cartographer },
		{ "b40", LevelID::_map_id_halo1_assault_on_the_control_room },
		{ "c10", LevelID::_map_id_halo1_343_guilty_spark },
		{ "c20", LevelID::_map_id_halo1_the_library },
		{ "c40", LevelID::_map_id_halo1_two_betrayals },
		{ "c45", LevelID::_map_id_halo1_two_betrayals },
		{ "d20", LevelID::_map_id_halo1_keyes },
		{ "d40", LevelID::_map_id_halo1_the_maw },
		// ⚠ THE THREE BONUS MISSIONS (levels\halo1\solo\extra\e10, e20, e30) ARE DELIBERATELY ABSENT. LevelID is
		// MCC's shared enum and has no value for them, so there is nothing to map them TO - inventing one would
		// ripple through every consumer that keys checkpoint folders off LevelID. On a bonus mission HCM
		// therefore still shows "no map loaded"; the warning below names the scenario so it is diagnosable
		// rather than mysterious. Fixing this properly means extending LevelID, which is its own change.
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
	// ALWAYS terminates `out`, including on every failure path: poll() tries two sources in turn, and a partial
	// write from the first one must not be readable as a name when the second one also fails.
	bool tryReadString(uintptr_t addr, char* out, size_t cap)
	{
		if (cap == 0) return false;
		out[0] = '\0';
		if (!addr) return false;
		if (IsBadReadPtr((void*)addr, 1)) return false;
		__try
		{
			size_t i = 0;
			for (; i + 1 < cap; ++i)
			{
				char c = *(volatile char*)(addr + i);
				if (c == '\0') break;
				// Not a printable name -> treat as unloaded. Terminate first: see the note above about the
				// second source overwriting what the first one left behind.
				if (c < 0x20 || c > 0x7E) { out[i] = '\0'; return false; }
				out[i] = c;
			}
			out[i] = '\0';
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
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

	// Signature-resolved ADDRESS of the game state block pointer, and the module base it was resolved against.
	// Touched only by the poll thread.
	uintptr_t mSigScannedBase = 0;
	uintptr_t mStateBlockSlot = 0;
	// Last "could not name the level" outcome, so the diagnostic in poll() reports each distinct one once
	// rather than every 40 ms. Poll thread only.
	std::string mLastUnmappedReport;

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

	// Resolves the ADDRESS of the game state block pointer, once, and says whether we have it. See the long note
	// at the top of this file for what it is and why the level comes from it.
	bool ensureStateBlockSlot(uintptr_t simBase)
	{
		if (mSigScannedBase != simBase)
		{
			// ONE scan per module load, and a FAILED scan is latched too. The scan walks the whole ~8 MB .text;
			// retrying that every 40 ms on a build the signature no longer matches would be a permanent
			// background cost that buys nothing. A module unload/reload (different base) is what re-arms it.
			mSigScannedBase = simBase;
			mStateBlockSlot = 0;

			int hits = 0;
			if (const uintptr_t match = HCESignatureScan::resolveUnique(simBase, kSigStateBlockSlot, hits))
				mStateBlockSlot = HCESignatureScan::ripTarget(match + kSigStateBlockInsnOffset,
					kSigStateBlockDispOffset, kSigStateBlockInsnLength);

			if (mStateBlockSlot)
				PLOG_INFO << "HCE state hook: game state block pointer resolved by signature @ 0x"
					<< std::hex << mStateBlockSlot << std::dec;
			else
				PLOG_WARNING << "HCE state hook: could not locate the game state block pointer by byte signature ("
					<< hits << " matches, exactly 1 is required). Halo Campaign Evolved probably updated. Falling "
					"back to the session/presence level string, which reports no level at all in some sessions.";
		}

		return mStateBlockSlot != 0;
	}

	// THE level name: the scenario path out of the live session header. false means NO LEVEL - either the state
	// block pointer is NULL (the teardown nulls it between levels) or the header is not written yet (mid-load).
	// ONLY call this once ensureStateBlockSlot() has said yes.
	bool tryReadScenarioPath(char* out, size_t cap)
	{
		uintptr_t stateBlock = 0;
		if (!tryReadPointer(mStateBlockSlot, stateBlock) || !stateBlock) return false;
		return tryReadString(stateBlock + kStateHeaderScenarioPath, out, cap);
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

				// The level is a STRING at both sources, not an index. HCE remakes the Halo CE campaign, so map
				// its scenario name onto the matching halo1 LevelID - those are contiguous from
				// _map_id_halo1_pillar_of_autumn in campaign order.
				//
				// THIS IS THE FIX: the level comes from the engine's live session header, and 0xCA2F00 is used
				// ONLY when that header could not be located at all. Reading 0xCA2F00 here reported
				// no_map_loaded for whole sessions with a level loaded and being played.
				//
				// ⚠ The two are NOT tried in turn. Once the header is available its answer is authoritative in
				// BOTH directions - the engine nulls the state block pointer between levels, so "no level" is a
				// real answer - whereas 0xCA2F00 is never cleared and would keep serving the last level you
				// played from the main menu.
				//
				// 128 bytes: the header's scenario path field is 0x100 and the presence buffer is 0x80, so this
				// is short enough to stay inside either field and long enough for any "levels\halo1\solo\x\x".
				char levelName[128]{};
				const bool haveHeader = ensureStateBlockSlot(base);
				const bool haveName = haveHeader
					? tryReadScenarioPath(levelName, sizeof(levelName))
					: tryReadString(base + kRvaCurrentLevel, levelName, sizeof(levelName));
				if (haveName) newLevel = levelIdFromHceName(levelName);

				// ⚠ DIAGNOSTIC, and it earns its place. This path has now been wrong TWICE for different
				// reasons (a presence cache that is never populated; then a header read that yields nothing),
				// and both times the log said only "level=255", which is indistinguishable between "could not
				// read the string", "read an empty string" and "read a string that matches no known level".
				// Reported once per distinct outcome, so it cannot spam a 40 ms poll.
				if (newLevel == LevelID::no_map_loaded)
				{
					std::string report = std::format("header={} name={} raw='{}'",
						haveHeader ? "yes" : "NO", haveName ? "yes" : "NO", levelName);
					if (report != mLastUnmappedReport)
					{
						mLastUnmappedReport = report;
						PLOG_WARNING << "HCE level unresolved: " << report
							<< " (slot=0x" << std::hex << mStateBlockSlot << std::dec << ")";
					}
				}
				else if (!mLastUnmappedReport.empty())
				{
					mLastUnmappedReport.clear();
				}
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
