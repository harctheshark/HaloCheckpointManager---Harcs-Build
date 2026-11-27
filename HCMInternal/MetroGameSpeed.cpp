#include "pch.h"
#include "MetroGameSpeed.h"
#include "MetroEngine.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "GlobalKill.h"

// 2026-11-25 (user, after build 112 scaled the engine's time multiplier: "lets make it more like cheat engine, the game still
// waits for dialog to finish (its on its own speed)"): Cheat Engine's speedhack scales the clock the game READS, so everything
// timed off it (scripts, dialogue waits, timers, animation, physics) runs at the new speed. metro.exe reads time only through
// two imports (static survey of the unpacked exe): KERNEL32!QueryPerformanceCounter (IAT slot 0xA21358) and
// WINMM!timeGetTime (0xA21A00). HCM points those two slots of metro.exe at detours that return a scaled clock. Only the game's
// own calls go through them: the D3D runtime, the driver, PhysX's profiler and HCM itself keep real time.
// The clock never jumps: a speed change rebases it at the current time, and turning the feature off leaves it at 1x
// (any time already gained or lost stays, so the game never sees time going backwards). The slots stay patched for the
// rest of the session (HCM stays resident).
namespace
{
	constexpr uintptr_t kQpcSlotRVA = 0xA21358;
	constexpr uintptr_t kTgtSlotRVA = 0xA21A00;
	constexpr uintptr_t kOldTimeScaleRVA = 0xFC10F0;   // build 112's engine multiplier (put back to 1.0 if it was left changed)

	using qpc_t = BOOL(WINAPI*)(LARGE_INTEGER*);
	using tgt_t = DWORD(WINAPI*)();

	struct Clock
	{
		int64_t realQ0;  double virtQ0;   // QueryPerformanceCounter ticks
		DWORD realT0;    double virtT0;   // timeGetTime ms
		double speed;
	};

	qpc_t gRealQpc = nullptr;
	tgt_t gRealTgt = nullptr;
	std::atomic<const Clock*> gClock{ nullptr };   // replaced on every speed change; old ones are leaked (tiny, rare)
	std::atomic<int64_t> gLastQ{ 0 };              // QPC results are kept monotonic across threads

	double virtQ(const Clock* c, int64_t real) { return c->virtQ0 + double(real - c->realQ0) * c->speed; }
	double virtT(const Clock* c, DWORD real) { return c->virtT0 + double(DWORD(real - c->realT0)) * c->speed; }

	BOOL WINAPI qpcDetour(LARGE_INTEGER* out)
	{
		LARGE_INTEGER r{};
		const BOOL ok = gRealQpc(&r);
		const Clock* c = gClock.load(std::memory_order_acquire);
		if (!ok || !out || !c) { if (out) *out = r; return ok; }
		int64_t v = (int64_t)virtQ(c, r.QuadPart);
		int64_t last = gLastQ.load(std::memory_order_relaxed);
		while (v > last && !gLastQ.compare_exchange_weak(last, v, std::memory_order_relaxed)) {}
		if (v < last) v = last;
		out->QuadPart = v;
		return ok;
	}

	DWORD WINAPI tgtDetour()
	{
		const DWORD r = gRealTgt();
		const Clock* c = gClock.load(std::memory_order_acquire);
		return c ? (DWORD)(uint64_t)virtT(c, r) : r;
	}

	// rebase the clock at "now" with a new speed (UI / render thread only)
	void setSpeed(double speed)
	{
		const Clock* old = gClock.load(std::memory_order_acquire);
		if (old && old->speed == speed) return;
		LARGE_INTEGER r{}; gRealQpc(&r);
		const DWORD t = gRealTgt();
		auto* c = new Clock();
		c->realQ0 = r.QuadPart; c->virtQ0 = old ? virtQ(old, r.QuadPart) : double(r.QuadPart);
		c->realT0 = t;          c->virtT0 = old ? virtT(old, t) : double(t);
		c->speed = speed;
		gClock.store(c, std::memory_order_release);
	}

