#include "pch.h"
#include "HCEAISquadOverlay.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"
#include "HCEGameThreadPump.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "RenderTextHelper.h"
#include "HCEAnchors.h"
#include "HCEProgressionGates.h"   // ScenarioDataPointer / TagAddressTable, resolved by signature not hard-coded
#include "GlobalKill.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

// See HCEAISquadOverlay.h for the whole runtime chain and why this cannot be a static name table.

namespace
{
	// ---- sim TLS slots -------------------------------------------------------------------------------
	constexpr int64_t kSimInitialisedByte = 0x14;   // every engine fn checks this before touching sim globals
	constexpr int64_t kObjectHeaderTls = 0x20;
	constexpr int64_t kAiHeaderTls = 0x28;
	constexpr int64_t kGameTimeGlobalsTls = 0x98;   // game_time_globals; the tick rate is a uint16 at +0x06

	// ---- Halo data-array header ----------------------------------------------------------------------
	constexpr int64_t kDataArrayHighWater = 0x44;   // one past the last slot ever used
	constexpr int64_t kDataArrayBase = 0x50;
	constexpr int64_t kDataArrayStride = 0x20;      // where the element stride is stored

	// ---- object table entry --------------------------------------------------------------------------
	constexpr int64_t kEntrySalt = 0x00;   // u16, 0 = free slot
	constexpr int64_t kEntryType = 0x04;   // u8, object_type_enum
	constexpr int64_t kEntryObject = 0x10; // object*
	constexpr size_t  kObjectEntryStride = 0x18;

	// Object type indices, read out of the sim DLL's own object_type_enum_definition (14 entries, the name
	// pointer table at VA 0x1809C5090 on the 2026-08-17 build - biped is [0], creature is [11]):
	//   0 biped   1 vehicle   2 weapon    3 equipment      4 terminal   5 projectile  6 scenery
	//   7 machine 8 control   9 sound_scenery  10 crate   11 creature  12 giant      13 effect_scenery
	constexpr uint8_t kTypeBiped = 0, kTypeVehicle = 1, kTypeCreature = 11, kTypeGiant = 12;

	// The things worth labelling: biped(0) | vehicle(1) | creature(11) | giant(12).
	// ⚠ creature(11) is IN because that is what the Flood infection forms are - they are .creature tags, not
	// .biped, so a biped-only mask silently drops every popcorn in the level. Everything else - weapons,
	// equipment, projectiles, scenery, crates, effect scenery - stays out: that is dropped-gun and bullet noise.
	constexpr uint32_t kLabelledTypeMask =
		(1u << kTypeBiped) | (1u << kTypeVehicle) | (1u << kTypeCreature) | (1u << kTypeGiant);   // 0x1803

	// ---- live object ---------------------------------------------------------------------------------
	constexpr int64_t kObjPosition = 0x20;   // float3, the ORIGIN - at the FEET
	constexpr int64_t kObjFlags = 0x128;     // bit 2 set = DEAD (object_get_health short-circuits on it)
	constexpr uint32_t kObjDeadBit = 4u;
	constexpr int64_t kObjTeam = 0x1BA;      // i8, global_campaign_team_enum
	constexpr int64_t kObjActorHandle = 0x1AC; // i32, -1 = not an AI actor

	// ---- live actor ----------------------------------------------------------------------------------
	constexpr size_t kActorStride = 0xD10;   // records stored INLINE, unlike the object table's pointers
	constexpr int64_t kActorCurrentSquad = 0x3C;
	constexpr int64_t kActorOriginSquad = 0x40;

	// ---- scenario tag blocks (offsets from walking the binary's own struct definition; the walk CLOSES
	// EXACTLY at the declared 1780 bytes and is pinned by five code-verified offsets) -------------------
	constexpr int64_t kScenarioSquadGroups = 0x33C;  // stride 40
	constexpr int64_t kScenarioSquads = 0x348;       // stride 108
	constexpr size_t kSquadStride = 108;
	constexpr size_t kSquadGroupStride = 40;
	constexpr int64_t kSquadName = 0x00;       // ⚠ inline char[32], NOT a string_id. TRUNCATED at 31 chars.
	constexpr int64_t kSquadTeam = 0x24;       // i16 - authored, and 'default' for 62.6% of squads. Display only.
	constexpr int64_t kSquadParentGroup = 0x26;// i16, -1 = none
	constexpr int64_t kGroupName = 0x00;       // inline char[32]
	constexpr int64_t kGroupParent = 0x20;     // i16 parent GROUP - groups NEST

	constexpr int32_t kMaxSquads = 4096;
	constexpr int32_t kMaxGroups = 4096;
	// ⚠ THIS CAP IS NOT JUST A RENDER BUDGET - classifyGates counts gate membership over the same vector, so
	// truncating it UNDERCOUNTS a squad, which makes its gate falsely satisfy and advances the chain early.
	// 343's tower gauntlet fields a wave plus dozens of infection forms at once, so 256 was uncomfortably
	// close. Raised, and truncation is now logged rather than silently changing the answer.
	constexpr size_t kMaxLabels = 1024;

	// global_campaign_team_enum, 14 named values.
	const char* teamName(int t)
	{
		switch (t)
		{
		case 0:  return "default";
		case 1:  return "player";
		case 2:  return "human";
		case 3:  return "covenant";
		case 4:  return "brute";
		case 5:  return "mule";
		case 6:  return "spare";
		case 7:  return "covenant_player";
		case 8:  return "flood";
		case 9:  return "sentinel";
		case 10: return "heretic";
		case 11: return "prophet";
		case 12: return "guilty";
		case 13: return "berserk";
		default: return "?";
		}
	}

