#include "pch.h"
#include "MasterTickrate.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "IMakeOrGetCheat.h"
#include "GameTickEventHook.h"
#include "ScopedThreadSuspender.h"
#include <Windows.h>
#include <atomic>

// The master tickrate lives in the "game time globals" struct ( *(halo2.dll+0x15FE008) ): an int16 tickrate
// at +0x02 and a seconds-per-tick float at +0x04 (set together). 343's build runs at 60; the flip button
// toggles these two fields between 60/(1/60) and 30/(1/30).
//
// BOTH the flip button and the "Custom Tickrate (Hz)" input do the full job: as well as writing the tickrate
// int16 + dt, they match the collision "tickrate scalar" and force a live collision rebuild so the change is
// felt immediately (the button picks 30/60, the input picks an arbitrary rate):
//
//   * SCALAR: halo2.dll+0x70DBFA is `mulss xmm6,[rip->flt_180C32AA0]` in the Havok collision-shape builder -
//     the ×30.0 that 343 hardcoded (Season 7 read the live tickrate here). We repoint that rip-disp to a float
//     WE own (a page allocated near the module so rip+disp32 reaches it) and write (float)rate into it, so newly
//     built shapes bake radius × rate. See memory halo2dll-tickrate-scalar / Season7Physics_BuildTimeCache_HANDOFF.
//   * INSTANT: the scalar is baked into each shape at construction and cached (obj collision @ +0xB4), never
//     re-read per tick, so existing shapes keep the old value. The game's own per-object "recreate collision"
//     routine sub_180705B20 (RVA 0x705B20: remove body from Havok world -> tear down shape -> rebuild -> re-add)
//     rebuilds one object with the current scalar. We walk the object header table ourselves (ptr @ 0x18B7398,
//     capacity 2048, 12-byte entries; game accessor sub_1808D7000 @ 0x8D7000 returns 0 for empty slots) and call
//     it for every live object with a collision body, on the game thread (GameTickEventHook), so the new scalar
//     takes effect without a level reload. (Self-contained: ObjectTableRange isn't wired for Halo2.)
//
// Season7Physics patches the same 0x70DBFA instruction with a fixed 60.0; the custom scalar takes precedence
// (SettingsStateAndEvents::customTickrateScalarActive) and Season7Physics stands down while it's set.
namespace
{
	// Repoints the collision-radius scalar `mulss xmm6,[rip+disp32]` at halo2.dll+0x70DBFA to read a float we own,
	// letting us set the scalar to any value by writing that float. allocNear keeps the float within rip+disp32
	// reach of the site. Self-contained raw-Win32, gated on the v1.3528 opcode - mirrors UncapDropShadows.
	class TickrateScalarPatcher
	{
		static constexpr uint32_t kSiteRVA = 0x70DBFA;                       // mulss xmm6,[rip+disp32]
		static constexpr uint8_t  kOpcode[4] = { 0xF3, 0x0F, 0x59, 0x35 };   // build gate (opcode + modrm)
		static constexpr uint8_t  kDefaultDisp[4] = { 0x9E, 0x4E, 0x52, 0x00 }; // -> flt_180C32AA0 (30.0), stock

		uintptr_t base = 0;
		float* scalarSlot = nullptr; // our near float; the mulss reads this once we repoint
		bool applied = false;

		static void* allocNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO si; GetSystemInfo(&si);
			uintptr_t gran = si.dwAllocationGranularity;
			uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL;
			uintptr_t hi = target + 0x70000000ULL;
			uintptr_t startp = target & ~(gran - 1);
			for (uintptr_t p = startp; p > lo; p -= gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
			for (uintptr_t p = startp + gran; p < hi; p += gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
			return nullptr;
		}

		// write to executable .text with other threads suspended (avoid a torn/half-written disp being executed)
		static void writeText(uintptr_t addr, const void* data, size_t len)
		{
			ScopedThreadSuspender suspend;
			DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, len);
			VirtualProtect((void*)addr, len, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
		}

	public:
		bool isApplied() const { return applied; }
		uintptr_t moduleBase() const { return base; }

		// false (no throw) if halo2.dll isn't loaded yet; throws if the site doesn't match build 1.3528.
		bool apply()
		{
			if (applied) return true;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;

			uintptr_t site = base + kSiteRVA;
			if (memcmp((void*)site, kOpcode, sizeof(kOpcode)) != 0)
				throw HCMRuntimeException("Tickrate scalar: opcode at halo2.dll+0x70DBFA doesn't match build 1.3528; refusing to patch.");

			scalarSlot = (float*)allocNear(base, 0x1000);
			if (!scalarSlot)
				throw HCMRuntimeException("Tickrate scalar: failed to allocate a near page for the scalar float.");
			*scalarSlot = 30.0f; // sane value until the first setScalar

			int64_t rel = (int64_t)(uintptr_t)scalarSlot - (int64_t)(site + 8); // rip = end of the 8-byte insn
			if (rel > INT32_MAX || rel < INT32_MIN)
			{
				VirtualFree(scalarSlot, 0, MEM_RELEASE); scalarSlot = nullptr;
				throw HCMRuntimeException("Tickrate scalar: near page out of rip+disp32 range (unexpected).");
			}
			int32_t disp = (int32_t)rel;
			writeText(site + 4, &disp, sizeof(disp)); // rewrite only the disp32 (bytes 4..7)
			applied = true;
			return true;
		}

		void setScalar(float f) { if (scalarSlot) *scalarSlot = f; } // 4-byte aligned store; read by the mulss

		void revert()
		{
			if (base)
				writeText(base + kSiteRVA + 4, kDefaultDisp, sizeof(kDefaultDisp)); // restore -> flt_180C32AA0 (30.0)
			if (scalarSlot) { VirtualFree(scalarSlot, 0, MEM_RELEASE); scalarSlot = nullptr; }
			applied = false;
		}
	};
} // namespace

template <GameState::Value gameT>
class MasterTickrateImpl : public IMasterTickrateImpl
{
private:
	GameState mGame;

