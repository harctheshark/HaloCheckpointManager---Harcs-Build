#include "pch.h"
#include "InfiniteAmmo.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "IMakeOrGetCheat.h"
#include "GetPlayerDatum.h"
#include "GetObjectAddress.h"
#include "GameTickEventHook.h"
#include "ScopedThreadSuspender.h"
#include <Windows.h>

// ---- Halo 2: which grenades the cheat covers (2026-11-24, user: "Set Grenade Type It effects/gives infinite of, with
// Default Being All, but a Frags Only option and a Plasmas Only Option") -------------------------------------------------
// The cheat is one engine byte (halo2.dll+0x15F2883 in MCC 1.3528, "infiniteAmmoFlag" in the pointer data). Eight places
// read it: six in the weapon code (ammo), and two for grenades:
//   * sub_18093C920 (the grenade is created in the hand), `cmp byte [flag], 0` at +0x93C9E9: flag set = the count of the
//     unit's current grenade type (unit+592; counts at unit+594 frag, +595 plasma) is not decremented;
//   * sub_1808FFC90 (unit update), `cmp byte [flag], r14b` at +0x8FFF53: flag set = a type with no grenades gets 1.
// Both instructions are repointed (their rip-relative disp32 only, like the tickrate scalar in MasterTickrate.cpp) at
// two bytes HCM owns. With "All" those bytes just follow the real flag, i.e. the game's own behaviour. With Frags / Plasmas:
// the throw byte is the flag only while the player's CURRENT grenade type is the chosen one (set every tick on the game
// thread, where both functions run), and the refill byte stays 0 - the chosen type is topped up to 1 here instead.
// The weapon code keeps reading the real flag, so weapon ammo is infinite whatever is picked.
namespace
{
	class H2GrenadeGate
	{
		static constexpr uint32_t kFlagRVA = 0x15F2883;
		static constexpr uint32_t kThrowSiteRVA = 0x93C9E9;    // 80 3D <disp32> 00 : cmp byte [rip+disp32], 0
		static constexpr uint32_t kThrowDispOff = 2;
		static constexpr uint8_t  kThrowStock[7] = { 0x80, 0x3D, 0x93, 0x5E, 0xCB, 0x00, 0x00 };
		static constexpr uint32_t kRefillSiteRVA = 0x8FFF53;   // 44 38 35 <disp32> : cmp byte [rip+disp32], r14b
		static constexpr uint32_t kRefillDispOff = 3;
		static constexpr uint8_t  kRefillStock[7] = { 0x44, 0x38, 0x35, 0x29, 0x29, 0xCF, 0x00 };
		static constexpr uint32_t kInsnLen = 7;                // both: rip = site + 7

		uintptr_t base = 0;
		uint8_t* slots = nullptr;   // [0] = what the throw reads, [1] = what the refill reads
		bool applied = false;
		bool refused = false;       // this halo2.dll is not the build the two sites were found in

