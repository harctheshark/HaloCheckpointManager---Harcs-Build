# MSL Research — Halo 4 biped physics flags and the slope-wedge desync

Live reverse engineering of **MCC 1.3528.0.0**, `halo4.dll`, 2026-09-13.

All offsets are from the **player biped base** — the address `GetObjectAddress` returns — unless a line
says `object +` (same base; "object" is used where the field belongs to the generic object, not the biped)
or `component +` (the havok component).

Before this, HCM had **zero flag fields for Halo 4**. `bipedDataFields Game="Halo4"` was only
position, velocity, health, shields and a few datums — no airborne, no crouch, no contact state.

---

## 1. The flags

| offset | meaning | how it was established |
|---|---|---|
| **`+0x0F8C` bit 1** | **AIRBORNE** (1 = airborne) | the Halo 1 `isAirborneFlag +0x4D8` equivalent |
| `+0x0F48` bit 9 | supported / grounded (inverse of airborne) | |
| `+0x0F48` bit 6 | holds a contact plane | |
| `+0x0F48` bit 16 | **proxy moved / was integrated this tick** | 0/544 frames frozen when SET; 6/9 frozen when CLEAR |
| `+0x0FEC` | contact index — `9` grounded, `0xFFFFFFFF` NONE airborne | |
| `+0x0FF4` low word | surface index — `0028/0045/0064/0079/0099`; `0xFFFF` NONE airborne | changes with the surface landed on |
| `+0x0FF8` (mirror `+0x1024`) | contact plane **normal**, unit length | `z=0.9995` flat, `z=0.7010` = 45.5° slope |
| `+0x00F4` | **active-simulation handle** (a `c_havok_contact_point` datum) | NONE when the body is not actively simulated |
| `+0x1168` | **at-rest position latch** — holds position at rest, zero while moving | nonzero 0/398 airborne, 7/156 grounded |
| **`object +0x100` bit 10** | **AT REST** | this is literally what `object_get_at_rest` returns |
| `object +0x100` bit 8 | physics-participating | gates the Havok branch in `object_set_at_rest` |
| `+0x00F0` | havok component datum | already in HCM as `havokComponentDatum` |

**Why the airborne bit is trusted:** `+0x0F8C bit1`, `+0x0F48 bit9`, `+0x0FEC` and `+0x0FF4` move in
**perfect lockstep across 554 samples, zero disagreements**. Four independent fields agreeing.

### Havok component chain (verified live)

```
hdr       = *(halo4.dll + 0x10BE960)
table     = hdr + *(u32*)(hdr + 0x60)
component = table + 0xE0 * (*(u32*)(biped + 0xF0) & 0xFFFF)
```
Positive control: `component + 0x58` holds the biped position exactly.
(The engine itself reaches the same table as `*(qword_1810BE960 + 80) + 0xE0*index`.)

---

## 2. The glitch signature

Walk up an over-steep slope, let the game start sliding you down, time it so you stop mid-slide.
Position pins to the exact bit while velocity accumulates.

```
airborne (+0x0F8C bit 1) = 1     you are airborne
+0x0F48 bit 6            = 1     you still hold a contact plane
+0x0F48 bit 9            = 0     but you are NOT supported by it
+0x0F48 bit 16           = 0     and the proxy never advances
```
i.e. **`+0x0F48 == 0x00840040` while airborne**. Occurrence: **15/15 glitch samples, 0/560 normal.**

⚠ **No single bit is unique.** Bit-by-bit the state is identical to ordinary airborne. The nearest normal
neighbour is `(b16=1, b9=0, b6=1, air=1)`, which occurs 26 times as a normal leaving-the-ground transient.
Only the **combination** separates. Any detector must test all four together.

---

## 3. What is actually happening

Measured, not inferred:

- **Position freezes exactly.** One distinct position value across 689 samples over 60 seconds.
- **Velocity accumulates in the biped's own field**, not in Havok. x and y freeze exactly
  (`-0.035, 0.064` for 8 consecutive samples); z grows linearly at `-0.0455 wu/tick` = `-2.73 wu/s²`
  at 60 Hz — plain gravity, no terminal clamp, no position integration.
