#pragma once
// HCM addition (2026-11-02): the map launcher - MCC's mod map packs (and Halo 2's stock maps) in a searchable list. The chosen
// map replaces the lobby's map when the next Halo 2 custom game starts (MCC's own Start button in the custom game lobby), so the
// lobby's gametype, settings and players are kept.
//
// How MCC names the map to Halo 2 (decoded on 7 launches, MCC Steam 1.3528): the launch block MCC hands the game DLL
// (GameGlobal + 0xA8, AlphaRing's CGameGlobal::GameData) holds it at +0x10:
//   MOD map   +0x10 = -3 and +0x14..+0x23 = the map JSON's "MapGuid" as a Windows GUID
//   STOCK map +0x10 = +0x14 = MCC's map index (halo2.dll's map table, e.g. Lockout 44), +0x18 = 0x88880000, then zeros
// The block is rewritten in halo2.dll's game_start (its engine vtable slot 2, RVA 0x545C0) before the original runs.
// ⚠ For a MOD map that alone HANGS the load: MCC prepares the map (its files, textures) from the LOBBY's selection record, not
// from the launch block - so Play also writes that record (MCC exe: [exe+0x3F66910] + 0x218 = i32 -1 for a mod map, +0x220 map
// id, +0x230 the mod's GUID; +0x240 holds exe+0x3F9D600). Only one of the two = hang (block) or corrupt textures + crash (record).
// Play also STARTS the match (2026-11-02, from the MCC exe's code, offline): MCC's custom game lobby screen ("MultiplayerCustomGame",
// vtable exe+0x32BE1C0) has StartCountdown at vtable slot 55 (exe+0x4DC994) - the lobby's own Start (its checks, the countdown,
// the preload, the launch). It runs on MCC's game thread from a hook on the screen's per-frame tick (slot 11, exe+0x4DECDC).
// The list: every pack under the folders in MCC's own ModManifest.txt (%LOCALAPPDATA%Low\MCC\Config) - local and workshop.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ModuleInlineHook;

namespace MapLauncher
{
	enum class Game : int { Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4, Halo2A, Count };
	const char* gameName(Game g);
	bool launchSupported(Game g);   // Halo 2 only so far

	struct MapEntry
	{
		Game game = Game::Halo2;
		std::string title, pack, scenario;
		bool stock = false;
		int stockIndex = -1;          // stock: MCC's map index
		uint8_t guid[16] = {};        // mod: MapGuid as a Windows GUID
		uint8_t modGuid[16] = {};     // mod: the pack's ModInfo ModGuid as a Windows GUID
	};

	class Launcher
	{
	public:
		Launcher();
		~Launcher();
		void refresh();                              // re-read the packs (background thread)
		bool refreshing() const;
		std::vector<MapEntry> maps() const;
		void arm(const MapEntry& m);
		void disarm();
		bool armed(MapEntry& out) const;
		std::string message() const;
		bool hookReady() const;

		// halo2.dll game_start detour body (MCC's game thread)
		void onHalo2GameStart(uint8_t* gameData);
		// the custom game lobby's tick detour body (MCC's game thread): starts the match once when Play asked for it
		void onLobbyTick(uintptr_t screen);

	private:
		struct Impl;
		std::unique_ptr<Impl> mImpl;
		std::unique_ptr<ModuleInlineHook> mH2Hook;
		std::unique_ptr<ModuleInlineHook> mLobbyTickHook;
	};
}
