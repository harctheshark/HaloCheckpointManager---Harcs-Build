#include "pch.h"
#include "MetroActorFlags.h"
#include "MetroEngine.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include <thread>

// 2026-11-25 (user: "when invincibility is on, make it so i cant die from Not wearing a Gas Mask, and i dont even want the
// sounds playing of me choking/gasping for air. set the bit for 'Gas Mask on', make sure theres no time limit on gas
// filters"). Static RE: Documents\Halo Mod And Tools\Metro Redux\binary_survey\agent_gas\ (IDB g.i64). While Invincibility
// is on, on top of the g_god bit:
//   * ps_actor_flags bit 0x80 (g_unlimitedfilters): the gas-mask tick 0x3290C0 skips the filter countdown, the low-filter
//     sound and the breathing sounds (test byte [F2E0F0],80h at 0x3291CC);
//   * the mask's filter time (gm+1100) is topped up to its maximum (gm+1108) as a safety net;
//   * the mask is put on once (state byte gm+1624 = 3, "worn": what the "gasmask" trigger the levels' toxic-air scripts
//     test reads) - once per mask, so taking it off yourself afterwards is not fought;
//   * the player's own invulnerable flag (player+1810 bit 0x80): the hit entry 0x2413B0 drops ordinary hits before the
//     hit reaction (the choking / hurt sounds) - restored when Invincibility goes off;
//   * the engine's health floor (player+1048 = 1.0, player+1056 bit 1): a hit that still gets through leaves you alive;
//     health is kept topped up to the most it has been this level, so the low-health breathing does not start.
// gm = 0x1E5670(player) (a plain read of the inventory's slot 4 item); 0 when the level has no mask.
namespace
{
	constexpr uint32_t kGodBit = 1;
	constexpr uint32_t kUnlimitedAmmoBit = 8;
	constexpr uint32_t kUnlimitedFiltersBit = 0x80;
	constexpr uint32_t kOurBits = kGodBit | kUnlimitedAmmoBit | kUnlimitedFiltersBit;

	constexpr uintptr_t kGetGasMaskRVA = 0x1E5670;
	constexpr uintptr_t kPlayerVsFlagsOff = 1810;      // byte; 0x80 = invulnerable, 0x40 = immortal
	constexpr uintptr_t kPlayerHealthFloorOff = 1048;  // float, -1 = none
	constexpr uintptr_t kPlayerFloorFlagsOff = 1056;   // dword; 1 = the floor holds for every hit
	constexpr uintptr_t kMaskFilterTimeOff = 1100;     // float seconds left
	constexpr uintptr_t kMaskFilterMaxOff = 1108;      // float seconds
	constexpr uintptr_t kMaskStateOff = 1624;          // byte; 3 = worn