- **Crouch is the pump:** `-0.0455 wu/tick` crouching vs `-0.0004 wu/tick` idle — a **100× difference**.
- **The slope angle is not the trigger.** Instance A's retained normal was 45.5°, instance B's was 18.1°.

### The state table

| state | biped says AIRBORNE | havok body STEPPED | at-rest latch `+0x1168` |
|---|---|---|---|
| **GLITCH** | **100%** | **1%** | **99%** |
| normal airborne | 100% | 100% | 0% |
| grounded, moving | 0% | 100% | 0% |
| grounded, resting | 0% | 13% | 83% |

The glitch is a **hybrid that never occurs in normal play**: airborne like a fall, deactivated and latched
like a body at rest.

### Break-out, frame-exact

`+0x00F4` NONE → real datum `8DA20006` · `+0x1168..0x1170` latch → 0 · `+0x0F48` `00840040 → 00850000` ·
`object+0x100` `0x500 → 0x100` (at-rest bit clearing) · contact block `+0x1058..0x1090` all-zero → populated ·
and **velocity `-54.51 → +2.70`, x/y zeroed**.

⚠⚠ **The banked velocity was DISCARDED on this exit, not converted.** The speed payoff must come from a
different exit path. See §5.

---

## 4. The code reason — two systems, no shared invariant

Static RE of `halo4.dll` (IDA 9.1 headless, on a copy of the database).

### `object_get_at_rest` is one bit

```c
object_get_at_rest(obj) -> sub_1806830C8 -> sub_1805D74A8:
    return (*(u32*)(object + 0x100) & 0x400) != 0;
```

### `object_wake_physics` is `object_set_at_rest(obj, false)`

```c
object_wake_physics(obj) -> sub_180683028 -> sub_1805D73BC(obj, 0)

char sub_1805D73BC(objIndex, bool atRest)            // object_set_at_rest
  typeMask = 1 << objectType
  if ( (typeMask & 0x3ADF) == 0                      // type not physics-bearing
    || (*(u32*)(obj+0x100) >> 8 & 1) == 0            // +0x100 bit 8 clear
    || *(i32*)(obj + 0xF0) == -1 )                   // havok component datum is NONE
  {                                                  //   <-- SIMPLE PATH
      obj[0x100] = atRest ? (v | 0x400) : (v & ~0x400);   // game bit ONLY; Havok never told
  }
  else if (!atRest) {
      sub_18023E868(havokTable + 0xE0*datum, 0);     // the real Havok wake
      obj[0x100] &= ~0x400;
  }
```

**54 call sites** set or clear at-rest. All are event-driven — impact, damage, script, attachment.
**None is "the biped is airborne this tick."** There is no periodic re-validation.

### Why AIRBORNE + AT-REST is representable

| | at-rest | airborne |
|---|---|---|
| lives on | the **object** (`+0x100` bit 10) | the **character-physics block** (`biped+0x0F8C` bit 1) |
| driven by | Havok **simulation-island deactivation** | **contact / support queries** |
| decided by | **kinetic energy** — is the body actually moving | **geometry** — was a supporting surface found |

Stopped mid-slide, **both systems are individually correct**. The proxy's real motion is ~0 because it is
blocked on the slope, so the energy test deactivates it and sets at-rest. The slope is too steep to count
as support, so the contact test finds none and airborne stays set. **Nothing asserts `airborne ⇒ !at_rest`.**

The editing kit confirms the machinery by name: `m_wantDeactivation`, `hkpEntity::requestDeactivation()`,
`Deactivating during warmup will apply deactivation energy penalty`,
`simulation islands (active/inactive): %d/%d`, and — naming `biped+0xF4` outright —
`c_havok_contact_point datums, freeing up indices from inactive entities`.

### The desync generator

The simple path in `object_set_at_rest` moves the game's at-rest bit **without Havok ever hearing about it**,
under three separate conditions. That is a desync by construction, not an accident of this one glitch.

### Dead end: the debug globals are husks