		static void* allocNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO si; GetSystemInfo(&si);
			const uintptr_t gran = si.dwAllocationGranularity;
			const uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL;
			const uintptr_t hi = target + 0x70000000ULL;
			const uintptr_t startp = target & ~(gran - 1);
			for (uintptr_t p = startp; p > lo; p -= gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
			for (uintptr_t p = startp + gran; p < hi; p += gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
			return nullptr;
		}

		// Only ever called on the game thread (the tick callback), which is the one thread that runs the two patched
		// functions - so no other thread can be half way through these bytes.
		static void writeText(uintptr_t addr, const void* data, size_t len)
		{
			DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, len);
			VirtualProtect((void*)addr, len, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
		}

		bool stillLoaded() const { return base && (uintptr_t)GetModuleHandleA("halo2.dll") == base; }

	public:
		bool isApplied() const { return applied && stillLoaded(); }
		bool isRefused() const { return refused; }
		uintptr_t moduleBase() const { return base; }

		// halo2.dll was unloaded or reloaded: the old patch went with it
		void forgetStale()
		{
			if (base && !stillLoaded())
			{
				if (slots) { VirtualFree(slots, 0, MEM_RELEASE); slots = nullptr; }
				applied = false; refused = false; base = 0;
			}
		}

		// true = both sites read our bytes now. false = not possible (no halo2.dll yet, or another build: see isRefused).
		bool apply()
		{
			forgetStale();
			if (applied) return true;
			if (refused) return false;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;
			const uintptr_t throwSite = base + kThrowSiteRVA, refillSite = base + kRefillSiteRVA;
			if (IsBadReadPtr((void*)throwSite, kInsnLen) || IsBadReadPtr((void*)refillSite, kInsnLen)
				|| memcmp((void*)throwSite, kThrowStock, kInsnLen) != 0 || memcmp((void*)refillSite, kRefillStock, kInsnLen) != 0)
			{
				refused = true;
				PLOG_ERROR << "Infinite Ammo grenade type: the grenade checks at halo2.dll+0x93C9E9 / +0x8FFF53 do not match MCC 1.3528; left alone";
				return false;
			}
			slots = (uint8_t*)allocNear(base, 0x1000);
			if (!slots) { refused = true; PLOG_ERROR << "Infinite Ammo grenade type: no memory near halo2.dll"; return false; }
			const int64_t relThrow = (int64_t)(uintptr_t)&slots[0] - (int64_t)(throwSite + kInsnLen);
			const int64_t relRefill = (int64_t)(uintptr_t)&slots[1] - (int64_t)(refillSite + kInsnLen);
			if (relThrow > INT32_MAX || relThrow < INT32_MIN || relRefill > INT32_MAX || relRefill < INT32_MIN)
			{
				VirtualFree(slots, 0, MEM_RELEASE); slots = nullptr; refused = true;
				PLOG_ERROR << "Infinite Ammo grenade type: the near page is out of reach";
				return false;
			}
			slots[0] = slots[1] = *(uint8_t*)(base + kFlagRVA);   // the game's own behaviour until the first set()
			const int32_t dThrow = (int32_t)relThrow, dRefill = (int32_t)relRefill;
			writeText(throwSite + kThrowDispOff, &dThrow, sizeof(dThrow));
			writeText(refillSite + kRefillDispOff, &dRefill, sizeof(dRefill));
			applied = true;
			PLOG_INFO << "Infinite Ammo grenade type: the two grenade checks (halo2+0x93C9E9 throw, +0x8FFF53 refill) now read HCM's bytes";
			return true;
		}

		uint8_t engineFlag() const { return base ? *(uint8_t*)(base + kFlagRVA) : 0; }
		void set(uint8_t throwFlag, uint8_t refillFlag) { if (slots) { slots[0] = throwFlag; slots[1] = refillFlag; } }

		// put the two instructions back as shipped. Called when HCM shuts down, i.e. NOT on the game thread: the other
		// threads are held while the bytes change (as MasterTickrate does for its patch).
		void revert()
		{
			if (applied && stillLoaded())
			{
				ScopedThreadSuspender suspend;
				writeText(base + kThrowSiteRVA, kThrowStock, kInsnLen);
				writeText(base + kRefillSiteRVA, kRefillStock, kInsnLen);
			}
			if (slots) { VirtualFree(slots, 0, MEM_RELEASE); slots = nullptr; }
			applied = false;
		}
	};

	// Halo 2 unit: the selected grenade type and the two counts (sub_18093C920 / sub_1808FFC90)
	constexpr uint32_t kUnitCurrentGrenadeOff = 592;
	constexpr uint32_t kUnitGrenadeCountOff = 594;   // int8 [2]: frag, plasma
}

template <GameState::Value gameT>
class InfiniteAmmoImplSetFlag : public IInfiniteAmmoImpl
{
private:

	// which game is this implementation for
	GameState mGame;

	// event callbacks
	ScopedCallback<ToggleEvent> mInfiniteAmmoCallbackHandle;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	//data
	std::shared_ptr<MultilevelPointer> infiniteAmmoFlag;

	// Halo 2 grenade type (see the note at the top). All optional: if any of it is missing the cheat is the plain flag.
	H2GrenadeGate mGrenadeGate;
	std::weak_ptr<GetPlayerDatum> getPlayerDatumWeak;
	std::weak_ptr<GetObjectAddress> getObjectAddressWeak;
	std::unique_ptr<ScopedCallback<GameTickEvent>> mGameTickCallback;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(SettingsEnums::InfiniteAmmoGrenades&)>>> mGrenadesChangedCallback;
	std::atomic<bool> mSaidRefused{ false };

	// primary event callback
	void onInfiniteAmmoToggle(bool& newValue)
	{



		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;

			PLOG_DEBUG << "onInfiniteAmmoToggle called, newValue: " << newValue;

			byte newFlagValue = newValue ? 1 : 0;
			if (!infiniteAmmoFlag->writeData(&newFlagValue)) throw HCMRuntimeException(std::format("Failed to write infiniteAmmoFlag flag {}", MultilevelPointer::GetLastError()));

			std::string what = newValue ? "Infinite Ammo enabled." : "Infinite Ammo disabled";
			if constexpr (gameT == GameState::Value::Halo2)
			{
				if (auto settings = settingsWeak.lock(); settings && newValue)
				{
					const auto g = settings->infiniteAmmoGrenades->GetValue();
					if (g == SettingsEnums::InfiniteAmmoGrenades::Frags) what = "Infinite Ammo enabled (grenades: frags only).";
					else if (g == SettingsEnums::InfiniteAmmoGrenades::Plasmas) what = "Infinite Ammo enabled (grenades: plasmas only).";
				}
			}
			messagesGUI->addMessage(what);
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}

	}

	void onGrenadesChanged(SettingsEnums::InfiniteAmmoGrenades& g)
	{
		auto mccStateHook = mccStateHookWeak.lock();
		if (!mccStateHook || mccStateHook->getCurrentMCCState().currentGameState != mGame) return;
		if (auto messagesGUI = messagesGUIWeak.lock())
			messagesGUI->addMessage(std::format("Infinite Ammo grenades: {}.", g == SettingsEnums::InfiniteAmmoGrenades::All ? "all"
				: g == SettingsEnums::InfiniteAmmoGrenades::Frags ? "frags only" : "plasmas only"));
	}

