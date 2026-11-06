#include "pch.h"
#include "MPRevertRepair.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "GameTickEventHook.h"
#include "IMakeOrGetCheat.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "GlobalKill.h"

// ================================================================================================================================
// WHY (halo3.dll 1.3528, RE 2026-11-05; notes in Documents\Halo Mod And Tools\HCM H3 MP Revert\):
// In multiplayer (simulation mode 7) every object is also a NETWORK SIMULATION ENTITY. Two tables link them:
//   - game state (saved + restored by checkpoints): the data array at gameTLS+576, 16-byte entries {u16 salt, ..., +4 entity id,
//     +8 owner (object datum for objects), +12 byte}; an object keeps its entry's datum at object+204.
//   - the simulation's entity database (NOT in the game state): [*(sim)+8] + 0x2138, 1024 slots of 56 bytes from +32:
//     {+0 entity id, +4 type, +8 registered, +12 back-reference to the game-state entry}.
// A revert restores the first but not the second. An object that spawned after the checkpoint and was then deleted leaves its
// slot freed / reused, the restored game-state entries point at it, and the next entity update calls a dead handler (the crash:
// sub_1800522E8, AV at +0x5233C). Campaign never runs simulation mode 7, so it never needs this.
// WHAT (once per tick while the option is on, only when the game is multiplayer and the simulation is mode 7):
//   1. orphans - a slot whose game-state entry no longer points back at it: deleted BY ITS OWN ID with the game's delete
//      (sub_1800523A0; it frees slot id & 0x3FF without checking the salt, so a stale id is never passed to it);
//   2. stale objects - a game-state entry whose slot no longer holds its entity: the reference is dropped, the entry freed,
//      the object unlinked (sub_180347170) and its entities created again by the game's own path (sub_18001EA04), exactly as
//      the object delete / create paths (sub_18001ECC0 / sub_18001EA04) do it.
// Nothing happens while the two tables agree, which they always do between reverts.
//
// HALO 2 (halo2.dll 1.3528, RE 2026-11-05; notes in Documents\Halo Mod And Tools\HCM H2 MP Revert\): the same two tables and
// the same bug; the crash is an object entity's update (sub_1808BB4D0) resolving the owner object of its game-state entry and
// reading object+170 through null (AV at +0x8BB542). Differences from Halo 3:
//   - plain globals instead of thread-local storage: the game-state entity array at qword_1818A8E50 (2048 x 16 bytes, its +72
//     data / +80 bitmap are OFFSETS from the array header), the simulation at xmmword_1818A4338 (mode 5 = networked host);
//   - the entity database is [*(sim)+8] + 0x1138 with slots of 56 bytes from +40; a slot is in use / alive by bits 0 / 2 of
//     the byte at [db+24] + 140 + 4*index (the id is not cleared on free, so the bits decide);
//   - entities exist only when the session byte [qword_180E80A78]+12 is 5 and its word +706 is 0 (sub_1806A6420/6350/63E0);
//   - an object keeps its entry's datum at object+212; the object header array qword_1818B7398 (12-byte entries, +8 = offset
//     into the object pool qword_1818B7360, -1 when there is no object).
// The calls are the game's own: entity delete sub_180885EB0(id), entry free sub_180886530(datum), object unlink
// sub_1808D5500(object), object entity create sub_180883210(object) (as the object delete path sub_180883440 / create path).
// WHEN (2026-11-06, from the second crash): in Halo 2 the crash is NOT in the sim tick - it is network_send (sub_1808803C0 ->
// sub_180866990, entity update writing) which the main loop (sub_18067A220) runs every frame right after the revert handler
// (sub_180679D50: revert = sub_1806AFFD0, core load = sub_180678660), before any sim tick. So the Halo 2 repair runs from a
// mid hook at halo2+0x67A3B0 (`call sub_1806BF920`, the join right after `call sub_180679D50`, reached every frame by both
// branches of the loop), i.e. immediately after any revert / core load and before anything reads the entity database.
// ================================================================================================================================
namespace
{
	struct ScanResult
	{
		int nOrphan = 0; uint32_t orphan[1024];
		int nStale = 0; uint32_t staleObject[512]; uint32_t staleGs[512];
		int nOtherStale = 0;
		uintptr_t gsArray = 0, gsData = 0;
		int gate = 0;   // why the scan did not run (0 = it ran; logged when it changes)
	};

