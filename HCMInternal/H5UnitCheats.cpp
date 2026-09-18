#include "pch.h"
#include "H5UnitCheats.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5UnitCheats.h for why these are malleable properties and not the player-traits struct.

namespace
{
	// ---- the two per-unit property structs, both on the player biped ----------------------------------
	// Built by the unit constructor exe+0x027F8600: damage at 0x027F8660, weapon at 0x027F8676.
	constexpr uintptr_t kUnitDamageProperties = 0x0E1C;   // c_unit_damage_properties
	constexpr uintptr_t kUnitWeaponProperties = 0x0EAC;   // c_unit_weapon_properties

	// FLOAT slot: { float value; u32 string_id; float override; u8 hasOverride; } padded to 0x10.
	// Slot order read off the ctor at exe+0x02972100.
	constexpr uintptr_t kDmgSlotMelee   = 0x00;
	constexpr uintptr_t kDmgSlotGrenade = 0x30;
	constexpr uintptr_t kDmgSlotWeapon  = 0x50;
	constexpr uintptr_t kFloatSlotValue = 0x00, kFloatSlotId = 0x04, kFloatSlotOverride = 0x08, kFloatSlotHas = 0x0C;

	// BYTE slot: { u8 value; u32 string_id; u8 override; u8 hasOverride; } stride 0x0C.
	// ⚠ NOT the same shape as the float slots - override sits at +0x08 and the flag at +0x09, packed, which is
	// exactly what the accessor at exe+0x028F7890 reads: cmp byte[+0x59] / movzx [+0x58] / else movzx [+0x50].
	constexpr uintptr_t kWpnSlotInfiniteAmmo   = 0x50;
	constexpr uintptr_t kWpnSlotBottomlessClip = 0x5C;
	constexpr uintptr_t kByteSlotValue = 0x00, kByteSlotId = 0x04, kByteSlotOverride = 0x08, kByteSlotHas = 0x09;

	// The engine's own string_ids, for a sanity check before we write. Cached at exe+0x05EB4CB4 / 0x05EB4DD4 /
	// 0x05EB4DD8 but hardcoded here because the VALUE is what we are validating against.
	constexpr uint32_t kIdWeaponDamageScalar  = 0xFDCEB810;
	constexpr uint32_t kIdInfiniteAmmo        = 0xEA846593;
	constexpr uint32_t kIdBottomlessClip      = 0xC05E8567;

	// ---- GRENADES ------------------------------------------------------------------------------------
	// The unit's grenade inventory. unit_get_total_grenade_count's implementation is literally
	// `lea rax,[rcx+0x518]` followed by a loop summing 8 bytes, so this is the live count array, indexed by
	// grenade type. Three types are real; bytes 3..7 exist and the net-state applier forces them to 0.
	constexpr uintptr_t kUnitGrenadeCounts = 0x0518;
	constexpr int kGrenadeTypeCount = 3;                  // frag, plasma, forerunner/splinter

	// The max_*_grenade_count properties live in that SAME inventory sub-object, as byte-property slots at
	// inv + 0x14 + 0x0C*i. Same {u8 value; u32 id; u8 override; u8 hasOverride} shape as the weapon block.
	constexpr uintptr_t kUnitGrenadeMaxSlot0 = 0x052C;    // +0x0C per type: 0x52C, 0x538, 0x544
	constexpr uintptr_t kGrenadeMaxSlotStride = 0x0C;
	constexpr uint32_t kIdMaxGrenade[kGrenadeTypeCount] = {
		0x8E5C305F,   // max_frag_grenade_count
		0x77C3D3EB,   // max_plasma_grenade_count
		0xF65644D8,   // max_forerunner_grenade_count
	};

	// ⚠⚠ THE COUNT BYTE IS READ AS SIGNED. The throw gate is `test al,al` + `jle`, and both the HUD exporter
	// and unit_get_grenade_count sign-extend it with movsx. So anything >= 0x80 reads as "no grenades" and
	// shows NEGATIVE on the HUD - writing 0xFF to mean "lots" would disable grenades entirely. 127 is the
	// ceiling, and we do not go near it anyway (see below).
	constexpr uint8_t kGrenadeCountCeiling = 127;