	bool writeSlot(uintptr_t slot, void* v)
	{
		DWORD old = 0;
		if (!VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
		*(void* volatile*)slot = v;
		DWORD tmp = 0;
		VirtualProtect((void*)slot, sizeof(void*), old, &tmp);
		return true;
	}

	// the IAT slot holding `fn` (the known RVA, else a search of the import table area around it)
	uintptr_t findSlot(uintptr_t knownRVA, void* fn, void* altFn)
	{
		auto holds = [&](uintptr_t at) { uintptr_t v = 0; return metro::read(at, v) && v && (v == (uintptr_t)fn || v == (uintptr_t)altFn); };
		const uintptr_t known = metro::base() + knownRVA;
		if (holds(known)) return known;
		for (uintptr_t at = metro::base() + 0xA20000; at < metro::base() + 0xA24000; at += 8) if (holds(at)) return at;
		return 0;
	}
}

class MetroGameSpeed::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::atomic<bool> mReady{ false };
	bool mHooked = false, mHookFailed = false;

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	bool ensureHooks()
	{
		if (mHooked) return true;
		if (mHookFailed) return false;
		HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), kb = GetModuleHandleW(L"kernelbase.dll"), wm = GetModuleHandleW(L"winmm.dll");
		void* qpc = k32 ? (void*)GetProcAddress(k32, "QueryPerformanceCounter") : nullptr;
		void* qpcAlt = kb ? (void*)GetProcAddress(kb, "QueryPerformanceCounter") : nullptr;
		void* tgt = wm ? (void*)GetProcAddress(wm, "timeGetTime") : nullptr;
		HMODULE wmb = GetModuleHandleW(L"winmmbase.dll");
		void* tgtAlt = wmb ? (void*)GetProcAddress(wmb, "timeGetTime") : nullptr;
		const uintptr_t qs = qpc ? findSlot(kQpcSlotRVA, qpc, qpcAlt) : 0;
		const uintptr_t ts = tgt ? findSlot(kTgtSlotRVA, tgt, tgtAlt) : 0;
		if (!qs)
		{
			mHookFailed = true;
			say("Game Speed: could not find the game's clock (another metro.exe build?) - not changed");
			return false;
		}
		gRealQpc = (qpc_t)metro::readPtr(qs);
		if (ts) gRealTgt = (tgt_t)metro::readPtr(ts);
		else { gRealTgt = (tgt_t)tgt; PLOG_ERROR << "Game Speed: timeGetTime slot not found - only the main clock is scaled"; }
		if (!gRealTgt) gRealTgt = []() -> DWORD { return GetTickCount(); };
		setSpeed(1.0);   // a 1x clock first, so the detours never see a null clock once live
		if (!writeSlot(qs, (void*)&qpcDetour)) { mHookFailed = true; say("Game Speed: could not change the game's clock - not changed"); return false; }
		if (ts && !writeSlot(ts, (void*)&tgtDetour)) PLOG_ERROR << "Game Speed: could not patch the timeGetTime slot";
		mHooked = true;
		PLOG_INFO << std::format("Game Speed: clock hooked (QPC slot +{:X}, timeGetTime slot +{:X})", qs - metro::base(), ts ? ts - metro::base() : 0);
		return true;
	}

	double wanted()
	{
		auto s = settingsWeak.lock();
		if (!s || !s->metroGameSpeedToggle->GetValue() || GlobalKill::isKillSet()) return 1.0;
		return std::clamp((double)s->metroGameSpeedAmount->GetValue(), 0.01, 10.0);
	}

	void onToggle(bool on)
	{
		if (on)
		{
			if (!ensureHooks()) return;
			setSpeed(wanted());
			say(std::format("Game Speed on: {:.2f}x", wanted()));
		}
		else
		{
			if (mHooked) setSpeed(1.0);
			say("Game Speed off: normal speed");
		}
	}

	// every frame: follow the slider (and drop to 1x if HCM is shutting down)
	void onRender(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire) || !mHooked) return;
		setSpeed(wanted());
	}

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<RenderEvent> mRenderCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->metroGameSpeedToggle->valueChangedEvent, [this](bool& on) { onToggle(on); }),
		mRenderCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRender(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroGameSpeed is Metro Redux only");
		// build 112 scaled the engine multiplier instead; if a session of it left that changed, put it back
		float cur = 0.f;
		if (metro::read(metro::base() + kOldTimeScaleRVA, cur) && cur != 1.f && cur > 0.f && cur <= 10.f) metro::write(metro::base() + kOldTimeScaleRVA, 1.f);
		if (auto s = settingsWeak.lock(); s && s->metroGameSpeedToggle->GetValue()) onToggle(true);
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderCallback.removeCallback();
		if (mHooked) setSpeed(1.0);   // HCM closing: normal speed (the slots stay on the detours, at 1x)
	}
};

MetroGameSpeed::MetroGameSpeed(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroGameSpeed::~MetroGameSpeed() { PLOG_VERBOSE << "~" << getName(); }
