#include "pch.h"
#include "MapLauncher.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include <ShlObj.h>
#include <algorithm>
#include <array>
#include <set>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <iterator>

namespace MapLauncher
{
	namespace
	{
		constexpr int64_t kH2GameStartRva = 0x545C0;   // halo2.dll engine vtable slot 2 (CreateGameEngine -> 0x180BE3480), 1.3528
		const uint8_t kH2GameStartBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };

		// MCC's custom game lobby screen (MCC Steam 1.3528, exe md5 f6ebac11): its per-frame tick (vtable slot 11) and
		// StartCountdown (slot 55), the class's vtable
		constexpr int64_t kLobbyTickRva = 0x4DECDC;
		const uint8_t kLobbyTickBytes[] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
		constexpr uintptr_t kCustomLobbyVtableRva = 0x32BE1C0, kStartCountdownRva = 0x4DC994;
		constexpr int kStartCountdownSlot = 55;
		constexpr uint64_t kStartTimeoutMs = 3000;

		std::atomic<Launcher*> sInstance{ nullptr };
		ModuleInlineHook* sH2Hook = nullptr;
		ModuleInlineHook* sLobbyTickHook = nullptr;

		// Halo 2's stock multiplayer maps: MCC's map index (halo2.dll's map table) and the usual name
		struct Stock { int index; const char* title; const char* scenario; };
		const Stock kH2Stock[] = {
			{ 45, "Ascension", "ascension" }, { 60, "Backwash", "backwash" }, { 48, "Beaver Creek", "beavercreek" },
			{ 49, "Burial Mounds", "burial_mounds" }, { 52, "Coagulation", "coagulation" }, { 50, "Colossus", "colossus" },
			{ 56, "Containment", "containment" }, { 47, "Ivory Tower", "cyclotron" }, { 58, "Sanctuary", "deltatap" },
			{ 63, "Relic", "dune" }, { 61, "Elongation", "elongation" }, { 55, "Foundation", "foundation" },
			{ 62, "Gemini", "gemini" }, { 53, "Headlong", "headlong" }, { 44, "Lockout", "lockout" },
			{ 46, "Midship", "midship" }, { 68, "Uplift", "needle" }, { 67, "District", "street_sweeper" },
			{ 64, "Terminal", "triplicate" }, { 59, "Turf", "turf" }, { 57, "Warlock", "warlock" },
			{ 54, "Waterworks", "waterworks" }, { 51, "Zanzibar", "zanzibar" }, { 66, "Tombstone", "highplains" },
			{ 65, "Desolation", "derelict" },
		};