	bool readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
	bool bytesMatch(uintptr_t a, const uint8_t* expect, size_t n) { return readable((const void*)a, n) && memcmp((const void*)a, expect, n) == 0; }
	// target of a `mov reg, [rip+disp32]` whose disp32 is at a, the instruction ending at end
	uintptr_t ripTarget(uintptr_t a, uintptr_t end) { return end + *(const int32_t*)a; }

	// ---------------------------------------------------------------- Halo 3
	namespace h3
	{
		constexpr uintptr_t kRvaTlsIndex = 0xA39F9C;      // halo3.dll TlsIndex
		constexpr uintptr_t kRvaDelete   = 0x523A0;       // sim_entity_delete(id)
		constexpr uintptr_t kRvaGsFree   = 0x1307B0;      // data_array_free(array, index)
		constexpr uintptr_t kRvaUnlink   = 0x347170;      // object_sim_unlink(object)   (+204 = -1, type callbacks)
		constexpr uintptr_t kRvaCreate   = 0x1EA04;       // object_sim_create(object)   (entities for the object and its children)
		// the first 16 bytes of each (1.3528): the repair refuses to run on any other build
		constexpr uint8_t kProDelete[16] = { 0x48,0x83,0xEC,0x28,0x4C,0x8B,0x0D,0x8D,0x51,0xD1,0x02,0x44,0x8B,0xD1,0x41,0x8B };
		constexpr uint8_t kProGsFree[16] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };
		constexpr uint8_t kProUnlink[16] = { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x48 };
		constexpr uint8_t kProCreate[16] = { 0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x08,0x49,0x89,0x6B,0x18,0x49,0x89,0x73,0x20,0x57 };
		// Main-loop join right after the revert handler (sub_1800B2250: `call sub_1800B35AC` checkpoint load, then
		// `call sub_1800B3740`; both paths meet here). The tick alone is too late: game_tick (sub_1800EF120) runs player unit
		// control (sub_1800E444C -> sub_1800E8DD8 -> ... -> sub_1800522E8) BEFORE sub_18056C4B8 where HCM's tick event fires,
		// so a restored unit with a dead entity (the forge monitor after leaving forge, 2026-11-06) crashed on the first tick.
		constexpr uintptr_t kRvaRevertHook = 0xB22CE;    // mov byte [byte_1820B96B9], dil ; call sub_1800F21AC
		constexpr uint8_t kProRevertHook[12] = { 0x40,0x88,0x3D,0xE4,0x73,0x00,0x02,0xE8,0xD2,0xFE,0x03,0x00 };

		typedef __int64(__fastcall* DeleteFn)(unsigned int id);
		typedef __int64(__fastcall* GsFreeFn)(__int64 array, unsigned short index);
		typedef char* (__fastcall* UnlinkFn)(unsigned int object);
		typedef char(__fastcall* CreateFn)(unsigned int object);

		struct Bound { uintptr_t base = 0, simGlobal = 0; DeleteFn del = nullptr; GsFreeFn gsFree = nullptr; UnlinkFn unlink = nullptr; CreateFn create = nullptr; };

		bool bind(uintptr_t b, Bound& o)
		{
			if (!bytesMatch(b + kRvaDelete, kProDelete, 16) || !bytesMatch(b + kRvaGsFree, kProGsFree, 16)
				|| !bytesMatch(b + kRvaUnlink, kProUnlink, 16) || !bytesMatch(b + kRvaCreate, kProCreate, 16))
				return false;
			o.base = b;
			// the delete function's own `mov r9, [rip+disp]` (at +4, 7 bytes) names the simulation global
			o.simGlobal = ripTarget(b + kRvaDelete + 7, b + kRvaDelete + 4 + 7);
			o.del = (DeleteFn)(b + kRvaDelete); o.gsFree = (GsFreeFn)(b + kRvaGsFree);
			o.unlink = (UnlinkFn)(b + kRvaUnlink); o.create = (CreateFn)(b + kRvaCreate);
			return true;
		}