	// The user's requested buckets, mapped onto the engine enum. Anything not named falls to Other, which is
	// where `default` also lands - a unit still reading 0 at runtime has not resolved a faction.
	enum class TeamBucket { Player, Human, Covenant, Flood, Sentinel, Other };

	TeamBucket bucketOf(int team)
	{
		switch (team)
		{
		case 1:  return TeamBucket::Player;
		case 2:  return TeamBucket::Human;
		case 3:
		case 7:  return TeamBucket::Covenant;
		case 8:  return TeamBucket::Flood;
		case 9:  return TeamBucket::Sentinel;
		default: return TeamBucket::Other;
		}
	}

	// ---- the snapshot the render path consumes -------------------------------------------------------
	struct AiLabel
	{
		SimpleMath::Vector3 position{};
		int32_t currentSquad = -1;
		int32_t originSquad = -1;
		int8_t team = 0;
		bool migrated = false;   // currentSquad != originSquad - the ai_migrate case, e.g. c10's bloodgate
		bool isCreature = false; // .creature, i.e. the Flood infection forms - tiny, and they come in clumps

		// 0 = neutral, 1 = REQUIRED (a hard gate, no timeout), 2 = TIMED (a soft gate that gives up)
		uint8_t gateClass = 0;
		float   secondsRemaining = -1.f;   // < 0 = unknown / not applicable; only set for gateClass 2
	};

	std::atomic<bool> gActive{ false };
	std::mutex gSnapshotMutex;
	std::vector<AiLabel> gLabels;
	std::vector<std::string> gSquadNames;   // indexed by squad index
	std::vector<std::string> gGroupNames;   // indexed by group index
	std::vector<int16_t> gSquadParent;      // squad index -> group index, -1 = none
	// ⚠⚠⚠ GROUPS NEST, AND THE SCRIPTS NAME THE OUTER ONE. The scenario tag's group for a c10 gauntlet
	// biped is `sq_gr_gauntlet3_combat`, but the script says `(f_run_gauntlet_wave sq_gr_gauntlet3 ...)` -
	// and `sq_gr_gauntlet3_combat` appears ZERO times in c10.hsc. Matching only the IMMEDIATE parent
	// therefore matches NOTHING and no gate ever colours anything. This is the chain that fixes it.
	std::vector<int16_t> gGroupParent;      // group index -> parent group index, -1 = none
	std::atomic<uint32_t> gSnapshotSerial{ 0 };

	inline bool rdPtr(uintptr_t a, uintptr_t& v)
	{
		return HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)) && v != 0;
	}

	// A 32-byte inline tag string. Stops at the first NUL or non-printable rather than showing heap garbage.
	std::string readInlineName(uintptr_t address)
	{
		char raw[33]{};
		if (!HCEGetPlayerState::tryReadRaw(address, raw, 32)) return {};
		raw[32] = '\0';
		std::string out;
		for (int i = 0; i < 32; ++i)
		{
			const char c = raw[i];
			if (c == '\0') break;
			if (c < 0x20 || c > 0x7E) break;
			out.push_back(c);
		}
		return out;
	}
}

