#include "pch.h"
#include "H5HavokOverlay.h"
#include "H5GetHavokData.h"
#include "IRenderer3D.h"
#include "IModel.h"
#include "Render3DEventProvider.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include <map>
#include <string>
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "GlobalKill.h"
#include <mutex>
#include <shared_mutex>

// See H5HavokOverlay.h. All geometry comes from H5GetHavokData; nothing here reads game memory.

namespace
{
	class PieceModel : public IModelTriangles, public IModelEdges
	{
	public:
		PieceModel(const VertexCollection& v, const IndexCollection& t, const IndexCollection& e)
			: mV(v), mT(t), mE(e) {}
		const VertexCollection& getTriangleVertices() const override { return mV; }
		const IndexCollection& getTriangleIndices() const override { return mT; }
		const VertexCollection& getEdgeVertices() const override { return mV; }
		const IndexCollection& getEdgeIndices() const override { return mE; }
	private:
		const VertexCollection& mV;
		const IndexCollection& mT;
		const IndexCollection& mE;
	};
}

class H5HavokOverlay::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetHavokData> mHavokWeak;

	std::atomic<bool> mReady{ false };

	// Render3DEvent is NOT a DI service - it belongs to Render3DEventProvider. See H5TriggerOverlay.
	std::optional<std::weak_ptr<Render3DEventProvider>> mProviderOptionalWeak;
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRender3DEventCallback;
	std::shared_mutex mRenderGuard;

	std::vector<H5GetHavokData::Piece> mPieces;
	std::chrono::steady_clock::time_point mLastFailureLog{};
	// Last reported layer census, so the message fires once per distinct result rather than per rebuild.
	std::string mLastLayerCensus;
	std::chrono::steady_clock::time_point mLastRebuild{};

	void setRenderingEnabled(bool enable)
	{
		std::unique_lock<std::shared_mutex> lk(mRenderGuard);
		if (!enable)
		{
			mRender3DEventCallback.reset();
			mPieces.clear();
			mPieces.shrink_to_fit();   // this can hold megabytes; do not keep it while switched off
			return;
		}
		if (mRender3DEventCallback) return;
		if (!mProviderOptionalWeak.has_value()) return;
		auto provider = mProviderOptionalWeak.value().lock();
		if (!provider) return;
		if (provider->d3d12RendererHasFailed()) return;
		mRender3DEventCallback = provider->getRender3DEvent()->subscribe(
			[this](GameState g, IRenderer3D* r) { onRender3DEvent(g, r); });
	}

	void onToggleChanged(bool& newValue)
	{
		try { setRenderingEnabled(newValue); }
		catch (...) {}
	}

	void onRender3DEvent(GameState game, IRenderer3D* renderer)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (GlobalKill::isKillSet()) return;
		if (!renderer) return;
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge) return;

		std::shared_lock<std::shared_mutex> lk(mRenderGuard);

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (!settings->h5HavokOverlayToggle->GetValue()) return;

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			lockOrThrow(mHavokWeak, havok);

			const float radius = settings->h5HavokOverlayRadius->GetValue();
			const int budget = (int)settings->h5HavokOverlayTriangleBudget->GetValue();
			const bool wantStatic = settings->h5HavokOverlayShowStatic->GetValue();
			const bool wantInstances = settings->h5HavokOverlayShowInstances->GetValue();
			const bool wantObjects = settings->h5HavokOverlayShowObjects->GetValue();
			const float fillAlpha = settings->h5HavokOverlayFillAlpha->GetValue();
			const float wireAlpha = settings->h5HavokOverlayWireAlpha->GetValue();
			const auto camera = renderer->getCameraPosition();

			// ⚠ Re-decode on a timer, not every frame. The cull is cheap but the decode is not free, and
			// collision does not move - re-walking it at 60Hz would burn CPU for an identical picture.
			// A moving camera still gets fresh geometry within a few frames.
			const auto now = std::chrono::steady_clock::now();
			const auto interval = std::chrono::milliseconds(
				(int)std::clamp(settings->h5HavokOverlayRefreshMs->GetValue(), 16.f, 5000.f));
			if (mPieces.empty() || mLastRebuild.time_since_epoch().count() == 0 || now - mLastRebuild > interval)
			{
				mLastRebuild = now;
				havok->collect(camera, radius, budget, wantStatic, wantInstances, wantObjects, mPieces);
			}

			auto staticColour = settings->h5HavokOverlayStaticColor->GetValue();
			auto objectColour = settings->h5HavokOverlayObjectColor->GetValue();

			// ---- collision layer filter ---------------------------------------------------------------
			// ⚠ NOT EVERY COLLISION SURFACE IS ONE THE PLAYER CAN TOUCH. body+0x44 is hknp's
			// collisionFilterInfo and its low 5 bits are the LAYER; the engine decides what collides with
			// what through a 32x32 layer matrix (hknp's collisionLookupTable, which is present in this
			// build's reflection data). Halo splits static geometry across several layers - measured on the
			// test level: 0x1D carried 6 bodies and 2,270,990 triangles, 0x1C another 6, 0x1B another 4 -
			// so drawing all of them outlines bullet-only and vehicle-only surfaces the player walks
			// straight through.
			//
			// ⚠ THE MATRIX IS NOT READ HERE. Resolving hknpWorld's filter pointer and validating the table
			// needs the game running, and guessing that offset would silently filter by nonsense. So this
			// is an explicit layer list the user controls, and the census below tells them what the level
			// actually has. Empty means draw everything, which is the old behaviour exactly.
			const std::string layerFilterRaw = settings->h5HavokOverlayLayerFilter->GetValue();
			uint32_t layerMask = 0;
			bool layerFilterActive = false;
			{
				std::string tok;
				auto flush = [&]()
					{
						if (tok.empty()) return;
						try
						{
							const int v = std::stoi(tok, nullptr, 0);   // accepts 29 and 0x1D
							if (v >= 0 && v < 32) { layerMask |= (1u << v); layerFilterActive = true; }
						}
						catch (...) {}
						tok.clear();
					};
				for (char c : layerFilterRaw)
				{
					if (c == ',' || c == ' ' || c == ';') flush();
					else tok += c;
				}
				flush();
			}

			// One-shot census so the user can find out what the level has without guessing. Rebuilt only
			// when the piece set is rebuilt, and reported once per distinct result.
			{
				std::map<uint32_t, std::pair<int, size_t>> census;   // layer -> (bodies, triangles)
				for (const auto& p : mPieces)
				{
					auto& e = census[p.collisionFilter & 0x1Fu];
					e.first += 1;
					e.second += p.triangles.size() / 3;
				}
				std::string line;
				for (const auto& [layer, e] : census)
					line += std::format("{}0x{:02X}: {} bodies, {} tris", line.empty() ? "" : " | ",
						layer, e.first, e.second);
				if (!line.empty() && line != mLastLayerCensus)
				{
					mLastLayerCensus = line;
					PLOG_INFO << "Havok collision layers present: " << line;
					if (auto messagesGUI = messagesGUIWeak.lock())
						messagesGUI->addMessage("Havok layers - " + line);
				}
			}

			for (const auto& p : mPieces)
			{
				if (p.verts.empty()) continue;
				if (layerFilterActive && !(layerMask & (1u << (p.collisionFilter & 0x1Fu)))) continue;
				auto c = (p.kind == H5GetHavokData::ShapeKind::CompressedMesh) ? staticColour : objectColour;
				PieceModel model(p.verts, p.triangles, p.edges);

				if (fillAlpha > 0.f && !p.triangles.empty())
				{
					auto fill = c; fill.w *= fillAlpha;
					renderer->drawTriangleCollection(&model, fill, CullingOption::CullNone, std::nullopt);
				}
				if (wireAlpha > 0.f && !p.edges.empty())
				{
					auto wire = c; wire.w *= wireAlpha;
					renderer->drawEdgeCollection(&model, wire);
				}
			}
		}
		catch (HCMRuntimeException& ex)
		{
			onTransientFailure(ex.what());
		}
		catch (...)
		{
			onTransientFailure("unknown error");
		}
	}

	void onTransientFailure(std::string_view what) noexcept
	{
		try
		{
			mPieces.clear();
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Havok overlay skipped a frame (will retry): " << what;
			}
		}
		catch (...) {}
	}

	void onGameStateChanged(const MCCState&)
	{
		// A level change replaces the physics world outright; drop everything decoded from the old one.
		try
		{
			std::unique_lock<std::shared_mutex> lk(mRenderGuard);
			mPieces.clear();
			mLastRebuild = {};
		}
		catch (...) {}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mHavokWeak(resolveDependentCheat(H5GetHavokData)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5HavokOverlayToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5HavokOverlay only supports Halo 5: Forge");

		try { mProviderOptionalWeak = resolveDependentCheat(Render3DEventProvider); }
		catch (HCMInitException)
		{
			PLOG_ERROR << "H5HavokOverlay could not resolve Render3DEventProvider; it will not draw";
		}

		mReady.store(true, std::memory_order_release);
		try
		{
			if (auto settings = settingsWeak.lock(); settings && settings->h5HavokOverlayToggle->GetValue())
				setRenderingEnabled(true);
		}
		catch (...) {}
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
		setRenderingEnabled(false);
	}
};


H5HavokOverlay::H5HavokOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5HavokOverlay::~H5HavokOverlay() { PLOG_VERBOSE << "~" << getName(); }
