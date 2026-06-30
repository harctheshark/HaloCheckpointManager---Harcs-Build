#include "pch.h"
#include "PlacementPointsOverlay.h"
#include "Render3DEventProvider.h"
#include "IMakeOrGetCheat.h"
#include "IMCCStateHook.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "IMessagesGUI.h"
#include "GetPlayerDatum.h"
#include "GetObjectPhysics.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"

// The 27 candidate directions (halo3.dll+0x7FB090). [0] = center; [1..17] = horizontal + up; [18..26] = down.
namespace
{
	constexpr float S2 = 0.70710677f; // 1/sqrt(2)
	constexpr float S3 = 0.5773503f;  // 1/sqrt(3)
	const std::array<SimpleMath::Vector3, 27> kPlacementDirs = { {
		{ 0,  0,  0}, { 1,  0,  0}, { 0,  0,  1}, { S2,  0, S2}, { S3,  S3, S3}, { S3, -S3, S3},
		{ S2, S2,  0}, { S2,-S2,  0}, { 0, S2, S2}, { 0,-S2, S2}, {-1,  0,  0}, { 0,  1,  0},
		{ 0, -1,  0}, {-S2,-S2,  0}, {-S2, S2, 0}, {-S2, 0, S2}, {-S3,-S3, S3}, {-S3, S3, S3},
		{ 0,  0, -1}, { S2, 0, -S2}, {-S2, 0, -S2}, { 0, S2,-S2}, { 0,-S2,-S2}, { S3, S3,-S3},
		{ S3,-S3,-S3}, {-S3,-S3,-S3}, {-S3, S3,-S3}
	} };
	constexpr int kBaseCount = 18; // respawn set (center + horizontal + up)
	constexpr int kFullCount = 27; // vehicle set (adds the 9 downward dirs)