class HCEAISquadOverlay::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::shared_ptr<HCEGetPlayerState> mPlayerState;
	std::shared_ptr<HCEGameThreadPumpHost> mPump;
	std::optional<std::weak_ptr<HCEGetCameraData>> mCameraDataOptionalWeak;
	bool mCameraHookRequested = false;

	bool mRegistered = false;
	std::atomic<bool> mReady{ false };

	// ---- scenario-side name resolution, refreshed when the scenario changes ------------------------
	static inline uintptr_t mScenarioSlot = 0;
	static inline uintptr_t mTagAddressTable = 0;
	static inline uintptr_t mCachedScenario = 0;

	static uintptr_t resolveTagBlock(uint32_t encoded)
	{
		// Identical arithmetic to every other HCE tag block read: the high nibble picks one of 16 region
		// bases AND stays part of the offset, so it is not expressible as a flat offset.
		if (!mTagAddressTable || encoded == 0 || encoded == 0xFFFFFFFFu) return 0;
		uintptr_t regionBase = 0;
		if (!rdPtr(mTagAddressTable + 8ull * (encoded >> 28), regionBase)) return 0;
		return regionBase + 4ull * encoded;
	}

	// Reads the squad and squad-group name tables out of the LIVE scenario tag. Doing it this way rather than
	// shipping a generated per-level header is what makes the overlay work on all 13 levels with no data at
	// all - and it is also the only thing that can name a squad the scripts never mention.
	// Returns false if it resolved nothing. ⚠ The CALLER MUST NOT commit its cache key unless this returned
	// true: the scenario tag pointer is published before the tag blocks behind it are all mapped, so the first
	// attempt after a level load can legitimately read nothing, and a latched key would leave every label
	// reading "(no squad)" for the whole level with only a toggle off/on to clear it.
	static bool refreshScenarioNames(uintptr_t scenario)
	{
		gSquadNames.clear();
		gGroupNames.clear();
		gSquadParent.clear();
		gGroupParent.clear();

		auto readBlock = [&](int64_t blockOffset, size_t stride, int32_t maxCount,
			std::vector<std::string>& names, std::vector<int16_t>* parents, int64_t parentOffset)
			{
				int32_t count = 0;
				uint32_t encoded = 0;
				if (!HCEGetPlayerState::tryReadRaw(scenario + blockOffset, &count, sizeof(count))) return;
				if (!HCEGetPlayerState::tryReadRaw(scenario + blockOffset + 4, &encoded, sizeof(encoded))) return;
				if (count <= 0 || count > maxCount) return;
				const uintptr_t base = resolveTagBlock(encoded);
				if (!base) return;

				names.resize((size_t)count);
				if (parents) parents->assign((size_t)count, (int16_t)-1);
				for (int32_t i = 0; i < count; ++i)
				{
					const uintptr_t element = base + (uintptr_t)stride * i;
					names[(size_t)i] = readInlineName(element + kSquadName);
					if (parents)
					{
						int16_t p = -1;
						HCEGetPlayerState::tryReadRaw(element + parentOffset, &p, sizeof(p));
						(*parents)[(size_t)i] = p;
					}
				}
			};

		readBlock(kScenarioSquadGroups, kSquadGroupStride, kMaxGroups, gGroupNames, &gGroupParent, kGroupParent);
		readBlock(kScenarioSquads, kSquadStride, kMaxSquads, gSquadNames, &gSquadParent, kSquadParentGroup);

		// Squads are what every label is keyed on; groups are a nicety. Resolving zero squads is a FAILURE to
		// retry, not a valid empty result - no shipped level has none.
		if (gSquadNames.empty())
			return false;

		PLOG_INFO << "AI Squad Overlay: resolved " << gSquadNames.size() << " squads and "
			<< gGroupNames.size() << " squad groups from the live scenario tag";
		return true;
	}

	// ================================================================================================
	// SIM THREAD, once per simulation tick, after the whole simulation update.
	//
	// ⚠ BULK-COPY THE ENTRY ARRAY. Reading the table field by field costs ~1500 SEH-guarded reads per tick
	// against an 800-2048 slot table; copying the whole array once and filtering in local memory costs ~200.
	// The __try frame is the expense, not the bytes.
	// ================================================================================================
	// Fills `built` and returns true, or returns false meaning "the simulation is not in a state I can read".
	// ⚠ EVERY false RETURN CLEARS THE ON-SCREEN LABELS (see squadPump). That is deliberate: during a level
	// load, a BSP switch or a revert this returns false for many ticks in a row, and keeping the previous
	// snapshot would pin fifty labels to the world coordinates the units occupied before the load.
	static bool buildSnapshot(uintptr_t tls, std::vector<AiLabel>& built) noexcept
	{
		uint8_t initialised = 0;
		if (!HCEGetPlayerState::tryReadRaw(tls + kSimInitialisedByte, &initialised, sizeof(initialised)) || !initialised)
			return false;

		// Re-resolve the scenario names whenever the scenario tag pointer changes (a level load).
		// ⚠ The key is committed ONLY on success - see refreshScenarioNames. And the sim thread must never
		// block here, so a contended lock just means "retry next tick".
		if (mScenarioSlot)
		{
			uintptr_t scenario = 0;
			if (rdPtr(mScenarioSlot, scenario) && scenario != mCachedScenario)
			{
				std::unique_lock lock(gSnapshotMutex, std::try_to_lock);
				if (!lock.owns_lock()) return false;
				if (refreshScenarioNames(scenario))
				{
					mCachedScenario = scenario;

					// New level => new gate chain. Both of these are cheap but neither is per-tick work.
					// ⚠⚠⚠ getCurrentLevelName RETURNS THE FULL TAG PATH, e.g. "levels\halo1\solo\c10\c10",
					// NOT "c10". The gate table is keyed on the short code, so comparing the two with ==
					// matched NOTHING on EVERY level and the overlay drew everything white however
					// correct the rest of the chain was. HCEStateHook has the same problem and solves it
					// by substring-matching; the last path component is exact, so use that.
					gLevelName.clear();
					if (auto ps = gPlayerStateWeak.lock())
					{
						try
						{
							std::string full = ps->getCurrentLevelName();
							const size_t cut = full.find_last_of("\\/");
							gLevelName = (cut == std::string::npos) ? full : full.substr(cut + 1);
						}
						catch (...) { gLevelName.clear(); }
					}
					resetGateState();

					size_t gatesForLevel = 0;
					for (size_t gi = 0; gi < HCEProgressionGates::kGateCount; ++gi)
						if (gLevelName == HCEProgressionGates::kGates[gi].level) ++gatesForLevel;

					PLOG_INFO << "AI Squad Overlay: level '" << gLevelName << "' resolved from the tag path, "
						<< gatesForLevel << " progression gates, chain starts at sequence " << (int)gActiveSequence;
					if (gatesForLevel == 0)
						PLOG_INFO << "AI Squad Overlay: no gate table for '" << gLevelName
							<< "' - every label will be neutral on this level, which is correct for b30/b40 "
							"(they have no kill-gated progression at all) and a bug for anything else.";
				}
			}
		}

		uintptr_t objHdr = 0, entries = 0, aiHdr = 0, actorBase = 0;
		if (!rdPtr(tls + kObjectHeaderTls, objHdr)) return false;
		if (!rdPtr(objHdr + kDataArrayBase, entries)) return false;

		int32_t highWater = 0;
		if (!HCEGetPlayerState::tryReadRaw(objHdr + kDataArrayHighWater, &highWater, sizeof(highWater))) return false;
		if (highWater <= 0 || highWater > 8192) return false;

		uint64_t stride = 0;
		if (!HCEGetPlayerState::tryReadRaw(objHdr + kDataArrayStride, &stride, sizeof(stride))) return false;
		if (stride != kObjectEntryStride) return false;   // ⚠ read it, do not assume - bail if the layout moved

		// The AI table is optional: a level with no AI still has objects. ⚠ Its high-water mark and stride are
		// read and CHECKED, not assumed: an object's +0x1AC is only an actor handle for things that actually
		// have an actor, and vehicles/giants are in kLabelledTypeMask too. An unchecked index of up to 0xFFFF
		// would read up to 0xFFFF * 0xD10 = ~209 MB past the table - SEH-safe, but any committed page there
		// returns plausible small integers and mislabels the AI with an unrelated squad.
		int32_t actorHighWater = 0;
		uint64_t actorStride = 0;
		const bool haveActors =
			rdPtr(tls + kAiHeaderTls, aiHdr)
			&& rdPtr(aiHdr + kDataArrayBase, actorBase)
			&& HCEGetPlayerState::tryReadRaw(aiHdr + kDataArrayHighWater, &actorHighWater, sizeof(actorHighWater))
			&& HCEGetPlayerState::tryReadRaw(aiHdr + kDataArrayStride, &actorStride, sizeof(actorStride))
			&& actorStride == kActorStride
			&& actorHighWater > 0 && actorHighWater <= 8192;

		static std::vector<uint8_t> entryBytes;
		entryBytes.resize((size_t)highWater * kObjectEntryStride);
		if (!HCEGetPlayerState::tryReadRaw(entries, entryBytes.data(), entryBytes.size())) return false;

		built.clear();
		built.reserve(64);

		for (int32_t i = 0; i < highWater && built.size() < kMaxLabels; ++i)
		{
			const uint8_t* e = entryBytes.data() + (size_t)i * kObjectEntryStride;
			const uint16_t salt = *reinterpret_cast<const uint16_t*>(e + kEntrySalt);
			if (salt == 0) continue;                                    // free slot

			const uint8_t type = *(e + kEntryType);
			if (type >= 32 || !((1u << type) & kLabelledTypeMask)) continue;   // the engine's own team mask

			const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(e + kEntryObject);
			if (obj < 0x10000ull) continue;

			uint32_t flags = 0;
			if (!HCEGetPlayerState::tryReadRaw(obj + kObjFlags, &flags, sizeof(flags))) continue;
			if (flags & kObjDeadBit) continue;                          // the engine's own dead flag

			AiLabel label{};
			label.isCreature = (type == kTypeCreature);
			if (!HCEGetPlayerState::tryReadRaw(obj + kObjPosition, &label.position, sizeof(label.position)))
				continue;
			if (!std::isfinite(label.position.x) || !std::isfinite(label.position.y) || !std::isfinite(label.position.z))
				continue;

			HCEGetPlayerState::tryReadRaw(obj + kObjTeam, &label.team, sizeof(label.team));

			int32_t actorHandle = -1;
			HCEGetPlayerState::tryReadRaw(obj + kObjActorHandle, &actorHandle, sizeof(actorHandle));
			// ⚠ -1 is the cheapest 'this is the player or a scripted prop' discriminator there is.
			if (actorHandle != -1 && haveActors)
			{
				const uint32_t actorIndex = (uint32_t)actorHandle & 0xFFFFu;
				if (actorIndex >= (uint32_t)actorHighWater) { built.push_back(label); continue; }
				const uintptr_t actor = actorBase + (uintptr_t)actorIndex * kActorStride;
				HCEGetPlayerState::tryReadRaw(actor + kActorCurrentSquad, &label.currentSquad, sizeof(label.currentSquad));
				HCEGetPlayerState::tryReadRaw(actor + kActorOriginSquad, &label.originSquad, sizeof(label.originSquad));
				label.migrated = (label.currentSquad != label.originSquad)
					&& label.currentSquad >= 0 && label.originSquad >= 0;
			}

			built.push_back(label);
		}

		// Script time is 30 Hz while the sim ticks at 60, but sleep/sleep_until scale by the live rate, so a
		// 45 s script timeout really is 45 real seconds. Read the rate rather than assuming it.
		{
			uintptr_t gtg = 0;
			uint16_t rate = 0;
			if (rdPtr(tls + kGameTimeGlobalsTls, gtg)
				&& HCEGetPlayerState::tryReadRaw(gtg + 0x06, &rate, sizeof(rate))
				&& rate > 0 && rate <= 1000)
				gTickRate = rate;
		}

		if (built.size() >= kMaxLabels)
		{
			static bool warnedOnce = false;
			if (!warnedOnce)
			{
				warnedOnce = true;
				PLOG_ERROR << "AI Squad Overlay: hit the " << kMaxLabels << "-label cap. Gate counts are now "
					"UNDERCOUNTS and the progression colouring may advance early - raise kMaxLabels.";
			}
		}

		classifyGates(built);
		return true;
	}

	// ================================================================================================
	// PROGRESSION GATES - decide who is REQUIRED to die right now.
	//
	// ⚠⚠⚠ THE WHOLE DIFFICULTY IS "WHICH GATE IS LIVE", NOT "WHICH SQUADS ARE GATES". c10 calls
	// f_run_gauntlet_wave six times IN SEQUENCE and each call BLOCKS on its own two gates, so exactly one
	// wave's gates matter at a time. Colouring every gauntlet squad at once is wrong five times out of six.
	//
	// So the sequence counter LATCHES: it starts at the lowest sequence in the table and only ever moves
	// FORWARD, and only once that sequence has been ENGAGED (some gate in it actually had living members)
	// and then satisfied. That mirrors the script's blocking structure exactly, and it is what makes waves
	// 2..6 light up when their turn comes instead of never.
	//
	// ⚠ A sequence whose gates read "satisfied" before it was ever engaged must NOT advance the counter -
	// at level start every count is zero, so every gate trivially satisfies its <= comparison and an
	// un-latched counter would run straight to the end of the table and colour nothing for the whole level.
	// ================================================================================================
	static inline std::weak_ptr<HCEGetPlayerState> gPlayerStateWeak;
	static inline std::string gLevelName;
	static inline int8_t  gActiveSequence = 0;
	static inline bool    gSequenceEngaged = false;
	static inline uint32_t gSequenceTicks = 0;      // sim ticks since this sequence became engaged
	static inline uint16_t gTickRate = 60;
	static inline std::vector<float> gGatePeak;     // per-gate strength-at-spawn, for FractionAtMost

	static void resetGateState()
	{
		gActiveSequence = INT8_MIN;
		gSequenceEngaged = false;
		gSequenceTicks = 0;
		gGatePeak.assign(HCEProgressionGates::kGateCount, 0.f);

		// Start at the lowest sequence this level actually has.
		for (size_t i = 0; i < HCEProgressionGates::kGateCount; ++i)
			if (gLevelName == HCEProgressionGates::kGates[i].level)
				if (gActiveSequence == INT8_MIN || HCEProgressionGates::kGates[i].sequence < gActiveSequence)
					gActiveSequence = HCEProgressionGates::kGates[i].sequence;
		if (gActiveSequence == INT8_MIN) gActiveSequence = 0;
	}

	// Does this label sit inside the gate's target squad / squad group? ⚠ CURRENT squad, never originating:
	// ai_living_count and ai_nonswarm_count both count present membership, and migration is the entire point.
	static bool labelInGate(const AiLabel& l, const HCEProgressionGates::Gate& g)
	{
		if (l.currentSquad < 0) return false;
		const size_t s = (size_t)l.currentSquad;

		if (!g.isGroup)
			return s < gSquadNames.size() && gSquadNames[s] == g.target;

		// ⚠⚠⚠ WALK THE WHOLE GROUP CHAIN, not just the immediate parent. The scripts gate on an OUTER
		// group (`sq_gr_gauntlet3`) while the tag files the biped under an inner one
		// (`sq_gr_gauntlet3_combat`), and ai_living_count on the outer group counts everything beneath it.
		// Checking only the immediate parent matched nothing and left the whole overlay colourless.
		if (s >= gSquadParent.size()) return false;
		int16_t node = gSquadParent[s];
		for (int depth = 0; depth < 16 && node >= 0 && (size_t)node < gGroupNames.size(); ++depth)
		{
			if (gGroupNames[(size_t)node] == g.target) return true;
			// The depth cap also contains a malformed tag: a parent cycle would otherwise spin forever
			// on the simulation thread.
			node = ((size_t)node < gGroupParent.size()) ? gGroupParent[(size_t)node] : (int16_t)-1;
		}
		return false;
	}

	static void classifyGates(std::vector<AiLabel>& built)
	{
		if (gLevelName.empty() || gGatePeak.size() != HCEProgressionGates::kGateCount) return;

		// Count living members per gate. nonSwarmOnly excludes creatures, exactly as ai_nonswarm_count does.
		std::vector<float> living(HCEProgressionGates::kGateCount, 0.f);
		for (size_t i = 0; i < HCEProgressionGates::kGateCount; ++i)
		{
			const auto& g = HCEProgressionGates::kGates[i];
			if (gLevelName != g.level) continue;
			// ⚠ Weighted, not counted. The scripts use three different counting functions and they
			// disagree about infection forms - see HCEProgressionGates::Gate::creatureWeight.
			for (const AiLabel& l : built)
				if (labelInGate(l, g))
					living[i] += l.isCreature ? g.creatureWeight : 1.f;
			gGatePeak[i] = std::max(gGatePeak[i], living[i]);
		}

		auto satisfied = [&](size_t i)
			{
				const auto& g = HCEProgressionGates::kGates[i];
				switch (g.kind)
				{
				case HCEProgressionGates::GateKind::KillAll:      return living[i] <= 0.f;
				case HCEProgressionGates::GateKind::CountAtMost:  return living[i] <= g.threshold;
				case HCEProgressionGates::GateKind::FractionAtMost:
					// The script captures strength AT SPAWN; the closest honest proxy we have is the peak
					// we have observed. Before anything has spawned there is no fraction to speak of.
					if (gGatePeak[i] <= 0.f) return true;
					return (living[i] / gGatePeak[i]) <= g.threshold;
				}
				return true;
			};

		// Advance the latch: engage on any living member, and only then allow the sequence to complete.
		for (int guard = 0; guard < 64; ++guard)
		{
			bool anySequenceGate = false, allSatisfied = true, anyLiving = false;
			for (size_t i = 0; i < HCEProgressionGates::kGateCount; ++i)
			{
				const auto& g = HCEProgressionGates::kGates[i];
				// sequence -1 is ALWAYS-ACTIVE and deliberately excluded from the latch: it is not a step
				// in the chain, so it must never hold the chain back nor push it forward.
				// sequence -1 is ALWAYS-ACTIVE and advisory gates only decorate: neither may engage the
				// latch nor hold it back. An advisory KillAll on a population that never fully dies would
				// otherwise freeze the whole chain at its sequence.
				if (gLevelName != g.level || g.sequence < 0 || g.sequence != gActiveSequence || g.advisory) continue;
				anySequenceGate = true;
				if (living[i] > 0.f) anyLiving = true;
				if (!satisfied(i)) allSatisfied = false;
			}
			if (!anySequenceGate) break;                       // past the end of this level's chain
			if (anyLiving) gSequenceEngaged = true;
			if (!(gSequenceEngaged && allSatisfied)) break;    // still the live one

			++gActiveSequence;                                 // this step is done - move on
			gSequenceEngaged = false;
			gSequenceTicks = 0;
		}

		++gSequenceTicks;
		const float elapsed = (float)gSequenceTicks / (float)(gTickRate ? gTickRate : 60);

		// Paint. A hard gate outranks a soft one: if a unit is in both, what matters is that it must die.
		for (size_t i = 0; i < HCEProgressionGates::kGateCount; ++i)
		{
			const auto& g = HCEProgressionGates::kGates[i];
			if (gLevelName != g.level) continue;
			if (g.sequence >= 0 && g.sequence != gActiveSequence) continue;   // -1 = always active
			// An advisory gate decorates the CURRENTLY ENGAGED step only. Without this it lights up the
			// moment the level loads, long before the fight it belongs to has started.
			if (g.advisory && !gSequenceEngaged) continue;
			if (satisfied(i)) continue;

			const uint8_t cls = (g.timeoutSeconds <= 0.f) ? 1u : 2u;
			for (AiLabel& l : built)
			{
				if (!labelInGate(l, g)) continue;
				if (l.isCreature && g.creatureWeight <= 0.f) continue;   // excluded from the count entirely
				if (cls == 1u || l.gateClass == 0u)
				{
					l.gateClass = cls;
					// ⚠ Measured from when WE saw the sequence engage, not from the script's own timer -
					// we do not read the sleep_until frame. If the overlay was off when the wave started
					// this reads late, so it is only shown while it is still plausible.
					l.secondsRemaining = (cls == 2u && gSequenceEngaged)
						? std::max(0.f, g.timeoutSeconds - elapsed) : -1.f;
				}
			}
		}
	}

	static void squadPump(uintptr_t tls) noexcept
	{
		if (!gActive.load(std::memory_order_acquire)) return;

		// Reused across ticks so the common path allocates nothing on the simulation's critical path.
		static std::vector<AiLabel> built;
		const bool ok = buildSnapshot(tls, built);

		// ⚠ try_to_lock, NEVER a blocking lock. The render thread holds this mutex, and the sim thread is
		// inside a midhook on the simulation's critical path: blocking here would stop the game simulating
		// until the render thread was scheduled again - a hitch per frame at best, a full freeze if the
		// render thread ever stalls. A missed tick just means the labels are one tick old.
		std::unique_lock lock(gSnapshotMutex, std::try_to_lock);
		if (!lock.owns_lock()) return;

		if (ok)
			gLabels.swap(built);
		else
			gLabels.clear();   // the sim is not readable - take the stale labels DOWN, do not keep drawing them

		gSnapshotSerial.fetch_add(1, std::memory_order_release);
	}

	// ---- render ------------------------------------------------------------------------------------
	struct Camera
	{
		SimpleMath::Vector3 position, forward, right, up;
		float focalPixels = 0.f;
		SimpleMath::Vector2 screenCentre;
	};
	static constexpr float kNearPlane = 0.01f;

	static Camera makeCameraFromUe(const SimpleMath::Vector3& positionBlam, float pitchDeg, float yawDeg,
		float rollDeg, const SimpleMath::Vector2& screenSize, float horizontalFovDegrees)
	{
		Camera camera;
		camera.position = positionBlam;
		HCEGetCameraData::ueRotationToBlamBasis(pitchDeg, yawDeg, rollDeg, camera.forward, camera.right, camera.up);
		const float fov = DirectX::XMConvertToRadians(std::clamp(horizontalFovDegrees, 10.f, 170.f));
		camera.focalPixels = screenSize.x / (2.f * std::tan(fov * 0.5f));
		camera.screenCentre = SimpleMath::Vector2(screenSize.x * 0.5f, screenSize.y * 0.5f);
		return camera;
	}

	static SimpleMath::Vector3 toCameraSpace(const Camera& camera, const SimpleMath::Vector3& world)
	{
		const SimpleMath::Vector3 delta = world - camera.position;
		return SimpleMath::Vector3(delta.Dot(camera.right), delta.Dot(camera.up), delta.Dot(camera.forward));
	}

	std::string labelTextFor(const AiLabel& label) const
	{
		std::string squad = "(no squad)";
		if (label.originSquad >= 0 && (size_t)label.originSquad < gSquadNames.size()
			&& !gSquadNames[(size_t)label.originSquad].empty())
			squad = gSquadNames[(size_t)label.originSquad];

		std::string out = squad;

		// The GROUP CHAIN, innermost first. Showing the whole chain rather than just the immediate parent
		// is what makes this self-diagnosing: the scripts gate on an OUTER group, so if the label only
		// showed `sq_gr_gauntlet3_combat` you could not tell it sits under `sq_gr_gauntlet3`.
		if (label.originSquad >= 0 && (size_t)label.originSquad < gSquadParent.size())
		{
			std::string chain;
			int16_t node = gSquadParent[(size_t)label.originSquad];
			for (int depth = 0; depth < 16 && node >= 0 && (size_t)node < gGroupNames.size(); ++depth)
			{
				if (!gGroupNames[(size_t)node].empty())
					chain += (chain.empty() ? "" : " / ") + gGroupNames[(size_t)node];
				node = ((size_t)node < gGroupParent.size()) ? gGroupParent[(size_t)node] : (int16_t)-1;
			}
			if (!chain.empty()) out += "  <" + chain + ">";
		}

		// ⚠ MIGRATION IS THE HEADLINE. A unit whose current squad differs from its originating one has been
		// ai_migrate'd - which on c10 means it has crossed into the bloodgate volume and now counts against
		// the level's only hard gate. Nothing in game marks these.
		if (label.migrated)
		{
			std::string moved = "?";
			if ((size_t)label.currentSquad < gSquadNames.size() && !gSquadNames[(size_t)label.currentSquad].empty())
				moved = gSquadNames[(size_t)label.currentSquad];
			out += "  -> " + moved;
		}

		return out;
	}

	bool teamShown(int team) const
	{
		auto settings = settingsWeak.lock();
		if (!settings) return true;
		switch (bucketOf(team))
		{
		case TeamBucket::Player:   return settings->hceAISquadOverlayShowPlayer->GetValue();
		case TeamBucket::Human:    return settings->hceAISquadOverlayShowHuman->GetValue();
		case TeamBucket::Covenant: return settings->hceAISquadOverlayShowCovenant->GetValue();
		case TeamBucket::Flood:    return settings->hceAISquadOverlayShowFlood->GetValue();
		case TeamBucket::Sentinel: return settings->hceAISquadOverlayShowSentinel->GetValue();
		default:                   return settings->hceAISquadOverlayShowOther->GetValue();
		}
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (GlobalKill::isKillSet()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (!settings->hceAISquadOverlayToggle->GetValue()) return;
			if (!mCameraDataOptionalWeak.has_value()) return;
			auto cameraData = mCameraDataOptionalWeak.value().lock();
			if (!cameraData) return;

			HCEGetCameraData::UeCamera ue;
			if (!cameraData->getUeCamera(ue)) return;

			const Camera camera = makeCameraFromUe(ue.positionBlam, ue.pitchDegrees, ue.yawDegrees,
				ue.rollDegrees, screenSize, ue.horizontalFovDegrees);

			const float labelScale = settings->hceAISquadOverlayLabelScale->GetValue();
			const float renderDistance = settings->hceAISquadOverlayRenderDistance->GetValue();
			const float zOffset = settings->hceAISquadOverlayLabelHeight->GetValue();
			const float creatureScale = settings->hceAISquadOverlayCreatureScale->GetValue();

			const SimpleMath::Vector4 neutralColour = settings->hceAISquadOverlayNeutralColor->GetValue();
			const SimpleMath::Vector4 migratedColour = settings->hceAISquadOverlayMigratedColor->GetValue();
			auto pack = [](const SimpleMath::Vector4& c)
				{
					return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w));
				};
			const ImU32 neutralPacked = pack(neutralColour);
			const ImU32 migratedPacked = pack(migratedColour);
			const ImU32 requiredPacked = pack(settings->hceAISquadOverlayRequiredColor->GetValue());
			const ImU32 timedPacked = pack(settings->hceAISquadOverlayTimedColor->GetValue());
			const bool showRequired = settings->hceAISquadOverlayTraitRequired->GetValue();
			const bool showTimed = settings->hceAISquadOverlayTraitTimed->GetValue();
			const bool showMigrated = settings->hceAISquadOverlayTraitMigrated->GetValue();

			std::scoped_lock lock(gSnapshotMutex);
			for (const AiLabel& label : gLabels)
			{
				if (!teamShown(label.team)) continue;

				const SimpleMath::Vector3 anchor(label.position.x, label.position.y, label.position.z + zOffset);
				if ((anchor - camera.position).Length() > renderDistance) continue;

				const SimpleMath::Vector3 cameraSpace = toCameraSpace(camera, anchor);
				if (cameraSpace.z < kNearPlane) continue;   // behind the camera

				const float scale = camera.focalPixels / cameraSpace.z;
				const SimpleMath::Vector2 screen(camera.screenCentre.x + cameraSpace.x * scale,
					camera.screenCentre.y - cameraSpace.y * scale);
				if (screen.x < 0 || screen.y < 0 || screen.x > screenSize.x || screen.y > screenSize.y) continue;

				// Shrink distant labels so a crowded fight stays readable rather than becoming a wall of text.
				float sizeScale = std::clamp(3.f / std::max(cameraSpace.z, 0.5f), 0.35f, 1.f);

				// Creatures (infection forms) are tiny and arrive in clumps of dozens, so at the shared size
				// their labels are a solid block of text with no model visible behind it. Scaled separately,
				// and NOT through the distance floor above - the whole point is that they go smaller.
				if (label.isCreature)
					sizeScale *= creatureScale;

				std::string text = labelTextFor(label);
				if (settings->hceAISquadOverlayShowTeam->GetValue())
					text += std::string("  [") + teamName(label.team) + "]";

				// A live countdown, but ONLY when we watched the wave start - see classifyGates.
				if (label.gateClass == 2 && showTimed && label.secondsRemaining >= 0.f)
					text += "  " + std::to_string((int)std::ceil(label.secondsRemaining)) + "s";

				// REQUIRED outranks everything: the point of the colour is "this one has to die".
				// ⚠ A disabled trait FALLS THROUGH to the next one it qualifies for, and ultimately to
				// neutral - it never hides the label. That is what makes these usable as a way to see
				// past an overlapping trait (migrated sitting on top of required) rather than as a filter.
				const ImU32 colour =
					(label.gateClass == 1 && showRequired) ? requiredPacked :
					(label.gateClass == 2 && showTimed) ? timedPacked :
					(label.migrated && showMigrated) ? migratedPacked : neutralPacked;

				RenderTextHelper::drawCenteredOutlinedText(text, screen, colour, labelScale * sizeScale);
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void setRegistered(bool wanted)
	{
		if (wanted && !mRegistered)
		{
			if (!HCEGameThreadPump::add(&squadPump))
				throw HCMRuntimeException("AI Squad Overlay: the simulation-thread pump table is full");
			mRegistered = true;
		}
		else if (!wanted && mRegistered)
		{
			HCEGameThreadPump::remove(&squadPump);
			mRegistered = false;
		}
	}

	// HCEGetCameraData installs its DoUpdateCamera midhook on a REFERENCE COUNT, not on construction - resolving
	// the cheat is not enough, every consumer has to register its own request or the hook is never installed and
	// getUeCamera() returns nothing. Missing this is why the overlay only drew while the trigger overlay (which
	// does register one) happened to be on. Same shape as HCEBspOverlay::attachCameraHook.
	void attachCameraHook()
	{
		if (!mCameraDataOptionalWeak.has_value())
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the AI "
				"Squad Overlay cannot read the render camera");
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData)
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the AI "
				"Squad Overlay cannot read the render camera");

		if (mCameraHookRequested) return;
		cameraData->setHookWanted(this, true);   // may throw; only recorded once it did not
		mCameraHookRequested = true;
	}

	void detachCameraHook()
	{
		if (!mCameraHookRequested) return;
		mCameraHookRequested = false;
		if (!mCameraDataOptionalWeak.has_value()) return;
		if (auto cameraData = mCameraDataOptionalWeak.value().lock())
		{
			try { cameraData->setHookWanted(this, false); }
			catch (HCMRuntimeException) {}   // releasing a request cannot meaningfully fail
		}
	}

	// The whole arm/disarm body, shared by the toggle and by the game-state hook. CALLER holds no lock.
	void applyWanted(bool want)
	{
			if (want)
			{
				// Resolve the two module globals the scenario read needs. Both are already proven addresses
				// used by the trigger and BSP overlays; they are resolved by signature, never hard-coded.
				mScenarioSlot = HCEAnchors::get(HCEAnchors::Anchor::ScenarioDataPointer);
				mTagAddressTable = HCEAnchors::get(HCEAnchors::Anchor::TagAddressTable);
				if (!mScenarioSlot || !mTagAddressTable)
					throw HCMRuntimeException("AI Squad Overlay: the scenario anchors did not resolve on this "
						"build of Halo Campaign Evolved, so squad names cannot be read");

				mCachedScenario = 0;   // force a name refresh on the next pump tick
				attachCameraHook();
				setRegistered(true);
				gActive.store(true, std::memory_order_release);
			}
			else
			{
				gActive.store(false, std::memory_order_release);
				setRegistered(false);
				detachCameraHook();
				std::scoped_lock lock(gSnapshotMutex);
				gLabels.clear();
			}

			// Self-heal, same as the trigger and BSP overlays: attachCameraHook is a no-op once THIS overlay's
			// request is registered, so if anything detached the midhook behind our back it would otherwise stay
			// detached for the rest of the session. Runs on the state-hook thread, never the render thread.
			if (want && mCameraDataOptionalWeak.has_value())
				if (auto cameraData = mCameraDataOptionalWeak.value().lock())
					cameraData->ensureHookLive();
	}

	void onToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			applyWanted(newValue);

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue ? "AI Squad Overlay on." : "AI Squad Overlay off.");
		}
		catch (HCMRuntimeException ex)
		{
			gActive.store(false, std::memory_order_release);
			try { setRegistered(false); } catch (...) {}
			try { detachCameraHook(); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	// ⚠ WITHOUT THIS the overlay is silently dead whenever the toggle was ALREADY on before the sim dll was
	// loaded - a persisted setting or a preset. The value-changed event fired during deserialisation, long
	// before this Impl existed to subscribe, so onToggle never runs: the checkbox reads ON, gActive stays
	// false, and nothing is ever drawn until the user manually toggles it off and on. Same fix, and the same
	// stated reason, as HCETriggerOverlay::onGameStateChanged.
	void onGameStateChanged(const MCCState& newState)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);

			// A level transition invalidates the cached scenario pointer and every name resolved from it.
			mCachedScenario = 0;
			{
				std::scoped_lock lock(gSnapshotMutex);
				gLabels.clear();
			}

			applyWanted(settings->hceAISquadOverlayToggle->GetValue()
				&& newState.currentGameState == mGame
				&& newState.currentPlayState == PlayState::Ingame);
		}
		catch (HCMRuntimeException ex)
		{
			gActive.store(false, std::memory_order_release);
			try { setRegistered(false); } catch (...) {}
			try { detachCameraHook(); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - ScopedCallbacks subscribe inside their own constructors.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceAISquadOverlayToggle->valueChangedEvent,
			[this](bool& n) { onToggle(n); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(),
			[this](SimpleMath::Vector2 ss) { onRenderEvent(ss); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEAISquadOverlay only supports Halo Campaign Evolved");

		mPlayerState = resolveDependentCheat(HCEGetPlayerState);
		gPlayerStateWeak = mPlayerState;   // the sim-thread pump is static and needs the level name
		mPump = resolveDependentCheat(HCEGameThreadPumpHost);

		// The UE render camera. HCEGetCameraData is an OPTIONAL CHEAT, not a DI-container service - it has to
		// come through resolveDependentCheat like every other consumer (dicon.Resolve<HCEGetCameraData>()
		// throws "Could not locate type in DIContainer" and takes the whole overlay's GUI row down with it).
		// Without it labels still draw, but off the sim's player-aim basis at a default FOV, i.e. visibly
		// misplaced - so this degrades rather than throwing.
		try
		{
			mCameraDataOptionalWeak = resolveDependentCheat(HCEGetCameraData);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCEAISquadOverlay could not resolve HCEGetCameraData; labels would be positioned "
				"with the sim's player camera and a default field of view";
		}

		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		gActive.store(false, std::memory_order_release);
		try { setRegistered(false); } catch (...) {}
		try { detachCameraHook(); } catch (...) {}   // never leak this overlay's midhook reference
		mToggleCallback.removeCallback();
		mRenderEventCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCEAISquadOverlay::HCEAISquadOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}

HCEAISquadOverlay::~HCEAISquadOverlay()
{
	PLOG_VERBOSE << "~" << getName();
}
