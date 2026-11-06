#include "pch.h"
#include "MapLauncher.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include <ShlObj.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

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

		// MCC's lobby selection record (what MCC prepares the map from; MCC Steam 1.3528, found by a pointer scan 2026-11-02):
		// [exe + 0x3F66910] + 0x218 = i32 stock map index or -1 (mod map), +0x220 map id, +0x230 the mod's GUID, and a signature
		// at +0x240 = exe + 0x3F9D600. Mod maps only. 1 written, 0 the record is not there / not recognised, -1 unreadable.
		// `wasMod` = the record held a mod map before (the lobby's map is a mod's).
		constexpr uintptr_t kLobbyRecordPtr = 0x3F66910, kLobbyRecordSig = 0x3F9D600;
		int writeLobbyRecord(const uint8_t* mapGuid, const uint8_t* modGuid, bool& wasMod)
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			__try
			{
				const uintptr_t obj = *(uintptr_t*)(exe + kLobbyRecordPtr);
				if (obj < 0x10000) return 0;
				if (*(uintptr_t*)(obj + 0x240) != exe + kLobbyRecordSig) return 0;
				wasMod = *(int32_t*)(obj + 0x218) == -1;
				*(int32_t*)(obj + 0x218) = -1;
				memcpy((void*)(obj + 0x220), mapGuid, 16);
				memcpy((void*)(obj + 0x230), modGuid, 16);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		// the record's current kind: 1 mod map, 0 stock map, -1 not found
		int lobbyRecordKind()
		{
			const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
			__try
			{
				const uintptr_t obj = *(uintptr_t*)(exe + kLobbyRecordPtr);
				if (obj < 0x10000 || *(uintptr_t*)(obj + 0x240) != exe + kLobbyRecordSig) return -1;
				return *(int32_t*)(obj + 0x218) == -1 ? 1 : 0;
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

		int64_t __fastcall h2GameStartDetour(int64_t engine, int64_t gameManager, int64_t gameData)
		{
			if (Launcher* self = sInstance.load()) self->onHalo2GameStart(reinterpret_cast<uint8_t*>(gameData));
			return sH2Hook ? sH2Hook->getInlineHook().call<int64_t>(engine, gameManager, gameData) : 0;
		}
	}

	const char* gameName(Game g)
	{
		static const char* n[] = { "Halo 1", "Halo 2", "Halo 3", "ODST", "Reach", "Halo 4", "H2A" };
		return (int)g >= 0 && (int)g < (int)Game::Count ? n[(int)g] : "?";
	}

	bool launchSupported(Game g) { return g == Game::Halo2; }

	struct Launcher::Impl
	{
		mutable std::mutex m;
		std::vector<MapEntry> maps;
		bool isArmed = false; MapEntry armedMap;
		std::string msg;
		std::atomic<bool> startPending{ false };
		std::atomic<uint64_t> startAskedMs{ 0 };
		std::atomic<bool> busy{ false };
		std::thread worker;
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
			impl->busy = false;
		});
	}

	bool Launcher::refreshing() const { return mImpl->busy.load(); }
	std::vector<MapEntry> Launcher::maps() const { std::scoped_lock lk(mImpl->m); return mImpl->maps; }
	bool Launcher::hookReady() const { return mH2Hook != nullptr; }

	void Launcher::arm(const MapEntry& m)
	{
		std::scoped_lock lk(mImpl->m);
		if (m.stock)
		{
			// a stock map needs nothing prepared - unless the lobby's map is a mod's (that record's stock form is not decoded)
			if (lobbyRecordKind() == 1)
			{
				mImpl->isArmed = false;
				mImpl->msg = "For a stock map, select any STOCK map in MCC's lobby first, then press Play again";
				return;
			}
		}
		else
		{
			// MCC prepares the map from its lobby record: point it at this one now (it is read when Start is pressed)
			bool wasMod = false;
			const int r = writeLobbyRecord(m.guid, m.modGuid, wasMod);
			if (r != 1)
			{
				mImpl->isArmed = false;
				mImpl->msg = r == 0 ? "MCC's lobby selection was not found - open a Halo 2 custom game lobby first, then press Play"
					: "MCC's lobby selection could not be read";
				PLOG_ERROR << "[MapLauncher] lobby record " << (r == 0 ? "not found / unrecognised" : "unreadable");
				return;
			}
			PLOG_INFO << "[MapLauncher] lobby record set to " << m.title << " (" << m.pack << "), lobby had a " << (wasMod ? "mod" : "stock") << " map";
		}
		mImpl->isArmed = true; mImpl->armedMap = m;
		if (mLobbyTickHook)
		{
			// the lobby's tick starts the match on MCC's game thread (onLobbyTick)
			mImpl->startAskedMs = GetTickCount64();
			mImpl->startPending = true;
			mImpl->msg = "Starting " + m.title + "...";
		}
		else mImpl->msg = "Ready: press Start in the Halo 2 custom game lobby (don't change its map) - " + m.title + " loads instead of the lobby's map";
	}

	void Launcher::onLobbyTick(uintptr_t screen)
	{
		if (!mImpl->startPending.load()) return;
		const uintptr_t start = customLobbyStart(screen);
		if (!start) return;   // another screen class sharing the tick (Firefight's custom game lobby)
		if (!mImpl->startPending.exchange(false)) return;
		const int32_t lobbyGame = readI32(screen + 5832, -2);
		PLOG_INFO << "[MapLauncher] starting the custom game lobby's match (lobby game field " << lobbyGame << ")";
		reinterpret_cast<int64_t(__fastcall*)(uintptr_t)>(start)(screen);
		std::scoped_lock lk(mImpl->m);
		if (mImpl->isArmed) mImpl->msg = "Started - " + mImpl->armedMap.title + " is loading";
	}

	void Launcher::disarm()
	{
		mImpl->startPending = false;
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
		// no custom game lobby ticked within the timeout: the start is dropped (the map stays armed for MCC's own Start)
		if (mImpl->startPending.load() && GetTickCount64() - mImpl->startAskedMs.load() > kStartTimeoutMs && mImpl->startPending.exchange(false))
		{
			std::scoped_lock lk(mImpl->m);
			mImpl->msg = "Couldn't start: open MCC's Halo 2 custom game lobby, then press Play (or press MCC's Start - the map is still armed)";
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
		const int r = writeHalo2Map(gd, m.stock, m.stockIndex, m.guid, lobbyMap);
		if (r < 0) { PLOG_ERROR << "[MapLauncher] launch block unreadable"; return; }
		if (r == 0) return;   // not a multiplayer launch: stays armed
		PLOG_INFO << "[MapLauncher] Halo 2 launch: lobby map " << lobbyMap << " -> " << m.title << " (" << m.pack << ")";
		mImpl->msg = "Launched " + m.title + " (" + m.pack + ")";
		mImpl->isArmed = false;   // one launch per Play
	}
}
