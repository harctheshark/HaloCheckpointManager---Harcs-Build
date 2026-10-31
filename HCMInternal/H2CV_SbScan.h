#pragma once
// Halo 2 monster / dreadnought spot scanner, in game: the level's collision surfaces -> every candidate place a charged
// landing could launch 20+ u/s, by the engine's own contact + solver rules (port of h2sbscan.cpp,
// Documents\Halo Mod And Tools\H2 Monster Bounce Model\scanner). Runs on background threads, nearest places first;
// results are cached per map in superbounce_predicted_<map key>.csv in the game folder.
#include <cstdint>
#include <vector>
#include <string>

namespace h2cv::sbscan
{
	struct Surface { uint32_t key = 0; float n[3]{}; std::vector<float> poly; uint8_t flags = 0; };   // poly: x,y,z per vertex; flags: 1 two-sided, 2 invisible
	struct Spot
	{
		float x = 0, y = 0, z = 0, best = 0, share20 = 0, share35 = 0, fall = 0, driftX = 0, driftY = 0;   // the map scan
		// the recipe (worked out for spots near the player): the best approach found by a dense sweep (fall speed 6..30,
		// 24 drift headings x 9 drift speeds, 8 landing phases, every contact pairing) and the ranges around it that keep
		// most of the launch. heading: degrees, 0 = +x, 90 = +y (world); the drift is the horizontal velocity on landing
		bool recipe = false;
		float rBest = 0, rShare20 = 0, rShare35 = 0, rFall = 0, rDriftX = 0, rDriftY = 0, rFeet = 0;   // rFeet: feet above the surface at the tick's start
		float rLaunchX = 0, rLaunchY = 0, rLaunchZ = 0;
		float rHeadLo = 0, rHeadHi = 0, rFallLo = 0, rFallHi = 0, rDriftLo = 0, rDriftHi = 0;          // the good ranges (>= 85% of the best, 20+)
		// the START HEIGHT band of the best approach: feet above the surface at the start of the landing tick that launch 20+
		// (of the 8 tested heights spread over one tick of fall: rHeights of them work, from rFeetLo to rFeetHi)
		float rFeetLo = 0, rFeetHi = 0; int rHeights = 0;
	};

	// load this map's predictions for this tick rate (superbounce_predicted_v3[_t<tick>]_<key>.csv; no file = no spots - the map is
	// scanned only on request, by the external scanner). Same map + tick again: the file is retried if it was missing.
	void start(uint64_t mapKey, std::vector<Surface>&& surfaces, float nearX, float nearY, int tick = 30);
	// re-read the prediction file (after a manual scan merged new spots into it)
	void reload();
	// the prediction file name for a map key + tick rate (relative to the game folder)
	std::string fileName(uint64_t mapKey, int tick);
	void cancel();
	// the predicted spots found so far (clustered), and the progress (0..1; 1 = done or loaded)
	void spots(std::vector<Spot>& out);
	float progress();
	uint64_t mapKey();
	// where the player is (recipes are worked out for the spots within 30 wu, nearest first)
	void setPlayer(float x, float y, float z);
}