		// POD-only, SEH-guarded: reads both tables and lists what to repair (writes nothing)
		bool scan(const Bound& B, ScanResult& r)
		{
			__try
			{
				r.nOrphan = r.nStale = r.nOtherStale = 0;
				const uint32_t tlsIndex = *(const uint32_t*)(B.base + kRvaTlsIndex);
				const uintptr_t tls = ((const uintptr_t*)__readgsqword(0x58))[tlsIndex];   // TEB->ThreadLocalStoragePointer (this is the game thread)
				if (!tls) return false;
				const uintptr_t globals = *(const uintptr_t*)(tls + 72);
				if (!globals || *(const uint8_t*)(globals + 17) != 5) return false;                 // multiplayer only
				const uintptr_t sim = *(const uintptr_t*)B.simGlobal;
				if (!sim || *(const int32_t*)(sim + 16) != 7) return false;                         // networked simulation only
				const uintptr_t simData = *(const uintptr_t*)(sim + 8);
				if (!simData) return false;
				const uintptr_t db = simData + 0x2138;
				const uintptr_t gs = *(const uintptr_t*)(tls + 576);
				if (!gs) return false;
				const int32_t gsSize = *(const int32_t*)(gs + 36), gsHigh = *(const int32_t*)(gs + 60);
				const uintptr_t gsData = *(const uintptr_t*)(gs + 72);
				const uint32_t* gsBits = *(const uint32_t* const*)(gs + 80);
				if (gsSize != 16 || gsHigh < 0 || gsHigh > 65535 || !gsData || !gsBits) return false;
				r.gsArray = gs; r.gsData = gsData;
				auto gsValid = [&](uint32_t i) { return (int32_t)i < gsHigh && ((gsBits[i >> 5] >> (i & 31)) & 1u) != 0; };
				const uintptr_t objs = *(const uintptr_t*)(tls + 56);
				const uintptr_t objData = objs ? *(const uintptr_t*)(objs + 72) : 0;
				const int32_t objHigh = objs ? *(const int32_t*)(objs + 60) : 0;

				// 1. orphans
				for (uint32_t k = 0; k < 1024; ++k)
				{
					const uintptr_t slot = db + 32 + 56ull * k;
					const int32_t id = *(const int32_t*)slot;
					if (id == -1) continue;
					const int32_t g = *(const int32_t*)(slot + 12);
					if (g == -1) continue;                                   // not tied to a game-state entry: leave it alone
					const uint32_t gi = (uint32_t)g & 0xFFFF;
					const uintptr_t e = gsData + 16ull * gi;
					const bool ok = gsValid(gi) && *(const uint16_t*)e == (uint16_t)((uint32_t)g >> 16) && *(const int32_t*)(e + 4) == id;
					if (!ok && r.nOrphan < 1024) r.orphan[r.nOrphan++] = (uint32_t)id;
				}
				// 2. stale references
				for (uint32_t gi = 0; gi < (uint32_t)gsHigh; ++gi)
				{
					if (!gsValid(gi)) continue;
					const uintptr_t e = gsData + 16ull * gi;
					const int32_t id = *(const int32_t*)(e + 4);
					if (id == -1) continue;
					const uintptr_t slot = db + 32 + 56ull * ((uint32_t)id & 0x3FF);
					if (*(const int32_t*)slot == id && ((uint32_t)*(const int32_t*)(slot + 12) & 0xFFFF) == gi) continue;
					const uint32_t owner = *(const uint32_t*)(e + 8);
					const uint32_t oi = owner & 0xFFFF;
					bool isObject = false;
					if (owner != 0xFFFFFFFFu && objData && (int32_t)oi < objHigh)
					{
						const uintptr_t oh = objData + 24ull * oi;
						const uintptr_t obj = *(const uintptr_t*)(oh + 16);
						if (*(const uint16_t*)oh == (uint16_t)(owner >> 16) && obj && (((uint32_t)*(const int32_t*)(obj + 204)) & 0xFFFF) == gi)
							isObject = true;
					}
					if (isObject && r.nStale < 512) { r.staleObject[r.nStale] = owner; r.staleGs[r.nStale] = gi; ++r.nStale; }
					else ++r.nOtherStale;
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void repair(const Bound& B, const ScanResult& s)
		{
			for (int i = 0; i < s.nOrphan; ++i) B.del(s.orphan[i]);
			for (int i = 0; i < s.nStale; ++i)
			{
				*(int32_t*)(s.gsData + 16ull * s.staleGs[i] + 4) = -1;   // drop the stale id (never delete through it)
				B.gsFree((__int64)s.gsArray, (unsigned short)s.staleGs[i]);
				B.unlink(s.staleObject[i]);
				B.create(s.staleObject[i]);
			}
		}
	}

	// ---------------------------------------------------------------- Halo 2
	namespace h2
	{
		constexpr uintptr_t kRvaDelete  = 0x885EB0;   // sim_entity_delete(id)                    (slot id & 0x3FF, no salt check)
		constexpr uintptr_t kRvaGsFree  = 0x886530;   // gamestate_entity_free(datum)             (mov rcx,[gs array]; jmp data_array_free)
		constexpr uintptr_t kRvaUnlink  = 0x8D5500;   // object_sim_unlink(object)                (type callbacks, +212 = -1, +216 = 0)
		constexpr uintptr_t kRvaCreate  = 0x883210;   // object_sim_create(object)                (entities for the object and its children)
		constexpr uintptr_t kRvaSimGet  = 0x866360;   // simulation_get()                         (mov rax,[sim]; ret)
		constexpr uintptr_t kRvaSession = 0x6A63E0;   // session_is_client()                      (mov rax,[session]; cmp word [rax+2C2h],0)
		constexpr uintptr_t kRvaObjGet  = 0x8D7000;   // object_header_get_object(header)        (mov edx,[rcx+8]; mov rcx,[pool]; jmp)
		constexpr uintptr_t kRvaFrameHook = 0x67A3B0; // main loop, right after the revert handler: call sub_1806BF920
		constexpr uint8_t kProFrameHook[10] = { 0xE8,0x6B,0x55,0x04,0x00,0x84,0xC0,0x74,0x0A,0xE8 };
		// the leading bytes of each (1.3528): the repair refuses to run on any other build
		constexpr uint8_t kProDelete[16]  = { 0x40,0x56,0x48,0x83,0xEC,0x20,0x8B,0xF1,0xE8,0xA3,0x04,0xFE,0xFF,0x8B,0x50,0x10 };
		constexpr uint8_t kProGsFree[16]  = { 0x8B,0xD1,0x48,0x8B,0x0D,0x17,0x29,0x02,0x01,0xE9,0x62,0x4F,0xDF,0xFF,0xCC,0xCC };
		constexpr uint8_t kProUnlink[17]  = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x05,0x87,0x1E,0xFE,0x00 };
		constexpr uint8_t kProCreate[16]  = { 0x40,0x57,0x48,0x83,0xEC,0x50,0x8B,0xF9,0xE8,0x03,0x32,0xE2,0xFF,0x84,0xC0,0x0F };
		constexpr uint8_t kProSimGet[8]   = { 0x48,0x8B,0x05,0xD1,0xDF,0x03,0x01,0xC3 };
		constexpr uint8_t kProSession[16] = { 0x48,0x8B,0x05,0x91,0xA6,0x7D,0x00,0x66,0x83,0xB8,0xC2,0x02,0x00,0x00,0x00,0x0F };
		constexpr uint8_t kProObjGet[15]  = { 0x8B,0x51,0x08,0x48,0x8B,0x0D,0x56,0x03,0xFE,0x00,0xE9,0x11,0x43,0xE9,0xFF };

		typedef char(__fastcall* DeleteFn)(unsigned int id);
		typedef __int64(__fastcall* GsFreeFn)(unsigned int datum);
		typedef __int64(__fastcall* UnlinkFn)(unsigned int object);
		typedef __int64(__fastcall* CreateFn)(unsigned int object);

		struct Bound
		{
			uintptr_t simGlobal = 0, gsGlobal = 0, sessionGlobal = 0, objPoolGlobal = 0, objHeaderGlobal = 0;
			DeleteFn del = nullptr; GsFreeFn gsFree = nullptr; UnlinkFn unlink = nullptr; CreateFn create = nullptr;
		};

		bool bind(uintptr_t b, Bound& o)
		{
			if (!bytesMatch(b + kRvaDelete, kProDelete, sizeof kProDelete) || !bytesMatch(b + kRvaGsFree, kProGsFree, sizeof kProGsFree)
				|| !bytesMatch(b + kRvaUnlink, kProUnlink, sizeof kProUnlink) || !bytesMatch(b + kRvaCreate, kProCreate, sizeof kProCreate)
				|| !bytesMatch(b + kRvaSimGet, kProSimGet, sizeof kProSimGet) || !bytesMatch(b + kRvaSession, kProSession, sizeof kProSession)
				|| !bytesMatch(b + kRvaObjGet, kProObjGet, sizeof kProObjGet) || !bytesMatch(b + kRvaFrameHook, kProFrameHook, sizeof kProFrameHook))
				return false;
			// every global is named by the game's own rip-relative loads in the functions checked above
			o.simGlobal       = ripTarget(b + kRvaSimGet + 3,  b + kRvaSimGet + 7);        // mov rax, [sim]
			o.gsGlobal        = ripTarget(b + kRvaGsFree + 5,  b + kRvaGsFree + 9);        // mov rcx, [gamestate entity array]
			o.sessionGlobal   = ripTarget(b + kRvaSession + 3, b + kRvaSession + 7);       // mov rax, [session]
			o.objPoolGlobal   = ripTarget(b + kRvaObjGet + 6,  b + kRvaObjGet + 10);       // mov rcx, [object pool]
			o.objHeaderGlobal = ripTarget(b + kRvaUnlink + 13, b + kRvaUnlink + 17);       // mov rax, [object header array]
			o.del = (DeleteFn)(b + kRvaDelete); o.gsFree = (GsFreeFn)(b + kRvaGsFree);
			o.unlink = (UnlinkFn)(b + kRvaUnlink); o.create = (CreateFn)(b + kRvaCreate);
			return true;
		}

		// H2 data arrays keep +72 data / +80 bitmap as offsets from the array header (0 = none)
		uintptr_t arrayData(uintptr_t a) { const int64_t off = *(const int64_t*)(a + 72); return off ? a + off : 0; }

		// POD-only, SEH-guarded: reads both tables and lists what to repair (writes nothing)
		bool scan(const Bound& B, ScanResult& r)
		{
			__try
			{
				r.nOrphan = r.nStale = r.nOtherStale = 0;
				const uintptr_t session = *(const uintptr_t*)B.sessionGlobal;
				if (!session) { r.gate = 1; return false; }
				if (*(const uint8_t*)(session + 12) != 5) { r.gate = 100 + *(const uint8_t*)(session + 12); return false; }   // not a networked game
				if (*(const uint16_t*)(session + 706) != 0) { r.gate = 2; return false; }                                    // a client: the host owns the entities
				const uintptr_t sim = *(const uintptr_t*)B.simGlobal;
				if (!sim) { r.gate = 3; return false; }
				if (*(const int32_t*)(sim + 16) != 5) { r.gate = 200 + *(const int32_t*)(sim + 16); return false; }
				const uintptr_t simData = *(const uintptr_t*)(sim + 8);
				if (!simData) { r.gate = 4; return false; }
				const uintptr_t db = simData + 0x1138;
				const uintptr_t mgr = *(const uintptr_t*)(db + 24);           // slot flags at +140 + 4*index
				if (!mgr) { r.gate = 5; return false; }
				const uintptr_t gs = *(const uintptr_t*)B.gsGlobal;
				if (!gs) { r.gate = 6; return false; }
				const int32_t gsSize = *(const int32_t*)(gs + 36), gsHigh = *(const int32_t*)(gs + 60);
				const uintptr_t gsData = arrayData(gs);
				if (gsSize != 16 || gsHigh < 0 || gsHigh > 65535 || !gsData) { r.gate = 7; return false; }
				r.gate = 0;
				r.gsArray = gs; r.gsData = gsData;
				// an entry is live when its salt is non-zero (data_array_free zeroes it) and matches the datum (as datum_get checks)
				auto gsLive = [&](uint32_t datum) {
					const uint32_t i = datum & 0xFFFF;
					if ((int32_t)i >= gsHigh) return false;
					const uint16_t salt = *(const uint16_t*)(gsData + 16ull * i);
					return salt != 0 && salt == (uint16_t)(datum >> 16);
				};
				const uintptr_t oh = *(const uintptr_t*)B.objHeaderGlobal;
				const uintptr_t ohData = oh ? arrayData(oh) : 0;
				const int32_t ohHigh = oh ? *(const int32_t*)(oh + 60) : 0;
				const bool ohOk = ohData && *(const int32_t*)(oh + 36) == 12;
				const uintptr_t pool = *(const uintptr_t*)B.objPoolGlobal;
				const uintptr_t poolBase = (pool + 87) & ~(uintptr_t)15;
				auto flags = [&](uint32_t k) { return *(const uint8_t*)(mgr + 140 + 4ull * k); };

				// 1. orphans: an alive slot whose game-state entry is gone or holds another entity
				for (uint32_t k = 0; k < 1024; ++k)
				{
					if ((flags(k) & 5) != 5) continue;                       // not in use, or already being deleted
					const uintptr_t slot = db + 40 + 56ull * k;
					const int32_t id = *(const int32_t*)slot;
					if (((uint32_t)id & 0x3FF) != k) continue;
					const int32_t g = *(const int32_t*)(slot + 12);
					if (g == -1) continue;                                   // not tied to a game-state entry: leave it alone
					const bool ok = gsLive((uint32_t)g) && *(const int32_t*)(gsData + 16ull * ((uint32_t)g & 0xFFFF) + 4) == id;
					if (!ok && r.nOrphan < 1024) r.orphan[r.nOrphan++] = (uint32_t)id;
				}
				// 2. stale references: a live entry whose entity's slot no longer holds it
				for (uint32_t gi = 0; gi < (uint32_t)gsHigh; ++gi)
				{
					const uintptr_t e = gsData + 16ull * gi;
					const uint16_t salt = *(const uint16_t*)e;
					if (!salt) continue;
					const int32_t id = *(const int32_t*)(e + 4);
					if (id == -1) continue;
					const uint32_t k = (uint32_t)id & 0x3FF;
					const uintptr_t slot = db + 40 + 56ull * k;
					if ((flags(k) & 1) && *(const int32_t*)slot == id && ((uint32_t)*(const int32_t*)(slot + 12) & 0xFFFF) == gi) continue;
					const uint32_t datum = ((uint32_t)salt << 16) | gi;
					const uint32_t owner = *(const uint32_t*)(e + 8);
					const uint32_t oi = owner & 0xFFFF;
					bool isObject = false;
					if (owner != 0xFFFFFFFFu && ohOk && (int32_t)oi < ohHigh)
					{
						const uintptr_t h = ohData + 12ull * oi;
						const int32_t off = *(const int32_t*)(h + 8);
						if (*(const uint16_t*)h == (uint16_t)(owner >> 16) && off != -1 && *(const uint32_t*)(poolBase + (intptr_t)off + 212) == datum)
							isObject = true;
					}
					if (isObject && r.nStale < 512) { r.staleObject[r.nStale] = owner; r.staleGs[r.nStale] = datum; ++r.nStale; }
					else ++r.nOtherStale;
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { r.gate = 9; return false; }
		}

		void repair(const Bound& B, const ScanResult& s)
		{
			for (int i = 0; i < s.nOrphan; ++i) B.del(s.orphan[i]);
			for (int i = 0; i < s.nStale; ++i)
			{
				*(int32_t*)(s.gsData + 16ull * (s.staleGs[i] & 0xFFFF) + 4) = -1;   // drop the stale id (never delete through it)
				B.gsFree(s.staleGs[i]);
				B.unlink(s.staleObject[i]);
				B.create(s.staleObject[i]);
			}
		}
	}
}

class MPRevertRepair::MPRevertRepairImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<GameTickEventHook> gameTickEventHookWeak;

	ScopedCallback<ToggleEvent> toggleCallback;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(uint32_t)>>> tickCallback;

	HMODULE mModule = nullptr; bool mResolved = false; bool mWrongBuildReported = false;
	h3::Bound mH3; h2::Bound mH2;
	ScanResult mScan;
	int mOtherReported = 0;
	int mLastGate = -1;

	// Both games: also runs from the main loop right after the revert handler (see WHEN above / h3::kRvaRevertHook) -
	// the sim tick alone is too late in Halo 2 (network_send) and in Halo 3 (player unit control early in game_tick).
	std::unique_ptr<ModuleMidHook> mFrameHook;
	uintptr_t mFrameHookRva = 0; const uint8_t* mFrameHookBytes = nullptr; size_t mFrameHookLen = 0;
	static inline MPRevertRepairImpl* sH2Instance = nullptr;
	static inline MPRevertRepairImpl* sH3Instance = nullptr;
	static inline std::atomic_bool sFrameRunning = false;
	static void runFrame(MPRevertRepairImpl* self)
	{
		if (GlobalKill::isKillSet() || !self) return;
		ScopedAtomicBool lock(sFrameRunning);
		try { self->onTick(0); }
		catch (HCMRuntimeException ex) { PLOG_ERROR << "MP revert repair (revert hook): " << ex.what(); }
		catch (const std::exception& ex) { PLOG_ERROR << "MP revert repair (revert hook): " << ex.what(); }
	}
	static void h2FrameHookFunction(SafetyHookContext& ctx) { runFrame(sH2Instance); }
	static void h3FrameHookFunction(SafetyHookContext& ctx) { runFrame(sH3Instance); }

	bool isHalo2() const { return mGame.operator GameState::Value() == GameState::Value::Halo2; }

	bool resolve()
	{
		const HMODULE mod = GetModuleHandleW(isHalo2() ? L"halo2.dll" : L"halo3.dll");
		if (mod == mModule && mResolved) return true;
		if (mod != mModule) mWrongBuildReported = false;
		mModule = mod; mResolved = false;
		if (!mod) return false;
		const bool ok = isHalo2() ? h2::bind((uintptr_t)mod, mH2) : h3::bind((uintptr_t)mod, mH3);
		if (!ok)
		{
			if (!mWrongBuildReported)
				PLOG_ERROR << "MP revert repair: " << (isHalo2() ? "halo2.dll" : "halo3.dll") << " is not the build this was made for (1.3528) - not running";
			mWrongBuildReported = true;
			return false;
		}
		mResolved = true;
		return true;
	}

	// game thread (the tick hook)
	void onTick(uint32_t)
	{
		if (!resolve()) return;
		const bool ran = isHalo2() ? h2::scan(mH2, mScan) : h3::scan(mH3, mScan);
		if (mScan.gate != mLastGate)
		{
			// gate 0 = the networked host, repairing; 1xx = session mode (5 needed), 2 = a client, 2xx = simulation mode (5 needed)
			PLOG_INFO << "MP revert repair (" << mGame.toString() << "): " << (mScan.gate == 0 ? "active (networked host)" : "idle, gate " + std::to_string(mScan.gate));
			mLastGate = mScan.gate;
		}
		if (!ran) return;
		if (mScan.nOrphan == 0 && mScan.nStale == 0)
		{
			if (mScan.nOtherStale > 0 && mOtherReported++ == 0)
				PLOG_WARNING << "MP revert repair: " << mScan.nOtherStale << " non-object network entities are stale (not repaired)";
			return;
		}
		if (isHalo2()) h2::repair(mH2, mScan); else h3::repair(mH3, mScan);
		PLOG_INFO << "MP revert repair (" << mGame.toString() << "): removed " << mScan.nOrphan << " orphan network entities, relinked " << mScan.nStale << " objects"
			<< (mScan.nOtherStale ? " (" + std::to_string(mScan.nOtherStale) + " other stale entities left)" : std::string());
		// log only (user, 2026-11-05): every revert repairs something, an on-screen message each time flooded the screen
	}

	void onToggle(bool& on)
	{
		try
		{
			if (mFrameHook)
			{
				bool attach = on;
				if (on)
				{
					const HMODULE mod = GetModuleHandleW(isHalo2() ? L"halo2.dll" : L"halo3.dll");
					if (mod && !bytesMatch((uintptr_t)mod + mFrameHookRva, mFrameHookBytes, mFrameHookLen))
					{
						PLOG_ERROR << "MP revert repair: " << mGame.toString() << " is not the build this was made for (1.3528) - revert hook not attached";
						attach = false;
					}
				}
				mFrameHook->setWantsToBeAttached(attach);
				PLOG_INFO << "MP revert repair (" << mGame.toString() << "): revert hook " << (attach ? "attached" : "detached");
			}
			if (isHalo2()) return;                 // Halo 2 runs from the revert hook only
			lockOrThrow(gameTickEventHookWeak, gameTickEventHook);
			if (on) tickCallback = gameTickEventHook->getGameTickEvent()->subscribe([this](uint32_t t) { onTick(t); });
			else tickCallback.reset();
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("MP revert repair: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	MPRevertRepairImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		toggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->mpRevertRepairToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		gameTickEventHookWeak(resolveDependentCheat(GameTickEventHook))
	{
		const auto g = game.operator GameState::Value();
		if (g != GameState::Value::Halo3 && g != GameState::Value::Halo2) throw HCMInitException("MP revert repair is Halo 2 / Halo 3 only (so far)");
		{
			const bool h2 = g == GameState::Value::Halo2;
			const std::wstring modName = h2 ? L"halo2.dll" : L"halo3.dll";
			if (h2) { sH2Instance = this; mFrameHookRva = h2::kRvaFrameHook; mFrameHookBytes = h2::kProFrameHook; mFrameHookLen = sizeof h2::kProFrameHook; }
			else    { sH3Instance = this; mFrameHookRva = h3::kRvaRevertHook; mFrameHookBytes = h3::kProRevertHook; mFrameHookLen = sizeof h3::kProRevertHook; }
			auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(modName, std::vector<int64_t>{ (int64_t)mFrameHookRva });
			mFrameHook = ModuleMidHook::make(modName, ptr, h2 ? h2FrameHookFunction : h3FrameHookFunction, false);
		}
		bool on = dicon.Resolve<SettingsStateAndEvents>().lock()->mpRevertRepairToggle->GetValue();
		if (on) onToggle(on);
	}
	~MPRevertRepairImpl()
	{
		tickCallback.reset();
		if (mFrameHook) { mFrameHook->setWantsToBeAttached(false); }
		if (sH2Instance == this) sH2Instance = nullptr;
		if (sH3Instance == this) sH3Instance = nullptr;
		while (sFrameRunning) Sleep(1);
		mFrameHook.reset();
	}
};

MPRevertRepair::MPRevertRepair(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<MPRevertRepairImpl>(gameImpl, dicon))
{
}
MPRevertRepair::~MPRevertRepair() = default;