	// Game thread, once a tick, while a Halo 2 level runs.
	void onGameTick(uint32_t)
	{
		auto settings = settingsWeak.lock();
		if (!settings) return;
		const auto mode = settings->infiniteAmmoGrenades->GetValue();
		mGrenadeGate.forgetStale();
		if (mode == SettingsEnums::InfiniteAmmoGrenades::All)
		{
			// the game's own behaviour: nothing to patch; if it was patched earlier, the bytes just follow the flag
			if (mGrenadeGate.isApplied()) { const uint8_t f = mGrenadeGate.engineFlag(); mGrenadeGate.set(f, f); }
			return;
		}
		if (!mGrenadeGate.apply())
		{
			if (mGrenadeGate.isRefused() && !mSaidRefused.exchange(true))
				if (auto messagesGUI = messagesGUIWeak.lock()) messagesGUI->addMessage("Infinite Ammo: picking a grenade type needs MCC 1.3528 - all grenades stay infinite.");
			return;
		}
		const uint8_t flag = mGrenadeGate.engineFlag();
		if (!flag) { mGrenadeGate.set(0, 0); return; }

		const int chosen = mode == SettingsEnums::InfiniteAmmoGrenades::Frags ? 0 : 1;
		uint8_t throwFlag = 0;
		try
		{
			lockOrThrow(getPlayerDatumWeak, getPlayerDatum);
			lockOrThrow(getObjectAddressWeak, getObjectAddress);
			const Datum player = getPlayerDatum->getPlayerDatum();
			if (!player.isNull())
			{
				const uintptr_t unit = getObjectAddress->getObjectAddress(player);
				if (unit && !IsBadWritePtr((void*)(unit + kUnitCurrentGrenadeOff), 4))
				{
					throwFlag = (*(int8_t*)(unit + kUnitCurrentGrenadeOff) == chosen) ? 1 : 0;
					int8_t& count = *(int8_t*)(unit + kUnitGrenadeCountOff + chosen);
					if (count < 1) count = 1;   // what the game's own refill does for both types
				}
			}
		}
		catch (HCMRuntimeException&) {}
		mGrenadeGate.set(throwFlag, 0);
	}


public:
	InfiniteAmmoImplSetFlag(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mInfiniteAmmoCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->infiniteAmmoToggle->valueChangedEvent, [this](bool& n) {onInfiniteAmmoToggle(n); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>())
	{

		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		infiniteAmmoFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(infiniteAmmoFlag), mGame);

		if constexpr (gameT == GameState::Value::Halo2)
		{
			// the grenade type is an extra: without these services the cheat still works as the plain flag
			try
			{
				GameState game = gameImpl;   // (the name resolveDependentCheat expects)
				getPlayerDatumWeak = resolveDependentCheat(GetPlayerDatum);
				getObjectAddressWeak = resolveDependentCheat(GetObjectAddress);
				auto gameTickEventHook = resolveDependentCheat(GameTickEventHook);
				if (!gameTickEventHook || getPlayerDatumWeak.expired() || getObjectAddressWeak.expired()) throw HCMInitException("a service it needs is missing");
				mGameTickCallback = gameTickEventHook->getGameTickEvent()->subscribe([this](uint32_t t) { onGameTick(t); });
				mGrenadesChangedCallback = std::make_unique<ScopedCallback<eventpp::CallbackList<void(SettingsEnums::InfiniteAmmoGrenades&)>>>(
					dicon.Resolve<SettingsStateAndEvents>().lock()->infiniteAmmoGrenades->valueChangedEvent,
					[this](SettingsEnums::InfiniteAmmoGrenades& g) { onGrenadesChanged(g); });
			}
			catch (HCMInitException& ex)
			{
				PLOG_ERROR << "Infinite Ammo: the grenade type option is unavailable (" << ex.what() << "); all grenades stay infinite";
			}
		}
	}

	// unlike most cheats we want to make sure we set the flag back to zero when HCM closes
	~InfiniteAmmoImplSetFlag()
	{
		mGameTickCallback.reset();   // no more ticks touching the gate
		mGrenadesChangedCallback.reset();
		mGrenadeGate.revert();
		byte newFlagValue = 0;
		if (!infiniteAmmoFlag->writeData(&newFlagValue)) PLOG_ERROR << (std::format("Failed to write infiniteAmmoFlag flag {}", MultilevelPointer::GetLastError()));
	}
};



InfiniteAmmo::InfiniteAmmo(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo1:
		pimpl = std::make_unique<InfiniteAmmoImplSetFlag<GameState::Value::Halo1>>(gameImpl, dicon);
		break;

	case GameState::Value::Halo2:
		pimpl = std::make_unique<InfiniteAmmoImplSetFlag<GameState::Value::Halo2>>(gameImpl, dicon);
		break;


	default:
		throw HCMInitException("not impl yet");
	}
}

InfiniteAmmo::~InfiniteAmmo()
{
	PLOG_DEBUG << "~" << getName();
}