	ScopedCallback<ActionEvent> mFlipCallback;
	// fires when the user commits a value in the "Custom Tickrate (Hz)" input
	ScopedCallback<eventpp::CallbackList<void(int&)>> mCustomRateChangedCallback;
	// re-apply the custom rate on level load (the game resets the timing struct each load)
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	// runs each game tick on the GAME thread; performs the deferred collision rebuild at a safe point
	std::unique_ptr<ScopedCallback<GameTickEvent>> mGameTickCallback;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<GameTickEventHook> gameTickEventHookWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::shared_ptr<MultilevelPointer> masterTickratePointer;   // int16 @ timingStruct + 0x02
	std::shared_ptr<MultilevelPointer> masterTickrateDtPointer; // float @ timingStruct + 0x04

	TickrateScalarPatcher mScalar;
	std::atomic<bool> mPendingRebuild{ false };

	// --- game functions/data we call to force a live collision rebuild (halo2.dll v1.3528 RVAs) ---
	using RecreateCollisionFn = uint32_t(__fastcall*)(uint16_t objectIndex);   // sub_180705B20 (self-no-ops if none)
	using GetObjectFn         = uintptr_t(__fastcall*)(uintptr_t headerEntry); // sub_1808D7000 (returns 0 if empty)
	static constexpr uint32_t  kRecreateCollisionRVA = 0x705B20;
	static constexpr uint32_t  kGetObjectRVA         = 0x8D7000;
	static constexpr uint8_t   kRecreateProbe[6] = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x20 };  // push rsi; sub rsp,20h
	static constexpr uint8_t   kGetObjectProbe[6] = { 0x8B, 0x51, 0x08, 0x48, 0x8B, 0x0D }; // mov edx,[rcx+8];mov rcx,[rip]
	static constexpr uint32_t  kObjectHeaderPtrRVA = 0x18B7398;  // -> qword_1818B7398 (object header table pointer)
	static constexpr uintptr_t kHeaderDataOffset   = 0x48;       // *(header+0x48) = offset from header to first entry
	static constexpr uintptr_t kHeaderCountOffset  = 0x40;       // *(header+0x40) = used/high-water object count
	static constexpr uint32_t  kHeaderStride       = 12;         // 12-byte header entries
	static constexpr uint32_t  kMaxObjects         = 2048;       // MAXIMUM_OBJECTS: array capacity + iteration clamp
	static constexpr uintptr_t kHavokComponentOffset = 0xB4;     // obj+0xB4 = collision body index (0xFFFFFFFF = none)

	// Write the tickrate int16 AND the matching seconds-per-tick (dt = 1/rate) together. dt = 1/rate is what keeps
	// 1x game speed (rate ticks/sec × 1/rate sec/tick = 1 sim-sec per real-sec). Caller must hold the game lock.
	void writeRate(int16_t newRate)
	{
		float newDt = 1.0f / (float)newRate;
		if (!masterTickratePointer->writeData(&newRate))
			throw HCMRuntimeException(std::format("Failed to write master tickrate: {}", MultilevelPointer::GetLastError()));
		if (!masterTickrateDtPointer->writeData(&newDt))
			throw HCMRuntimeException(std::format("Failed to write tickrate dt: {}", MultilevelPointer::GetLastError()));
	}

	// Point the collision scalar at `rate` and take precedence over Season7Physics. May defer (no-op) if halo2.dll
	// isn't loaded yet; throws only on a build mismatch.
	void applyScalarForRate(int rate)
	{
		if (auto settings = settingsWeak.lock())
			settings->customTickrateScalarActive = true; // custom scalar owns 0x70DBFA now; Season7Physics stands down
		if (!mScalar.apply()) return; // module not loaded yet
		mScalar.setScalar((float)rate);
	}

	// user typed an arbitrary rate: set tickrate+dt, match the collision scalar, and queue a live rebuild.
	void onSetCustomRate(int& newRate)
	{
		PLOG_DEBUG << "MasterTickrate onSetCustomRate: " << newRate;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				messagesGUI->addMessage("Load a Halo 2 game first to change the tickrate.");
				return;
			}

