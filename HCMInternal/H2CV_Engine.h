#pragma once
// Which Halo engine the viewer is attached to. The viewer (Viewer / Collision / Renderer) is shared; everything that
// touches a game's memory sits behind two engine layers:
//   Halo2.h  - halo2.dll    (Havok 2, welder / TIM children, backstepping)           namespace h2
//   Halo3.h  - halo3.dll / halo3odst.dll (Havok 4.5 continuous simulation, 1n tracks) namespace h3
// The host picks the game once (HCM knows it; the standalone injector detects the loaded module). Default = Halo 2, so
// a host that never calls setGame() behaves exactly as before.
#include "H2CV_Halo2.h"
#include <cstdint>
#include <string>

namespace h2cv
{
	enum class Game : uint8_t { None = 0, Halo2, Halo3, Halo3ODST, HaloReach };

	Game game();
	void setGame(Game g);
	// first of halo2.dll / halo3.dll / halo3odst.dll that is loaded in this process (standalone injector)
	Game detectGame();
	inline bool isH3() { const Game g = game(); return g == Game::Halo3 || g == Game::Halo3ODST; }
	// Halo: Reach has its own self-contained path (ViewerReach.cpp, Reach.h); the Halo 2 / 3 code never runs for it
	inline bool isReach() { return game() == Game::HaloReach; }
	const char* gameName(Game g);   // "halo2" / "halo3" / "halo3odst" (JSONL "engine" field, log lines)

	// the calls both engines answer with the same shared types
	namespace eng
	{
		uintptr_t base();
		bool buildOk();
		h2::Camera readCamera();      // the game's current observer camera (Present-time fallback when no frame was captured)
		uint32_t gameTick();
		std::string levelCode();      // current scenario / map code ("" when unreadable)
	}
}
