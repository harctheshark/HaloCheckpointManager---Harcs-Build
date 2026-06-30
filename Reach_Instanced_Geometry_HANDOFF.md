# Halo Reach — Instanced Geometry (Havok Debugger) — ★ SOLVED (blob path), production TODO below

## ★★★ WORKING (2026-06-27, user-confirmed "it works!") — tag-blob pipeline
Instanced geometry now renders CORRECTLY in the viewer via a precomputed tag blob (bypasses the runtime BvCMS bake):
1. OFFLINE: `reach_sbsp_extract_instanced.ps1` drives `HREK\bin\ManagedBlam.dll` from pwsh 7 -> reads the source
   `.scenario_structure_bsp`: for every `instanced geometry instances` element (mesh_index + scale + forward/left/up/
   position) it transforms its definition's winged-edge `collision info/surfaces` (walked surface->edges->verts, fanned)
   into world space: `world = position + scale*(forward*lx + left*ly + up*lz)`. Writes a blob:
   `"RINST001"[8] + int32 triCount + triCount*9 float32`. m50_000 => 316 defs (74 w/ collision), **121638 world tris**, 4.3MB.
2. RUNTIME: HCM `loadReachInstancedBlob(path)` (in HavokDebuggerImpl.cpp) loads the blob into `g_instTri` in `gatherBSP`
   (g_game==3), overriding the runtime instanced routing. The "Instanced Geometry - Needs Work" viewer renders it (magenta).
PROVEN: the offline placement == the runtime A2CFE0 read exactly (instance[2] match), so the data is faithful.

## ★ MAP DETECTION DONE (2026-06-27) — datum-keyed blobs, viewer renamed
- The **scenario tag datum** = `*(u32) @ haloreach.dll+0xAFBE38` (resolveAddrs g_game==3: `g_scenDatumAddr=RV(0x180AFBE38)`)
  is a stable per-map id (read directly from the module; MCC strips tag names so this is the reliable key). m50 = `E17E0008`.
- `loadReachInstanced()` loads `<HCMInternal.dll dir>\reach_instanced\<datum:08X>.rinst`. If absent -> g_instTriCount=0
  (empty viewer; does NOT fall back to broken runtime instanced). Datum + path + loaded flag printed via OutputDebugString
  AND surfaced in g_rbDiag (scenDatum/instBlobLoaded/instBlobTris) read by reach_bspread.lua ("INSTANCED BLOB" line).
- Viewer registered as **"Instanced Geometry"** (dropped "Needs Work"), magenta, Reach-only.
- Blob folder: `HCMExternal\bin\x64\Release\net7.0-windows\reach_instanced\` (m50: E17E0008.rinst, 4.3MB / 121638 tris).
- WORKFLOW (per map): `reach_sbsp_extract_instanced.ps1 <tagpath> out.rinst` (dev, needs HREK) -> launch map -> read datum from
  reach_bspread.lua -> rename out.rinst to `<datum>.rinst` in the folder. End users just need the shipped blobs (no HREK).

## REMAINING
- **SHIP THE BLOBS** with HCM for the maps we support (pre-generated). Multi-BSP levels: one blob per scenario currently
  (datum is per-scenario, not per-bsp) — fine for single-bsp campaign levels; revisit for multi-bsp if needed.
- **CACHE READER** (future ideal): parse instanced geo straight from the .map cache at runtime — no precomputed blob, no
  HREK ever, fully self-contained. The ManagedBlam dump (reach_sbsp_managedblam_dump.ps1) is the ground-truth oracle.
- Strip REACHBSP01/SRCDIAG diagnostics before any public release (kept now for the cache-reader effort).

---
# (prior investigation notes below — kept for the cache-reader effort)
# Halo Reach — Instanced Geometry (Havok Debugger) — HANDOFF / NEEDS WORK

**Status (banked):** World structure collision renders correctly in the **BSP Collision** viewer (read live from
the runtime `hkpBvCompressedMeshShape` via in-process `getChildShape`). Reach **instanced geometry** is **parked**
in its own Reach-only viewer **"Instanced Geometry - Needs Work"** (rendered **magenta** = unverified). Its
placement/shape is wrong and unsolved. This doc captures everything learned so the next session can resume cold.

Target: MCC `haloreach.dll` 1.3528.0.0, ImageBase `0x180000000`, ASLR. Code: `HCMInternal/HavokDebuggerImpl.cpp`.

---

## What renders where now
- **BSP Collision** (`CAT_BSP_SOLID`, `g_bspTri`): world structure polys (`A2D558`). Correct. Leave alone.
- **Instanced Geometry - Needs Work** (`CAT_INSTANCED`, `g_instTri`, Reach-only, magenta): everything instanced —
  `A2D1D0` instance flat polys, `A2CFE0` instance transforms, `A7F4B0` instance transforms. Routed there via
  `fanReachInst()` / `emitSolidInst()` (swap the global emit target to `g_instTri`, then restore).
- Gather is content-gated (runs once per map load on the engine-thread step hook), not per frame.

## The runtime chain (verified)
```
fixed/active/inactive island body -> shape:
  0xA2D3A8 MOPP (VT_INST_MOPP) -> child BvCMS @ +0x70 (magic 123456789 @ +0x10) -> container @ +0x20 (vt A2D308)
  0x9E76F0 MOPP (VT_MOPP)      -> single-shape-container @ +0x50 -> *(mopp+0x58)=A2D450 -> container @ +0x20 (vt A2D418)
