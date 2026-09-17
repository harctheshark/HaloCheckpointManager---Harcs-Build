#include "pch.h"
#include "H5TriggerActivity.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "RuntimeExceptionHandler.h"
#include <array>
#include <atomic>

// See H5TriggerActivity.h for where these addresses came from and why the register differs per function.

namespace
{
	// Upper bound on the scenario trigger-volume block. The largest shipped level has 379; this is a hard
	// bound for the array, and anything outside it is dropped rather than clamped - a clamp would attribute
	// a bogus index to a real volume and light up the wrong one.
	constexpr int kMaxVolumes = 2048;

	// ---- the hooked script bindings -------------------------------------------------------------------
	// ⚠ Register per function - verified individually, see the header. Do not assume ECX.
	constexpr uintptr_t kRvaTestPlayers          = 0x00A855C0;   // ECX
	constexpr uintptr_t kRvaTestPlayersAll       = 0x00A85650;   // ECX
	constexpr uintptr_t kRvaTestObject           = 0x00A855B0;   // ECX
	constexpr uintptr_t kRvaTestObjects          = 0x00892150;   // ECX
	constexpr uintptr_t kRvaTestObjectsAll       = 0x008920B0;   // ECX
	constexpr uintptr_t kRvaTestPlayerLookat     = 0x00A84BA0;   // ECX
	constexpr uintptr_t kRvaTestPlayersLookat    = 0x00A84C10;   // ECX
	constexpr uintptr_t kRvaTestPlayersAllLookat = 0x00A84A70;   // ECX
	constexpr uintptr_t kRvaAiGetAllInVolume     = 0x0113A840;   // EDX

	// GetTickCount64 rather than steady_clock: this runs on the game thread inside a script evaluation, at
	// whatever rate the mission polls, so it has to be as close to free as possible. 0 means "never tested",
	// which is why the API returns -1 for that rather than a huge age.
	std::array<std::atomic<uint64_t>, kMaxVolumes> gLastTested{};

	inline void stamp(int index) noexcept
	{
		if (index < 0 || index >= kMaxVolumes) return;
		gLastTested[(size_t)index].store(GetTickCount64(), std::memory_order_relaxed);
	}

	// ⚠ These run on the GAME THREAD, potentially many times per tick. Do nothing but a bounds check and a
	// relaxed store - no locks, no allocation, no logging, no exceptions.
	void hookEcx(SafetyHookContext& ctx) { stamp((int)(int32_t)(ctx.rcx & 0xFFFFFFFFull)); }
	void hookEdx(SafetyHookContext& ctx) { stamp((int)(int32_t)(ctx.rdx & 0xFFFFFFFFull)); }
}

class H5TriggerActivity::Impl
{
private:
	GameState mGame;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::vector<std::unique_ptr<ModuleMidHook>> mHooks;
	bool mEnabled = false;

	void addHook(uintptr_t rva, safetyhook::MidHookFn fn)
	{
		auto target = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(
			GameState(mGame).toModuleName(), std::vector<int64_t>{ (int64_t)rva });
		mHooks.push_back(ModuleMidHook::make(GameState(mGame).toModuleName(), target, fn, false));
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game), runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5TriggerActivity only supports Halo 5: Forge");

		// ⚠ Constructed but NOT attached (startEnabled=false) - the constructor must not touch game memory.
		addHook(kRvaTestPlayers, hookEcx);
		addHook(kRvaTestPlayersAll, hookEcx);
		addHook(kRvaTestObject, hookEcx);
		addHook(kRvaTestObjects, hookEcx);
		addHook(kRvaTestObjectsAll, hookEcx);
		addHook(kRvaTestPlayerLookat, hookEcx);
		addHook(kRvaTestPlayersLookat, hookEcx);
		addHook(kRvaTestPlayersAllLookat, hookEcx);
		addHook(kRvaAiGetAllInVolume, hookEdx);
	}

	~Impl()
	{
		try { setEnabled(false); }
		catch (...) {}
		mHooks.clear();
	}

	void setEnabled(bool enable)
	{
		if (enable == mEnabled) return;
		for (auto& h : mHooks)
			if (h) h->setWantsToBeAttached(enable);
		mEnabled = enable;

		if (!enable)
		{
			// Forget history on disable, so re-enabling cannot show stale "live" volumes from minutes ago.
			for (auto& v : gLastTested) v.store(0, std::memory_order_relaxed);
		}
		PLOG_DEBUG << "H5TriggerActivity hooks " << (enable ? "attached" : "detached");
	}

	bool isActive(int index, uint32_t windowMs) const noexcept
	{
		const int64_t age = millisecondsSinceTested(index);
		return age >= 0 && age <= (int64_t)windowMs;
	}

	int64_t millisecondsSinceTested(int index) const noexcept
	{
		if (index < 0 || index >= kMaxVolumes) return -1;
		const uint64_t last = gLastTested[(size_t)index].load(std::memory_order_relaxed);
		if (last == 0) return -1;
		const uint64_t now = GetTickCount64();
		return now >= last ? (int64_t)(now - last) : 0;
	}

	int everTestedCount() const noexcept
	{
		int n = 0;
		for (const auto& v : gLastTested)
			if (v.load(std::memory_order_relaxed) != 0) ++n;
		return n;
	}
};


H5TriggerActivity::H5TriggerActivity(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5TriggerActivity::~H5TriggerActivity() { PLOG_VERBOSE << "~" << getName(); }

void H5TriggerActivity::setEnabled(bool enable) { pimpl->setEnabled(enable); }
bool H5TriggerActivity::isActive(int volumeIndex, uint32_t windowMs) const noexcept
{
	return pimpl->isActive(volumeIndex, windowMs);
}
int64_t H5TriggerActivity::millisecondsSinceTested(int volumeIndex) const noexcept
{
	return pimpl->millisecondsSinceTested(volumeIndex);
}
int H5TriggerActivity::everTestedCount() const noexcept { return pimpl->everTestedCount(); }
