#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) scenario trigger-volume ACTIVITY tracker.
//
// Answers one question for a viewer: "is the mission script currently testing this trigger volume?", so live
// volumes can be drawn green and dormant ones red.
//
// THE GROUND TRUTH IS THE TEST CALL, NOT A STORED FLAG. There is no per-volume runtime enable bit worth reading
// (scenario_runtime_trigger_volume_flags exists but is a zone-set/BSP gate, not "a script is watching this one").
// A volume is live iff a running HaloScript thread is evaluating it, so:
//
//     active(v)  ==  (GetTickCount() - lastTested[v]) < window
//
// We learn lastTested by patching the eight HaloScript CALL SITES of trigger_volume_test_point - never the
// function prologue. See HCETriggerActivity.cpp for why that distinction is the entire feature.
//
// Nothing is patched until setEnabled(true) is called, and no game memory is touched by the constructor.
// ================================================================================================================
class HCETriggerActivity : public IOptionalCheat
{
private:
	class HCETriggerActivityImpl;
	std::unique_ptr<HCETriggerActivityImpl> pimpl;

public:
	HCETriggerActivity(GameState game, IDIContainer& dicon);
	~HCETriggerActivity();
	std::string_view getName() override { return nameof(HCETriggerActivity); }

	// true if this scenario trigger-volume index was tested by a script within the active window.
	// Never throws. Returns false when disabled, when the index was never tested, or when the index is out of
	// range. Safe from any thread.
	bool isVolumeActive(uint32_t volumeIndex) const;

	// true if a script tested this volume and the test PASSED within the last flashMilliseconds.
	//
	// ACTIVE and HIT are different questions and must not be conflated. Active is "a script is watching this",
	// and stays true for a multi-second window because scripts poll in bursts. Hit is "you were inside it when
	// the script looked", and is an instantaneous event - which is why the caller passes its own, much shorter,
	// flash duration. Before this existed the viewer could only show active, so entering a volume looked like
	// the volume slowly going dormant several seconds later.
	bool isVolumeHit(uint32_t volumeIndex, uint32_t flashMilliseconds) const;

	// Raw last-hit timestamp for this volume; 0 means never hit. For building an edge-triggered "volumes hit"
	// list: keep your own previous value per volume and report the ones that changed. Kept out of this class
	// deliberately - it cannot know when a level changed and would otherwise carry stale per-scenario state.
	uint32_t getLastHitTick(uint32_t volumeIndex) const;

	// Enable/disable the call-site patches. Safe to call repeatedly and from a non-game thread.
	// Never throws - a failure (unrecognised game build, allocation failure) is logged and reported through
	// RuntimeExceptionHandler, and the patches simply stay off.
	void setEnabled(bool enabled);

	// Whether the call-site patches are currently installed.
	bool isEnabled() const;
};
