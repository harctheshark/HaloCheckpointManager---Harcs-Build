# Switch Zone Set — Halo 3 / ODST / Reach / Halo 4 research

Research only. Nothing here is implemented. Targets **MCC 1.3528.0.0** (the installed build); every
module offset below is 1.3528-only unless stated.

Method: eight agents — one miner plus one adversarial verifier per game, each verifier required to use a
different evidence path than its miner (Hex-Rays vs. raw PE + capstone, cache-file parsing in Python vs.
editing-kit tag definitions). Findings below are the post-verification state, with my own corrections
where I checked a claim myself.

---

## The headline: the difficulty order inverted

The plan was "Halo 3 first, then replicate". The research says the opposite. **Reach and Halo 4 are
near drop-in ports of the shipped HaloCER implementation. Halo 3 and ODST are the hard ones**, because
their zone sets have no readable name — only a `string_id`, and HCM has no `string_id` resolver for any
third-gen title.

| | Reach | Halo 4 | Halo 3 | ODST |
|---|---|---|---|---|
| zone set block in scenario | `0xAC` | `0x100` | `0x58` | `0x74` |
| element stride | `0x13C` | `0x1A0` | `0x28` | `0x28` |
| name | **char[256] @ +0x04** | **char[256] @ +0x04** | string_id @ +0x00 | string_id @ +0x00 |
| flags (internal bit) | `0x108` bit 0x4 | `0x108` bit 0x4 | `0x08` bit 0x4 | `0x08` bit 0x4 |
| bsp zone flags (authored) | `0x10C` | `0x10C` | `0x0C` | `0x0C` |
| bsp zone flags (runtime) | `0x114` | `0x114` | *(none in element)* | *(none in element)* |
| current zone set global | `haloreach.dll+0xAFBE30` ✔ | `halo4.dll+0xE5BB50` ✔ | `halo3.dll+0x89CADC` ✚ | `halo3odst.dll+0x8E0B94` ✚ |
| max zone sets | 48 | 64 | 48 | 48 |
| effort | **low** | **low** | high | high |

✔ already in `InternalPointerData.xml` and confirmed correct · ✚ newly derived, HCM had nothing

Reach's `0xAC`/`0x13C` and Halo 4's `0x100`/`0x1A0` — HCM's existing hypotheses, carried over from the
soft-ceiling work — both **confirmed**. Halo 3 and ODST are new.

---

## Correction: `SendCommand` is available for all four games

Three of the four miners concluded "HCM has no HaloScript path for MCC titles, so the HaloCER approach
of queueing `switch_zone_set <name>` cannot be ported". **That is wrong.** The Halo 4 verifier caught it
and I confirmed it independently:

- [GameEngineFunctions.cpp:47](HCMInternal/GameEngineFunctions.cpp:47) — `SendCommand` prefixes `"HS: "`
  and calls `GameEngineDetail::getCommonHandle()->execute_command`.
- `gameEnginePointer` in `InternalPointerData.xml` has **no `Game=` attribute** — it is keyed by MCC
  version only (1.2448 … 1.3528), i.e. process-wide.
- `gameEngineCommonFunctionTableFields` is `Version="All"`, `execute_command` at `+0x48`.
- `CommandConsoleGUI.cpp:111` already ships on this path, and `HCEConsole.h:11` says the HaloCER console
  was modelled on it.

So `switch_zone_set <name>` is reachable in all four games. It is also the *better* mechanism where it
is usable, because it runs the engine's own validator instead of bypassing it.

**It still needs a name.** Which is why it solves Reach and Halo 4 and does nothing for Halo 3 / ODST.

---

## Mechanism, per game

### Reach and Halo 4 — use `SendCommand`, exactly like HaloCER

Name is a literal `char[256]` at element `+0x04` — the same shape and the same offset as HaloCER. The
name-reading loop in `HCESwitchZoneSet.cpp::refresh()` copies over verbatim. `switch_zone_set` exists as
a script function in both (Reach: name string `+0x9FE990`, eval `sub_1801AEDBC`; Halo 4: impl
`sub_18006252C`, registered at `+0x159786`).

Verified against shipped caches: all 44 Reach maps / 130 elements have non-empty names, count never 0,
max 18 (m45). Halo 4 likewise — and **neither game ships an unnamed internal zone set**, so the
HaloCER `switchable()` filter and the `(internal zone set N)` placeholder can be dropped. Keep the
empty-name fallback anyway.

Global-write fallback if `SendCommand` disappoints in game (both proven, index first then flag):

- Reach: `int32 haloreach.dll+0x263EAD4` = index, then `uint8 +0x263EAC2` = 1
- Halo 4: `int32 halo4.dll+0x293DEC8` = index, then `uint8 +0x293DEB4` = 1

Halo 4's flag byte sits in the same block as HCM's already-shipping `forceCheckpointFlag`
(`halo4.dll+0x293DEAF`), which is good corroboration that this is the engine's deferred-request area.

### Halo 3 and ODST — index-based global writes, names are the problem

Element is only `0x28` bytes: `+0x00` name string_id, `+0x04` pvs index, `+0x08` flags, `+0x0C` bsp zone
flags, `+0x10` required designer zones, `+0x24` chocolate mountain override. No char array anywhere.

Switch by writing the pending-request pair, index first:

