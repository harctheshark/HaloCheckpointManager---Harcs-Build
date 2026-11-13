#include "pch.h"
#include "OneShotKill.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "IMakeOrGetCheat.h"
#include "GetPlayerDatum.h"
#include "GetObjectAddress.h"
#include "Datum.h"

// ================================================================================================================================
// ONE SHOT KILL for the MCC games (1.3528; RE notes in Documents\Halo Mod And Tools\HCM One Shot Kill\).
//
// The engine's cheat_omnipotent (Halo 2 Sapien sub_A1B100 = object_cause_damage, where it exists in full) is:
//     instant_kill = (damage_data->flags >> 2) & 1;                 // _damage_kill_instantly_bit
//     ...
//     if (cheat_omnipotent && damage_data->owner.player_index != NONE) instant_kill = true;
// Each damage event carries a "kill" flag the damage function reads as "this damage kills, whatever the amount", and the
// cheat forced it for damage owned by a player. Retail has the switch compiled out of Halo 3 / ODST / Reach / Halo 4 (Halo
// 1 and 2 still read their cheat variable), but every game still has the flag - so the flag is what this drives, in all six.
//
// WHAT IT DOES (the user's spec, 2026-11-11): while the option is on, the kill flag is set for damage whose owner is a
// player, for each object it reaches - around that one call of the damage function, and taken off again after it (an area
// effect reuses one damage record for every object in range).
// EXCEPTIONS: never a player's unit, never the owner itself, never what the owner is riding - your own grenade's blast does
// not kill you. Nothing else changes: health and shields are untouched, enemy damage is as before, and turning it off leaves
// nothing behind. Allies and empty vehicles do die of a player's fire, as with the engine's cheat.
//
// HOW: object_cause_damage is hooked at its entry (one inline hook = all of its call sites: 20 in Halo 1, 7 in Halo 2, 21
// in Halo 3 and ODST, 4 in Reach and Halo 4). s_damage_data: +4 flags, +8 owner {player index, object index} - Halo 1: +8
// flags, +0xC owner player index, +0x10 owner object index. The object offsets (parent object index = HCM's
// bipedDataFields::currentVehicleDatum; a unit's controlling player index) are read off each game's own damage function,
// which tests `unit->controlling_player_index != NONE` itself. Halo 1's is not known: there only HCM's own player is
// recognised as "a player's unit".
// ⚠ Reach and Halo 4 pass a FLOAT as the second argument (xmm1; Hex-Rays shows an unused integer) - their detour carries it.
// ================================================================================================================================
namespace
{
	struct Spec
	{
		GameState::Value game; const wchar_t* module;
		uintptr_t fnRva;                                                   // object_cause_damage
		const uint8_t* probe; size_t probeLen;                             // its first bytes
		uintptr_t probe2Rva; const uint8_t* probe2; size_t probe2Len;      // a second, build-specific site (0 = none)
		uint32_t flagsOff, ownerPlayerOff, ownerObjectOff;                 // s_damage_data
		uint32_t parentOff;                                                // object: parent object index
		uint32_t unitPlayerOff;                                            // unit: controlling player index (0 = not known)
		bool floatArg2;                                                    // Reach / Halo 4: (damage, float, object, ...)
	};
	constexpr uint32_t kKillInstantlyBit = 4;

	// Halo 1: the function's first bytes, and `cmp cs:[cheat_omnipotent], al` inside it (its operand is the build's own)
	const uint8_t kH1Pro[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48 };
	const uint8_t kH1Probe2[] = { 0x38, 0x05, 0xC4, 0x2D, 0x0A, 0x01 };
	// Halo 2 / 3 / ODST share a generic prologue, so each has a second probe: Halo 2 `cmp cs:[cheat_omnipotent], 0`; Halo 3
	// and ODST the bytes just BEFORE HCM's own Invulnerability hook site (which may be patched)
	const uint8_t kGenericPro[] = { 0x48, 0x8B, 0xC4, 0x66, 0x44, 0x89, 0x48, 0x20, 0x66, 0x44, 0x89, 0x40, 0x18, 0x89, 0x50, 0x10 };
	const uint8_t kH2Probe2[] = { 0x80, 0x3D, 0x2B, 0x38, 0xCE, 0x00, 0x00 };
	const uint8_t kH3Probe2[] = { 0x78, 0x01, 0x00, 0x00, 0x74, 0x08, 0x66, 0x44, 0x89, 0x9B, 0x82, 0x00, 0x00, 0x00, 0x4D, 0x85, 0xFF };
	const uint8_t kOdstProbe2[] = { 0x70, 0x01, 0x00, 0x00, 0x74, 0x08, 0x66, 0x44, 0x89, 0x9B, 0x86, 0x00, 0x00, 0x00, 0x4D, 0x85, 0xE4 };
	// Reach / Halo 4: the prologue up to and including the stack cookie load (its rip displacement is the build's own)
	const uint8_t kReachPro[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xC7, 0xD4, 0x61, 0x00 };
	const uint8_t kH4Pro[] = { 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x62, 0x40, 0x7F, 0x00 };