`havok_disable_deactivation`, `debug_objects_force_awake` and `havok_deactivation_reference_distance` all
exist as names but resolve to **valuePtr = 0** in the live process (debug-global table, stride `0x18` =
`{name, type, value}`, confirmed by adjacency). No runtime toggle. Same finding as the `cheat_*` globals
in H3/ODST/H4/CER.

---

## 5. What follows — predictions and leads

**Strong, straight from the code:**

1. **`object_wake_physics` should break the state instantly.** It *is* `object_set_at_rest(obj, false)`,
   and it is a HaloScript function, reachable through `GameEngineFunctions::SendCommand` (which is
   MCC-wide, not per-game). One command to falsify. **UNTESTED.**
2. **The speed-preserving exit is probably the simple path.** The banked velocity lives in the biped field
   `+0x88/+0x90`, not Havok. A wake through the Havok branch re-syncs from the body and discards it —
   exactly what was measured. The simple path clears at-rest with **no Havok re-sync**, so the biped would
   keep its accumulated velocity. **UNTESTED, but it is the obvious candidate.**

**Plausible, untested:**

3. At-rest is an *object* property, so **any object with a havok component** should be desyncable the same
   way — vehicles, crates, dead bodies.
4. `object_get_at_rest` is script-visible, so mission scripts branch on it. Anything waiting for a physics
   object to settle could be satisfied while that object is still "moving".
5. Deactivation is energy-thresholded and tunable, predicting the trick is easiest where residual proxy
   velocity is smallest — consistent with "time it right".

---

## 6. Method (reusable for H3 / ODST / Reach)

`OpenProcess(PROCESS_VM_READ)` on `MCC-Win64-Shipping.exe`, `ReadProcessMemory` of `0x1400` bytes at the
biped. Python + ctypes, no build step. Label states by physics, diff bitwise, and require a bit to be
constant within each state and opposite across states.

**Positive control first, every time:** `+0x70` forward must be unit length, `+0x7C` up must be `(0,0,1)`,
`+0x24` vehicle datum must be `0xFFFFFFFF` on foot. An all-zero read is an access failure until proven
otherwise — that exact check caught a wrong file-offset→VA mapping in this session.

### ⚠⚠⚠ The trap that cost the most time

The first diff produced **24 "unique to glitch" bits. Every one was an artifact** — tick counters at
`+0xFC`, `+0x480`, `+0x650`, `+0x654`, `+0x660`, `+0xF54`, `+0xF78`, `+0xF7C`, `+0xF84`, plus position
mirrors at `+0x1168..0x1170`.

Worse: `bit 14` of every tick counter **survived two instances**. Glitch A was captured at tick `0x30C1`
and B at `0x8B1B` (both have bit 14 clear), while all 554 normal samples fell in `0x5238..0x6024` — the one
window where bit 14 is set. Pure coincidence, and it looked exactly like a real flag twice in a row.

**Capture at least two instances at well-separated ticks *and* locations, and explain every survivor before
believing it.**

### One more correction worth recording

`component+0x80` and `component+0xC0` are **not** the character proxy. They re-point every ~9 s through a
rotating pool of 7–8 addresses, i.e. transient contact/collision records. An early conclusion that "the
havok body never changes" was measuring the wrong object. The state that matters is `+0x00F4` and `+0x1168`
on the biped, and `object+0x100` bit 10.

---

## 7. Confidence

**Established by measurement** — the flag offsets and meanings, the glitch signature and its 15/15 vs 0/560
separation, position freeze, velocity accumulation rate, the crouch 100× difference, the state table, the
frame-exact break-out.

**Established by disassembly** — `object_get_at_rest` = `object+0x100 & 0x400`; `object_wake_physics` =
`object_set_at_rest(obj, false)`; the three-condition simple path; 54 event-driven call sites; the debug
globals being husks.

**Inferred, not proven** — that Havok's energy-based island deactivation specifically (rather than some
other game path) is what sets at-rest during the slide. Which of the 54 call sites fires. Everything in §5.

**Not attempted** — nothing was written to the game. All analysis was read-only.
