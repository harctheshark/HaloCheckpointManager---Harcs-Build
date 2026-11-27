#include "pch.h"
#include "UncapDynamicLights.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "H2FadeListHooks.h"
#include "H2SafeWrite.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include <atomic>
#include <mutex>

// ============================================================================
//  THE TAG'S DISTANCE FADING (2026-11-19, user: "have it NOT fade ever via distance, or ONLY fade by a set amount, maybe 50%,
//  but never go to 0%"). A light tag's "Distance Fading Parameters" (Illumination / Shadow / Specular Fade: Fade Very Far ..)
//  are read by sub_1808E0180(light index) (= Xbox function_c12d0), which the renderer runs for every active light each frame.
//  It writes into the light (0x110 bytes, pool qword_1818B93C0), from the camera's distance past the light's radius:
//     +0x100 illumination  1 - clamp((d - r - {20,15,10,5,0}[illumination enum]) / 10)   <- what the fade lists fade towards
//     +0xF8  shadow        1 - clamp((d - r - {15,10,7,3,0}[shadow enum]) / 5), then <= illumination
//     +0xFC  (shadow enum, over the light's radius)
//     +0x104 specular      1 - clamp(d - r - {2.5,2,1.5,1,0.5}[specular enum]), then <= shadow
//  The hook lets the game compute them, then raises those four to at least the floor ("Min Distance Fade", 0..100 %). The
//  order the game keeps (specular <= shadow <= illumination) still holds: all four are raised to the same floor.
// ============================================================================
namespace
{
	constexpr uint32_t kLightFadeRva = 0x8E0180;
	const uint8_t kLightFadeBytes[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
	constexpr uint32_t kLightPoolRva = 0x18B93C0;   // qword: the lights' data array header (+72: offset of the records)
	constexpr uint32_t kLightStride = 272;
	constexpr uint32_t kFadeOffsets[] = { 0x100, 0xF8, 0xFC, 0x104 };

	std::unique_ptr<ModuleInlineHook> sFadeHook;
	std::atomic<ModuleInlineHook*> sFade{ nullptr };
	std::atomic<float> sFloor{ 1.0f };

	// POD + SEH: raise light `index`'s tag-driven fades to the floor (halo2.dll's base from where the hook sits)
	void raiseFades(uintptr_t base, uint16_t index, float floor)
	{
		__try
		{
			const uintptr_t pool = *(const uintptr_t*)(base + kLightPoolRva);
			if (!pool) return;
			const int64_t first = *(const int64_t*)(pool + 72);
			if (!first) return;
			float* const light = (float*)(pool + first + (uintptr_t)kLightStride * index);
			for (uint32_t off : kFadeOffsets)
			{
				float* const f = (float*)((uint8_t*)light + off);
				if (!(*f >= floor)) *f = floor;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	void __fastcall lightFadeDetour(uint64_t index)
	{
		ModuleInlineHook* const h = sFade.load();
		if (!h) return;
		h->getInlineHook().call<void>(index);
		const float floor = sFloor.load(std::memory_order_relaxed);
		if (floor > 0.0f) raiseFades(h->getInlineHook().target_address() - kLightFadeRva, (uint16_t)index, floor);
	}
}

// ============================================================================
//  Halo 2 (MCC halo2.dll 1.3528) - how many dynamic lights may shine at once.  2026-11-19
//
//  What limits them (read in MCC's code and in the Xbox decomp drop of 2026-11-18, src\unknown_0350e0.cpp: 0x31910 /
//  0x4a7c0 / 0x4ac30 - the same logic):
//    sub_1807E97C0  gathers the frame's visible lights (at most 128) into its caller's 128-record buffer and hands them
//                   to the view's fade list: add (sub_1807F6FE0), then process (sub_1807F7400) with limit 3.
//    the fade list  holds 32 entries; add cannot append an input past the 32nd (a 32-bit "taken" mask); process lets
//                   the first `limit` entries fade in and makes every other one fade out.
//    sub_1807EB4D0  draws each light whose faded value is above 0.07.
//  So a view shows its 3 most important lights: 3 by the limit, 32 by the list, 128 by the gather. (The lights pool
//  itself has room for 350, but no more than 128 are ever gathered for a frame - that is in the gatherer and its
//  caller's stack buffer, which this does NOT change.)
//
//  What this does: switches the lights' lists over to HCM's own (H2FadeListHooks.cpp / H2FadeLists.h - the engine's two
//  list functions rewritten with a 128-entry capacity, proven identical to the engine's at its own settings) and sets
//  how many may shine: "Max Lights", 3..128, default 128 = every light the game gathers.
//
//  The frame's visible lights come from the camera visibility collection's list at +0x18 (sub_180709F70()[3]), which holds
//  64 in stock (Uncap Visibility Limits raises it to 2048) - so past 64 shining lights that toggle is needed too.
//  Why stock already shows up to ~32: lights whose definition has bit 0x100000 never fade out past the limit (process
//  leaves them alone), so in stock the 32-entry list itself is their cap.
//
//  Cost / unknowns (nothing here has been seen running): a shining light is one more visibility query and one more pass
//  over what it lights, so busy scenes cost frame time; more lit sections also use up Halo 2's 850-render-section
//  budget sooner (Uncap Render Sections raises that). sub_1807EB4D0 also notes each drawn light in an 8-entry table
//  (dword_18186EC70) and stops noting past 8 - what that table feeds was not read. If a scene misbehaves, lower Max
//  Lights: the number at which it starts is the next limit to look at.
// ============================================================================

template <GameState::Value gameT>
class UncapDynamicLightsImpl : public IUncapDynamicLightsImpl
{
private:
	GameState mGame;
	std::mutex mMutex;   // the toggle arrives on the render thread and on hotkeys, MCC's state event on MCC's thread

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(int&)>> mMaxCallback;
	ScopedCallback<eventpp::CallbackList<void(int&)>> mFloorCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	void say(const std::string& text)
	{
		try { lockOrThrow(messagesGUIWeak, messagesGUI); messagesGUI->addMessage(text); }
		catch (HCMRuntimeException&) {}
	}

	// the hooks follow the settings. 1 = set, -1 = not build 1.3528, 0 = settings gone
	int sync(bool& on, int& max)
	{
		on = false; max = 128;
		int floorPercent = 100;
		try
		{
			lockOrThrow(settingsWeak, settings);
			on = settings->uncapDynamicLightsToggle->GetValue();
			max = settings->uncapDynamicLightsMax->GetValue();
			floorPercent = settings->uncapDynamicLightsFadeFloor->GetValue();
		}
		catch (HCMRuntimeException&) { return 0; }
		const int r = H2FadeListHooks::configure(H2FadeListHooks::Lights, on, max);
		// the tag's distance fading: hooked while the toggle is on and the floor is above 0, if the function is 1.3528's
		sFloor = (float)floorPercent / 100.0f;
		bool wantFade = on && floorPercent > 0 && sFadeHook;
		if (wantFade && !sFadeHook->isHookInstalled())
		{
			const uintptr_t base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (base && !h2safe::equals(base + kLightFadeRva, kLightFadeBytes, sizeof(kLightFadeBytes)))
			{
				PLOG_ERROR << "UncapDynamicLights: the light fade function is not build 1.3528's - Min Distance Fade unavailable";
				wantFade = false;
			}
		}
		if (sFadeHook) sFadeHook->setWantsToBeAttached(wantFade);
		PLOG_INFO << "UncapDynamicLights: min distance fade " << floorPercent << "% -> " << (wantFade ? "hooked" : "off");
		return r;
	}

	void onToggle(bool& newValue)
	{
		std::scoped_lock lock(mMutex);
		bool on = false; int max = 128;
		const int r = sync(on, max);
		if (r < 0) say("Uncap Dynamic Lights: this halo2.dll is not build 1.3528 - nothing was changed");
		else if (!GetModuleHandleA("halo2.dll") && newValue) say("Uncap Dynamic Lights: turns on when a Halo 2 level is loaded");
		else say(newValue ? std::format("Dynamic lights uncapped: up to {} can shine at once (stock 3)", max) : std::string("Dynamic lights back to the stock 3"));
	}

	void onMax(int& newValue)
	{
		std::scoped_lock lock(mMutex);
		bool on = false; int max = 128;
		const int r = sync(on, max);
		// (2026-11-19: the user changed Max Lights with the toggle off and saw no change - say so)
		if (r > 0 && on) say(std::format("Dynamic lights: up to {} can shine at once", max));
		else if (r >= 0 && !on) say(std::format("Max Lights {} is kept - it takes effect when Uncap Dynamic Lights is ticked", max));
	}

	void onFloor(int& newValue)
	{
		std::scoped_lock lock(mMutex);
		bool on = false; int max = 128;
		if (sync(on, max) < 0) return;
		if (!on) say(std::format("Min Distance Fade {}% is kept - it takes effect when Uncap Dynamic Lights is ticked", newValue));
		else say(newValue >= 100 ? std::string("Lights no longer fade with distance") : newValue <= 0 ? std::string("Lights fade with distance as the game does")
			: std::format("Lights fade with distance to no less than {}%", newValue));
	}

	// halo2.dll is unloaded when a game ends and loaded fresh for the next one: the hooks are asked for again once a level
	// is playing (that is also when a toggle set at the menu takes hold)
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame || newState.currentPlayState != PlayState::Ingame) return;
		std::scoped_lock lock(mMutex);
		bool on = false; int max = 128;
		if (sync(on, max) < 0) PLOG_ERROR << "UncapDynamicLights: not build 1.3528 - not applied";
	}

public:
	UncapDynamicLightsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapDynamicLightsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMaxCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapDynamicLightsMax->valueChangedEvent, [this](int& n) { onMax(n); }),
		mFloorCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapDynamicLightsFadeFloor->valueChangedEvent, [this](int& n) { onFloor(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
		H2FadeListHooks::acquire();
		try
		{
			auto at = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)kLightFadeRva });
			sFadeHook = ModuleInlineHook::make(L"halo2.dll", at, (void*)&lightFadeDetour);
			sFade = sFadeHook.get();
		}
		catch (const std::exception& ex) { PLOG_ERROR << "UncapDynamicLights: could not create the light fade hook: " << ex.what(); }
	}

	// the engine's own list code again, so nothing of the game points into HCM once this is gone
	~UncapDynamicLightsImpl()
	{
		std::scoped_lock lock(mMutex);
		H2FadeListHooks::configure(H2FadeListHooks::Lights, false, 3);
		H2FadeListHooks::release();
		if (sFadeHook) sFadeHook->setWantsToBeAttached(false);
		sFade = nullptr;
		sFadeHook.reset();
	}
};


UncapDynamicLights::UncapDynamicLights(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<UncapDynamicLightsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("UncapDynamicLights not impl for this game");
	}
}

UncapDynamicLights::~UncapDynamicLights()
{
	PLOG_DEBUG << "~" << getName();
}
