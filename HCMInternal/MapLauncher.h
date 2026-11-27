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
// from the launch block - so Play also writes that record (MCC exe: [exe+0x3F66910] = MCC's mod manager; +0x220 map id, +0x230
// the mod's GUID - see writeLobbyRecord in the .cpp for how it is recognised). Only one of the two = hang (block) or corrupt
// textures + crash (record).
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
	// Every game with MCC custom games (2026-11-19, user: "making map launcher working with all the other games"); not ODST
	// (in MCC it only has Firefight, a different lobby).
	bool launchSupported(Game g);
	// MCC's own index of a game (its catalogue / lobby screen +5832: 0 Halo 1, 1 Halo 2, 2 Halo 3, 3 Halo 4, 4 H2A, 5 ODST,
	// 6 Reach - sub_1401C960C / sub_140210DD0 in the exe), and back (Game::Count when out of range)
	int mccGameIndex(Game g);
	Game gameFromMccIndex(int i);

	struct MapEntry
	{
		Game game = Game::Halo2;
		std::string title, pack, scenario;
		bool stock = false;
		int stockIndex = -1;          // stock: MCC's map index
		uint8_t guid[16] = {};        // mod: MapGuid as a Windows GUID
		uint8_t modGuid[16] = {};     // mod: the pack's ModInfo ModGuid as a Windows GUID
		bool byId = false;            // a stock map read from MCC's catalogue (not Halo 2's built-in list): `guid` holds its
		                              // catalogue map id (node +1000) and the lobby is matched on that
	};

	// The names a map and its pack go by in the favourites / blacklist file (hcm_maplauncher_marks.txt): "m:<MapGuid hex>",
	// "s:<stock index>", "p:<ModGuid hex>", with fallbacks "mn:<game>|<pack>|<title>" / "pn:<pack>" when a GUID is missing.
	// packMarkKey is "" for an entry without a pack (stock maps).
	std::string mapMarkKey(const MapEntry& e);
	std::string packMarkKey(const MapEntry& e);

	// A gametype in MCC's catalogue (2026-11-19): what the panel's Gametype list shows and Play selects. Identified by its
	// category (MCC's gametype category id, e.g. Slayer) and its name; MCC's own variant ids are not kept (they are handed
	// out as MCC reads its content and need not be the same next session).
	struct GameVariant
	{
		int32_t category = -1;
		std::string categoryName, name;           // MCC's own strings (a stock one is a localization key such as "$team_slayer")
		std::string categoryDisplay, display;     // what MCC shows for them ("Team Slayer"); = the name when it is not a key
	};

	// MCC's display text for a string from its content (2026-11-19): "$key" -> its text in MCC's English UI string tables
	// (data\ui\Localization\EN_Global.bin, EN_Halo2.bin), turned from MCC's capitals into Title Case (acronyms kept); a key
	// the tables don't have -> the key made readable; any other string unchanged.
	std::string displayText(const std::string& s, Game game = Game::Halo2);

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
		bool canLaunch(Game g) const;   // Play can launch this game's maps here (its game supported + the hooks it needs)

		// HIDE BLACKLISTED MAPS IN MCC'S OWN MAP LIST (2026-11-18). blackKeys = every blacklisted mark key (maps and packs);
		// hideInMcc = the option. The launcher works out which of its maps that covers (again after every re-read of the
		// packs) and, while the option is on, MCC's custom game lobby leaves them out of its Choose Map list - this machine
		// only, nothing is sent to other players. See the .cpp ("Hiding maps in MCC's own map list").
		void setBlacklist(std::vector<std::string> blackKeys, bool hideInMcc);
		bool hideSupported() const;   // this MCC build has the list's filter where it is expected
		int hiddenCount() const;      // maps currently hidden in MCC's list (0 while the option is off)

		// THE GAMETYPE PLAY SELECTS (2026-11-19, user: "an option for the Gametype it loads ... always have that selected by
		// default"). gameVariants = MCC's gametypes for the lobby's game (Halo 2 until a custom game lobby has been seen), as
		// its Choose Gametype lists them by category; empty while MCC has not read them. setGametype: Play selects this one in
		// the lobby right after the map (if the map allows it); an empty name = keep the lobby's gametype (the old behaviour).
		// 2026-11-19: PER GAME (user: "bounce passion saved for h2, CTF for halo 3 etc") - Play uses the map's game's choice.
		std::vector<GameVariant> gameVariants(Game g) const;
		void setGametype(Game g, int32_t category, const std::string& name);

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