	// surface_test sub_18009BC78(uint16_t* out, float* pos): sets *out to a surface ref (0xFFFF = none) for a world
	// position by walking the active collision-BSP clusters. Read-only. SEH-guarded so a bad engine state during a
	// level transition can't crash the overlay. Returns 1=valid surface, 0=no surface, -1=call faulted/unknown.
	static int safeSurfaceTest(uintptr_t pFn, const SimpleMath::Vector3& pos)
	{
		__try
		{
			typedef void(__fastcall* SurfaceTest_t)(uint16_t*, const float*);
			uint16_t surfaceRef = 0xFFFF;
			float p[4] = { pos.x, pos.y, pos.z, 0.f };
			((SurfaceTest_t)pFn)(&surfaceRef, p);
			return surfaceRef != 0xFFFF ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	// BSP-validity sub_1801FCAC0(int64 flags, float* pos, int64 unitDatum, int a4=-1, int* a5=null) -> char.
	// Nonzero return = the point is BLOCKED / in solid (the engine rejects such candidates). Read-only walk of the
	// static collision BSP, SEH-guarded. Returns 1=blocked(invalid), 0=clear(valid), -1=call faulted/unknown.
	static int safeBspBlocked(uintptr_t pFn, uint64_t flags, uint32_t unitDatum, const SimpleMath::Vector3& pos)
	{
		__try
		{
			typedef char(__fastcall* BspTest_t)(int64_t, const float*, int64_t, int, int*);
			float p[4] = { pos.x, pos.y, pos.z, 0.f };
			// a5 is an output buffer the engine writes unconditionally on some games (e.g. Reach writes a5[0..3]);
			// pass a real scratch buffer, not nullptr, or those games fault on every point.
			int scratch[16] = { 0 };
			char r = ((BspTest_t)pFn)((int64_t)flags, p, (int64_t)unitDatum, -1, scratch);
			return r != 0 ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

}


template <GameState::Value gameT>
class PlacementPointsOverlayImpl : public PlacementPointsOverlayUntemplated
{
private:
	// callbacks
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRenderEventCallback;
	ScopedCallback<ToggleEvent> placementPointsOverlayToggleEventCallback;

	// injected services
	std::weak_ptr<Render3DEventProvider> render3DEventProviderWeak;
	std::weak_ptr<GetPlayerDatum> getPlayerDatumWeak;
	std::weak_ptr<GetObjectPhysics> getObjectPhysicsWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// engine validity tests (V2) - null on game versions we don't have offsets for (validity colouring then off)
	std::shared_ptr<MultilevelPointer> placementSurfaceTestFunction;
	std::shared_ptr<MultilevelPointer> placementBspValidityFunction;
	std::shared_ptr<MultilevelPointer> placementBspFlagsGlobal;

	std::atomic_bool renderingMutex = false;

	void onToggleChange(bool& newValue)
	{
		lockOrThrow(mccStateHookWeak, mccStateHook);
		if (mccStateHook->isGameCurrentlyPlaying(gameT) == false)
		{
			if (renderingMutex) renderingMutex.wait(true);
			mRenderEventCallback.reset();
			return;
		}

		if (newValue)
		{
			try
			{
				lockOrThrow(render3DEventProviderWeak, render3DEventProvider);
				mRenderEventCallback = render3DEventProvider->getRender3DEvent()->subscribe([this](GameState g, IRenderer3D* n) { onRenderEvent(g, n); });
			}
			catch (HCMRuntimeException ex)
			{
				runtimeExceptions->handleMessage(ex);
			}
		}
		else
		{
			if (renderingMutex) renderingMutex.wait(true);
			mRenderEventCallback.reset();
		}

		try
		{
			lockOrThrow(messagesWeak, messages);
			messages->addMessage(newValue ? "Placement Points Overlay enabled." : "Placement Points Overlay disabled.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onRenderEvent(GameState game, IRenderer3D* renderer)
	{
		if (game.operator GameState::Value() != gameT) return;

		try
		{
			ScopedAtomicBool lockRender(renderingMutex);

			lockOrThrow(getPlayerDatumWeak, getPlayerDatum);
			lockOrThrow(getObjectPhysicsWeak, getObjectPhysics);
			lockOrThrow(settingsWeak, settings);

			auto playerDatum = getPlayerDatum->getPlayerDatum();
			if (playerDatum.isNull()) return; // no player yet (menus etc) - silently skip

			auto playerPositionPtr = getObjectPhysics->getObjectPosition(playerDatum);
			if (!playerPositionPtr) return;
			const SimpleMath::Vector3 center = *playerPositionPtr;

			float baseRadius = settings->placementPointsOverlayRadius->GetValue();
			if (!(baseRadius > 0.001f)) baseRadius = 0.45f;
			const bool extended = settings->placementPointsOverlayExtended->GetValue();
			const int dirCount = extended ? kFullCount : kBaseCount;

			// V2: colour by engine validity (green valid / red invalid). Prefer the BSP-validity test (catches
			// points inside walls/solid - the false-greens); fall back to the surface test if unavailable.
			const bool wantValidity = settings->placementPointsOverlayShowValidity->GetValue();
			uintptr_t pBspFn = 0, pBspFlagsAddr = 0, pSurfaceFn = 0;
			const bool haveBsp = wantValidity
				&& placementBspValidityFunction && placementBspValidityFunction->resolve(&pBspFn) && pBspFn != 0
				&& placementBspFlagsGlobal && placementBspFlagsGlobal->resolve(&pBspFlagsAddr) && pBspFlagsAddr != 0;
			const bool haveSurface = wantValidity && !haveBsp
				&& placementSurfaceTestFunction && placementSurfaceTestFunction->resolve(&pSurfaceFn) && pSurfaceFn != 0;
			const bool useValidity = haveBsp || haveSurface;
			const uint64_t bspFlags = haveBsp ? *reinterpret_cast<uint64_t*>(pBspFlagsAddr) : 0ull;
			const uint32_t datumVal = (uint32_t)playerDatum;

			// unit_fix_position retries at the seeded radius then doubles, up to 3 tries -> radius x {1, 2, 4}.
			constexpr int kRings = 3;
			constexpr float kRingScale[kRings] = { 1.0f, 2.0f, 4.0f };
			// ring colour ramp (green -> yellow -> orange); the downward (vehicle) set is tinted blue to stand out.
			constexpr float kSphereScale = 0.06f;

			for (int ring = 0; ring < kRings; ++ring)
			{
				const float r = baseRadius * kRingScale[ring];
				for (int i = 0; i < dirCount; ++i)
				{
					// point 0 is the center = the player's own position; you can't respawn inside yourself, so skip it.
					if (i == 0) continue;

					const SimpleMath::Vector3& dir = kPlacementDirs[i];
					const SimpleMath::Vector3 point = center + (dir * r);

					SimpleMath::Vector4 color;
					if (i == 1) // the default spawn direction - the first real candidate the engine tries outside you
						color = { 1.0f, 0.84f, 0.0f, 1.0f }; // gold
					else if (useValidity)
					{
						int v; // 1 = valid, 0 = invalid, -1 = call faulted/unknown
						if (haveBsp)
						{
							const int blocked = safeBspBlocked(pBspFn, bspFlags, datumVal, point);
							if (blocked == -1) v = -1;                 // call faulted
							else if (blocked == 1) v = 0;              // in solid -> invalid
							else v = 1;                                // not in solid -> valid
						}
						else
						{
							v = safeSurfaceTest(pSurfaceFn, point);
						}
						const float dim = 1.0f - 0.18f * ring; // outer rings slightly dimmer
						if (v == 1)      color = { 0.10f * dim, 1.0f * dim, 0.10f * dim, 0.85f }; // valid -> green
						else if (v == 0) color = { 1.0f * dim, 0.15f * dim, 0.10f * dim, 0.85f }; // invalid -> red
						else             color = { 0.55f, 0.55f, 0.55f, 0.55f };                 // call faulted -> grey
					}
					else if (i >= kBaseCount) // downward / vehicle-only set
						color = { 0.2f, 0.55f + 0.15f * ring, 1.0f, 0.85f };
					else // base respawn set: green -> yellow -> orange by ring
						color = { 0.2f + 0.4f * ring, 1.0f - 0.25f * ring, 0.2f, 0.85f };

					renderer->renderSphere(point, color, kSphereScale, false);
				}
			}
		}
		catch (HCMRuntimeException ex)
		{
			PLOG_ERROR << "Placement Points Overlay rendering error: " << std::endl << ex.what();
			runtimeExceptions->handleMessage(ex);
			try
			{
				lockOrThrow(settingsWeak, settings);
				settings->placementPointsOverlayToggle->GetValueDisplay() = false;
				settings->placementPointsOverlayToggle->UpdateValueWithInput();
			}
			catch (HCMRuntimeException ex2) { runtimeExceptions->handleMessage(ex2); }
		}
	}

public:
	PlacementPointsOverlayImpl(GameState game, IDIContainer& dicon) :
		render3DEventProviderWeak(resolveDependentCheat(Render3DEventProvider)),
		getPlayerDatumWeak(resolveDependentCheat(GetPlayerDatum)),
		getObjectPhysicsWeak(resolveDependentCheat(GetObjectPhysics)),
		placementPointsOverlayToggleEventCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->placementPointsOverlayToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>())
	{
		// Resolve the validity test function (optional - only present for versions we have the offset for).
		try
		{
			auto pds = dicon.Resolve<PointerDataStore>().lock();
			if (pds)
			{
				placementSurfaceTestFunction = pds->getData<std::shared_ptr<MultilevelPointer>>(nameof(placementSurfaceTestFunction), game);
				placementBspValidityFunction = pds->getData<std::shared_ptr<MultilevelPointer>>(nameof(placementBspValidityFunction), game);
				placementBspFlagsGlobal = pds->getData<std::shared_ptr<MultilevelPointer>>(nameof(placementBspFlagsGlobal), game);
			}
		}
		catch (HCMInitException&) { placementBspValidityFunction = nullptr; }
		catch (HCMRuntimeException&) { placementBspValidityFunction = nullptr; }
		catch (...) { placementBspValidityFunction = nullptr; }
	}

	~PlacementPointsOverlayImpl()
	{
		if (renderingMutex) renderingMutex.wait(true);
		mRenderEventCallback.reset();
	}
};


PlacementPointsOverlay::PlacementPointsOverlay(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo3:
		pimpl = std::make_unique<PlacementPointsOverlayImpl<GameState::Value::Halo3>>(gameImpl, dicon);
		break;
	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<PlacementPointsOverlayImpl<GameState::Value::Halo3ODST>>(gameImpl, dicon);
		break;
	case GameState::Value::HaloReach:
		pimpl = std::make_unique<PlacementPointsOverlayImpl<GameState::Value::HaloReach>>(gameImpl, dicon);
		break;
	default:
		throw HCMInitException(std::format("{} not implemented for this game yet", nameof(PlacementPointsOverlay)));
	}
}

PlacementPointsOverlay::~PlacementPointsOverlay() = default;
