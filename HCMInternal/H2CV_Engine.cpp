#include "pch.h"
#include "H2CV_Engine.h"
#include "H2CV_Halo3.h"
#include "H2CV_Reach.h"
#include "H2CV_Mem.h"
#include <windows.h>
#include <atomic>

namespace h2cv
{
	namespace { std::atomic<uint8_t> gGame{ (uint8_t)Game::Halo2 }; }

	Game game() { return (Game)gGame.load(std::memory_order_relaxed); }
	void setGame(Game g) { gGame.store((uint8_t)g); }

	Game detectGame()
	{
		if (GetModuleHandleW(L"halo2.dll")) return Game::Halo2;
		if (GetModuleHandleW(L"halo3.dll")) return Game::Halo3;
		if (GetModuleHandleW(L"halo3odst.dll")) return Game::Halo3ODST;
		return Game::Halo2;
	}

	const char* gameName(Game g)
	{
		switch (g)
		{
		case Game::Halo2: return "halo2";
		case Game::Halo3: return "halo3";
		case Game::Halo3ODST: return "halo3odst";
		case Game::HaloReach: return "haloreach";
		default: return "none";
		}
	}

	namespace eng
	{
		uintptr_t base() { return isReach() ? reach::base() : isH3() ? h3::base() : h2::base(); }
		bool buildOk() { return isReach() ? reach::buildOk() : isH3() ? h3::buildOk() : h2::buildOk(); }
		h2::Camera readCamera() { return isReach() ? reach::readCamera() : isH3() ? h3::readObserverCamera() : h2::readCamera(); }
		uint32_t gameTick() { return isReach() ? 0 : isH3() ? h3::gameTick() : h2::gameTick(); }
		std::string levelCode()
		{
			if (isReach()) return "";
			if (isH3()) return h3::levelCode();
			// current level code string (halo2.dll+0xE70E68, the one AnimationFixes' cyclotron check reads)
			char s[33] = {};
			if (!h2::base() || !mem::copy(s, h2::base() + 0xE70E68, 32)) return "";
			std::string r;
			for (char c : s) { if (!c) break; r += (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '?'; }
			return r;
		}
	}
}