- Halo 3: `uint32 halo3.dll+0x20B96C0` = index, then `uint8 +0x20B96B1` = 1
  (main loop `sub_1800B2250` polls the byte each tick → `sub_1800B3888` → `main_switch_structure_bsp`, clears both)
- ODST: `int32 halo3odst.dll+0x20FF6D0` = index, then `uint8 +0x20FF6C1` = 1

Bound-check yourself — the pump does not. The engine's own check is **signed**: `test/js` then
`cmp/jge` against the count. An unsigned check would let `-1` through.

**The name blocker.** [GetDebugString.cpp:7](HCMInternal/GetDebugString.cpp:7) says *"just doing the h2
impl for now"* and its pointer data exists for Halo 2 only. `GetSoftCeilingData` stores third-gen
`stringID`s but only ever uses them as match keys — it never resolves one to text. There is **no
string_id→text resolver for any third-gen title in HCM**. Options:

1. **Call the engine resolver.** Halo 3 `+0x108BE0`, ODST `+0x1272C4`, both
   `const char* __fastcall(uint32_t)`, returning null on miss. The ODST verifier's warning matters: it
   is *not* safe unguarded — it dereferences `*(void**)(module+0x2029340)` with no null check and
   `hash_find` immediately does `mov rax,[rcx+0x10]` / `div`. Must check the table pointer and its first
   dword before calling, and never call with no map loaded. Thread-safety unproven.
2. **Ship without names** — `(zone set N)`. Switching works fine, since it's index-based.
3. Hardcode per level, the way HCM already does Halo 3 trigger names (`TriggerNameResolverHardcoded`).

The verifier also killed the Halo 3 miner's string_id decoding theory outright: the stored dword is
**not** a direct index into the cache string table and carries no length byte. `set_null` in
`010_jungle` is stored as `0x5A9` while the string sits at index `0xE34`; indexing naively returns
`resume_campaign`, and all 38 Halo 3 maps store `0x5A9` in their first zone set despite different names.
Do not reimplement the hash table from the delta — it is map-specific.

---

## Claims I checked myself and corrected

- **"ODST's `currentBSPSet` 1.3528 entry is an empty `<Offsets></Offsets>`"** — **false.** It is
  `0x46E261C`, and there are zero empty `<Offsets>` blocks in the entire file. Drive-by remark from the
  Halo 3 verifier about a game outside its scope.
- **"HCM has no console/script-injection path for MCC titles"** — false, see above.
- **`GetCurrentZoneSet::getCurrentZoneSet()` returns `uint32_t`** —
  [GetCurrentZoneSet.h:19](HCMInternal/GetCurrentZoneSet.h:19), **true and a real trap.** The engine's
  "none" sentinel is `-1`, so callers receive `0xFFFFFFFF`. Any `index != current` guard or `<- current`
  marker comparing against a signed int will misbehave.

---

## Open risks

- **Nothing was tested in game.** Anti-cheat is active, so this is entirely static. Every mechanism
  above proves the data path exists and is reachable; none proves the game performs the switch. First
  real test: set index, set flag, confirm `currentZoneSet` changes on the next tick.
- **`ZoneSetChangeFunction` has no Halo 3 entry** even though `ZoneSetChangeHookEvent.cpp` and
  `GetCurrentZoneSet` already have Halo 3 cases — those services throw at init today. This work fills
  the `currentZoneSet` gap; the hook site is still single-source and unverified.
- **1.3385.0.0 is unverified for every game.** All offsets here are 1.3528-only. Halo 4 1.3385 already
  has a *different* `scenarioAddress` (`0x49679B0` vs `0x4967A30`), so extrapolating by a constant delta
  is not safe.
- **Reach tag-block decode disagreement.** The verifier claims the engine decodes via a region table
  (`regionTable[enc>>28] + 4*enc` at `qword_184E39F20`), not the single-region
  `magicAddress + tagBase + (enc<<2)` that HCM's `TagBlockReaderImplMagic` uses for Reach. It also says
  every encoded value in every shipped Reach map lands in region 0, so both agree in practice. Worth one
  look before trusting `TagBlockReader` on a map that might not.
- **Reach reads names through a different scenario handle** than the one the switch validates against
  (`qword_180C1A600 + 8*(u16)dword_180AFBE38 + 4` vs `qword_180C1A230`). Read names and count from the
  same handle the switch uses, or they can disagree across a level transition.
- **Reach/Halo 4 "already current" is a three-part test**, not `index != current`: index matches AND the
  loaded BSP mask is a superset of `element+0x114` AND the designer-zone mask matches. Writing
  `index == current` raw is not the no-op it looks like.
- `cex_ff_halo.map` is the one shipped Reach map where `+0x10C != +0x114` — always read `+0x114`.
- ODST firefight/DLC maps unchecked (`c100.map` did not resolve through either agent's parser).

---

## Suggested order for the build session

1. **Halo 4** — everything confirmed, name is char[256] at the same offset as HaloCER, `SendCommand`
   available, and the flag block is corroborated by shipping HCM features on the same module.
2. **Reach** — same shape; settle the tag-block decode and scenario-handle questions while doing it.
3. **Halo 3** — index-based switch, ship `(zone set N)` names first, add the engine resolver after.
4. **ODST** — replicate Halo 3; offsets differ (`0x74` vs `0x58`) so re-derive, don't assume the delta.

Per-game raw agent output (miner + verifier JSON) is in the session scratchpad as `zs_all.json`.