	bool sehRead(const void* src, void* dst, size_t n)
	{
		__try { memcpy(dst, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehWrite(void* dst, const void* src, size_t n)
	{
		__try { memcpy(dst, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// ⚠ VALIDATE THE SLOT BEFORE WRITING IT. Every slot carries the property's own string_id, so we can prove we
	// are looking at the struct we think we are rather than at whatever else happens to live at that offset in a
	// build we have not seen. If the id does not match we refuse rather than write - a wrong write here would be
	// into a live object.
	bool slotIdMatches(uintptr_t slotAddr, uintptr_t idOffset, uint32_t expected)
	{
		uint32_t id = 0;
		if (!sehRead((const void*)(slotAddr + idOffset), &id, sizeof(id))) return false;
		return id == expected;
	}
}


// ================================================================================================================
// INFINITE AMMO / BOTTOMLESS CLIP
// ================================================================================================================
class H5InfiniteAmmo::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::atomic<bool> mReady{ false };

	std::unique_ptr<ScopedCallback<RenderEvent>> mRenderCallback;

	// What we found before touching anything, so it can be put back. Keyed on the object we read it from:
	// a respawn makes a NEW biped at a NEW address, and restoring a dead object's bytes into a live one would
	// be writing values that never belonged to it.
	struct Saved { uint8_t value, over, has; };
	uintptr_t mAppliedTo = 0;
	Saved mSavedAmmo{}, mSavedClip{};
	bool mHaveSaved = false;

	std::chrono::steady_clock::time_point mLastFailureLog{};

	void logThrottled(const char* what)
	{
		const auto now = std::chrono::steady_clock::now();
		if (mLastFailureLog.time_since_epoch().count() != 0 && (now - mLastFailureLog) < std::chrono::seconds(5))
			return;
		mLastFailureLog = now;
		PLOG_ERROR << "H5InfiniteAmmo: " << what;
	}

	static bool readSlot(uintptr_t slot, Saved& out)
	{
		return sehRead((const void*)(slot + kByteSlotValue), &out.value, 1)
			&& sehRead((const void*)(slot + kByteSlotOverride), &out.over, 1)
			&& sehRead((const void*)(slot + kByteSlotHas), &out.has, 1);
	}

	// Write the override AND the flag. Writing only the base would be undone by anything that re-derives it;
	// the override wins over the base by the accessor's own logic.
	static bool writeSlot(uintptr_t slot, uint8_t value)
	{
		const uint8_t one = 1;
		return sehWrite((void*)(slot + kByteSlotOverride), &value, 1)
			&& sehWrite((void*)(slot + kByteSlotHas), &one, 1);
	}

	static bool restoreSlot(uintptr_t slot, const Saved& s)
	{
		return sehWrite((void*)(slot + kByteSlotValue), &s.value, 1)
			&& sehWrite((void*)(slot + kByteSlotOverride), &s.over, 1)
			&& sehWrite((void*)(slot + kByteSlotHas), &s.has, 1);
	}

	void restoreIfApplied()
	{
		if (!mHaveSaved || !mAppliedTo) { mHaveSaved = false; mAppliedTo = 0; return; }
		const uintptr_t props = mAppliedTo + kUnitWeaponProperties;
		restoreSlot(props + kWpnSlotInfiniteAmmo, mSavedAmmo);
		restoreSlot(props + kWpnSlotBottomlessClip, mSavedClip);
		// ⚠ Grenade counts are deliberately NOT restored. They are consumable inventory, not a mode flag -
		// there is no "original" to put back once the player has thrown some, and handing them a remembered
		// count from thirty seconds ago would be worse than leaving what they have.
		mHaveSaved = false;
		mAppliedTo = 0;
	}

	// ⚠ REFILL TO THE UNIT'S OWN MAX, NOT TO A BIG NUMBER. Writing 99 would work mechanically but reads as
	// nonsense on the HUD, and the count is SIGNED so a careless large value is actively harmful. Topping up
	// to the engine's own resolved cap gives genuinely unlimited grenades while the HUD keeps showing the
	// normal number, which is what the game itself does when you walk over a resupply.
	//
	// ⚠ THE MAX PROPERTY IS WRITE-ONLY DATA - nothing in the image reads it back (three independent searches
	// found zero readers), so raising it would refill nothing. It is useful to us only as a value to READ.
	void topUpGrenades(uintptr_t object)
	{
		const uintptr_t counts = object + kUnitGrenadeCounts;
		for (int i = 0; i < kGrenadeTypeCount; ++i)
		{
			const uintptr_t maxSlot = object + kUnitGrenadeMaxSlot0 + (uintptr_t)i * kGrenadeMaxSlotStride;
			if (!slotIdMatches(maxSlot, kByteSlotId, kIdMaxGrenade[i]))
				continue;   // not the struct we think it is - leave this type alone rather than guess

			uint8_t maxValue = 0, maxOver = 0, maxHas = 0;
			if (!sehRead((const void*)(maxSlot + kByteSlotValue), &maxValue, 1)) continue;
			if (!sehRead((const void*)(maxSlot + kByteSlotOverride), &maxOver, 1)) continue;
			if (!sehRead((const void*)(maxSlot + kByteSlotHas), &maxHas, 1)) continue;

			// Same accessor rule the engine uses everywhere: hasOverride ? override : value.
			uint8_t cap = maxHas ? maxOver : maxValue;
			if (cap == 0) continue;                       // this unit is not meant to carry this type
			if (cap > kGrenadeCountCeiling) cap = kGrenadeCountCeiling;

			uint8_t current = 0;
			if (!sehRead((const void*)(counts + i), &current, 1)) continue;
			// Only write when it is actually short. A blind write every frame would fight the throw path's
			// decrement in the same tick and could swallow the throw.
			if (current < cap)
				sehWrite((void*)(counts + i), &cap, 1);
		}
	}

	void onFrame()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			const bool wantAmmo = settings->h5InfiniteAmmoToggle->GetValue();
			const bool wantClip = settings->h5BottomlessClipToggle->GetValue();
			if (!wantAmmo && !wantClip) { restoreIfApplied(); return; }

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);

			// ⚠ RE-RESOLVE EVERY FRAME. Respawn replaces the object, and its constructor zeroes these bytes.
			const uintptr_t object = playerState->getPlayerObject();
			if (!object) return;

			if (object != mAppliedTo)
			{
				// New unit (first apply, or a respawn). The old object's saved bytes are meaningless now.
				mHaveSaved = false;
				mAppliedTo = 0;
			}

			const uintptr_t props = object + kUnitWeaponProperties;
			const uintptr_t slotAmmo = props + kWpnSlotInfiniteAmmo;
			const uintptr_t slotClip = props + kWpnSlotBottomlessClip;

			if (!slotIdMatches(slotAmmo, kByteSlotId, kIdInfiniteAmmo)
				|| !slotIdMatches(slotClip, kByteSlotId, kIdBottomlessClip))
			{
				logThrottled("property ids did not match - refusing to write (has the game updated?)");
				return;
			}

			if (!mHaveSaved)
			{
				if (!readSlot(slotAmmo, mSavedAmmo) || !readSlot(slotClip, mSavedClip))
				{
					logThrottled("could not read the weapon properties");
					return;
				}
				mHaveSaved = true;
				mAppliedTo = object;
			}

			// Bottomless clip is the stronger of the two (the magazine stops draining as well), so it wins when
			// both are on rather than the two fighting each other frame to frame.
			writeSlot(slotAmmo, wantAmmo ? 1u : 0u);
			writeSlot(slotClip, wantClip ? 1u : 0u);

			topUpGrenades(object);
		}
		catch (HCMRuntimeException&)
		{
			// Expected at a menu, during a load, or while dead. Not an error, and not worth a log line a frame.
		}
	}

	void onToggleChanged(bool&)
	{
		try
		{
			lockOrThrow(settingsWeak, settings);
			const bool want = settings->h5InfiniteAmmoToggle->GetValue()
				|| settings->h5BottomlessClipToggle->GetValue();

			if (want && !mRenderCallback)
			{
				mRenderCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onFrame(); });
			}
			else if (!want && mRenderCallback)
			{
				// Put the unit back before we stop running, or the override stays latched on for the session.
				restoreIfApplied();
				mRenderCallback.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling Infinite Ammo: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mAmmoToggleCallback;
	ScopedCallback<ToggleEvent> mClipToggleCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEvent(dicon.Resolve<RenderEvent>().lock()),
		mAmmoToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5InfiniteAmmoToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); }),
		mClipToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5BottomlessClipToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5InfiniteAmmo only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		// ⚠ HCM stays resident across sessions; leaving the override latched would persist into the next one.
		try { restoreIfApplied(); } catch (...) {}
		mRenderCallback.reset();
		mAmmoToggleCallback.removeCallback();
		mClipToggleCallback.removeCallback();
	}
};