	using getmask_t = uintptr_t(__fastcall*)(uintptr_t player);
	uintptr_t sehGetMask(uintptr_t player)
	{
		__try { return ((getmask_t)(metro::base() + kGetGasMaskRVA))(player); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
}

class MetroActorFlags::Impl
{
private:
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::atomic<bool> mRun{ true };
	std::thread mThread;
	std::mutex mMutex;   // one writer at a time (the toggles' threads + the re-apply thread)

	// what we changed on the current player, to put back
	uintptr_t mPlayer = 0;
	bool mVsBitWasSet = false;
	bool mTouchedPlayer = false;
	float mFloorWas = -1.f;
	uint32_t mFloorFlagsWas = 0;
	float mHealthMax = 0.f;
	uintptr_t mMaskPutOn = 0;    // the mask we put on (once per mask)

	bool invuln() { auto s = settingsWeak.lock(); return s && s->invulnerabilityToggle->GetValue(); }

	uint32_t wanted()
	{
		auto s = settingsWeak.lock();
		if (!s) return 0;
		return (s->invulnerabilityToggle->GetValue() ? (kGodBit | kUnlimitedFiltersBit) : 0) | (s->infiniteAmmoToggle->GetValue() ? kUnlimitedAmmoBit : 0);
	}

	// set the bits we want, clear the ones of ours we do not; never touch the others
	bool applyFlags(uint32_t want)
	{
		const uintptr_t at = metro::base() + metro::kGodFlagsRVA;
		uint32_t v = 0;
		if (!metro::read(at, v)) return false;
		const uint32_t nv = (v & ~kOurBits) | want;
		return nv == v || metro::write(at, nv);
	}

	void restorePlayer()
	{
		if (!mTouchedPlayer) return;
		mTouchedPlayer = false;
		if (!mPlayer || mPlayer != metro::player()) { mPlayer = 0; return; }   // a new level: the old entity is gone
		uint8_t vs = 0;
		if (!mVsBitWasSet && metro::read(mPlayer + kPlayerVsFlagsOff, vs)) metro::write<uint8_t>(mPlayer + kPlayerVsFlagsOff, vs & ~0x80);
		metro::write(mPlayer + kPlayerHealthFloorOff, mFloorWas);
		metro::write(mPlayer + kPlayerFloorFlagsOff, mFloorFlagsWas);
	}

	void applyPlayer()
	{
		const uintptr_t pl = metro::player();
		if (!pl) return;
		if (pl != mPlayer) { mTouchedPlayer = false; mPlayer = pl; mHealthMax = 0.f; }
		if (!mTouchedPlayer)
		{
			uint8_t vs = 0; float floor = -1.f; uint32_t ff = 0;
			if (!metro::read(pl + kPlayerVsFlagsOff, vs) || !metro::read(pl + kPlayerHealthFloorOff, floor) || !metro::read(pl + kPlayerFloorFlagsOff, ff)) return;
			mVsBitWasSet = (vs & 0x80) != 0; mFloorWas = floor; mFloorFlagsWas = ff;
			mTouchedPlayer = true;
		}
		uint8_t vs = 0;
		if (metro::read(pl + kPlayerVsFlagsOff, vs) && !(vs & 0x80)) metro::write<uint8_t>(pl + kPlayerVsFlagsOff, vs | 0x80);
		metro::write(pl + kPlayerHealthFloorOff, 1.0f);
		uint32_t ff = 0;
		if (metro::read(pl + kPlayerFloorFlagsOff, ff) && !(ff & 1)) metro::write(pl + kPlayerFloorFlagsOff, ff | 1);
		float health = 0.f;
		if (metro::read(pl + metro::kPlayerHealthOff, health) && std::isfinite(health) && health > 0.f)
		{
			if (health > mHealthMax) mHealthMax = health;
			else if (health < mHealthMax) metro::write(pl + metro::kPlayerHealthOff, mHealthMax);
		}

		const uintptr_t gm = sehGetMask(pl);
		if (!metro::okPtr(gm)) return;
		float t = 0.f, tmax = 0.f;
		if (metro::read(gm + kMaskFilterTimeOff, t) && metro::read(gm + kMaskFilterMaxOff, tmax) && std::isfinite(tmax) && tmax > 0.f && tmax <= 3000.f && t < tmax)
			metro::write(gm + kMaskFilterTimeOff, tmax);
		if (gm != mMaskPutOn)
		{
			mMaskPutOn = gm;
			uint8_t st = 0;
			if (metro::read(gm + kMaskStateOff, st) && st != 3 && st != 9) metro::write<uint8_t>(gm + kMaskStateOff, 3);
		}
	}

	void tick()
	{
		std::scoped_lock lk(mMutex);
		const uint32_t w = wanted();
		if (w) applyFlags(w);   // only while something is on: off = leave the word to the game
		if (invuln()) applyPlayer();
	}

	void onToggle(const char* what, bool on)
	{
		bool ok;
		{
			std::scoped_lock lk(mMutex);
			ok = applyFlags(wanted());
			if (!invuln()) { restorePlayer(); mMaskPutOn = 0; }
		}
		if (on) tick();
		if (auto m = messagesWeak.lock()) m->addMessage(ok ? std::format("{} {}", what, on ? "on" : "off") : std::format("{}: could not write the game's flags", what));
	}

	ScopedCallback<ToggleEvent> mInvulnCallback;
	ScopedCallback<ToggleEvent> mAmmoCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		mInvulnCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->invulnerabilityToggle->valueChangedEvent, [this](bool& on) { onToggle("Invincibility", on); }),
		mAmmoCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->infiniteAmmoToggle->valueChangedEvent, [this](bool& on) { onToggle("Infinite Ammo", on); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroActorFlags is Metro Redux only");
		mThread = std::thread([this]() {
			while (mRun.load())
			{
				tick();
				for (int i = 0; i < 5 && mRun.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		});
	}

	~Impl()
	{
		mRun = false;
		if (mThread.joinable()) mThread.join();
		std::scoped_lock lk(mMutex);
		applyFlags(0);   // HCM closing: the game keeps no cheat bits of ours
		restorePlayer();
	}
};

MetroActorFlags::MetroActorFlags(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroActorFlags::~MetroActorFlags() { PLOG_VERBOSE << "~" << getName(); }
