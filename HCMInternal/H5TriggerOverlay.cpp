#include "pch.h"
#include "H5TriggerOverlay.h"
#include "H5GetTriggerData.h"
#include "H5TriggerActivity.h"
#include "IRenderer3D.h"
#include "IModel.h"
#include "Render3DEventProvider.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "RenderTextHelper.h"
#include "H5GetPlayerState.h"
#include "IMakeOrGetCheat.h"
#include "GlobalKill.h"
#include "ModalDialogRenderer.h"
#include "ModalDialogFactory.h"
#include "HCETriggerNameFilterDialog.h"   // reused wholesale - see onEditNameFilter
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

// See H5TriggerOverlay.h. Geometry and categories come from H5GetTriggerData; nothing here reads game memory.

namespace
{
	// The 8 corners buildCorners emits are indexed by (sx, sy, sz) in that nesting order, so bit 2 = x sign,
	// bit 1 = y sign, bit 0 = z sign. These index lists are written against exactly that ordering - changing
	// the corner loop in H5GetTriggerData without changing these silently produces a scrambled box.
	constexpr uint16_t kBoxTriangles[36] = {
		0,1,3,  0,3,2,   // x-
		4,6,7,  4,7,5,   // x+
		0,4,5,  0,5,1,   // y-
		2,3,7,  2,7,6,   // y+
		0,2,6,  0,6,4,   // z-
		1,5,7,  1,7,3,   // z+
	};
	constexpr uint16_t kBoxEdges[24] = {
		0,1, 1,3, 3,2, 2,0,
		4,5, 5,7, 7,6, 6,4,
		0,4, 1,5, 2,6, 3,7,
	};

	// Adapts one volume's corners to the renderer's model interfaces. Stack temporary only - it holds
	// references into the caller's buffers.
	class BoxModel : public IModelTriangles, public IModelEdges
	{
	public:
		BoxModel(const VertexCollection& verts, const IndexCollection& tris, const IndexCollection& edges)
			: mVerts(verts), mTris(tris), mEdges(edges) {}
		const VertexCollection& getTriangleVertices() const override { return mVerts; }
		const IndexCollection& getTriangleIndices() const override { return mTris; }
		const VertexCollection& getEdgeVertices() const override { return mVerts; }
		const IndexCollection& getEdgeIndices() const override { return mEdges; }
	private:
		const VertexCollection& mVerts;
		const IndexCollection& mTris;
		const IndexCollection& mEdges;
	};
}