// ================================================================================================================
// ONE SHOT KILL
// ================================================================================================================
class H5OneShotKill::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::atomic<bool> mReady{ false };

	std::unique_ptr<ScopedCallback<RenderEvent>> mRenderCallback;

	struct SavedF { float value, over; uint8_t has; };
	uintptr_t mAppliedTo = 0;
	SavedF mSavedWeapon{}, mSavedMelee{}, mSavedGrenade{};
	bool mHaveSaved = false;
	std::chrono::steady_clock::time_point mLastFailureLog{};

	void logThrottled(const char* what)
	{
		const auto now = std::chrono::steady_clock::now();
		if (mLastFailureLog.time_since_epoch().count() != 0 && (now - mLastFailureLog) < std::chrono::seconds(5))
			return;
		mLastFailureLog = now;
		PLOG_ERROR << "H5OneShotKill: " << what;
	}

	static bool readSlotF(uintptr_t slot, SavedF& out)
	{
		return sehRead((const void*)(slot + kFloatSlotValue), &out.value, sizeof(float))
			&& sehRead((const void*)(slot + kFloatSlotOverride), &out.over, sizeof(float))
			&& sehRead((const void*)(slot + kFloatSlotHas), &out.has, 1);
	}

	static bool writeSlotF(uintptr_t slot, float value)
	{
		const uint8_t one = 1;
		return sehWrite((void*)(slot + kFloatSlotOverride), &value, sizeof(float))
			&& sehWrite((void*)(slot + kFloatSlotHas), &one, 1);
	}

	static bool restoreSlotF(uintptr_t slot, const SavedF& s)
	{
		return sehWrite((void*)(slot + kFloatSlotValue), &s.value, sizeof(float))
			&& sehWrite((void*)(slot + kFloatSlotOverride), &s.over, sizeof(float))
			&& sehWrite((void*)(slot + kFloatSlotHas), &s.has, 1);
	}

	void restoreIfApplied()
	{
		if (!mHaveSaved || !mAppliedTo) { mHaveSaved = false; mAppliedTo = 0; return; }
		const uintptr_t props = mAppliedTo + kUnitDamageProperties;
		restoreSlotF(props + kDmgSlotWeapon, mSavedWeapon);
		restoreSlotF(props + kDmgSlotMelee, mSavedMelee);
		restoreSlotF(props + kDmgSlotGrenade, mSavedGrenade);
		mHaveSaved = false;
		mAppliedTo = 0;
	}

	void onFrame()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			if (!settings->h5OneShotKillToggle->GetValue()) { restoreIfApplied(); return; }
			const float mult = settings->h5OneShotKillMultiplier->GetValue();

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);

			const uintptr_t object = playerState->getPlayerObject();
			if (!object) return;
			if (object != mAppliedTo) { mHaveSaved = false; mAppliedTo = 0; }

			const uintptr_t props = object + kUnitDamageProperties;
			const uintptr_t slotW = props + kDmgSlotWeapon;

			// Only the weapon slot's id is checked: it is the one whose offset the whole struct layout was
			// derived from, so if it matches, the sibling slots at fixed strides are the ones we expect.
			if (!slotIdMatches(slotW, kFloatSlotId, kIdWeaponDamageScalar))
			{
				logThrottled("weapon_damage_scalar id did not match - refusing to write (has the game updated?)");
				return;
			}

			if (!mHaveSaved)
			{
				if (!readSlotF(slotW, mSavedWeapon)
					|| !readSlotF(props + kDmgSlotMelee, mSavedMelee)
					|| !readSlotF(props + kDmgSlotGrenade, mSavedGrenade))
				{
					logThrottled("could not read the damage properties");
					return;
				}
				mHaveSaved = true;
				mAppliedTo = object;
			}

			// ⚠ ALL THREE, NOT JUST THE WEAPON ONE. They are independent multipliers - scaling only the weapon
			// scalar leaves melee and grenades at their stock values, which is not "one tap anything".
			// ⚠ SET, DO NOT SCALE. Live, melee ships at 1.15 and grenade at 0.0 on the player biped, so
			// multiplying what is there would give three different results and leave grenades doing nothing.
			writeSlotF(slotW, mult);
			writeSlotF(props + kDmgSlotMelee, mult);
			writeSlotF(props + kDmgSlotGrenade, mult);
		}
		catch (HCMRuntimeException&)
		{
			// Menu / load / dead. Expected.
		}
	}

	void onToggleChanged(bool& newValue)
	{
		try
		{
			if (newValue && !mRenderCallback)
			{
				mRenderCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onFrame(); });
			}
			else if (!newValue && mRenderCallback)
			{
				restoreIfApplied();
				mRenderCallback.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling One Shot Kill: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEvent(dicon.Resolve<RenderEvent>().lock()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5OneShotKillToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5OneShotKill only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		try { restoreIfApplied(); } catch (...) {}
		mRenderCallback.reset();
		mToggleCallback.removeCallback();
	}
};


H5InfiniteAmmo::H5InfiniteAmmo(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5InfiniteAmmo::~H5InfiniteAmmo() { PLOG_VERBOSE << "~" << getName(); }

H5OneShotKill::H5OneShotKill(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5OneShotKill::~H5OneShotKill() { PLOG_VERBOSE << "~" << getName(); }