	constexpr int kSlots = 6;
	const Spec kSpecs[kSlots] = {
		//  game                        module            fn        prologue                          second probe                                   flags own.pl own.obj parent unit.player float2
		{ GameState::Value::Halo1,     L"halo1.dll",     0xB9EA28, kH1Pro, sizeof(kH1Pro),           0xB9F3E5, kH1Probe2, sizeof(kH1Probe2),          8,   0xC,   0x10,   0xD8, 0,     false },
		{ GameState::Value::Halo2,     L"halo2.dll",     0x90EB80, kGenericPro, sizeof(kGenericPro), 0x90F055, kH2Probe2, sizeof(kH2Probe2),          4,   8,     0xC,    0x14, 0x140, false },
		{ GameState::Value::Halo3,     L"halo3.dll",     0x36FE00, kGenericPro, sizeof(kGenericPro), 0x3700F0, kH3Probe2, sizeof(kH3Probe2),          4,   8,     0xC,    0x10, 0x178, false },
		{ GameState::Value::Halo3ODST, L"halo3odst.dll", 0x3B68A8, kGenericPro, sizeof(kGenericPro), 0x3B6B88, kOdstProbe2, sizeof(kOdstProbe2),      4,   8,     0xC,    0x10, 0x170, false },
		{ GameState::Value::HaloReach, L"haloreach.dll", 0x4DCB30, kReachPro, sizeof(kReachPro),     0, nullptr, 0,                                   4,   8,     0xC,    0x14, 0x1CC, true },
		{ GameState::Value::Halo4,     L"halo4.dll",     0x646F94, kH4Pro, sizeof(kH4Pro),           0, nullptr, 0,                                   4,   8,     0xC,    0x24, 0x49C, true },
	};
	int slotFor(GameState::Value g) { for (int i = 0; i < kSlots; ++i) if (kSpecs[i].game == g) return i; return -1; }

