# HCE Anchor Updater

**After Halo Campaign Evolved updates, double-click `Update Anchors.bat`.**

That's the whole workflow. Everything below is context for when it tells you something interesting.

---

## What problem this solves

HaloCER ships **no version resource** — every build of `HaloSimulation_tag_release.dll` reports
`0.0.0.0`. HCM cannot tell two builds apart by version, so it cannot keep a table of "addresses for
build X".

Instead HCM finds ~22 **anchors** (functions and globals it needs) by scanning for **byte
signatures** — short fingerprints of the machine code around each one — and it requires
**exactly one match**. Zero matches or two matches means the feature *switches itself off* rather
than using an address that has moved.

Two consequences worth internalising:

- **Most game updates fix themselves.** Code moves, but the fingerprint moves with it, so the scan
  still finds it. You often need to do nothing at all.
- **A broken anchor is safe.** HCM loses that one feature; it does not write to a wrong address.
  You can ship a build with broken anchors — it just does less.

This tool tells you which of the two happened, and repairs the second case where it can.

## Reading the output

```
OK    CurrentBSP        0x1809A14E0   (match rva 0x198E1D, RipRelative)
BROKE ForceRevertFlag   NO MATCH - signature no longer present
AMBIG CheckpointShell   3 matches (0x19D4C5, 0x1A02F1, 0x1B7730) - must be exactly 1
```

| | meaning | what to do |
|---|---|---|
| `OK` | resolved to exactly one place | nothing |
| `BROKE` | the code around it changed | see `anchor_repair.txt` |
| `AMBIG` | the fingerprint is no longer unique | see `anchor_repair.txt`; the signature needs *more* bytes, not fewer |

`RESULT: every anchor still resolves` means the update changed nothing HCM depends on. Ship as-is.

## When something breaks

`anchor_repair.txt` gets a block per broken anchor:

```
--- ForceRevertFlag (RipRelative, insn 0, disp 2, len 6)
    old: "C6 05 ?? ?? ?? ?? 01 48 8B 5C 24 30"
    new: "C6 05 ?? ?? ?? ?? 01 48 8B 5C 24 ??"
```

1. Open `HCMInternal/HCEAnchors.cpp`
2. Find that anchor's entry (search for its name)
3. Replace the old signature string with the new one
4. Rebuild HCM
5. Run this tool again — it should now say `OK`

The tool only proposes a `new:` line when the repaired signature is **unique**. If it says
`NEEDS A HUMAN`, the code changed too much to repair mechanically and that anchor needs someone in
a disassembler. HCM will keep working without it, minus the feature.

## How the repair works

It searches every executable byte of the DLL for the place that *most nearly* matches the old
fingerprint — allowing a few bytes to differ, which is what a recompile does when register
allocation or a nearby constant shifts. If it finds one clear winner, it rewrites just the changed
bytes as wildcards (`??`) and checks the result is still unique.

**It never guesses.** Every address it prints comes from an exact unique match. If it cannot prove
uniqueness it refuses and says so. That matters: a signature that is 16 bytes stale once shipped a
reliable crash-on-pause, because HCM *writes* to some of these sites.

Raise the search width if a repair fails:

```
py hce_anchor_update.py --repair-distance 8
```

Wider searches find more, but a candidate that differs in eight places is weak evidence — check it
in a disassembler before trusting it.

## Using the GPU

The scan is CPU-bound and takes about a second; your graphics card cannot help with it and there is
nothing to gain there.

The **repair** search is different — it compares the fingerprint against millions of positions at
once, which is exactly what a GPU is good at. If you install PyTorch with CUDA the tool picks it up
automatically and says `backend : torch (GPU)`:

```
py -m pip install torch --index-url https://download.pytorch.org/whl/cu121
```

Entirely optional. Without it you get `backend : numpy (CPU)` and everything still works — a repair
sweep over all 22 anchors takes seconds either way. Install it if you want to run
`--repair-distance 12` sweeps without waiting.

## What this tool deliberately does not do

**It does not use an AI model to work out addresses.** A language model reading raw x86-64 will
produce confident, plausible, wrong answers, and a wrong address here is worse than a missing one —
HCM patches some of these sites, so a bad address is a crash in someone else's game rather than a
disabled feature. Signature matching is exact, checkable, and explains itself. That is the right
tool for the job.

## Requirements

- Python 3.8+ (`py --version`). Get it from python.org, tick *Add python.exe to PATH*.
- The HCM source tree — the tool reads `HCMInternal/HCEAnchors.cpp` so there is one source of truth
  and it can never drift from what HCM actually scans for.
- HaloCER installed. Found automatically via Steam's library list, or pass `--game "D:\path"`.