class H5TriggerOverlay::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetTriggerData> mTriggerDataWeak;
	std::weak_ptr<H5GetPlayerState> mPlayerStateWeak;
	std::weak_ptr<ModalDialogRenderer> modalDialogsWeak;
	// Optional: the overlay must still work if the activity tracker fails to construct.
	std::weak_ptr<H5TriggerActivity> mActivityWeak;

	std::atomic<bool> mReady{ false };

	// ⚠⚠ Render3DEvent IS NOT A DI SERVICE. Resolving it from the container fails with "Could not locate
	// type in DIContainer" and takes the whole cheat down as a failed optional service - which is exactly
	// what the first version of this class did. It is owned by Render3DEventProvider; resolve the PROVIDER
	// as a dependent cheat and subscribe through getRender3DEvent().
	std::optional<std::weak_ptr<Render3DEventProvider>> mProviderOptionalWeak;
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRender3DEventCallback;

	// ⚠ SUBSCRIBE ONLY WHILE THE TOGGLE IS ON. Subscribing is what makes Render3DEventProvider do its
	// per-frame camera work, and on Halo 5 that work is NOT cheap: H5GetPlayerState caches nothing, so
	// every camera read walks the thread list (CreateToolhelp32Snapshot) to find the simulation thread.
	// Paying that 60 times a second with the overlay switched off would be a pointless permanent cost.
	// Guards every read and write of mRender3DEventCallback, so a drop cannot race an in-flight frame.
	std::shared_mutex mRenderGuard;

	// Reused every frame so the render path allocates nothing.
	VertexCollection mVerts;
	IndexCollection mTris;
	IndexCollection mEdges;

	// Rate-limited failure logging; a per-frame callback must never spam.
	std::chrono::steady_clock::time_point mLastFailureLog{};

	bool categoryEnabled(const std::shared_ptr<SettingsStateAndEvents>& settings,
		H5GetTriggerData::Category c) const
	{
		using Cat = H5GetTriggerData::Category;
		switch (c)
		{
		case Cat::Kill:          return settings->h5TriggerOverlayShowKill->GetValue();
		case Cat::BeginZoneSet:  return settings->h5TriggerOverlayShowBeginZoneSet->GetValue();
		case Cat::CommitZoneSet: return settings->h5TriggerOverlayShowCommitZoneSet->GetValue();
		default:                 return settings->h5TriggerOverlayShowRegular->GetValue();
		}
	}

	// ---- name / speedrun filter ---------------------------------------------------------------------
	// Shares MCC's and HaloCER's triggerOverlayFilterString / FilterToggle / FilterExactMatch, so the same
	// picker and the same preset mean the same thing everywhere; the three games can never share a process.
	//
	// ⚠ EMPTY MEANS EVERYTHING. Arming the filter with nothing chosen must not blank the overlay and leave
	// the user hunting for what broke, so "draw nothing" gets its own value - the picker writes
	// HCETriggerNameFilterDialog::kNoneSentinel, which matches no real volume. Built once per frame.
	struct NameFilter
	{
		bool speedrunOnly = false;
		bool active = false;
		bool exactMatch = true;
		std::unordered_set<std::string> names;
		std::vector<std::string> substrings;   // lowercased
	};

	static NameFilter readNameFilter(const std::shared_ptr<SettingsStateAndEvents>& settings)
	{
		NameFilter f;
		f.speedrunOnly = settings->h5TriggerOverlaySpeedrunOnly->GetValue();
		if (!settings->triggerOverlayFilterToggle->GetValue()) return f;

		f.exactMatch = settings->triggerOverlayFilterExactMatch->GetValue();
		const std::string raw = settings->triggerOverlayFilterString->GetValue();

		std::string current;
		auto push = [&]()
			{
				while (!current.empty() && (current.back() == ' ' || current.back() == '\t')) current.pop_back();
				if (current.empty()) return;
				if (f.exactMatch) f.names.insert(current);
				else
				{
					std::string lowered;
					for (char c : current) lowered += (char)std::tolower((unsigned char)c);
					f.substrings.push_back(lowered);
				}
				current.clear();
			};
		for (char c : raw)
		{
			if (c == ';' || c == '\n' || c == '\r') push();
			else if (c != ' ' || !current.empty()) current += c;
		}
		push();

		f.active = !f.names.empty() || !f.substrings.empty();
		return f;
	}

	static bool passesNameFilter(const H5GetTriggerData::Volume& v, const NameFilter& f)
	{
		// ⚠ Speedrun-only is checked FIRST and independently of the name list, so the two compose: with both
		// armed you get "the speedrun volumes whose names also match", which is what a runner narrowing down
		// a route actually wants.
		if (f.speedrunOnly && !v.isSpeedrun) return false;
		if (!f.active) return true;

		if (f.exactMatch) return f.names.contains(v.name);

		std::string lowered;
		lowered.reserve(v.name.size());
		for (char c : v.name) lowered += (char)std::tolower((unsigned char)c);
		for (const auto& needle : f.substrings)
			if (lowered.find(needle) != std::string::npos) return true;
		return false;
	}

	SimpleMath::Vector4 categoryColour(const std::shared_ptr<SettingsStateAndEvents>& settings,
		H5GetTriggerData::Category c) const
	{
		using Cat = H5GetTriggerData::Category;
		switch (c)
		{
		case Cat::Kill:          return settings->h5TriggerOverlayKillColor->GetValue();
		case Cat::BeginZoneSet:  return settings->h5TriggerOverlayBeginZoneSetColor->GetValue();
		case Cat::CommitZoneSet: return settings->h5TriggerOverlayCommitZoneSetColor->GetValue();
		default:                 return settings->h5TriggerOverlayNormalColor->GetValue();
		}
	}

	// "Blue if a script can hit or wake it, red if inert."
	//
	// ⚠⚠ ENGINE-DRIVEN VOLUMES KEEP THEIR CATEGORY COLOUR AND ARE NEVER PAINTED INERT. Kill volumes and
	// zone set volumes are driven by the engine and are referenced by NO mission script - in the test
	// scenario all 30 kill volumes fall inside the "nothing references this" set. A naive red/blue split
	// on the script corpus alone therefore paints thirty volumes that actively kill the player as inert,
	// which is the most dangerous thing this overlay could get wrong.
	SimpleMath::Vector4 volumeColour(const std::shared_ptr<SettingsStateAndEvents>& settings,
		const H5GetTriggerData::Volume& v) const
	{
		if (!settings->h5TriggerOverlayColourByScript->GetValue())
			return categoryColour(settings, v.category);

		if (v.category != H5GetTriggerData::Category::Regular || v.isKillVolume)
			return categoryColour(settings, v.category);

		// ★ LIVE ACTIVITY BEATS THE STATIC CORPUS. If the activity tracker is up, ask it whether a script has
		// actually tested this volume recently - that is the ground truth. The corpus tier only says the volume
		// is referenced SOMEWHERE in the level's script, which scores a volume from a goal finished an hour ago
		// exactly the same as the one the current goal is waiting on.
		//
		// ⚠ Falls back to the corpus, never to "inert". If the tracker is off or its hooks are not installed,
		// nothing has been tested and every volume would read as dead - which is worse than the old answer.
		if (settings->h5TriggerOverlayUseLiveActivity->GetValue())
		{
			if (auto activity = mActivityWeak.lock())
			{
				const auto windowMs = (uint32_t)std::clamp(
					settings->h5TriggerOverlayActivityWindowMs->GetValue(), 50.f, 10000.f);
				if (activity->everTestedCount() > 0)
				{
					return activity->isActive(v.index, windowMs)
						? settings->h5TriggerOverlayScriptedColor->GetValue()
						: settings->h5TriggerOverlayInertColor->GetValue();
				}
			}
		}

		return H5TriggerVolumeNames::isScriptReachable(v.scriptTier)
			? settings->h5TriggerOverlayScriptedColor->GetValue()
			: settings->h5TriggerOverlayInertColor->GetValue();
	}

	// Subscribe / unsubscribe the 3D render event. See mRenderGuard.
	void setRenderingEnabled(bool enable)
	{
		// The activity hooks only exist to feed this overlay, so they live and die with it rather than
		// staying installed while nothing reads them.
		try { if (auto a = mActivityWeak.lock()) a->setEnabled(enable); } catch (...) {}

		std::unique_lock<std::shared_mutex> lk(mRenderGuard);
		if (!enable)
		{
			// Taken the unique lock, so any in-flight onRender3DEvent has already finished.
			mRender3DEventCallback.reset();
			return;
		}

		if (mRender3DEventCallback) return;                      // already subscribed
		if (!mProviderOptionalWeak.has_value()) return;          // no 3D path in this process
		auto provider = mProviderOptionalWeak.value().lock();
		if (!provider) return;
		// If the D3D12 renderer already latched an init failure it will never draw; do not pay the camera
		// cost for nothing.
		if (provider->d3d12RendererHasFailed()) return;

		mRender3DEventCallback = provider->getRender3DEvent()->subscribe(
			[this](GameState g, IRenderer3D* r) { onRender3DEvent(g, r); });
	}

	// ⚠ Either feature needs the subscription: hit reporting also runs from the render callback, so if the
	// overlay is off but "report entered/exited" is on we must STILL be subscribed. Keying this off the
	// draw toggle alone would silently make hit messages dead whenever drawing was off - which is the
	// combination the user is most likely to want.
	bool anyFeatureWanted() const
	{
		auto settings = settingsWeak.lock();
		if (!settings) return false;
		return settings->h5TriggerOverlayToggle->GetValue()
			|| settings->h5TriggerOverlayHitMessages->GetValue();
	}

	void onToggleChanged(bool&)
	{
		try { setRenderingEnabled(anyFeatureWanted()); }
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
			const bool wantDraw = settings->h5TriggerOverlayToggle->GetValue();
			const bool wantHits = settings->h5TriggerOverlayHitMessages->GetValue();
			if (!wantDraw && !wantHits) return;

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			lockOrThrow(mTriggerDataWeak, triggerData);
			const auto& volumes = triggerData->getVolumes();
			if (volumes.empty()) return;

			// Enter/exit reporting runs even with drawing switched off, so "tell me what I hit" is usable
			// on its own.
			if (wantHits) updateHitReporting(volumes);
			if (!wantDraw) return;

			const float alpha = settings->h5TriggerOverlayAlpha->GetValue();
			const float wireAlpha = settings->h5TriggerOverlayWireframeAlpha->GetValue();
			const float maxDistance = settings->h5TriggerOverlayRenderDistance->GetValue();
			const bool labels = settings->h5TriggerOverlayShowLabels->GetValue();
			const float labelScale = settings->h5TriggerOverlayLabelScale->GetValue();
			const ImU32 labelPacked = ImGui::ColorConvertFloat4ToU32(
				[&] { const auto c = settings->h5TriggerOverlayLabelColor->GetValue(); return ImVec4(c.x, c.y, c.z, c.w); }());

			const auto camera = renderer->getCameraPosition();
			const NameFilter nameFilter = readNameFilter(settings);

			for (const auto& v : volumes)
			{
				if (v.index < 0) continue;                       // padding slot for a skipped volume
				// Name / speedrun filter BEFORE the category toggles: an explicit list of names is the most
				// specific thing the user can have asked for and must not be quietly overruled by a category
				// toggle they forgot was off.
				if (!passesNameFilter(v, nameFilter)) continue;
				if (nameFilter.speedrunOnly || nameFilter.active) { /* filters win over categories */ }
				else if (!categoryEnabled(settings, v.category)) continue;
				// Sectors now carry real decoded geometry, so both shapes draw from the same mesh.
				if (v.meshVerts.empty() || v.meshTriangles.empty()) continue;

				// Cull by distance to the volume's SURFACE, not its centre - a large volume you are standing
				// inside has a distant centre and must not be culled.
				if (maxDistance > 0.f)
				{
					const float d = SimpleMath::Vector3::Distance(camera, v.centre) - v.boundingRadius;
					if (d > maxDistance) continue;
				}

				auto fill = volumeColour(settings, v);
				auto wire = fill;
				fill.w *= alpha;
				wire.w *= wireAlpha;

				// The reader already built world-space geometry for both shapes, so nothing here needs to
				// know whether this is a box or an extruded polygon.
				BoxModel model(v.meshVerts, v.meshTriangles, v.meshEdges);
				if (alpha > 0.f)
					renderer->drawTriangleCollection(&model, fill, CullingOption::CullNone, std::nullopt);
				if (wireAlpha > 0.f && !v.meshEdges.empty())
					renderer->drawEdgeCollection(&model, wire);

				// The NAME, at the volume's centre. Skipped for anything behind the camera, because
				// worldPointToScreenPosition would otherwise project it to a mirrored on-screen position.
				if (labels && !renderer->pointBehindCamera(v.centre))
				{
					const std::string text = v.name.empty() ? unnamedLabel(v) : v.name;
					const auto screen = renderer->worldPointToScreenPosition(v.centre, false);
					// Shrink with distance so a room full of volumes stays readable, with a floor so far
					// ones do not vanish entirely.
					const float distance = SimpleMath::Vector3::Distance(camera, v.centre);
					const float sizeScale = std::clamp(12.f / std::max(distance, 1.f), 0.4f, 1.f);
					RenderTextHelper::drawCenteredOutlinedText(text,
						SimpleMath::Vector2(screen.x, screen.y), labelPacked, labelScale * sizeScale);
				}
			}
		}
		catch (HCMRuntimeException& ex)
		{
			// Transient by default - no scenario during a load, no player while dead. Skip the frame and keep
			// the subscription; never turn the toggle off. See hcm-transient-throws-are-normal.
			onTransientFailure(ex.what());
		}
		catch (...)
		{
			onTransientFailure("unknown error");
		}
	}

	// A stable, honest label for a volume the script corpus could not name. Never invents a name - it says
	// the index and the category, which is all we actually know.
	std::string unnamedLabel(const H5GetTriggerData::Volume& v) const
	{
		return std::format("[{} #{}]", H5GetTriggerData::categoryName(v.category), v.index);
	}

	std::string describe(const H5GetTriggerData::Volume& v) const
	{
		std::string s = v.name.empty() ? unnamedLabel(v) : v.name;
		if (v.category != H5GetTriggerData::Category::Regular)
		{
			s += std::format(" ({})", H5GetTriggerData::categoryName(v.category));
			if (!v.zoneSetName.empty()) s += std::format(" -> {}", v.zoneSetName);
		}
		return s;
	}

	// ⚠⚠ THIS IS **PLAYER OCCUPANCY**, NOT SCRIPT ACTIVITY, AND THEY ARE DIFFERENT QUESTIONS.
	// It reports the volumes the player is standing inside, tested geometrically against our own box model.
	// It does NOT mean a script looked at the volume, and a volume can be entered without anything
	// happening. The script-activity signal (blue/red) is a separate piece of work that hooks Halo 5's Lua
	// volume_test_* bindings; do not conflate the two or the colouring will lie.
	//
	// Sector volumes are invisible to this, because containsPoint refuses to box-test placeholder extents.
	std::vector<bool> mInside;
	void updateHitReporting(const std::vector<H5GetTriggerData::Volume>& volumes)
	{
		auto playerState = mPlayerStateWeak.lock();
		if (!playerState) return;

		SimpleMath::Vector3 p;
		try { p = playerState->getPlayerPosition(); }
		catch (HCMRuntimeException&) { return; }   // dead / loading - not an event

		if (mInside.size() != volumes.size())
		{
			// Level changed (or first run). Re-baseline WITHOUT reporting, so a load does not spam an
			// entry message for every volume the player happens to spawn inside.
			mInside.assign(volumes.size(), false);
			for (size_t i = 0; i < volumes.size(); ++i)
				mInside[i] = volumes[i].index >= 0 && volumes[i].containsPoint(p);
			return;
		}

		auto messagesGUI = messagesGUIWeak.lock();
		if (!messagesGUI) return;

		for (size_t i = 0; i < volumes.size(); ++i)
		{
			const auto& v = volumes[i];
			if (v.index < 0) continue;
			const bool now = v.containsPoint(p);
			if (now == mInside[i]) continue;
			mInside[i] = now;
			messagesGUI->addMessage(std::format("{} {}", now ? "Entered" : "Exited", describe(v)));
		}
	}

	void onTransientFailure(std::string_view what) noexcept
	{
		try
		{
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "H5TriggerOverlay skipped a frame (will retry): " << what;
			}
		}
		catch (...) {}
	}

	void onGameStateChanged(const MCCState&)
	{
		// A level change replaces the scenario, so the snapshot and every cached index with it.
		if (auto triggerData = mTriggerDataWeak.lock())
			triggerData->invalidate();
	}

	// ---------------------------------------------------------------------------------------------------------
	// The name picker. Reuses HaloCER's dialog wholesale rather than growing a second one: it already does the
	// search box, the per-category grouping, the "Speedrun only" preset button and the green (speedrun) tag,
	// and it reads and writes the same triggerOverlayFilterString this overlay reads.
	// ---------------------------------------------------------------------------------------------------------
	void onEditNameFilter()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			auto modalDialogs = modalDialogsWeak.lock();
			if (!modalDialogs) throw HCMRuntimeException("The dialog service is unavailable.");

			std::vector<HCETriggerNameFilterDialog::Entry> entries;
			{
				lockOrThrow(mTriggerDataWeak, triggerData);
				const auto& volumes = triggerData->getVolumes();   // cached; does not re-read the game
				entries.reserve(volumes.size());
				for (const auto& v : volumes)
				{
					if (v.index < 0 || v.name.empty()) continue;   // an unnamed volume cannot be picked by name
					HCETriggerNameFilterDialog::Entry e;
					e.name = v.name;
					e.speedrun = v.isSpeedrun;
					// Mapped onto the CER dialog's categories. H5 has no CheckpointGrant or SafeZone category,
					// and a sector is a SHAPE not a category, so those list as Regular.
					e.category = v.category == H5GetTriggerData::Category::CommitZoneSet
						? HCETriggerNameFilterDialog::Category::ZoneSet
						: v.category == H5GetTriggerData::Category::BeginZoneSet
						? HCETriggerNameFilterDialog::Category::BeginZoneSet
						: v.isKillVolume
						? HCETriggerNameFilterDialog::Category::Kill
						: HCETriggerNameFilterDialog::Category::Regular;
					entries.push_back(std::move(e));
				}
			}

			if (entries.empty())
			{
				if (auto messagesGUI = messagesGUIWeak.lock())
					messagesGUI->addMessage("No trigger volumes loaded yet - turn the Trigger Overlay on, in a level.");
				return;
			}

			const std::string current = settings->triggerOverlayFilterString->GetValue();
			const std::string result = modalDialogs->showReturningDialog(   // blocking
				ModalDialogFactory::makeHCETriggerNameFilterDialog(
					"Filter Halo 5 Trigger Volumes by Name", current, std::move(entries)));

			if (result == current) return;

			settings->triggerOverlayFilterString->GetValueDisplay() = result;
			settings->triggerOverlayFilterString->UpdateValueWithInput();

			// Picking names and then seeing nothing change would read as a broken picker, so arm the filter for
			// them. Never disarm it - clearing the list back to "everything" is a reasonable thing to do while
			// leaving the filter on.
			if (!result.empty() && !settings->triggerOverlayFilterToggle->GetValue())
			{
				settings->triggerOverlayFilterToggle->GetValueDisplay() = true;
				settings->triggerOverlayFilterToggle->UpdateValueWithInput();
			}
		}
		catch (HCMRuntimeException& ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<ToggleEvent> mHitMessagesCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;
	ScopedCallback<ActionEvent> mEditNameFilterCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mTriggerDataWeak(resolveDependentCheat(H5GetTriggerData)),
		mPlayerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		modalDialogsWeak(dicon.Resolve<ModalDialogRenderer>()),
		mActivityWeak(resolveDependentCheat(H5TriggerActivity)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5TriggerOverlayToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); }),
		mHitMessagesCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5TriggerOverlayHitMessages->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState& s) { onGameStateChanged(s); }),
		mEditNameFilterCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5TriggerOverlayEditNameFilterEvent,
			[this]() { onEditNameFilter(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5TriggerOverlay only supports Halo 5: Forge");

		// ⚠ OPTIONAL, and it must stay optional. Render3DEventProvider can legitimately fail to construct
		// (no D3D12 hook yet, renderer init failed), and taking this cheat down with it would show the user
		// a service failure for a feature that simply has nothing to draw on yet.
		try
		{
			mProviderOptionalWeak = resolveDependentCheat(Render3DEventProvider);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "H5TriggerOverlay could not resolve Render3DEventProvider; the overlay will not draw";
		}

		mReady.store(true, std::memory_order_release);

		// Honour a toggle that was already on when HCM attached (presets restore it before we exist).
		try
		{
			if (anyFeatureWanted()) setRenderingEnabled(true);
		}
		catch (...) {}
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mHitMessagesCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
		// Drops the subscription under the unique lock, so it cannot free state a render callback is using.
		setRenderingEnabled(false);
	}
};


H5TriggerOverlay::H5TriggerOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5TriggerOverlay::~H5TriggerOverlay() { PLOG_VERBOSE << "~" << getName(); }