	// ---- POD-only, SEH-guarded memory access (the game's thread, inside the damage call) ----
	bool bytesAt(uintptr_t a, const uint8_t* expect, size_t n)
	{
		__try { return memcmp((const void*)a, expect, n) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool readU32(uintptr_t a, uint32_t& out)
	{
		__try { out = *(const uint32_t*)a; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool readDamage(uintptr_t dmg, const Spec& s, uint32_t& flags, uint32_t& ownerPlayer, uint32_t& ownerObject)
	{
		__try
		{
			if (dmg < 0x10000) return false;
			flags = *(const uint32_t*)(dmg + s.flagsOff);
			ownerPlayer = *(const uint32_t*)(dmg + s.ownerPlayerOff);
			ownerObject = *(const uint32_t*)(dmg + s.ownerObjectOff);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	void setKillBit(uintptr_t dmg, uint32_t flagsOff, bool set)
	{
		__try
		{
			if (set) *(uint32_t*)(dmg + flagsOff) |= kKillInstantlyBit;
			else *(uint32_t*)(dmg + flagsOff) &= ~kKillInstantlyBit;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
}

class OneShotKill::OneShotKillImpl
{
private:
	GameState mGame;
	int mSlot;
	const Spec* mSpec;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<GetPlayerDatum> getPlayerDatumWeak;
	std::weak_ptr<GetObjectAddress> getObjectAddressWeak;

	std::mutex mMutex;
	std::unique_ptr<ModuleInlineHook> mHook;
	std::atomic<bool> mOn{ false };
	// The build was checked once, with the game's code untouched. Never again: once the hook is attached its site no longer
	// holds the original bytes (and the game's dll cannot change within one run of MCC).
	bool mVerified = false, mVerifyFailed = false;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	// one detour per game (an inline hook carries no user data)
	static inline std::atomic<OneShotKillImpl*> sInstance[kSlots]{};
	static inline std::atomic<ModuleInlineHook*> sHook[kSlots]{};
	static inline std::atomic<int> sInFlight[kSlots]{};

	// Is `victim` up the parent chain of `object` (what it is riding / attached to)? Throws nothing; "cannot tell" = false.
	bool rides(GetObjectAddress& objects, uint32_t object, uint32_t victim)
	{
		for (int i = 0; i < 6 && object != 0xFFFFFFFFu; ++i)
		{
			const uintptr_t addr = objects.getObjectAddress(Datum(object));
			uint32_t parent = 0xFFFFFFFFu;
			if (!addr || !readU32(addr + mSpec->parentOff, parent)) return false;
			if (parent == victim) return true;
			object = parent;
		}
		return false;
	}

	// The game's thread, at the entry of object_cause_damage: should this damage kill this object outright?
	// Anything that cannot be established leaves the damage exactly as the game made it.
	bool shouldKill(uintptr_t dmg, uint32_t victim)
	{
		if (!mOn.load()) return false;
		uint32_t flags = 0, ownerPlayer = 0xFFFFFFFFu, ownerObject = 0xFFFFFFFFu;
		if (!readDamage(dmg, *mSpec, flags, ownerPlayer, ownerObject)) return false;
		if (ownerPlayer == 0xFFFFFFFFu) return false;               // not a player's damage
		if (flags & kKillInstantlyBit) return false;                // the game already made it a kill: not ours to take off
		if (victim == 0xFFFFFFFFu) return false;
		if (ownerObject != 0xFFFFFFFFu && victim == ownerObject) return false;   // never the owner itself
		try
		{
			auto objects = getObjectAddressWeak.lock();
			if (!objects) return false;
			// never a player's unit: HCM's own player ...
			uint32_t localPlayer = 0xFFFFFFFFu;
			if (auto players = getPlayerDatumWeak.lock())
			{
				const Datum p = players->getPlayerDatum();
				if (!p.isNull()) localPlayer = (uint32_t)p;
			}
			if (victim == localPlayer) return false;
			// ... or any unit a player controls (where the game's field for it is known)
			CommonObjectType type = CommonObjectType::FailedTypeConversion;
			const uintptr_t victimAddr = objects->getObjectAddress(Datum(victim), &type);
			if (!victimAddr) return false;
			if (mSpec->unitPlayerOff && (type == CommonObjectType::Biped || type == CommonObjectType::Vehicle))
			{
				uint32_t controller = 0xFFFFFFFFu;
				if (!readU32(victimAddr + mSpec->unitPlayerOff, controller)) return false;
				if (controller != 0xFFFFFFFFu) return false;
			}
			// never what the owner is riding - nor what HCM's player is riding
			if (rides(*objects, ownerObject, victim)) return false;
			if (localPlayer != ownerObject && rides(*objects, localPlayer, victim)) return false;
		}
		catch (HCMRuntimeException&) { return false; }
		catch (const std::exception&) { return false; }
		return true;
	}

	// Halo 1 / 2 / 3 / ODST: (damage data, object index, node, region, material, normal)
	template <int slot> static int64_t __fastcall detourInt(uintptr_t dmg, uint32_t object, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6)
	{
		++sInFlight[slot];
		OneShotKillImpl* const self = sInstance[slot].load();
		const bool kill = self && self->shouldKill(dmg, object);
		if (kill) setKillBit(dmg, kSpecs[slot].flagsOff, true);
		int64_t r = 0;
		if (ModuleInlineHook* const hook = sHook[slot].load())
			r = hook->getInlineHook().call<int64_t, uintptr_t, uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t>(dmg, object, a3, a4, a5, a6);
		if (kill) setKillBit(dmg, kSpecs[slot].flagsOff, false);   // taken off again after that one object
		--sInFlight[slot];
		return r;
	}
	// Reach / Halo 4: (damage data, FLOAT, object index, ...) - seven arguments
	template <int slot> static int64_t __fastcall detourFloat(uintptr_t dmg, float f, uint32_t object, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
	{
		++sInFlight[slot];
		OneShotKillImpl* const self = sInstance[slot].load();
		const bool kill = self && self->shouldKill(dmg, object);
		if (kill) setKillBit(dmg, kSpecs[slot].flagsOff, true);
		int64_t r = 0;
		if (ModuleInlineHook* const hook = sHook[slot].load())
			r = hook->getInlineHook().call<int64_t, uintptr_t, float, uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t>(dmg, f, object, a4, a5, a6, a7);
		if (kill) setKillBit(dmg, kSpecs[slot].flagsOff, false);
		--sInFlight[slot];
		return r;
	}
	static void* detourFor(int slot)
	{
		switch (slot)
		{
		case 0: return (void*)&detourInt<0>;
		case 1: return (void*)&detourInt<1>;
		case 2: return (void*)&detourInt<2>;
		case 3: return (void*)&detourInt<3>;
		case 4: return (void*)&detourFloat<4>;
		default: return (void*)&detourFloat<5>;
		}
	}

	void say(const std::string& text)
	{
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(text); }
		catch (HCMRuntimeException&) {}
	}

	// 0 the game is not loaded (it applies when it is), 1 applied, -1 not this build
	int apply(bool on)
	{
		std::scoped_lock lk(mMutex);
		mOn = on;
		const HMODULE mod = GetModuleHandleW(mSpec->module);
		if (!mod) return 0;
		const uintptr_t base = (uintptr_t)mod;
		if (!mVerified)
		{
			if (mVerifyFailed) return -1;
			const bool ok = bytesAt(base + mSpec->fnRva, mSpec->probe, mSpec->probeLen)
				&& (!mSpec->probe2Len || bytesAt(base + mSpec->probe2Rva, mSpec->probe2, mSpec->probe2Len));
			if (!ok)
			{
				mVerifyFailed = true;
				PLOG_ERROR << "One Shot Kill: " << mGame.toString() << " is not the build this was made for (1.3528) - not applied";
				return -1;
			}
			mVerified = true;
		}
		mHook->setWantsToBeAttached(on);
		PLOG_INFO << "One Shot Kill (" << mGame.toString() << "): " << (on ? "on" : "off");
		return 1;
	}

	void onToggle(bool& on)
	{
		const int r = apply(on);
		if (r < 0) say(std::format("One Shot Kill: this {} build is not supported (made for MCC 1.3528)", mGame.toString()));
		else if (r == 0 && on) say(std::format("One Shot Kill is armed - it applies when {} loads", mGame.toString()));
		else say(on ? "One Shot Kill on - what a player damages dies (never a player or their ride)" : "One Shot Kill off");
	}

	// the game loaded: the hook is only attached once its build has been checked, which needs the module there
	void onMCCStateChanged(const MCCState& s)
	{
		if (s.currentGameState != mGame) return;
		auto settings = settingsWeak.lock();
		if (!settings || !settings->oneShotKillToggle->GetValue()) return;
		apply(true);
	}

public:
	OneShotKillImpl(GameState game, IDIContainer& dicon)
		: mGame(game), mSlot(slotFor(game)), mSpec(mSlot >= 0 ? &kSpecs[mSlot] : nullptr),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		getPlayerDatumWeak(resolveDependentCheat(GetPlayerDatum)),
		getObjectAddressWeak(resolveDependentCheat(GetObjectAddress)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->oneShotKillToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		if (!mSpec) throw HCMInitException("One Shot Kill is not implemented for this game");
		if (sInstance[mSlot].load()) throw HCMInitException(std::format("One Shot Kill already exists for {}", game.toString()));
		auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(mSpec->module), std::vector<int64_t>{ (int64_t)mSpec->fnRva });
		mHook = ModuleInlineHook::make(mSpec->module, ptr, detourFor(mSlot), false);   // attached by apply(), after the build check
		sHook[mSlot] = mHook.get();
		sInstance[mSlot] = this;
		if (dicon.Resolve<SettingsStateAndEvents>().lock()->oneShotKillToggle->GetValue()) apply(true);
	}

	~OneShotKillImpl()
	{
		mOn = false;
		if (mHook) mHook->setWantsToBeAttached(false);          // no new calls come through the detour
		if (mSlot >= 0)
		{
			sInstance[mSlot] = nullptr;
			for (int i = 0; i < 500 && sInFlight[mSlot].load() > 0; ++i) Sleep(1);   // a call still inside the detour finishes first
			sHook[mSlot] = nullptr;
		}
	}
};

OneShotKill::OneShotKill(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<OneShotKillImpl>(gameImpl, dicon))
{
}
OneShotKill::~OneShotKill() { PLOG_DEBUG << "~" << getName(); }
