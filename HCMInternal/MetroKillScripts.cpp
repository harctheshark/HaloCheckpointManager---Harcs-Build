#include "pch.h"
#include "MetroKillScripts.h"
#include "MetroEngine.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "GlobalKill.h"

// 2026-11-25 (user: "make sure that Kill Scripts allows me to jump/sprint/have weapons drawn in areas the game normally doesnt
// let you, like in 'cinematic levels' ... Kill Scripts should override that"). Static RE: binary_survey\agent_restrict\.
//  - WEAPONS LOWERED ("actions/player/civil mode"): the action pushes {callback, owner, allowed-weapon-type mask} onto a stack in
//    the player's weapon state machine H = *(player+0xB70): entries (32 B) at *(H+1720), count u16 H+1730, mask u32 at +24.
//    The push is H vtbl+584 = 0x4E20A0(H, activate, &cb, mask, ...); a weapon whose type bit is not in the top mask is holstered
//    and cannot be drawn (0x4F5FA0, 0x4E4E90 ...). While on: the push gets mask 0xFFFFFFFF (the callback still fires at once,
//    so the script does not hang) and every entry already on the stack is opened up each frame.
//  - SPRINT / JUMP ("actions/player/movement"): 7 movement factors - current values player+5836.. (walk, run, sprint, crouch,
//    jump, accel, brake) and targets +5868..; a factor <= 0 blocks that move (sprint gate 0x276C10 tests +5844, jump 0x278280
//    tests +5852). While on, any factor below 0.05 is lifted back to 1.0 every frame (real slow-walk factors are left alone).
//  Not covered: a player bound to a scripted animation (0x2788B0) - those gates are checked before the factors.
namespace
{
	constexpr uintptr_t kEmitRVA = 0x735D60;
	// mov [rsp+8],rbx; push rdi; sub rsp,20h; xor ebx,ebx; mov rdi,rcx; cmp bx,[rcx+1Ah]
	constexpr uint8_t kEmitPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x48, 0x8B, 0xF9, 0x66, 0x3B, 0x59, 0x1A };
	constexpr uint8_t kRet = 0xC3;

	constexpr uintptr_t kWeaponPushRVA = 0x4E20A0;
	constexpr uint8_t kWeaponPushPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
	constexpr uintptr_t kPlayerWeaponsOff = 0xB70, kWStackOff = 1720, kWStackCountOff = 1730, kWEntryStride = 32, kWEntryMaskOff = 24;
	constexpr uintptr_t kFactorsOff = 5836, kFactorTargetsOff = 5868;
	constexpr int kFactorCount = 7;

	using push_t = uintptr_t(__fastcall*)(uintptr_t H, uintptr_t activate, uintptr_t cb, uintptr_t mask, uintptr_t a5, uintptr_t a6, uintptr_t a7);
	std::atomic<push_t> gOrigPush{ nullptr };
	std::atomic<bool> gOverride{ false };
	std::atomic<int> gOpenedPushes{ 0 };

	uintptr_t __fastcall pushDetour(uintptr_t H, uintptr_t activate, uintptr_t cb, uintptr_t mask, uintptr_t a5, uintptr_t a6, uintptr_t a7)
	{
		if (gOverride.load(std::memory_order_relaxed) && (activate & 0xFF) && (uint32_t)mask != 0xFFFFFFFFu)
		{
			mask = 0xFFFFFFFFu;
			gOpenedPushes.fetch_add(1, std::memory_order_relaxed);
		}
		return gOrigPush.load(std::memory_order_acquire)(H, activate, cb, mask, a5, a6, a7);
	}

	bool writeCodeByte(uintptr_t at, uint8_t v)
	{
		DWORD old = 0;
		if (!VirtualProtect((void*)at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
		*(volatile uint8_t*)at = v;
		DWORD tmp = 0;
		VirtualProtect((void*)at, 1, old, &tmp);
		FlushInstructionCache(GetCurrentProcess(), (void*)at, 1);
		return true;
	}
}

class MetroKillScripts::Impl
{
private:
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::mutex mMutex;
	bool mPatched = false;
	bool mBuildOk = false;
	bool mPushOk = false;
	bool mHooked = false;
	std::atomic<bool> mReady{ false };
	int mLiftsLogged = 0;

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	bool ensureHook()
	{
		if (mHooked) return true;
		if (!mPushOk) return false;
		static auto* parked = new std::vector<safetyhook::InlineHook>();   // never freed (HCM stays resident)
		auto h = safetyhook::InlineHook::create((void*)(metro::base() + kWeaponPushRVA), (void*)&pushDetour, safetyhook::InlineHook::StartDisabled);
		if (!h) return false;
		gOrigPush.store(h->original<push_t>(), std::memory_order_release);
		if (!h->enable()) return false;
		parked->push_back(std::move(*h));
		mHooked = true;
		return true;
	}

	void apply(bool on)
	{
		std::scoped_lock lk(mMutex);
		const uintptr_t at = metro::base() + kEmitRVA;
		if (on == mPatched) return;
		if (on)
		{
			if (!mBuildOk) { say("Kill Scripts: this metro.exe build does not match the one HCM was made for - not changed"); return; }
			if (!writeCodeByte(at, kRet)) { say("Kill Scripts: could not change the game's code"); return; }
			mPatched = true;
			if (!ensureHook()) PLOG_ERROR << "Kill Scripts: could not hook the weapon restriction push - lowered weapons stay lowered";
			gOverride = true;
			say("Kill Scripts on: level scripts are stopped, and their limits on weapons, sprint and jump are lifted");
		}
		else
		{
			gOverride = false;
			if (!writeCodeByte(at, kEmitPrologue[0])) { say("Kill Scripts: could not restore the game's code"); return; }
			mPatched = false;
			say("Kill Scripts off: level scripts run again");
		}
	}

	// every frame while on: open the weapon restrictions already on the stack, lift blocked movement factors
	void onRender(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire) || GlobalKill::isKillSet() || !gOverride.load()) return;
		const uintptr_t pl = metro::player();
		if (!pl) return;
		int lifted = 0, opened = 0;
		if (const uintptr_t H = metro::readPtr(pl + kPlayerWeaponsOff))
		{
			uint16_t n = 0;
			const uintptr_t entries = metro::readPtr(H + kWStackOff);
			if (entries && metro::read(H + kWStackCountOff, n) && n > 0 && n < 64)
				for (uint16_t i = 0; i < n; ++i)
				{
					uint32_t m = 0;
					const uintptr_t at = entries + kWEntryStride * i + kWEntryMaskOff;
					if (metro::read(at, m) && m != 0xFFFFFFFFu) { metro::write(at, 0xFFFFFFFFu); ++opened; }
				}
		}
		for (uintptr_t base : { pl + kFactorsOff, pl + kFactorTargetsOff })
			for (int i = 0; i < kFactorCount; ++i)
			{
				float f = 1.f;
				if (metro::read(base + 4 * i, f) && std::isfinite(f) && f < 0.05f) { metro::write(base + 4 * i, 1.f); ++lifted; }
			}
		if ((opened || lifted) && mLiftsLogged < 20)
		{
			++mLiftsLogged;
			PLOG_INFO << std::format("Kill Scripts: lifted {} weapon restriction(s) and {} blocked movement factor(s) ({} restriction pushes opened so far)",
				opened, lifted, gOpenedPushes.load());
		}
	}

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<RenderEvent> mRenderCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->metroKillScriptsToggle->valueChangedEvent, [this](bool& on) { apply(on); }),
		mRenderCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRender(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroKillScripts is Metro Redux only");
		uint8_t got[sizeof(kEmitPrologue)]{};
		mBuildOk = metro::detail::sehRead(got, (const void*)(metro::base() + kEmitRVA), sizeof(got)) && memcmp(got, kEmitPrologue, sizeof(got)) == 0;
		if (!mBuildOk) PLOG_ERROR << "MetroKillScripts: the script emit prologue does not match";
		uint8_t pro[sizeof(kWeaponPushPrologue)]{};
		mPushOk = metro::detail::sehRead(pro, (const void*)(metro::base() + kWeaponPushRVA), sizeof(pro)) && memcmp(pro, kWeaponPushPrologue, sizeof(pro)) == 0;
		if (!mPushOk) PLOG_ERROR << "MetroKillScripts: the weapon restriction push prologue does not match";
		if (auto s = settingsWeak.lock(); s && s->metroKillScriptsToggle->GetValue()) apply(true);
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderCallback.removeCallback();
		apply(false);   // HCM closing: scripts run again (the push hook stays parked but passes everything through)
	}
};

MetroKillScripts::MetroKillScripts(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroKillScripts::~MetroKillScripts() { PLOG_VERBOSE << "~" << getName(); }
