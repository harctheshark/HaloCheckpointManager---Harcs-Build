#include "pch.h"
#include "HavokSleepCountdown.h"
#include "GameTickEventHook.h"
#include "GetPlayerDatum.h"
#include "GetObjectAddress.h"
#include "ObjectTypes.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "IMCCStateHook.h"
#include "IMakeOrGetCheat.h"
#include "GlobalKill.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "imgui.h"

// ================================================================================================================
// HAVOK SLEEP COUNTDOWN (Halo 2, MCC halo2.dll 1.3528, PE stamp 0x68A0F0F2)
//
// Havok 2 deactivates ("sleeps") a simulation island when its bodies stay put. The island check (sub_1809E4480, called
// from the world step only while world+0x2BD is set) adds the step's dt to two island timers every step:
//   - SHORT check: timer +0x48 passes the world's short period (world+0x48). Every body's deactivator slot-5 test must say
//     "quiet"; if one does not, nothing changes. If all do: quiet rounds (+0x40) < 5 -> rounds + 1; at 5 -> SLEEP, rounds 0.
//   - LONG check (only when no short check is due that step): timer +0x4C passes world+0x4C; all slot-6 tests quiet -> SLEEP.
// Slot 5/6 of Havok's spatial deactivator (sub_1809DB9D0 / sub_1809DBB00) compare the body's transform translation
// (motion +0xC0) and orientation quaternion (motion +0x40) with the reference the deactivator stored at its last FAILED
// test; Bungie's subclass (vtable 0xB19600) first asks its keep-awake check sub_180715480 (bipeds airborne past the
// coyote window, driven vehicles, a per-body flag), which answers "not quiet" whatever the motion.
// Every offset below is MCC x64, IDA-confirmed (H2 Ghosting Research\db\h2_full.i64); the Vista values (Cartographer's
// havok_sleep.cpp) are in the comments: deactivator and island fields moved +0x10 / +0x14, biped/vehicle fields +0x30.
// ================================================================================================================
namespace
{
	constexpr uint32_t kBuildStamp = 0x68A0F0F2;

	// module RVAs
	constexpr uintptr_t kWorldPtr = 0x15FDFE0;               // hkWorld*
	constexpr uintptr_t kTimeGlobalsPtr = 0x15FE008;         // time globals*: i16 ticks per second at +2 (sub_180706710)
	constexpr uintptr_t kComponentTablePtr = 0x1648940;      // havok components data array (rows 0xC0)
	constexpr uintptr_t kTagInstancesPtr = 0x15E4B30;        // tag instances (16 B each, i32 data offset at +8)
	constexpr uintptr_t kTagDataBasePtr = 0x15E4B38;
	constexpr uintptr_t kVtWorld = 0xB71A28;                 // hkWorld::vftable
	constexpr uintptr_t kVtIsland = 0xB73498;                // hkSimulationIsland::vftable
	constexpr uintptr_t kVtSpatial = 0xB729E0;               // hkSpatialRigidBodyDeactivator::vftable
	constexpr uintptr_t kVtBungie = 0xB19600;                // Bungie's subclass (ctor in sub_18070FCB0)
	constexpr uintptr_t kVtFake = 0xB73390;                  // hkFakeRigidBodyDeactivator: both tests answer "not quiet"
	constexpr uintptr_t kSleepingDeactivator = 0xE1A568;     // the shared empty object every new body points at

	// hkWorld
	constexpr uintptr_t kWorldActive = 0x10, kWorldActiveCount = 0x18;     // Vista +0x08
	constexpr uintptr_t kWorldInactive = 0x20, kWorldInactiveCount = 0x28; // Vista +0x14
	constexpr uintptr_t kWorldPeriodShort = 0x48, kWorldPeriodLong = 0x4C; // Vista +0x30/+0x34 (hkWorldCinfo +0x88/+0x8C)
	constexpr uintptr_t kWorldDeactivationOn = 0x2BD;                      // Vista +0x23D
	// hkSimulationIsland (ctor sub_1809E1F80)
	constexpr uintptr_t kIslandWorld = 0x30, kIslandRounds = 0x40, kIslandTimerShort = 0x48, kIslandTimerLong = 0x4C;
	constexpr uintptr_t kIslandEntities = 0x50, kIslandEntityCount = 0x58;
	// hkRigidBody / motion
	constexpr uintptr_t kBodyMotion = 0x68, kBodyDeactivator = 0x90;        // Vista +0x3C / +0x54
	constexpr uintptr_t kMotionRotation = 0x40, kMotionTranslation = 0xC0;  // Vista +0x30 / +0xB0
	// deactivator (Vista: every field -0x10)
	constexpr uintptr_t kDeactRadiusSq = 0x10, kDeactShortPos = 0x20, kDeactShortRot = 0x30, kDeactLongPos = 0x40, kDeactLongRot = 0x50;
	constexpr uintptr_t kDeactShortT = 0x60, kDeactShortR = 0x64, kDeactLongT = 0x68, kDeactLongR = 0x6C;
	constexpr uintptr_t kDeactObject = 0x70, kDeactBodyIndex = 0x74;
	// havok component row: rigid-body entries at +0x70 (0x70 each, the body at +0x40, the keep-awake flag byte at +0x48)
	constexpr uintptr_t kCompEntries = 0x70, kCompEntryCount = 0x78, kEntryStride = 0x70, kEntryBody = 0x40, kEntryFlag = 0x48;
	// object / biped / vehicle (sub_180715480, sub_18092A8A0, sub_180927B70, sub_18091B540)
	constexpr uintptr_t kObjHavok = 0xB4, kObjParent = 0x14, kObjPhysicsFlagsHi = 0xC1;   // bit 8 of the word at +0xC0
	constexpr uintptr_t kBipedPhysicsState = 0x393, kBipedAirborne = 0x390, kBipedAirborneTicks = 0x3E1, kBipedMode = 0x428;
	constexpr uintptr_t kVehicleDefFlags = 0x1EC, kVehSpeed = 0x39C, kVehGear = 0x3D3, kVehTurn = 0x3A4, kVehSlide = 0x3A0;
	constexpr uintptr_t kVehDriverPower = 0x270, kVehGunnerPower = 0x274;

	constexpr int kRoundsToSleep = 5;
	constexpr int kIslandsShown = 5, kBodiesShown = 6, kIslandsConsidered = 64, kEntitiesRead = 64, kPlayerBodiesMax = 32;

