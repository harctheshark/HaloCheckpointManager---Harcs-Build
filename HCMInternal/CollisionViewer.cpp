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
#include "DirPathContainer.h"
#include "Halo2Physics.h"
#include "HavokDebuggerBridge.h"
#include "H2CV_Reach.h"
#include "H2CV_Mem.h"
#include <filesystem>
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
	// Halo: Reach objects (scenery / crates / machines), live: their Havok components handed to the engine-thread gather
	std::shared_ptr<MultilevelPointer> mObjMeta;    // objectMetaDataTable (InternalPointerData.xml): the object header entries
	uint32_t mObjDiagLogged = 0;
	uint64_t mRxObjNextMs = 0;
	size_t mRxObjLast = ~size_t(0);
	bool mRxObjsSent = false;                 // a component list has been handed to the gather since the object toggles went off
	std::string mRxMachineDiagLast;           // the last machine diagnostics logged ...
	uint64_t mRxMachineDiagNextMs = 0;        // ... and when the next may be (a moving door changes it every pass)
	uint32_t mWhyLogged = 0;                 // diagnostics: which "frame skipped" reasons were logged since the last toggle
	const bool mIsReach;                      // Halo: Reach: no engine render hooks (drawn at Present only, ViewerReach.cpp; + the out-of-bounds bar)
	const bool mIsH3;                         // Halo 3 or ODST (Havok 4.5 engine layer); Halo 2 otherwise
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<DirectXRenderEvent> renderEventWeak;
	std::weak_ptr<IMCCStateHook> mccStateWeak;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<ToggleEvent> mInfoPanelCallback, mOobBarCallback;        // these two now run without the Collision Viewer
	ScopedCallback<ToggleEvent> mSuperbounceCallback;                       // and the superbounce viewer
	ScopedCallback<ToggleEvent> mKeepChargeCallback, mForceChargeCallback;  // and its charge options (they run without it)
	ScopedCallback<ActionEvent> mOffsetSaveCallback, mOffsetLoadCallback;   // Halo 2 offset save state
	ScopedCallback<ActionEvent> mInjectCallback;                            // superbounce: an injected checkpoint's charge is unknown
	ScopedCallback<ActionEvent> mSpotScanCallback;
	ScopedCallback<ActionEvent> mRecipeCallback;
	ScopedCallback<ActionEvent> mRecipeRedoCallback;                        // superbounce: "redo approaches here"                            // superbounce: "work out approaches"                          // superbounce: "scan around me"
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
	static constexpr int kSlots = 4;
	static inline std::atomic<Impl*> sSlot[kSlots]{};
	const int mSlot;
	const h2cv::Game mCvGame;
	std::atomic_bool mEnabled{ false };        // render / tick callbacks running: Collision Viewer OR info panel OR out-of-bounds bar
	std::atomic_bool mCollisionOn{ false };    // the Collision Viewer itself (gates the in-frame engine-pass drawing)
	std::atomic_bool mBounceOn{ false };       // the superbounce viewer / its charge options (Halo 3 / ODST: need the frame's camera too)

	static int slotOf(GameState g)
	{
		return g == GameState(GameState::Value::Halo3) ? 1 : g == GameState(GameState::Value::Halo3ODST) ? 2 : g == GameState(GameState::Value::HaloReach) ? 3 : 0;
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
		if (self && self->mEnabled.load() && self->mCollisionOn.load())
			guardedCall([](void* p) { static_cast<Impl*>(p)->onFirstPersonPass(); }, self, "first-person pass hook");
		return self ? self->mFpHook->getInlineHook().call<int64_t>(a1, a2, a3, a4, a5, a6) : 0;
	}

	static int64_t __fastcall resolveDetour(int64_t a1, int64_t a2, int64_t a3)
	{
		Impl* self = sSlot[0].load();
		const int64_t r = self ? self->mResolveHook->getInlineHook().call<int64_t>(a1, a2, a3) : 0;
		if (self && self->mEnabled.load() && self->mCollisionOn.load())
			guardedCall([](void* p) { static_cast<Impl*>(p)->onResolveDone(); }, self, "resolve hook");
		return r;
	}

	// Halo 3 / ODST: sub_1802835D4(view = rcx, a2 = dl); a2 == 0 is the opaque pass (world depth complete, gun and HUD not drawn yet)
	struct Fp3Args { Impl* self; uintptr_t view; bool opaque; };
	template <int Slot>
	static int64_t __fastcall fp3Detour(int64_t a1, char a2)
	{
		Impl* self = sSlot[Slot].load();
		// the superbounce viewer needs this frame's camera too (2026-11-02: with the Collision Viewer off it fell back to the observer
		// camera's fov - every bounce line squeezed toward the crosshair on a 21:9 screen)
		if (self && self->mEnabled.load() && (self->mCollisionOn.load() || self->mBounceOn.load()))
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
		v.offsetNaturalReverts = s.collisionViewerOffsetNaturalReverts->GetValue();
		v.killVolumes = s.collisionViewerKillVolumes->GetValue();
		v.superbounce = s.collisionViewerSuperbounce->GetValue();
		v.sbSeamText = s.collisionViewerSuperbounceSeamText->GetValue();
		v.sbSpeed = s.collisionViewerSuperbounceSpeed->GetValue();
		v.sbDebounce = s.collisionViewerSuperbounceDebounce->GetValue();
		v.sbObjects = s.collisionViewerSuperbounceObjects->GetValue();
		v.sbSeams = s.collisionViewerSuperbounceSeams->GetValue();
		v.sbSpots = s.collisionViewerSuperbounceSpots->GetValue();
		v.sbPredicted = s.collisionViewerSuperbouncePredicted->GetValue();
		v.sbPredictedRange = s.collisionViewerSuperbouncePredictedRange->GetValue();
		v.sbPredictedMaxApproach = s.collisionViewerSuperbouncePredictedMaxApproach->GetValue();
		v.sbPredictedMonsters = s.collisionViewerSuperbouncePredictedMonsters->GetValue();
		v.sbPredictedDreadnoughts = s.collisionViewerSuperbouncePredictedDreadnoughts->GetValue();
		v.sbPredictedHeights = s.collisionViewerSuperbouncePredictedHeights->GetValue();
		v.sbPredictedHideNarrow = s.collisionViewerSuperbouncePredictedHideNarrow->GetValue();
		v.sbScanRadius = s.collisionViewerSpotScanRadius->GetValue();
		v.sbScanSlow = s.collisionViewerSpotScanSlow->GetValue();
		v.sbScanCurrentTick = s.collisionViewerSpotScanCurrentTick->GetValue();
		v.sbRecord = s.collisionViewerSuperbounceRecord->GetValue();
		v.sbHighlight = s.collisionViewerSuperbounceHighlight->GetValue();
		v.sbKeepCharge = s.collisionViewerSuperbounceKeepCharge->GetValue();
		v.sbForceCharge = s.collisionViewerSuperbounceForceCharge->GetValue();
		v.infoSuperbounce = s.collisionViewerInfoSuperbounce->GetValue();
		v.collision = s.collisionViewerToggle->GetValue();
		v.oobAnchor = (int)s.oobBarAnchorCorner->GetValue();
		v.oobX = s.oobBarScreenOffset->GetValue().x; v.oobY = s.oobBarScreenOffset->GetValue().y;
		v.oobW = s.oobBarSize->GetValue().x; v.oobH = s.oobBarSize->GetValue().y;
		v.detectAnchor = (int)s.detectBarAnchorCorner->GetValue();
		v.detectX = s.detectBarScreenOffset->GetValue().x; v.detectY = s.detectBarScreenOffset->GetValue().y;
		v.detectW = s.detectBarSize->GetValue().x; v.detectH = s.detectBarSize->GetValue().y;
		v.infoGeneral = s.collisionViewerInfoGeneral->GetValue();
		v.infoTim = s.collisionViewerInfoTim->GetValue();
		v.infoGhost = s.collisionViewerInfoGhost->GetValue();
		v.infoGhostObjects = s.collisionViewerInfoGhostObjects->GetValue();
		v.infoGhostDetails = s.collisionViewerInfoGhostDetails->GetValue();
		v.infoPass = s.collisionViewerInfoPass->GetValue();
		v.infoTravel = s.collisionViewerInfoTravel->GetValue();
		v.infoLegend = s.collisionViewerInfoLegend->GetValue();
		v.infoGrids = s.collisionViewerInfoGrids->GetValue();
		v.infoKill = s.collisionViewerInfoKill->GetValue();
		v.infoWeld = s.collisionViewerInfoWeld->GetValue();
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
		// one log line per reason a frame is not drawn (the first time each happens): where the frame path stops
		auto why = [this](int bit, const char* what) { if (!(mWhyLogged & (1u << bit))) { mWhyLogged |= 1u << bit; PLOG_INFO << "[CollisionViewer] " << mGame.toString() << ": " << what; } };
		if (!settings) { why(0, "frame skipped: settings gone"); return; }
		if (!isActiveGame()) { why(1, "frame skipped: not the game MCC is running"); return; }
		std::unique_lock lk(mViewerMutex, std::try_to_lock);
		if (!lk.owns_lock()) { why(2, "frame skipped: viewer busy"); return; }
		why(3, "frame reached the viewer");
		h2cv::setGame(mCvGame);
		syncSettings(*settings);
		if (mIsReach)
		{
			// Reach: the collision drawing (ViewerReach.cpp) while the Collision Viewer is on, the superbounce viewer (drawn from the
			// same function, independent of the Collision Viewer's scene) while it or its charge options are on, then the
			// out-of-bounds bars and the panel's out-of-bounds line (also without them)
			if (mCollisionOn) updateReachObjects(*settings);
			if (mCollisionOn || mBounceOn) mViewer.onRender(dev, ctx, rtv, (uint32_t)screen.x, (uint32_t)screen.y);
			mViewer.onRenderReach();
			return;
		}
		if (!mHooksArmed) armHooks();
		mViewer.onRender(dev, ctx, rtv, (uint32_t)screen.x, (uint32_t)screen.y);
	}

	// Arm the engine hooks only once the game module is present and its entry bytes are the ones the RE was done on
	// (refuse rather than patch another build or a function someone else already hooked).
	void armHooks()
	{
		if (mIsReach) { mHooksArmed = true; return; }
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

	// Reach: 4 times a second, the scenery / crates / machines near the camera -> their Havok component datums -> the
	// engine-thread gather (HavokDebuggerBridge::setReachObjects), tagged with the viewer category.
	// The object table is read here (HCM's ObjectTableRange has no Reach table size): objectMetaDataTable resolves to the
	// header entries (stride 0x18: +0 salt, +4 type byte, +0x10 object address); the data-array header before them is found
	// by its 'd@t@' signature at header+0x28. Reach (live, 2026-10-21): header = entries - 0x70, name "object" @+0,
	// entry size u32 @+0x20 = 0x18, signature @+0x28, maximum count u32 @+0x2C = 2048.
	// Raw type byte (HaloReachObjectType): 6 scenery, 7 machine, 8 control (drawn as machines), 10 crate.
	// Object +0xCC = Havok component datum, +0x44 = position. Every read guarded.
	void updateReachObjects(SettingsStateAndEvents& s)
	{
		if (!mObjMeta) return;
		const bool wantScenery = s.collisionViewerShowScenery->GetValue(), wantCrates = s.collisionViewerShowCrates->GetValue(), wantMachines = s.collisionViewerShowMachines->GetValue();
		// no object category shown: the object table is not walked at all (the gather is stopped too, ViewerReach.cpp) -
		// only an empty list once, should the gather still be running for the Havok structure fallback
		if (!wantScenery && !wantCrates && !wantMachines)
		{
			if (mRxObjsSent) { HavokDebuggerBridge::setReachObjects(nullptr, nullptr, 0); mRxObjsSent = false; mRxObjLast = 0; }
			return;
		}
		const uint64_t now = GetTickCount64();
		if (now < mRxObjNextMs) return;
		mRxObjNextMs = now + 250;
		mRxObjsSent = true;
		std::vector<unsigned long long> comps;
		std::vector<unsigned char> tags;
		const h2::Camera cam = reach::readCamera();
		uintptr_t entries = 0;
		if ((wantScenery || wantCrates || wantMachines) && cam.ok && mObjMeta->resolve(&entries) && mem::okp(entries))
		{
			uintptr_t header = 0;
			for (uintptr_t h = entries - 0x2C; h + 0x200 >= entries && h > 0x10000; h -= 4)
				if (mem::u32(h + 0x28, 0) == 0x64407440u && (mem::u32(h + 0x20, 0) == 0x18 || mem::u16(h + 0x22, 0) == 0x18)) { header = h; break; }
			int maxCount = 0;
			if (header)
			{
				const int32_t m32 = mem::i32(header + 0x2C, 0);                // Reach: maximum count u32 after the signature
				maxCount = (m32 > 0 && m32 <= 16384) ? m32 : (int)(int16_t)mem::u16(header + 0x20, 0);
			}
			if (!header || maxCount <= 0 || maxCount > 16384)
			{
				if (!(mObjDiagLogged & 1))
				{
					mObjDiagLogged |= 1;
					char b[96]; std::string hex;
					for (int k = -0x80; k < 0; k += 4) { snprintf(b, sizeof(b), "%08X ", mem::u32(entries + k, 0)); hex += b; }
					PLOG_ERROR << "[CollisionViewer] Reach: object table header not found before entries 0x" << std::hex << entries << " (max " << std::dec << maxCount << "); 0x80 bytes before: " << hex;
				}
			}
			else
			{
				if (!(mObjDiagLogged & 2)) { mObjDiagLogged |= 2; PLOG_INFO << "[CollisionViewer] Reach: object table header 0x" << std::hex << header << " entries 0x" << entries << std::dec << ", max " << maxCount; }
				const float R = s.collisionViewerRadius->GetValue() + 30.f, R2 = R * R;
				for (int i = 0; i < maxCount && comps.size() < 1024; ++i)
				{
					const uintptr_t e = entries + (uintptr_t)i * 0x18;
					if (mem::u16(e, 0) == 0) continue;                         // free slot (salt 0)
					const uint8_t t = mem::u8(e + 4, 0xFF);
					h2cv::Cat cat;
					if (t == 6 && wantScenery) cat = h2cv::Cat::Scenery;
					else if (t == 10 && wantCrates) cat = h2cv::Cat::Crate;
					else if ((t == 7 || t == 8) && wantMachines) cat = h2cv::Cat::Machine;
					else continue;
					const uintptr_t obj = mem::q(e + 0x10);
					if (!mem::okp(obj)) continue;
					float p[3];
					if (!mem::copy(p, obj + 0x44, sizeof(p))) continue;
					const float dx = p[0] - cam.pos.x, dy = p[1] - cam.pos.y, dz = p[2] - cam.pos.z;
					if (dx * dx + dy * dy + dz * dz > R2) continue;
					const uint32_t havokDatum = mem::u32(obj + 0xCC, 0xFFFFFFFFu);
					if (havokDatum == 0xFFFFFFFFu || (havokDatum & 0xFFFF) == 0xFFFF) continue;
					comps.push_back(havokDatum);
					tags.push_back((unsigned char)cat);
				}
			}
		}
		HavokDebuggerBridge::setReachObjects(comps.data(), tags.data(), (int)comps.size());
		if (comps.size() != mRxObjLast) { mRxObjLast = comps.size(); PLOG_INFO << "[CollisionViewer] Reach: " << comps.size() << " objects with Havok collision near the camera"; }
		// what the machines near the camera gave (logged when it changes; positions round to 1 cm so a still door is quiet)
		if (std::string d = HavokDebuggerBridge::reachMachineDiag(); !d.empty() && d != mRxMachineDiagLast && now >= mRxMachineDiagNextMs)
		{
			mRxMachineDiagLast = d;
			mRxMachineDiagNextMs = now + 2000;
			PLOG_INFO << "[CollisionViewer] Reach machines:\n" << d;
		}
	}

	// The Collision Viewer, the info panel and the out-of-bounds bar are separate options now: the render / tick
	// callbacks run while ANY of them is on, the world drawing only with the Collision Viewer itself.
	// which: 0 Collision Viewer, 1 info panel, 2 out-of-bounds bar, 4-6 superbounce (its new value comes in `v`, the others are read)
	void refreshActive(int which, bool v)
	{
		auto s = settingsWeak.lock();
		if (!s) return;
		const bool cv = which == 0 ? v : s->collisionViewerToggle->GetValue();
		const bool info = which == 1 ? v : s->collisionViewerInfoPanel->GetValue();
		const bool bar = which == 2 ? v : s->collisionViewerKillVolumes->GetValue();
		// Halo 2, Halo 3 / ODST / Reach (h3sb.dll's Halo 2 body, 2026-11-02; Reach 2026-11-04): the viewer or its charge options
		const bool bounce = ((which == 4 ? v : s->collisionViewerSuperbounce->GetValue())
			|| (which == 5 ? v : s->collisionViewerSuperbounceKeepCharge->GetValue())
			|| (which == 6 ? v : s->collisionViewerSuperbounceForceCharge->GetValue()));
		const bool wasCollision = mCollisionOn.exchange(cv);
		mBounceOn = bounce;
		bool active = cv || info || bar || bounce;
		if (active != mEnabled.load()) onToggle(active);
		else if (mIsReach && active && wasCollision && !cv)
		{
			// Reach: the Collision Viewer went off but the bars stay - let the engine step function go (the Havok gather)
			std::scoped_lock lk(mViewerMutex);
			mViewer.stopReach();
		}
	}

	void onToggle(bool& on)
	{
		PLOG_INFO << "[CollisionViewer] " << mGame.toString() << ": toggle " << (on ? "on" : "off") << (renderEventWeak.expired() ? " (render event expired!)" : "");
		mWhyLogged = 0;
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
			if (mIsReach) mViewer.stopReach();   // let the engine step function go (the Havok gather)
		}
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mIsReach(game == GameState(GameState::Value::HaloReach)),
		mIsH3(game == GameState(GameState::Value::Halo3) || game == GameState(GameState::Value::Halo3ODST)),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		renderEventWeak(dicon.Resolve<DirectXRenderEvent>()),
		mccStateWeak(dicon.Resolve<IMCCStateHook>()),
		mSlot(slotOf(game)),
		mCvGame(game == GameState(GameState::Value::Halo3) ? h2cv::Game::Halo3 : game == GameState(GameState::Value::Halo3ODST) ? h2cv::Game::Halo3ODST : game == GameState(GameState::Value::HaloReach) ? h2cv::Game::HaloReach : h2cv::Game::Halo2),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerToggle->valueChangedEvent,
			[this](bool& on) { refreshActive(0, on); }),
		mInfoPanelCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerInfoPanel->valueChangedEvent,
			[this](bool& on) { refreshActive(1, on); }),
		mOobBarCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerKillVolumes->valueChangedEvent,
			[this](bool& on) { refreshActive(2, on); }),
		mSuperbounceCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerSuperbounce->valueChangedEvent,
			[this](bool& on) { refreshActive(4, on); }),
		mKeepChargeCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerSuperbounceKeepCharge->valueChangedEvent,
			[this](bool& on) { refreshActive(5, on); }),
		mForceChargeCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerSuperbounceForceCharge->valueChangedEvent,
			[this](bool& on) { refreshActive(6, on); }),
		mOffsetSaveCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->offsetSaveEvent,
			[this]() { if (!mIsH3 && isActiveGame()) mViewer.requestOffsetSave(); }),
		mOffsetLoadCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->offsetLoadEvent,
			[this]() { if (!mIsH3 && isActiveGame()) mViewer.requestOffsetLoad(); }),
		mInjectCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->injectCheckpointEvent,
			[this]() { if (!mIsH3 && !mIsReach) h2cv::sb::forgetCheckpointCharges(); }),
		mSpotScanCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerSpotScanEvent,
			[this]() { h2cv::sb::requestSpotScan(); }),   // Halo 2, Halo 3, ODST, Reach
		mRecipeCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerRecipeEvent,
			[this]() { h2cv::sb::requestRecipes(); }),
		mRecipeRedoCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->collisionViewerRecipeRedoEvent,
			[this]() { h2cv::sb::requestRecipes(true); })
	{
		// offset save state: the viewer triggers HCM's own force checkpoint / force revert (unchanged) and reports here
		mViewer.fireCheckpoint = [w = settingsWeak]() { if (auto s = w.lock()) s->forceCheckpointEvent->operator()(); };
		mViewer.fireRevert = [w = settingsWeak]() { if (auto s = w.lock()) s->forceRevertEvent->operator()(); };
		mViewer.message = [w = messagesWeak](std::string msg) { if (auto m = w.lock()) m->addMessage(msg); };
		if (mIsReach)
		{
			// Halo: Reach: nothing hooked in the game's render. Collision: Poorboy's world files in <HCM folder>\h3sb\ (his
			// h3sb.dll writes them; asked for in observe mode when a map has none), else the Havok debugger's engine-thread
			// gather (started on the first frame drawn). The out-of-bounds bar is read on the render thread (H2CV_ReachOob).
			try { mObjMeta = dicon.Resolve<PointerDataStore>().lock()->getData<std::shared_ptr<MultilevelPointer>>("objectMetaDataTable", game); }
			catch (...) { PLOG_ERROR << "[CollisionViewer] Reach: no objectMetaDataTable pointer - objects will not be drawn"; mObjMeta.reset(); }
			if (auto dir = dicon.Resolve<DirPathContainer>().lock())
				mViewer.setReachWorldSource((std::filesystem::path(dir->dirPath) / L"h3sb").wstring(), [](bool on) { Halo2Physics::wantWorldFile(on); });
		}
		else if (mIsH3)
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
		if (auto s = settingsWeak.lock(); s && (s->collisionViewerToggle->GetValue() || s->collisionViewerInfoPanel->GetValue() || s->collisionViewerKillVolumes->GetValue()
			|| s->collisionViewerSuperbounce->GetValue() || s->collisionViewerSuperbounceKeepCharge->GetValue()
			|| s->collisionViewerSuperbounceForceCharge->GetValue()))
		{
			mCollisionOn = s->collisionViewerToggle->GetValue();
			mBounceOn = s->collisionViewerSuperbounce->GetValue() || s->collisionViewerSuperbounceKeepCharge->GetValue()
				|| s->collisionViewerSuperbounceForceCharge->GetValue();
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

// Constructed for Halo 2, Halo 3, Halo 3: ODST and Halo: Reach.
CollisionViewer::CollisionViewer(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(gameImpl, dicon))
{
}

CollisionViewer::~CollisionViewer() = default;
