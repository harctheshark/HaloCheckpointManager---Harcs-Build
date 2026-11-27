#include "pch.h"
#include "MetroCheckpoint.h"
#include "MetroEngine.h"
#include "MetroMove.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "GlobalKill.h"

namespace
{
	// 2026-11-25 (user: revert "restarted the mission (the in game checkpoint was further ahead)"): both use the game's
	// checkpoint slot ("auto_save" - what the game's own "load last checkpoint" loads), like Halo's forced checkpoint.
	constexpr const char* kSaveName = "auto_save";
	using queue_t = void(*)(uintptr_t console, const char* fmt, ...);

	// 2026-11-25 build 114 (user: "it acts more like a save location, teleport back than a true revert ... if i die, and press
	// revert, it doesnt restore my health/player control ... dialog/scripts continue etc, id like it to act like a real
	// revert...just without the loading screen"). Static RE (binary_survey\agent_revert\): the engine's save is an in-memory
	// blob of per-entity chunks (player, AI, scripts, triggers ...); its load core 0x672030(game, name) - during gameplay -
	// only arms the per-frame save/load state machine 0x675CC0 (game+7568: 5 load -> 6 apply chunks -> 7 cleanup -> 0), and
	// for a save of the CURRENT level that path restores every entity in place (0x6748E0 same-level branch: no change_map, no
	// loader, no loading screen). The stock "gameload" console command never uses it: on the same level it calls the level
	// loader 0x69CEC0 (the loading screen). So HCM calls 0x672030 itself, on the game thread, from a hook on the tick.
	constexpr uintptr_t kTickRVA = 0x675CC0;
	constexpr uint8_t kTickPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xFF, 0x15 };
	constexpr uintptr_t kLoadRVA = 0x672030;
	constexpr uint8_t kLoadPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9 };
	constexpr uintptr_t kStateOff = 7568, kDelayOff = 7560, kCheckpointOff = 8272;
	constexpr uintptr_t kLoaderStateRVA = 0xFBDFC0;   // 8 = in gameplay
	constexpr uintptr_t kLevelFramesOff = 0x220;      // frames since the level started (the apply step waits for >= 5)

	using tick_t = uintptr_t(__fastcall*)(uintptr_t game);
	using load_t = uintptr_t(__fastcall*)(uintptr_t game, const char* name);

	std::atomic<tick_t> gOrigTick{ nullptr };
	std::atomic<bool> gRequest{ false };
	std::atomic<int> gResult{ 0 };          // posted by the game thread, reported by the render thread
	enum Result { kNone = 0, kStarted, kBusy, kNotInGameplay, kNoCheckpoint, kLevelStarting, kFaulted, kDone, kStuck };
	std::atomic<bool> gInFlight{ false };
	int gLastState = 0;
	ULONGLONG gStartMs = 0;

	bool sehQueue(queue_t fn, uintptr_t console, const char* fmt, const char* arg)
	{
		__try { fn(console, fmt, arg); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehLoad(load_t fn, uintptr_t game, const char* name)
	{
		__try { fn(game, name); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	int tryStartRestore(uintptr_t game)
	{
		int state = -1, loader = 0; uint32_t frames = 0;
		if (!metro::read(game + kStateOff, state)) return kFaulted;
		if (state != 0) return kBusy;
		if (!metro::read(metro::base() + kLoaderStateRVA, loader) || loader != 8) return kNotInGameplay;
		const uintptr_t level = metro::level();
		if (!level || !metro::read(level + kLevelFramesOff, frames) || frames < 5) return kLevelStarting;
		const uintptr_t ck = metro::readPtr(game + kCheckpointOff);   // same test as the game's own "continue" (0x671D80)
		uint16_t len = 0;
		if (!ck || !metro::read(ck + 12, len) || len == 0) return kNoCheckpoint;
		if (!sehLoad((load_t)(metro::base() + kLoadRVA), game, kSaveName)) return kFaulted;
		gInFlight = true; gLastState = 5; gStartMs = GetTickCount64();
		PLOG_INFO << "Revert: in-place restore armed (state 5)";
		return kStarted;
	}

	uintptr_t __fastcall tickDetour(uintptr_t game)
	{
		if (gRequest.exchange(false, std::memory_order_acq_rel) && !GlobalKill::isKillSet()) gResult = tryStartRestore(game);
		const uintptr_t r = gOrigTick.load(std::memory_order_acquire)(game);
		if (gInFlight.load(std::memory_order_relaxed))
		{
			int st = -1;
			metro::read(game + kStateOff, st);
			if (st != gLastState)
			{
				PLOG_INFO << std::format("Revert: state {} -> {} after {} ms", gLastState, st, GetTickCount64() - gStartMs);
				gLastState = st;
			}
			if (st == 0) { gInFlight = false; gResult = kDone; }
			else if (GetTickCount64() - gStartMs > 20000) { gInFlight = false; gResult = kStuck; }
		}
		return r;
	}
}

class MetroCheckpoint::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	bool mHooked = false;
	ULONGLONG mRequestMs = 0;

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	// queue "<verb> <name>" on the engine's console; "" = done, else why not
	std::string queue(const char* fmt, const char* name)
	{
		const uintptr_t console = metro::readPtr(metro::base() + metro::kConsoleRVA);
		if (!console) return "the game's console is not there yet";
		const uintptr_t vtbl = metro::readPtr(console);
		const uintptr_t fn = vtbl ? metro::readPtr(vtbl + 72) : 0;
		// the queue function must lie inside metro.exe's code
		const uintptr_t lo = metro::base(), hi = metro::base() + 0x1600000;
		if (!fn || fn < lo || fn >= hi) return "the game's console does not look as expected (another build?)";
		if (!sehQueue((queue_t)fn, console, fmt, name)) return "the game's console call faulted";
		return {};
	}

	bool inLevel() { auto m = mccWeak.lock(); return m && m->isGameCurrentlyPlaying(mGame); }

	// ticked = the stock way: Revert reloads the level from the save (loading screen)
	bool stockReload() { auto s = settingsWeak.lock(); return s && s->metroUseGameSavesToggle->GetValue(); }

	bool ensureHook()
	{
		if (mHooked) return true;
		auto matches = [](uintptr_t rva, const uint8_t* want, size_t n)
		{
			uint8_t got[32]{};
			return metro::detail::sehRead(got, (const void*)(metro::base() + rva), n) && memcmp(got, want, n) == 0;
		};
		if (!matches(kTickRVA, kTickPrologue, sizeof(kTickPrologue)) || !matches(kLoadRVA, kLoadPrologue, sizeof(kLoadPrologue)))
		{
			PLOG_ERROR << "MetroCheckpoint: the save/load tick or load core does not match this metro.exe build";
			return false;
		}
		static auto* parked = new std::vector<safetyhook::InlineHook>();   // never freed (HCM stays resident)
		auto h = safetyhook::InlineHook::create((void*)(metro::base() + kTickRVA), (void*)&tickDetour, safetyhook::InlineHook::StartDisabled);
		if (!h) { PLOG_ERROR << "MetroCheckpoint: could not hook the save/load tick"; return false; }
		gOrigTick.store(h->original<tick_t>(), std::memory_order_release);
		if (!h->enable()) { PLOG_ERROR << "MetroCheckpoint: could not enable the save/load tick hook"; return false; }
		parked->push_back(std::move(*h));
		mHooked = true;
		return true;
	}

	void onCheckpoint()
	{
		if (!inLevel()) { say("Save Checkpoint: load a level first"); return; }
		const std::string why = queue("gamesave %s", kSaveName);
		if (why.empty()) say("Checkpoint saved");
		else say("Save Checkpoint failed: " + why);
	}

	void reloadRevert(const char* note)
	{
		const std::string why = queue("gameload %s", kSaveName);
		if (why.empty()) say(note);
		else say("Revert failed: " + why);
	}

	void onRevert()
	{
		if (!inLevel()) { say("Revert: load a level first"); return; }
		if (stockReload() || !ensureHook()) { reloadRevert("Reverting to the last checkpoint (level reload)"); return; }
		gResult = kNone;
		gRequest = true;
		mRequestMs = GetTickCount64();
	}

	// render thread: report what the game thread did with the request
	void onRender(SimpleMath::Vector2)
	{
		if (mRequestMs && gRequest.load() && GetTickCount64() - mRequestMs > 3000)
		{
			// the tick never picked the request up (menu / pause?) - take it back
			gRequest = false; mRequestMs = 0;
			say("Revert failed: the game is not running its save/load step right now (paused or in a menu?)");
			return;
		}
		const int r = gResult.exchange(kNone);
		switch (r)
		{
		case kNone: return;
		case kStarted: return;   // wait for kDone
		case kDone: mRequestMs = 0; say("Reverted"); return;
		case kBusy: mRequestMs = 0; say("Revert: the game is already saving or loading - try again in a moment"); return;
		case kNotInGameplay: mRequestMs = 0; say("Revert: not in gameplay right now"); return;
		case kLevelStarting: mRequestMs = 0; say("Revert: the level is still starting - try again in a moment"); return;
		case kNoCheckpoint: mRequestMs = 0; say("Revert: there is no checkpoint on this level yet (press Save Checkpoint first)"); return;
		case kFaulted: mRequestMs = 0; reloadRevert("Revert: the instant restore failed - reloading the last checkpoint instead"); return;
		case kStuck: mRequestMs = 0; say("Revert: the restore did not finish (see the HCM log)"); return;
		default: return;
		}
	}

	ScopedCallback<ActionEvent> mCheckpointCallback;
	ScopedCallback<ActionEvent> mRevertCallback;
	ScopedCallback<RenderEvent> mRenderCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mCheckpointCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceCheckpointEvent, [this]() { onCheckpoint(); }),
		mRevertCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceRevertEvent, [this]() { onRevert(); }),
		mRenderCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRender(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroCheckpoint is Metro Redux only");
		metro::move::init();
		ensureHook();
	}

	~Impl()
	{
		mCheckpointCallback.removeCallback();
		mRevertCallback.removeCallback();
		mRenderCallback.removeCallback();
		gRequest = false;   // the tick hook stays parked and passes straight through
	}
};

MetroCheckpoint::MetroCheckpoint(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroCheckpoint::~MetroCheckpoint() { PLOG_VERBOSE << "~" << getName(); }