	// ---- guarded reads (no C++ objects in the __try frame) ----
	bool rawCopy(void* dst, uintptr_t src, size_t n)
	{
		if (src < 0x10000) return false;
		__try { memcpy(dst, (const void*)src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	template<typename T> T rd(uintptr_t a, T fallback = T{}) { T v; return rawCopy(&v, a, sizeof(T)) ? v : fallback; }
	bool plausible(uintptr_t p) { return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 3) == 0; }

	enum class Veto : uint8_t { None, Asleep, NoDeactivator, NeverSleeps, UnknownClass, EntryFlag, BipedAirborne, Vehicle };
	const char* vetoText(Veto v)
	{
		switch (v)
		{
		case Veto::Asleep: return "asleep";
		case Veto::NoDeactivator: return "no deactivator (never quiet)";
		case Veto::NeverSleeps: return "never sleeps (fake deactivator)";
		case Veto::UnknownClass: return "unknown deactivator";
		case Veto::EntryFlag: return "kept awake (body flag)";
		case Veto::BipedAirborne: return "kept awake (airborne)";
		case Veto::Vehicle: return "kept awake (vehicle moving/driven)";
		default: return "";
		}
	}
	const char* typeName(int t)
	{
		static const char* const k[] = { "biped", "vehicle", "weapon", "equipment", "garbage", "projectile", "scenery",
			"machine", "control", "light", "placeholder", "sound scenery", "crate", "creature" };
		return (t >= 0 && t < (int)std::size(k)) ? k[t] : "object";
	}

	struct Body
	{
		uintptr_t entity = 0;
		char label[40] = {};
		bool player = false, playerBiped = false;
		bool bungie = false;            // Bungie's deactivator (keep-awake check first): a fresh body's, until its island has slept
		Veto veto = Veto::None;
		bool quietShort = false, quietLong = false, tested = false;
		bool stillNow = false;          // quiet, or (with a stale reference) too slow to fail the short test over a whole period
		float shortDist = 0, shortDistMax = 0, shortRot = 0, shortRotMax = 0;
		float longDist = 0, longDistMax = 0, longRot = 0, longRotMax = 0;
		float speed = 0;
	};
	struct Island
	{
		uintptr_t island = 0;
		bool active = false, hasPlayer = false;
		float distance = 1e9f;
		int rounds = 0, entityCount = 0, notQuiet = 0;
		float timerShort = 0, timerLong = 0;
		int ticksToShort = 0, ticksToLong = 0, ticksToSleep = 0;
		bool allQuiet = false, allQuietLong = false, allStillNow = false, predictionValid = true, neverSleeps = false;
		// Tantrums need the player's biped on Havok's plain spatial deactivator: while it still has Bungie's (a new body - spawn,
		// revert, any state change), its keep-awake check answers "not quiet" in the air. The island's first sleep swaps it:
		// the wake builds a plain one (Cartographer's havok_sleep.cpp, tantrum_blocked).
		bool tantrumBlocked = false;
		bool jumpReady = false;         // every body still, the prediction valid: a stationary jump can be timed (the auto-jump's gate)
		Body bodies[kBodiesShown]; int bodyCount = 0;
	};
	struct Readout
	{
		bool valid = false;
		std::string note;
		uint32_t tick = 0;              // the game tick counter when sampled (end of a tick: the next tick starts from this state)
		int ticksPerSecond = 30;
		float periodShort = 0, periodLong = 0;
		int ticksPerShortPeriod = 6;
		bool deactivationOn = false;
		int activeIslands = 0, inactiveIslands = 0;
		bool playerAsleep = false;      // the player's island not among the awake ones (asleep, or its body on the sleeping deactivator)
		uint32_t playerBipedDatum = 0xFFFFFFFFu;
		uintptr_t playerDeactivator = 0;
		Island islands[kIslandsShown]; int islandCount = 0;
	};
	struct PlayerBody { uintptr_t entity; bool vehicle; };

	// the last jump the player's biped started, judged against the readout it was pressed under (Cartographer's judge)
	struct Judge
	{
		uint32_t tick = 0xFFFFFFFFu;
		bool automatic = false, judged = false, ready = false, blocked = false, hit = false;
		int offset = 0;                 // shown countdown minus the jump line: 0 on it, > 0 early, < 0 late
		int ticksToSleep = 0;
	};
}

class HavokSleepCountdown::Impl
{
	GameState mGame;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::optional<std::weak_ptr<GetPlayerDatum>> playerDatumWeak;
	std::optional<std::weak_ptr<GetObjectAddress>> objectAddressWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::unique_ptr<ScopedCallback<RenderEvent>> mRenderCallback;
	std::unique_ptr<ScopedCallback<GameTickEvent>> mTickCallback;
	std::weak_ptr<GameTickEventHook> mTickHookWeak;

	// written on the game thread (the tick event, the jump check), read by the window: all under mReadoutMutex
	std::mutex mReadoutMutex;
	Readout mReadout;
	Judge mJudge;
	int mJudgedCount = 0, mHitCount = 0;
	// the auto-jump: one press per cycle; it arms again once the player's island has slept
	bool mAutoSpent = false;
	uint32_t mAutoLastTick = 0xFFFFFFFFu;
	int mAutoCount = 0;
	// the player's island as the previous sample saw it - to read a sleep that was over before any sample could see it
	bool mPrevValid = false, mPrevActive = false;
	int mPrevRounds = 0;
	uintptr_t mPrevDeactivator = 0;

	std::unordered_map<uintptr_t, std::array<float, 3>> mLastPos;   // per body: its translation at mLastPosTick (for its speed)
	std::unordered_map<uintptr_t, float> mLastSpeed;                 // per body: the speed measured when the tick last advanced
	uint32_t mLastPosTick = 0;
	std::atomic_bool mRunning = false;

	uintptr_t mBase = 0;

	// ---- the jump check hook (sub_18092BFA0, MCC 1.3528; Vista 0x558C3F): biped datum in ecx, al = 1 when a jump started.
	// It reads the unit control flags (+0x150, the same offset as Vista) and starts a jump on bit 1 through the jump gate
	// sub_180927B70 and the engine's other conditions - so the auto-jump's press goes through exactly what a real press does.
	static constexpr uintptr_t kRvaJumpCheck = 0x92BFA0;
	static constexpr uint8_t kJumpCheckEntry[15] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 };
	static constexpr uintptr_t kUnitControlFlags = 0x150;           // bit 1 = jump (held for this one tick)
	static inline Impl* sInstance = nullptr;
	static inline ModuleInlineHook* sJumpHook = nullptr;
	std::unique_ptr<ModuleInlineHook> mJumpHook;

	static __int64 __fastcall jumpCheckDetour(uint32_t biped)
	{
		if (Impl* self = sInstance) self->beforeJumpCheck(biped);
		const __int64 result = sJumpHook->getInlineHook().call<__int64, uint32_t>(biped);
		if (Impl* self = sInstance) self->afterJumpCheck(biped, (result & 0xFF) != 0);
		return result;
	}

	bool jumpCheckSiteMatches() const
	{
		const auto base = (const uint8_t*)GetModuleHandleW(L"halo2.dll");
		if (!base) return false;
		const uint8_t* at = base + kRvaJumpCheck;
		return !IsBadReadPtr(at, sizeof(kJumpCheckEntry)) && std::memcmp(at, kJumpCheckEntry, sizeof(kJumpCheckEntry)) == 0;
	}

	uint32_t currentTick()
	{
		if (auto hook = mTickHookWeak.lock()) try { return hook->getCurrentGameTick(); } catch (HCMRuntimeException) {}
		return 0;
	}

	bool autoJumpEnabled() const
	{
		auto s = settingsWeak.lock();
		return s && s->havokSleepAutoJump->GetValue();
	}

	// the readout the tick in progress started from (sampled at the end of the previous tick); null when stale
	const Readout* readoutForThisTick(uint32_t now) const
	{
		return (mReadout.valid && mReadout.islandCount > 0 && now - mReadout.tick <= 1) ? &mReadout : nullptr;
	}

	// the auto-jump: on the tick whose starting readout puts the player's island exactly on the jump line, with every body still
	// and tantrums not blocked, set the jump bit in the player's unit control flags - this tick's jump check then starts a jump as
	// if the player had pressed it. Once per cycle.
	void beforeJumpCheck(uint32_t biped)
	{
		if (!autoJumpEnabled()) return;
		std::scoped_lock lock(mReadoutMutex);
		if (mAutoSpent) return;
		const uint32_t now = currentTick();
		const Readout* r = readoutForThisTick(now);
		if (!r || mAutoLastTick == now || (biped & 0xFFFF) != (r->playerBipedDatum & 0xFFFF)) return;
		const Island& is = r->islands[0];
		if (!is.hasPlayer || !is.active || !is.jumpReady || is.tantrumBlocked) return;
		if (is.ticksToSleep != jumpMarkTicks(r->ticksPerSecond)) return;
		Halo2ObjectType type;
		const uintptr_t unit = objectAt(biped, type);
		if (!unit || type != Halo2ObjectType::Biped) return;
		if (!orByte(unit + kUnitControlFlags, 0x02)) return;
		mAutoLastTick = now;
		++mAutoCount;
		mAutoSpent = true;
	}

	static bool orByte(uintptr_t at, uint8_t bits)
	{
		__try { *reinterpret_cast<uint8_t*>(at) |= bits; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// a jump the player's biped started (the check answered 1): judge it against the readout it was pressed under, the way the
	// auto-jump decides its press - offset = shown countdown - the jump line; a hit needs it on the line, every body still and
	// tantrums not blocked. A jump the engine refused is not a jump.
	void afterJumpCheck(uint32_t biped, bool jumped)
	{
		if (!jumped) return;
		std::scoped_lock lock(mReadoutMutex);
		const uint32_t now = currentTick();
		if (mJudge.tick == now) return;
		if ((biped & 0xFFFF) != (mReadout.playerBipedDatum & 0xFFFF)) return;
		Judge j;
		j.tick = now;
		j.automatic = mAutoLastTick == now;
		if (const Readout* r = readoutForThisTick(now))
		{
			const Island& is = r->islands[0];
			if (is.hasPlayer && is.active && is.predictionValid && !is.neverSleeps)
			{
				j.judged = true;
				j.ready = is.jumpReady;
				j.blocked = is.tantrumBlocked;
				j.ticksToSleep = is.ticksToSleep;
				j.offset = is.ticksToSleep - jumpMarkTicks(r->ticksPerSecond);
				j.hit = j.offset == 0 && j.ready && !j.blocked;
			}
		}
		mJudge = j;
		if (j.judged) ++mJudgedCount;
		if (j.hit) ++mHitCount;
	}

	// an object's address and Halo 2 type, or 0
	uintptr_t objectAt(uint32_t datum, Halo2ObjectType& type)
	{
		type = Halo2ObjectType::FailedTypeConversion;
		if (datum == 0xFFFFFFFFu || !objectAddressWeak.has_value()) return 0;
		auto objects = objectAddressWeak.value().lock();
		if (!objects) return 0;
		try { return objects->getObjectAddress(Datum((uint16_t)(datum & 0xFFFF), (uint16_t)(datum >> 16)), &type); }
		catch (HCMRuntimeException) { return 0; }
	}

	// the havok component row of an object (by its datum at +0xB4), or 0
	uintptr_t componentOf(uintptr_t object)
	{
		const uint32_t havok = rd<uint32_t>(object + kObjHavok, 0xFFFFFFFFu);
		if (havok == 0xFFFFFFFFu || havok == 0) return 0;
		const uintptr_t hdr = rd<uintptr_t>(mBase + kComponentTablePtr);
		if (!plausible(hdr)) return 0;
		const uintptr_t dataOff = rd<uint64_t>(hdr + 0x48);
		if (!dataOff) return 0;
		return hdr + dataOff + 0xC0ull * (havok & 0xFFFF);
	}

	int bodiesOf(uintptr_t object, uintptr_t* out, int max)
	{
		const uintptr_t comp = componentOf(object);
		if (!comp) return 0;
		const uintptr_t entries = rd<uintptr_t>(comp + kCompEntries);
		const int count = rd<int32_t>(comp + kCompEntryCount);
		if (!plausible(entries) || count <= 0 || count > 64) return 0;
		int n = 0;
		for (int i = 0; i < count && n < max; ++i)
			if (const uintptr_t e = rd<uintptr_t>(entries + kEntryStride * i + kEntryBody); plausible(e)) out[n++] = e;
		return n;
	}

	bool translation(uintptr_t entity, float out[3])
	{
		const uintptr_t motion = rd<uintptr_t>(entity + kBodyMotion);
		return plausible(motion) && rawCopy(out, motion + kMotionTranslation, 12);
	}

	// Bungie's keep-awake check sub_180715480 (+ the biped gate sub_18092A8A0/sub_180927B70 and vehicle sub_18091B540)
	Veto keepAwake(uintptr_t deactivator, int ticksPerSecond, Body& b)
	{
		const uint32_t objectDatum = rd<uint32_t>(deactivator + kDeactObject, 0xFFFFFFFFu);
		const int bodyIndex = rd<int32_t>(deactivator + kDeactBodyIndex, -1);
		Halo2ObjectType type;
		const uintptr_t object = objectAt(objectDatum, type);
		if (!object) return Veto::None;
		if (!b.label[0]) snprintf(b.label, sizeof(b.label), "%s %04X", typeName((int)type), objectDatum & 0xFFFF);

		bool flag = false;
		if (const uintptr_t comp = componentOf(object))
		{
			const uintptr_t entries = rd<uintptr_t>(comp + kCompEntries);
			const int count = rd<int32_t>(comp + kCompEntryCount);
			if (plausible(entries) && bodyIndex >= 0 && bodyIndex < count)
				flag = rd<uint8_t>(entries + kEntryStride * bodyIndex + kEntryFlag) == 1;
		}

		if (type == Halo2ObjectType::Biped)
		{
			if (rd<uint8_t>(object + kBipedPhysicsState) == 1) return Veto::None;
			if (flag) return Veto::EntryFlag;
			const uint8_t mode = rd<uint8_t>(object + kBipedMode);
			if (mode != 1) return Veto::None;                                       // sub_18092A8A0: ground mode only
			const int vetoTicks = (int)std::lround(0.18f * (float)ticksPerSecond);
			const bool airborne = (rd<uint8_t>(object + kObjPhysicsFlagsHi) & 1) == 0
				&& (rd<uint8_t>(object + kBipedAirborne) & 1) != 0
				&& (int)rd<int8_t>(object + kBipedAirborneTicks) >= vetoTicks
				&& rd<int32_t>(object + kObjParent, 0) == -1;
			return airborne ? Veto::BipedAirborne : Veto::None;
		}
		if (type == Halo2ObjectType::Vehicle)
		{
			if (flag) return Veto::EntryFlag;
			const uint16_t tag = rd<uint16_t>(object);
			const uintptr_t instances = rd<uintptr_t>(mBase + kTagInstancesPtr), dataBase = rd<uintptr_t>(mBase + kTagDataBasePtr);
			if (!plausible(instances) || !dataBase) return Veto::None;
			const uintptr_t def = dataBase + (intptr_t)rd<int32_t>(instances + 16ull * tag + 8);
			const uint32_t f = rd<uint32_t>(def + kVehicleDefFlags);
			const bool moving = ((f & 1) && (rd<float>(object + kVehSpeed) != 0.f || rd<uint8_t>(object + kVehGear) != 0xFF))
				|| ((f & 2) && rd<float>(object + kVehTurn) != 0.f)
				|| ((f & 4) && rd<float>(object + kVehDriverPower) != 0.f)
				|| ((f & 8) && rd<float>(object + kVehGunnerPower) != 0.f)
				|| ((f & 0x20) && rd<float>(object + kVehSlide) != 0.f);
			return moving ? Veto::Vehicle : Veto::None;
		}
		return flag ? Veto::EntryFlag : Veto::None;
	}

	// Havok's spatial tests (sub_1809DB9D0 short / sub_1809DBB00 long), against the stored references
	void spatialTests(uintptr_t entity, uintptr_t deactivator, Body& b)
	{
		float s[0x70 / 4]; float rot[4]; float t[3];
		const uintptr_t motion = rd<uintptr_t>(entity + kBodyMotion);
		if (!plausible(motion) || !rawCopy(s, deactivator, sizeof(s)) || !rawCopy(rot, motion + kMotionRotation, 16) || !rawCopy(t, motion + kMotionTranslation, 12)) return;
		const float radiusSq = s[kDeactRadiusSq / 4];
		auto test = [&](uintptr_t posOff, uintptr_t rotOff, uintptr_t tOff, uintptr_t rOff, float& dist, float& distMax, float& r, float& rMax)
			{
				const float* p = &s[posOff / 4]; const float* q = &s[rotOff / 4];
				const float dx = p[0] - t[0], dy = p[1] - t[1], dz = p[2] - t[2];
				const float d2 = dx * dx + dy * dy + dz * dz;
				float r2 = 0; for (int i = 0; i < 4; ++i) { const float d = q[i] - rot[i]; r2 += d * d; }
				r2 *= radiusSq;
				distMax = s[tOff / 4]; rMax = s[rOff / 4];
				dist = std::sqrt(d2); r = std::sqrt((std::max)(r2, 0.f));
				return d2 < distMax * distMax && r2 < rMax * rMax;   // the engine: NOT quiet when thr^2 <= d^2 or thr^2 <= r^2
			};
		const bool shortOk = test(kDeactShortPos, kDeactShortRot, kDeactShortT, kDeactShortR, b.shortDist, b.shortDistMax, b.shortRot, b.shortRotMax);
		const bool longOk = test(kDeactLongPos, kDeactLongRot, kDeactLongT, kDeactLongR, b.longDist, b.longDistMax, b.longRot, b.longRotMax);
		b.tested = true;
		b.quietShort = b.veto == Veto::None && shortOk;
		b.quietLong = b.veto == Veto::None && longOk;
	}

	void describeBody(uintptr_t entity, const PlayerBody* players, int playerCount, int tps, uint32_t tickDelta, Body& b,
		std::unordered_map<uintptr_t, std::array<float, 3>>& seen)
	{
		b = Body{};
		b.entity = entity;
		for (int i = 0; i < playerCount; ++i)
			if (players[i].entity == entity) { b.player = true; b.playerBiped = !players[i].vehicle; snprintf(b.label, sizeof(b.label), "%s", players[i].vehicle ? "your vehicle" : "your biped"); break; }

		float t[3];
		if (translation(entity, t))
		{
			if (tickDelta == 0)
			{
				// same tick as the stored positions: keep them, and the speed they gave
				if (auto it = mLastPos.find(entity); it != mLastPos.end()) seen[entity] = it->second; else seen[entity] = { t[0], t[1], t[2] };
				if (auto it = mLastSpeed.find(entity); it != mLastSpeed.end()) b.speed = it->second;
			}
			else
			{
				if (auto it = mLastPos.find(entity); it != mLastPos.end())
				{
					const float dx = t[0] - it->second[0], dy = t[1] - it->second[1], dz = t[2] - it->second[2];
					b.speed = std::sqrt(dx * dx + dy * dy + dz * dz) * (float)tps / (float)tickDelta;
				}
				mLastSpeed[entity] = b.speed;
				seen[entity] = { t[0], t[1], t[2] };
			}
		}

		const uintptr_t deactivator = rd<uintptr_t>(entity + kBodyDeactivator);
		if (!deactivator) b.veto = Veto::NoDeactivator;
		else if (deactivator == mBase + kSleepingDeactivator) b.veto = Veto::Asleep;
		else if (!plausible(deactivator)) b.veto = Veto::UnknownClass;
		else
		{
			const uintptr_t vt = rd<uintptr_t>(deactivator);
			if (vt == mBase + kVtFake) b.veto = Veto::NeverSleeps;
			else if (vt == mBase + kVtBungie) { b.bungie = true; b.veto = keepAwake(deactivator, tps, b); spatialTests(entity, deactivator, b); }
			else if (vt == mBase + kVtSpatial) { b.veto = Veto::None; spatialTests(entity, deactivator, b); }
			else b.veto = Veto::UnknownClass;
		}
		if (!b.label[0]) snprintf(b.label, sizeof(b.label), "body");
	}

	// The countdown reading (Cartographer, refined): the check runs when the timer, after this step's dt, exceeds the period
	// strictly, and keeps the remainder - so this reads 5, 4, 3, 2, 1, 0 over a period at 30 ticks per second (11..0 at 60),
	// 0 being the tick before the check tick: a press made at reading N lands N ticks before the check.
	static int ticksUntil(float period, float timer, float dt) { return (std::max)(0, (int)std::ceil((period - timer) / dt - 0.0001f)); }

	void readIsland(uintptr_t isl, bool active, bool hasPlayer, float distance, const PlayerBody* players, int playerCount, uint32_t tickDelta,
		Readout& r, Island& out, std::unordered_map<uintptr_t, std::array<float, 3>>& seen)
	{
		out = Island{};
		out.island = isl; out.active = active; out.hasPlayer = hasPlayer; out.distance = distance;
		out.rounds = rd<uint8_t>(isl + kIslandRounds);
		out.timerShort = rd<float>(isl + kIslandTimerShort);
		out.timerLong = rd<float>(isl + kIslandTimerLong);
		out.entityCount = rd<int32_t>(isl + kIslandEntityCount);
		const float dt = 1.f / (float)(std::max)(r.ticksPerSecond, 1);
		out.ticksToShort = ticksUntil(r.periodShort, out.timerShort, dt);
		out.ticksToLong = ticksUntil(r.periodLong, out.timerLong, dt);
		// if nothing in it moves from here: the all-quiet check made at 5 rounds sleeps the island, so (5 - rounds) counting
		// checks come first. Refined below once the bodies say whether the next check counts.
		out.ticksToSleep = out.ticksToShort + (std::max)(0, kRoundsToSleep - out.rounds) * r.ticksPerShortPeriod;
		if (!active) return;

		const uintptr_t entities = rd<uintptr_t>(isl + kIslandEntities);
		const int count = (std::min)(out.entityCount, kEntitiesRead);
		uintptr_t list[kEntitiesRead];
		if (!plausible(entities) || count <= 0 || !rawCopy(list, entities, sizeof(uintptr_t) * count)) return;
		out.allQuiet = out.allQuietLong = out.allStillNow = true;
		for (int pass = 0; pass < 2; ++pass)            // the player's bodies get the first rows
			for (int i = 0; i < count; ++i)
			{
				bool isPlayer = false;
				for (int p = 0; p < playerCount; ++p) if (players[p].entity == list[i]) isPlayer = true;
				if ((pass == 0) != isPlayer) continue;
				Body b; describeBody(list[i], players, playerCount, r.ticksPerSecond, tickDelta, b, seen);
				b.stillNow = b.quietShort || (b.veto == Veto::None && b.speed * r.periodShort < b.shortDistMax);
				if (!b.stillNow) out.allStillNow = false;
				if (b.veto == Veto::UnknownClass) out.predictionValid = false;
				if (b.veto == Veto::NoDeactivator || b.veto == Veto::NeverSleeps) out.neverSleeps = true;
				if (b.playerBiped && b.bungie) out.tantrumBlocked = true;
				if (!b.quietShort) { out.allQuiet = false; ++out.notQuiet; }
				if (!b.quietLong) out.allQuietLong = false;
				if (out.bodyCount < kBodiesShown) out.bodies[out.bodyCount++] = b;
			}

		out.jumpReady = out.predictionValid && !out.neverSleeps && out.allStillNow;
		// with 5/5 stored and every body still, the first check only re-samples and the jump is timed to it: no extra period
		const bool stored = out.allStillNow && out.rounds >= kRoundsToSleep;
		if (out.predictionValid && !out.allQuiet && !stored)
			out.ticksToSleep += r.ticksPerShortPeriod;   // the next check won't count (a body is past its reference): one period more
		if (out.allQuietLong && out.ticksToLong < out.ticksToSleep)
			out.ticksToSleep = out.ticksToLong;           // the long check sleeps the island at once when every body passes it
	}

	void sample(Readout& r, uint32_t tick)
	{
		r = Readout{};
		r.tick = tick;
		mBase = (uintptr_t)GetModuleHandleW(L"halo2.dll");
		if (!mBase) { r.note = "halo2.dll not loaded"; return; }
		const uint32_t pe = rd<uint32_t>(mBase + 0x3C);
		if (rd<uint32_t>(mBase + pe + 8) != kBuildStamp) { r.note = "unsupported halo2.dll build (made for MCC 1.3528)"; return; }

		const uintptr_t timeGlobals = rd<uintptr_t>(mBase + kTimeGlobalsPtr);
		const int16_t tps = plausible(timeGlobals) ? rd<int16_t>(timeGlobals + 2) : 0;
		r.ticksPerSecond = tps > 0 ? tps : 30;

		const uintptr_t world = rd<uintptr_t>(mBase + kWorldPtr);
		if (!plausible(world) || rd<uintptr_t>(world) != mBase + kVtWorld) { r.note = "no physics world"; return; }
		r.periodShort = rd<float>(world + kWorldPeriodShort);
		r.periodLong = rd<float>(world + kWorldPeriodLong);
		if (!(r.periodShort > 0.f && r.periodShort < 60.f && r.periodLong > 0.f && r.periodLong < 600.f))
		{
			r.note = std::format("sleep periods not recognised ({:.3f} / {:.3f})", r.periodShort, r.periodLong);
			return;
		}
		r.deactivationOn = rd<uint8_t>(world + kWorldDeactivationOn) != 0;
		r.ticksPerShortPeriod = (std::max)(1, (int)std::lround(r.periodShort * (float)r.ticksPerSecond));
		r.valid = true;

		// the player's biped and vehicle bodies, and the player's position
		PlayerBody players[kPlayerBodiesMax]; int playerCount = 0;
		float playerPos[3] = {}; bool havePos = false;
		if (playerDatumWeak.has_value())
			if (auto pd = playerDatumWeak.value().lock())
			{
				Datum unitDatum;
				try { unitDatum = pd->getPlayerDatum(); } catch (HCMRuntimeException) {}
				Halo2ObjectType type;
				if (const uintptr_t unit = objectAt((uint32_t)unitDatum, type))
				{
					r.playerBipedDatum = (uint32_t)unitDatum;
					uintptr_t e[16];
					const int n = bodiesOf(unit, e, 16);
					for (int i = 0; i < n && playerCount < kPlayerBodiesMax; ++i) players[playerCount++] = { e[i], false };
					if (n > 0) r.playerDeactivator = rd<uintptr_t>(e[0] + kBodyDeactivator);
					if (n > 0 && translation(e[0], playerPos)) havePos = true;
					else if (rawCopy(playerPos, unit + 0x64, 12)) havePos = true;   // object position
					Halo2ObjectType parentType;
					if (const uintptr_t parent = objectAt(rd<uint32_t>(unit + kObjParent, 0xFFFFFFFFu), parentType))
					{
						const int m = bodiesOf(parent, e, 16);
						for (int i = 0; i < m && playerCount < kPlayerBodiesMax; ++i) players[playerCount++] = { e[i], true };
					}
				}
			}

		// every island, awake and asleep: has-player first, then by distance
		struct Candidate { uintptr_t island; bool active; bool hasPlayer; float distance; };
		std::vector<Candidate> cands;
		cands.reserve(kIslandsConsidered);
		auto collect = [&](uintptr_t listOff, uintptr_t countOff, bool active, int& total)
			{
				const uintptr_t list = rd<uintptr_t>(world + listOff);
				const int count = rd<int32_t>(world + countOff);
				total = (count > 0 && count <= 4096) ? count : 0;
				if (!plausible(list)) return;
				for (int i = 0; i < total && (int)cands.size() < kIslandsConsidered; ++i)
				{
					const uintptr_t isl = rd<uintptr_t>(list + 8ull * i);
					if (!plausible(isl) || rd<uintptr_t>(isl) != mBase + kVtIsland || rd<uintptr_t>(isl + kIslandWorld) != world) continue;
					Candidate c{ isl, active, false, 1e9f };
					const uintptr_t entities = rd<uintptr_t>(isl + kIslandEntities);
					const int n = (std::min)(rd<int32_t>(isl + kIslandEntityCount), kEntitiesRead);
					uintptr_t ents[kEntitiesRead];
					if (plausible(entities) && n > 0 && rawCopy(ents, entities, sizeof(uintptr_t) * n))
						for (int k = 0; k < n; ++k)
						{
							for (int p = 0; p < playerCount; ++p) if (players[p].entity == ents[k] && !players[p].vehicle) c.hasPlayer = true;
							float t[3];
							if (havePos && translation(ents[k], t))
								c.distance = (std::min)(c.distance, std::sqrt((t[0] - playerPos[0]) * (t[0] - playerPos[0]) + (t[1] - playerPos[1]) * (t[1] - playerPos[1]) + (t[2] - playerPos[2]) * (t[2] - playerPos[2])));
						}
					cands.push_back(c);
				}
			};
		collect(kWorldActive, kWorldActiveCount, true, r.activeIslands);
		collect(kWorldInactive, kWorldInactiveCount, false, r.inactiveIslands);
		std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b)
			{ return a.hasPlayer != b.hasPlayer ? a.hasPlayer : a.distance < b.distance; });

		std::unordered_map<uintptr_t, std::array<float, 3>> seen;
		const uint32_t tickDelta = (mLastPosTick != 0 && tick > mLastPosTick && tick - mLastPosTick < 64) ? tick - mLastPosTick : (tick == mLastPosTick ? 0u : 1u);
		r.islandCount = (!cands.empty() && cands[0].hasPlayer) ? 1 : 0;      // your biped's island only
		for (int i = 0; i < r.islandCount; ++i)
			readIsland(cands[i].island, cands[i].active, cands[i].hasPlayer, cands[i].distance, players, playerCount, tickDelta, r, r.islands[i], seen);
		mLastPos = std::move(seen);
		if (tickDelta != 0) mLastPosTick = tick;

		// a body with no awake island is asleep (it points at the shared sleeping deactivator) - say so, not "not found"
		r.playerAsleep = playerCount > 0 && (r.islandCount == 0 || !r.islands[0].active || r.playerDeactivator == mBase + kSleepingDeactivator);
		if (cands.empty()) r.note = "no islands";
		else if (playerCount == 0) r.note = "your biped has no physics body right now";
	}

	// game thread, once per tick: the snapshot the next tick's jump check (and the window) works from; re-arm the auto-jump once
	// the player's island has slept. A sleep can be over within the tick it began (a jump buffered into the tantrum wakes the
	// island the moment the freeze lands the biped), so a sleep no sample caught is read from its traces: the rounds falling while
	// the island stayed awake (the sleeping check zeroes them), or a new deactivator object on the player's body (the wake builds one).
	void onTick(uint32_t)
	{
		if (GlobalKill::isKillSet()) return;
		Readout r;
		sample(r, currentTick());
		std::scoped_lock lock(mReadoutMutex);
		const Island* island = r.islandCount > 0 && r.islands[0].hasPlayer ? &r.islands[0] : nullptr;
		if (mAutoSpent && r.valid)
		{
			const bool sleptNow = r.playerAsleep;
			const bool sleptUnseen = island && island->active && mPrevValid && mPrevActive
				&& (island->rounds < mPrevRounds || (r.playerDeactivator && mPrevDeactivator && r.playerDeactivator != mPrevDeactivator));
			if (sleptNow || sleptUnseen) mAutoSpent = false;
		}
		mPrevValid = island != nullptr;
		mPrevActive = island ? island->active : false;
		mPrevRounds = island ? island->rounds : 0;
		mPrevDeactivator = r.playerDeactivator;
		mReadout = std::move(r);
	}

	// ---- the "jump now" line (the player's island), on the refined countdown above ----
	// With quiet rounds at 5/5, a 3.08 wu/s jump pressed at this reading freezes the player at the apex: the island sleeps on a
	// check that finds the player's height unchanged since the check before - the two straddle the apex of the (symmetric) arc.
	// Measured (user, 2026-10-26, on the old floor+1 reading: 3 at 30 tick/s, 2 at 60). On the refined reading: 2 at 30
	// (Cartographer's own note); at 60 the "one lower" guess (1) jumped 2 ticks late - the bar and the auto-jump both (user,
	// 2026-10-29) - so 3. Other rates: the nearer of the two.
	static int jumpMarkTicks(int ticksPerSecond) { return ticksPerSecond >= 45 ? 3 : 2; }

	// the judge line under the bar ("" when nothing was judged yet)
	std::string judgeText(const Judge& j, int judged, int hits, bool autoOn, bool spent) const
	{
		std::string s;
		if (j.tick != 0xFFFFFFFFu)
		{
			if (!j.judged) s = "last jump: not judged (no countdown)";
			else if (j.blocked) s = "last jump: tantrums blocked (sleep once first)";
			else if (!j.ready) s = "last jump: not still";
			else if (j.offset == 0) s = "last jump: PERFECT";
			else if (j.offset > 0) s = std::format("last jump: EARLY by {}", j.offset);
			else s = std::format("last jump: LATE by {}", -j.offset);
			if (j.automatic) s += " (auto)";
			s += std::format("   hits {}/{}", hits, judged);
		}
		if (autoOn) s += std::string(s.empty() ? "" : "   ") + (spent ? "auto-jump: used (re-arms after a sleep)" : "auto-jump: armed");
		return s;
	}

	// ---- the sleep bar (both modes): a label above, the draining to-sleep countdown, the JUMP line through it, a judge line ----
	// Fixed colours (nothing changes colour); only the fill and the numbers move. The bar's scale is the whole countdown from
	// 0/5 quiet rounds (6 short periods), so the line stays put.
	static float sleepBar(ImDrawList* dl, ImVec2 pos, float width, const Island& is, int tps, int perShort, const std::string& footer)
	{
		const ImU32 kGreen = IM_COL32(70, 230, 70, 255), kTrack = IM_COL32(25, 30, 25, 220), kEdge = IM_COL32(10, 10, 10, 255);
		const ImU32 kYellow = IM_COL32(240, 220, 40, 255), kBoxBg = IM_COL32(0, 0, 0, 190), kWhite = IM_COL32(235, 235, 235, 255);
		const float line = ImGui::GetTextLineHeight();
		const float barH = line * 0.85f;
		auto boxedText = [&](ImVec2 at, ImU32 col, const std::string& text)
			{
				const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
				dl->AddRectFilled(ImVec2(at.x - 3.f, at.y - 1.f), ImVec2(at.x + ts.x + 3.f, at.y + ts.y + 1.f), kBoxBg, 2.f);
				dl->AddText(at, col, text.c_str());
				return ts;
			};

		std::string label;
		if (!is.active) label = "asleep";
		else
			label = std::format("sleeps in {} ticks - {}", is.ticksToSleep,
				is.tantrumBlocked ? "tantrums won't work this cycle (sleep once first)" : "tantrums work this cycle");
		const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
		boxedText(ImVec2(pos.x + (width - ls.x) * 0.5f, pos.y), kGreen, label);

		const ImVec2 b0(pos.x, pos.y + line + 4.f), b1(pos.x + width, pos.y + line + 4.f + barH);
		const float total = (float)((kRoundsToSleep + 1) * perShort);
		const float frac = is.active ? std::clamp((float)is.ticksToSleep / total, 0.f, 1.f) : 0.f;
		dl->AddRectFilled(ImVec2(b0.x - 2.f, b0.y - 2.f), ImVec2(b1.x + 2.f, b1.y + 2.f), kEdge, 4.f);
		dl->AddRectFilled(b0, b1, kTrack, 3.f);
		if (frac > 0.f) dl->AddRectFilled(b0, ImVec2(b0.x + width * frac, b1.y), kGreen, 3.f);

		const int mark = jumpMarkTicks(tps);
		const float x = b0.x + width * std::clamp((float)mark / total, 0.f, 1.f);
		dl->AddLine(ImVec2(x, b0.y - 4.f), ImVec2(x, b1.y + 4.f), kYellow, 3.f);
		const std::string jump = "JUMP";
		const ImVec2 js = ImGui::CalcTextSize(jump.c_str());
		boxedText(ImVec2(std::clamp(x - js.x * 0.5f, pos.x, pos.x + width - js.x), b1.y + 6.f), kYellow, jump);
		float bottom = b1.y + 6.f + js.y + 2.f;
		if (!footer.empty())
		{
			const ImVec2 fs = ImGui::CalcTextSize(footer.c_str());
			boxedText(ImVec2(pos.x + (width - fs.x) * 0.5f, bottom + 2.f), kWhite, footer);
			bottom += fs.y + 4.f;
		}
		return bottom - pos.y;
	}

	void onRender(SimpleMath::Vector2 screenSize)
	{
		if (GlobalKill::isKillSet()) return;
		Readout r; Judge j; int judged = 0, hits = 0; bool spent = false;
		{
			std::scoped_lock lock(mReadoutMutex);
			r = mReadout; j = mJudge; judged = mJudgedCount; hits = mHitCount; spent = mAutoSpent;
		}
		const auto settings = settingsWeak.lock();
		const bool simple = settings && settings->havokSleepCountdownSimple->GetValue();
		const std::string footer = judgeText(j, judged, hits, autoJumpEnabled(), spent);
		const int tps = (std::max)(r.ticksPerSecond, 1);
		const int perShort = (std::max)(1, r.ticksPerShortPeriod);
		// the island the bar shows: yours, or an "asleep" one when your body has no awake island
		Island asleep; asleep.active = false; asleep.hasPlayer = true;
		const Island* shown = r.islandCount > 0 ? &r.islands[0] : (r.valid && r.playerAsleep ? &asleep : nullptr);

		if (simple)
		{
			// just the bar: a borderless, transparent, RESIZABLE window (move/resize it while HCM's cursor is free; a faint
			// outline shows its edges under the mouse). The bar fills the window's width; the text and bar thickness scale with
			// its height (the bar at scale s is ~3.9 lines + 16 px tall). ImGui remembers the size and place.
			const float baseFont = ImGui::GetFontSize();
			ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.62f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.f));
			ImGui::SetNextWindowSize(ImVec2(baseFont * 1.25f * 26.f, baseFont * 1.25f * 3.9f + 32.f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(baseFont * 8.f, baseFont * 2.f), ImVec2(FLT_MAX, FLT_MAX));
			if (ImGui::Begin("##havokSleepSimple", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar
				| ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground
				| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
			{
				const ImVec2 avail = ImGui::GetContentRegionAvail();
				ImGui::SetWindowFontScale(std::clamp((avail.y - 16.f) / (3.9f * baseFont), 0.6f, 6.f));
				if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
				{
					const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
					ImGui::GetWindowDrawList()->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(255, 255, 255, 90), 4.f);
				}
				const float width = (std::max)(avail.x, 50.f);
				if (r.valid && shown)
				{
					const float h = sleepBar(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), width, *shown, tps, perShort, footer);
					ImGui::Dummy(ImVec2(width, h));
				}
				else
					ImGui::TextUnformatted(r.note.empty() ? "waiting for the game ..." : r.note.c_str());
			}
			ImGui::End();
			return;
		}

		// resizable; opens 1.5x the old auto size with 1.5x text (the size is remembered by ImGui after a resize)
		ImGui::SetNextWindowPos(ImVec2(40, 260), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(1150, 560), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.8f);
		if (!ImGui::Begin("Havok sleep (Halo 2)", nullptr, ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
		{
			ImGui::End();
			return;
		}
		ImGui::SetWindowFontScale(1.5f);
		if (!r.valid)
		{
			ImGui::TextUnformatted(r.note.empty() ? "waiting for the game ..." : r.note.c_str());
			ImGui::End();
			return;
		}
		ImGui::Text("%d tick/s   short check every %d ticks, long every %d   jump line at %d", tps, perShort,
			(int)std::lround(r.periodLong * (float)tps), jumpMarkTicks(tps));
		if (!r.deactivationOn) ImGui::TextUnformatted("deactivation is OFF in this world: nothing sleeps");
		if (!r.note.empty()) ImGui::TextUnformatted(r.note.c_str());
		if (!shown) { ImGui::End(); return; }

		const Island& is = *shown;
		ImGui::Separator();
		ImGui::Text("YOUR ISLAND   %d bodies%s", is.entityCount, is.active ? "" : "   ASLEEP");
		if (is.active)
			ImGui::Text("sleeps in %d ticks (%.2f s) if it stays quiet   quiet rounds %d/%d   next check %d   long check %d   %s   %s   %s",
				is.ticksToSleep, (float)is.ticksToSleep / (float)tps, is.rounds, kRoundsToSleep, is.ticksToShort, is.ticksToLong,
				is.allQuiet ? "all quiet now" : std::format("{} moving", is.notQuiet).c_str(),
				is.jumpReady ? "ready to jump" : "not still",
				is.tantrumBlocked ? "tantrums: not yet (Bungie deactivator - sleep once)" : "tantrums: work");
		const float width = (std::max)(ImGui::GetContentRegionAvail().x, 100.f);
		const float h = sleepBar(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), width, is, tps, perShort, footer);
		ImGui::Dummy(ImVec2(width, h));
		if (!is.predictionValid) ImGui::TextUnformatted("(a body has an unrecognised deactivator: prediction uncertain)");
		for (int k = 0; k < is.bodyCount; ++k)
		{
			const Body& b = is.bodies[k];
			const char* state = b.quietShort ? "quiet " : (b.stillNow ? "still " : "moving");
			if (b.veto != Veto::None)
				ImGui::Text("    %-16s %5.2f wu/s  %s  %s", b.label, b.speed, state, vetoText(b.veto));
			else if (b.tested)
				ImGui::Text("    %-16s %5.2f wu/s  %s  moved %.3f of %.3f  turned %.3f of %.3f", b.label, b.speed, state,
					b.shortDist, b.shortDistMax, b.shortRot, b.shortRotMax);
			else
				ImGui::Text("    %-16s %5.2f wu/s  %s", b.label, b.speed, state);
		}
		if (is.entityCount > is.bodyCount) ImGui::Text("    ... %d more", is.entityCount - is.bodyCount);
		ImGui::End();
	}

	void setRunning(bool on)
	{
		if (on == mRunning) return;
		mRunning = on;
		if (on)
		{
			if (auto hook = mTickHookWeak.lock()) mTickCallback = hook->getGameTickEvent()->subscribe([this](uint32_t t) { onTick(t); });
			mRenderCallback = std::make_unique<ScopedCallback<RenderEvent>>(mRenderEvent, [this](SimpleMath::Vector2 s) { onRender(s); });
			// the jump check: the judge (and the auto-jump) - only on the build it was read from
			if (mJumpHook)
			{
				if (jumpCheckSiteMatches()) mJumpHook->setWantsToBeAttached(true);
				else PLOG_ERROR << "HavokSleepCountdown: the jump check's entry bytes differ (another halo2.dll build?) - no judge / auto-jump";
			}
		}
		else
		{
			if (mJumpHook) mJumpHook->setWantsToBeAttached(false);
			mTickCallback.reset();
			mRenderCallback.reset();
			std::scoped_lock lock(mReadoutMutex);
			mReadout = Readout{};
			mJudge = Judge{}; mJudgedCount = mHitCount = 0;
			mAutoSpent = false; mAutoLastTick = 0xFFFFFFFFu; mAutoCount = 0;
			mPrevValid = mPrevActive = false; mPrevRounds = 0; mPrevDeactivator = 0;
			mLastPos.clear(); mLastSpeed.clear(); mLastPosTick = 0;
		}
	}

	void update(bool toggle)
	{
		auto mcc = mccStateHookWeak.lock();
		setRunning(toggle && mcc && mcc->isGameCurrentlyPlaying(mGame));
	}

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mStateCallback;
	ScopedCallback<ToggleEvent> mAutoJumpCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		mRenderEvent(dicon.Resolve<RenderEvent>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->havokSleepCountdownToggle->valueChangedEvent, [this](bool& n) { update(n); }),
		mStateCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState&)
			{ if (auto s = settingsWeak.lock()) update(s->havokSleepCountdownToggle->GetValue()); }),
		mAutoJumpCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->havokSleepAutoJump->valueChangedEvent, [this](bool&)
			{ std::scoped_lock lock(mReadoutMutex); mAutoSpent = false; mAutoCount = 0; mAutoLastTick = 0xFFFFFFFFu; })
	{
		if (game.operator GameState::Value() != GameState::Value::Halo2) throw HCMInitException("Havok sleep countdown is Halo 2 only");
		mTickHookWeak = resolveDependentCheat(GameTickEventHook);
		try { playerDatumWeak = resolveDependentCheat(GetPlayerDatum); }
		catch (HCMInitException ex) { PLOG_ERROR << "HavokSleepCountdown: GetPlayerDatum unavailable (no 'your island'): " << ex.what(); }
		try { objectAddressWeak = resolveDependentCheat(GetObjectAddress); }
		catch (HCMInitException ex) { PLOG_ERROR << "HavokSleepCountdown: GetObjectAddress unavailable (no keep-awake rules or labels): " << ex.what(); }
		auto site = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring(L"halo2.dll"), std::vector<int64_t>{ (int64_t)kRvaJumpCheck });
		mJumpHook = ModuleInlineHook::make(L"halo2.dll", site, reinterpret_cast<void*>(&jumpCheckDetour));   // attached while running
		sJumpHook = mJumpHook.get();
		sInstance = this;
	}

	~Impl()
	{
		sInstance = nullptr;
		mToggleCallback.removeCallback();
		mStateCallback.removeCallback();
		mAutoJumpCallback.removeCallback();
		setRunning(false);
		mJumpHook.reset();
		sJumpHook = nullptr;
	}
};

HavokSleepCountdown::HavokSleepCountdown(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
HavokSleepCountdown::~HavokSleepCountdown() { PLOG_VERBOSE << "~" << getName(); }
