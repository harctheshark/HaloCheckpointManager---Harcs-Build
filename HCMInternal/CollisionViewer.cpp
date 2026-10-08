#include "pch.h"
#include "CollisionViewer.h"
#include "SettingsStateAndEvents.h"
#include "DirectXRenderEvent.h"
#include "IMessagesGUI.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "H2CV_Viewer.h"
#include "H2CV_Halo2.h"
#include "H2CV_Log.h"

namespace
{
	constexpr int64_t kFirstPersonPassRva = 0x7E0C60; // sub_1807E0C60: entry = world depth complete, gun + HUD not drawn
	constexpr int64_t kResolveRva = 0x951EC0;         // sub_180951EC0: exit  = bloom + composite done, HUD not drawn

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
		uint8_t got[32] = {};
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
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<DirectXRenderEvent> renderEventWeak;

	ScopedCallback<ToggleEvent> mToggleCallback;
	std::unique_ptr<ScopedCallback<DirectXRenderEvent>> mRenderCallback;
	std::unique_ptr<ModuleInlineHook> mFpHook, mResolveHook;
	bool mHooksArmed = false;

	h2cv::Viewer mViewer;
	std::mutex mViewerMutex;                 // render-thread drawing vs toggle-off teardown
	static inline std::atomic<Impl*> sInstance{ nullptr };
	static inline std::atomic_bool sEnabled{ false };

	// ---- engine hook detours (render thread) ----
	static int64_t __fastcall fpDetour(int64_t a1, int64_t a2, int64_t a3, char a4, unsigned a5, int a6)
	{
		Impl* self = sInstance.load();
		if (self && sEnabled.load())
			guardedCall([](void* p) { static_cast<Impl*>(p)->onFirstPersonPass(); }, self, "first-person pass hook");
		return self ? self->mFpHook->getInlineHook().call<int64_t>(a1, a2, a3, a4, a5, a6) : 0;
	}

	static int64_t __fastcall resolveDetour(int64_t a1, int64_t a2, int64_t a3)
	{
		Impl* self = sInstance.load();
		const int64_t r = self ? self->mResolveHook->getInlineHook().call<int64_t>(a1, a2, a3) : 0;
		if (self && sEnabled.load())
			guardedCall([](void* p) { static_cast<Impl*>(p)->onResolveDone(); }, self, "resolve hook");
		return r;
	}

	void onFirstPersonPass()
	{
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		mViewer.onFirstPersonPass();
	}

	void onResolveDone()
	{
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		mViewer.onResolveDone();
	}

	// ---- HCM settings -> viewer (every frame; the GUI writes on this same render thread) ----
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
		v.fills = s.collisionViewerFills->GetValue();
		v.tim = s.collisionViewerTim->GetValue();
		v.timLabels = s.collisionViewerTimLabels->GetValue();
		v.pills = s.collisionViewerPills->GetValue();
		v.hud = s.collisionViewerInfoPanel->GetValue();
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
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		syncSettings(*settings);
		if (!mHooksArmed) armHooks();
		mViewer.onRender(dev, ctx, rtv, (uint32_t)screen.x, (uint32_t)screen.y);
	}

	// Arm the engine hooks only once halo2.dll is present and its entry bytes are the ones the RE was done on
	// (refuse rather than patch another build or a function someone else already hooked).
	void armHooks()
	{
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

	void onToggle(bool& on)
	{
		if (on)
		{
			if (!mRenderCallback)
				mRenderCallback = std::make_unique<ScopedCallback<DirectXRenderEvent>>(renderEventWeak.lock(),
					[this](ID3D11Device* d, ID3D11DeviceContext* c, SimpleMath::Vector2 s, ID3D11RenderTargetView* r) { onRender(d, c, s, r); });
			sEnabled = true;
		}
		else
		{
			sEnabled = false;
			mRenderCallback.reset();
			std::scoped_lock lk(mViewerMutex);
			mViewer.restoreBloom();
		}
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		renderEventWeak(dicon.Resolve<DirectXRenderEvent>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerToggle->valueChangedEvent,
			[this](bool& on) { onToggle(on); })
	{
		auto fp = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(mGame.toModuleName(), std::vector<int64_t>{ kFirstPersonPassRva });
		auto rs = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(mGame.toModuleName(), std::vector<int64_t>{ kResolveRva });
		mFpHook = ModuleInlineHook::make(mGame.toModuleName(), fp, (void*)&fpDetour);
		mResolveHook = ModuleInlineHook::make(mGame.toModuleName(), rs, (void*)&resolveDetour);
		sInstance = this;
		if (auto s = settingsWeak.lock(); s && s->collisionViewerToggle->GetValue())
		{
			bool on = true;
			onToggle(on);
		}
	}

	~Impl()
	{
		sEnabled = false;
		mRenderCallback.reset();
		{
			std::scoped_lock lk(mViewerMutex);
			mViewer.shutdown();
		}
		if (mFpHook) mFpHook->setWantsToBeAttached(false);
		if (mResolveHook) mResolveHook->setWantsToBeAttached(false);
		mFpHook.reset();
		mResolveHook.reset();
		sInstance = nullptr;
	}
};

// Only ever constructed for Halo 2: every GUI row that requires it is registered (Halo2) only.
CollisionViewer::CollisionViewer(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(gameImpl, dicon))
{
}

CollisionViewer::~CollisionViewer() = default;