container.getChildShape(key>>29): ==1 A2D558 (flat convex WORLD poly) | ==2 A7F4B0 (xform) | ==3 A2CFE0 (xform)
A2CFE0 instance transform: basis(cols) @ +0x40/+0x50/+0x60, translation @ +0x70, child @ +0x90 = A2D1D0
A2D1D0 wrapper: UNIFORM SCALE @ +0x30 (s,s,s), child @ +0x28 = A2D140
A2D140 = hkpConvexVerticesShape: count(int) @ +0x60, verts via *(+0x50) FourVectors SoA, planes via *(+0x70)/np@+0x78
```

## What we PROVED
- **Counts:** 2165 tag instances; runtime ≈ **11442 A2CFE0** (in the A2D3A8 structure BvCMS) + **88 A7F4B0**
  (A2D418 container). ~5.3 convex pieces per instance (convex decomposition baked into the structure mesh).
- **Per-instance SCALE** = `A2D1D0 + 0x30` (uniform; range **0.068 .. 6.57**). Now applied (`fanReachPoly` builds
  `Xs` = X with columns scaled). This == the tag's `Scale @ 0x0`.
- **Translation** (`A2CFE0 + 0x70`) XY is **exact**: the piece nearest the player read `(390.6, 268.6, 139.6)`;
  player stood at `(390, 267.8, 191)` and the tag sub-group center was `(390.4, 269.7, 124.9)` — XY/Z all agree,
  so the *position* of that instance is right (player at 191 is simply ~66u **above** geometry that sits at ~125).
- **Rotation** is a valid orthonormal basis at `+0x40/+0x50/+0x60`, applied as Havok columns
  (`out = lx*col0 + ly*col1 + lz*col2 + t`). The "largest building" instance's matrix is a genuine **180° flip**
  (col2 = (0,0,-1)) and is *symmetric*, so transpose changes nothing — not a row/col bug.
- Census: 11133 rendered, **0 outlier verts**, maxVertCount 44, centroid bounds X[-483..536] Y[-413..363]
  Z[-74..165]. No origin cluster in the triangle attribution (near-origin <30 was ~1192 tris total).

## What's BROKEN (the mess)
Isolating instanced-only showed pieces **radiating/fanning out from a single center** (star of long beams).
Per-piece capture for the player's building:
```
piece 0-3: worldT=(390.6 268.6 139.6)  scaleXYZ=(1.07 1.07 1.07)  localExt=40.8  nv=6
```
- All pieces of one instance share the **same translation** (the instance center) — expected for a convex
  decomposition (the per-piece offset should be inside the local verts). Scale/size fine.
- So the pieces should *assemble* into the instance, but they **fan out** → the per-piece **offset/orientation
  within the instance is being lost or misapplied**, AND:
- **Plane-clip over-extension:** `emitReachHull` renders each A2D140 from its stored facet planes by clipping a
  seed quad. Simple convexes (e.g. nv=6 building pieces) often have **incomplete plane sets** (~**9.6% unbounded
  faces**, `facesSkippedUnbounded=18443`) → faces can't be bounded and stretch toward the seed → **long spikes**.
  A **vertex hull** (bounded by the actual verts) kills the spikes, but then **many buildings vanish** (the
  vertex-hull eps/dedup drops their faces). Tried + reverted.

## Tag format (Assembly plugins)
- **ReachMCC** `sbsp` `Instanced Geometry Instances` @ header `0x258`, **elementSize 0x4 = NAME ONLY** (stringid,
  e.g. "shape346 (m50_buildings)"). **No transform in MCC.** Groups (`0x234`) + Sub Groups (`0x240`: Center,
  Radius, Cluster Count, Members) are spatial culling, not placements.
- **ReachBeta** `sbsp` `Instanced Geometry Instances` @ `0x26C`, **elementSize 0xA8** = the FULL record:
  `Scale @0x0`, `Forward @0x4`, `Left @0x10`, `Up @0x1C`, `Position @0x28`, `Mesh Index @0x34`, Flags @0x36.
  MCC baked these out → the clean per-instance transform is **not in the MCC tag block** (must come from the
  runtime bake, or a not-yet-found MCC block, or by wiring the tag chain to a different layout).
- Tag chain (Reach, unverified/unwired here): `*(haloreach.dll + 0x25bda20) + 0x2F8 + 0x18 + 0`; scenario datum
  `@ 0xAFBE38`. Reach `g_tagResolve` is currently 0 (deferred) in `resolveAddrs`.

## NEXT IDEAS (pick one)
1. **Crack the BvCMS bake**: find where the per-piece offset/orientation inside an instance lives (the `+0x80`
   mystery vec? a section transform in the BvCMS `m_transforms` array?) so the runtime pieces assemble.
2. **Tag-driven placement**: wire the Reach tag chain, read instances per (sub)group + their definition meshes,
   place definition geometry by the tag transform. Blocker: MCC instances block is name-only — need to find the
   real per-instance transform block in the MCC sbsp (or accept group-level placement).
3. **Hybrid**: tag placement (clean) + runtime decompressed convex geometry, correlated per instance.
4. **Renderer**: make `emitReachHull` robust — vertex hull for simple convexes WITHOUT dropping faces (fix the
   eps/dedup that made buildings vanish), keep plane-clip only for high-nv rounded convexes with complete planes.

## ★ FOUNDRY reference (the proven method — read the EK SOURCE tag, NOT the MCC cache/runtime)
`C:\Users\hurri\Downloads\Foundry-master` (Blender addon, ManagedBlam) imports Reach instanced geo with CORRECT
placement. `managed_blam/scenario_structure_bsp.py` + `connected_geometry.py`:
- **Instances** = block `instanced geometry instances`; per element: `ShortInteger:mesh_index` (-> definition),
  `scale`, `forward`, `left`, `up`, `position`. Matrix = COLUMNS [forward | left | up | position*100], uniform
  `scale` applied separately. ← **CONFIRMS my transform math (Xs) was correct.**
- **Definitions** = `Struct:resource interface[0]/Block:raw_resources[0]/Struct:raw_items[0]/Block:instanced
  geometries definitions`; per def: `mesh index` + `compression index` (render mesh, dequant via CompressionBounds)
  AND (Reach, non-corinth) **`Struct:collision info[0]/Block:surfaces`** = the per-definition WINGED-EDGE collision
  (plain float surfaces/edges/verts — NO quantization; same shape as H2/H3 collision_bsp). Proxy collision via
  `Block:render bsp`. Structure collision = `raw_items[0]/Block:collision bsp` (+`large collision bsp`).
- KEY: Foundry reads the **EK source `.scenario_structure_bsp` tag file** (uncompressed, FULL instance records) via
  ManagedBlam. The MCC **cache** tag baked the per-instance transforms OUT (Assembly showed instances block
  elementSize 4 = name only) — that's why my runtime reconstruction was lossy. The clean data only exists in the
  EK source tag (or the loaded resource).
- "Compression" the user mentioned = RENDER-geometry vertex quantization (CompressionBounds dequant). The
  **COLLISION surfaces are uncompressed floats** — so a collision debugger doesn't need to decompress anything;
  it needs the winged-edge `collision info/surfaces` per definition + the per-instance matrix.
- Proof file: `C:\Users\hurri\Documents\exodusshared.blend` (Foundry import, everything correctly placed).

## ★ RECOMMENDED PATH (supersedes the runtime-BvCMS struggle)
Parse the **HREK source `.scenario_structure_bsp` tag file** (on disk) like Foundry: read `instanced geometry
instances` (mesh_index + scale + forward/left/up/position) + `instanced geometries definitions` (collision
info/surfaces, winged-edge). Compose `world = position + Rot[forward|left|up] * (scale * surfaceVert)` and render.
This sidesteps ALL runtime-bake problems (placement + geometry are clean + complete in the source tag). Decide:
(a) parse the EK tag file directly from HCM, or (b) an offline tool exports a placement+collision blob HCM loads
per map (like the user's HKT extractor). Need the tag-file binary layout for the two blocks (Assembly ReachBeta
plugin had the instances record @ 0xA8: Scale@0/Forward@4/Left@0x10/Up@0x1C/Pos@0x28/MeshIdx@0x34; definitions +
collision-surfaces layout TBD from a complete plugin or the EK tag def).

## ★★ GROUND TRUTH via ManagedBlam (VALIDATED 2026-06-27) — placement was ALWAYS correct
Drove `HREK\bin\ManagedBlam.dll` from PowerShell 7 (script: `reach_sbsp_managedblam_dump.ps1` in repo root).
Init recipe: LoadFrom managedblam.dll; `Bungie.ManagedBlamSystem.Start(hrekRoot, crashCallback, params{InitializationLevel=TagsOnly})`;
`Bungie.Tags.TagPath.FromPathAndExtension("levels\\solo\\m50\\m50_000","scenario_structure_bsp")`; `TagFile.Load(tp)`.
PowerShell gotchas: (1) load managedblam ONCE + AssemblyResolve must return the already-loaded asm (double-load = type-identity
mismatch -> "no method named Start"); call Start via reflection MethodInfo.Invoke. (2) Bungie collections have a buggy
generic enumerator (ArrayEnumerator->IEnumerator<TagField> cast) -> NEVER let pwsh enumerate them: get Count/Item via
reflection property, and wrap element returns in `Write-Output -NoEnumerate`. callback = `{param($i)} -as $tCb`.
DUMP RESULT (m50_000.scenario_structure_bsp):
- `instanced geometry instances` = **2165** (matches the MCC cache count). Per element: `ShortInteger:mesh_index`, `scale`,
  `forward`, `left`, `up`, `position` (all GAME UNITS — no x100). Instance[2]: mesh=1 scale=1 pos=(-157.82,-44.119,-26.26)
  fwd=(-0.423,-0.906,0) left=(-0.906,0.423,0) up=(0,0,-1) — **EXACTLY equals the runtime A2CFE0 I captured for the "largest
  building".** So the runtime placement + my transform math were CORRECT; the building's up=(0,0,-1) is the authored value.
- `Struct:resource interface[0]/Block:raw_resources[0]/Struct:raw_items[0]/Block:instanced geometries definitions` = **316**.
  Per def: `mesh index`, `compression index`, and `Struct:collision info[0]/...` winged-edge collision_bsp. def[1] (the
  building, used by instances 2/3): surfaces=8, edges=18, vertices=12, planes=8, bsp3d nodes=8, leaves=8. Verts =
  (-6.6,-9.8,0)/(-6.6,-3.012,0)/(-6.6,-3.012,-65.6)/... = the SAME local convex verts seen in the runtime A2D140. Plain
  floats, LOCAL space, standard Halo collision_bsp (see [[halo2-collision-bsp-format]] for the winged-edge walk).
CONCLUSION: the clean model = 2165 instances (mesh_index + scale + fwd/left/up/pos) x 316 definitions (collision_bsp,
local). Render: world = pos + scale*[fwd|left|up]*localVert, walk surfaces->edges->verts. The runtime "mess" was the BvCMS
per-piece convex decomposition assembly, NOT placement. Tag-based rendering bypasses it entirely.
TWO SHIP PATHS (decide): (A) OFFLINE: ManagedBlam tool (proven) reads HREK source tag -> exports per-map world-space
collision-triangle blob; HCM loads the blob (end users need NO HREK, just the shipped blob; dev regenerates per map). FAST
— the hard part is done. (B) CACHE READER: HCM parses the .map cache (instances + definition collision_bsp) live, no
precompute, no HREK — but needs the cache tag-format + any resource compression reversed (user's stated preference).

## Diagnostics LEFT IN (strip before any public release)
- `REACHBSP01` struct (`g_rbDiag`) + `SRCDIAG0` (`g_srcDiag`) + the `g_skipWorldPolys` (now unused) flag.
- Reader: `G:\SteamLibrary\steamapps\common\H3EK\vdb_inject\reach_bspread.lua` (AOB-scans REACHBSP01 / SRCDIAG0;
  dumps counts, nearest-instance probe, A2D1D0 wrapper, building-piece capture, near-origin attribution).
- Probes: `reach_playerpos.lua` (player biped pos), plus older `reach_bspdiag/inst/census/bvcms/a2d450.lua`.
