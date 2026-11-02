# Superbounce viewer, spot predictor and scanner: changes of 2026-11-01

Follows commit `105fcd9`. HCM source changes are in this commit. The offline scanner, design tools and logs live in
`Documents\Halo Mod And Tools\H2 Monster Bounce Model\` (not in this repo); they are listed here too, because the in-game
code and the scanner share one physics model.

---

## 1. The physics: the solver's start velocity is capped at 4.085 u/s

**The biggest change of the day.** Before its contact solver runs, the game scales the pill's whole velocity (after the
tick's gravity) down to a speed of **4.085 u/s**, keeping its direction. The contacts still come from the real fall: the
swept capsule, so they are deep. Only the solver's starting velocity is capped.

How it was found:

- **The recordings (50 launches):** a rail, and a lightpost with Force Launch and natural falls. The game's own contacts,
  solved from the real fall velocity, were off by a median of 1.24 u/s. Solved from a capped start, the launch vector is
  off by a median of **0.013 u/s**.
- **A live probe (`live_logs\probe_force.py`) caught the capped velocity in the pill's memory mid-step:**
  (0.4629, 1.6156, -3.7231), length 4.0849, the fall plus one tick of gravity scaled by 0.747.
- **The pill's stored velocity (motion+0x50) is the real one.** The accumulator setup (`sub_1809DF650`) adds gravity and
  force, then applies damping; it has no cap. No float near 4.08 is stored on the body. So the cap is computed during the
  step. The exact instruction hasn't been found yet.

What it explains:

- natural falls running about 7% under the old model;
- Force Launch plunges running 15–35% under;
- the game "cancelling" sideways drift: it's shrunk by the same factor;
- starting bounces: falls of 4.85 u/s launched 7.00 and 7.38; the model now gives 6.95 and 7.33.

**Ruled out along the way:**

- **The contact depth bank** (contact `props+0x18`, rule read from builder `sub_1809EBF70`). It only affects contacts that
  persist into a second tick. The rail launch had fresh contacts. A multi-tick bank simulation over-predicted well-tested
  spots.
- **Friction:** solver op 7 is a 2D friction row (radius = Σimpulse × μ / n / 8), but μ (`props[2]`) is 0 on all 4,934
  logged contact points, so it never acts on the pill.

**Where the cap lives:**

- `h2sb_core.h` (`SOLVE_VMAX`) and `h2sb_fast.h`: CPU and GPU scanner.
- `H2CV_SbScan.cpp` `solve()`: in-game recipe and physics checks.

**Accuracy from the map geometry alone** (the scanner's own contacts plus the cap): about 80% of 52 recorded landings
within 0.5 u/s; median 0.05 on the rail, 0.19 on the lightpost. The misses are contacts built differently from the game
(odd normals).

## 2. Slow falls (starting bounces)

Below 5.4465 u/s the game doesn't sweep: the pill sinks in, and the contacts are found at the sunk position. Two fixes,
applied in the scanner (CPU core, GPU fast path) and in HCM:

1. **Reachability:** "was the tick before clear" sweeps that tick only if **its own** speed (one tick of gravity less) was
   over the sweep threshold. The faster landing tick's sweep used to see the floor the pill had already reached, and
   rejected the landing.
2. **Start heights:** a slow, unswept fall starts **sunk in** (`phase_side()` in the scanner; the HCM refine and seam
   checks do the same).

Check: the recorded 4.85 u/s starting bounce replays to (1.29, -0.39, 7.00), the game's exact launch. Standard scans
(falls 10+) are byte-identical to before.

## 3. Seam colours on the validated physics (HCM, `H2CV_Superbounce.cpp`)

**What it does:** in the air, the 6 nearest seams that aren't already green get a physics check. Your height and fall
decide **when** you land; your forward speed decides **where** you are on that tick. HCM steps the fall tick by tick, at
forward speeds 0–2.25 (the biped's maximum) along your heading, until the game's rules find a contact. It then runs the
capped solver through the new `h2cv::sbscan::landingLaunch()`.

| Colour | Meaning |
|---|---|
| green | launches at your current forward speed |
| **light blue** | **slow down**: tap or fragment forward |
| **purple** | **speed up**, up to 2.25 |
| (unchanged) | no forward speed launches |

Each label adds a `PHYSICS:` line with the forward-speed range that works and the launch size. Starting bounces work
(confirmed in game).

**Ledge edges are seams now:** a floor meeting a wall that goes down from it, like a post top, a ledge or a step down.
`sb_ledge_seam()` covers level geometry, instances and objects, with bend 0. A floor meeting a wall going up (an inside
corner) is still left out, by choice.

## 4. Predicted spots (HCM)

**Tick-aware lines** (`lineMonster` / `lineDreadnought`):

| Tick rate | Monster | Dreadnought |
|---|---|---|
| 30 | 20 | 35 |
| 60 | 35 | 60 (provisional) |

Between 30 and 60 the lines are linear. A 60-tick file no longer flags every bounce. The scanner's window columns and
merge line use the same lines.

**Display and recipe:**

- **All predictions are shown.** Monster windows under 1% get thin, dim rings. A new toggle, **"Hide narrow windows
  (under 1%)"**, is off by default; dreadnoughts are always shown.
- **The recipe is now a dense refine of the spot:**
  - a 3×3 patch 0.05 apart and 16 start heights;
  - falls 4, 4.5, 5 (starting bounces) and 6–50;
  - **legit drift only (0–2.25)**.

  Recipes run for every spot, narrow ones included. The ring text shows the exact landing point. Headlong lightpost:
  scan 25.0, refined 29.0, the user's record 28–29.
- **Scan around me** now includes the objects' triangles.

## 5. Recording and spots (HCM)

- **Force Launch is no longer recorded as a bounce or a remembered spot** (`h2cv::sb::noteForceLaunch()`, called from
  `ForceLaunch.cpp`, ignores 3 logged ticks).
- **`superbounce_log.csv` is opened shareable for reading** (`_fsopen`, `_SH_DENYWR`), so it can be read while recording.
- **Floating spots removed:** 14 remembered spots with no collision within 0.6 wu (1–2 landings each), with
  `live_logs\clean_spots.py`.

## 6. Scanner and tools (outside this repo)

- **The fast certified GPU scanner** is installed: output byte-identical to the reference, 2.3–3.5× faster.
- **The v5 re-scan** (`run_v5_all.sh`) starts with a safety check: every CPU row must appear identically in the GPU output
  on soccer (174,145 rows, 0 differences). It then installs the capped scanners and re-scans all MP maps (`results_v5`),
  then campaign (`run_campaign_v5.py`). The old-physics predictions are backed up in `pred_backup_20261101_old_physics\`.
- **Design tools** (`design\`):
  - `sb_design` (scores test geometry with the scanner's exact rules);
  - `search.py` (split-robust evolutionary search);
  - `finalize.py`, `build_blend.py` → `H2_dreadnought_test_map.blend`, a 1000-unit room (1 Blender unit = 1/100 wu, the
    toolset's ASS default) with `matte_grey)` and `+sky`;
  - `sb_bank` (the depth-bank experiment, shelved).
- **Probes and checks** (`live_logs\`):
  - `probe_force.py` (fast pill velocity / force probe);
  - `fl_lag_check.py` (replays logged launches against solver-start rules; splits the log at reverts);
  - `clean_spots.py`.

## Open

- The exact instruction behind the 4.085 cap, and its value at 60 tick.
- Seam colours on the ground, before you walk off: the physics check runs once you're airborne.
- Odd contact normals (about 1 in 5 deep contacts) are still the main gap from geometry to contacts.
