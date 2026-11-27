#include "pch.h"
#include "H2EraseEntities.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "IMakeOrGetCheat.h"
#include "GameTickEventHook.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include <Windows.h>
#include <unordered_set>

// ---- What the game's own (ai_erase_all) does (hs function 300, halo2.dll sub_180618F50, read 2026-11-24) ---------------
//   if AI is active (ai globals byte +1):  for every actor in the actor array (2208-byte elements):
//       unit = actor+24, swarm = actor+28;  actor_delete(actor);   [sub_180624B20]
//       unit != -1 ? object_delete(unit) : (swarm != -1 ? swarm_delete(swarm) : nothing)   [sub_1808D4D10 / sub_180662580]
// So it only reaches things that still have an actor: a dead AI has none (its body is just a biped), and a vehicle an AI
// drives is not the actor's unit. It deletes the unit without the "is this, or does this carry, a player" test
// (sub_1808D71C0) that (object_destroy) and (ai_erase <squad>) both make first. And typed into a console it runs whenever
// the console happens to execute it, not at a fixed point of the simulation.
//
// ---- What Erase All Entities does instead --------------------------------------------------------------------------
// The press is only latched; the work runs on the GAME thread at the start of the next simulation tick (the game-tick
// event), where nothing else is half way through the object or actor arrays. Then, with the engine's own functions:
//   1. every actor, exactly as above - but the unit is deleted only if it still exists, is not already being deleted, and
//      the engine's player test says no;
//   2. every vehicle whose DRIVER (vehicle+604, what (vehicle_driver) returns) was one of those actors' units - unless a
//      player is in or on it;
//   3. every biped and creature left that is not a player's: the dead AI (bodies), and anything else standing around.
// Each engine call is fault-guarded: if one faults, the erase stops there and says so instead of taking the game down.
// The players are protected three ways: the player array's unit datums are never touched, the engine's own player test
// is asked for every object, and a vehicle carrying a player is left alone.
//
// "Also Stop New AI From Spawning?": the squad placement core every (ai_place ...) and scripted spawn goes through
// (sub_180637000) returns at once while the box is ticked - the same way it returns by itself when AI is not active, so
// every caller already copes with "nothing was placed". (Things that are not placed as a squad - e.g. infection forms
// bursting out of a carrier - still appear.)
namespace
{
	constexpr uintptr_t kAiGlobalsRVA = 0x1A79258;      // qword ptr; byte +1 = AI active
	constexpr uintptr_t kActorsRVA = 0x1A79298;         // qword ptr to the actor data array (2208-byte elements)
	constexpr int32_t   kActorSize = 2208;
	constexpr uint32_t  kActorUnitOff = 24, kActorSwarmOff = 28;
	constexpr uint32_t  kVehicleDriverOff = 604;        // vehicle_get_driver (sub_180940280)
	constexpr uint32_t  kPlayerSize = 548, kPlayerUnitOff = 44;
	constexpr uint32_t  kUnitPlayerIndexOff = 320;      // unit: the player controlling it, -1 = none (sub_18069D010)
	constexpr uintptr_t kSwarmsRVA = 0x1A792E8;         // qword ptr to the swarm data array (52-byte elements)
	constexpr int32_t   kSwarmSize = 52;
	constexpr uint8_t   kObjectTypeBiped = 0, kObjectTypeVehicle = 1, kObjectTypeCreature = 12;
	constexpr uint8_t   kHeaderBeingDeleted = 0x10;

