// HCEHavokDebuggerImpl.cpp - Halo Campaign Evolved Havok Visual Debugger, ported into HCMInternal.
//
// SOURCE OF TRUTH: repos\HceHavokDebugger\src\vdb\vdb.cpp (+ src\vdb\sigscan.h). This file is that
// payload with the standalone-injector scaffolding removed and an HCM bridge bolted on. If you fix a
// decode bug here, fix it there too - or better, re-run the port script and re-apply the deltas.
//
// WHAT CHANGED FROM THE STANDALONE, AND WHY:
//   * No DllMain. HCM owns the DLL entry; the VDB thread is spun from HceHavokDebuggerBridge::start().
//   * No AddVectoredExceptionHandler / SetUnhandledExceptionFilter / _set_invalid_parameter_handler.
//     All three are PROCESS-GLOBAL and would fight HCM's own crash handling. They were pure diagnostics.
//   * No psapi. SizeOfImage is read straight out of the PE header (initSections already parses it),
//     which avoids adding Psapi.lib to the link.
//   * Live Trigger Volumes and the input-latch probe are dropped. They are the only parts that wrote
//     executable memory and patched game call sites - see the stub block further down.
//   * g_paused, honoured by the tick detour, the VDB loop and every viewer's step().
//   * uninstallTickHook(), so stop() genuinely takes the hook off (the standalone never did).
//   * The four scenario/mesh scratch buffers are RESERVED and committed on demand instead of being
//     committed up front (91 MB of commit charge on first enable that a small level never touched).
//
// ODR WARNING: HavokDebuggerImpl.cpp (the MCC/H3/H2/ODST/Reach debugger) declares its OWN class named
// CatProcess at namespace scope, with a different layout, and is compiled into the same DLL under the
// same HCM_HAVOK_AVAILABLE gate. Everything below therefore lives in an ANONYMOUS NAMESPACE. Do not
// remove it: without it MSVC silently COMDAT-folds one vtable/typeinfo over the other.
//
// Compiled C++14 against the Havok 2007 SDK, /EHa (the __try blocks below sit in functions that hold
// C++ objects), warnings off, no PCH. See the ClCompile entry in HCMInternal.vcxproj.

#ifdef HCM_HAVOK_AVAILABLE

#include "ImageResidencyShim.h"   // tickDetour is an HCM entry point the unload drain must wait for
#include <Common/Base/hkBase.h>
#include <Common/Base/Memory/Memory/Pool/hkPoolMemory.h>
#include <Common/Base/Memory/hkThreadMemory.h>
#include <Common/Base/System/hkBaseSystem.h>
#include <Common/Visualize/hkVisualDebugger.h>
#include <Common/Visualize/hkProcess.h>
#include <Common/Visualize/hkProcessFactory.h>
#include <Common/Visualize/hkProcessContext.h>
#include <Common/Visualize/hkDebugDisplayHandler.h>
#include <Common/Visualize/Shape/hkDisplayConvex.h>
#include <Common/Base/Types/Geometry/hkGeometry.h>
#include <Common/Visualize/Type/hkColor.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#include <cstdio>
#include <stdarg.h>
#include <stdlib.h>
#include <cstdint>
#include <cmath>
#include <cstddef>
#include <intrin.h>

#include <cstring>

