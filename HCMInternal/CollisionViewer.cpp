#include "pch.h"
#include "CollisionViewer.h"
#include "SettingsStateAndEvents.h"
#include "DirectXRenderEvent.h"
#include "IMessagesGUI.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "GameTickEventHook.h"
#include "IMCCStateHook.h"
#include "IMakeOrGetCheat.h"
#include "H2CV_Viewer.h"
#include "H2CV_Halo2.h"
#include "H2CV_Halo3.h"
#include "H2CV_Engine.h"
#include "H2CV_Log.h"

namespace
{
	// ---- Halo 2 (halo2.dll) ----
	constexpr int64_t kFirstPersonPassRva = 0x7E0C60; // sub_1807E0C60: entry = world depth complete, gun + HUD not drawn
	constexpr int64_t kResolveRva = 0x951EC0;         // sub_180951EC0: exit  = bloom + composite done, HUD not drawn

	// ---- Halo 3 / ODST: the per-view first-person pass, sub_1802835D4(view, a2) / ODST sub_1802AC2E4. Prologue up to the first call. ----
	constexpr uint8_t kH3FirstPersonBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x55, 0x41, 0x56,
		0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x40, 0x8A, 0xF2, 0x8B, 0x89, 0xF4, 0x27, 0x00, 0x00 };
	constexpr uint8_t kOdstFirstPersonBytes[] = {
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC,
		0x20, 0x48, 0x8B, 0xD9, 0x40, 0x8A, 0xF2, 0x8B, 0x89, 0xFC, 0x27, 0x00, 0x00 };

	// SEH must live in a function without C++ unwinding (no logging in here)
	bool sehCall(void (*fn)(void*), void* arg)
	{
		__try { fn(arg); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void guardedCall(void (*fn)(void*), void* arg, const char* what)
	{
		if (!sehCall(fn, arg)) PLOG_ERROR << "[CollisionViewer] exception in " << what << " - skipped";
	}

	bool entryBytesMatch(uintptr_t at, const uint8_t* expect, size_t n)
	{
		uint8_t got[64] = {};
		if (n > sizeof(got)) return false;
		__try { memcpy(got, (const void*)at, n); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return memcmp(got, expect, n) == 0;
	}
}

class CollisionViewer::Impl
{
private:
	GameState mGame;
	const bool mIsH3;                         // Halo 3 or ODST (Havok 4.5 engine layer); Halo 2 otherwise
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<DirectXRenderEvent> renderEventWeak;
	std::weak_ptr<IMCCStateHook> mccStateWeak;

	ScopedCallback<ToggleEvent> mToggleCallback;
	std::unique_ptr<ScopedCallback<DirectXRenderEvent>> mRenderCallback;
	std::unique_ptr<ModuleInlineHook> mFpHook, mResolveHook;
	std::shared_ptr<MultilevelPointer> mFp3Ptr;     // Halo 3 / ODST first-person pass (InternalPointerData.xml collisionViewerFirstPersonPass)
	bool mHooksArmed = false;

	// Halo 3 / ODST: the simulation thread calls back once per game tick (HCM's game tick event); the viewer samples the player's Havok state there
	std::shared_ptr<ObservedEvent<GameTickEvent>> mTickEvent;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(uint32_t)>>> mTickCallback;
	std::atomic_int mSimInFlight{ 0 };

	h2cv::Viewer mViewer;
	std::mutex mViewerMutex;                 // render-thread drawing vs toggle-off teardown
	// HCM constructs one Impl per game (Halo 2, Halo 3, ODST) and all of them live for the whole session. Each one owns a slot,
	// so a detour always reaches the Impl (and trampoline) of the module it was installed in, and only the Impl of the game MCC
	// is running draws / samples. h2cv's game selector is process-global: the active Impl sets it before every call into h2cv.
	static constexpr int kSlots = 3;
	static inline std::atomic<Impl*> sSlot[kSlots]{};
	const int mSlot;
	const h2cv::Game mCvGame;
	std::atomic_bool mEnabled{ false };

	static int slotOf(GameState g)
	{
		return g == GameState(GameState::Value::Halo3) ? 1 : g == GameState(GameState::Value::Halo3ODST) ? 2 : 0;
	}

	bool isActiveGame()
	{
		auto mcc = mccStateWeak.lock();
		return mcc && mcc->getCurrentMCCState().currentGameState == mGame;
	}

	// ---- engine hook detours (render thread) ----
	static int64_t __fastcall fpDetour(int64_t a1, int64_t a2, int64_t a3, char a4, unsigned a5, int a6)
	{
		Impl* self = sSlot[0].load();
		if (self && self->mEnabled.load())
			guardedCall([](void* p) { static_cast<Impl*>(p)->onFirstPersonPass(); }, self, "first-person pass hook");
		return self ? self->mFpHook->getInlineHook().call<int64_t>(a1, a2, a3, a4, a5, a6) : 0;
	}

	static int64_t __fastcall resolveDetour(int64_t a1, int64_t a2, int64_t a3)
	{
		Impl* self = sSlot[0].load();
		const int64_t r = self ? self->mResolveHook->getInlineHook().call<int64_t>(a1, a2, a3) : 0;
		if (self && self->mEnabled.load())
			guardedCall([](void* p) { static_cast<Impl*>(p)->onResolveDone(); }, self, "resolve hook");
		return r;
	}

	// Halo 3 / ODST: sub_1802835D4(view = rcx, a2 = dl); a2 == 0 is the opaque pass (world depth complete, gun and HUD not drawn yet)
	struct Fp3Args { Impl* self; uintptr_t view; bool opaque; };
	template <int Slot>
	static int64_t __fastcall fp3Detour(int64_t a1, char a2)
	{
		Impl* self = sSlot[Slot].load();
		if (self && self->mEnabled.load())
		{
			Fp3Args args{ self, (uintptr_t)a1, a2 == 0 };
			guardedCall([](void* p) { auto* a = static_cast<Fp3Args*>(p); a->self->onFirstPersonPass3(a->view, a->opaque); }, &args, "first-person pass hook");
		}
		return self ? self->mFpHook->getInlineHook().call<int64_t>(a1, a2) : 0;
	}

	void onFirstPersonPass()
	{
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		h2cv::setGame(mCvGame);
		mViewer.onFirstPersonPass();
	}

	void onFirstPersonPass3(uintptr_t view, bool opaque)
	{
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		h2cv::setGame(mCvGame);
		mViewer.onFirstPersonPass3(view, opaque);
	}

	void onResolveDone()
	{
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		h2cv::setGame(mCvGame);
		mViewer.onResolveDone();
	}

	// simulation thread: sample the player's Havok state / capture the thread's TLS block. Touches only the viewer's thread-safe queue.
	void onSimTick(uint32_t tick)
	{
		++mSimInFlight;
		if (mEnabled.load() && isActiveGame()) { h2cv::setGame(mCvGame); mViewer.onSimTick(tick); }
		--mSimInFlight;
	}

	// ---- HCM settings -> viewer (every frame; the GUI writes on this same render thread) ----
	// colour picker (0..1 floats) -> the viewer's rgba() packing: r | g<<8 | b<<16 | a<<24
	static uint32_t packColour(const SimpleMath::Vector4& c)
	{
		auto ch = [](float f) { return (uint32_t)(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
		return ch(c.x) | (ch(c.y) << 8) | (ch(c.z) << 16) | (255u << 24);
	}

	void syncSettings(SettingsStateAndEvents& s)
	{
		auto& v = mViewer.settings;
		using h2cv::Cat;
		v.show[(int)Cat::Bsp] = s.collisionViewerShowBsp->GetValue();
		v.show[(int)Cat::Instanced] = s.collisionViewerShowInstanced->GetValue();
		const bool invis = s.collisionViewerShowInvisible->GetValue();
		v.show[(int)Cat::BspInvisible] = invis;
		v.show[(int)Cat::InstancedInvisible] = invis;
		v.show[(int)Cat::Breakable] = s.collisionViewerShowBreakable->GetValue();
		v.show[(int)Cat::Scenery] = s.collisionViewerShowScenery->GetValue();
		v.show[(int)Cat::Crate] = s.collisionViewerShowCrates->GetValue();
		v.show[(int)Cat::Machine] = s.collisionViewerShowMachines->GetValue();
		v.show[(int)Cat::KillTrigger] = s.collisionViewerShowKillTriggers->GetValue();
		v.show[(int)Cat::SoftCeiling] = s.collisionViewerShowSoftCeilings->GetValue();
		v.fills = s.collisionViewerFills->GetValue();
		v.tim = s.collisionViewerTim->GetValue();
		v.timLabels = s.collisionViewerTimLabels->GetValue();
		v.pills = s.collisionViewerPills->GetValue();
		v.hud = s.collisionViewerInfoPanel->GetValue();
		v.ghostLog = s.collisionViewerGhostLog->GetValue();
		v.ghostVehicles = s.collisionViewerGhostVehicles->GetValue();
		v.creases = s.collisionViewerCreases->GetValue();
		v.creaseFaces = s.collisionViewerCreaseFaces->GetValue();
		v.ghostShowReal = s.collisionViewerGhostShowReal->GetValue();
		v.ghostColNormal = packColour(s.collisionViewerGhostColNormal->GetValue());
		v.ghostColDesync = packColour(s.collisionViewerGhostColDesync->GetValue());
		v.ghostColBelieved = packColour(s.collisionViewerGhostColBelieved->GetValue());
		v.ghost = s.collisionViewerGhost->GetValue();
		v.ghostBelieved = s.collisionViewerGhostBelieved->GetValue();
		v.ghostBelievedObjects = s.collisionViewerGhostBelievedObjects->GetValue();
		v.ghostLinkLabels = s.collisionViewerGhostLinkLabels->GetValue();
		v.ghostPass = s.collisionViewerGhostPass->GetValue();
		v.ghostTravel = s.collisionViewerGhostTravel->GetValue();
		v.ghostTravelObjects = s.collisionViewerGhostTravelObjects->GetValue();
		v.weldZonesNear = s.collisionViewerWeldZonesNear->GetValue();
		v.weldZonesWelded = s.collisionViewerWeldZonesWelded->GetValue();
		v.weldZoneRange = s.collisionViewerWeldZoneRange->GetValue();
		v.ghostTrail = s.collisionViewerGhostTrail->GetValue();
		v.ghostRewind = s.collisionViewerGhostRewind->GetValue();
		v.ghostJump = s.collisionViewerGhostJump->GetValue();
		v.ghostPrev = s.collisionViewerGhostPrev->GetValue();
		v.ghostHalo = s.collisionViewerGhostHalo->GetValue();
		v.ghostEdges = s.collisionViewerGhostEdges->GetValue();
		v.ghostEdgesSuspectOnly = s.collisionViewerGhostEdgesSuspect->GetValue();
		v.lineWidth = s.collisionViewerLineWidth->GetValue();
		v.pull = s.collisionViewerDepthPull->GetValue();
		v.nearRadius = s.collisionViewerRadius->GetValue();
		v.hiddenAlpha = s.collisionViewerHiddenAlpha->GetValue();
		v.gameZReversed = s.collisionViewerReversedZ->GetValue();
		using O = SettingsEnums::CollisionViewerOcclusionEnum;
		switch (s.collisionViewerOcclusion->GetValue())
		{
		case O::InFrame_NoBloom: v.occlusion = (int)h2cv::Occlusion::InFrame; break;
		case O::PostBloom_KeepsBloom: v.occlusion = (int)h2cv::Occlusion::PostBloom; break;
		case O::OwnCollisionDepth: v.occlusion = (int)h2cv::Occlusion::Own; break;
		default: v.occlusion = (int)h2cv::Occlusion::XRay; break;
		}
	}

	void onRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, SimpleMath::Vector2 screen, ID3D11RenderTargetView* rtv)
	{
		auto settings = settingsWeak.lock();
		if (!settings) return;
		if (!isActiveGame()) return;              // another game's Impl owns the overlay this frame
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		h2cv::setGame(mCvGame);
		syncSettings(*settings);
		if (!mHooksArmed) armHooks();
		mViewer.onRender(dev, ctx, rtv, (uint32_t)screen.x, (uint32_t)screen.y);
	}

	// Arm the engine hooks only once the game module is present and its entry bytes are the ones the RE was done on
	// (refuse rather than patch another build or a function someone else already hooked).
	void armHooks()
	{
		if (mIsH3) { armHooks3(); return; }
		if (!h2::base() || !h2::buildOk()) return;
		mHooksArmed = true;
		static const uint8_t fpBytes[] = { 0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x01, 0x00, 0x00 };
		static const uint8_t rsBytes[] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00 };
		const bool fpOk = mFpHook->isHookInstalled() || entryBytesMatch(h2::base() + kFirstPersonPassRva, fpBytes, sizeof(fpBytes));
		const bool rsOk = mResolveHook->isHookInstalled() || entryBytesMatch(h2::base() + kResolveRva, rsBytes, sizeof(rsBytes));
		if (!fpOk || !rsOk)
		{
			PLOG_ERROR << "[CollisionViewer] render-pass entry bytes differ (another tool hooked them, or a different build) - only the own-depth mode will work";
			if (auto m = messagesWeak.lock()) m->addMessage("Collision Viewer: render hooks unavailable - using own collision depth only");
			return;
		}
		// Attached once and left attached for the session: detaching an inline hook while the render thread is inside
		// the function races (crash). The detours are a cheap no-op while the viewer is off.
		mFpHook->setWantsToBeAttached(true);
		mResolveHook->setWantsToBeAttached(true);
		PLOG_INFO << "[CollisionViewer] render hooks armed";
	}

	void armHooks3()
	{
		if (!h3::base() || !h3::buildOk()) return;
		mHooksArmed = true;
		uintptr_t fp = 0;
		if (!mFp3Ptr || !mFp3Ptr->resolve(&fp)) { PLOG_ERROR << "[CollisionViewer] first-person pass address did not resolve - only the own-depth mode will work"; return; }
		const bool odst = mGame == GameState(GameState::Value::Halo3ODST);
		const uint8_t* expect = odst ? kOdstFirstPersonBytes : kH3FirstPersonBytes;
		const size_t n = odst ? sizeof(kOdstFirstPersonBytes) : sizeof(kH3FirstPersonBytes);
		if (!mFpHook->isHookInstalled() && !entryBytesMatch(fp, expect, n))
		{
			PLOG_ERROR << "[CollisionViewer] first-person pass entry bytes differ (another tool hooked it, or a different build) - only the own-depth mode will work";
			if (auto m = messagesWeak.lock()) m->addMessage("Collision Viewer: render hook unavailable - using own collision depth only");
			return;
		}
		mFpHook->setWantsToBeAttached(true);   // attached once, left attached (see the Halo 2 note above)
		PLOG_INFO << "[CollisionViewer] first-person pass hook armed @0x" << std::hex << fp;
	}

	void onToggle(bool& on)
	{
		if (on)
		{
			if (!mRenderCallback)
				mRenderCallback = std::make_unique<ScopedCallback<DirectXRenderEvent>>(renderEventWeak.lock(),
					[this](ID3D11Device* d, ID3D11DeviceContext* c, SimpleMath::Vector2 s, ID3D11RenderTargetView* r) { onRender(d, c, s, r); });
			if (mTickEvent && !mTickCallback)
				mTickCallback = mTickEvent->subscribe([this](uint32_t t) { onSimTick(t); });
			mEnabled = true;
		}
		else
		{
			mEnabled = false;
			mRenderCallback.reset();
			mTickCallback.reset();
			for (int i = 0; i < 400 && mSimInFlight.load() > 0; ++i) Sleep(1);
			std::scoped_lock lk(mViewerMutex);
			mViewer.restoreBloom();
		}
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mIsH3(game == GameState(GameState::Value::Halo3) || game == GameState(GameState::Value::Halo3ODST)),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		renderEventWeak(dicon.Resolve<DirectXRenderEvent>()),
		mccStateWeak(dicon.Resolve<IMCCStateHook>()),
		mSlot(slotOf(game)),
		mCvGame(game == GameState(GameState::Value::Halo3) ? h2cv::Game::Halo3 : game == GameState(GameState::Value::Halo3ODST) ? h2cv::Game::Halo3ODST : h2cv::Game::Halo2),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerToggle->valueChangedEvent,
			[this](bool& on) { onToggle(on); })
	{
		if (mIsH3)
		{
			// A missing XML entry throws HCMInitException here -> the cheat is unavailable for this game build and its GUI rows grey out.
			auto ptr = dicon.Resolve<PointerDataStore>().lock();
			auto collisionViewerFirstPersonPass = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(collisionViewerFirstPersonPass), game);
			mFp3Ptr = collisionViewerFirstPersonPass;
			mFpHook = ModuleInlineHook::make(mGame.toModuleName(), collisionViewerFirstPersonPass, mSlot == 2 ? (void*)&fp3Detour<2> : (void*)&fp3Detour<1>);
			mTickEvent = resolveDependentCheat(GameTickEventHook)->getGameTickEvent();
		}
		else
		{
			auto fp = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(mGame.toModuleName(), std::vector<int64_t>{ kFirstPersonPassRva });
			auto rs = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(mGame.toModuleName(), std::vector<int64_t>{ kResolveRva });
			mFpHook = ModuleInlineHook::make(mGame.toModuleName(), fp, (void*)&fpDetour);
			mResolveHook = ModuleInlineHook::make(mGame.toModuleName(), rs, (void*)&resolveDetour);
		}
		sSlot[mSlot] = this;
		if (auto s = settingsWeak.lock(); s && s->collisionViewerToggle->GetValue())
		{
			bool on = true;
			onToggle(on);
		}
	}

	~Impl()
	{
		mEnabled = false;
		mRenderCallback.reset();
		mTickCallback.reset();
		for (int i = 0; i < 400 && mSimInFlight.load() > 0; ++i) Sleep(1);
		{
			std::scoped_lock lk(mViewerMutex);
			mViewer.shutdown();
		}
		if (mFpHook) mFpHook->setWantsToBeAttached(false);
		if (mResolveHook) mResolveHook->setWantsToBeAttached(false);
		mFpHook.reset();
		mResolveHook.reset();
		sSlot[mSlot] = nullptr;
	}
};

// Constructed for Halo 2, Halo 3 and Halo 3: ODST: every GUI row that requires it is registered for exactly those games.
CollisionViewer::CollisionViewer(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(gameImpl, dicon))
{
}

CollisionViewer::~CollisionViewer() = default;