		bool bytesMatch(uintptr_t at, const uint8_t* expect, size_t n)
		{
			uint8_t got[32] = {};
			if (n > sizeof(got)) return false;
			__try { memcpy(got, (const void*)at, n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			return memcmp(got, expect, n) == 0;
		}

		// ---- the few JSON fields needed (MCC's mod files are plain, machine-written JSON) ----
		std::string readFileUtf8(const std::filesystem::path& p)
		{
			std::ifstream f(p, std::ios::binary);
			if (!f) return {};
			std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (s.size() >= 2 && (uint8_t)s[0] == 0xFF && (uint8_t)s[1] == 0xFE)   // UTF-16 LE
			{
				const int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)(s.data() + 2), (int)(s.size() - 2) / 2, nullptr, 0, nullptr, nullptr);
				std::string o(n, '\0');
				WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)(s.data() + 2), (int)(s.size() - 2) / 2, o.data(), n, nullptr, nullptr);
				return o;
			}
			if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
			return s;
		}

		// the string value of "key" at or after `from` (escapes: \" \\ \/ \n, \uXXXX kept only for ASCII)
		bool jsonString(const std::string& s, const char* key, std::string& out, size_t from = 0, size_t* endAt = nullptr)
		{
			const std::string k = std::string("\"") + key + "\"";
			size_t p = s.find(k, from);
			if (p == std::string::npos) return false;
			p = s.find(':', p + k.size());
			if (p == std::string::npos) return false;
			p = s.find('"', p + 1);
			if (p == std::string::npos) return false;
			out.clear();
			for (++p; p < s.size() && s[p] != '"'; ++p)
			{
				if (s[p] == '\\' && p + 1 < s.size())
				{
					const char c = s[++p];
					if (c == 'n') out += ' ';
					else if (c == 'u' && p + 4 < s.size()) { const int v = std::stoi(s.substr(p + 1, 4), nullptr, 16); if (v < 128) out += (char)v; p += 4; }
					else out += c;
				}
				else out += s[p];
			}
			if (endAt) *endAt = p;
			return true;
		}

		// "Title": { "Neutral": "..." }
		std::string jsonTitle(const std::string& s)
		{
			const size_t t = s.find("\"Title\"");
			std::string v;
			if (t != std::string::npos && jsonString(s, "Neutral", v, t)) return v;
			return {};
		}

		bool parseGuid(const std::string& g, uint8_t out[16])
		{
			unsigned long d1 = 0; unsigned int d2 = 0, d3 = 0, b[8] = {};
			if (sscanf_s(g.c_str(), "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) return false;
			const uint32_t a = (uint32_t)d1; const uint16_t c2 = (uint16_t)d2, c3 = (uint16_t)d3;
			memcpy(out, &a, 4); memcpy(out + 4, &c2, 2); memcpy(out + 6, &c3, 2);
			for (int i = 0; i < 8; ++i) out[8 + i] = (uint8_t)b[i];
			return true;
		}

		bool gameFromEngine(const std::string& e, Game& g)
		{
			static const std::pair<const char*, Game> m[] = { { "Halo1", Game::Halo1 }, { "Halo2", Game::Halo2 }, { "Halo3", Game::Halo3 },
				{ "Halo3ODST", Game::Halo3ODST }, { "HaloReach", Game::HaloReach }, { "Halo4", Game::Halo4 }, { "Halo2A", Game::Halo2A } };
			for (const auto& [n, v] : m) if (e == n) { g = v; return true; }
			return false;
		}

		std::filesystem::path modManifest()
		{
			PWSTR low = nullptr;
			std::filesystem::path p;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &low))) p = std::filesystem::path(low) / L"MCC" / L"Config" / L"ModManifest.txt";
			if (low) CoTaskMemFree(low);
			return p;
		}

		std::vector<MapEntry> scanAll()
		{
			std::vector<MapEntry> out;
			for (const auto& s : kH2Stock)
			{
				MapEntry e; e.game = Game::Halo2; e.title = s.title; e.pack = "Halo 2 (stock)"; e.scenario = s.scenario; e.stock = true; e.stockIndex = s.index;
				out.push_back(e);
			}
			std::istringstream roots(readFileUtf8(modManifest()));
			std::string line;
			std::error_code ec;
			while (std::getline(roots, line))
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
				if (line.empty()) continue;
				const int wn = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0);
				std::wstring wl(wn, L'\0'); MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), wl.data(), wn);
				const std::filesystem::path root(wl);
				for (const auto& packDir : std::filesystem::directory_iterator(root, ec))
				{
					if (!packDir.is_directory(ec)) continue;
					const std::string info = readFileUtf8(packDir.path() / L"ModInfo.json");
					std::string engine;
					Game game;
					if (info.empty() || !jsonString(info, "Engine", engine) || !gameFromEngine(engine, game)) continue;
					std::string pack = jsonTitle(info);
					if (pack.empty()) pack = packDir.path().filename().string();
					std::string modGuidText;
					uint8_t modGuid[16] = {};
					if (!jsonString(info, "ModGuid", modGuidText) || !parseGuid(modGuidText, modGuid)) continue;
					for (const auto& jf : std::filesystem::directory_iterator(packDir.path() / L"multiplayer", ec))
					{
						if (jf.path().extension() != L".json") continue;
						const std::string j = readFileUtf8(jf.path());
						std::string guid;
						MapEntry e; e.game = game; e.pack = pack;
						memcpy(e.modGuid, modGuid, 16);
						if (!jsonString(j, "MapGuid", guid) || !parseGuid(guid, e.guid)) continue;
						jsonString(j, "ScenarioFile", e.scenario);
						e.title = jsonTitle(j);
						if (e.title.empty()) e.title = e.scenario;
						out.push_back(std::move(e));
					}
				}
			}
			std::sort(out.begin(), out.end(), [](const MapEntry& a, const MapEntry& b) {
				if (a.game != b.game) return (int)a.game < (int)b.game;
				if (a.stock != b.stock) return a.stock;   // stock first
				return _stricmp(a.title.c_str(), b.title.c_str()) < 0; });
			return out;
		}

		// the map field of Halo 2's launch block (no C++ objects here: __try). 1 written, 0 not a multiplayer launch (the lobby's
		// gametype name at +0x304 is empty - campaign), -1 unreadable
		int writeHalo2Map(uint8_t* gd, bool stock, int stockIndex, const uint8_t* guid, int32_t& lobbyMap)
		{
			__try
			{
				if (gd[0x304] == 0) return 0;
				lobbyMap = *(int32_t*)(gd + 0x10);
				// a stock map from a lobby whose map is a mod's (-3): MCC has prepared that mod's files - not swapped (2)
				if (stock && lobbyMap == -3) return 2;
				if (stock)
				{
					const uint32_t tail[4] = { (uint32_t)stockIndex, 0x88880000u, 0u, 0u };
					*(int32_t*)(gd + 0x10) = stockIndex;
					memcpy(gd + 0x14, tail, sizeof(tail));
				}
				else
				{
					*(int32_t*)(gd + 0x10) = -3;
					memcpy(gd + 0x14, guid, 16);
				}
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// MCC's "lobby record" (what MCC prepares the map from; MCC Steam 1.3528, found by a pointer scan 2026-11-02, decoded
		// from the exe 2026-11-10): [exe + 0x3F66910] is MCC's MOD MANAGER (built by sub_14208A0CC, 0x240 bytes; i32 state at +0,
		// 2 = ready - sub_14031E0C0). +0x220 / +0x230 are its map -> mod pair: sub_14208B684(manager, mapGuid) sets them, under
		// the mutex at +0x1D0, when a mod map is looked up (the lobby's selection), and the launch reads the pair. Play writes
		// it: +0x220 = the map's MapGuid, +0x230 = its pack's ModGuid.
		// ⚠ What the first version got wrong (the launcher said "lobby selection was not found" in a session with other packs):
		//   - its "signature" at +0x240 is PAST THE END of the object - the neighbouring heap block, different every session;
		//   - its "+0x218 = -1 means mod map" is the mutex's owner thread id (-1 = unlocked): always -1, and never to be written.
		// Recognised now by what MCC itself puts in the object: state 2 and, at +0x60, the std::function MCC installs when it
		// sets this global (sub_14031E1A4: its vtable off_1432951B0, .rdata).
		// 1 written, 0 the manager is not there / not ready / not recognised, -1 unreadable. state / cb = what was seen (logged).
		constexpr uintptr_t kLobbyRecordPtr = 0x3F66910, kModManagerCallbackVt = 0x32951B0;
		int writeLobbyRecord(const uint8_t* mapGuid, const uint8_t* modGuid, int32_t& state, uintptr_t& cb)
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			state = -1; cb = 0;
			__try
			{
				const uintptr_t obj = *(uintptr_t*)(exe + kLobbyRecordPtr);
				if (obj < 0x10000) return 0;
				state = *(int32_t*)obj;
				cb = *(uintptr_t*)(obj + 0x60) - exe;
				if (state != 2 || cb != kModManagerCallbackVt) return 0;
				memcpy((void*)(obj + 0x220), mapGuid, 16);
				memcpy((void*)(obj + 0x230), modGuid, 16);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// the lobby tick takes (screen, delta time) - Hex-Rays drops the float; it is passed through as it came
		int64_t __fastcall lobbyTickDetour(int64_t screen, float dt)
		{
			const int64_t r = sLobbyTickHook ? sLobbyTickHook->getInlineHook().call<int64_t>(screen, dt) : 0;
			if (Launcher* self = sInstance.load()) self->onLobbyTick((uintptr_t)screen);
			return r;
		}

		int32_t readI32(uintptr_t at, int32_t fallback)
		{
			__try { return *(int32_t*)at; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
		}

		// the screen is MCC's custom game lobby (its class's vtable, whose slot 55 is StartCountdown): that function, else 0
		uintptr_t customLobbyStart(uintptr_t screen)
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			__try
			{
				const uintptr_t vt = *(uintptr_t*)screen;
				if (vt != exe + kCustomLobbyVtableRva) return 0;
				const uintptr_t fn = *(uintptr_t*)(vt + 8 * kStartCountdownSlot);
				return fn == exe + kStartCountdownRva ? fn : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		// ---- Selecting the map IN THE LOBBY (2026-11-10: so the choice reaches the other players of a LAN / online lobby) ----
		// The first launcher swapped the map only on this machine (the launch block + the mod manager's pair): another player
		// in the lobby was still sent the lobby's own map. What MCC does when the host picks a map and presses Start (exe
		// 1.3528, offline RE in MCC Map Launcher\db):
		//   - the screen keeps the selection as ids into MCC's catalogue [exe+0x4001650]: +5832 game (0..6), +5836 map,
		//     +5840 map variant, +5844 gametype category, +5848 game variant;
		//   - the catalogue's map list for a game is a linked list headed at catalogue + 264 + 16*game (next at node+8); a node:
		//     +16 state (>= 2 usable), +20 kind (1 stock, 5 mod, 3/4 saved variants), +28 id, +1000 its 16-byte map id (a mod
		//     map's MapGuid; a stock map's {u32 MCC index, u32 0x88880000, 0}), +1016 i32 map index (-3 mod);
		//   - picking a map in the UI (sub_1404D7160, page 0) is: +5836 = id; sub_1404D6C1C (its variant -> +5840); vtable slot
		//     66 sub_1404D6D54 (keeps / replaces the gametype so it is one the map supports); sub_1404D4C8C (team settings);
		//   - StartCountdown (slot 55) then does everything from +5840: checks the map is installed (for a mod map that is the
		//     mod manager lookup that sets the pair above), preloads it, and calls slot 56 sub_1404DD30C, which writes the map's
		//     id into the NETWORK SESSION (sub_14034D344 -> HaloNetworkLayer) - what every player's game loads from.
		// So Play now makes the UI's own selection and calls StartCountdown; nothing is swapped at the launch any more.
		constexpr uintptr_t kCatalogueRva = 0x4001650, kPickVariantRva = 0x4D6C1C, kLobbyApplyRva = 0x4D4C8C, kFixGametypeRva = 0x4D6D54;
		constexpr int kFixGametypeSlot = 66;
		const uint8_t kPickVariantBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83 };
		const uint8_t kLobbyApplyBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
		const uint8_t kFixGametypeBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };

		// result: 1 selected, 0 the map is not in the lobby's catalogue, -1 a read faulted, -2 not the MCC build this was made for
		struct LobbySelect { int result = 0; int32_t game = -1, id = -1, kind = -1, matches = 0, oldMap = -1, oldVariant = -1, newVariant = -1, oldGametype = -1, newGametype = -1; int broadcast = 0; };

		// ---- Telling the OTHER PLAYERS (2026-11-11, first LAN test: "the player doesn't have the map" although he did) ----
		// StartCountdown's check (sub_140251354) wants, for a mod map, the bit 0x80000 in every player's properties
		// (HaloNetworkLayer_Ship_59(player) + 4). Each player's own game sets that bit (sub_140256520) when the mod manager
		// knows the map id in ITS copy of the lobby state block ([exe+0x4001590], 2488 bytes: map title +836, +1092, map id
		// +1876). The host fills that block and sends it (sub_14024DDE0) when its map tile is refreshed - vtable slot 49
		// sub_1404D8820, which reads the screen's own map id at +5864 (set when a map is highlighted, slot 47). The first
		// version did neither, so nobody was ever told. Now: +5864 = the node's map id, slot 49 (the tile shows the map, the
		// block is sent), and Play WAITS until every player's bit is set before StartCountdown.
		constexpr uintptr_t kRefreshMapTileRva = 0x4D8820, kLobbyStateRva = 0x4001590, kBroadcastRva = 0x24DDE0;
		constexpr int kRefreshMapTileSlot = 49;
		const uint8_t kRefreshMapTileBytes[] = { 0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0x00, 0x01 };
		const uint8_t kBroadcastBytes[] = { 0x48, 0x83, 0xEC, 0x28, 0xFF, 0x15, 0x66, 0xC7, 0x61, 0x02, 0x84, 0xC0, 0x74, 0x15, 0xE8, 0x01 };
		// the check's own calls (their operands name the two HaloNetworkLayer imports): Ship_221 (who is in the session) and
		// Ship_59 (a player's properties)
		constexpr uintptr_t kPeerMaskCallRva = 0x2514A6, kPeerInfoCallRva = 0x2514E6;
		const uint8_t kPeerMaskCallBytes[] = { 0xFF, 0x15, 0x1C, 0x90, 0x61, 0x02 };
		const uint8_t kPeerInfoCallBytes[] = { 0xFF, 0x15, 0xAC, 0x90, 0x61, 0x02 };

		// how many players have not (yet) reported the selected mod map: >= 0, or -1 when it cannot be told. POD-only (SEH).
		int playersMissingMap(int& players)
		{
			players = 0;
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			if (!bytesMatch(exe + kPeerMaskCallRva, kPeerMaskCallBytes, sizeof(kPeerMaskCallBytes))
				|| !bytesMatch(exe + kPeerInfoCallRva, kPeerInfoCallBytes, sizeof(kPeerInfoCallBytes))) return -1;
			__try
			{
				const uintptr_t maskFn = *(const uintptr_t*)(exe + kPeerMaskCallRva + 6 + 0x0261901C);
				const uintptr_t infoFn = *(const uintptr_t*)(exe + kPeerInfoCallRva + 6 + 0x026190AC);
				if (maskFn < 0x10000 || infoFn < 0x10000) return -1;
				uint64_t mask[8] = {};
				typedef char(__fastcall* MaskFn)(int64_t, int64_t, int64_t, int64_t, uint64_t*, int64_t, int64_t, int64_t);
				typedef uintptr_t(__fastcall* InfoFn)(unsigned int);
				if (!reinterpret_cast<MaskFn>(maskFn)(0, 0, 0, 0, mask, 0, 0, 0)) return -1;
				const uint32_t m = (uint32_t)mask[0] & 0x1FFFF;
				int missing = 0;
				for (unsigned int i = 0; i < 17; ++i)
				{
					if (!((m >> i) & 1)) continue;
					const uintptr_t info = reinterpret_cast<InfoFn>(infoFn)(i);
					if (info < 0x10000) continue;
					++players;
					if (!(*(const uint32_t*)(info + 4) & 0x80000u)) ++missing;
				}
				return missing;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// a usable catalogue node that is this map (called inside the SEH guards below)
		bool nodeIsMap(uintptr_t n, bool stock, int stockIndex, const uint8_t* guid, bool byId)
		{
			if (*(const int32_t*)(n + 16) < 2) return false;
			if (!stock || byId) return memcmp((const void*)(n + 1000), guid, 16) == 0;
			return *(const uint32_t*)(n + 1000) == (uint32_t)stockIndex && *(const uint32_t*)(n + 1004) == 0x88880000u;
		}

		// POD-only (SEH). Runs on MCC's game thread (the lobby's tick).
		void selectInLobby(uintptr_t screen, bool stock, int stockIndex, const uint8_t* guid, bool byId, LobbySelect& r)
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			if (!bytesMatch(exe + kPickVariantRva, kPickVariantBytes, sizeof(kPickVariantBytes))
				|| !bytesMatch(exe + kLobbyApplyRva, kLobbyApplyBytes, sizeof(kLobbyApplyBytes))
				|| !bytesMatch(exe + kFixGametypeRva, kFixGametypeBytes, sizeof(kFixGametypeBytes))) { r.result = -2; return; }
			__try
			{
				const uintptr_t vt = *(const uintptr_t*)screen;
				if (*(const uintptr_t*)(vt + 8 * kFixGametypeSlot) != exe + kFixGametypeRva) { r.result = -2; return; }
				const uintptr_t cat = *(const uintptr_t*)(exe + kCatalogueRva);
				const int32_t game = *(const int32_t*)(screen + 5832);
				r.game = game;
				if (cat < 0x10000 || game < 0 || game > 6) { r.result = 0; return; }
				const uintptr_t head = cat + 264 + 16ull * (uint32_t)game;
				const int32_t wantKind = stock ? 1 : 5;
				uintptr_t best = 0; int32_t bestKind = -1; int guard = 0;
				for (uintptr_t n = *(const uintptr_t*)(head + 8); n != head && n > 0x10000 && guard < 200000; n = *(const uintptr_t*)(n + 8), ++guard)
				{
					if (!nodeIsMap(n, stock, stockIndex, guid, byId)) continue;
					++r.matches;
					const int32_t kind = *(const int32_t*)(n + 20);
					if (!best || (kind == wantKind && bestKind != wantKind)) { best = n; bestKind = kind; }
				}
				if (!best && stock && !byId)   // a stock node whose id is not in the {index, 0x88880000} form: by its map index
				{
					guard = 0;
					for (uintptr_t n = *(const uintptr_t*)(head + 8); n != head && n > 0x10000 && guard < 200000; n = *(const uintptr_t*)(n + 8), ++guard)
						if (*(const int32_t*)(n + 16) >= 2 && *(const int32_t*)(n + 20) == 1 && *(const int32_t*)(n + 1016) == stockIndex) { best = n; bestKind = 1; ++r.matches; break; }
				}
				if (!best) { r.result = 0; return; }
				const int32_t id = *(const int32_t*)(best + 28);
				r.id = id; r.kind = bestKind;
				r.oldMap = *(const int32_t*)(screen + 5836); r.oldVariant = *(const int32_t*)(screen + 5840); r.oldGametype = *(const int32_t*)(screen + 5848);

				*(int32_t*)(screen + 5836) = id;
				memcpy((void*)(screen + 5864), (const void*)(best + 1000), 16);   // the screen's own map id (as highlighting a map does)
				reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(exe + kPickVariantRva)(screen);
				// the variant MCC picked must be one of this map's - else the map's own node
				const int32_t v = *(const int32_t*)(screen + 5840);
				bool variantOk = false; guard = 0;
				for (uintptr_t n = *(const uintptr_t*)(head + 8); n != head && n > 0x10000 && guard < 200000; n = *(const uintptr_t*)(n + 8), ++guard)
					if (*(const int32_t*)(n + 28) == v) { variantOk = nodeIsMap(n, stock, stockIndex, guid, byId) || n == best; break; }
				if (!variantOk) *(int32_t*)(screen + 5840) = id;
				reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(exe + kFixGametypeRva)(screen);
				reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(exe + kLobbyApplyRva)(screen);
				// the map tile + the lobby state block, sent to the other players (slot 49)
				if (*(const uintptr_t*)(vt + 8 * kRefreshMapTileSlot) == exe + kRefreshMapTileRva
					&& memcmp((const void*)(exe + kRefreshMapTileRva), kRefreshMapTileBytes, sizeof(kRefreshMapTileBytes)) == 0)
				{
					reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(exe + kRefreshMapTileRva)(screen);
					r.broadcast = 1;
				}
				// slot 49 skips the block when the tile is not on screen: then the map id is put in the block and sent here
				const uintptr_t st = *(const uintptr_t*)(exe + kLobbyStateRva);
				if (st > 0x10000 && memcmp((const void*)(st + 1876), (const void*)(best + 1000), 16) != 0
					&& memcmp((const void*)(exe + kBroadcastRva), kBroadcastBytes, sizeof(kBroadcastBytes)) == 0)
				{
					memcpy((void*)(st + 1876), (const void*)(best + 1000), 16);
					reinterpret_cast<int64_t(__fastcall*)()>(exe + kBroadcastRva)();
					r.broadcast = 2;
				}
				r.newVariant = *(const int32_t*)(screen + 5840); r.newGametype = *(const int32_t*)(screen + 5848);
				r.result = 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { r.result = -1; }
		}

		// ---- Selecting the GAMETYPE in the lobby (2026-11-19, user: "an option for the Gametype it loads ... save whatever
		// option you last chose") - MCC exe 1.3528, offline RE in MCC Map Launcher\db (q60-q65):
		//   - the catalogue's gametype CATEGORIES for a game: a list at catalogue + 152 + 16*game (next at node+8; lookup
		//     sub_14043DCD0 by +20). A node: +20 category id, +24 its name (an MCC string: +20 length, +24 char*), +312 count,
		//     +320 u32* the ids of its game variants;
		//   - the GAME VARIANTS: a list at catalogue + 40 + 16*game (lookup sub_14043DC94 by +28). A node: +16 state, +28 id,
		//     +88 its name (MCC string), +1000 the variant, +1104 its category;
		//   - MCC's Choose Gametype page (populateGameTypeVariant, sub_1404D5F34) lists the category's ids that
		//     sub_14043E768(catalogue, game, screen+5840 = the map variant, id) allows. Picking one (sub_1404D7160, page 3) is
		//     +5848 = id, +5856 = sub_1404E1188(game, id), sub_1404D4C8C(screen); the category page sets +5844 first. Slot 52
		//     (sub_1404D925C) redraws the lobby's gametype tile.
		// So Play does exactly that after selecting the map (whose slot 66 may already have swapped the gametype for one the
		// map supports), and only if the map allows the chosen one. StartCountdown then launches what the screen holds.
		constexpr uintptr_t kVariantAllowedRva = 0x43E768, kVariantDataRva = 0x4E1188, kRefreshGametypeTileRva = 0x4D925C;
		constexpr int kRefreshGametypeTileSlot = 52;
		const uint8_t kVariantAllowedBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
		const uint8_t kVariantDataBytes[] = { 0x48, 0x83, 0xEC, 0x28, 0x83, 0xFA, 0xFF, 0x75, 0x07, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3 };
		const uint8_t kRefreshGametypeTileBytes[] = { 0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x48 };

		// an MCC string's text into out (at most n-1 bytes). POD-only: call inside an SEH guard.
		void copyMccString(uintptr_t s, char* out, size_t n)
		{
			out[0] = 0;
			const int32_t len = *(const int32_t*)(s + 20);
			const char* p = *(const char* const*)(s + 24);
			if ((uintptr_t)p < 0x10000 || len <= 0 || len > 4096) return;
			size_t k = 0;
			for (; k + 1 < n && k < (size_t)len && p[k]; ++k) out[k] = p[k];
			out[k] = 0;
		}

		// the game variant node with this id in a game's list, or 0. POD-only: inside an SEH guard.
		uintptr_t variantNode(uintptr_t cat, int32_t game, int32_t id)
		{
			const uintptr_t head = cat + 40 + 16ull * (uint32_t)game;
			int guard = 0;
			for (uintptr_t n = *(const uintptr_t*)(head + 8); n != head && n > 0x10000 && guard < 100000; n = *(const uintptr_t*)(n + 8), ++guard)
				if (*(const int32_t*)(n + 28) == id) return n;
			return 0;
		}

		struct RawVariant { int32_t category; int32_t id; char cat[96]; char name[128]; };

		// every (category, variant) MCC's Choose Gametype could list for a game, in its order (map compatibility not
		// applied - that depends on the map). Only reads; any thread (a list MCC rewrites meanwhile = a fault, caught).
		int readVariants(int32_t game, RawVariant* out, int cap)
		{
			int n = 0;
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			__try
			{
				const uintptr_t cat = *(const uintptr_t*)(exe + kCatalogueRva);
				if (cat < 0x10000 || game < 0 || game > 6) return 0;
				const uintptr_t head = cat + 152 + 16ull * (uint32_t)game;
				int guard = 0;
				for (uintptr_t c = *(const uintptr_t*)(head + 8); c != head && c > 0x10000 && guard < 4096 && n < cap; c = *(const uintptr_t*)(c + 8), ++guard)
				{
					const uint32_t count = *(const uint32_t*)(c + 312);
					const uint32_t* ids = *(const uint32_t* const*)(c + 320);
					if (!count || count > 4096 || (uintptr_t)ids < 0x10000) continue;
					char cname[96];
					copyMccString(c + 24, cname, sizeof(cname));
					for (uint32_t i = 0; i < count && n < cap; ++i)
					{
						const uintptr_t v = variantNode(cat, game, (int32_t)ids[i]);
						if (!v) continue;
						RawVariant& r = out[n];
						copyMccString(v + 88, r.name, sizeof(r.name));
						if (!r.name[0]) continue;
						r.category = *(const int32_t*)(c + 20);
						r.id = (int32_t)ids[i];
						memcpy(r.cat, cname, sizeof(cname));
						++n;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return n;
		}

		// Every game's STOCK maps (2026-11-19): the catalogue's map list for a game (catalogue + 264 + 16*game), nodes of kind
		// 1 (stock) and state >= 2 - what MCC's Choose Map lists (populateChooseMap sub_1404D55C0: name = node +88, an MCC
		// string, usually a "$key"; id = node +1000, 16 bytes; MCC map index = node +1016). Only reads; any thread. A game
		// whose content MCC has not read yet simply has no nodes.
		struct RawStock { int32_t index; uint8_t id[16]; char name[128]; };
		int readCatalogueStock(int32_t game, RawStock* out, int cap)
		{
			int n = 0;
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			__try
			{
				const uintptr_t cat = *(const uintptr_t*)(exe + kCatalogueRva);
				if (cat < 0x10000 || game < 0 || game > 6) return 0;
				const uintptr_t head = cat + 264 + 16ull * (uint32_t)game;
				int guard = 0;
				for (uintptr_t m = *(const uintptr_t*)(head + 8); m != head && m > 0x10000 && guard < 200000 && n < cap; m = *(const uintptr_t*)(m + 8), ++guard)
				{
					if (*(const int32_t*)(m + 16) < 2 || *(const int32_t*)(m + 20) != 1) continue;
					RawStock& r = out[n];
					copyMccString(m + 88, r.name, sizeof(r.name));
					if (!r.name[0]) continue;
					memcpy(r.id, (const void*)(m + 1000), 16);
					r.index = *(const int32_t*)(m + 1016);
					++n;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return n;
		}

		// result: 1 selected, 2 it already was the lobby's, 0 no gametype of that name in the lobby's list, 3 the lobby's map
		// does not allow it, -1 a read faulted, -2 not the MCC build this was made for. POD-only (SEH). MCC's game thread.
		struct GametypeSelect { int result = 0; int32_t oldId = -1, newId = -1, category = -1; char current[128] = {}; };
		void selectGametypeInLobby(uintptr_t screen, int32_t wantCategory, const char* wantName, GametypeSelect& r)
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			if (!bytesMatch(exe + kVariantAllowedRva, kVariantAllowedBytes, sizeof(kVariantAllowedBytes))
				|| !bytesMatch(exe + kVariantDataRva, kVariantDataBytes, sizeof(kVariantDataBytes))
				|| !bytesMatch(exe + kLobbyApplyRva, kLobbyApplyBytes, sizeof(kLobbyApplyBytes))) { r.result = -2; return; }
			__try
			{
				const uintptr_t cat = *(const uintptr_t*)(exe + kCatalogueRva);
				const int32_t game = *(const int32_t*)(screen + 5832);
				if (cat < 0x10000 || game < 0 || game > 6) { r.result = 0; return; }
				r.oldId = *(const int32_t*)(screen + 5848);
				if (const uintptr_t cur = variantNode(cat, game, r.oldId)) copyMccString(cur + 88, r.current, sizeof(r.current));
				// the variant of that name - in the saved category if it has one there, else in any category
				const uintptr_t head = cat + 152 + 16ull * (uint32_t)game;
				int32_t bestId = -1, bestCat = -1;
				int guard = 0;
				char name[128];
				for (uintptr_t c = *(const uintptr_t*)(head + 8); c != head && c > 0x10000 && guard < 4096; c = *(const uintptr_t*)(c + 8), ++guard)
				{
					const uint32_t count = *(const uint32_t*)(c + 312);
					const uint32_t* ids = *(const uint32_t* const*)(c + 320);
					if (!count || count > 4096 || (uintptr_t)ids < 0x10000) continue;
					const int32_t cid = *(const int32_t*)(c + 20);
					for (uint32_t i = 0; i < count; ++i)
					{
						const uintptr_t v = variantNode(cat, game, (int32_t)ids[i]);
						if (!v) continue;
						copyMccString(v + 88, name, sizeof(name));
						if (strcmp(name, wantName) != 0) continue;
						if (bestId < 0 || (cid == wantCategory && bestCat != wantCategory)) { bestId = (int32_t)ids[i]; bestCat = cid; }
					}
				}
				if (bestId < 0) { r.result = 0; return; }
				r.newId = bestId; r.category = bestCat;
				if (r.oldId == bestId && *(const int32_t*)(screen + 5844) == bestCat) { r.result = 2; return; }
				typedef char(__fastcall* AllowedFn)(uintptr_t, int32_t, int32_t, int32_t);
				if (!reinterpret_cast<AllowedFn>(exe + kVariantAllowedRva)(cat, game, *(const int32_t*)(screen + 5840), bestId)) { r.result = 3; return; }
				*(int32_t*)(screen + 5844) = bestCat;
				*(int32_t*)(screen + 5848) = bestId;
				*(int64_t*)(screen + 5856) = reinterpret_cast<int64_t(__fastcall*)(int32_t, int32_t)>(exe + kVariantDataRva)(game, bestId);
				reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(exe + kLobbyApplyRva)(screen);
				// the lobby's gametype tile (UI only)
				const uintptr_t vt = *(const uintptr_t*)screen;
				if (*(const uintptr_t*)(vt + 8 * kRefreshGametypeTileSlot) == exe + kRefreshGametypeTileRva
					&& memcmp((const void*)(exe + kRefreshGametypeTileRva), kRefreshGametypeTileBytes, sizeof(kRefreshGametypeTileBytes)) == 0)
					reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(exe + kRefreshGametypeTileRva)(screen);
				r.result = 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { r.result = -1; }
		}

		// ---- Changing maps from INSIDE a game (2026-11-21, user: "in game on a custom headlong map ... choose custom terminal,
		// and press play, it should either put me back to the menu and instantly start countdown, or load me directly") ----
		// MCC exe 1.3528, offline RE (MCC Map Launcher\db q80-q84): the MP pause menu (sub_1404ECE18) confirms "End Game"
		// ($DIALOG_END_GAME_SUMMARY, message 0x39D73B7) with sub_1401EDC18(): byte_144000B99 = 1, then the GAME ENGINE
		// (qword_144000BA0 = the game DLL's CreateGameEngine object; halo2.dll's vtable 0x180BE3480, slot 3 forwards to the
		// game thread's object at engine+0x28) ->vtable[3](13) = end the game (12 = restart match, 2 = leave). Only if
		// byte_144000B97 (MCC's pause menu is open) does it also run sub_1401EDAE8, which closes MCC's pause screens - UI work,
		// so Play refuses while that menu is open and otherwise makes exactly MCC's own call. The engine pointer is zeroed
		// when the game is torn down (sub_1401EE2E8), which is how "the game is gone" is told. Loading the new map straight
		// into the running game is not done: MCC prepares a mod map's files from the lobby (see writeLobbyRecord) - the
		// match is ended, the lobby comes back, and Play's lobby selection + StartCountdown run from there.
		constexpr uintptr_t kGameEngineRva = 0x4000BA0, kMccPauseMenuOpenRva = 0x4000B97, kEndGameRva = 0x1EDC18;
		const uint8_t kEndGameBytes[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, 0x7D, 0x2F, 0xE1, 0x03, 0x45, 0x33, 0xC0, 0xC6, 0x05 };
		constexpr uint64_t kEndGameTimeoutMs = 120000;   // the postgame screen can take a while
		constexpr uint64_t kLobbySettleMs = 2000;        // the lobby must tick this long before Play acts in it

		// 1 a game engine exists (a game is running or being torn down), 0 none, -1 unreadable
		int gameEngineExists()
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			__try { return *(const uintptr_t*)(exe + kGameEngineRva) != 0 ? 1 : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// 1 ended (MCC's own End Game call), 0 MCC's pause menu is open (refused), -1 no game / unreadable, -2 another build
		int endCurrentGame()
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			if (!bytesMatch(exe + kEndGameRva, kEndGameBytes, sizeof(kEndGameBytes))) return -2;
			__try
			{
				if (*(const uintptr_t*)(exe + kGameEngineRva) == 0) return -1;
				if (*(const uint8_t*)(exe + kMccPauseMenuOpenRva) != 0) return 0;
				reinterpret_cast<int64_t(__fastcall*)()>(exe + kEndGameRva)();
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		// does the launch block already name this map? 1 yes, 0 no, -1 unreadable, -2 not a multiplayer launch
		int blockHoldsMap(const uint8_t* gd, bool stock, int stockIndex, const uint8_t* guid, int32_t& lobbyMap)
		{
			__try
			{
				if (gd[0x304] == 0) return -2;
				lobbyMap = *(const int32_t*)(gd + 0x10);
				if (stock) return lobbyMap == stockIndex ? 1 : 0;
				return lobbyMap == -3 && memcmp(gd + 0x14, guid, 16) == 0 ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		int64_t __fastcall h2GameStartDetour(int64_t engine, int64_t gameManager, int64_t gameData)
		{
			if (Launcher* self = sInstance.load()) self->onHalo2GameStart(reinterpret_cast<uint8_t*>(gameData));
			return sH2Hook ? sH2Hook->getInlineHook().call<int64_t>(engine, gameManager, gameData) : 0;
		}

		// ---- Hiding maps in MCC's OWN map list (2026-11-18; MCC Steam 1.3528, offline RE in MCC Map Launcher\db, q40-q44) ----
		// The custom game lobby fills its Choose Map list in vtable slot 39 (sub_1404D55C0, "populateChooseMap"): it walks the
		// catalogue's map list for the lobby's game and, for every usable stock / mod map node the gametype allows, asks the
		// screen's vtable slot 70 - (*vt + 560)(screen, node) = sub_1404E0F9C, 29 bytes: "screen+5904 or +5903 set ? yes :
		// sub_14043EEB4(node)" - whether to list it. The map-variant list (sub_1404D58D8) and the lobby's own choice of a map
		// when it opens (sub_1404DE7D8) ask the same slot. Two vtables hold that function: the custom game lobby's and the
		// class after it (exe+0x32BE518).
		// So the slot is pointed at mapFilterDetour: "no" for a hidden map, MCC's own answer for every other. A vtable slot
		// is one aligned pointer - written in one go; none of MCC's code is changed, and taking it back is the same write.
		// Only what MCC LISTS changes: a hidden map is still in the catalogue, stays the lobby's map if it was, can still be
		// started by Play, and other players are told nothing.
		constexpr uintptr_t kMapFilterRva = 0x4E0F9C, kLobbyVtable2Rva = 0x32BE518;
		constexpr int kMapFilterSlot = 70;
		const uint8_t kMapFilterBytes[] = { 0x80, 0xB9, 0x10, 0x17, 0x00, 0x00, 0x00, 0x75, 0x09, 0x80, 0xB9, 0x0F, 0x17, 0x00, 0x00, 0x00,
			0x74, 0x03, 0xB0, 0x01, 0xC3, 0x48, 0x8B, 0xCA };

		struct HideKeys
		{
			std::vector<std::array<uint8_t, 16>> ids;   // sorted: a catalogue node's 16-byte map id (+1000)
			std::vector<int32_t> stock;                 // MCC map indices (for a stock node whose id is not in the usual form)
		};
		std::mutex sHideMutex;
		std::shared_ptr<const HideKeys> sHide;          // null = nothing is hidden

		// POD-only (SEH): what identifies a catalogue node
		bool readNodeId(uintptr_t node, uint8_t* id, int32_t& kind, int32_t& index)
		{
			__try { memcpy(id, (const void*)(node + 1000), 16); kind = *(const int32_t*)(node + 20); index = *(const int32_t*)(node + 1016); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool nodeHidden(uintptr_t node)
		{
			std::shared_ptr<const HideKeys> keys;
			{ std::scoped_lock lk(sHideMutex); keys = sHide; }
			if (!keys) return false;
			std::array<uint8_t, 16> id{}; int32_t kind = 0, index = 0;
			if (!readNodeId(node, id.data(), kind, index)) return false;
			if (std::binary_search(keys->ids.begin(), keys->ids.end(), id)) return true;
			return kind == 1 && std::find(keys->stock.begin(), keys->stock.end(), index) != keys->stock.end();
		}
		// MCC's game thread (the lobby builds its lists there). Anything going wrong on our side = MCC's own answer.
		char __fastcall mapFilterDetour(int64_t screen, int64_t node)
		{
			bool hidden = false;
			try { hidden = nodeHidden((uintptr_t)node); } catch (...) {}
			if (hidden) return 0;
			return reinterpret_cast<char(__fastcall*)(int64_t, int64_t)>((uintptr_t)GetModuleHandleW(nullptr) + kMapFilterRva)(screen, node);
		}
		// POD-only (SEH)
		bool readPtr(uintptr_t at, uintptr_t& out)
		{
			__try { out = *(const uintptr_t*)at; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// the filter's code is the build this was made for, and the custom game lobby's slot holds it (or already ours)
		bool mapFilterRecognised()
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			uintptr_t cur = 0;
			return bytesMatch(exe + kMapFilterRva, kMapFilterBytes, sizeof(kMapFilterBytes))
				&& readPtr(exe + kCustomLobbyVtableRva + 8 * kMapFilterSlot, cur)
				&& (cur == exe + kMapFilterRva || cur == (uintptr_t)&mapFilterDetour);
		}
		// Points the two slots at our filter (on) or back at MCC's (off). A slot holding anything else is left alone.
		// Returns how many slots are now as asked.
		int setMapFilter(bool on)
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			if (!bytesMatch(exe + kMapFilterRva, kMapFilterBytes, sizeof(kMapFilterBytes))) return 0;
			const uintptr_t mcc = exe + kMapFilterRva, ours = (uintptr_t)&mapFilterDetour;
			const uintptr_t from = on ? mcc : ours, to = on ? ours : mcc;
			int done = 0;
			for (const uintptr_t vt : { exe + kCustomLobbyVtableRva, exe + kLobbyVtable2Rva })
			{
				const uintptr_t slot = vt + 8 * kMapFilterSlot;
				uintptr_t cur = 0;
				if (!readPtr(slot, cur)) continue;
				if (cur == to) { ++done; continue; }
				if (cur != from) continue;
				DWORD old = 0;
				if (!VirtualProtect((LPVOID)slot, 8, PAGE_READWRITE, &old)) continue;
				InterlockedExchange64((volatile LONG64*)slot, (LONG64)to);
				DWORD ignored = 0;
				VirtualProtect((LPVOID)slot, 8, old, &ignored);
				++done;
			}
			return done;
		}

		bool guidBlank(const uint8_t* g) { for (int i = 0; i < 16; ++i) if (g[i]) return false; return true; }
		std::string hexOf(const uint8_t* g, int n)
		{
			static const char* d = "0123456789abcdef";
			std::string s;
			for (int i = 0; i < n; ++i) { s += d[g[i] >> 4]; s += d[g[i] & 15]; }
			return s;
		}
	}

	std::string mapMarkKey(const MapEntry& e)
	{
		const auto byName = [&e] { return "mn:" + std::to_string((int)e.game) + "|" + e.pack + "|" + e.title; };
		// a stock map read from MCC's catalogue (2026-11-19: every game's): its game + catalogue map id
		if (e.stock && e.byId) return guidBlank(e.guid) ? byName() : "c:" + std::to_string((int)e.game) + ":" + hexOf(e.guid, 16);
		if (e.stock) return e.stockIndex >= 0 ? "s:" + std::to_string(e.stockIndex) : byName();
		if (!guidBlank(e.guid)) return "m:" + hexOf(e.guid, 16);
		return byName();
	}
	std::string packMarkKey(const MapEntry& e)
	{
		if (e.stock) return "";
		if (!guidBlank(e.modGuid)) return "p:" + hexOf(e.modGuid, 16);
		return e.pack.empty() ? "" : "pn:" + e.pack;
	}

	const char* gameName(Game g)
	{
		static const char* n[] = { "Halo 1", "Halo 2", "Halo 3", "ODST", "Reach", "Halo 4", "H2A" };
		return (int)g >= 0 && (int)g < (int)Game::Count ? n[(int)g] : "?";
	}

	bool launchSupported(Game g) { return g != Game::Halo3ODST && (int)g >= 0 && g < Game::Count; }

	int mccGameIndex(Game g)
	{
		switch (g)
		{
		case Game::Halo1: return 0; case Game::Halo2: return 1; case Game::Halo3: return 2; case Game::Halo4: return 3;
		case Game::Halo2A: return 4; case Game::Halo3ODST: return 5; case Game::HaloReach: return 6; default: return -1;
		}
	}
	Game gameFromMccIndex(int i)
	{
		static const Game m[] = { Game::Halo1, Game::Halo2, Game::Halo3, Game::Halo4, Game::Halo2A, Game::Halo3ODST, Game::HaloReach };
		return i >= 0 && i < 7 ? m[i] : Game::Count;
	}

	struct Launcher::Impl
	{
		mutable std::mutex m;
		std::vector<MapEntry> maps;
		bool isArmed = false; MapEntry armedMap;
		bool lobbySelected = false;   // the armed map was selected in the lobby itself (MCC launches it; nothing to swap)
		std::string msg;
		std::atomic<bool> startPending{ false };
		// the map is selected and sent; StartCountdown waits for every player to report it (onLobbyTick)
		std::atomic<bool> waitingPlayers{ false };
		std::atomic<uint64_t> waitSinceMs{ 0 };
		std::string waitNote;   // appended to the "Started" message (under m)
		std::atomic<uint64_t> startAskedMs{ 0 };
		// Play from inside a game (2026-11-21): the game was ended; wait for the lobby, then select + start there
		std::atomic<bool> endingGame{ false };
		std::atomic<uint64_t> lobbyBackMs{ 0 };   // first lobby tick seen after the game was gone (0 = not yet)
		std::atomic<bool> busy{ false };
		std::thread worker;

		// the blacklist as the panel last gave it (under m), and how many maps it hides in MCC's own list right now
		std::set<std::string> blackKeys;
		bool hideInMcc = false;
		std::atomic<int> hidden{ 0 };

		// the gametype Play selects (under m; empty name = keep the lobby's), and the game of the last custom game lobby seen
		// (MCC's catalogue index; 1 = Halo 2 until a lobby has ticked)
		std::array<int32_t, (size_t)Game::Count> gtCategory{ -1, -1, -1, -1, -1, -1, -1 };   // per game (Game order)
		std::array<std::string, (size_t)Game::Count> gtName;
		std::atomic<int32_t> lobbyGame{ 1 };
		// every game's stock maps but Halo 2's (it has its built-in list), from MCC's catalogue; re-read every few seconds
		std::vector<MapEntry> catStock;
		uint64_t catStockMs = 0;

		// Works out which of `maps` the blacklist covers and installs (or removes) MCC's list filter. Any thread.
		void applyHidden()
		{
			auto keys = std::make_shared<HideKeys>();
			bool on = false;
			{
				std::scoped_lock lk(m);
				on = hideInMcc;
				if (on)
				{
					constexpr int G = (int)Game::Count;
					int total[G] = {}, black[G] = {}; bool knowsStock[G] = {};
					std::vector<const MapEntry*> covered;
					for (const MapEntry& e : maps)
					{
						const int g = (int)e.game;
						if (g < 0 || g >= G) continue;
						++total[g];
						if (e.stock) knowsStock[g] = true;
						const std::string pk = packMarkKey(e);
						if (blackKeys.count(mapMarkKey(e)) || (!pk.empty() && blackKeys.count(pk))) { ++black[g]; covered.push_back(&e); }
					}
					for (const MapEntry* e : covered)
					{
						// A game whose EVERY map is blacklisted keeps them all: MCC's lobby must have something to list. (Only
						// where this list has the game's stock maps too - elsewhere MCC always has those left.)
						const int g = (int)e->game;
						if (knowsStock[g] && black[g] >= total[g]) continue;
						std::array<uint8_t, 16> id{};
						if (e->stock && e->byId)
						{
							if (guidBlank(e->guid)) continue;
							memcpy(id.data(), e->guid, 16);   // the catalogue's own map id
						}
						else if (e->stock)
						{
							if (e->stockIndex < 0) continue;
							const uint32_t form[4] = { (uint32_t)e->stockIndex, 0x88880000u, 0u, 0u };   // a stock map's id in the catalogue
							memcpy(id.data(), form, 16);
							keys->stock.push_back(e->stockIndex);
						}
						else
						{
							if (guidBlank(e->guid)) continue;
							memcpy(id.data(), e->guid, 16);
						}
						keys->ids.push_back(id);
					}
					std::sort(keys->ids.begin(), keys->ids.end());
					keys->ids.erase(std::unique(keys->ids.begin(), keys->ids.end()), keys->ids.end());
				}
			}
			const int n = (int)keys->ids.size();
			int now = 0;
			if (on && n > 0)
			{
				{ std::scoped_lock lk(sHideMutex); sHide = keys; }
				if (setMapFilter(true) > 0) now = n;
				else { std::scoped_lock lk(sHideMutex); sHide.reset(); }
			}
			else
			{
				setMapFilter(false);
				std::scoped_lock lk(sHideMutex); sHide.reset();
			}
			if (hidden.exchange(now) != now)
				PLOG_INFO << "[MapLauncher] MCC's own map list: " << now << " blacklisted map(s) hidden" << (on && n > 0 && now == 0 ? " (the list's filter was not found in this MCC build)" : "");
		}
	};

	Launcher::Launcher() : mImpl(std::make_unique<Impl>())
	{
		// Attached on halo2.dll load (MCC loads it when the first Halo 2 game starts) and left attached for the session. Not
		// armed when a loaded halo2.dll's game_start is not the expected code (another build, or another tool's hook there).
		const HMODULE h2 = GetModuleHandleW(L"halo2.dll");
		if (h2 && !bytesMatch((uintptr_t)h2 + kH2GameStartRva, kH2GameStartBytes, sizeof(kH2GameStartBytes)))
		{
			PLOG_ERROR << "[MapLauncher] halo2.dll game_start bytes differ - Halo 2 launching disabled";
			mImpl->msg = "Halo 2 launching unavailable (unexpected halo2.dll build)";
		}
		else
		{
			auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ kH2GameStartRva });
			mH2Hook = ModuleInlineHook::make(L"halo2.dll", ptr, (void*)&h2GameStartDetour);
			sH2Hook = mH2Hook.get();
			mH2Hook->setWantsToBeAttached(true);
		}
		// the custom game lobby's tick: Play starts the match from it (on MCC's game thread). Without it Play only arms.
		const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
		if (bytesMatch(exe + kLobbyTickRva, kLobbyTickBytes, sizeof(kLobbyTickBytes)))
		{
			auto tick = std::make_shared<MultilevelPointerSpecialisation::ExeOffset>(std::vector<int64_t>{ kLobbyTickRva });
			mLobbyTickHook = ModuleInlineHook::make(L"main", tick, (void*)&lobbyTickDetour);
			sLobbyTickHook = mLobbyTickHook.get();
			mLobbyTickHook->setWantsToBeAttached(true);
		}
		else PLOG_ERROR << "[MapLauncher] MCC's custom game lobby tick bytes differ - Play will only arm (press MCC's Start yourself)";
		sInstance = this;
		refresh();
	}

	Launcher::~Launcher()
	{
		sInstance = nullptr;
		if (mImpl->worker.joinable()) mImpl->worker.join();
		// MCC's map list filter back to MCC's own (the slot must not point into HCM once this is gone)
		setMapFilter(false);
		{ std::scoped_lock lk(sHideMutex); sHide.reset(); }
		// the hook detaches with mH2Hook; the detour reads sH2Hook only while it is set
		if (mH2Hook) { mH2Hook->setWantsToBeAttached(false); }
		if (mLobbyTickHook) { mLobbyTickHook->setWantsToBeAttached(false); }
		sH2Hook = nullptr;
		sLobbyTickHook = nullptr;
	}

	void Launcher::refresh()
	{
		if (mImpl->busy.exchange(true)) return;
		if (mImpl->worker.joinable()) mImpl->worker.join();
		mImpl->worker = std::thread([impl = mImpl.get()]() {
			std::vector<MapEntry> m;
			try { m = scanAll(); }
			catch (const std::exception& ex) { PLOG_ERROR << "[MapLauncher] scan failed: " << ex.what(); }
			size_t mods = 0; for (const auto& e : m) mods += !e.stock;
			PLOG_INFO << "[MapLauncher] " << m.size() << " maps (" << mods << " from mod packs)";
			{ std::scoped_lock lk(impl->m); impl->maps = std::move(m); }
			impl->applyHidden();   // the blacklist may cover maps that were only just read
			impl->busy = false;
		});
	}

	void Launcher::setBlacklist(std::vector<std::string> blackKeys, bool hideInMcc)
	{
		{
			std::scoped_lock lk(mImpl->m);
			mImpl->blackKeys = std::set<std::string>(blackKeys.begin(), blackKeys.end());
			mImpl->hideInMcc = hideInMcc;
		}
		mImpl->applyHidden();
	}
	// ---- MCC's UI string tables (2026-11-19, user: show "Slayer / Team Slayer / Swords", not "$slayer") ----
	// MCC exe 1.3528, offline RE (MCC Map Launcher\db q66-q69, loc_probe.py): the UI dictionary (UIDictionary::LoadDictionary,
	// sub_1408CAD1C) loads data\ui\Localization\<LANG>_<Game>.bin. File: at 0x70 a self-relative u64 -> { u32 bytes of
	// hashes, u32 bytes of text, u32 hashes[], then the texts NUL-terminated in the same order } (the text size can run a
	// few bytes past the file end - clamped). A key's hash (sub_14088E340): CRC-32 MSB-first (poly 0x04C11DB7) over the key
	// WITHOUT its '$', a-z upper-cased, seed 0xFFFFFFFF, no final xor. Verified offline: slayer -> "SLAYER", team_slayer ->
	// "TEAM SLAYER", GAMETYPE_SLAYER -> "SLAYER" (all in EN_Global.bin). English only (MCC's language is not read).
	namespace
	{
		uint32_t mccKeyHash(const std::string& key)
		{
			static const auto table = [] {
				std::array<uint32_t, 256> t{};
				for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i << 24; for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1; t[i] = c; }
				return t;
			}();
			uint32_t a = 0xFFFFFFFFu;
			for (unsigned char ch : key)
			{
				if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32);
				a = (a << 8) ^ table[((a >> 24) ^ ch) & 0xFF];
			}
			return a;
		}

		void loadLocFile(const std::filesystem::path& p, std::unordered_map<uint32_t, std::string>& out)
		{
			std::ifstream f(p, std::ios::binary);
			if (!f) return;
			const std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			auto u32 = [&d](size_t o) { uint32_t v = 0; memcpy(&v, d.data() + o, 4); return v; };
			if (d.size() < 0x78) return;
			uint64_t q = 0; memcpy(&q, d.data() + 0x70, 8);
			if (q > d.size()) return;
			const size_t base = 0x70 + (size_t)q;
			if (base + 8 > d.size()) return;
			const size_t nh = u32(base), nt = u32(base + 4);
			const size_t hashes = base + 8, texts = hashes + nh;
			if (nh % 4 || texts > d.size()) return;
			const size_t end = std::min(d.size(), texts + nt);
			size_t t = texts;
			for (size_t i = 0; i < nh / 4 && t < end; ++i)
			{
				const size_t z = d.find('\0', t);
				const size_t e = (z == std::string::npos || z > end) ? end : z;
				out.emplace(u32(hashes + 4 * i), d.substr(t, e - t));
				t = e + 1;
			}
		}

		// per game (2026-11-19): EN_<Game>.bin (that game's own strings, first) + EN_Global.bin; loaded once per game
		const std::unordered_map<uint32_t, std::string>& locTable(Game game)
		{
			static std::mutex mtx;
			static std::array<std::unique_ptr<std::unordered_map<uint32_t, std::string>>, (size_t)Game::Count> tables;
			static const char* files[] = { "Halo1", "Halo2", "Halo3", "Halo3ODST", "HaloReach", "Halo4", "Halo2A" };   // Game order
			const size_t g = (int)game >= 0 && game < Game::Count ? (size_t)game : (size_t)Game::Halo2;
			std::scoped_lock lk(mtx);
			if (!tables[g])
			{
				auto t = std::make_unique<std::unordered_map<uint32_t, std::string>>();
				try
				{
					wchar_t exePath[MAX_PATH] = {};
					GetModuleFileNameW(nullptr, exePath, MAX_PATH);
					// <MCC>\MCC\Binaries\Win64\MCC-Win64-Shipping.exe -> <MCC>\data\ui\Localization
					const auto dir = std::filesystem::path(exePath).parent_path().parent_path().parent_path().parent_path() / "data" / "ui" / "Localization";
					loadLocFile(dir / (std::string("EN_") + files[g] + ".bin"), *t);   // the game's own first (emplace keeps the first)
					loadLocFile(dir / "EN_Global.bin", *t);
					PLOG_INFO << "[MapLauncher] MCC UI strings for " << files[g] << ": " << t->size() << " from " << dir.string();
				}
				catch (...) { t->clear(); }
				tables[g] = std::move(t);
			}
			return *tables[g];
		}

		// MCC's capitals -> Title Case; all-caps acronyms of MCC's gametypes stay
		std::string titleCase(const std::string& s)
		{
			bool hasLower = false;
			for (unsigned char c : s) if (c >= 'a' && c <= 'z') { hasLower = true; break; }
			if (hasLower) return s;   // MCC already wrote it in mixed case ("Infection", "One Flag")
			static const char* keep[] = { "CTF", "SWAT", "VIP", "KOTH", "FFA", "MLG", "BR", "DMR", "SMG", "HCS", "AR", "1V1", "2V2", "4V4" };
			std::string out; out.reserve(s.size());
			size_t i = 0;
			while (i < s.size())
			{
				if (s[i] == ' ') { out += ' '; ++i; continue; }
				size_t e = s.find(' ', i); if (e == std::string::npos) e = s.size();
				std::string w = s.substr(i, e - i);
				bool acronym = false;
				for (const char* k : keep) if (w == k) { acronym = true; break; }
				if (!acronym)
					for (size_t j = 1; j < w.size(); ++j) if (w[j] >= 'A' && w[j] <= 'Z') w[j] = (char)(w[j] + 32);
				out += w;
				i = e;
			}
			return out;
		}
	}

	std::string displayText(const std::string& s, Game game)
	{
		if (s.size() < 2 || s[0] != '$') return s;
		const std::string key = s.substr(1);
		const auto& t = locTable(game);
		const auto it = t.find(mccKeyHash(key));
		if (it != t.end() && !it->second.empty()) return titleCase(it->second);
		// not in the tables: the key made readable ("GAMETYPE_TEAM_SLAYER" -> "Team Slayer")
		std::string k = key;
		if (k.rfind("GAMETYPE_", 0) == 0) k = k.substr(9);
		for (auto& c : k) if (c == '_') c = ' ';
		std::string up = k;
		for (auto& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
		return titleCase(up);
	}

	std::vector<GameVariant> Launcher::gameVariants(Game g) const
	{
		std::vector<RawVariant> raw(1024);
		const int n = readVariants(mccGameIndex(g), raw.data(), (int)raw.size());
		std::vector<GameVariant> out;
		out.reserve((size_t)n);
		for (int i = 0; i < n; ++i)
		{
			const RawVariant& r = raw[(size_t)i];
			out.push_back(GameVariant{ r.category, r.cat, r.name, displayText(r.cat, g), displayText(r.name, g) });
		}
		return out;
	}

	void Launcher::setGametype(Game g, int32_t category, const std::string& name)
	{
		if ((int)g < 0 || g >= Game::Count) return;
		std::scoped_lock lk(mImpl->m);
		mImpl->gtCategory[(size_t)g] = category;
		mImpl->gtName[(size_t)g] = name;
	}

	bool Launcher::hideSupported() const { return mapFilterRecognised(); }
	int Launcher::hiddenCount() const { return mImpl->hidden.load(); }

	bool Launcher::refreshing() const { return mImpl->busy.load(); }
	std::vector<MapEntry> Launcher::maps() const
	{
		// every game's stock maps from MCC's catalogue (2026-11-19), merged into the scanned list
		const uint64_t now = GetTickCount64();
		bool reread = false;
		{ std::scoped_lock lk(mImpl->m); reread = now - mImpl->catStockMs > 3000; if (reread) mImpl->catStockMs = now; }
		if (reread)
		{
			std::vector<MapEntry> found;
			std::vector<RawStock> raw(512);
			for (int gi = 0; gi < (int)Game::Count; ++gi)
			{
				const Game g = (Game)gi;
				if (g == Game::Halo2 || !launchSupported(g)) continue;
				const int n = readCatalogueStock(mccGameIndex(g), raw.data(), (int)raw.size());
				for (int i = 0; i < n; ++i)
				{
					const RawStock& r = raw[(size_t)i];
					MapEntry e; e.game = g; e.stock = true; e.byId = true; e.stockIndex = r.index;
					memcpy(e.guid, r.id, 16);
					e.title = displayText(r.name, g);
					e.pack = std::string(gameName(g)) + " (stock)";
					found.push_back(std::move(e));
				}
			}
			std::sort(found.begin(), found.end(), [](const MapEntry& a, const MapEntry& b) {
				if (a.game != b.game) return (int)a.game < (int)b.game;
				return _stricmp(a.title.c_str(), b.title.c_str()) < 0; });
			std::scoped_lock lk(mImpl->m);
			mImpl->catStock = std::move(found);
		}
		std::scoped_lock lk(mImpl->m);
		std::vector<MapEntry> out = mImpl->maps;
		// each game's catalogue stock maps go before that game's mod maps (the scan puts stock first too)
		for (const auto& e : mImpl->catStock)
		{
			auto at = std::find_if(out.begin(), out.end(), [&e](const MapEntry& x) { return (int)x.game > (int)e.game || (x.game == e.game && !x.stock); });
			out.insert(at, e);
		}
		return out;
	}
	bool Launcher::canLaunch(Game g) const
	{
		if (!launchSupported(g)) return false;
		// other games need the lobby (Play selects the map there); Halo 2 also has the older launch-block swap
		return mLobbyTickHook != nullptr || (g == Game::Halo2 && mH2Hook != nullptr);
	}
	bool Launcher::hookReady() const { return mH2Hook != nullptr; }

	void Launcher::arm(const MapEntry& m)
	{
		std::scoped_lock lk(mImpl->m);
		mImpl->lobbySelected = false;
		mImpl->waitingPlayers = false;   // Play again while waiting for the players: start over with this map
		if (mLobbyTickHook)
		{
			mImpl->endingGame = false;
			mImpl->lobbyBackMs = 0;
			// Play from INSIDE a game (2026-11-21): end it as MCC's pause menu does; the lobby then selects + starts this map
			if (gameEngineExists() == 1)
			{
				const int r = endCurrentGame();
				PLOG_INFO << "[MapLauncher] Play in game: end the current game -> " << r;
				if (r != 1)
				{
					mImpl->isArmed = false;
					mImpl->msg = r == 0 ? "Close MCC's pause menu, then press Play again (HCM's window can stay open)"
						: r == -2 ? "Can't end the current game with this MCC build - end it yourself, then press Play in the lobby"
						: "Couldn't end the current game - end it yourself, then press Play in the lobby";
					return;
				}
				mImpl->isArmed = true; mImpl->armedMap = m;
				mImpl->startAskedMs = GetTickCount64();
				mImpl->endingGame = true;
				mImpl->startPending = true;
				mImpl->msg = "Ending this game - " + m.title + " starts as soon as MCC's lobby is back...";
				return;
			}
			// the lobby's tick (MCC's game thread) selects the map in the lobby and starts the match (onLobbyTick)
			mImpl->isArmed = true; mImpl->armedMap = m;
			mImpl->startAskedMs = GetTickCount64();
			mImpl->startPending = true;
			mImpl->msg = "Starting " + m.title + "...";
			return;
		}
		// ---- no lobby tick hook (another MCC build): the old local-only way - the user presses MCC's Start ----
		if (m.game != Game::Halo2)
		{
			mImpl->isArmed = false;
			mImpl->msg = std::string("Can't launch ") + gameName(m.game) + " maps with this MCC build (its custom game lobby wasn't recognised)";
			return;
		}
		// A stock map needs nothing prepared - but only from a lobby whose own map is stock. Whether it is, is known only at
		// the launch (the launch block's map field), so that case is refused there (onHalo2GameStart), not here.
		if (!m.stock)
		{
			// MCC prepares the map from its mod manager's map -> mod pair: point it at this one now (read when Start is pressed)
			int32_t state = -1; uintptr_t cb = 0;
			const int r = writeLobbyRecord(m.guid, m.modGuid, state, cb);
			if (r != 1)
			{
				mImpl->isArmed = false;
				mImpl->msg = r == 0 ? "MCC's mod manager is not ready - open a Halo 2 custom game lobby first, then press Play"
					: "MCC's mod manager could not be read";
				PLOG_ERROR << "[MapLauncher] mod manager " << (r == 0 ? "not found / not ready / unrecognised" : "unreadable")
					<< " (state " << state << ", callback exe+0x" << std::hex << cb << std::dec << "; expected 2, exe+0x32951b0)";
				return;
			}
			PLOG_INFO << "[MapLauncher] mod manager's map set to " << m.title << " (" << m.pack << ")";
		}
		mImpl->isArmed = true; mImpl->armedMap = m;
		mImpl->msg = "Ready: press Start in the Halo 2 custom game lobby (don't change its map) - " + m.title + " loads instead of the lobby's map (this machine only)";
	}

	void Launcher::onLobbyTick(uintptr_t screen)
	{
		if (!mImpl->startPending.load() && !mImpl->waitingPlayers.load())
		{
			// the panel's Gametype list follows the lobby's game (only a custom game lobby's, and only a valid index)
			if (customLobbyStart(screen))
			{
				const int32_t g = readI32(screen + 5832, -1);
				if (g >= 0 && g <= 6) mImpl->lobbyGame = g;
			}
			return;
		}
		const uintptr_t start = customLobbyStart(screen);
		if (!start) return;   // another screen class sharing the tick (Firefight's custom game lobby)
		// never select or start anything while a game is still up (or being torn down)
		if (gameEngineExists() != 0) { mImpl->lobbyBackMs = 0; return; }
		// Play from inside a game: give the lobby a moment to come back fully before acting in it
		if (mImpl->endingGame.load())
		{
			const uint64_t now = GetTickCount64();
			uint64_t first = mImpl->lobbyBackMs.load();
			if (!first) { mImpl->lobbyBackMs = now; return; }
			if (now - first < kLobbySettleMs) return;
			mImpl->endingGame = false;
			PLOG_INFO << "[MapLauncher] the lobby is back after ending the game - starting the armed map";
		}

		// the map is selected and sent: start once every player has reported it (a mod map's check in StartCountdown)
		if (mImpl->waitingPlayers.load())
		{
			const uint64_t waited = GetTickCount64() - mImpl->waitSinceMs.load();
			int players = 0;
			const int missing = playersMissingMap(players);
			// - everyone has it: start (with other players, not before their answers to THIS map can have arrived)
			// - cannot be told: a short fixed wait
			// - still missing after 6 s: start anyway - MCC's own dialog then names who does not have the map
			const bool go = missing == 0 ? (players <= 1 || waited >= 1000) : missing < 0 ? waited >= 1500 : waited >= 6000;
			if (!go)
			{
				std::scoped_lock lk(mImpl->m);
				if (mImpl->isArmed)
					mImpl->msg = missing > 0
						? "Waiting for the other players to confirm " + mImpl->armedMap.title + " (" + std::to_string(players - missing) + " of " + std::to_string(players) + ")..."
						: "Sending " + mImpl->armedMap.title + " to the other players...";
				return;
			}
			if (!mImpl->waitingPlayers.exchange(false)) return;
			std::string title, note; bool armedNow = false;
			{ std::scoped_lock lk(mImpl->m); armedNow = mImpl->isArmed; title = mImpl->armedMap.title; note = mImpl->waitNote; }
			if (!armedNow) return;
			PLOG_INFO << "[MapLauncher] starting the custom game lobby's match after " << waited << " ms (" << players << " players, "
				<< (missing < 0 ? std::string("their map status unknown") : std::to_string(missing) + " without the map") << ")";
			reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(start)(screen);
			std::scoped_lock lk(mImpl->m);
			if (mImpl->isArmed)
				mImpl->msg = missing > 0 ? "Not everyone confirmed " + title + " - see MCC's message" : "Started - " + title + " is loading" + note;
			if (mImpl->armedMap.game != Game::Halo2) { mImpl->isArmed = false; mImpl->lobbySelected = false; }   // only Halo 2's launch is watched
			return;
		}

		if (!mImpl->startPending.exchange(false)) return;
		MapEntry m; bool armedNow = false;
		{ std::scoped_lock lk(mImpl->m); armedNow = mImpl->isArmed; m = mImpl->armedMap; }
		if (!armedNow) return;

		// 0. (2026-11-19, every game) the lobby must be a custom game lobby of the map's game: MCC fixes a lobby's game when
		//    the lobby opens (screen +5832 from its open args, slot 5) - it has no in-place switch to call
		const int32_t lobbyGame = readI32(screen + 5832, -1);
		if (lobbyGame != mccGameIndex(m.game))
		{
			const Game lg = gameFromMccIndex(lobbyGame);
			PLOG_INFO << "[MapLauncher] not started: the lobby is game " << lobbyGame << ", " << m.title << " is " << gameName(m.game);
			std::scoped_lock lk(mImpl->m);
			mImpl->isArmed = false;
			mImpl->msg = std::string("MCC's lobby is a ") + (lg == Game::Count ? "different game's" : gameName(lg)) + " custom game - open a "
				+ gameName(m.game) + " custom game lobby, then press Play";
			return;
		}

		// 1. select the map in the lobby, as the UI does: MCC then prepares it, puts it in the network session (every
		//    player loads it) and launches it itself
		LobbySelect sel;
		selectInLobby(screen, m.stock, m.stockIndex, m.guid, m.byId, sel);
		bool selected = sel.result == 1;
		std::string note;
		if (selected)
		{
			PLOG_INFO << "[MapLauncher] selected in the lobby: " << m.title << " (" << m.pack << ") - game " << sel.game << ", catalogue id " << sel.id
				<< " kind " << sel.kind << " (" << sel.matches << " nodes), map " << sel.oldMap << " -> " << sel.id << ", variant " << sel.oldVariant
				<< " -> " << sel.newVariant << ", gametype " << sel.oldGametype << " -> " << sel.newGametype
				<< ", sent to players: " << (sel.broadcast == 1 ? "map tile refresh" : sel.broadcast == 2 ? "lobby state block" : "NO");
			if (sel.newGametype != sel.oldGametype) note = " (MCC changed the gametype: the lobby's one does not support this map)";
			// 1b. the gametype chosen in the panel (2026-11-19), as MCC's Choose Gametype picks one
			int32_t wantCat = -1; std::string wantName;
			{ std::scoped_lock lk(mImpl->m); wantCat = mImpl->gtCategory[(size_t)m.game]; wantName = mImpl->gtName[(size_t)m.game]; }
			if (!wantName.empty())
			{
				GametypeSelect gs;
				selectGametypeInLobby(screen, wantCat, wantName.c_str(), gs);
				PLOG_INFO << "[MapLauncher] gametype '" << wantName << "' (category " << wantCat << "): result " << gs.result
					<< ", variant " << gs.oldId << " -> " << gs.newId << " (category " << gs.category << "), lobby's was '" << gs.current << "'";
				const std::string shown = displayText(wantName, m.game);
				const std::string kept = gs.current[0] ? displayText(gs.current, m.game) : std::string("the lobby's gametype");
				switch (gs.result)
				{
				case 1: case 2: note = " with " + shown; break;
				case 3: note = " (" + shown + " doesn't support this map - kept " + kept + ")"; break;
				case 0: note = " (" + shown + " isn't in MCC's gametype list - kept " + kept + ")"; break;
				default: note = " (couldn't select " + shown + (gs.result == -2 ? ": another MCC build" : ": read fault") + " - kept the lobby's gametype)"; break;
				}
			}
			if (!m.stock)
			{
				// a mod map: every player's game must report it before StartCountdown's check passes - wait for that
				std::scoped_lock lk(mImpl->m);
				mImpl->lobbySelected = true;
				mImpl->waitNote = note;
				mImpl->waitSinceMs = GetTickCount64();
				mImpl->waitingPlayers = true;
				mImpl->msg = "Sending " + m.title + " to the other players...";
				return;
			}
		}
		else
		{
			// 2. fallback, this machine only: the mod manager's pair now, the launch block at the game's start (Halo 2 only:
			//    the launch-block swap is halo2.dll's)
			if (m.game != Game::Halo2)
			{
				PLOG_ERROR << "[MapLauncher] could not select " << m.title << " in the lobby (result " << sel.result << ", lobby game " << sel.game << ")";
				std::scoped_lock lk(mImpl->m);
				mImpl->isArmed = false;
				mImpl->msg = "Couldn't select " + m.title + " in MCC's lobby" + (sel.result == 0 ? std::string(" - it isn't in the lobby's map list") : sel.result == -2 ? std::string(" (another MCC build)") : std::string(""));
				return;
			}
			PLOG_ERROR << "[MapLauncher] could not select " << m.title << " in the lobby (result " << sel.result << ": "
				<< (sel.result == 0 ? "not in the lobby's map list" : sel.result == -2 ? "another MCC build" : "read fault")
				<< ", lobby game " << sel.game << ") - falling back to the local swap (NOT sent to other players)";
			note = " (local only - not sent to other players)";
			if (!m.stock)
			{
				int32_t state = -1; uintptr_t cb = 0;
				const int r = writeLobbyRecord(m.guid, m.modGuid, state, cb);
				if (r != 1)
				{
					PLOG_ERROR << "[MapLauncher] mod manager " << (r == 0 ? "not found / not ready / unrecognised" : "unreadable")
						<< " (state " << state << ", callback exe+0x" << std::hex << cb << std::dec << "; expected 2, exe+0x32951b0)";
					std::scoped_lock lk(mImpl->m);
					mImpl->isArmed = false;
					mImpl->msg = "Couldn't launch " + m.title + ": it is not in this lobby's map list and MCC's mod manager is not ready";
					return;
				}
			}
		}
		{ std::scoped_lock lk(mImpl->m); mImpl->lobbySelected = selected; }
		PLOG_INFO << "[MapLauncher] starting the custom game lobby's match";
		reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(start)(screen);
		std::scoped_lock lk(mImpl->m);
		if (mImpl->isArmed) mImpl->msg = "Started - " + m.title + " is loading" + note;
		if (m.game != Game::Halo2) { mImpl->isArmed = false; mImpl->lobbySelected = false; }   // only Halo 2's launch is watched
	}

	void Launcher::disarm()
	{
		mImpl->startPending = false;
		mImpl->endingGame = false;
		mImpl->waitingPlayers = false;
		std::scoped_lock lk(mImpl->m);
		mImpl->isArmed = false; mImpl->msg.clear();
	}

	bool Launcher::armed(MapEntry& out) const
	{
		std::scoped_lock lk(mImpl->m);
		if (mImpl->isArmed) out = mImpl->armedMap;
		return mImpl->isArmed;
	}

	std::string Launcher::message() const
	{
		// no custom game lobby ticked within the timeout: the start is dropped, and so is the map (nothing was prepared for it)
		if (mImpl->startPending.load() && GetTickCount64() - mImpl->startAskedMs.load() > (mImpl->endingGame.load() ? kEndGameTimeoutMs : kStartTimeoutMs)
			&& mImpl->startPending.exchange(false))
		{
			std::scoped_lock lk(mImpl->m);
			mImpl->isArmed = false;
			if (mImpl->endingGame.exchange(false))
			{
				mImpl->msg = "The game ended, but MCC's custom game lobby didn't come back within 2 minutes - press Play again from the lobby";
				return mImpl->msg;
			}
			mImpl->msg = std::string("Couldn't start: open MCC's ") + gameName(mImpl->armedMap.game) + " custom game lobby, then press Play";
		}
		// the lobby stopped ticking while Play waited for the players (the lobby was left): dropped
		if (mImpl->waitingPlayers.load() && GetTickCount64() - mImpl->waitSinceMs.load() > 10000 && mImpl->waitingPlayers.exchange(false))
		{
			std::scoped_lock lk(mImpl->m);
			mImpl->isArmed = false;
			mImpl->msg = "Not started: the custom game lobby was left";
		}
		std::scoped_lock lk(mImpl->m);
		return mImpl->msg;
	}

	void Launcher::onHalo2GameStart(uint8_t* gd)
	{
		std::scoped_lock lk(mImpl->m);
		if (!mImpl->isArmed || !gd) return;
		const MapEntry& m = mImpl->armedMap;
		if (m.game != Game::Halo2) return;
		int32_t lobbyMap = 0;
		if (mImpl->lobbySelected)
		{
			// the map was selected in the lobby: MCC built this launch itself. Nothing is swapped - a block naming another map
			// means the selection did not take, and swapping it here (with nothing prepared, and only on this machine) would
			// hang the load or split the players.
			const int same = blockHoldsMap(gd, m.stock, m.stockIndex, m.guid, lobbyMap);
			if (same == -2) return;   // not a multiplayer launch: stays armed
			mImpl->isArmed = false; mImpl->lobbySelected = false;
			if (same == 1)
			{
				PLOG_INFO << "[MapLauncher] Halo 2 launch: MCC launched " << m.title << " (" << m.pack << ") from the lobby selection";
				mImpl->msg = "Launched " + m.title + " (" + m.pack + ")";
			}
			else
			{
				PLOG_ERROR << "[MapLauncher] Halo 2 launch: the launch names map " << lobbyMap << ", not " << m.title << " - the lobby selection did not take (nothing swapped)";
				mImpl->msg = "MCC launched the lobby's own map - selecting " + m.title + " did not take";
			}
			return;
		}
		const int r = writeHalo2Map(gd, m.stock, m.stockIndex, m.guid, lobbyMap);
		if (r < 0) { PLOG_ERROR << "[MapLauncher] launch block unreadable"; return; }
		if (r == 0) return;   // not a multiplayer launch: stays armed
		if (r == 2)
		{
			PLOG_INFO << "[MapLauncher] Halo 2 launch: the lobby's map is a mod's - stock map " << m.title << " not swapped in";
			mImpl->msg = "Not launched: for a stock map, select any STOCK map in MCC's lobby first, then press Play again";
			mImpl->isArmed = false;
			return;
		}
		PLOG_INFO << "[MapLauncher] Halo 2 launch: lobby map " << lobbyMap << " -> " << m.title << " (" << m.pack << ")";
		mImpl->msg = "Launched " + m.title + " (" + m.pack + ")";
		mImpl->isArmed = false;   // one launch per Play
	}
}