namespace {

// ================= sigscan.h (inlined; see repos\HceHavokDebugger\src\vdb\sigscan.h) =================
// sigscan.h - byte-pattern resolution of engine anchors.
//
// Every address this tool depends on used to be a hardcoded VA from an IDA session. That works
// until the game updates, at which point each stale VA silently points at whatever now occupies
// that address - which is far worse than not resolving at all, because the tool keeps running and
// draws nonsense (or writes through a function pointer that is no longer a function).
//
// So each anchor is located by a byte SIGNATURE instead. At startup we scan the module's executable
// sections; only a unique hit is accepted, and there is deliberately no static-VA fallback. Verified
// across two different game builds: the same 24 patterns each match exactly once in both and resolve
// to that build's own addresses, so one binary supports both.


namespace sig {

// ---- pattern parsing -------------------------------------------------------------------------
// "48 8B 05 ?? ?? ?? ?? 48 85 C0" - hex bytes, ?? for a wildcard. Whitespace is ignored.
static const int MAX_SIG = 128;

struct Pattern { unsigned char b[MAX_SIG]; bool wild[MAX_SIG]; int n; };

static inline int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool parse(const char* p, Pattern& out)
{
    out.n = 0;
    while (*p && out.n < MAX_SIG) {
        if (*p == ' ' || *p == '\t') { ++p; continue; }
        if (*p == '?') {
            out.b[out.n] = 0; out.wild[out.n] = true; ++out.n;
            ++p; if (*p == '?') ++p;
            continue;
        }
        const int hi = hexVal(p[0]); if (hi < 0) return false;
        const int lo = hexVal(p[1]); if (lo < 0) return false;
        out.b[out.n] = (unsigned char)((hi << 4) | lo); out.wild[out.n] = false; ++out.n;
        p += 2;
    }
    return out.n > 0;
}

// ---- executable section enumeration -----------------------------------------------------------
struct Region { uintptr_t begin, end; };

static inline int execRegions(uintptr_t base, Region* out, int max)
{
    int n = 0;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && n < max; ++i) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            out[n].begin = base + sec[i].VirtualAddress;
            out[n].end   = out[n].begin + sec[i].Misc.VirtualSize;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

// ---- scanning ----------------------------------------------------------------------------------
// Returns the number of matches found, writing up to 'maxHits' addresses. Callers must treat any
// count other than 1 as a failure: 0 means the update moved or changed the code, >1 means the
// signature is not specific enough to trust.
static inline int scan(uintptr_t base, const char* patStr, uintptr_t* hits, int maxHits)
{
    Pattern pat;
    if (!parse(patStr, pat)) return -1;

    Region regs[16];
    const int nr = execRegions(base, regs, 16);
    if (nr <= 0) return -1;

    int found = 0;
    __try {
        for (int r = 0; r < nr; ++r) {
            if (regs[r].end <= regs[r].begin + (uintptr_t)pat.n) continue;
            const unsigned char* const b = (const unsigned char*)regs[r].begin;
            const size_t span = (size_t)(regs[r].end - regs[r].begin) - (size_t)pat.n;
            const unsigned char first = pat.b[0];
            const bool firstWild = pat.wild[0];
            for (size_t i = 0; i <= span; ++i) {
                if (!firstWild && b[i] != first) continue;
                int k = 1;
                for (; k < pat.n; ++k)
                    if (!pat.wild[k] && b[i + k] != pat.b[k]) break;
                if (k != pat.n) continue;
                if (found < maxHits) hits[found] = (uintptr_t)(b + i);
                ++found;
                if (found > maxHits) return found;      // caller only needs to know it is >1
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return found;
}

// Resolve a rip-relative operand. 'insn' points at the first byte of the instruction, 'dispOff' is
// the byte offset of the disp32 within it, and 'insnLen' its total length (RIP is the address of
// the NEXT instruction).
static inline uintptr_t ripTarget(uintptr_t insn, int dispOff, int insnLen)
{
    __try {
        const int32_t disp = *(const int32_t*)(insn + dispOff);
        return insn + insnLen + (intptr_t)disp;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Resolve a rel32 call/jmp target. 'insn' points at the opcode, 'relOff' at the rel32.
static inline uintptr_t relTarget(uintptr_t insn, int relOff, int insnLen)
{
    __try {
        const int32_t rel = *(const int32_t*)(insn + relOff);
        return insn + insnLen + (intptr_t)rel;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- anchor table --------------------------------------------------------------------------
enum AnchorKind {
    ANCHOR_CODE,      // the match itself is the thing we want (a function head)
    ANCHOR_RIP,       // the match contains a rip-relative reference to the thing we want
    ANCHOR_CALL,      // the match contains a rel32 call to the thing we want
    ANCHOR_RVA,       // the match embeds a plain RVA (base-relative addressing, e.g. [r13+idx*4+rva])
};

// Deliberately NO static-VA field. Carrying a last-known address invites falling back to it, and a
// stale address is exactly the failure this mechanism exists to prevent - on one observed update the
// old shell-vtable VA landed inside a string table, where patching would have corrupted unrelated
// data and never fired. Unresolved must mean "disable the feature", not "guess".
struct Anchor {
    const char* name;
    const char* pattern;
    AnchorKind  kind;
    int         dispOff;   // for RIP/CALL: offset of the disp32/rel32 within the match
    int         insnLen;   // for RIP/CALL: length of that instruction
    int         adjust;    // for CODE: bytes to subtract from the match to reach the function head

    uintptr_t   resolved;  // runtime result, 0 if unresolved
    int         hits;      // match count, for diagnostics
};

// Resolve one anchor. True only when the signature produced EXACTLY one hit; 0 matches means the
// build changed that code, >1 means the pattern is not specific enough to trust. Both leave
// resolved at 0 so the caller disables the dependent feature.
static inline bool resolve(uintptr_t base, Anchor& a)
{
    a.resolved = 0; a.hits = 0;
    if (!a.pattern) return false;
    uintptr_t hits[4];
    a.hits = scan(base, a.pattern, hits, 4);
    if (a.hits != 1) return false;
    switch (a.kind) {
    case ANCHOR_CODE: a.resolved = hits[0] - (uintptr_t)a.adjust;                  break;
    case ANCHOR_RIP:  a.resolved = ripTarget(hits[0], a.dispOff, a.insnLen);       break;
    case ANCHOR_CALL: a.resolved = relTarget(hits[0], a.dispOff, a.insnLen);       break;
    case ANCHOR_RVA:  { uint32_t rva = 0;
                        __try { rva = *(const uint32_t*)(hits[0] + a.dispOff); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { rva = 0; }
                        a.resolved = rva ? base + rva : 0; }                       break;
    }
    return a.resolved != 0;
}

} // namespace sig


typedef hkDebugDisplayHandler HDH;

// ===================== target constants (see README) =====================
static const uintptr_t IMGBASE = 0x180000000ull;
static const char* MODNAME = "HaloSimulation_tag_release.dll";

enum { DA_ELEMSIZE = 0x20, DA_MAXCOUNT = 0x2C, DA_DATA = 0x50 };
enum { COMP_STRIDE = 0xC0, COMP_RBARR = 0x30, COMP_RBCOUNT = 0x38 };
enum { RBREC_STRIDE = 0x50, RBREC_BODY = 0x40 };
enum { WO_WORLD = 0x10, OFF_SHAPE = 0x20, OFF_TRANSFORM = 0x170, OFF_COM = 0x1C0 };
enum { SHAPE_TYPE = 0x18 };
enum { W_FIXEDISLAND = 0x30, W_ACTIVE_ISLANDS = 0x40, W_INACTIVE_ISLANDS = 0x50, W_BROADPHASE = 0x88 };
enum { ISLAND_ENTITIES = 0x60 };   // solved by hce_probe (matched in all 231 islands)

// hkpShapeType values, confirmed empirically by the probe census
enum {
    ST_SPHERE = 1, ST_BOX = 4, ST_CAPSULE = 5, ST_CONVEX_VERTICES = 6,
    ST_LIST = 9, ST_BVTREE = 10, ST_CONVEX_TRANSLATE = 11, ST_CONVEX_TRANSFORM = 12,
};

// hkpConvexShape / hkpConvexVerticesShape / hkpConvexTransformShapeBase
enum {
    CVX_RADIUS   = 0x20,
    CVX_AABB_HE  = 0x30,   // hkpConvexVerticesShape::m_aabbHalfExtents
    CVX_AABB_C   = 0x40,   // ::m_aabbCenter
    CVX_VDATA    = 0x50,   // ::m_rotatedVertices (hkFourTransposedPoints, 48B blocks)
    CVX_NUMVERTS = 0x60,
    CVX_PDATA    = 0x70,   // ::m_planeEquations data   (Reach-matching; validated at runtime)
    CVX_NUMPLANES= 0x78,
    // hkpConvexTransformShapeBase::m_childShape is a hkpSingleShapeContainer {vtable@0, child@8}
    // located at +0x28 -> the actual child pointer is at +0x30. NOT +0x28.
    XF_CHILD     = 0x30,
    XF_TRANSFORM = 0x40,   // translate: translation vector; transform: 3 rotation cols + translation
    // capsule
    CAP_VERTA    = 0x30,
    CAP_VERTB    = 0x40,
    // list / collection
    LIST_ARR     = 0x30,   // hkArray {data@0x30, size@0x38}
    // hkpMoppBvTreeShape (m_type 10, bvTreeType 0 = BVTREE_MOPP): carries an
    // hkpSingleShapeContainer at +0x50, so the wrapped collection is at +0x58.
    // Confirmed by probe: child is a m_type 9 LIST (collectionType 0) with 48 children.
    // +0x28/+0x30 are the raw MOPP code buffers, NOT shapes - do not follow them.
    BV_CHILD     = 0x58,
    // m_type 28: container vtable at +0x28 like the transform shapes -> single child at +0x30.
    WRAP_CHILD   = 0x30,
};

#define DEBUG_TYPE_COLORS 0   // 1 = tint object geometry by shape type (debugging aid)

// Are instanced (type-15) child triangles returned in LOCAL space (transform must be applied) or
// already in WORLD space (applying it again double-transforms and scatters them)? Compare the
// first instanced triangle's raw coords against its instance translation.
static bool  g_inInstance = false;
static float g_instT[3] = { 0, 0, 0 };
static bool  g_dumpedInstTri = false;


// type-31 layout probe: +0x34 looks like a vertex count (3 = tri, 4 = quad?). If quads exist and we
// always read 3 verts, half of every quad is missing -> holes scattered through a complete mesh.
static int g_t31Field34[16] = { 0 };
static int g_t31Field30[16] = { 0 };
static int g_t31Degenerate = 0;
static int g_t31Snap34[16] = { 0 }, g_t31SnapDegen = 0;
// Which key "sections" the walk actually visited. getFirstKey builds keys as (section << 26);
// if getNextKey only chains within one section we would silently miss whole parts of the BSP.
static int g_secSeen[64] = { 0 };
static int g_secSnap[64] = { 0 };
static int g_secProbe[64] = { 0 };
static bool g_secProbedThisGather = false;

static const int MAX_TRIS = 400000;
// 8 was arbitrary. A refusal here is a whole discarded subtree, and it was INVISIBLE: g_wmChildren
// is incremented before emitShape is called, so keys==children never proved children made geometry.
static const int MAX_DEPTH = 32;

// ===================== module state =====================
static uintptr_t g_base = 0, g_end = 0;
static FILE* g_log = nullptr;
static volatile bool g_ready = false;
static volatile bool g_paused = true;   // HCM bridge: set by stop(), cleared by start()
static bool g_wantFirstWorld = true;    // ask the engine thread for one world walk after each start()

static inline uintptr_t RV(uintptr_t va) { return g_base + (va - IMGBASE); }
static inline bool inMod(uintptr_t p) { return p >= g_base && p < g_end; }

static void LOG(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA("[hce_vdb] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
    if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
}

// ===================== guarded reads =====================
static bool rdRaw(const void* src, void* dst, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> static bool rd(uintptr_t a, T& o)
{
    if (a < 0x10000ull || a >= 0x7FFFFFFFFFFFull) return false;
    return rdRaw((const void*)a, &o, sizeof(T));
}
static uint64_t rdQ(uintptr_t a) { uint64_t v = 0; return rd(a, v) ? v : 0; }
static uint32_t rdD(uintptr_t a) { uint32_t v = 0; return rd(a, v) ? v : 0; }
static float    rdF(uintptr_t a) { float v = 0;    return rd(a, v) ? v : 0.f; }
static inline bool okPtr(uint64_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull && (p & 7) == 0; }

// ---- bounded string building -------------------------------------------------------------------
// THIS IS A PROCESS-LIFETIME ISSUE, not a cosmetic one. sprintf_s does NOT truncate: when the result
// does not fit it sets ERANGE, calls the invalid-parameter handler and, with no handler installed in
// a release CRT, FAST-FAILS the process - surfacing as STATUS_STACK_BUFFER_OVERRUN (0xC0000409),
// which no SEH/VEH guard can intercept because __fastfail bypasses both.
// The old idiom `appendf(b, sizeof(b), o, ...)` had two independent faults:
//   1. `sizeof(b) - o` is size_t arithmetic, so the moment o reaches the capacity it wraps to ~2^64
//      and the call writes without bound;
//   2. before that, an exactly-too-long append kills the process outright.
// It killed the game on the hex-dump lines because %12.4f is a MINIMUM width: one garbage float out
// of uninitialised memory prints as 35 characters ("361998934036675138130255282176.0000"), so a
// 16-byte row needs ~205 bytes, not the 160 that was allocated.
// _vsnprintf_s with _TRUNCATE truncates instead of failing, and the offset is clamped so it can
// never run past the end or go negative.
static void appendf(char* buf, size_t cap, int& off, const char* fmt, ...)
{
    if (!buf || cap == 0) return;
    if (off < 0) off = 0;
    if ((size_t)off + 1 >= cap) { buf[cap - 1] = 0; return; }   // already full: keep it terminated
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(buf + off, cap - (size_t)off, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) off = (int)cap - 1;              // truncated: the buffer is now exactly full
    else       off += n;
    if ((size_t)off >= cap) off = (int)cap - 1;
}

static bool isHkObj(uintptr_t p) { if (!okPtr(p)) return false; uint64_t v = 0; return rd(p, v) && inMod((uintptr_t)v); }

struct SecRange { uintptr_t lo, hi; };
static SecRange g_roSec[16]; static int g_roSecN = 0;   // read-only, non-exec: vtables live here
static SecRange g_txSec[16]; static int g_txSecN = 0;   // executable: vtable slot 0 points here
static int g_rejVt = 0, g_rejSlot0 = 0;


// ---- crash localisation -----------------------------------------------------------------------
// A breadcrumb, set around each of our phases, so a fault report says which one was running rather
// than leaving us to bisect by rebuilding. Deliberately a plain store: it must never itself fault.
static const char* volatile g_phase = "init";
static volatile long g_phaseShape = 0;      // m_type currently being emitted
static volatile long g_faults = 0;


static void initSections()
{
    g_roSecN = g_txSecN = 0;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const uintptr_t lo = g_base + sec[i].VirtualAddress;
            const DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            const uintptr_t hi = lo + vs;
            const DWORD c = sec[i].Characteristics;
            char nm[9] = { 0 }; memcpy(nm, sec[i].Name, 8);
            if (c & IMAGE_SCN_MEM_EXECUTE) {
                if (g_txSecN < 16) { g_txSec[g_txSecN].lo = lo; g_txSec[g_txSecN].hi = hi; ++g_txSecN; }
            } else if (!(c & IMAGE_SCN_MEM_WRITE)) {
                if (g_roSecN < 16) { g_roSec[g_roSecN].lo = lo; g_roSec[g_roSecN].hi = hi; ++g_roSecN; }
            }
            LOG("  sec %-8s %016llX..%016llX chars=%08lX", nm,
                (unsigned long long)lo, (unsigned long long)hi, (unsigned long)c);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_roSecN = g_txSecN = 0; }
}
static inline bool inRO(uintptr_t p)
{ for (int i = 0; i < g_roSecN; ++i) if (p >= g_roSec[i].lo && p < g_roSec[i].hi) return true; return false; }
static inline bool inTX(uintptr_t p)
{ for (int i = 0; i < g_txSecN; ++i) if (p >= g_txSec[i].lo && p < g_txSec[i].hi) return true; return false; }

// A polymorphic Havok object: vtable in read-only data, and slot 0 pointing into code. Live proof
// of the split - the type-18 wrapper's vtable (+0x88E150) and a type-31 surface's (+0x88E4E0) are
// both .rdata, whereas the phantom "type 0" object's first qword pointed into .data.
// Fails OPEN if the header walk failed, so behaviour is unchanged rather than blank.
static bool isShapeObj(uintptr_t p)
{
    if (!isHkObj(p)) return false;
    if (!g_roSecN) return true;
    const uintptr_t vt = (uintptr_t)rdQ(p);
    if (!inRO(vt)) { ++g_rejVt; return false; }
    if (g_txSecN && !inTX((uintptr_t)rdQ(vt))) { ++g_rejSlot0; return false; }
    return true;
}


// ===================================================================================================
// MULTI-BUILD ANCHOR RESOLUTION
//
// Every address below used to be a hardcoded VA from one IDA session, which meant a game update
// silently repointed us at whatever now lived there. That is strictly worse than not resolving: on
// the 2026-07-29 update the old shell-vtable VA landed inside a STRING TABLE, so patching it would
// have corrupted unrelated data and never produced a callback.
//
// Instead each anchor carries a byte signature. Verified against two different builds of
// HaloSimulation_tag_release.dll: all 24 patterns match EXACTLY ONCE in both, and resolve to that
// build's own addresses - so one binary supports both, and most likely future builds too.
//
//                              build A (older)     build B (newer)
//   tag address table          0x182C2DCC0         0x182C2CCC0
//   tag_get_data               0x180201460         0x180201470
//   string_id -> text          0x180204B40         0x180204B50
//   havok components array     0x1810CDC78         0x1810CCC90
//   shell vtable2 (tick)       0x1807B1610         0x1807B0610
//   tick target                0x18000E670         0x18000E670   (unchanged - do not diff to detect)
//   scenario tag data ptr      0x1810C4550         0x1810C3558
//   bsp_from_index             0x180198560         0x180198570
//   trigger_volume_test_point  0x1802E45E0         0x1802E45F0
//
// A pattern that matches 0 or >1 times NEVER falls back to a static VA - the dependent feature is
// disabled and logged instead. A stale address is the failure mode this whole mechanism exists to
// prevent, so silently using one would defeat the purpose.
// ===================================================================================================
enum AnchorId {
    A_TAG_TABLE = 0, A_TAG_GET_DATA, A_SID_TEXT, A_COMP_ARRAY,
    A_VT2, A_TICK_TARGET, A_SCEN_DATA, A_BSP_FROM_IDX,
    A_COUNT
};

static sig::Anchor g_anchors[A_COUNT] = {
    // name                pattern                                                       kind, disp, len, adj
    { "tag_address_table",
      "48 8B D9 4C 8D 05 ?? ?? ?? ?? 8B 09 8B 53 08 8B C2 48 C1 E8 1C 49 8B 04 C0 3B 4C 90 10 7D",
      sig::ANCHOR_RIP, 6, 10, 0, 0, 0 },
    { "tag_get_data",
      "48 89 5C 24 08 57 48 83 EC 20 0F B7 C2 8B DA 48 8D 3C 40 48 8B 05",
      sig::ANCHOR_CODE, 0, 0, 0, 0, 0 },
    { "string_id_get_string",
      "40 53 55 56 57 48 83 EC 28 48 8B 3D ?? ?? ?? ?? 48 63 E9",
      sig::ANCHOR_CODE, 0, 0, 0, 0, 0 },
    { "havok_components_array",
      "0F B7 C0 48 8D 14 40 48 C1 E2 06 48 8B 05 ?? ?? ?? ?? 48 8B 48 50",
      sig::ANCHOR_RIP, 14, 18, 0, 0, 0 },
    { "shell_vtable2",
      "48 8D 05 ?? ?? ?? ?? 4C 89 A4 24 60 01 00 00 4C 89 AC 24 68 01 00 00",
      sig::ANCHOR_RIP, 3, 7, 0, 0, 0 },
    { "tick_target",
      "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 80 FD FF FF",
      sig::ANCHOR_CODE, 0, 0, 0, 0, 0 },
    { "scenario_data_ptr",
      "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00",
      sig::ANCHOR_RIP, 3, 7, 0, 0, 0 },
    { "bsp_from_index",
      "48 89 5C 24 08 57 48 83 EC 20 48 63 F9 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 8B D7 48 8B CB",
      sig::ANCHOR_CODE, 0, 0, 0, 0, 0 },
};

static inline uintptr_t A(AnchorId id) { return g_anchors[id].resolved; }
static bool g_anchorsOk = false;

// Resolve everything once, at load. Logs each result so a future build that shifts something shows
// up as a named unresolved anchor rather than as mysterious wrong behaviour.
static bool resolveAnchors()
{
    int ok = 0, bad = 0;
    LOG("resolving anchors by signature (build-independent):");
    for (int i = 0; i < A_COUNT; ++i) {
        sig::Anchor& a = g_anchors[i];
        sig::resolve(g_base, a);
        if (a.resolved) {
            LOG("   %-26s %016llX  (static +%llX)  hits=%d", a.name,
                (unsigned long long)a.resolved,
                (unsigned long long)(a.resolved - g_base), a.hits);
            ++ok;
        } else {
            LOG("   %-26s !! UNRESOLVED (hits=%d) - dependent features DISABLED", a.name, a.hits);
            ++bad;
        }
    }
    // The tick target is a cross-check, not a consumer: vtable2 slot 0 should already BE it.
    if (A(A_VT2) && A(A_TICK_TARGET)) {
        const uint64_t slot0 = rdQ(A(A_VT2));
        LOG("   vtable2 slot 0 = %016llX %s tick_target %016llX",
            (unsigned long long)slot0,
            slot0 == (uint64_t)A(A_TICK_TARGET) ? "==" : "!= (MISMATCH)",
            (unsigned long long)A(A_TICK_TARGET));
    }
    LOG("anchors: %d resolved, %d unresolved", ok, bad);
    g_anchorsOk = (bad == 0);
    return g_anchorsOk;
}

// ===================== math =====================
struct Xform { float r[9]; float o[3]; };
static const Xform IDENT = { {1,0,0,0,1,0,0,0,1}, {0,0,0} };

// ---- INSTANCING -----------------------------------------------------------------------------
// 6,878 type-15 placements each re-expand a shared mesh -> 3.67M redundant triangles. Instead:
// walk each UNIQUE container once at identity into a mesh buffer, then record a transform per
// placement and let the client instance them (addGeometryInstance).
struct WmMesh { uintptr_t cont; long triStart; long triCount; };
struct WmInst { int mesh; Xform X; };
static WmMesh g_wmMesh[2048];
static int    g_wmMeshCount = 0;
static WmInst* g_wmInst = nullptr;
static int    g_wmInstCount = 0;
static const int WM_MAX_INST = 65536;
static bool   g_buildingMesh = false;   // triangles go to the mesh buffer, not the base buffer
static float (*g_wmMeshTri)[9] = nullptr;
static long   g_wmMeshTriCount = 0;
// Bounds of what we actually emit. If this is much smaller than the world broadphase extents we
// are missing entire regions; if it matches, the holes are scattered within covered space.
// Base-BSP-only copy (no instanced geometry), single-sided, for an OBJ export. Every theory so
// far has assumed what reaches HVDB is correct; exporting our own buffer tests that directly.
static float g_wmMin[3] = { 1e30f, 1e30f, 1e30f };
static float g_wmMax[3] = { -1e30f, -1e30f, -1e30f };
static const int MAX_MESH_TRIS = 2000000;
// Containers are NOT shared between placements (2048 meshes for 2048 placements = 1:1), so
// instancing buys nothing here. Kept behind a flag rather than deleted.
#define WM_USE_INSTANCING 0

static inline void xf(const Xform& X, float lx, float ly, float lz, float& ox, float& oy, float& oz)
{
    ox = X.r[0]*lx + X.r[3]*ly + X.r[6]*lz + X.o[0];
    oy = X.r[1]*lx + X.r[4]*ly + X.r[7]*lz + X.o[1];
    oz = X.r[2]*lx + X.r[5]*ly + X.r[8]*lz + X.o[2];
}
// compose parent * (rot ct)
static Xform compose(const Xform& P, const float cr[9], const float ct[3])
{
    Xform R;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            R.r[c*3+r] = P.r[0*3+r]*cr[c*3+0] + P.r[1*3+r]*cr[c*3+1] + P.r[2*3+r]*cr[c*3+2];
    xf(P, ct[0], ct[1], ct[2], R.o[0], R.o[1], R.o[2]);
    return R;
}

// ===================== triangle buffer =====================
// Two independent meshes: dynamic objects (from the havok component array) and static world
// collision (from hkpWorld's fixed island). They are separate VDB viewers so the level can be
// toggled without hiding objects - and because they come from completely different sources.
// Object geometry is split by shape TYPE into separately coloured meshes so a rendering defect
// identifies its own culprit: whichever colour the holes are in names the emitter at fault.
enum { BK_CONVEX, BK_BOX, BK_ROUND, BK_COUNT };   // 6 | 4 | 1+5
static hkGeometry* g_objBucket[BK_COUNT] = { HK_NULL, HK_NULL, HK_NULL };
static int g_bucketTris[BK_COUNT] = { 0, 0, 0 };
static hkGeometry* g_objGeo = HK_NULL;   // unused for objects now; kept for the world path
// The world mesh is split across MANY hkGeometry objects. A single geometry holding millions of
// vertices is far outside what the VDB display path expects - display geometry is commonly indexed
// with 16-bit indices, so anything past 65535 vertices in one geometry is silently dropped, which
// shows up as large patchy holes even though every triangle was emitted correctly.
// 20000 tris = 60000 verts, safely under that ceiling.
static const int WM_TRIS_PER_CHUNK = 20000;
static const int WM_MAX_CHUNKS = 2048;          // 40M triangles of capacity
static hkGeometry* g_worldChunk[WM_MAX_CHUNKS] = { HK_NULL };
static int g_worldChunkCount = 0;
static hkGeometry* g_worldGeo = HK_NULL;
static hkGeometry* g_tvGeo = HK_NULL;      // trigger volume faces
static hkGeometry* g_scGeo = HK_NULL;      // soft ceiling faces
static long g_scenGeoGen = -1;
static hkGeometry* g_ltvOnGeo  = HK_NULL;
static hkGeometry* g_ltvOffGeo = HK_NULL;
static long g_liveGeoGen = -1;
static hkGeometry* g_target = HK_NULL;   // gather destination
// The world gather runs on the ENGINE thread (getChildShape needs Blam's TLS), but hkGeometry's
// hkArray allocates through Havok thread-memory that only exists on the VDB thread. So the engine
// thread fills this plain buffer and the VDB thread converts it into hkGeometry.
// Big maps: type-15 nodes are INSTANCE placements (each wraps its own type-7 container), so a
// shared mesh is re-emitted per instance and the level expands enormously. 25M triangles is
// 900 MB of plain buffer, so the region is RESERVED and committed on demand in 64 MB chunks -
// a small map pays only for what it touches. The proper fix remains addGeometryInstance().
static const int MAX_WORLD_TRIS = 25000000;
static const size_t WM_TRI_BYTES = 9 * sizeof(float);
static const size_t WM_CHUNK = 64u << 20;
static float (*g_wmTri)[9] = nullptr;
static size_t g_wmCommitted = 0;   // bytes actually committed out of the reservation
static bool   g_wmOom = false;

// Commit lazily so a small level does not pay for a huge reservation.
static bool wmEnsure(long triIndex)
{
    const size_t need = (size_t)(triIndex + 1) * WM_TRI_BYTES;
    if (need <= g_wmCommitted) return true;
    if (!g_wmTri) return false;
    size_t want = ((need + WM_CHUNK - 1) / WM_CHUNK) * WM_CHUNK;
    const size_t cap = (size_t)MAX_WORLD_TRIS * WM_TRI_BYTES;
    if (want > cap) want = cap;
    if (!VirtualAlloc((char*)g_wmTri + g_wmCommitted, want - g_wmCommitted, MEM_COMMIT, PAGE_READWRITE)) {
        if (!g_wmOom) { g_wmOom = true; LOG("!! world buffer commit FAILED at %zu MB - capping", g_wmCommitted >> 20); }
        return false;
    }
    g_wmCommitted = want;
    return true;
}
static volatile long g_wmTriCount = 0;
static volatile long g_wmGen = 0;
static bool g_plainTarget = false;
static int g_triCount = 0;               // triangles into the CURRENT target
static bool g_triCapped = false;
// Three separate caps all used to set g_triCapped, so "(TRUNCATED)" never said WHICH ran out and the
// obvious suspect (the 25M world buffer) was not necessarily the one that fired. Count them apart.
static long g_capMesh = 0;      // MAX_MESH_TRIS  - per-instance mesh buffer
static long g_capWorld = 0;     // MAX_WORLD_TRIS - the big world buffer (or a commit failure)
static long g_capGeom = 0;      // MAX_TRIS       - the hkGeometry target on the VDB thread

static void addTri(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
{
    if (g_plainTarget) {                       // engine thread: no Havok allocations here
        if (g_buildingMesh) {
            long mi = g_wmMeshTriCount;
            if (!g_wmMeshTri || mi >= MAX_MESH_TRIS) { g_triCapped = true; ++g_capMesh; return; }
            float* mt = g_wmMeshTri[mi];
            mt[0]=ax; mt[1]=ay; mt[2]=az; mt[3]=bx; mt[4]=by; mt[5]=bz; mt[6]=cx; mt[7]=cy; mt[8]=cz;
            g_wmMeshTriCount = mi + 1;
            ++g_triCount;
            return;
        }
        // record base-BSP triangles (outside any instance) once, single-sided, for the OBJ dump
        { const float px[3] = { ax, bx, cx }, py[3] = { ay, by, cy }, pz[3] = { az, bz, cz };
          for (int k = 0; k < 3; ++k) {
              if (px[k] < g_wmMin[0]) g_wmMin[0] = px[k];  if (px[k] > g_wmMax[0]) g_wmMax[0] = px[k];
              if (py[k] < g_wmMin[1]) g_wmMin[1] = py[k];  if (py[k] > g_wmMax[1]) g_wmMax[1] = py[k];
              if (pz[k] < g_wmMin[2]) g_wmMin[2] = pz[k];  if (pz[k] > g_wmMax[2]) g_wmMax[2] = pz[k];
          } }
        long i = g_wmTriCount;
        if (!g_wmTri || i >= MAX_WORLD_TRIS || !wmEnsure(i)) { g_triCapped = true; ++g_capWorld; return; }
        float* t = g_wmTri[i];
        t[0]=ax; t[1]=ay; t[2]=az; t[3]=bx; t[4]=by; t[5]=bz; t[6]=cx; t[7]=cy; t[8]=cz;
        g_wmTriCount = i + 1;
        ++g_triCount;
        return;
    }
    if (!g_target) return;
    if (g_triCount >= MAX_TRIS) { g_triCapped = true; ++g_capGeom; return; }  // never silently
    int base = g_target->m_vertices.getSize();
    hkVector4 v;
    v.set(ax, ay, az); g_target->m_vertices.pushBack(v);
    v.set(bx, by, bz); g_target->m_vertices.pushBack(v);
    v.set(cx, cy, cz); g_target->m_vertices.pushBack(v);
    hkGeometry::Triangle t; t.set(base, base + 1, base + 2);
    g_target->m_triangles.pushBack(t);
    ++g_triCount;
}
// Emit ONE triangle, wound so its normal points away from a reference point (the shape centre).
//
// Do NOT emit each triangle twice with opposing winding to fake two-sidedness: the duplicate pair
// is coincident, so the renderer shades whichever wins the depth test and a flat surface reads as
// alternating raised/recessed tiles. Consistent outward winding instead.
static void addTriOut(float ax, float ay, float az, float bx, float by, float bz,
                      float cx, float cy, float cz, float rx, float ry, float rz)
{
    float ux = bx - ax, uy = by - ay, uz = bz - az;
    float vx = cx - ax, vy = cy - ay, vz = cz - az;
    float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
    float mx = (ax + bx + cx) / 3.f - rx, my = (ay + by + cy) / 3.f - ry, mz = (az + bz + cz) / 3.f - rz;
    if (nx*mx + ny*my + nz*mz < 0.f) addTri(ax, ay, az, cx, cy, cz, bx, by, bz);
    else                             addTri(ax, ay, az, bx, by, bz, cx, cy, cz);
}

// ===================== primitives =====================
static void emitBox(const Xform& X, float hx, float hy, float hz)
{
    static const int f[6][4] = { {0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
    float p[8][3];
    for (int i = 0; i < 8; ++i)
        xf(X, (i & 1) ? hx : -hx, (i & 2) ? hy : -hy, (i & 4) ? hz : -hz, p[i][0], p[i][1], p[i][2]);
    for (int i = 0; i < 6; ++i) {
        const int* q = f[i];
        addTriOut(p[q[0]][0],p[q[0]][1],p[q[0]][2], p[q[1]][0],p[q[1]][1],p[q[1]][2],
                  p[q[2]][0],p[q[2]][1],p[q[2]][2], X.o[0], X.o[1], X.o[2]);
        addTriOut(p[q[0]][0],p[q[0]][1],p[q[0]][2], p[q[2]][0],p[q[2]][1],p[q[2]][2],
                  p[q[3]][0],p[q[3]][1],p[q[3]][2], X.o[0], X.o[1], X.o[2]);
    }
}

// unit sphere as a subdivided octahedron, scaled to r, centred at local c
static void emitSphere(const Xform& X, const float c[3], float r, int subdiv = 2)
{
    static const float v[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    static const int   f[8][3] = { {0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5} };
    float rc[3];   // world-space sphere centre = winding reference
    xf(X, c[0], c[1], c[2], rc[0], rc[1], rc[2]);
    const int n = 1 << subdiv;
    for (int i = 0; i < 8; ++i) {
        float a[3], b[3], cc[3];
        for (int k = 0; k < 3; ++k) { a[k] = v[f[i][0]][k]; b[k] = v[f[i][1]][k]; cc[k] = v[f[i][2]][k]; }

        // Barycentric grid over the face. Each cell contributes an "up" triangle AND, unless it
        // sits on the hypotenuse, a "down" triangle. Emitting only the up triangle leaves exactly
        // every other triangle missing - which is what put holes in every sphere and capsule cap.
        auto vert = [&](int iu, int iv, float* out) {
            float u = (float)iu / n, w = (float)iv / n;
            for (int k = 0; k < 3; ++k) out[k] = a[k] + (b[k] - a[k]) * u + (cc[k] - a[k]) * w;
            float L = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
            if (L > 1e-9f) { out[0] /= L; out[1] /= L; out[2] /= L; }
            // to local sphere space, then to world
            float lx = c[0] + out[0]*r, ly = c[1] + out[1]*r, lz = c[2] + out[2]*r;
            xf(X, lx, ly, lz, out[0], out[1], out[2]);
        };

        for (int iu = 0; iu < n; ++iu) {
            for (int iv = 0; iv + iu < n; ++iv) {
                float p00[3], p10[3], p01[3], p11[3];
                vert(iu, iv, p00); vert(iu + 1, iv, p10); vert(iu, iv + 1, p01);
                addTriOut(p00[0],p00[1],p00[2], p10[0],p10[1],p10[2], p01[0],p01[1],p01[2], rc[0],rc[1],rc[2]);
                if (iu + iv < n - 1) {
                    vert(iu + 1, iv + 1, p11);
                    addTriOut(p10[0],p10[1],p10[2], p11[0],p11[1],p11[2], p01[0],p01[1],p01[2], rc[0],rc[1],rc[2]);
                }
            }
        }
    }
}

static void emitCapsule(const Xform& X, const float A[3], const float B[3], float r)
{
    float ax = B[0]-A[0], ay = B[1]-A[1], az = B[2]-A[2];
    float L = sqrtf(ax*ax + ay*ay + az*az);
    if (L < 1e-6f) { emitSphere(X, A, r); return; }
    ax /= L; ay /= L; az /= L;
    float hx = (fabsf(ax) < 0.9f) ? 1.f : 0.f, hy = (fabsf(ax) < 0.9f) ? 0.f : 1.f;
    float ux = ay*0 - az*hy, uy = az*hx - ax*0, uz = ax*hy - ay*hx;
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul < 1e-6f) return;
    ux /= ul; uy /= ul; uz /= ul;
    float vx = ay*uz - az*uy, vy = az*ux - ax*uz, vz = ax*uy - ay*ux;
    float mid[3];   // world-space capsule axis midpoint = winding reference
    xf(X, (A[0]+B[0])*0.5f, (A[1]+B[1])*0.5f, (A[2]+B[2])*0.5f, mid[0], mid[1], mid[2]);
    const int N = 12;
    for (int i = 0; i < N; ++i) {
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        float p0[3], p1[3], p2[3], p3[3];
        for (int k = 0; k < 3; ++k) {
            float U = (k == 0 ? ux : k == 1 ? uy : uz), V = (k == 0 ? vx : k == 1 ? vy : vz);
            p0[k] = A[k] + r*(U*c0 + V*s0); p1[k] = A[k] + r*(U*c1 + V*s1);
            p2[k] = B[k] + r*(U*c0 + V*s0); p3[k] = B[k] + r*(U*c1 + V*s1);
        }
        float w0[3], w1[3], w2[3], w3[3];
        xf(X, p0[0],p0[1],p0[2], w0[0],w0[1],w0[2]); xf(X, p1[0],p1[1],p1[2], w1[0],w1[1],w1[2]);
        xf(X, p2[0],p2[1],p2[2], w2[0],w2[1],w2[2]); xf(X, p3[0],p3[1],p3[2], w3[0],w3[1],w3[2]);
        addTriOut(w0[0],w0[1],w0[2], w1[0],w1[1],w1[2], w2[0],w2[1],w2[2], mid[0],mid[1],mid[2]);
        addTriOut(w1[0],w1[1],w1[2], w3[0],w3[1],w3[2], w2[0],w2[1],w2[2], mid[0],mid[1],mid[2]);
    }
    emitSphere(X, A, r, 1);
    emitSphere(X, B, r, 1);
}

// Where convex geometry is LOST. Guessing at hole causes has failed repeatedly; count every
// discard point instead so the next change is aimed at the one that actually fires.
static int g_cvxShapes, g_cvxFallbackAabb, g_cvxNoPlanes, g_cvxCapped;
static int g_listStride[4], g_listStrideFail, g_listChildren, g_listEmitted, g_listChildBad;
static bool g_inWorldMesh = false;   // true while inside a type-7 subtree (its leaves ARE the level)
static int g_wmContainers, g_wmKeys, g_wmChildren, g_wmCallFail, g_wmSeen;
// Key encoding (RE'd from sub_1803CF110/350): class = key>>29.
//   1: index=key&0x3FFFFFF, subslot=(key>>26)&7   2: record=key&0xFFFF
//   3: record=key&0xFFFF, tri=(key>>16)&0x1FFF    <- "sections 24..27" are TRIANGLE BUCKETS of
//                                                    class 3, not four separate BSP sections.
static int g_wmDepthCut, g_wmDepthCutSnap;
static int g_wmClass[8], g_wmClassSnap[8];
static int g_wmRecSpan, g_wmRecEntered, g_wmRecSpanSnap, g_wmRecEnteredSnap;
static int g_wmEndInvalid, g_wmEndGuard, g_wmEndStuck, g_wmEndWrap;
// --- ground truth for the BASE structure BSP -------------------------------------------------
// The walk yields only 239 base surfaces (tag 1). Read the engine's OWN surface count and compare.
//   bspIdx = *(u32*)(shape+0x60); D = sub_180198560(bspIdx); V = resolve(D+0x2E8)
//   small CB = resolve(V+0x04) if *(u32*)(V+0x00)==1 ; large CB = resolve(V+0x10) if *(u32*)(V+0x0C)==1
//   surface count = *(int*)(CB+0x48)      resolve(off) = qword_182C2DCC0[off>>28] + 4*off
typedef uintptr_t (__fastcall *FnBspFromIdx)(unsigned);
static int g_bspTrue[8], g_bspTrueLarge[8], g_bspWalked[8], g_bspN;
static int g_bspTrueSnap[8], g_bspTrueLargeSnap[8], g_bspWalkedSnap[8], g_bspNSnap;
// The 239 base-BSP surfaces are POLYGONS (edge rings), not triangles - we have been reading only
// 3 vertices of each. Dump the first few raw so the real vertex layout/count can be read off.
static int g_tag1Dumped = 0;
static int g_polyHist[16], g_polyHistSnap[16], g_polyTooFew, g_polyTooFewSnap;
static int g_polyFarVtx, g_polyFarVtxSnap;
static int g_polyNonPlanar, g_polyNonPlanarSnap;

static uintptr_t tagResolve(uint32_t off)
{
    if (!off || off == 0xFFFFFFFFu) return 0;
    uint64_t base = rdQ(A(A_TAG_TABLE) + 8ull * (off >> 28));
    if (!okPtr(base)) return 0;
    return (uintptr_t)base + 4ull * off;
}
static bool bspSurfaceCounts(uintptr_t shape, int& small, int& large)
{
    small = large = -1;
    uint32_t bspIdx = rdD(shape + 0x60);
    if (!bspIdx || bspIdx == 0xFFFFFFFFu) return false;
    {   // gate against the scenario's own structure-BSP count: the callee does not bounds-check
        const uintptr_t sc = (uintptr_t)rdQ(A(A_SCEN_DATA));
        if (!okPtr(sc)) return false;
        const int nbsp = (int)rdD(sc + 0x60);
        if (nbsp <= 0 || nbsp > 64 || (int)bspIdx >= nbsp) return false;
    }
    FnBspFromIdx f = (FnBspFromIdx)A(A_BSP_FROM_IDX);
    if (!f) return false;
    if (!inMod((uintptr_t)f)) return false;
    uintptr_t D = 0;
    __try { D = f(bspIdx); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!okPtr(D)) return false;
    uintptr_t V = tagResolve(rdD(D + 0x2E8));
    if (!okPtr(V)) return false;
    if (rdD(V + 0x00) == 1) { uintptr_t cb = tagResolve(rdD(V + 0x04)); if (cb) small = (int)rdD(cb + 0x48); }
    if (rdD(V + 0x0C) == 1) { uintptr_t cb = tagResolve(rdD(V + 0x10)); if (cb) large = (int)rdD(cb + 0x48); }
    return true;
}
static int g_wmEndInvalidSnap, g_wmEndGuardSnap, g_wmEndStuckSnap, g_wmEndWrapSnap;
// The object pass runs every 4 frames and the world pass every 120, and BOTH call resetCvxStats().
// So world-pass counters must be snapshotted at the end of the world gather or later object
// passes wipe them to zero before the stats line prints (which made a working walk look dead).
static int g_wmSnap[5];
static int g_faceVertsSum;
static int g_cntHist[8];      // face-size histogram: index 0=3 verts .. 5=8+, 6/7 unused
static int g_looseVertsSum;   // what a LOOSE tolerance would have gathered, for comparison
// m_type 18 = a Blam scaled-convex wrapper. Counters only, never gates: a gate that silently
// rejects is indistinguishable from the missing-collision bug this is meant to fix.
static int g_t18Seen, g_t18Drawn, g_t18BadChild, g_t18BadScale, g_t18VtMatch, g_t18NonUniform;
static int g_t18WrongThread;   // seen off the engine thread - not followed, see case 18
static uintptr_t g_t18VtOff = 0;   // observed producer vtable, as a module offset
static int g_t18ChildType[64];
static float g_t18SMin = 1e30f, g_t18SMax = -1e30f;
static bool  g_t18Logged = false;
static int g_planesTotal, g_faceEmitted, g_faceTooFewVerts, g_faceTooFewHull, g_triDroppedNormal;
static int g_faceDegenNormal;   // plane normal collapsed under a scaled transform
static int g_maxNv, g_maxNp;
static void resetCvxStats()
{
    g_cvxShapes = g_cvxFallbackAabb = g_cvxNoPlanes = g_cvxCapped = 0;
    g_listStride[0]=g_listStride[1]=g_listStride[2]=g_listStride[3]=0;
    g_listStrideFail = g_listChildren = g_listEmitted = g_listChildBad = 0;
    g_wmContainers = g_wmKeys = g_wmChildren = g_wmCallFail = g_wmSeen = 0;
    g_wmDepthCut = g_wmRecSpan = g_wmRecEntered = 0;
    g_wmEndInvalid = g_wmEndGuard = g_wmEndStuck = g_wmEndWrap = 0;
    g_bspN = 0; g_tag1Dumped = 0; g_polyTooFew = 0; g_polyFarVtx = 0; g_polyNonPlanar = 0;
    for (int i = 0; i < 16; ++i) g_polyHist[i] = 0;
    for (int i = 0; i < 8; ++i) g_wmClass[i] = 0;
    g_dumpedInstTri = false;
    g_t31Degenerate = 0;
    for (int i = 0; i < 16; ++i) { g_t31Field34[i] = 0; g_t31Field30[i] = 0; }
    for (int i = 0; i < 64; ++i) { g_secSeen[i] = 0; g_secProbe[i] = 0; }
    g_secProbedThisGather = false;
    g_planesTotal = g_faceEmitted = g_faceTooFewVerts = g_faceTooFewHull = g_triDroppedNormal = 0;
    g_t18Seen = g_t18Drawn = g_t18BadChild = g_t18BadScale = g_t18VtMatch = 0;
    g_t18NonUniform = g_t18WrongThread = 0;
    for (int i = 0; i < 64; ++i) g_t18ChildType[i] = 0;
    g_faceDegenNormal = 0;
    g_faceVertsSum = 0; g_looseVertsSum = 0;
    for (int i = 0; i < 8; ++i) g_cntHist[i] = 0;
    g_maxNv = g_maxNp = 0;
}

// hkpConvexVerticesShape: vertices are hkFourTransposedPoints (48B blocks, SoA x4/y4/z4);
// faces are recovered by grouping vertices onto each plane equation and fanning them.
// Falls back to the reflected AABB if the plane set is unusable — never draws garbage.
static void emitConvexVertices(uintptr_t shape, const Xform& X)
{
    uintptr_t vdata = (uintptr_t)rdQ(shape + CVX_VDATA);
    int nv = (int)rdD(shape + CVX_NUMVERTS);
    uintptr_t pdata = (uintptr_t)rdQ(shape + CVX_PDATA);
    int np = (int)rdD(shape + CVX_NUMPLANES);

    float he[3] = { rdF(shape + CVX_AABB_HE), rdF(shape + CVX_AABB_HE + 4), rdF(shape + CVX_AABB_HE + 8) };
    float ce[3] = { rdF(shape + CVX_AABB_C),  rdF(shape + CVX_AABB_C + 4),  rdF(shape + CVX_AABB_C + 8) };

    auto fallbackAabb = [&]() {
        Xform T = X;
        xf(X, ce[0], ce[1], ce[2], T.o[0], T.o[1], T.o[2]);
        emitBox(T, he[0] > 0 ? he[0] : 0.05f, he[1] > 0 ? he[1] : 0.05f, he[2] > 0 ? he[2] : 0.05f);
    };

    ++g_cvxShapes;
    if (nv > g_maxNv) g_maxNv = nv;
    if (np > g_maxNp) g_maxNp = np;
    if (nv > 256 || np > 256) ++g_cvxCapped;   // exceeds our fixed arrays -> geometry WILL be lost
    if (!okPtr(vdata) || nv <= 0 || nv > 256) { ++g_cvxFallbackAabb; fallbackAabb(); return; }

    static float lv[256][3];
    float ext = 1e-4f;
    bool sane = true;
    for (int k = 0; k < nv; ++k) {
        float blk[12];
        if (!rdRaw((const void*)(vdata + (uintptr_t)(k / 4) * 48), blk, sizeof(blk))) { sane = false; break; }
        int ln = k & 3;
        lv[k][0] = blk[ln]; lv[k][1] = blk[4 + ln]; lv[k][2] = blk[8 + ln];
        for (int d = 0; d < 3; ++d) {
            if (!(lv[k][d] > -1e6f && lv[k][d] < 1e6f)) { sane = false; }
            float a = fabsf(lv[k][d]); if (a > ext) ext = a;
        }
        if (!sane) break;
    }
    // sanity: decoded verts must sit inside the reflected AABB (+slack). This is what validates
    // CVX_VDATA/CVX_NUMVERTS against the live build rather than trusting the Reach-matching guess.
    if (sane && (he[0] > 0 || he[1] > 0 || he[2] > 0)) {
        float lim = 1.5f * (fabsf(he[0]) + fabsf(he[1]) + fabsf(he[2]) + fabsf(ce[0]) + fabsf(ce[1]) + fabsf(ce[2])) + 1.0f;
        if (ext > lim) sane = false;
    }
    if (!sane) { ++g_cvxFallbackAabb; fallbackAabb(); return; }

    if (np < 1 || np > 256 || !okPtr(pdata)) {
        ++g_cvxNoPlanes;
        // no usable planes -> tight box around the decoded point cloud
        float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = { -1e9f,-1e9f,-1e9f };
        for (int k = 0; k < nv; ++k) for (int d = 0; d < 3; ++d) {
            if (lv[k][d] < mn[d]) mn[d] = lv[k][d];
            if (lv[k][d] > mx[d]) mx[d] = lv[k][d];
        }
        Xform T = X;
        xf(X, (mn[0]+mx[0])*0.5f, (mn[1]+mx[1])*0.5f, (mn[2]+mx[2])*0.5f, T.o[0], T.o[1], T.o[2]);
        emitBox(T, (mx[0]-mn[0])*0.5f, (mx[1]-mn[1])*0.5f, (mx[2]-mn[2])*0.5f);
        return;
    }

    // Hull centroid in WORLD space. For a convex shape this is strictly inside, so it gives an
    // unambiguous outward direction for every face. Do NOT wind from the plane normal: Havok's
    // m_planeEquations are not all outward-facing on this build, so plane-normal winding leaves
    // some faces inward -> invisible under backface culling -> holes. (The old double-winding
    // addTri2 hid this by drawing both orientations.)
    float hc[3] = { 0, 0, 0 };
    for (int k = 0; k < nv; ++k) { hc[0] += lv[k][0]; hc[1] += lv[k][1]; hc[2] += lv[k][2]; }
    hc[0] /= nv; hc[1] /= nv; hc[2] /= nv;
    xf(X, hc[0], hc[1], hc[2], hc[0], hc[1], hc[2]);

    // Loose tolerance (the value that produced hole-free objects), clamped so it can never
    // exceed a quarter of the hull's thinnest dimension - which is the only way two opposite
    // faces can be merged into one gather.
    float thin = fabsf(he[0]);
    if (fabsf(he[1]) < thin) thin = fabsf(he[1]);
    if (fabsf(he[2]) < thin) thin = fabsf(he[2]);
    thin *= 2.f;                                  // half-extent -> full thickness
    float eps = ext * 0.03f + 1e-4f;
    if (thin > 1e-5f && thin * 0.25f < eps) eps = thin * 0.25f;
    static int idx[256]; static float ang[256];
    for (int f = 0; f < np; ++f) {
        ++g_planesTotal;
        float pl[4];
        if (!rdRaw((const void*)(pdata + 16ull * f), pl, sizeof(pl))) break;
        float nx = pl[0], ny = pl[1], nz = pl[2], dp = pl[3];
        // ADAPTIVE tolerance. Start tight so a thin slab's opposite faces never merge; only if a
        // plane fails to find a face do we relax, which recovers hulls whose vertices are less
        // exactly coplanar (quantised hulls) without re-breaking thin ones. A fixed tight eps
        // punched holes in objects; a fixed loose eps indented flat floors.
        // Tolerance is the ORIGINAL LOOSE value, capped by the hull's THINNEST dimension.
        //
        // The two failures pull opposite ways and one global value cannot serve both:
        //   loose -> faces complete (no holes), but a thin slab's top plane also gathers its
        //            bottom verts, so a flat floor renders indented
        //   tight -> slab correct, but faces truncate and leave wedge holes
        // The separating quantity is per-shape: only a THIN shape can have its two faces merge.
        // m_aabbHalfExtents (already read for the fallback) gives that directly, so keep the loose
        // behaviour everywhere and clamp it below the slab thickness only where it matters.
        static float ad[256];
        for (int k = 0; k < nv; ++k)
            ad[k] = fabsf(nx*lv[k][0] + ny*lv[k][1] + nz*lv[k][2] + dp);
        int cnt = 0; float cx = 0, cy = 0, cz = 0;
        for (int k = 0; k < nv; ++k)
            if (ad[k] <= eps) { idx[cnt++] = k; cx += lv[k][0]; cy += lv[k][1]; cz += lv[k][2]; }
        if (cnt < 3) { ++g_faceTooFewVerts; continue; }
        g_faceVertsSum += cnt;
        g_cntHist[cnt < 3 ? 0 : (cnt - 3 < 5 ? cnt - 3 : 5)]++;
        // How many vertices a LOOSE tolerance would have taken for this same plane. If this is
        // ~equal to cnt then the hull is genuinely triangulated and the convex path is complete
        // (so holes are elsewhere); if it is much larger, we are still truncating faces.
        { int lc = 0; const float le = ext * 0.03f + 1e-4f;
          for (int k = 0; k < nv; ++k) if (ad[k] <= le) ++lc;
          g_looseVertsSum += lc; }
        cx /= cnt; cy /= cnt; cz /= cnt;
        float hx = (fabsf(nx) < 0.9f) ? 1.f : 0.f, hy = (fabsf(nx) < 0.9f) ? 0.f : 1.f;
        float ux = ny*0 - nz*hy, uy = nz*hx - nx*0, uz = nx*hy - ny*hx;
        float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul < 1e-6f) continue;
        ux /= ul; uy /= ul; uz /= ul;
        float vx = ny*uz - nz*uy, vy = nz*ux - nx*uz, vz = nx*uy - ny*ux;
        // Order the face by computing the 2D CONVEX HULL of the gathered vertices projected into
        // the plane basis - do NOT just sort by angle around the centroid. Angular sort assumes
        // every gathered vertex lies on the face boundary; one stray vertex pulled in from a
        // neighbouring face (which the relaxed tolerance can do) makes the ring zigzag, flipping
        // fan triangles and punching wedge-shaped holes. A hull discards strays and interior
        // points instead, and yields a guaranteed-convex, correctly wound polygon.
        static float ps[256], pt[256];
        for (int i = 0; i < cnt; ++i) {
            float wx = lv[idx[i]][0]-cx, wy = lv[idx[i]][1]-cy, wz = lv[idx[i]][2]-cz;
            ps[i] = wx*ux + wy*uy + wz*uz;
            pt[i] = wx*vx + wy*vy + wz*vz;
        }
        // sort by (s,t)
        for (int i = 1; i < cnt; ++i) {
            float a = ps[i], b = pt[i]; int ki = idx[i]; int j = i - 1;
            while (j >= 0 && (ps[j] > a || (ps[j] == a && pt[j] > b))) {
                ps[j+1] = ps[j]; pt[j+1] = pt[j]; idx[j+1] = idx[j]; --j;
            }
            ps[j+1] = a; pt[j+1] = b; idx[j+1] = ki;
        }
        // Andrew's monotone chain
        static int hull[520]; int hn = 0;
        for (int i = 0; i < cnt; ++i) {
            while (hn >= 2) {
                int o = hull[hn-2], p = hull[hn-1];
                if ((ps[p]-ps[o])*(pt[i]-pt[o]) - (pt[p]-pt[o])*(ps[i]-ps[o]) > 1e-12f) break;
                --hn;
            }
            hull[hn++] = i;
        }
        int lower = hn + 1;
        for (int i = cnt - 2; i >= 0; --i) {
            while (hn >= lower) {
                int o = hull[hn-2], p = hull[hn-1];
                if ((ps[p]-ps[o])*(pt[i]-pt[o]) - (pt[p]-pt[o])*(ps[i]-ps[o]) > 1e-12f) break;
                --hn;
            }
            hull[hn++] = i;
        }
        cnt = hn - 1;                       // last point repeats the first
        if (cnt < 3) { ++g_faceTooFewHull; continue; }
        ++g_faceEmitted;
        static int ord[520];
        for (int i = 0; i < cnt; ++i) ord[i] = idx[hull[i]];
        for (int i = 0; i < cnt; ++i) idx[i] = ord[i];
        // Face normal in world space. X is NOT necessarily rigid - the m_type 18 wrapper composes a
        // uniform scale into it - so this must be renormalised. The comparison below divides the
        // triangle normal by its own length but not this one, so a non-unit pn scales d directly:
        // at the live scale 0.0819 every |d| lands under the 0.3 threshold and the whole shape is
        // dropped as "face-jumping". Normalising is a no-op when X is rigid.
        float pnx = X.r[0]*nx + X.r[3]*ny + X.r[6]*nz;
        float pny = X.r[1]*nx + X.r[4]*ny + X.r[7]*nz;
        float pnz = X.r[2]*nx + X.r[5]*ny + X.r[8]*nz;
        {
            const float pl = sqrtf(pnx*pnx + pny*pny + pnz*pnz);
            if (pl < 1e-12f) { ++g_faceDegenNormal; continue; }
            pnx /= pl; pny /= pl; pnz /= pl;
        }

        float w0[3], wi[3], wj[3];
        xf(X, lv[idx[0]][0], lv[idx[0]][1], lv[idx[0]][2], w0[0], w0[1], w0[2]);
        for (int i = 1; i + 1 < cnt; ++i) {
            xf(X, lv[idx[i]][0],   lv[idx[i]][1],   lv[idx[i]][2],   wi[0], wi[1], wi[2]);
            xf(X, lv[idx[i+1]][0], lv[idx[i+1]][1], lv[idx[i+1]][2], wj[0], wj[1], wj[2]);
            // Second guard: a triangle belonging to this face must have a normal parallel to the
            // plane. Any triangle that jumps between two faces points sideways - drop it rather
            // than draw a spurious step. Winding comes from the plane normal, not a centroid guess.
            float ux2 = wi[0]-w0[0], uy2 = wi[1]-w0[1], uz2 = wi[2]-w0[2];
            float vx2 = wj[0]-w0[0], vy2 = wj[1]-w0[1], vz2 = wj[2]-w0[2];
            float tnx = uy2*vz2 - uz2*vy2, tny = uz2*vx2 - ux2*vz2, tnz = ux2*vy2 - uy2*vx2;
            float tl = sqrtf(tnx*tnx + tny*tny + tnz*tnz);
            if (tl < 1e-12f) continue;
            float d = (tnx*pnx + tny*pny + tnz*pnz) / tl;
            if (fabsf(d) < 0.3f) { ++g_triDroppedNormal; continue; }   // face-jumping tris
            addTriOut(w0[0],w0[1],w0[2], wi[0],wi[1],wi[2], wj[0],wj[1],wj[2], hc[0],hc[1],hc[2]);
        }
    }
}

// ===================== shape dispatch (on hkpShape::m_type, not vtables) =====================
static int g_unhandled[64] = { 0 };
// Snapshot of the WORLD pass's unhandled types: the world walk now runs once per level, so the
// shared counters get cleared by later object passes before the stats line prints.
static int g_unhandledWorld[64] = { 0 };

// One-shot hexdump of each unhandled shape type. getChildShape builds its result into a transient
// caller buffer, so these shapes only exist during the walk - the offline probe can never see them.
static bool g_dumpedType[64] = { false };
static void dumpUnknownShape(uintptr_t shape, uint32_t type)
{
    if (type >= 64 || g_dumpedType[type]) return;
    g_dumpedType[type] = true;
    unsigned char b[0xB0];
    if (!rdRaw((const void*)shape, b, sizeof(b))) { LOG("  type %u @ %016llX unreadable", type, (unsigned long long)shape); return; }
    LOG("  === UNHANDLED shape type %u @ %016llX ===", type, (unsigned long long)shape);
    for (int i = 0; i < (int)sizeof(b); i += 16) {
        char line[384]; int o = 0; line[0] = 0;
        appendf(line, sizeof(line), o, "    +%03X ", i);
        for (int j = 0; j < 16; ++j) appendf(line, sizeof(line), o, "%02X ", b[i + j]);
        appendf(line, sizeof(line), o, " | ");
        for (int j = 0; j < 16; j += 4) {
            float f; memcpy(&f, b + i + j, 4);
            // %g, not %f: this is a dump of possibly uninitialised memory, and %f has no maximum
            // width - a garbage float prints 35 characters and used to blow the row past its buffer.
            appendf(line, sizeof(line), o, "%12.4g ", (double)f);
        }
        LOG("%s", line);
    }
}



// ===================== m_type 7: the level mesh (COLLECTION_USER) =====================
// A Blam-custom shape collection. There is no static layout to read - it is enumerated through
// the standard hkpShapeContainer interface, whose sub-object sits at shape+0x20 and whose vtable
// is the shape's own vtable + 0x68 (same arrangement as the m_type 9 LIST).
//   slot 2 = getFirstKey, slot 3 = getNextKey, slot 5 = getChildShape   (RVA 0x88F398)
// These are ENGINE virtuals, so every call is SEH-guarded: if the engine expects thread state we
// do not have, the walk aborts cleanly instead of taking the game down.
// WM_MAX_KEYS is only an anti-spin backstop. 400000 was too low: one container tripped it and had
// its enumeration truncated, losing a large contiguous chunk of the level. Real spins are caught
// precisely by the wrap check below, so this can be generous.
enum { WM_CONTAINER = 0x20, HK_INVALID_SHAPE_KEY = 0xFFFFFFFFu, WM_MAX_KEYS = 8000000 };

// An m_type 31 N-gon stores its vertices at shape+0x40+16*i. We scan up to WM_POLY_SLOTS of them and
// scrub each one's w tag before every getChildShape call, so the buffer must cover
// 0x40 + 16*WM_POLY_SLOTS bytes - 576, NOT the 512 of Havok's nominal hkpShapeBuffer.
// Getting this wrong wrote 0xFFFFFFFF over the /GS cookie 64 bytes past a 512-byte stack buffer and
// took the game down with STATUS_STACK_BUFFER_OVERRUN (0xC0000409) - which no SEH guard can catch,
// because the cookie check raises it through __fastfail and bypasses SEH and VEH entirely.
enum { WM_POLY_SLOTS = 32, WM_SHAPE_BUF = 1024 };
static_assert(0x40 + 16 * WM_POLY_SLOTS <= WM_SHAPE_BUF, "shape buffer too small for the w scrub");

typedef unsigned  (__fastcall *FnFirstKey)(uintptr_t);
typedef unsigned  (__fastcall *FnNextKey)(uintptr_t, unsigned);
typedef uintptr_t (__fastcall *FnChildShape)(uintptr_t, unsigned, void*);

static bool callFirstKey(uintptr_t c, FnFirstKey f, unsigned& out)
{ __try { out = f(c); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool callNextKey(uintptr_t c, FnNextKey f, unsigned k, unsigned& out)
{ __try { out = f(c, k); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool callChildShape(uintptr_t c, FnChildShape f, unsigned k, void* buf, uintptr_t& out)
{ __try { out = f(c, k, buf); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

static void emitShape(uintptr_t shape, const Xform& X, int depth);   // fwd

static void emitWorldMesh(uintptr_t shape, const Xform& X, int depth)
{
    uintptr_t cont = shape + WM_CONTAINER;
    uint64_t cvt = rdQ(cont);
    if (!inMod((uintptr_t)cvt)) { ++g_wmCallFail; return; }
    FnFirstKey   first = (FnFirstKey)  rdQ((uintptr_t)cvt + 8 * 2);
    FnNextKey    next  = (FnNextKey)   rdQ((uintptr_t)cvt + 8 * 3);
    FnChildShape child = (FnChildShape)rdQ((uintptr_t)cvt + 8 * 5);
    if (!inMod((uintptr_t)first) || !inMod((uintptr_t)next) || !inMod((uintptr_t)child)) { ++g_wmCallFail; return; }
    ++g_wmContainers;

    __declspec(align(16)) unsigned char buf[WM_SHAPE_BUF];
    unsigned key = 0;
    if (!callFirstKey(cont, first, key)) { ++g_wmCallFail; return; }

    const bool savedIn = g_inWorldMesh;
    g_inWorldMesh = true;
    int guard = 0;
    int recMax = -1, recEntered = 0, tag1Keys = 0;
    bool endedInvalid = true;
    while (key != HK_INVALID_SHAPE_KEY) {
        if (++guard >= WM_MAX_KEYS) { ++g_wmEndGuard; endedInvalid = false; break; }
        ++g_wmKeys;
        g_secSeen[(key >> 26) & 63]++;
        { const unsigned cls = key >> 29;
          ++g_wmClass[cls & 7];
          if (cls == 1) ++tag1Keys;              // tag 1 = base structure-BSP surfaces
          if (cls == 2 || cls == 3) {                     // class 2/3 index a record table
              const int rec = (int)(key & 0xFFFF);
              if (rec > recMax) recMax = rec;
              if (((key >> 16) & 0x1FFF) == 0) ++recEntered;   // first key of that record
          } }
        // Invalidate the vertex index tags before every call. getChildShape reuses THIS buffer, so
        // a 4-vertex surface following an 8-vertex one leaves slots 4..7 holding the older polygon's
        // vertices - whose w tags continue the run and get absorbed, drawing a sliver across the
        // level. Clearing just the 32 w words is enough (the run detector only reads those).
        for (int wv = 0; wv < WM_POLY_SLOTS; ++wv)
            *(uint32_t*)(buf + 0x40 + 16 * wv + 12) = 0xFFFFFFFFu;
        uintptr_t cs = 0;
        if (!callChildShape(cont, child, key, buf, cs)) { ++g_wmCallFail; endedInvalid = false; break; }
        // one-shot raw dump of base-BSP (tag 1) children: 0x100 bytes as hex + floats
        if ((key >> 29) == 1 && g_tag1Dumped < 3 && isHkObj(cs)) {
            ++g_tag1Dumped;
            unsigned char db[0x100];
            if (rdRaw((const void*)cs, db, sizeof(db))) {
                LOG("  === TAG-1 BASE BSP SURFACE #%d key=%08X m_type=%u +0x34=%u ===",
                    g_tag1Dumped, key, rdD(cs + SHAPE_TYPE), rdD(cs + 0x34));
                for (int r = 0; r < (int)sizeof(db); r += 16) {
                    char ln[384]; int o5 = 0; ln[0] = 0;
                    appendf(ln, sizeof(ln), o5, "    +%03X ", r);
                    for (int q = 0; q < 16; ++q) appendf(ln, sizeof(ln), o5, "%02X ", db[r + q]);
                    appendf(ln, sizeof(ln), o5, "| ");
                    for (int q = 0; q < 16; q += 4) {
                        float fv; memcpy(&fv, db + r + q, 4);
                        appendf(ln, sizeof(ln), o5, "%11.3g ", (double)fv);
                    }
                    LOG("%s", ln);
                }
            }
        }
        if (isHkObj(cs)) { ++g_wmChildren; emitShape(cs, X, depth + 1); }
        unsigned nk = HK_INVALID_SHAPE_KEY;
        if (!callNextKey(cont, next, key, nk)) { ++g_wmCallFail; endedInvalid = false; break; }
        if (nk == key) { ++g_wmEndStuck; endedInvalid = false; break; }
        // Class-3 triangle index is 13 bits: at tri == 0x1FFF the increment WRAPS to tri 0 of the
        // same record instead of advancing, which nk!=key cannot detect - it would spin until the
        // key cap and truncate the container. Detect the regression directly.
        if ((nk >> 29) == 3 && (key >> 29) == 3 &&
            (nk & 0xFFFF) == (key & 0xFFFF) &&
            ((nk >> 16) & 0x1FFF) <= ((key >> 16) & 0x1FFF)) {
            ++g_wmEndWrap; endedInvalid = false; break;
        }
        key = nk;
    }
    if (endedInvalid) ++g_wmEndInvalid;
    // Base structure-BSP containers only (they carry a valid bspIdx). 5 of them - O(1).
    if (g_bspN < 8) {
        int sm = -1, lg = -1;
        if (bspSurfaceCounts(shape, sm, lg) && (sm >= 0 || lg >= 0)) {
            g_bspTrue[g_bspN] = sm; g_bspTrueLarge[g_bspN] = lg;
            g_bspWalked[g_bspN] = tag1Keys;      // tag-1 keys walked in THIS container
            ++g_bspN;
        }
    }
    // The engine SKIPS records it considers non-collidable (four gates inside getFirstKey/getNextKey).
    // span vs entered exposes how many records it refused to hand us at all.
    g_wmRecSpan += recMax + 1;
    g_wmRecEntered += recEntered;
    g_inWorldMesh = savedIn;
}

// Two passes over the SAME body set, filtered by shape type:
//   false -> object pass: emit everything except the world mesh (type 7)
//   true  -> world pass : emit ONLY the world mesh
// Container nodes are always traversed in both passes so the world mesh can be reached.
static bool g_wantWorldMesh = false;

static void emitShape(uintptr_t shape, const Xform& X, int depth)
{
    if (depth > MAX_DEPTH) { if (g_wantWorldMesh) ++g_wmDepthCut; return; }
    if (!isHkObj(shape)) return;
    uint32_t type = rdD(shape + SHAPE_TYPE);

    // leaf filtering: skip leaves that belong to the other pass
    const bool isWorldMesh = (type == 7);
    const bool isLeaf = (type == 1 || type == 3 || type == 4 || type == 5 || type == 6 ||
                        type == 15 || type == 31);
    if (isLeaf) {
        // world pass draws leaves only inside a type-7 subtree; object pass draws all others
        if (g_wantWorldMesh ? !g_inWorldMesh : false) return;
    } else if (type == 7 && !g_wantWorldMesh) {
        return;   // level mesh belongs to the World viewer only
    }

    switch (type) {
    case ST_SPHERE: {
        if (!g_wantWorldMesh) g_target = g_objBucket[BK_ROUND];
        float c[3] = { 0,0,0 };
        emitSphere(X, c, rdF(shape + CVX_RADIUS));
        return;
    }
    case ST_CAPSULE: {
        if (!g_wantWorldMesh) g_target = g_objBucket[BK_ROUND];
        float A[3] = { rdF(shape+CAP_VERTA), rdF(shape+CAP_VERTA+4), rdF(shape+CAP_VERTA+8) };
        float B[3] = { rdF(shape+CAP_VERTB), rdF(shape+CAP_VERTB+4), rdF(shape+CAP_VERTB+8) };
        emitCapsule(X, A, B, rdF(shape + CVX_RADIUS));
        return;
    }
    case ST_BOX: {
        if (!g_wantWorldMesh) g_target = g_objBucket[BK_BOX];
        emitBox(X, rdF(shape+CVX_AABB_HE), rdF(shape+CVX_AABB_HE+4), rdF(shape+CVX_AABB_HE+8));
        return;
    }
    case 3: {   // hkpTriangleShape: hkpConvexShape then vertexA/B/C
        float a[3] = { rdF(shape+0x30), rdF(shape+0x34), rdF(shape+0x38) };
        float b[3] = { rdF(shape+0x40), rdF(shape+0x44), rdF(shape+0x48) };
        float c[3] = { rdF(shape+0x50), rdF(shape+0x54), rdF(shape+0x58) };
        float wa[3], wb[3], wc[3];
        xf(X, a[0],a[1],a[2], wa[0],wa[1],wa[2]);
        xf(X, b[0],b[1],b[2], wb[0],wb[1],wb[2]);
        xf(X, c[0],c[1],c[2], wc[0],wc[1],wc[2]);
        addTri(wa[0],wa[1],wa[2], wb[0],wb[1],wb[2], wc[0],wc[1],wc[2]);
        addTri(wa[0],wa[1],wa[2], wc[0],wc[1],wc[2], wb[0],wb[1],wb[2]);  // 2-sided: mesh tris have no inside
        return;
    }
    case 15: {   // level transform wrapper: container @ +0x20 -> child @ +0x28,
                 // hkTransform @ +0x50 (cols +0x50/+0x60/+0x70, translation +0x80).
                 // Verified orthonormal in the live dump: |col|=1.0 and col0.col1 == 0.
        uintptr_t child = (uintptr_t)rdQ(shape + 0x28);
        if (!isHkObj(child)) { if (type < 64) g_unhandled[type]++; return; }
        float m[16];
        if (!rdRaw((const void*)(shape + 0x50), m, sizeof(m))) return;
        float r9[9] = { m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10] };
        float t3[3] = { m[12],m[13],m[14] };
        // Instance it: build the child mesh once at IDENTITY, then just record this placement.
        // (Nested placements inside a mesh are expanded normally - g_buildingMesh guards that.)
        if (WM_USE_INSTANCING && g_wantWorldMesh && !g_buildingMesh) {
            int mi = -1;
            for (int k = 0; k < g_wmMeshCount; ++k)
                if (g_wmMesh[k].cont == child) { mi = k; break; }
            if (mi < 0 && g_wmMeshCount < 2048) {
                mi = g_wmMeshCount++;
                g_wmMesh[mi].cont = child;
                g_wmMesh[mi].triStart = g_wmMeshTriCount;
                g_buildingMesh = true;
                emitShape(child, IDENT, depth + 1);
                g_buildingMesh = false;
                g_wmMesh[mi].triCount = g_wmMeshTriCount - g_wmMesh[mi].triStart;
            }
            if (mi >= 0 && g_wmInst && g_wmInstCount < WM_MAX_INST) {
                g_wmInst[g_wmInstCount].mesh = mi;
                g_wmInst[g_wmInstCount].X = compose(X, r9, t3);
                ++g_wmInstCount;
            }
            return;
        }
        { const bool savedIn = g_inInstance;
          g_inInstance = true;
          emitShape(child, compose(X, r9, t3), depth + 1);
          g_inInstance = savedIn; }
        return;
    }
    case 31: {   // level-mesh SURFACE: an N-gon, not a triangle.
        // Vertices run at +0x40 + 16*i and are SELF-DESCRIBING: each w component holds its own
        // index (w_i == w_0 + i, e.g. 0x3F000000, 0x3F000001, ...), so the run terminates itself.
        // Reading a fixed 3 drew one triangle across an N-gon's corners and left the rest of every
        // surface as a hole - which is exactly the BSP hole pattern (base surfaces are quads).
        static const int MAX_POLY_V = WM_POLY_SLOTS;   // must match the scrub above
        float pv[MAX_POLY_V][3];
        const uint32_t w0 = rdD(shape + 0x40 + 12);
        int nv = 0;
        for (; nv < MAX_POLY_V; ++nv) {
            const uintptr_t vp = shape + 0x40 + 16 * (uintptr_t)nv;
            if (nv > 0 && (rdD(vp + 12) - w0) != (uint32_t)nv) break;   // index run ended
            // No distance test: a legitimate surface can span most of the level, so any threshold
            // either never fires or amputates real polygons. Planarity below is the real check.
            pv[nv][0] = rdF(vp); pv[nv][1] = rdF(vp + 4); pv[nv][2] = rdF(vp + 8);
        }
        if (nv < 3) { ++g_polyTooFew; return; }
        if (nv > 3) {
            float ux = pv[1][0]-pv[0][0], uy = pv[1][1]-pv[0][1], uz = pv[1][2]-pv[0][2];
            float vx2 = pv[2][0]-pv[0][0], vy2 = pv[2][1]-pv[0][1], vz2 = pv[2][2]-pv[0][2];
            float nx2 = uy*vz2 - uz*vy2, ny2 = uz*vx2 - ux*vz2, nz2 = ux*vy2 - uy*vx2;
            float nl = sqrtf(nx2*nx2 + ny2*ny2 + nz2*nz2);
            if (nl > 1e-9f) {
                nx2 /= nl; ny2 /= nl; nz2 /= nl;
                float ext = 0.f;
                for (int i = 1; i < nv; ++i) {
                    float ex = fabsf(pv[i][0]-pv[0][0]), ey = fabsf(pv[i][1]-pv[0][1]), ez = fabsf(pv[i][2]-pv[0][2]);
                    if (ex > ext) ext = ex; if (ey > ext) ext = ey; if (ez > ext) ext = ez;
                }
                const float tol = 0.10f * ext + 0.05f;
                for (int i = 3; i < nv; ++i) {
                    float d = nx2*(pv[i][0]-pv[0][0]) + ny2*(pv[i][1]-pv[0][1]) + nz2*(pv[i][2]-pv[0][2]);
                    if (fabsf(d) > tol) { nv = i; ++g_polyNonPlanar; break; }   // stale vertex -> stop
                }
            }
        }
        if (nv < 16) ++g_polyHist[nv]; else ++g_polyHist[15];

        float w[MAX_POLY_V][3];
        for (int i = 0; i < nv; ++i) xf(X, pv[i][0], pv[i][1], pv[i][2], w[i][0], w[i][1], w[i][2]);
        // fan from vertex 0; two-sided, since a collision surface has no "outside" to orient to
        for (int i = 1; i + 1 < nv; ++i) {
            addTri(w[0][0],w[0][1],w[0][2], w[i][0],w[i][1],w[i][2], w[i+1][0],w[i+1][1],w[i+1][2]);
            addTri(w[0][0],w[0][1],w[0][2], w[i+1][0],w[i+1][1],w[i+1][2], w[i][0],w[i][1],w[i][2]);
        }
        return;
    }
    case 7:
        ++g_wmSeen;                       // counted before any validity check
        emitWorldMesh(shape, X, depth);
        return;
    case ST_CONVEX_VERTICES:
        if (!g_wantWorldMesh) g_target = g_objBucket[BK_CONVEX];
        emitConvexVertices(shape, X);
        return;
    case ST_CONVEX_TRANSLATE: {
        uintptr_t child = (uintptr_t)rdQ(shape + XF_CHILD);
        float t[3] = { rdF(shape+XF_TRANSFORM), rdF(shape+XF_TRANSFORM+4), rdF(shape+XF_TRANSFORM+8) };
        static const float I9[9] = { 1,0,0,0,1,0,0,0,1 };
        emitShape(child, compose(X, I9, t), depth + 1);
        return;
    }
    // m_type 30: a custom Blam shape. Its vtable fn sub_1806DFC00 (RVA 0x6DFC00) loads
    // [this+0x40/0x50/0x60/0x70] as a 4x4 transform and calls a virtual on [this+0x30], i.e. it
    // is structurally a transform wrapper like type 12. isHkObj() gates the child, so if this
    // reading is wrong nothing is drawn rather than garbage.
    case 30:
    case ST_CONVEX_TRANSFORM: {
        uintptr_t child = (uintptr_t)rdQ(shape + XF_CHILD);
        float m[16];
        if (!rdRaw((const void*)(shape + XF_TRANSFORM), m, sizeof(m))) return;
        float r9[9] = { m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10] };
        float t3[3] = { m[12],m[13],m[14] };
        emitShape(child, compose(X, r9, t3), depth + 1);
        return;
    }
    case ST_LIST: {
        uintptr_t arr = (uintptr_t)rdQ(shape + LIST_ARR);
        int n = (int)rdD(shape + LIST_ARR + 8);
        if (!okPtr(arr) || n <= 0 || n > 1024) return;
        // childInfo stride is not reflected. Validate a candidate against MANY entries, not two:
        // a 32-byte record whose +16 field happens to hold a pointer will pass a 2-entry test at
        // stride 16 and then read every other child as garbage, silently dropping half of a
        // multi-part object. Require every sampled entry to resolve, and score the candidates.
        static const int strides[] = { 32, 16, 24, 8 };
        const int probeN = n < 16 ? n : 16;
        int stride = 0, bestScore = 0;
        for (int si = 0; si < 4; ++si) {
            const int s = strides[si];
            int good = 0;
            for (int i = 0; i < probeN; ++i)
                if (isHkObj((uintptr_t)rdQ(arr + (uintptr_t)s * i))) ++good;
            if (good == probeN && good > bestScore) { bestScore = good; stride = s; }
        }
        if (!stride) { ++g_listStrideFail; return; }
        g_listStride[stride == 32 ? 0 : stride == 16 ? 1 : stride == 24 ? 2 : 3]++;
        int emitted = 0;
        for (int i = 0; i < n; ++i) {
            uintptr_t c = (uintptr_t)rdQ(arr + (uintptr_t)stride * i);
            if (!isHkObj(c)) { ++g_listChildBad; continue; }   // count, never skip silently
            ++emitted;
            emitShape(c, X, depth + 1);
        }
        g_listChildren += n; g_listEmitted += emitted;
        return;
    }
    // m_type 18: a Blam scaled-convex WRAPPER (producer sub_1803D0B00, vtable +0x88E150). Built on
    // the fly when a polyhedra element carries scale != 1 - scale == 1 returns the child raw, which
    // is why most of these already drew. VERIFIED against a live dump: userData 0xFFFF00AB,
    // m_type 0x12, unscaled child radius @ +0x20, RAW CHILD @ +0x28, uniform scale broadcast into
    // four lanes @ +0x30.
    // ⚠ +0x28 is NOT an hkpSingleShapeContainer - the engine's getAabb dereferences [this+0x28]
    //   directly as an object, so there is no +8 indirection here (unlike types 11/12/15).
    // ⚠ The wrapper lives in emitWorldMesh's own 512-byte buffer (its +0x4C/+0x5C/... still held our
    //   w-tag scrub), so it is TRANSIENT: dispatch immediately, never cache or instance on it.
    // The engine scales in CHILD space (getAabb scales the child's half-extents, then transforms),
    // and compose(P, cr, ct) computes P * cr, so composing with a diagonal reproduces that order
    // exactly - and a diagonal is symmetric, so the row/column-major question cannot arise.
    case 18: {
        ++g_t18Seen;
        // Engine thread (world pass) only: see the note above about pool lifetime. The object pass
        // runs on the VDB thread, so following this child there would race the engine.
        if (!g_plainTarget) { ++g_t18WrongThread; return; }
        {   // Identity check, build-independent: every type-18 should come from ONE producer, so
            // record the observed vtable offset and count agreement rather than comparing against a
            // hardcoded per-build VA (which would read 0 on any other build and look like a bug).
            const uintptr_t vt = (uintptr_t)rdQ(shape);
            const uintptr_t off = (vt > g_base) ? vt - g_base : 0;
            if (!g_t18VtOff) g_t18VtOff = off;
            if (off == g_t18VtOff) ++g_t18VtMatch;
        }
        const uintptr_t child = (uintptr_t)rdQ(shape + 0x28);
        if (!isShapeObj(child)) { ++g_t18BadChild; return; }
        const float sc[3] = { rdF(shape + 0x30), rdF(shape + 0x34), rdF(shape + 0x38) };
        if (!(sc[1] == sc[0] && sc[2] == sc[0])) ++g_t18NonUniform;       // producer broadcasts one float
        for (int k = 0; k < 3; ++k)                                       // rejects 0, NaN and absurd
            if (!(fabsf(sc[k]) > 1e-6f && fabsf(sc[k]) < 1e4f)) { ++g_t18BadScale; return; }
        if (sc[0] < g_t18SMin) g_t18SMin = sc[0];
        if (sc[0] > g_t18SMax) g_t18SMax = sc[0];
        { const uint32_t cty = rdD(child + SHAPE_TYPE); ++g_t18ChildType[cty < 64 ? cty : 63]; }
        const float cr[9] = { sc[0],0,0, 0,sc[1],0, 0,0,sc[2] };
        static const float ct0[3] = { 0, 0, 0 };
        const Xform CX = compose(X, cr, ct0);
        // One-shot pivot proof: the engine scales about the CHILD's origin. If a prop's hull is
        // authored around its own origin this lands on the prop; if it is authored in BSP space the
        // world centre collapses toward the origin, which this line makes obvious immediately.
        if (!g_t18Logged) {
            g_t18Logged = true;
            const float lc[3] = { rdF(child + CVX_AABB_C), rdF(child + CVX_AABB_C + 4),
                                  rdF(child + CVX_AABB_C + 8) };
            float wc[3]; xf(CX, lc[0], lc[1], lc[2], wc[0], wc[1], wc[2]);
            LOG("  t18 #1: vt=+%llX child=%016llX childType=%u scale=%.4f "
                "centre local=(%.2f %.2f %.2f) world=(%.2f %.2f %.2f)",
                (unsigned long long)(rdQ(shape) - g_base), (unsigned long long)child,
                rdD(child + SHAPE_TYPE), sc[0], lc[0], lc[1], lc[2], wc[0], wc[1], wc[2]);
        }
        ++g_t18Drawn;
        emitShape(child, CX, depth + 1);
        return;
    }

    case ST_BVTREE: {
        // world/static collision: MOPP wrapping a shape collection
        uintptr_t child = (uintptr_t)rdQ(shape + BV_CHILD);
        if (!isHkObj(child)) { if (type < 64) g_unhandled[type]++; return; }
        emitShape(child, X, depth + 1);
        return;
    }
    case 28: {
        uintptr_t child = (uintptr_t)rdQ(shape + WRAP_CHILD);
        if (!isHkObj(child)) { if (type < 64) g_unhandled[type]++; return; }
        emitShape(child, X, depth + 1);
        return;
    }
    default:
        if (type < 64) g_unhandled[type]++;
        dumpUnknownShape(shape, type);
        return;
    }
}

// ===================== body transform =====================
static bool bodyXform(uintptr_t body, Xform& X)
{
    float m[16];
    if (!rdRaw((const void*)(body + OFF_TRANSFORM), m, sizeof(m))) return false;
    X.r[0]=m[0]; X.r[1]=m[1]; X.r[2]=m[2];
    X.r[3]=m[4]; X.r[4]=m[5]; X.r[5]=m[6];
    X.r[6]=m[8]; X.r[7]=m[9]; X.r[8]=m[10];
    X.o[0]=m[12]; X.o[1]=m[13]; X.o[2]=m[14];
    // reject a body whose basis is not orthonormal rather than drawing a smeared shape
    float l0 = X.r[0]*X.r[0]+X.r[1]*X.r[1]+X.r[2]*X.r[2];
    return l0 > 0.9f && l0 < 1.1f;
}

// ===================== gather =====================
static uintptr_t g_world = 0;
static int g_bodyCount = 0;

// Every body we draw: the havok component array (dynamic objects) PLUS hkpWorld's fixed bodies
// (static scenery/machines and the level mesh), deduplicated. Both viewers iterate this one set.
static int g_scBadTri = 0;   // soft-ceiling triangles rejected by the bounding-sphere check
static volatile long g_objGen = 0;   // bumped per object gather; viewers re-send only on change
static uint64_t g_lastWorldSig = 0;  // world is re-walked only when this fingerprint changes
static uintptr_t g_allBodies[65536];
static int g_allN = 0;

static uintptr_t compArray()
{
    uintptr_t p = (uintptr_t)rdQ(A(A_COMP_ARRAY));
    if (!okPtr(p) || rdQ(p + DA_ELEMSIZE) != COMP_STRIDE) return 0;
    int mc = (int)rdD(p + DA_MAXCOUNT);
    return (mc > 0 && mc <= 65536) ? p : 0;
}

// walk every live havok component -> rigid bodies, emitting each body's shape tree posed by
// its live motion-state transform.
static void addBody(uintptr_t b)
{
    if (!isHkObj(b) || g_allN >= 65536) return;
    for (int i = 0; i < g_allN; ++i) if (g_allBodies[i] == b) return;   // dedup
    g_allBodies[g_allN++] = b;
}

// Build the full body set: havok components first (which also yields the world pointer), then
// hkpWorld's fixed rigid body + fixed island entities.
static void collectBodies()
{
    g_allN = 0; g_world = 0;
    uintptr_t da = compArray();
    if (da) {
        uintptr_t comps = (uintptr_t)rdQ(da + DA_DATA);
        int maxc = (int)rdD(da + DA_MAXCOUNT);
        if (okPtr(comps)) {
            for (int i = 0; i < maxc; ++i) {
                uintptr_t comp = comps + (uintptr_t)COMP_STRIDE * i;
                uint64_t rbArr = rdQ(comp + COMP_RBARR);
                int32_t rbCnt = (int32_t)rdD(comp + COMP_RBCOUNT);
                if (!okPtr(rbArr) || rbCnt <= 0 || rbCnt > 64) continue;
                for (int k = 0; k < rbCnt; ++k) {
                    uintptr_t body = (uintptr_t)rdQ((uintptr_t)rbArr + (uintptr_t)RBREC_STRIDE * k + RBREC_BODY);
                    if (!isHkObj(body)) continue;
                    if (!g_world) { uint64_t w = rdQ(body + WO_WORLD); if (okPtr(w)) g_world = (uintptr_t)w; }
                    addBody(body);
                }
            }
        }
    }
    if (!g_world) return;
    addBody((uintptr_t)rdQ(g_world + 0x38));                    // fixed rigid body

    // Every entity of an island array (entities hkArray @ island+0x60).
    auto addIsland = [](uintptr_t isl) {
        if (!okPtr(isl)) return;
        uint64_t ents = rdQ(isl + ISLAND_ENTITIES);
        uint32_t ec = rdD(isl + ISLAND_ENTITIES + 8);
        if (!okPtr(ents) || ec > 65535) return;
        for (uint32_t i = 0; i < ec; ++i)
            addBody((uintptr_t)rdQ((uintptr_t)ents + 8ull * i));
    };

    addIsland((uintptr_t)rdQ(g_world + W_FIXEDISLAND));         // the fixed island

    // ALSO the active and inactive island arrays. These were only ever read by the "Havok Islands"
    // viewer, never by the gather - so any BSP cluster living on a body in one of the 231 inactive
    // islands was invisible to us. That is a strong candidate for the level being far smaller than
    // it should be (334k tris where millions are expected).
    struct { int off; } arrs[2] = { { W_ACTIVE_ISLANDS }, { W_INACTIVE_ISLANDS } };
    for (int a = 0; a < 2; ++a) {
        uint64_t data = rdQ(g_world + arrs[a].off);
        uint32_t n = rdD(g_world + arrs[a].off + 8);
        if (!okPtr(data) || n > 65535) continue;
        for (uint32_t i = 0; i < n; ++i)
            addIsland((uintptr_t)rdQ((uintptr_t)data + 8ull * i));
    }
}

static void emitAll(hkGeometry* dst, bool worldPass, int& bodiesOut)
{
    g_target = dst;
    g_wantWorldMesh = worldPass;
    resetCvxStats();
    g_triCount = 0; g_triCapped = false;
    g_capMesh = g_capWorld = g_capGeom = 0;
    bodiesOut = 0;
    if (dst) { dst->m_vertices.clear(); dst->m_triangles.clear(); }
    if (!worldPass)
        for (int b = 0; b < BK_COUNT; ++b)
            if (g_objBucket[b]) { g_objBucket[b]->m_vertices.clear(); g_objBucket[b]->m_triangles.clear(); }
    for (int i = 0; i < g_allN; ++i) {
        uintptr_t body = g_allBodies[i];
        Xform X;
        if (!bodyXform(body, X)) {
            if (!worldPass) continue;   // static world shapes are usually authored in world space
            X = IDENT;
        }
        ++bodiesOut;
        emitShape((uintptr_t)rdQ(body + OFF_SHAPE), X, 0);
    }
}

static void gatherBodies()
{
    collectBodies();
    emitAll(g_objGeo, false, g_bodyCount);
    InterlockedIncrement(&g_objGen);
}

// Cheap VDB-thread fingerprint of the STATIC world: world ptr, fixed island, entity count, and
// each fixed body's root shape. Changes on map load / zone stream, so the expensive engine-thread
// walk of ~341k children runs once per level instead of on a timer.
// ORDER-INDEPENDENT fingerprint. Havok reorders the island entity array as bodies activate and
// deactivate, so an order-dependent hash flapped between the same few values and triggered a full
// 2.5M-key re-walk (a multi-second engine-thread stall) on every flap. Accumulate with commutative
// addition so a permutation of the same bodies hashes identically.
static uint64_t worldSignature()
{
    auto scramble = [](uint64_t v) {
        v ^= v >> 33; v *= 0xFF51AFD7ED558CCDull;
        v ^= v >> 33; v *= 0xC4CEB9FE1A85EC53ull;
        return v ^ (v >> 33);
    };
    if (!g_world) return 0;
    // Fingerprint the STATIC LEVEL ONLY: bodies whose root shape is a type-7 level-mesh container,
    // keyed by the shape and its structure-BSP index. Do NOT include island membership or body
    // count - Havok migrates bodies into the fixed island as they fall asleep, so a settling crate
    // changed the fingerprint and triggered a full ~400 ms re-walk of the entire level.
    uint64_t acc = scramble(g_world);
    for (int i = 0; i < g_allN; ++i) {
        const uintptr_t b = g_allBodies[i];
        const uintptr_t sh = (uintptr_t)rdQ(b + OFF_SHAPE);
        if (!isHkObj(sh) || rdD(sh + SHAPE_TYPE) != 7) continue;   // dynamic objects ignored
        acc += scramble(sh) ^ scramble(rdD(sh + 0x60));            // shape + bspIdx
    }
    return acc;
}
static uint64_t worldSignatureGuarded()
{ __try { return worldSignature(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

// ---- static world collision -------------------------------------------------------------
// The level BSP is NOT in the havok components array (that is Blam's per-object physics only).
// It lives on the world's fixed bodies: hkpWorld::m_fixedIsland (+0x30) and m_fixedRigidBody
// (+0x38). Walk those instead. Fixed bodies keep a valid motion state, but fall back to identity
// if it is not orthonormal, since static world shapes are usually authored in world space.
static int g_worldBodies = 0, g_worldTris = 0;
static volatile long g_ticks = 0;            // engine-thread tick counter
static volatile long g_wantWorldGather = 0;  // VDB thread asks, engine thread performs
static volatile long g_gatherBusy = 0;       // the two gathers share g_allBodies
static long g_worldGeoGen = -1;
static bool g_worldCapped = false;
static long g_capMeshWorld = 0, g_capWorldWorld = 0;

// World Collision = ONLY the level mesh (shape type 7). Static scenery and machines are ordinary
// objects and are drawn by the Object Collision viewer, so they no longer appear here.
static void gatherWorld()
{
    if (!g_allN) collectBodies();
    g_plainTarget = true;
    g_wmTriCount = 0;
    g_wmMeshTriCount = 0;
    g_wmMin[0]=g_wmMin[1]=g_wmMin[2]= 1e30f;
    g_wmMax[0]=g_wmMax[1]=g_wmMax[2]=-1e30f;
    g_wmMeshCount = 0;
    g_wmInstCount = 0;
    emitAll(HK_NULL, true, g_worldBodies);
    g_plainTarget = false;
    InterlockedIncrement(&g_wmGen);
    g_worldTris = g_triCount;
    g_worldCapped = g_triCapped;
    g_wmSnap[0] = g_wmSeen; g_wmSnap[1] = g_wmContainers; g_wmSnap[2] = g_wmKeys;
    g_wmSnap[3] = g_wmChildren; g_wmSnap[4] = g_wmCallFail;
    for (int i = 0; i < 64; ++i) g_unhandledWorld[i] = g_unhandled[i];
    g_capMeshWorld = g_capMesh; g_capWorldWorld = g_capWorld;
    for (int i = 0; i < 16; ++i) g_t31Snap34[i] = g_t31Field34[i];
    g_t31SnapDegen = g_t31Degenerate;
    for (int i = 0; i < 64; ++i) g_secSnap[i] = g_secSeen[i];
    for (int i = 0; i < 8; ++i) g_wmClassSnap[i] = g_wmClass[i];
    g_wmDepthCutSnap = g_wmDepthCut;
    g_wmRecSpanSnap = g_wmRecSpan; g_wmRecEnteredSnap = g_wmRecEntered;
    g_wmEndInvalidSnap = g_wmEndInvalid; g_wmEndGuardSnap = g_wmEndGuard; g_wmEndStuckSnap = g_wmEndStuck; g_wmEndWrapSnap = g_wmEndWrap;
    for (int i = 0; i < 8; ++i) { g_bspTrueSnap[i] = g_bspTrue[i]; g_bspTrueLargeSnap[i] = g_bspTrueLarge[i]; g_bspWalkedSnap[i] = g_bspWalked[i]; }
    g_bspNSnap = g_bspN;
    for (int i = 0; i < 16; ++i) g_polyHistSnap[i] = g_polyHist[i];
    g_polyTooFewSnap = g_polyTooFew; g_polyFarVtxSnap = g_polyFarVtx; g_polyNonPlanarSnap = g_polyNonPlanar;
}


// ===================== scenario: trigger volumes + soft ceilings =====================
// Offsets RE'd from the tag metadata and confirmed against real reading code (see README).
// Everything here runs on the ENGINE THREAD: the string_id and tag-index lookups are engine
// calls, and this project has already learned what happens when engine code is called from
// the wrong thread.

enum {
    SCEN_STRUCT_DESIGNS = 0x6C,   // count @ +0x6C, address @ +0x70, stride 32
    SCEN_TRIGGER_VOLS   = 0x278,  // count @ +0x278, address @ +0x27C, stride 124
    // scenario_trigger_volume
    TV_NAME = 0x00, TV_TYPE = 0x0C, TV_FORWARD = 0x10, TV_UP = 0x1C,
    TV_POSITION = 0x28, TV_EXTENTS = 0x34,
    TV_SECTOR_PTS = 0x44,          // count @ +0x44, addr @ +0x48, stride 20 (position xyz @ +0)
    TV_BOUNDS = 0x5C,              // x0,x1,y0,y1,z0,z1 as 6 floats
    TV_STRIDE = 124,
    // structure_design ('sddt')
    SD_SOFTCEIL = 0x40,            // count @ D+0x40, address @ D+0x44, stride 20
    SC_NAME = 0x00, SC_TYPE = 0x04, SC_TRIS = 0x08,   // triangles: count @ +0x08, addr @ +0x0C
    SC_STRIDE = 20,
    SCT_V0 = 0x20, SCT_V1 = 0x2C, SCT_V2 = 0x38, SCT_STRIDE = 68,
};

typedef const char* (__fastcall *FnStringIdText)(unsigned);
typedef uintptr_t   (__fastcall *FnTagGetData)(uintptr_t, unsigned);

struct ScenLabel { char name[48]; float ctr[3]; };

static const int MAX_SCEN_TRIS  = 200000;
static const int MAX_SCEN_EDGES = 200000;
static const int MAX_LABELS     = 512;

static float (*g_tvTri)[9]  = nullptr;  static int g_tvTriN  = 0;   // trigger volume faces
static float (*g_tvEdge)[6] = nullptr;  static int g_tvEdgeN = 0;   // trigger volume edges
static float (*g_scTri)[9]  = nullptr;  static int g_scTriN  = 0;   // soft ceiling faces
static ScenLabel g_tvLabel[MAX_LABELS]; static int g_tvLabelN = 0;

// ====================================================================================================================
// COMMIT-ON-DEMAND FOR THE SCENARIO SCRATCH BUFFERS.
//
// ⚠ THESE WERE RESERVED BUT NEVER COMMITTED, SO EVERY WRITE TO THEM FAULTED.
//
// The port switched all six scratch reservations from committed-up-front to MEM_RESERVE, to avoid charging the
// game process ~91 MB on the first Havok toggle. Its comment says "g_wmTri already worked this way (wmEnsure);
// the rest now match" - but only g_wmTri ever got an ensure helper. The other five reserve address space and
// nothing else, so the first store into any of them hits a reserved-but-uncommitted page.
//
// MEASURED, via the teardown diagnostic and the PDB:
//
//     FIRST-CHANCE 0xC0000005 at HCMInternal.dll+0x92BE82 - tried to WRITE 0x2353B1F0000
//     = `anonymous namespace'::tvTri + 0x22        (i.e. `t[0]=a[0]`, the first store)
//
// It never surfaced as a crash because the gather paths run under __try: the access violation was swallowed and
// the affected viewer simply came back empty or truncated. A silently broken feature rather than a visible fault,
// which is why it survived the port unnoticed.
//
// Same shape as wmEnsure, generalised. Chunked so a small level commits one chunk rather than the whole
// reservation, which is the entire point of reserving in the first place.
// ====================================================================================================================
static const size_t SCRATCH_CHUNK = 4u << 20;   // 4 MB

struct ScratchCommit
{
    void*       base      = nullptr;
    size_t      committed = 0;
    size_t      cap       = 0;
    bool        oom       = false;
    const char* name      = "";
};

static ScratchCommit g_tvTriCommit, g_tvEdgeCommit, g_scTriCommit;

// Returns false when the write must be skipped (not reserved, past the cap, or the commit failed).
static bool scratchEnsure(ScratchCommit& s, size_t needBytes)
{
    if (needBytes <= s.committed) return true;
    if (!s.base || needBytes > s.cap) return false;

    size_t want = ((needBytes + SCRATCH_CHUNK - 1) / SCRATCH_CHUNK) * SCRATCH_CHUNK;
    if (want > s.cap) want = s.cap;

    if (!VirtualAlloc((char*)s.base + s.committed, want - s.committed, MEM_COMMIT, PAGE_READWRITE))
    {
        if (!s.oom) { s.oom = true; LOG("!! %s commit FAILED at %zu MB - capping", s.name, s.committed >> 20); }
        return false;
    }
    s.committed = want;
    return true;
}

// Per-volume slices of the shared triangle/edge buffers, so the live viewer can recolour a volume
// without re-reading any tag data. Index matches g_tvLabel.
struct TvRange { int triStart, triCount, edgeStart, edgeCount, index; uintptr_t elem; };
static TvRange g_tvRange[MAX_LABELS];

// ---------------------------------------------------------------------------------------------
// The game clock the SCRIPTS run on.
//
// Wall-clock time is the wrong unit for "is this volume still being tested": when the player pauses,
// no script runs, so every volume's window expires and the whole viewer goes red. What we want is
// the clock the script scheduler itself uses, because it stops exactly when scripts stop.
//
// sleep_until (sub_1801FB130) computes its wake deadline from
//     *(int*)( *(void**)(TLS[TlsIndex] + 0x98) + 0x0C )
// and compares the same value to decide whether a sleeping thread is due. That is the script clock,
// so it is the correct unit for a "recently tested" window - and it freezes while paused, which
// makes holding the last known state fall out for free rather than needing a pause flag.
//
// TlsIndex comes from our target's own PE TLS directory, so it needs no hardcoded address and
// survives updates. AddressOfIndex is already a relocated VA.
static DWORD blamTlsIndex()
{
    static DWORD cached = 0xFFFFFFFFu;
    if (cached != 0xFFFFFFFFu) return cached;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0xFFFFFFFFu;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0xFFFFFFFFu;
        const IMAGE_DATA_DIRECTORY& d =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (!d.VirtualAddress || d.Size < sizeof(IMAGE_TLS_DIRECTORY64)) return 0xFFFFFFFFu;
        const IMAGE_TLS_DIRECTORY64* tls =
            (const IMAGE_TLS_DIRECTORY64*)(g_base + d.VirtualAddress);
        if (!tls->AddressOfIndex) return 0xFFFFFFFFu;
        cached = *(DWORD*)tls->AddressOfIndex;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
    return cached;
}

// Engine thread only (needs Blam's TLS block). Returns -1 when unavailable.
static int readGameTick()
{
    const DWORD idx = blamTlsIndex();
    if (idx == 0xFFFFFFFFu) return -1;
    __try {
        void** tp = (void**)__readgsqword(0x58);          // TEB::ThreadLocalStoragePointer
        if (!tp) return -1;
        void* blk = tp[idx];
        if (!blk) return -1;
        void* timing = *(void**)((char*)blk + 0x98);
        if (!timing) return -1;
        return *(int*)((char*)timing + 0x0C);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Sampled once per engine tick so the hot recorder is a plain global read with no TLS access.
static volatile long g_gameTick = -1;
static volatile long g_gameTickBad = 0;

static unsigned char g_ltvActive[MAX_LABELS];   // engine thread writes, VDB thread reads

// "Live" means the mission scripts are currently TESTING the volume - not that anything is inside
// it. The engine only evaluates a volume when a running script polls it, so the test call itself is
// the ground truth. A detour records the tick of the most recent test per volume; a volume counts
// as live while that tick is recent enough to span a script's sleep interval.
static const int MAX_SCEN_VOLS  = 8192;
// Measured in GAME ticks (the script clock above), not wall clock and not pump ticks. Script poll
// loops sleep 15..60 game ticks between tests, so the window must comfortably exceed the longest of
// those or a live volume flickers red between its own polls. Because this clock stops when the
// simulation stops, a paused game simply holds the last known state.
static const long LIVE_WINDOW_TICKS = 120;
static volatile long g_tvLastTest[MAX_LABELS];  // game-tick stamp; detour writes, poll reads
static short g_tvSlotOfIndex[MAX_SCEN_VOLS];    // scenario volume index -> our label slot, -1 none
static volatile long g_tvTestHits = 0;          // total detour firings, for the diagnostic
// A volume that has actually FIRED (a test returned "inside") has done its job, so it stops being
// interesting even while the script is still nominally polling it. The latch is cleared when the
// volume goes dormant, so if a script ever re-arms it, it counts as live again.
static unsigned char g_tvFired[MAX_LABELS];
static volatile long g_tvFireHits = 0;
static int  g_ltvOnN = 0, g_ltvOffN = 0;
static volatile long g_ltvGen = 0;              // bumped only when the active SET changes
static int  g_ltvTick = 0;
static int g_tvCount = 0, g_scCount = 0, g_tvSector = 0, g_tvBox = 0;
static volatile long g_scenGen = 0;

// The scratchEnsure call is what makes these stores legal - see the block above. Committing BEFORE the index is
// advanced matters: on a commit failure the triangle is dropped rather than counted, so the count never claims
// data that was never written.
static void tvTri(const float* a, const float* b, const float* c)
{
    if (!g_tvTri || g_tvTriN >= MAX_SCEN_TRIS) return;
    if (!scratchEnsure(g_tvTriCommit, (size_t)(g_tvTriN + 1) * 9 * sizeof(float))) return;
    float* t = g_tvTri[g_tvTriN++];
    t[0]=a[0];t[1]=a[1];t[2]=a[2]; t[3]=b[0];t[4]=b[1];t[5]=b[2]; t[6]=c[0];t[7]=c[1];t[8]=c[2];
}
static void tvTri2(const float* a, const float* b, const float* c)
{   // no two-sided flag exists in the protocol; a camera inside a volume must still see it
    tvTri(a, b, c); tvTri(a, c, b);
}
static void tvEdge(const float* a, const float* b)
{
    if (!g_tvEdge || g_tvEdgeN >= MAX_SCEN_EDGES) return;
    if (!scratchEnsure(g_tvEdgeCommit, (size_t)(g_tvEdgeN + 1) * 6 * sizeof(float))) return;
    float* e = g_tvEdge[g_tvEdgeN++];
    e[0]=a[0];e[1]=a[1];e[2]=a[2]; e[3]=b[0];e[4]=b[1];e[5]=b[2];
}
static void scTri(const float* a, const float* b, const float* c)
{
    if (!g_scTri || g_scTriN >= MAX_SCEN_TRIS) return;
    if (!scratchEnsure(g_scTriCommit, (size_t)(g_scTriN + 1) * 9 * sizeof(float))) return;
    float* t = g_scTri[g_scTriN++];
    t[0]=a[0];t[1]=a[1];t[2]=a[2]; t[3]=b[0];t[4]=b[1];t[5]=b[2]; t[6]=c[0];t[7]=c[1];t[8]=c[2];
}

// Extrude an XY polygon between z0 and z1: side quads + top/bottom fans + all edges.
static void emitPrism(const float (*pts)[2], int n, float z0, float z1, float* ctrOut)
{
    if (n < 3) return;
    float cx = 0, cy = 0;
    for (int i = 0; i < n; ++i) { cx += pts[i][0]; cy += pts[i][1]; }
    cx /= n; cy /= n;
    ctrOut[0] = cx; ctrOut[1] = cy; ctrOut[2] = 0.5f * (z0 + z1);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float a0[3] = { pts[i][0], pts[i][1], z0 }, a1[3] = { pts[i][0], pts[i][1], z1 };
        const float b0[3] = { pts[j][0], pts[j][1], z0 }, b1[3] = { pts[j][0], pts[j][1], z1 };
        tvTri2(a0, b0, b1); tvTri2(a0, b1, a1);           // side wall
        tvEdge(a0, b0); tvEdge(a1, b1); tvEdge(a0, a1);   // outline
        if (i >= 1 && i + 1 < n) {                        // caps, fanned from vertex 0
            const float f0[3] = { pts[0][0], pts[0][1], z0 }, f1[3] = { pts[0][0], pts[0][1], z1 };
            tvTri2(f0, a0, b0); tvTri2(f1, a1, b1);
        }
    }
}

static void emitObb(const float* pos, const float* ext, const float* fwd, const float* up, float* ctrOut)
{
    // right-handed basis from forward/up; the box spans [0,extents] along (f, r, u) from position,
    // matching the engine's own point test (0 <= local <= extents).
    float f[3] = { fwd[0], fwd[1], fwd[2] }, u[3] = { up[0], up[1], up[2] };
    auto norm = [](float* v) { float l = sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; } };
    norm(f); norm(u);
    float r[3] = { u[1]*f[2]-u[2]*f[1], u[2]*f[0]-u[0]*f[2], u[0]*f[1]-u[1]*f[0] };
    norm(r);
    float c[8][3];
    for (int i = 0; i < 8; ++i) {
        const float a = (i & 1) ? ext[0] : 0.f, b = (i & 2) ? ext[1] : 0.f, d = (i & 4) ? ext[2] : 0.f;
        for (int k = 0; k < 3; ++k) c[i][k] = pos[k] + f[k]*a + r[k]*b + u[k]*d;
    }
    for (int k = 0; k < 3; ++k) ctrOut[k] = 0.5f * (c[0][k] + c[7][k]);
    static const int F[6][4] = { {0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
    for (int i = 0; i < 6; ++i) {
        tvTri2(c[F[i][0]], c[F[i][1]], c[F[i][2]]);
        tvTri2(c[F[i][0]], c[F[i][2]], c[F[i][3]]);
    }
    static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    for (int i = 0; i < 12; ++i) tvEdge(c[E[i][0]], c[E[i][1]]);
}

static void gatherScenario()
{
    g_tvTriN = g_tvEdgeN = g_scTriN = g_tvLabelN = 0;
    g_tvCount = g_scCount = g_tvSector = g_tvBox = 0;
    for (int i = 0; i < MAX_SCEN_VOLS; ++i) g_tvSlotOfIndex[i] = -1;
    for (int i = 0; i < MAX_LABELS; ++i) { g_tvLastTest[i] = -1; g_tvFired[i] = 0; }

    const uintptr_t scen = (uintptr_t)rdQ(A(A_SCEN_DATA));
    if (!okPtr(scen)) return;

    FnStringIdText sidText = (FnStringIdText)A(A_SID_TEXT);
    FnTagGetData   tagGet  = (FnTagGetData)A(A_TAG_GET_DATA);
    if (!sidText || !tagGet) return;

    // ---- trigger volumes -------------------------------------------------------------
    const int tvN = (int)rdD(scen + SCEN_TRIGGER_VOLS);
    const uintptr_t tvBase = tagResolve(rdD(scen + SCEN_TRIGGER_VOLS + 4));
    if (okPtr(tvBase) && tvN > 0 && tvN < 8192) {
        g_tvCount = tvN;
        for (int i = 0; i < tvN; ++i) {
            const uintptr_t e = tvBase + (uintptr_t)TV_STRIDE * i;
            const uint32_t ty = rdD(e + TV_TYPE) & 0xFFFF;
            float ctr[3] = { 0, 0, 0 };
            bool drew = false;
            const int triStart = g_tvTriN, edgeStart = g_tvEdgeN;

            if (ty == 1) {                       // sector: XY polygon extruded between z0..z1
                const int pn = (int)rdD(e + TV_SECTOR_PTS);
                const uintptr_t pb = tagResolve(rdD(e + TV_SECTOR_PTS + 4));
                if (okPtr(pb) && pn >= 3 && pn <= 256) {
                    static float pts[256][2];
                    for (int k = 0; k < pn; ++k) {
                        pts[k][0] = rdF(pb + 20ull * k);
                        pts[k][1] = rdF(pb + 20ull * k + 4);
                    }
                    emitPrism(pts, pn, rdF(e + TV_BOUNDS + 16), rdF(e + TV_BOUNDS + 20), ctr);
                    drew = true; ++g_tvSector;
                }
            }
            if (!drew) {                          // bounding box (or a sector with no points)
                float pos[3] = { rdF(e+TV_POSITION), rdF(e+TV_POSITION+4), rdF(e+TV_POSITION+8) };
                float ext[3] = { rdF(e+TV_EXTENTS),  rdF(e+TV_EXTENTS+4),  rdF(e+TV_EXTENTS+8)  };
                float fwd[3] = { rdF(e+TV_FORWARD),  rdF(e+TV_FORWARD+4),  rdF(e+TV_FORWARD+8)  };
                float up[3]  = { rdF(e+TV_UP),       rdF(e+TV_UP+4),       rdF(e+TV_UP+8)       };
                if (ext[0] != 0.f || ext[1] != 0.f || ext[2] != 0.f) {
                    emitObb(pos, ext, fwd, up, ctr); drew = true; ++g_tvBox;
                }
            }
            if (drew && g_tvLabelN < MAX_LABELS) {
                TvRange& R  = g_tvRange[g_tvLabelN];
                R.triStart  = triStart;  R.triCount  = g_tvTriN  - triStart;
                R.edgeStart = edgeStart; R.edgeCount = g_tvEdgeN - edgeStart;
                R.index     = i;         R.elem      = e;
                if (i >= 0 && i < MAX_SCEN_VOLS) g_tvSlotOfIndex[i] = (short)g_tvLabelN;
                ScenLabel& L = g_tvLabel[g_tvLabelN++];
                L.ctr[0] = ctr[0]; L.ctr[1] = ctr[1]; L.ctr[2] = ctr[2];
                L.name[0] = 0;
                __try {
                    const char* nm = sidText(rdD(e + TV_NAME));
                    if (nm) { strncpy_s(L.name, nm, _TRUNCATE); }
                } __except (EXCEPTION_EXECUTE_HANDLER) { L.name[0] = 0; }
                if (!L.name[0]) sprintf_s(L.name, "trigger_%d", i);
            }
        }
    }

    // ---- soft ceilings: structure_design ('sddt') tags, NOT the Havok world ------------
    const int sdN = (int)rdD(scen + SCEN_STRUCT_DESIGNS);
    const uintptr_t sdBase = tagResolve(rdD(scen + SCEN_STRUCT_DESIGNS + 4));
    if (okPtr(sdBase) && sdN > 0 && sdN < 256) {
        for (int b = 0; b < sdN; ++b) {
            const uint32_t designIdx = rdD(sdBase + 32ull * b + 0x0C);
            if (designIdx == 0xFFFFFFFFu || (designIdx & 0xFFFF) == 0xFFFF) continue;
            uintptr_t D = 0;
            __try { D = tagGet(0, designIdx); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!okPtr(D)) continue;

            const int scN = (int)rdD(D + SD_SOFTCEIL);
            const uintptr_t scBase = tagResolve(rdD(D + SD_SOFTCEIL + 4));
            if (!okPtr(scBase) || scN <= 0 || scN > 4096) continue;
            g_scCount += scN;
            for (int i = 0; i < scN; ++i) {
                const uintptr_t sc = scBase + (uintptr_t)SC_STRIDE * i;
                const int tn = (int)rdD(sc + SC_TRIS);
                const uintptr_t tb = tagResolve(rdD(sc + SC_TRIS + 4));
                if (!okPtr(tb) || tn <= 0 || tn > 65535) continue;
                for (int k = 0; k < tn; ++k) {
                    const uintptr_t t = tb + (uintptr_t)SCT_STRIDE * k;
                    float v0[3] = { rdF(t+SCT_V0), rdF(t+SCT_V0+4), rdF(t+SCT_V0+8) };
                    float v1[3] = { rdF(t+SCT_V1), rdF(t+SCT_V1+4), rdF(t+SCT_V1+8) };
                    float v2[3] = { rdF(t+SCT_V2), rdF(t+SCT_V2+4), rdF(t+SCT_V2+8) };
                    // The three vertices are read by no instruction in the engine (only the plane
                    // and bounding sphere are), so the offsets are arithmetically forced rather
                    // than code-proven. Validate against the triangle's own bounding sphere and
                    // drop anything inconsistent instead of drawing garbage.
                    float ctr[3] = { rdF(t+0x10), rdF(t+0x14), rdF(t+0x18) };
                    const float rad = rdF(t + 0x1C);
                    if (rad > 0.f && rad < 1e5f) {
                        const float mx = (v0[0]+v1[0]+v2[0])/3.f - ctr[0];
                        const float my = (v0[1]+v1[1]+v2[1])/3.f - ctr[1];
                        const float mz = (v0[2]+v1[2]+v2[2])/3.f - ctr[2];
                        if (mx*mx + my*my + mz*mz > (rad*rad*4.f + 1.f)) { ++g_scBadTri; continue; }
                    }
                    scTri(v0, v1, v2); scTri(v0, v2, v1);
                }
            }
        }
    }
    InterlockedIncrement(&g_scenGen);
}

// --------------------------------------------------------------------------------------------
// Live activity.  PREDICATE_BODY
// --------------------------------------------------------------------------------------------
// Called from the volume-test detour. Must be trivially cheap and non-allocating: it runs on the
// engine thread inside a function the mission scripts poll.
static void noteVolumeTested(int scenarioIndex, int inside)
{
    if ((unsigned)scenarioIndex >= (unsigned)MAX_SCEN_VOLS) return;
    const short slot = g_tvSlotOfIndex[scenarioIndex];
    if (slot < 0 || slot >= MAX_LABELS) return;
    g_tvLastTest[slot] = g_gameTick;                // sampled on the engine tick, no TLS here
    InterlockedIncrement(&g_tvTestHits);
    if (inside && !g_tvFired[slot]) { g_tvFired[slot] = 1; InterlockedIncrement(&g_tvFireHits); }
}

// green = the scripts are testing this volume AND it has not fired since it was last armed.
static bool tvIsActive(int slot)
{
    const long last = g_tvLastTest[slot];
    if (last < 0) return false;                    // never tested since the scenario loaded
    const long now = g_gameTick;
    if (now < 0) return g_ltvActive[slot] != 0;    // no clock: hold whatever we last showed
    if ((now - last) >= LIVE_WINDOW_TICKS) {
        g_tvFired[slot] = 0;                       // gone dormant: re-arm, so a later poll is green
        return false;
    }
    return g_tvFired[slot] == 0;
}

// Re-evaluate every volume's activity. Engine thread only. Cheap: no tag block resolution, just a
// handful of loads per volume, and the geometry is only rebuilt when the SET actually changes.
static void pollLiveTrig()
{
    int on = 0, off = 0; bool changed = false;
    for (int i = 0; i < g_tvLabelN; ++i) {
        const unsigned char a = tvIsActive(i) ? 1u : 0u;
        if (a != g_ltvActive[i]) { g_ltvActive[i] = a; changed = true; }
        if (a) ++on; else ++off;
    }
    g_ltvOnN = on; g_ltvOffN = off;
    if (changed) InterlockedIncrement(&g_ltvGen);
}

static void pollLiveTrigGuarded()
{ __try { pollLiveTrig(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }



// ===================================================================================================
// Live Trigger Volumes and the input-latch probe are NOT ported into HCM.
//
// Both wrote executable memory and patched call sites in the shipping game: the live-volume feature
// allocates 8 stubs and rewrites the rel32 of 8 HaloScript call sites, and the input probe redirects
// the pad-sample eat call. Neither is needed for a collision viewer, both are exactly the pattern HCM
// keeps out of its injected DLL (see the AV-hardening note), and the input one overlaps HCM's own HCE
// input blocking. The STATIC "Trigger Volumes" and "Soft Ceilings" viewers are read-only tag walks
// and are ported unchanged.
//
// These three stay as constants purely so the diagnostic log line below still reads naturally.
// ===================================================================================================
static const int VOL_SITE_N = 0;
static const int g_volPatched = 0;
static const DWORD g_volThread = 0;

static void gatherScenarioGuarded()
{ g_phase = "scenario"; __try { gatherScenario(); } __except (EXCEPTION_EXECUTE_HANDLER) {} g_phase = "idle"; }

// Plain wrappers: __try is illegal in a function that needs C++ object unwinding (the VDB thread
// holds hkArray locals), so the guards live here.
static void gatherGuarded()
{
    // object gather (VDB thread) and world gather (engine thread) share g_allBodies
    if (InterlockedCompareExchange(&g_gatherBusy, 1, 0) != 0) return;
    __try { gatherBodies(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    InterlockedExchange(&g_gatherBusy, 0);
}
static void gatherWorldGuarded()
{
    if (InterlockedCompareExchange(&g_gatherBusy, 1, 0) != 0) return;   // retry next request
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    __try { gatherWorld(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    QueryPerformanceCounter(&t1);
    InterlockedExchange(&g_gatherBusy, 0);
    // This runs INSIDE the engine tick, so its duration is a frame stall. Log it.
    LOG("world walk took %.1f ms ON THE ENGINE THREAD (%d keys) - this is a frame stall",
        1000.0 * double(t1.QuadPart - t0.QuadPart) / double(fq.QuadPart), g_wmKeys);
}

// ===================== line drawers =====================
static inline void DL(HDH* H, int tag, float ax, float ay, float az, float bx, float by, float bz, int color)
{
    hkVector4 a, b; a.set(ax, ay, az); b.set(bx, by, bz);
    H->displayLine(a, b, color, tag);
}

static void drawCoM(HDH* H, int tag)
{
    uintptr_t da = compArray(); if (!da) return;
    uintptr_t comps = (uintptr_t)rdQ(da + DA_DATA);
    int maxc = (int)rdD(da + DA_MAXCOUNT);
    if (!okPtr(comps)) return;
    for (int i = 0; i < maxc; ++i) {
        uintptr_t comp = comps + (uintptr_t)COMP_STRIDE * i;
        uint64_t rbArr = rdQ(comp + COMP_RBARR);
        int32_t rbCnt = (int32_t)rdD(comp + COMP_RBCOUNT);
        if (!okPtr(rbArr) || rbCnt <= 0 || rbCnt > 64) continue;
        for (int k = 0; k < rbCnt; ++k) {
            uintptr_t body = (uintptr_t)rdQ((uintptr_t)rbArr + (uintptr_t)RBREC_STRIDE * k + RBREC_BODY);
            if (!isHkObj(body)) continue;
            float c[3] = { rdF(body + OFF_COM), rdF(body + OFF_COM + 4), rdF(body + OFF_COM + 8) };
            const float s = 0.08f;
            DL(H, tag, c[0]-s,c[1],c[2], c[0]+s,c[1],c[2], hkColor::YELLOW);
            DL(H, tag, c[0],c[1]-s,c[2], c[0],c[1]+s,c[2], hkColor::YELLOW);
            DL(H, tag, c[0],c[1],c[2]-s, c[0],c[1],c[2]+s, hkColor::YELLOW);
        }
    }
}

// island AABB from its member bodies' transforms (entities array offset solved by the probe)
static void drawIslands(HDH* H, int tag)
{
    if (!g_world) return;
    struct { int off; int color; } arrs[2] = {
        { W_ACTIVE_ISLANDS,   hkColor::GREEN },
        { W_INACTIVE_ISLANDS, hkColor::CYAN  },
    };
    for (int a = 0; a < 2; ++a) {
        uint64_t data = rdQ(g_world + arrs[a].off);
        uint32_t n = rdD(g_world + arrs[a].off + 8);
        if (!okPtr(data) || n > 4096) continue;
        for (uint32_t i = 0; i < n; ++i) {
            uintptr_t isl = (uintptr_t)rdQ((uintptr_t)data + 8ull * i);
            if (!okPtr(isl)) continue;
            uint64_t ents = rdQ(isl + ISLAND_ENTITIES);
            uint32_t ec = rdD(isl + ISLAND_ENTITIES + 8);
            if (!okPtr(ents) || ec == 0 || ec > 4096) continue;
            float mn[3] = { 1e9f,1e9f,1e9f }, mx[3] = { -1e9f,-1e9f,-1e9f };
            int used = 0;
            for (uint32_t e = 0; e < ec; ++e) {
                uintptr_t b = (uintptr_t)rdQ((uintptr_t)ents + 8ull * e);
                if (!isHkObj(b)) continue;
                Xform X; if (!bodyXform(b, X)) continue;
                for (int d = 0; d < 3; ++d) { if (X.o[d] < mn[d]) mn[d] = X.o[d]; if (X.o[d] > mx[d]) mx[d] = X.o[d]; }
                ++used;
            }
            if (!used) continue;
            for (int d = 0; d < 3; ++d) { mn[d] -= 0.15f; mx[d] += 0.15f; }
            float p[8][3];
            for (int v = 0; v < 8; ++v) {
                p[v][0] = (v & 1) ? mx[0] : mn[0];
                p[v][1] = (v & 2) ? mx[1] : mn[1];
                p[v][2] = (v & 4) ? mx[2] : mn[2];
            }
            static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            for (int e2 = 0; e2 < 12; ++e2)
                DL(H, tag, p[E[e2][0]][0],p[E[e2][0]][1],p[E[e2][0]][2],
                           p[E[e2][1]][0],p[E[e2][1]][1],p[E[e2][1]][2], arrs[a].color);
        }
    }
}

// plain wrapper again: __try is illegal in a destructor (needs unwinding)
static void removeGeomGuarded(HDH* H, hkUlong id, int tag)
{
    __try { H->removeGeometry(id, tag, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}


// ===================== engine step hook (shell vtable) =====================
// HaloSimulation exports only CreateBlamEngineShell; UE5 drives the engine through that object's
// vtables, so the per-tick entry is a virtual on one of them. Slot 2 of the primary vtable was NOT
// it (ticks stayed 0), so rather than guess again we COUNT every slot of both vtables.
//
// The counting stub is a pure tail-jump, which makes it signature-agnostic and argument-safe:
//     lock inc dword [rip+counter]
//     jmp  qword  [rip+origptr]
// It creates no frame, so register AND stack arguments pass through untouched - important because
// these slots have varying prototypes (some take doubles in XMM).

static bool patchSlot(uintptr_t slotAddr, void* newVal, uint64_t* oldOut)
{
    uint64_t cur = rdQ(slotAddr);
    if (!inMod((uintptr_t)cur)) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)slotAddr, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (oldOut) *oldOut = cur;
    *(void**)slotAddr = newVal;
    VirtualProtect((void*)slotAddr, sizeof(void*), old, &old);
    return true;
}



// ---- the real per-tick detour -------------------------------------------------------------
// Measured: vtable2 slot 0 (sub_18000E670) fires ~75/s; every other slot of both vtables is dead.
// Signature confirmed by decompile: void __fastcall(this, double dt)  -> RCX + XMM1.
typedef void (__fastcall *FnTick)(void*);   // NO dt: all 5 call sites set only RCX
static FnTick g_origTick = nullptr;
static uintptr_t g_tickSlot = 0;
// CONTENT fingerprint of the static level: for every type-7 root shape, its structure-BSP index
// AND the engine's own surface counts. Identity alone is not enough - shape pointers and bspIdx are
// reused across zone streaming, so a pointer-based hash never changed and the world went stale.
// Surface counts DO change when a zone swaps. Engine thread only: bspSurfaceCounts does a tag lookup.
static uint64_t worldContentSig()
{
    auto scramble = [](uint64_t v) {
        v ^= v >> 33; v *= 0xFF51AFD7ED558CCDull;
        v ^= v >> 33; v *= 0xC4CEB9FE1A85EC53ull;
        return v ^ (v >> 33);
    };
    // A type-7 container is almost never the body's ROOT shape - bodies wrap it in a MOPP (10)
    // and/or a list (9). Scanning roots found nothing, so the signature was always 0 and the check
    // silently never fired. Descend through wrapper shapes, stopping at the first type 7.
    uint64_t acc = 0;
    int found = 0;
    for (int i = 0; i < g_allN; ++i) {
        uintptr_t stack[32]; int sp = 0;
        stack[sp++] = (uintptr_t)rdQ(g_allBodies[i] + OFF_SHAPE);
        int steps = 0;
        while (sp > 0 && ++steps < 64) {
            const uintptr_t sh = stack[--sp];
            if (!isHkObj(sh)) continue;
            const uint32_t ty = rdD(sh + SHAPE_TYPE);
            if (ty == 7) {                                  // a level-mesh container: fingerprint it
                int sm = -1, lg = -1;
                bspSurfaceCounts(sh, sm, lg);
                acc += scramble(rdD(sh + 0x60)) ^
                       scramble(((uint64_t)(uint32_t)sm << 32) | (uint32_t)lg);
                ++found;
                continue;                                   // do NOT descend into it
            }
            if (sp >= 30) continue;
            if (ty == 10) { stack[sp++] = (uintptr_t)rdQ(sh + BV_CHILD); continue; }
            if (ty == 11 || ty == 12 || ty == 28 || ty == 30 || ty == 15) {
                stack[sp++] = (uintptr_t)rdQ(sh + XF_CHILD); continue;
            }
            if (ty == 9) {                                   // list: children array @ +0x30
                const uintptr_t arr = (uintptr_t)rdQ(sh + LIST_ARR);
                const int n = (int)rdD(sh + LIST_ARR + 8);
                if (!okPtr(arr) || n <= 0 || n > 256) continue;
                for (int k = 0; k < n && sp < 30; ++k)
                    stack[sp++] = (uintptr_t)rdQ(arr + 32ull * k);
            }
        }
    }
    return found ? acc : 0;
}
static uint64_t worldContentSigGuarded()
{ __try { return worldContentSig(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static void __fastcall tickDetour(void* a1)
{
    // ⚠ THIS FUNCTION IS AN ENTRY POINT INTO HCM'S IMAGE, AND IT WAS THE ONE NOTHING WAS WAITING FOR.
    //
    // uninstallTickHook() restores the vtable slot, and its comment correctly notes that a thread which already
    // loaded the detour address will still enter here afterwards. That is harmless while HCM stays loaded. It is
    // fatal when HCM is about to UNLOAD: the thread calls an address that no longer exists.
    //
    // MEASURED. Closing HCM produced an unhandled access violation whose faulting address resolved, through the
    // PDB, to exactly this function's first instruction:
    //
    //     EXCEPTION_ACCESS_VIOLATION 0x00007FFD7D288AA0
    //     = HCMInternal.dll (base 0x7FFD7C960000) + 0x928AA0
    //     = `anonymous namespace'::tickDetour + 0x0
    //
    // ImageResidency::drain() in dllmain already waits for HCM's other entry points before unmapping - the window
    // procedure and the ModuleHookManager hooks. This one is a RAW VTABLE SLOT patched by hand, so it was never
    // in that set and the drain reported "clean" while a game thread was still on its way in. Registering here
    // puts it under the same guard: unmapping now blocks until this returns.
    //
    // Via the shim rather than ScopedImageResidency directly - this TU has no PCH and must not pull one in.
    struct ResidencyScope
    {
        ResidencyScope() noexcept { ImageResidency::enterFromForeignTU(); }
        ~ResidencyScope() noexcept { ImageResidency::leaveFromForeignTU(); }
    } residency;

    if (g_origTick) g_origTick(a1);              // engine first: world is post-step, TLS live
    // HCM bridge: the engine call above is ALWAYS made (we are standing in the engine's own vtable slot
    // and skipping it would freeze the simulation). Only our own work is paused.
    if (g_paused) return;
    const long n = InterlockedIncrement(&g_ticks);
    if (InterlockedCompareExchange(&g_wantWorldGather, 0, 1) == 1) { gatherWorldGuarded(); gatherScenarioGuarded(); return; }
    {   // sample the script clock; while it is stopped (pause) the live state simply holds
        const int gt = readGameTick();
        if (gt >= 0) g_gameTick = gt; else InterlockedIncrement(&g_gameTickBad);
    }
    // ~every 1.5 s, cheaply re-check the level's content and re-walk only if it actually changed
    if ((n % 120) == 0 && g_ready && g_allN > 0) {
        const uint64_t sig = worldContentSigGuarded();
        if (sig && sig != g_lastWorldSig) {
            LOG("level content changed (sig %016llX -> %016llX) - re-walking",
                (unsigned long long)g_lastWorldSig, (unsigned long long)sig);
            g_lastWorldSig = sig;
            gatherWorldGuarded();
            gatherScenarioGuarded();
        }
    }
}

// ====================================================================================================================
// THE FORWARDING THUNK - why the vtable never points at HCM's own code.
//
// ⚠ AN ADDRESS PUBLISHED INTO THE GAME MUST OUTLIVE HCM. THIS ONE DID NOT, AND IT WAS THE LAST CRASH.
//
// Patching the vtable slot to &tickDetour hands the engine a pointer into HCMInternal.dll. uninstallTickHook()
// restores the slot, but a thread that has ALREADY LOADED the old value still calls it afterwards - the existing
// comment says so. While HCM stays loaded that is harmless. On unload it is fatal.
//
// MEASURED TWICE, resolved through the PDB both times, and the second one is the proof that guarding was not
// enough. Between the two runs tickDetour moved 0x1A0 bytes (a ScopedImageResidency was added to it) - and the
// faulting address moved by exactly 0x1A0:
//
//     build A:  fault 0x7FFD7D288AA0  = HCMInternal + 0x928AA0 = tickDetour + 0x0
//     build B:  fault 0x7FFD7D288C40  = HCMInternal + 0x928C40 = tickDetour + 0x0
//
// ImageResidency only covers a thread that is INSIDE the function. The thread here has loaded the pointer and not
// yet executed its first instruction, so the drain sees nothing, reports clean, HCM unmaps, and the call lands in
// a hole. NOTHING SYNCHRONISED CAN CLOSE THAT WINDOW - the gap is between a load and a call, with no code of ours
// running in between to be waited on. The address itself has to stay valid forever.
//
// So the slot gets a permanently-allocated thunk instead, and never sees HCM's address at all:
//
//     jmp qword ptr [rip+2]      ; 6 bytes
//     <2 bytes padding>          ; aligns the target
//     <8-byte target pointer>    ; &tickDetour while loaded, g_origTick after uninstall
//
// The page is never freed - same deliberate leak as HCETriggerActivity's stub page, and for the same reason: a
// game thread can be inside it, or on its way into it, at any instant. Uninstall becomes an aligned 8-byte store
// that redirects the thunk to the engine's own function, so a caller arriving at any time - before, during or
// long after HCM is gone - lands somewhere real.
// ====================================================================================================================
static void*  g_tickThunk       = nullptr;   // executable page, NEVER freed
static void** g_tickThunkTarget = nullptr;   // 8-byte aligned slot inside it

static bool buildTickThunk()
{
    if (g_tickThunk) return true;

    // RWX and leaked, deliberately. The target slot lives in the same page so it can be swapped with a single
    // aligned store while the page stays executable - splitting code and data would need the two allocations
    // within rip-relative range of each other, which VirtualAlloc does not promise.
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) { LOG("tick hook: could not allocate the forwarding thunk"); return false; }

    unsigned char* p = (unsigned char*)page;
    p[0] = 0xFF; p[1] = 0x25;                       // jmp qword ptr [rip+disp32]
    p[2] = 0x02; p[3] = 0x00; p[4] = 0x00; p[5] = 0x00;   // disp32 = 2 -> target at offset 8
    p[6] = 0x00; p[7] = 0x00;                       // padding, so the target is 8-byte aligned

    g_tickThunkTarget = (void**)(p + 8);
    *g_tickThunkTarget = (void*)&tickDetour;

    FlushInstructionCache(GetCurrentProcess(), page, 16);
    g_tickThunk = page;
    LOG("tick hook: forwarding thunk at %016llX (never freed)", (unsigned long long)(uintptr_t)page);
    return true;
}

static bool installTickHook()
{
    g_tickSlot = A(A_VT2);                       // slot 0 of the second vtable (signature-resolved)
    if (!g_tickSlot) { LOG("tick hook: shell vtable2 unresolved - REFUSING to patch"); return false; }
    uint64_t orig = rdQ(g_tickSlot);
    if (!inMod((uintptr_t)orig)) { LOG("tick hook: slot 0 of vt2 is not a module ptr"); return false; }
    g_origTick = (FnTick)orig;                   // publish BEFORE patching
    _ReadWriteBarrier();

    if (!buildTickThunk()) return false;
    *g_tickThunkTarget = (void*)&tickDetour;     // re-arm, in case this is a second enable
    _ReadWriteBarrier();

    // The THUNK goes in the slot, never &tickDetour. See the block above.
    if (!patchSlot(g_tickSlot, g_tickThunk, nullptr)) return false;
    LOG("tick hook installed on vt2 slot 0 via thunk (orig %016llX)", (unsigned long long)orig);
    return true;
}

// Restore the vtable slot. HCM's stop() contract requires the hook to come off; the standalone never
// uninstalled. A single aligned pointer store is atomic, so a call already in flight just completes.
//
// g_origTick is deliberately NOT cleared. A thread that already loaded the detour address out of the
// vtable will still enter tickDetour after the restore, and `if (g_origTick) g_origTick(a1)` has to
// keep forwarding - nulling it would silently DROP a whole simulation step of the game.
static void uninstallTickHook()
{
    if (!g_tickSlot || !g_origTick) return;

    // ⚠ ORDER MATTERS, AND THE THUNK REDIRECT MUST COME FIRST.
    //
    // Point the (permanent) thunk at the engine's own function before touching the vtable. After this store, a
    // thread arriving at the thunk from ANY direction - one that read the slot a moment ago, one still to read it,
    // one that will not get there until long after HCM has unmapped - jumps straight to the engine and never
    // reaches our code. An aligned pointer store is atomic, so there is no torn state to observe.
    //
    // Only then is the slot restored, which is now merely tidiness: the thunk is already inert and would forward
    // correctly forever even if this failed.
    if (g_tickThunkTarget)
    {
        *g_tickThunkTarget = (void*)g_origTick;
        _ReadWriteBarrier();
    }

    patchSlot(g_tickSlot, (void*)g_origTick, nullptr);
    LOG("tick hook uninstalled (thunk redirected to the engine; page intentionally left mapped)");
    g_tickSlot = 0;
}



// ===================== viewer processes =====================
enum { CAT_OBJECTS, CAT_WORLD, CAT_COM, CAT_ISLANDS, CAT_TRIGVOL, CAT_SOFTCEIL,
       CAT_LIVETRIG, CAT_COUNT };
static int s_tag[CAT_COUNT];
// Distinct ranges: OBJ occupies OBJ_GEOM_ID .. +BK_COUNT-1, so it must not reach WORLD_GEOM_ID.
static const hkUlong OBJ_GEOM_ID   = 0x11CE0100ull;
static const hkUlong WORLD_GEOM_ID   = 0x11CE1000ull;   // .. +2048 (chunks)
static const hkUlong TRIGVOL_GEOM_ID = 0x11CE2000ull;
static const hkUlong SOFTCEIL_GEOM_ID= 0x11CE2001ull;
static const hkUlong LIVETRIG_ON_ID   = 0x11CE2002ull;   // active volumes   (green)
static const hkUlong LIVETRIG_OFF_ID  = 0x11CE2003ull;   // inactive volumes (red)

// hkColor is 0xAARRGGBB in host order (hkColor.cpp), and alpha is written through untouched by
// hkServerDebugDisplayHandler on every path - geometry, lines and 3d text alike.
static const unsigned COL_TRIGVOL_FACE = 0x33FF0000u;   // red    20%
static const unsigned COL_TRIGVOL_EDGE = 0xFFFF0000u;   // red   100%
static const unsigned COL_TRIGVOL_TEXT = 0xBFFFFFFFu;   // white  75%
static const unsigned COL_SOFTCEIL     = 0x73FFFF00u;   // yellow 45%
static const unsigned COL_LIVE_ON_FACE = 0x3300FF00u;   // green  20%
static const unsigned COL_LIVE_ON_EDGE = 0xFF00FF00u;   // green 100%
static const unsigned COL_LIVE_OFFFACE = 0x33FF0000u;   // red    20%
static const unsigned COL_LIVE_OFFEDGE = 0xFFFF0000u;   // red   100%

class CatProcess : public hkProcess
{
public:
    int m_cat;
    hkBool m_added;
    long m_objGen, m_worldGen;   // generations this client's geometry reflects
    int  m_worldChunks;          // highest world chunk id this client has been sent
    int  m_worldSent;            // how many chunks have been pushed so far (paced)
    long m_scenGen;              // scenario generation this client's volumes reflect
    CatProcess(int c) : hkProcess(true), m_cat(c), m_added(false), m_objGen(-1), m_worldGen(-1), m_worldChunks(0), m_worldSent(0), m_scenGen(-1) {}

    // Unticking a viewer in the HVDB client DESTROYS the process - so geometry we added must be
    // removed here, otherwise the mesh stays on screen forever and the toggle appears to do
    // nothing. (Line-drawn viewers re-emit per frame and need no cleanup.)
    virtual ~CatProcess()
    {
        if (!m_added || !m_displayHandler) return;
        if (m_cat == CAT_LIVETRIG) {
            removeGeomGuarded(m_displayHandler, LIVETRIG_ON_ID,  s_tag[m_cat]);
            removeGeomGuarded(m_displayHandler, LIVETRIG_OFF_ID, s_tag[m_cat]);
        }
        else if (m_cat == CAT_TRIGVOL)  removeGeomGuarded(m_displayHandler, TRIGVOL_GEOM_ID, s_tag[m_cat]);
        else if (m_cat == CAT_SOFTCEIL) removeGeomGuarded(m_displayHandler, SOFTCEIL_GEOM_ID, s_tag[m_cat]);
        else if (m_cat == CAT_WORLD) { for (int c = 0; c < m_worldChunks; ++c)
                 removeGeomGuarded(m_displayHandler, WORLD_GEOM_ID + c, s_tag[m_cat]); }
        else for (int b = 0; b < BK_COUNT; ++b)
                 removeGeomGuarded(m_displayHandler, OBJ_GEOM_ID + b, s_tag[m_cat]);
        m_added = false;
    }
    virtual int getProcessTag() { return s_tag[m_cat]; }

    // The gather runs on the VDB thread loop, NOT here - so the stats stay meaningful (and the
    // offsets stay testable) even with no client attached. This only publishes the result.
    void stepObjects(HDH* H, int tag)
    {
        if (!g_ready) return;
        if (m_added && m_objGen == g_objGen) return;   // nothing changed - do not re-send
        m_objGen = g_objGen;
        // Remove EVERY bucket id, not just the first: the add path emits BK_COUNT geometries, so
        // dropping only OBJ_GEOM_ID left buckets 1..2 accumulating a copy per frame (the capsule
        // bucket is the player, which trailed a "worm" of past positions behind them).
        if (m_added) {
            for (int b = 0; b < BK_COUNT; ++b) H->removeGeometry(OBJ_GEOM_ID + b, tag, 0);
            m_added = false;
        }
        // Per-type buckets are kept wired up: flip DEBUG_TYPE_COLORS to tint convex/box/round
        // separately and any future rendering defect names its own emitter by colour.
        // Per-type buckets stay wired up: set DEBUG_TYPE_COLORS to 1 and convex/box/round are
        // tinted separately, so any future rendering defect names its own emitter by colour.
        static const unsigned kDbg[BK_COUNT] = { 0xFF40C0FF, 0xFF40E060, 0xFFFFA030 };
        static const unsigned kUni[BK_COUNT] = { 0xFF40C0FF, 0xFF40C0FF, 0xFF40C0FF };
        const unsigned* kCol = DEBUG_TYPE_COLORS ? kDbg : kUni;
        bool any = false;
        for (int b = 0; b < BK_COUNT; ++b) {
            if (!g_objBucket[b] || g_objBucket[b]->m_triangles.getSize() == 0) continue;
            hkArray<hkDisplayGeometry*> geoms;
            hkDisplayConvex dc(g_objBucket[b]);
            geoms.pushBack(&dc);
            hkTransform tr; tr.setIdentity();
            H->addGeometry(geoms, tr, OBJ_GEOM_ID + b, tag, 0);
            H->setGeometryColor(kCol[b], OBJ_GEOM_ID + b, tag);
            dc.m_geometry = HK_NULL;   // geometry is ours; don't let the wrapper free it
            any = true;
        }
        m_added = any;
    }

    void stepWorld(HDH* H, int tag)
    {
        if (!g_ready) return;
        // PACE THE SEND. Pushing all ~400 chunks inside one vdb->step() dumps hundreds of MB of
        // serialised geometry into the VDB stream in a single frame; whatever the socket/command
        // buffer cannot take is silently lost, which is what produced scattered holes. Even the
        // base-BSP-only case was ~40 MB in one step. Object collision (<1 MB) never had holes.
        // The level is static, so spreading the upload over a few seconds costs nothing.
        if (m_worldGen != g_worldGeoGen) {          // new build of the world mesh -> restart upload
            for (int c = 0; c < m_worldChunks; ++c) H->removeGeometry(WORLD_GEOM_ID + c, tag, 0);
            m_worldGen = g_worldGeoGen;
            m_worldChunks = 0;
            m_worldSent = 0;
            m_added = false;
        }
        if (m_worldSent >= g_worldChunkCount) return;   // fully uploaded, nothing to do

        // Send the whole mesh in one step. Pacing was added to test a per-step-burst theory for the
        // holes; that theory was wrong (the holes were an N-gon decode bug), so pacing only cost
        // ~10 s of latency. The upload is a one-off per level, so a single frame's work is fine.
        while (m_worldSent < g_worldChunkCount) {
            const int c = m_worldSent++;
            hkGeometry* g = g_worldChunk[c];
            if (!g || g->m_triangles.getSize() == 0) continue;
            hkArray<hkDisplayGeometry*> geoms;
            hkDisplayConvex dc(g);
            geoms.pushBack(&dc);
            hkTransform tr; tr.setIdentity();
            H->addGeometry(geoms, tr, WORLD_GEOM_ID + c, tag, 0);
            H->setGeometryColor(0xFF909090, WORLD_GEOM_ID + c, tag);   // grey = static world
            dc.m_geometry = HK_NULL;
            if (c + 1 > m_worldChunks) m_worldChunks = c + 1;
            m_added = true;
        }
        if (m_worldSent >= g_worldChunkCount)
            LOG("world upload complete: %d/%d chunks pushed", m_worldSent, g_worldChunkCount);
    }

    // Trigger volumes: translucent skin as persistent geometry (only re-sent when the scenario
    // changes) + opaque edges and the name label as immediate-mode calls every step. There is no
    // wireframe display type and no per-triangle colour in this protocol, so this is the only
    // construction that gives a see-through volume with solid outlines.
    void stepTrigVol(HDH* H, int tag)
    {
        if (!g_ready) return;
        if (m_scenGen != g_scenGeoGen) {
            if (m_added) { H->removeGeometry(TRIGVOL_GEOM_ID, tag, 0); m_added = false; }
            m_scenGen = g_scenGeoGen;
            if (g_tvGeo && g_tvGeo->m_triangles.getSize()) {
                hkArray<hkDisplayGeometry*> geoms;
                hkDisplayConvex dc(g_tvGeo);
                geoms.pushBack(&dc);
                hkTransform tr; tr.setIdentity();
                H->addGeometry(geoms, tr, TRIGVOL_GEOM_ID, tag, 0);
                H->setGeometryColor((int)COL_TRIGVOL_FACE, TRIGVOL_GEOM_ID, tag);
                dc.m_geometry = HK_NULL;
                m_added = true;
            }
        }
        for (int i = 0; i < g_tvEdgeN; ++i) {
            const float* e = g_tvEdge[i];
            DL(H, tag, e[0],e[1],e[2], e[3],e[4],e[5], (int)COL_TRIGVOL_EDGE);
        }
        for (int i = 0; i < g_tvLabelN; ++i) {
            hkVector4 p; p.set(g_tvLabel[i].ctr[0], g_tvLabel[i].ctr[1], g_tvLabel[i].ctr[2]);
            H->display3dText(g_tvLabel[i].name, p, (int)COL_TRIGVOL_TEXT, tag);
        }
    }

    void stepSoftCeil(HDH* H, int tag)
    {
        if (!g_ready) return;
        if (m_scenGen == g_scenGeoGen) return;
        if (m_added) { H->removeGeometry(SOFTCEIL_GEOM_ID, tag, 0); m_added = false; }
        m_scenGen = g_scenGeoGen;
        if (!g_scGeo || g_scGeo->m_triangles.getSize() == 0) return;
        hkArray<hkDisplayGeometry*> geoms;
        hkDisplayConvex dc(g_scGeo);
        geoms.pushBack(&dc);
        hkTransform tr; tr.setIdentity();
        H->addGeometry(geoms, tr, SOFTCEIL_GEOM_ID, tag, 0);
        H->setGeometryColor((int)COL_SOFTCEIL, SOFTCEIL_GEOM_ID, tag);
        dc.m_geometry = HK_NULL;
        m_added = true;
    }

    // Two geometries rather than one: setGeometryColor tints a whole geometry, so green/red per
    // volume means splitting the triangles into an active set and an inactive set. Edges are
    // immediate-mode, so those carry the exact per-volume colour with no batching at all.
    void stepLiveTrig(HDH* H, int tag)
    {
        if (!g_ready) return;
        if (m_scenGen != g_liveGeoGen) {
            if (m_added) {
                H->removeGeometry(LIVETRIG_ON_ID,  tag, 0);
                H->removeGeometry(LIVETRIG_OFF_ID, tag, 0);
                m_added = false;
            }
            m_scenGen = g_liveGeoGen;
            hkTransform tr; tr.setIdentity();
            if (g_ltvOnGeo && g_ltvOnGeo->m_triangles.getSize()) {
                hkArray<hkDisplayGeometry*> g; hkDisplayConvex dc(g_ltvOnGeo); g.pushBack(&dc);
                H->addGeometry(g, tr, LIVETRIG_ON_ID, tag, 0);
                H->setGeometryColor((int)COL_LIVE_ON_FACE, LIVETRIG_ON_ID, tag);
                dc.m_geometry = HK_NULL; m_added = true;
            }
            if (g_ltvOffGeo && g_ltvOffGeo->m_triangles.getSize()) {
                hkArray<hkDisplayGeometry*> g; hkDisplayConvex dc(g_ltvOffGeo); g.pushBack(&dc);
                H->addGeometry(g, tr, LIVETRIG_OFF_ID, tag, 0);
                H->setGeometryColor((int)COL_LIVE_OFFFACE, LIVETRIG_OFF_ID, tag);
                dc.m_geometry = HK_NULL; m_added = true;
            }
        }
        for (int i = 0; i < g_tvLabelN; ++i) {
            const bool on = g_ltvActive[i] != 0;
            const int  c  = (int)(on ? COL_LIVE_ON_EDGE : COL_LIVE_OFFEDGE);
            const TvRange& R = g_tvRange[i];
            for (int k = 0; k < R.edgeCount; ++k) {
                const float* e = g_tvEdge[R.edgeStart + k];
                DL(H, tag, e[0],e[1],e[2], e[3],e[4],e[5], c);
            }
            hkVector4 p; p.set(g_tvLabel[i].ctr[0], g_tvLabel[i].ctr[1], g_tvLabel[i].ctr[2]);
            H->display3dText(g_tvLabel[i].name, p, c, tag);
        }
    }

    virtual void step(hkReal)
    {
        HDH* H = m_displayHandler; if (!H) return;
        if (g_paused) return;
        int tag = getProcessTag();
        __try {
            switch (m_cat) {
            case CAT_OBJECTS:  stepObjects(H, tag);  break;
            case CAT_WORLD:    stepWorld(H, tag);    break;
            case CAT_COM:      drawCoM(H, tag);      break;
            case CAT_ISLANDS:  drawIslands(H, tag);  break;
            case CAT_TRIGVOL:  stepTrigVol(H, tag);  break;
            case CAT_SOFTCEIL: stepSoftCeil(H, tag); break;
            case CAT_LIVETRIG: stepLiveTrig(H, tag); break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
};

static hkProcess* HK_CALL crObjects(const hkArray<hkProcessContext*>&) { return new CatProcess(CAT_OBJECTS); }
static hkProcess* HK_CALL crWorld  (const hkArray<hkProcessContext*>&) { return new CatProcess(CAT_WORLD); }
static hkProcess* HK_CALL crTrigVol(const hkArray<hkProcessContext*>&) { return new CatProcess(CAT_TRIGVOL); }
static hkProcess* HK_CALL crSoftCeil(const hkArray<hkProcessContext*>&){ return new CatProcess(CAT_SOFTCEIL); }
static hkProcess* HK_CALL crLiveTrig(const hkArray<hkProcessContext*>&){ return new CatProcess(CAT_LIVETRIG); }
static hkProcess* HK_CALL crCoM    (const hkArray<hkProcessContext*>&) { return new CatProcess(CAT_COM); }
static hkProcess* HK_CALL crIslands(const hkArray<hkProcessContext*>&) { return new CatProcess(CAT_ISLANDS); }

static void HK_CALL errorReport(const char* msg, void*) { LOG("havok: %s", msg); }


// ===================== module/base resolution (psapi-free) =====================
static bool resolveModule()
{
    HMODULE sim = GetModuleHandleA(MODNAME);
    if (!sim) return false;
    uintptr_t base = (uintptr_t)sim;
    DWORD imageSize = 0;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        imageSize = nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!imageSize) return false;
    g_base = base;
    g_end  = base + imageSize;
    return true;
}

// ===================== thread =====================
static volatile long g_threadStarted = 0;

// ⚠ TEARDOWN. The standalone's VDB thread ran forever and was never meant to stop; inside HCM that is a
// CRASH, not a leak. HCMInternal.dll is FreeLibrary'd when HCM closes, and unmapping the image while this
// thread is executing code inside it is an unconditional access violation - every HCM close after using the
// debugger. It also left 127.0.0.1:25001 listening, so toggling the feature off never disconnected the HVD
// client and a re-enable collided on the port.
//
// So the thread now has a real exit: an exit flag it polls, the hkVisualDebugger deleted on the way out
// (which closes the socket), and a handle the bridge can wait on.
static volatile long g_vdbShouldExit = 0;
static HANDLE        g_vdbThreadHandle = nullptr;

// ⚠ SEPARATE FUNCTION ON PURPOSE. vdbThread holds objects that require unwinding (hkArray, and everything
// Havok constructs on its stack), and MSVC refuses __try in such a function - C2712. Same shape as
// worldContentSigGuarded above. Deleting the visual debugger is what closes the listening socket, and it runs
// arbitrary Havok teardown, so it is worth guarding.
static void deleteVdbGuarded(hkVisualDebugger* vdb)
{
    __try { delete vdb; } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static DWORD WINAPI vdbThread(LPVOID)
{
    // Log next to HCMInternal.dll under our OWN name - never HCMInternal.log, which plog owns.
    {
        char path[MAX_PATH] = { 0 };
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&vdbThread, &self);
        if (GetModuleFileNameA(self, path, MAX_PATH)) {
            char* sl = strrchr(path, '\\');
            if (sl) { sl[1] = 0; strcat_s(path, MAX_PATH, "HCM_HCEHavokDebugger.log"); }
            else    { strcpy_s(path, MAX_PATH, "HCM_HCEHavokDebugger.log"); }
            g_log = _fsopen(path, "w", _SH_DENYWR);
        }
    }
    LOG("HCE Havok Debugger starting, pid=%lu  BUILD %s %s", GetCurrentProcessId(), __DATE__, __TIME__);
    LOG("%s @ %016llX .. %016llX", MODNAME, (unsigned long long)g_base, (unsigned long long)g_end);

    hkPoolMemory* mem = new hkPoolMemory();
    hkThreadMemory* tm = new hkThreadMemory(mem, 16);
    hkBaseSystem::init(mem, tm, errorReport);
    int ss = 0x100000;
    char* sb = hkAllocate<char>(ss, HK_MEMORY_CLASS_BASE);
    hkThreadMemory::getInstance().setStackArea(sb, ss);

    g_objGeo = new hkGeometry();
    for (int b = 0; b < BK_COUNT; ++b) g_objBucket[b] = new hkGeometry();

    // RESERVE, commit on demand. The standalone committed 91 MB of scratch on load and never freed it;
    // inside HCM that is charged to the game process on the very first toggle, for buffers a small
    // level never touches. g_wmTri already worked this way (wmEnsure); the rest now match.
    g_wmTri     = (float(*)[9])VirtualAlloc(nullptr, (size_t)MAX_WORLD_TRIS * WM_TRI_BYTES, MEM_RESERVE, PAGE_READWRITE);
    g_tvTri     = (float(*)[9])VirtualAlloc(nullptr, (size_t)MAX_SCEN_TRIS * 9 * sizeof(float), MEM_RESERVE, PAGE_READWRITE);
    g_scTri     = (float(*)[9])VirtualAlloc(nullptr, (size_t)MAX_SCEN_TRIS * 9 * sizeof(float), MEM_RESERVE, PAGE_READWRITE);
    g_tvEdge    = (float(*)[6])VirtualAlloc(nullptr, (size_t)MAX_SCEN_EDGES * 6 * sizeof(float), MEM_RESERVE, PAGE_READWRITE);
    // ⚠ These two have no ensure helper and no chunked writer to hang one off, so they are COMMITTED here.
    // Reserving them without a commit path is exactly the defect that broke the scenario buffers - address space
    // that faults on first write. They are the two smallest reservations, so committing up front costs little and
    // is the honest option rather than leaving a third landmine.
    g_wmMeshTri = (float(*)[9])VirtualAlloc(nullptr, (size_t)MAX_MESH_TRIS * WM_TRI_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_wmInst    = (WmInst*)   VirtualAlloc(nullptr, sizeof(WmInst) * WM_MAX_INST, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    // The three scenario buffers commit lazily through scratchEnsure. Publish their reservations to it.
    g_tvTriCommit  = { g_tvTri,  0, (size_t)MAX_SCEN_TRIS  * 9 * sizeof(float), false, "trigger volume tris" };
    g_tvEdgeCommit = { g_tvEdge, 0, (size_t)MAX_SCEN_EDGES * 6 * sizeof(float), false, "trigger volume edges" };
    g_scTriCommit  = { g_scTri,  0, (size_t)MAX_SCEN_TRIS  * 9 * sizeof(float), false, "soft ceiling tris" };
    LOG("scratch reserved: world %zu MB, scenario tris 2x%zu MB, edges %zu MB, mesh %zu MB (committed on demand)",
        ((size_t)MAX_WORLD_TRIS * WM_TRI_BYTES) >> 20,
        ((size_t)MAX_SCEN_TRIS * 9 * sizeof(float)) >> 20,
        ((size_t)MAX_SCEN_EDGES * 6 * sizeof(float)) >> 20,
        ((size_t)MAX_MESH_TRIS * WM_TRI_BYTES) >> 20);

    g_ltvOnGeo  = new hkGeometry();
    g_ltvOffGeo = new hkGeometry();
    g_worldGeo  = new hkGeometry();
    g_tvGeo     = new hkGeometry();
    g_scGeo     = new hkGeometry();

    hkProcessFactory& f = hkProcessFactory::getInstance();
    s_tag[CAT_OBJECTS] = f.registerProcess("Object Collision", crObjects);
    s_tag[CAT_WORLD]   = f.registerProcess("World Collision",  crWorld);
    s_tag[CAT_COM]     = f.registerProcess("Center of Mass",   crCoM);
    s_tag[CAT_ISLANDS] = f.registerProcess("Havok Islands",    crIslands);
    s_tag[CAT_TRIGVOL] = f.registerProcess("Trigger Volumes",  crTrigVol);
    s_tag[CAT_SOFTCEIL]= f.registerProcess("Soft Ceilings",    crSoftCeil);
    // "Live Trigger Volumes" is deliberately NOT registered - see the stub block above.

    hkArray<hkProcessContext*> contexts;
    hkVisualDebugger* vdb = new hkVisualDebugger(contexts);
    vdb->addDefaultProcess("Object Collision");
    vdb->addDefaultProcess("World Collision");
    vdb->serve(25001);
    LOG("VDB serving on 127.0.0.1:25001 - connect the Havok Visual Debugger client");

    g_ready = true;
    int frame = 0;
    for (;;) {
        // Checked FIRST, before any gather, so an exit request is honoured even while the debugger is busy.
        if (InterlockedCompareExchange(&g_vdbShouldExit, 0, 1) == 1)
        {
            LOG("VDB thread exit requested - shutting the server down");
            g_ready = false;
            // Deleting the visual debugger is what actually closes the listening socket and drops any
            // connected HVD client. Without it the port stays bound and a re-enable fails on serve().
            deleteVdbGuarded(vdb);
            vdb = nullptr;
            LOG("VDB thread exiting cleanly");
            if (g_log) { fflush(g_log); }
            // Deliberately NOT tearing down hkBaseSystem or the geometry/scratch allocations: they are
            // shared with the gather paths and a re-enable reuses them. This thread's job is to stop
            // touching the game and release the socket, nothing more.
            InterlockedExchange(&g_threadStarted, 0);   // allow a later start() to spin a fresh thread
            return 0;
        }

        if (!g_paused) {
            if (frame % 4 == 0) gatherGuarded();      // dynamic objects move every frame
            // The world mesh must be walked on the ENGINE thread (getChildShape needs Blam's TLS), so
            // ask the tick hook to do it and convert its plain buffer into hkGeometry here (Havok's
            // allocator is thread-local and belongs to this thread).
            if (g_wantFirstWorld) { g_wantFirstWorld = false; InterlockedExchange(&g_wantWorldGather, 1); }

            if (g_scenGen != g_scenGeoGen) {
                g_scenGeoGen = g_scenGen;
                auto fill = [](hkGeometry* g, float (*src)[9], int n) {
                    if (!g) return;
                    g->m_vertices.clear(); g->m_triangles.clear();
                    for (int i = 0; i < n; ++i) {
                        const float* t = src[i];
                        int b = g->m_vertices.getSize();
                        hkVector4 v;
                        v.set(t[0],t[1],t[2]); g->m_vertices.pushBack(v);
                        v.set(t[3],t[4],t[5]); g->m_vertices.pushBack(v);
                        v.set(t[6],t[7],t[8]); g->m_vertices.pushBack(v);
                        hkGeometry::Triangle tr; tr.set(b, b+1, b+2);
                        g->m_triangles.pushBack(tr);
                    }
                };
                fill(g_tvGeo, g_tvTri, g_tvTriN);
                fill(g_scGeo, g_scTri, g_scTriN);
                LOG("scenario: %d trigger volumes (%d sector, %d box) -> %d tris %d edges %d labels | "
                    "soft ceilings %d -> %d tris (rejected %d)",
                    g_tvCount, g_tvSector, g_tvBox, g_tvTriN, g_tvEdgeN, g_tvLabelN,
                    g_scCount, g_scTriN, g_scBadTri);
            }

            if (g_wmGen != g_worldGeoGen) {
                g_worldGeoGen = g_wmGen;
                const long n = g_wmTriCount;
                int chunks = (int)((n + WM_TRIS_PER_CHUNK - 1) / WM_TRIS_PER_CHUNK);
                if (chunks > WM_MAX_CHUNKS) {
                    LOG("!! world needs %d chunks, capped at %d", chunks, WM_MAX_CHUNKS);
                    chunks = WM_MAX_CHUNKS;
                }
                for (int c = 0; c < chunks; ++c) {
                    if (!g_worldChunk[c]) g_worldChunk[c] = new hkGeometry();
                    hkGeometry* g = g_worldChunk[c];
                    g->m_vertices.clear();
                    g->m_triangles.clear();
                    const long lo = (long)c * WM_TRIS_PER_CHUNK;
                    const long hi = (lo + WM_TRIS_PER_CHUNK < n) ? lo + WM_TRIS_PER_CHUNK : n;
                    for (long i = lo; i < hi; ++i) {
                        const float* t = g_wmTri[i];
                        int b = g->m_vertices.getSize();
                        hkVector4 v;
                        v.set(t[0],t[1],t[2]); g->m_vertices.pushBack(v);
                        v.set(t[3],t[4],t[5]); g->m_vertices.pushBack(v);
                        v.set(t[6],t[7],t[8]); g->m_vertices.pushBack(v);
                        hkGeometry::Triangle tr; tr.set(b, b+1, b+2);
                        g->m_triangles.pushBack(tr);
                    }
                }
                g_worldChunkCount = chunks;
                g_worldTris = (int)n;
                LOG("world geometry rebuilt: %ld tris in %d chunks | AABB (%.1f %.1f %.1f)..(%.1f %.1f %.1f)",
                    n, g_worldChunkCount, g_wmMin[0], g_wmMin[1], g_wmMin[2], g_wmMax[0], g_wmMax[1], g_wmMax[2]);
            }
        }

        vdb->step(16.0f);

        // Periodic fidelity report. Without it an empty or partial view is indistinguishable from
        // "the offsets are wrong on this build".
        if (++frame % 250 == 0 && !g_paused) {
            char un[256]; int o = 0; un[0] = 0;
            for (int t = 0; t < 64; ++t)
                if (g_unhandled[t]) appendf(un, sizeof(un), o, "type%d(x%d) ", t, g_unhandled[t]);
            LOG("obj: bodies=%d tris=%d%s | world: bodies=%d tris=%d%s | ticks=%ld | unhandled=[%s]",
                g_bodyCount,
                (g_objBucket[0] ? g_objBucket[0]->m_triangles.getSize() : 0) +
                (g_objBucket[1] ? g_objBucket[1]->m_triangles.getSize() : 0) +
                (g_objBucket[2] ? g_objBucket[2]->m_triangles.getSize() : 0),
                g_triCapped ? " (TRUNCATED)" : "",
                g_worldBodies, g_worldTris, g_worldCapped ? " (TRUNCATED)" : "",
                g_ticks, un[0] ? un : "none");
            for (int t = 0; t < 64; ++t) g_unhandled[t] = 0;
        }
        Sleep(16);
    }
}

// Reset every gather-state variable so a re-enable (typically after a level change) rebuilds from
// scratch instead of showing the previous map.
static void resetGatherState()
{
    g_lastWorldSig = 0;
    g_wmTriCount = 0;
    g_wmMeshTriCount = 0;
    g_wmMeshCount = 0;
    g_wmInstCount = 0;
    g_wmMin[0] = g_wmMin[1] = g_wmMin[2] =  1e30f;
    g_wmMax[0] = g_wmMax[1] = g_wmMax[2] = -1e30f;
    g_triCapped = g_worldCapped = false;
    g_capMesh = g_capWorld = g_capGeom = 0;
    g_capMeshWorld = g_capWorldWorld = 0;
    g_tvTriN = g_tvEdgeN = g_scTriN = g_tvLabelN = 0;
    g_scBadTri = 0;
    g_allN = 0;
    g_bodyCount = g_worldBodies = g_worldTris = 0;
    g_world = 0;
    InterlockedIncrement(&g_wmGen);      // force the VDB thread to re-publish
    InterlockedIncrement(&g_scenGen);
    g_wantFirstWorld = true;
}

} // anonymous namespace


// ===================== HCM bridge =====================
#include "HCEHavokDebuggerBridge.h"

namespace HceHavokDebuggerBridge
{
    bool isRunning() { return g_threadStarted != 0 && !g_paused; }

    bool start()
    {
        if (!resolveModule()) return false;      // HaloSimulation_tag_release.dll not loaded yet

        g_paused = true;                         // silence the gather while we re-establish

        initSections();
        resolveAnchors();

        // A stale address is worse than none: refuse rather than serve a scene built from whatever
        // now occupies the old offsets. HCE ships no version resource, so signatures are the ONLY
        // update detector available - which is exactly why there is no static fallback.
        if (!A(A_COMP_ARRAY) || !A(A_VT2)) {
            LOG("REFUSING to start: havok_components_array=%p shell_vtable2=%p (signature unresolved - "
                "the game build changed)", (void*)A(A_COMP_ARRAY), (void*)A(A_VT2));
            return false;
        }

        uninstallTickHook();                     // no-op the first time
        resetGatherState();
        if (!installTickHook()) return false;

        if (InterlockedCompareExchange(&g_threadStarted, 1, 0) == 0)
        {
            InterlockedExchange(&g_vdbShouldExit, 0);
            // ⚠ THE HANDLE IS KEPT, not discarded. stop() has to be able to WAIT for this thread to leave
            // our image before HCMInternal.dll can be unloaded; without a handle there is nothing to wait
            // on and the unload races a thread executing inside the module being unmapped.
            if (g_vdbThreadHandle) { CloseHandle(g_vdbThreadHandle); g_vdbThreadHandle = nullptr; }
            g_vdbThreadHandle = CreateThread(nullptr, 0, vdbThread, nullptr, 0, nullptr);
        }

        g_paused = false;
        return true;
    }

    void stop()
    {
        g_paused = true;                         // gather first...
        uninstallTickHook();                     // ...then take the vtable slot back

        // ...then genuinely stop the server. Un-ticking the toggle used to leave the VDB thread running and
        // 127.0.0.1:25001 still listening, so a connected HVD client never noticed and a re-enable collided
        // on the port. It also meant HCMInternal.dll could be unloaded with this thread still inside it.
        if (g_threadStarted == 0 || !g_vdbThreadHandle) return;

        InterlockedExchange(&g_vdbShouldExit, 1);

        // The loop polls the flag once per iteration (~16ms), so 3s is generous. A timeout means the thread
        // is wedged somewhere in Havok - a hang we cannot safely interrupt, since TerminateThread would
        // leave Havok's thread-local allocator corrupt.
        const DWORD waited = WaitForSingleObject(g_vdbThreadHandle, 3000);
        if (waited == WAIT_OBJECT_0)
        {
            CloseHandle(g_vdbThreadHandle);
            g_vdbThreadHandle = nullptr;
            LOG("VDB thread joined");
            return;
        }

        // ⚠ LAST RESORT. The thread did not come back, so unloading HCMInternal.dll would unmap code it is
        // still executing. PIN the module: a permanent leak of one DLL is strictly better than an access
        // violation in the user's game. Logged loudly because it also means the close/reopen fix cannot
        // work for this session - HCM will have to be used with the game restarted.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)&vdbThread, &self);
        // Its own LOG, not plog: this file deliberately keeps its logging self-contained.
        LOG("VDB thread did NOT exit within 3s. HCMInternal.dll has been PINNED so it can never be unloaded - "
            "this avoids an access violation, but HCM cannot be cleanly reopened against this game session.");
    }

    int unresolvedAnchorCount()
    {
        int bad = 0;
        for (int i = 0; i < A_COUNT; ++i) if (!g_anchors[i].resolved) ++bad;
        return bad;
    }
}

#else  // !HCM_HAVOK_AVAILABLE -> no-op stub so Debug links without havok.lib

#include "HCEHavokDebuggerBridge.h"
namespace HceHavokDebuggerBridge
{
    bool start()  { return false; }
    void stop()   {}
    bool isRunning() { return false; }
    int  unresolvedAnchorCount() { return 0; }
}

#endif // HCM_HAVOK_AVAILABLE