	struct EngineFn { uintptr_t rva; uint8_t bytes[14]; const char* name; };
	// the first 14 bytes of each function in MCC 1.3528 (none of them holds a rip-relative operand before byte 14 except
	// where noted, and those are part of the build identity anyway)
	constexpr EngineFn kActorDelete{ 0x624B20, { 0x40, 0x56, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x48 }, "actor delete" };
	constexpr EngineFn kObjectDelete{ 0x8D4D10, { 0x40, 0x53, 0x56, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x05, 0x7A, 0x26, 0xFE, 0x00 }, "object delete" };
	constexpr EngineFn kObjectHasPlayer{ 0x8D71C0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC }, "player test" };
	constexpr EngineFn kSwarmDelete{ 0x662580, { 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x05, 0x5B, 0x6D, 0x41, 0x01, 0x48 }, "swarm delete" };
	constexpr EngineFn kSquadPlace{ 0x637000, { 0x44, 0x88, 0x4C, 0x24, 0x20, 0x66, 0x89, 0x54, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08 }, "squad placement" };

	bool matches(uintptr_t base, const EngineFn& f)
	{
		uint8_t now[sizeof(f.bytes)];
		return mem::copy(now, base + f.rva, sizeof(now)) && memcmp(now, f.bytes, sizeof(now)) == 0;
	}

	using fn_u32_t = int64_t(__fastcall*)(uint32_t);
	using fn_bool_u32_t = bool(__fastcall*)(uint32_t);

	// One engine call, fault-guarded. (No C++ objects in here: __try.)
	bool guardedCall(fn_u32_t fn, uint32_t arg)
	{
		__try { fn(arg); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	// 0 = no, 1 = yes, -1 = the test itself faulted (treated as "yes": leave the object alone)
	int guardedTest(fn_bool_u32_t fn, uint32_t arg)
	{
		__try { return fn(arg) ? 1 : 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}

	// a Halo data array (see H2CV_Halo2.cpp datumGet): element size i32 +36, count bound i32 +60, data offset i64 +72,
	// salt u16 at element +0 (0 = free)
	struct DataArray
	{
		uintptr_t first = 0; int32_t elementSize = 0, count = 0;
		bool read(uintptr_t hdr, int32_t wantSize, int32_t maxCount)
		{
			if (!mem::okp(hdr)) return false;
			elementSize = mem::i32(hdr + 36, 0); count = mem::i32(hdr + 60, 0);
			const int64_t off = mem::get<int64_t>(hdr + 72, 0);
			if (elementSize != wantSize || count <= 0 || count > maxCount || off == 0) return false;
			first = hdr + (uintptr_t)off;
			return true;
		}
		uintptr_t element(int32_t i) const { return first + (uintptr_t)i * (uintptr_t)elementSize; }
		// the element a datum names, or 0 if the slot is free / reused since
		uintptr_t fromDatum(uint32_t datum) const
		{
			const int32_t i = (int32_t)(datum & 0xFFFF);
			if (datum == 0xFFFFFFFFu || i >= count) return 0;
			const uintptr_t e = element(i);
			return mem::u16(e, 0) == (uint16_t)(datum >> 16) && (datum >> 16) != 0 ? e : 0;
		}
	};
}

class H2EraseEntities::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMCCStateHook> mccWeak;

	ScopedCallback<ActionEvent> mEraseCallback;
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mBlockSpawnsCallback;
	std::unique_ptr<ScopedCallback<GameTickEvent>> mGameTickCallback;
	std::unique_ptr<ModuleInlineHook> mSquadPlaceHook;

	std::atomic<bool> mErasePending{ false };
	std::mutex mToggleMutex;

	static inline std::atomic<Impl*> sSelf{ nullptr };
	static inline std::atomic<bool> sBlockSpawns{ false };
	static inline std::atomic<uint32_t> sBlockedPlacements{ 0 };

	void say(const std::string& s) { if (auto m = messagesGUIWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	// object header entry (12 bytes: salt u16, flags u8 +2, type u8 +3, data offset u32 +8) of a live object, or 0
	static uintptr_t liveHeader(const DataArray& headers, uint32_t datum)
	{
		const uintptr_t h = headers.fromDatum(datum);
		if (!h || mem::u32(h + 8) == 0xFFFFFFFFu) return 0;
		return (mem::u8(h + 2) & kHeaderBeingDeleted) ? 0 : h;
	}

	// Game thread, start of a tick.
	void erase()
	{
		const uintptr_t base = h2::base();
		if (!base) { say("Erase All Entities: Halo 2 is not loaded."); return; }
		for (const EngineFn* f : { &kActorDelete, &kObjectDelete, &kObjectHasPlayer, &kSwarmDelete })
			if (!matches(base, *f)) { say(std::format("Erase All Entities: this Halo 2 build is not supported (made for MCC 1.3528; the {} function differs). Nothing was erased.", f->name)); return; }
		const auto actorDelete = (fn_u32_t)(base + kActorDelete.rva);
		const auto objectDelete = (fn_u32_t)(base + kObjectDelete.rva);
		const auto swarmDelete = (fn_u32_t)(base + kSwarmDelete.rva);
		const auto objectHasPlayer = (fn_bool_u32_t)(base + kObjectHasPlayer.rva);

		DataArray headers;
		if (!headers.read(mem::q(base + h2::rva::OBJ_HEADER), 12, 4096)) { say("Erase All Entities: no level is running."); return; }

		// the players' own units: never touched, whatever else says
		std::unordered_set<uint32_t> playerUnits;
		{
			DataArray players;
			if (players.read(mem::q(base + h2::rva::PLAYERS_ARRAY), (int32_t)kPlayerSize, 64))
				for (int32_t i = 0; i < players.count; ++i)
				{
					const uintptr_t p = players.element(i);
					if (mem::u16(p, 0) == 0) continue;
					const uint32_t unit = mem::u32(p + kPlayerUnitOff);
					if (unit != 0xFFFFFFFFu) playerUnits.insert(unit);
				}
		}

		int erasedAI = 0, erasedVehicles = 0, erasedBodies = 0, kept = 0;
		bool faulted = false;

		// Delete one object if it is live and not a player's. Returns false only on a fault.
		//   a vehicle: the engine's own test - is it, or does anything on it belong to, a player (what (object_destroy) and
		//     (ai_erase <squad>) ask before deleting);
		//   a biped: its own player index (unit+320, -1 = nobody's) - the engine's test would also keep a marine who merely
		//     rides in the player's vehicle, and that marine is exactly what this button is for.
		auto deleteObject = [&](uint32_t datum, int& counter) -> bool
		{
			if (playerUnits.contains(datum)) return true;
			const uintptr_t header = liveHeader(headers, datum);
			if (!header) return true;
			const uint8_t type = mem::u8(header + 3, 0xFF);
			if (type == kObjectTypeBiped)
			{
				const uintptr_t obj = h2::objectFromDatum(datum);
				if (!obj) return true;
				if (mem::u32(obj + kUnitPlayerIndexOff, 0) != 0xFFFFFFFFu) { ++kept; return true; }
			}
			else if (type != kObjectTypeCreature)
			{
				const int hasPlayer = guardedTest(objectHasPlayer, datum);
				if (hasPlayer != 0) { ++kept; return hasPlayer > 0; }
			}
			if (!guardedCall(objectDelete, datum)) return false;
			++counter;
			return true;
		};

		// ---- 1. the actors (what the game's ai_erase_all does), remembering the vehicles they drive ----
		struct Actor { uint32_t datum, unit, swarm; };
		std::vector<Actor> actors;
		std::vector<uint32_t> vehicles;
		std::unordered_set<uint32_t> swarmsDone;
		DataArray actorArray;
		const uintptr_t aiGlobals = mem::q(base + kAiGlobalsRVA);
		const bool aiActive = mem::okp(aiGlobals) && mem::u8(aiGlobals + 1, 0) != 0;
		if (aiActive && actorArray.read(mem::q(base + kActorsRVA), kActorSize, 4096))
		{
			for (int32_t i = 0; i < actorArray.count; ++i)
			{
				const uintptr_t a = actorArray.element(i);
				const uint16_t salt = mem::u16(a, 0);
				if (salt == 0) continue;
				actors.push_back({ ((uint32_t)salt << 16) | (uint32_t)i, mem::u32(a + kActorUnitOff), mem::u32(a + kActorSwarmOff) });
			}
			for (const Actor& a : actors)
			{
				if (a.unit == 0xFFFFFFFFu || !liveHeader(headers, a.unit)) continue;
				const uintptr_t unitObj = h2::objectFromDatum(a.unit);
				uint32_t vehicleDatum = 0xFFFFFFFFu;
				if (!unitObj || !h2::vehicleOfUnit(unitObj, &vehicleDatum) || vehicleDatum == 0xFFFFFFFFu) continue;
				const uintptr_t vh = liveHeader(headers, vehicleDatum);
				const uintptr_t vehicleObj = vh ? h2::objectFromDatum(vehicleDatum) : 0;
				if (vehicleObj && mem::u8(vh + 3) == kObjectTypeVehicle && mem::u32(vehicleObj + kVehicleDriverOff) == a.unit
					&& std::find(vehicles.begin(), vehicles.end(), vehicleDatum) == vehicles.end())
					vehicles.push_back(vehicleDatum);
			}
			for (const Actor& a : actors)
			{
				if (!actorArray.fromDatum(a.datum)) continue;   // already gone (deleted along with another one)
				if (!guardedCall(actorDelete, a.datum)) { faulted = true; break; }
				++erasedAI;
				if (a.unit != 0xFFFFFFFFu)
				{
					int ignored = 0;
					if (!deleteObject(a.unit, ignored)) { faulted = true; break; }
				}
				else if (a.swarm != 0xFFFFFFFFu)
				{
					// Once per swarm, and only while that swarm still exists. The game's own loop calls this for EVERY actor
					// that names the swarm, i.e. again on a swarm it has just deleted.
					DataArray swarms;
					if (!swarmsDone.insert(a.swarm).second) continue;
					if (!swarms.read(mem::q(base + kSwarmsRVA), kSwarmSize, 4096) || !swarms.fromDatum(a.swarm)) continue;
					if (!guardedCall(swarmDelete, a.swarm)) { faulted = true; break; }
				}
			}
		}

		// ---- 2. the vehicles those actors were driving ----
		if (!faulted)
			for (uint32_t v : vehicles)
				if (!deleteObject(v, erasedVehicles)) { faulted = true; break; }

		// ---- 3. every biped and creature that is left and is not a player's: the dead ones included ----
		if (!faulted)
		{
			std::vector<h2::ObjectRef> objects;
			h2::enumerateObjects(objects);
			for (const auto& o : objects)
			{
				if (o.type != kObjectTypeBiped && o.type != kObjectTypeCreature) continue;
				if (!deleteObject(o.datum, erasedBodies)) { faulted = true; break; }
			}
		}

		std::string report = std::format("Erase All Entities: {} AI, {} other bodies, {} vehicle{} erased", erasedAI, erasedBodies, erasedVehicles, erasedVehicles == 1 ? "" : "s");
		if (kept > 0) report += std::format(" ({} left alone: a player is on or in {})", kept, kept == 1 ? "it" : "them");
		report += faulted ? " - STOPPED EARLY: an engine call faulted (the game was kept running; please report this)." : ".";
		if (faulted) PLOG_ERROR << report;
		say(report);
	}

	void onGameTick(uint32_t)
	{
		if (!mErasePending.exchange(false)) return;
		erase();
	}

	void onErasePressed()
	{
		auto mcc = mccWeak.lock();
		if (!mcc || mcc->getCurrentMCCState().currentGameState != mGame) return;
		if (!mcc->isGameCurrentlyPlaying(mGame)) { say("Erase All Entities: load a level first."); return; }
		mErasePending = true;   // done on the game thread at the start of the next tick
	}

	// the squad placement core: (squad, starting location, count, ?, ?) - returns unused. Blocked = its own "AI is not
	// active" early return.
	static int64_t __fastcall squadPlaceDetour(uint32_t squad, int16_t location, int16_t count, char a4, uint8_t a5)
	{
		if (sBlockSpawns.load(std::memory_order_relaxed))
		{
			sBlockedPlacements.fetch_add(1, std::memory_order_relaxed);
			return 0;
		}
		Impl* self = sSelf.load();
		if (!self || !self->mSquadPlaceHook) return 0;   // HCM is shutting down
		return self->mSquadPlaceHook->getInlineHook().call<int64_t, uint32_t, int16_t, int16_t, char, uint8_t>(squad, location, count, a4, a5);
	}

	// the box could not be honoured: untick it (directly - no event, this is already the event's handler)
	void untick()
	{
		if (auto settings = settingsWeak.lock())
		{
			settings->h2BlockAISpawningToggle->GetValue() = false;
			settings->h2BlockAISpawningToggle->GetValueDisplay() = false;
		}
	}

	void applyBlock(bool on, bool announce)
	{
		std::scoped_lock lk(mToggleMutex);
		if (!on)
		{
			sBlockSpawns = false;
			if (mSquadPlaceHook) mSquadPlaceHook->setWantsToBeAttached(false);
			const uint32_t stopped = sBlockedPlacements.exchange(0);
			if (announce) say(std::format("New AI can spawn again ({} squad placement{} stopped while it was on).", stopped, stopped == 1 ? " was" : "s were"));
			return;
		}
		const uintptr_t base = h2::base();
		if (!base)
		{
			if (announce) say("Stop New AI From Spawning: load Halo 2 first.");
			untick();
			return;
		}
		// (an installed hook has replaced the function's first bytes with its jump - then they were checked when it went in)
		const bool installed = mSquadPlaceHook && mSquadPlaceHook->isHookInstalled();
		if (!mSquadPlaceHook || (!installed && !matches(base, kSquadPlace)))
		{
			if (announce) say("Stop New AI From Spawning: this Halo 2 build is not supported (made for MCC 1.3528).");
			untick();
			return;
		}
		sBlockedPlacements = 0;
		sBlockSpawns = true;
		mSquadPlaceHook->setWantsToBeAttached(true);
		if (announce) say("New AI is stopped from spawning.");
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		mEraseCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2EraseAllEntitiesEvent, [this]() { onErasePressed(); }),
		mBlockSpawnsCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2BlockAISpawningToggle->valueChangedEvent, [this](bool& on) { applyBlock(on, true); })
	{
		if (game.operator GameState::Value() != GameState::Value::Halo2) throw HCMInitException("Erase All Entities is Halo 2 only");
		if (sSelf.load()) throw HCMInitException("Erase All Entities already exists");

		auto gameTickEventHook = resolveDependentCheat(GameTickEventHook);
		if (!gameTickEventHook) throw HCMInitException("Erase All Entities needs the game tick event");
		mGameTickCallback = gameTickEventHook->getGameTickEvent()->subscribe([this](uint32_t t) { onGameTick(t); });

		auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)kSquadPlace.rva });
		mSquadPlaceHook = ModuleInlineHook::make(L"halo2.dll", ptr, (void*)&squadPlaceDetour, false);
		sSelf = this;
		if (dicon.Resolve<SettingsStateAndEvents>().lock()->h2BlockAISpawningToggle->GetValue()) applyBlock(true, false);
	}

	~Impl()
	{
		mGameTickCallback.reset();
		sBlockSpawns = false;
		if (mSquadPlaceHook) mSquadPlaceHook->setWantsToBeAttached(false);
		sSelf = nullptr;
	}
};

H2EraseEntities::H2EraseEntities(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(gameImpl, dicon))
{
}

H2EraseEntities::~H2EraseEntities() = default;
