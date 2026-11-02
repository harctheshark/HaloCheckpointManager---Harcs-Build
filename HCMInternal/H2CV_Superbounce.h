#pragma once
// Superbounce viewer (Halo 2, MCC): where you will land and what that landing launches - the seams below you, monster
// gaps, dreadnought fins, the crouch charge, the recorder. Generated core: H2CV_Superbounce.cpp (port_superbounce.py).
#include <cstdint>
#include <string>
#include <vector>
#include "H2CV_Collision.h"
#include "H2CV_Renderer.h"

namespace h2cv::sb
{
	struct Options
	{
		bool on = false;           // the viewer
		bool debounce = true;      // flag landings / seams where a late contact falls through the floor (a debounce)
		bool spots = true;         // show the remembered monster / dreadnought spots
		bool predicted = true;     // show the spot scanner's predicted spots (H2CV_SbScan)
		float predictedRange = 60.f;   // ... within this distance of the player (wu)
		bool predictedMonsters = true, predictedDreadnoughts = true;   // ... of these kinds (20-35 / 35+)
		bool predictedHeights = false;  // ... with the start-height band of the worked-out approach
		bool predictedHideNarrow = false;   // ... hiding monsters whose window is under 1% (dreadnoughts always shown)
		float scanRadius = 64.f; bool scanSlow = false, scanCurrentTick = false;   // the manual spot scan ("scan around me")
		bool seamText = true;      // text in the world (off: colours and the launch arrows' speed / height only)
		bool record = false;       // superbounce_log.csv in the game folder
		bool highlight = false;    // possible monster bounces on the whole map (WIP)
		bool keepCharge = false;   // a crouch charge survives checkpoints and reverts
		bool forceCharge = false;  // the pair with the level held at -3 no matter what (overrides keepCharge)
	};
	struct Label { h2::Vec3 at; uint32_t rgba; std::string text; };       // world-anchored text
	struct PanelLine { uint32_t rgba; std::string text; };                // a line of the Info panel's superbounce section
	struct Output
	{
		std::vector<LineVertex> thin, thick;   // world lines: 1.5 px / 3 px, drawn without occlusion
		std::vector<Label> labels;
		std::vector<PanelLine> panel;
		int hud = -1;                          // -1 nothing to show, 0 not charged, 1 charged
		float speed = -1.f;                    // your speed (u/s) for the HUD; -1 = none
		int streak = 0, record = 0;            // bounces in a row (this chain) and the level's record
		std::string message;                   // one-off notice for HCM's message list ("Charge kept through the revert")
	};
	// once per frame, render thread: reads the local player and the level (world must be up to date), fills out
	void frame(const CollisionWorld& world, const Options& opt, Output& out);
	void reset();   // unload / game change
	void forgetCheckpointCharges();   // HCM injected a checkpoint: forget which checkpoints were made charged (any thread)
	void requestSpotScan();           // the "scan around me" button (any thread): scanned on the next frame
	void noteForceLaunch();           // HCM's Force Launch just set the player's velocity (any thread): not a bounce / spot
}
