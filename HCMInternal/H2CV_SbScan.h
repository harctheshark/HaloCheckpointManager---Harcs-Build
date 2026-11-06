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
		float rAtX = 0, rAtY = 0;   // where on the 3x3 refine patch the best approach lands (world x, y)
	};

	// load this map's predictions for this tick rate (superbounce_predicted_v3[_t<tick>]_<key>.csv; no file = no spots - the map is
	// scanned only on request, by the external scanner). Same map + tick again: the file is retried if it was missing.
	void start(uint64_t mapKey, std::vector<Surface>&& surfaces, float nearX, float nearY, int tick = 30);
	// re-read the prediction file (after a manual scan merged new spots into it)
	void reload();
	// Halo 3 / ODST (2026-11-02): a second, per-map prediction file read with the key file - the offline whole-map scan of the
	// map's BSPs (superbounce_predicted_gen3_<map>[_t<tick>].csv; world coordinates, so it holds whichever BSPs are loaded).
	// "" = none (Halo 2). Set before start().
	// prefix: "gen3" (Halo 3 / ODST, the default) or "reach" - the file is superbounce_predicted_<prefix>_<map>[_t<tick>].csv
	void setMapFile(const std::string& mapName, const char* prefix = "gen3");
	// the monster / dreadnought lines (u/s) at a tick rate: 20 / 35 at 30 tick, 35 / 60 at 60 tick (user, 2026-11-01: 60-tick
	// normal bounces ~24, monsters 35+; the 60-tick dreadnought line is provisional), linear between, clamped outside
	inline float lineMonster(int tps) { const float t = tps < 30 ? 30.f : (tps > 60 ? 60.f : (float)tps); return 20.f + (t - 30.f) * 0.5f; }
	inline float lineDreadnought(int tps) { const float t = tps < 30 ? 30.f : (tps > 60 ? 60.f : (float)tps); return 35.f + (t - 30.f) * (25.f / 30.f); }
	// the tick rate of the loaded predictions (their share columns use that tick's lines)
	int tick();
	// the prediction file name for a map key + tick rate (relative to the game folder)
	std::string fileName(uint64_t mapKey, int tick);
	void cancel();
	// the predicted spots found so far (clustered), and the progress (0..1; 1 = done or loaded)
	void spots(std::vector<Spot>& out);
	// only the spots within r wu (horizontally) of x, y; total = how many the map has (a whole ODST map's file holds ~175k -
	// copying them all every tick cost ~20 MB a tick, 2026-11-02)
	void spots(std::vector<Spot>& out, float x, float y, float r, size_t& total);
	float progress();
	uint64_t mapKey();
	// one landing with the scan's exact rules on the level last handed to start(): the pill's feet at the start of the landing
	// tick and its velocity for that tick (gravity already added). false: the level is not ready, nothing is touched, the tick
	// before was not clear (it would have landed earlier) or only two-sided / invisible surfaces are touched. launch = the
	// solved velocity (any direction). Thread-safe (the level is read-only once built); costs ~10-30 us.
	bool landingLaunch(float fx, float fy, float fz, float vx, float vy, float vz, float launch[3]);
	// the current job's serial if it is ready (0: none / not ready): a caller on another thread reads it before and after a run
	// of landingLaunch calls and drops the results if it changed (start() swapped the job / its tick rate meanwhile)
	uint64_t readySerial();
	// where the player is
	void setPlayer(float x, float y, float z);
	// recipes (each spot's best approach + launch direction) are worked out ONLY on request (2026-11-03): every spot within r
	// (x / y) and 30 wu (z) of x, y, z, nearest first, in the background; saved to the map's .recipes.csv as they finish
	void requestRecipes(float x, float y, float z, float r);
	void cancelRecipes();
	bool recipeStatus(int& done, int& total);   // true while a request runs; done / total of it
	// the GPU path (2026-11-03): the spots of such a request that still lack a recipe (nearest first), and the merge of the GPU tool's
	// rows (h2sbrecipe_gpu, the same sweep - verified identical to this file's recipe on headlong) into them + the map's recipe cache
	// all = every spot in range (the "Redo approaches here" button), not only those without a recipe
	std::vector<Spot> recipeTargets(float x, float y, float z, float r, bool all = false);
	// overwrite = replace the recipes the spots already have (each row once per run: a replaced spot is not replaced again)
	int mergeRecipes(const std::string& path, bool overwrite = false, int run = 0);   // run: the redo run (a new run replaces again)
}