			int16_t rate16 = (int16_t)newRate; // validator clamps to 1..1000 (fits the int16 field & keeps dt sane)
			writeRate(rate16);
			applyScalarForRate(newRate);
			mPendingRebuild.store(true); // rebuild existing shapes on the next game tick (safe point)

			messagesGUI->addMessage(std::format("Master tickrate set to {} Hz (rebuilding collision).", (int)rate16));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// The game resets the timing struct (int16/dt) on every level load. If a custom rate is active, restore it and
	// re-assert the scalar. No rebuild needed here: the scalar repoint persists across loads, so shapes freshly
	// built during the load already read the correct scalar.
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState != PlayState::Ingame) return;
		auto settings = settingsWeak.lock();
		if (!settings || !settings->customTickrateScalarActive) return;
		try
		{
			int rate = settings->customTickrate->GetValue();
			writeRate((int16_t)rate);
			applyScalarForRate(rate);
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "MasterTickrate re-arm skipped: " << ex.what(); }
	}

	// runs on the GAME thread each tick; when a rebuild is queued, recreate collision for every live object.
	void onGameTick(uint32_t)
	{
		if (!mPendingRebuild.exchange(false)) return;
		if (!mScalar.isApplied()) return;
		uintptr_t base = mScalar.moduleBase();
		if (!base) return;

		// build gate: the two functions we call must match build 1.3528 before we jump into them
		if (memcmp((void*)(base + kRecreateCollisionRVA), kRecreateProbe, sizeof(kRecreateProbe)) != 0
			|| memcmp((void*)(base + kGetObjectRVA), kGetObjectProbe, sizeof(kGetObjectProbe)) != 0)
		{
			PLOG_ERROR << "MasterTickrate: object-fn prologue mismatch; skipping collision rebuild.";
			return;
		}

		uintptr_t headerPtr = *(uintptr_t*)(base + kObjectHeaderPtrRVA); // qword_1818B7398 (object header table)
		if (!headerPtr) return; // object table not up yet
		uintptr_t dataRegion = headerPtr + *(uintptr_t*)(headerPtr + kHeaderDataOffset); // first header entry
		uint32_t count = *(uint32_t*)(headerPtr + kHeaderCountOffset);
		if (count > kMaxObjects) count = kMaxObjects; // clamp to the array's real capacity (never read past it)

		auto getObject = reinterpret_cast<GetObjectFn>(base + kGetObjectRVA);
		auto recreate  = reinterpret_cast<RecreateCollisionFn>(base + kRecreateCollisionRVA);

		int rebuilt = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			uintptr_t entry = dataRegion + (uintptr_t)kHeaderStride * i;
			uintptr_t objData = getObject(entry); // 0 for empty/deleted slots (null-safe)
			if (!objData) continue;
			if (*(uint32_t*)(objData + kHavokComponentOffset) == 0xFFFFFFFF) continue; // no collision body
			recreate((uint16_t)i);
			++rebuilt;
		}
		PLOG_DEBUG << "MasterTickrate rebuilt collision for " << rebuilt << " objects";
	}

	void onFlip()
	{
		PLOG_DEBUG << "MasterTickrate onFlip";
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				messagesGUI->addMessage("Load a Halo 2 game first to change the tickrate.");
				return;
			}

			int16_t currentRate = 0;
			if (!masterTickratePointer->readData(&currentRate))
				throw HCMRuntimeException(std::format("Failed to read master tickrate: {}", MultilevelPointer::GetLastError()));

			// flip: anything at (about) 60 -> 30, otherwise -> 60. Like the custom input, the button also matches
			// the collision scalar and rebuilds so 30/60 physics is consistent (not just game speed).
			int16_t newRate = (currentRate >= 45) ? (int16_t)30 : (int16_t)60;
			writeRate(newRate);
			applyScalarForRate(newRate);
			mPendingRebuild.store(true);

			messagesGUI->addMessage(std::format("Master tickrate set to {} Hz (rebuilding collision).", newRate));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	MasterTickrateImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mFlipCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->masterTickrateFlipEvent, [this]() { onFlip(); }),
		mCustomRateChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->customTickrate->valueChangedEvent, [this](int& n) { onSetCustomRate(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		gameTickEventHookWeak(resolveDependentCheat(GameTickEventHook)),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		masterTickratePointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(masterTickrate), mGame);
		masterTickrateDtPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(masterTickrateDt), mGame);

		if (auto gth = gameTickEventHookWeak.lock())
			mGameTickCallback = gth->getGameTickEvent()->subscribe([this](uint32_t t) { onGameTick(t); });
	}

	~MasterTickrateImpl()
	{
		try { if (mScalar.isApplied()) mScalar.revert(); }
		catch (...) {}
		if (auto settings = settingsWeak.lock()) settings->customTickrateScalarActive = false;
	}
};


MasterTickrate::MasterTickrate(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("MasterTickrate not impl for this game");
	}
}

MasterTickrate::~MasterTickrate()
{
	PLOG_DEBUG << "~" << getName();
}
