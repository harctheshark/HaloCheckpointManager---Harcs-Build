#!/usr/bin/env python3
"""
HCE ANCHOR UPDATER - re-derive Halo Checkpoint Manager's byte signatures after a game update.

WHAT THIS IS FOR
Halo Campaign Evolved ships no version resource: every build of HaloSimulation_tag_release.dll
reports 0.0.0.0, so HCM cannot tell two builds apart by version. Instead it locates ~22 "anchors"
(functions and globals) by scanning for byte signatures, and REQUIRES EXACTLY ONE MATCH. Zero
matches or two matches disables the feature rather than using a stale address.

That means most game updates heal themselves. This tool tells you which signatures did NOT, and
tries to repair them.

WHAT IT WILL NEVER DO
Guess. Every address it prints is either an exact unique signature match or it is reported as
broken. A confidently wrong address is far worse than a missing one - a 16-byte-stale patch site
once shipped a reliable crash-on-pause to users.

USAGE
    Update Anchors.bat                     (double-click; finds the game automatically)
    py hce_anchor_update.py                (same thing)
    py hce_anchor_update.py --game "F:\\SteamLibrary\\steamapps\\common\\Halo Campaign Evolved"
    py hce_anchor_update.py --repair-distance 6      (search harder for broken signatures)

OUTPUT
    anchor_report.txt   human-readable, one line per anchor
    anchor_repair.txt   proposed replacement signatures, ready to paste into HCEAnchors.cpp
"""

import argparse, os, re, struct, sys, hashlib, datetime

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ANCHORS_CPP = os.path.normpath(os.path.join(HERE, "..", "HCMInternal", "HCEAnchors.cpp"))
SIM_DLL_RELATIVE = os.path.join("Meteorite", "Binaries", "Win64", "HaloSimulation_tag_release.dll")


# ---------------------------------------------------------------------------------------------
# 1. Find the game.
# ---------------------------------------------------------------------------------------------
def find_game_folder(explicit=None):
    if explicit:
        return explicit if os.path.isdir(explicit) else None

    # Steam records every library root in libraryfolders.vdf. Parse the "path" values out of it
    # rather than guessing drive letters - this user's install is on F:, not the default C:.
    candidates = []
    for steam in (r"C:\Program Files (x86)\Steam", r"C:\Steam", os.environ.get("STEAM", "")):
        vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            try:
                text = open(vdf, encoding="utf-8", errors="replace").read()
                candidates += re.findall(r'"path"\s+"([^"]+)"', text)
            except OSError:
                pass
    # Plus the obvious roots, in case Steam is not where we expect.
    candidates += [f"{d}:\\SteamLibrary" for d in "CDEFGH"]

    for root in candidates:
        folder = os.path.join(root.replace("\\\\", "\\"), "steamapps", "common", "Halo Campaign Evolved")
        if os.path.isfile(os.path.join(folder, SIM_DLL_RELATIVE)):
            return folder
    return None


# ---------------------------------------------------------------------------------------------
# 2. Read the anchor table out of HCEAnchors.cpp, so there is ONE source of truth.
#    Parsing the C++ rather than keeping a copy means this tool cannot silently drift from HCM.
# ---------------------------------------------------------------------------------------------
ENTRY_RE = re.compile(
    r"\{\s*Anchor::(\w+)\s*,\s*\"(\w+)\"\s*,(.*?)Extract::(\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,",
    re.S)
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def parse_anchor_table(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    # Only the definition table, not the code below it.
    entries = []
    for m in ENTRY_RE.finditer(src):
        anchor, name, sig_span, extract, insn_off, disp_off, insn_len = m.groups()
        if "nullptr" in sig_span:
            signature = None
        else:
            parts = STRING_RE.findall(sig_span)
            signature = "".join(parts).strip() or None
        entries.append(dict(anchor=anchor, name=name, signature=signature, extract=extract,
                            insn_off=int(insn_off), disp_off=int(disp_off), insn_len=int(insn_len)))
    return entries


# ---------------------------------------------------------------------------------------------
# 3. PE parsing - executable sections only, matching HCESignatureScan::scanExecutableSections.
# ---------------------------------------------------------------------------------------------
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def load_pe(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path} is not a PE image")
    n_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic != 0x20B:
        raise SystemExit("only PE32+ (64-bit) is supported")
    image_base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    sec_off = pe + 24 + opt_size

    sections = []
    for i in range(n_sections):
        o = sec_off + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rsize, ptr = struct.unpack_from("<IIII", data, o + 8)
        chars = struct.unpack_from("<I", data, o + 36)[0]
        sections.append(dict(name=name, va=va, vsize=vsize, ptr=ptr, rsize=rsize, exec=bool(chars & IMAGE_SCN_MEM_EXECUTE)))

    # TLS directory (data directory index 9) -> AddressOfIndex, for Anchor::TlsIndex.
    tls_index_va = 0
    n_dirs = struct.unpack_from("<I", data, pe + 24 + 108)[0]
    if n_dirs > 9:
        tls_rva, tls_size = struct.unpack_from("<II", data, pe + 24 + 112 + 9 * 8)
        if tls_rva:
            off = rva_to_offset(sections, tls_rva)
            if off is not None:
                # IMAGE_TLS_DIRECTORY64: StartAddressOfRawData, EndAddress, AddressOfIndex, ...
                tls_index_va = struct.unpack_from("<Q", data, off + 16)[0]
    return dict(data=data, sections=sections, image_base=image_base, tls_index_va=tls_index_va,
                path=path, md5=hashlib.md5(data).hexdigest(), size=len(data))


def rva_to_offset(sections, rva):
    for s in sections:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rsize"]):
            return s["ptr"] + (rva - s["va"])
    return None


def offset_to_rva(sections, off):
    for s in sections:
        if s["ptr"] <= off < s["ptr"] + s["rsize"]:
            return s["va"] + (off - s["ptr"])
    return None


# ---------------------------------------------------------------------------------------------
# 4. Pattern matching. parse_pattern mirrors HCESignatureScan::parsePattern exactly.
# ---------------------------------------------------------------------------------------------
def parse_pattern(text):
    """'48 8B 05 ?? ?? ?? ??' -> (bytes, wildcard mask). Whitespace ignored, ?? = wildcard."""
    vals, wild, i = [], [], 0
    while i < len(text):
        c = text[i]
        if c in " \t":
            i += 1
            continue
        if c == "?":
            vals.append(0); wild.append(True)
            i += 2 if i + 1 < len(text) and text[i + 1] == "?" else 1
            continue
        vals.append(int(text[i:i + 2], 16)); wild.append(False)
        i += 2
    return bytes(vals), wild


def _backend():
    """CUDA if the user has it, else numpy. Only the fuzzy REPAIR search is heavy enough to care."""
    try:
        import torch
        if torch.cuda.is_available():
            return "torch", torch
    except Exception:
        pass
    try:
        import cupy
        return "cupy", cupy
    except Exception:
        pass
    import numpy
    return "numpy", numpy


def find_matches(pe, pattern_text, limit=64):
    """Exact matches (honouring wildcards) across EXECUTABLE sections only. Returns file offsets."""
    pat, wild = parse_pattern(pattern_text)
    fixed = [(i, b) for i, (b, w) in enumerate(zip(pat, wild)) if not w]
    if not fixed:
        return []
    data = pe["data"]
    hits = []
    # Anchor the search on the rarest fixed byte to keep the candidate set small.
    lead_i, lead_b = fixed[0]
    for s in pe["sections"]:
        if not s["exec"]:
            continue
        start, end = s["ptr"], s["ptr"] + s["rsize"] - len(pat)
        pos = data.find(bytes([lead_b]), start + lead_i, end + lead_i)
        while pos != -1 and len(hits) < limit:
            base = pos - lead_i
            if base >= start and all(data[base + i] == b for i, b in fixed):
                hits.append(base)
            pos = data.find(bytes([lead_b]), pos + 1, end + lead_i)
    return hits


def fuzzy_candidates(pe, pattern_text, max_mismatch, xp_name, xp, top=5):
    """
    REPAIR SEARCH. Find places that ALMOST match - the recompiled version of the same code, where a
    few operand bytes moved. Returns [(offset, mismatch_count)] best first.

    This is the only expensive step (millions of offsets x pattern length), and the only one that
    benefits from a GPU.
    """
    import numpy as np
    pat, wild = parse_pattern(pattern_text)
    idx = np.array([i for i, w in enumerate(wild) if not w], dtype=np.int64)
    vals = np.array([pat[i] for i in idx], dtype=np.uint8)
    if idx.size == 0:
        return []

    out = []
    for s in pe["sections"]:
        if not s["exec"] or s["rsize"] <= len(pat):
            continue
        buf = np.frombuffer(pe["data"], dtype=np.uint8, count=s["rsize"], offset=s["ptr"])
        n = buf.size - len(pat)
        if n <= 0:
            continue

        if xp_name == "torch":
            t = xp.from_numpy(buf.copy()).cuda()
            ti = xp.from_numpy(idx).cuda()
            tv = xp.from_numpy(vals).cuda()
            base = xp.arange(n, device="cuda").unsqueeze(1)
            mism = xp.zeros(n, dtype=xp.int16, device="cuda")
            CH = 4096  # chunk the pattern positions so we never allocate n x len(pat) at once
            for k in range(0, ti.numel(), CH):
                cols = base + ti[k:k + CH].unsqueeze(0)
                mism += (t[cols] != tv[k:k + CH].unsqueeze(0)).sum(dim=1).to(xp.int16)
            mism = mism.cpu().numpy()
        else:
            mism = np.zeros(n, dtype=np.int16)
            for i, v in zip(idx.tolist(), vals.tolist()):
                mism += (buf[i:i + n] != v)

        good = np.nonzero(mism <= max_mismatch)[0]
        for g in good.tolist():
            out.append((s["ptr"] + g, int(mism[g])))
    out.sort(key=lambda x: x[1])
    return out[:top]


# ---------------------------------------------------------------------------------------------
# 5. Resolve an anchor the same way HCEAnchors::resolveAll does.
# ---------------------------------------------------------------------------------------------
def resolve(pe, entry, match_off):
    """Returns (rva, va) of what the anchor actually points at, or (None, None)."""
    sections, base = pe["sections"], pe["image_base"]
    match_rva = offset_to_rva(sections, match_off)
    if match_rva is None:
        return None, None
    if entry["extract"] == "MatchIsTarget":
        return match_rva, base + match_rva

    if entry["extract"] == "RipRelative":
        # HCESignatureScan::ripTarget(instruction, dispOffset, insnLength)
        #   target = instruction + insnLength + int32_at(instruction + dispOffset)
        insn_rva = match_rva + entry["insn_off"]
        disp_off = rva_to_offset(sections, insn_rva + entry["disp_off"])
        if disp_off is None:
            return None, None
        disp = struct.unpack_from("<i", pe["data"], disp_off)[0]
        target_rva = insn_rva + entry["insn_len"] + disp
        return target_rva, base + target_rva
    return None, None


# ---------------------------------------------------------------------------------------------
# 6. Drive it.
# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Re-derive HCM's byte signatures after a HaloCER update.")
    ap.add_argument("--game", help="Halo Campaign Evolved folder (auto-detected if omitted)")
    ap.add_argument("--anchors", default=DEFAULT_ANCHORS_CPP, help="path to HCEAnchors.cpp")
    ap.add_argument("--repair-distance", type=int, default=4,
                    help="how many mismatched bytes a repair candidate may have (default 4)")
    ap.add_argument("--out", default=HERE, help="where to write the reports")
    args = ap.parse_args()

    game = find_game_folder(args.game)
    if not game:
        print("!! Could not find 'Halo Campaign Evolved'.")
        print("   Pass it explicitly:  py hce_anchor_update.py --game \"D:\\path\\to\\Halo Campaign Evolved\"")
        return 2
    dll = os.path.join(game, SIM_DLL_RELATIVE)
    if not os.path.isfile(dll):
        print(f"!! Found the game at {game} but not {SIM_DLL_RELATIVE}")
        return 2

    if not os.path.isfile(args.anchors):
        print(f"!! Cannot read the anchor table at {args.anchors}")
        print("   This tool lives in the HCM source tree and reads HCMInternal/HCEAnchors.cpp.")
        return 2

    entries = parse_anchor_table(args.anchors)
    if not entries:
        print("!! Parsed ZERO anchors out of HCEAnchors.cpp - its table format must have changed.")
        print("   Nothing was checked. Do not treat this as a pass.")
        return 3

    pe = load_pe(dll)
    xp_name, xp = _backend()

    print("=" * 78)
    print(" HCE ANCHOR UPDATER")
    print("=" * 78)
    print(f" game    : {game}")
    print(f" sim dll : {pe['size']:,} bytes   md5 {pe['md5']}")
    print(f" anchors : {len(entries)} from {os.path.basename(args.anchors)}")
    print(f" backend : {xp_name}" + ("  (GPU)" if xp_name in ("torch", "cupy") else "  (CPU - install torch+CUDA to use the 4090 for repairs)"))
    print()

    ok, ambiguous, missing, structural = [], [], [], []
    lines = []

    for e in entries:
        if e["extract"] == "TlsDirectory":
            va = pe["tls_index_va"]
            status = "OK  " if va else "FAIL"
            (structural if va else missing).append(e)
            lines.append(f"{status}  {e['name']:<26} 0x{va:X}   (PE TLS directory, not a signature)")
            continue

        if not e["signature"]:
            lines.append(f"SKIP  {e['name']:<26} no signature in the table")
            continue

        hits = find_matches(pe, e["signature"])
        if len(hits) == 1:
            rva, va = resolve(pe, e, hits[0])
            ok.append(e)
            match_rva = offset_to_rva(pe["sections"], hits[0])
            detail = f"0x{va:X}" if va else "resolve FAILED"
            lines.append(f"OK    {e['name']:<26} {detail}   (match rva 0x{match_rva:X}, {e['extract']})")
        elif len(hits) == 0:
            missing.append(e)
            lines.append(f"BROKE {e['name']:<26} NO MATCH - signature no longer present")
        else:
            ambiguous.append(e)
            rvas = ", ".join(f"0x{offset_to_rva(pe['sections'], h):X}" for h in hits[:6])
            lines.append(f"AMBIG {e['name']:<26} {len(hits)} matches ({rvas}) - must be exactly 1")

    for l in lines:
        print(" " + l)

    print()
    print(f" {len(ok)} resolved, {len(structural)} structural, {len(ambiguous)} ambiguous, {len(missing)} broken")

    # ---- repairs -----------------------------------------------------------------------------
    repairs = []
    broken = missing + ambiguous
    if broken:
        print()
        print("-" * 78)
        print(f" REPAIR SEARCH for {len(broken)} anchor(s), up to {args.repair_distance} mismatched bytes")
        print("-" * 78)
        for e in broken:
            if not e["signature"]:
                continue
            cands = fuzzy_candidates(pe, e["signature"], args.repair_distance, xp_name, xp)
            if not cands:
                print(f" {e['name']}: no near match within {args.repair_distance} bytes. The code has changed")
                print(f"   too much for an automatic repair - this one needs a human in a disassembler.")
                repairs.append((e, None, None))
                continue

            best_off, best_mism = cands[0]
            pat, wild = parse_pattern(e["signature"])
            actual = pe["data"][best_off:best_off + len(pat)]
            # Wildcard exactly the bytes that changed, keep everything else fixed.
            new_wild = list(wild)
            for i, (b, w) in enumerate(zip(pat, wild)):
                if not w and actual[i] != b:
                    new_wild[i] = True
            new_sig = " ".join("??" if new_wild[i] else f"{actual[i]:02X}" for i in range(len(pat)))

            # A repair is only useful if the NEW signature is unique.
            uniq = len(find_matches(pe, new_sig, limit=8))
            rva = offset_to_rva(pe["sections"], best_off)
            verdict = "UNIQUE" if uniq == 1 else f"{uniq} matches - NOT USABLE AS-IS"
            print(f" {e['name']}: nearest match at rva 0x{rva:X}, {best_mism} byte(s) differ -> {verdict}")
            if uniq == 1:
                tmp = dict(e); rva2, va2 = resolve(pe, tmp, best_off)
                print(f"   would resolve to 0x{va2:X}" if va2 else "   (resolve failed)")
            repairs.append((e, new_sig if uniq == 1 else None, best_off))

    # ---- write the reports -------------------------------------------------------------------
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    rep = os.path.join(args.out, "anchor_report.txt")
    with open(rep, "w", encoding="utf-8") as f:
        f.write(f"HCE anchor report  {stamp}\ngame: {game}\nsim dll md5: {pe['md5']}  ({pe['size']} bytes)\n\n")
        f.write("\n".join(lines))
        f.write(f"\n\n{len(ok)} resolved, {len(structural)} structural, {len(ambiguous)} ambiguous, {len(missing)} broken\n")
    print(f"\n wrote {rep}")

    if repairs:
        fix = os.path.join(args.out, "anchor_repair.txt")
        with open(fix, "w", encoding="utf-8") as f:
            f.write(f"Proposed signature repairs  {stamp}\n")
            f.write("Paste the new signature string over the old one in HCMInternal/HCEAnchors.cpp,\n")
            f.write("then REBUILD HCM and run this tool again - every anchor should read OK.\n")
            f.write("Anything marked NEEDS A HUMAN could not be repaired automatically.\n\n")
            for e, new_sig, off in repairs:
                f.write(f"--- {e['name']} ({e['extract']}, insn {e['insn_off']}, disp {e['disp_off']}, len {e['insn_len']})\n")
                f.write(f"    old: \"{e['signature']}\"\n")
                if new_sig:
                    f.write(f"    new: \"{new_sig}\"\n")
                else:
                    f.write("    NEEDS A HUMAN - no unique near match.\n")
                f.write("\n")
        print(f" wrote {fix}")

    print()
    if not broken:
        print(" RESULT: every anchor still resolves. HCM needs no signature changes for this build.")
        return 0
    print(f" RESULT: {len(broken)} anchor(s) need attention. See anchor_repair.txt.")
    print("         HCM will DISABLE the affected features rather than use a wrong address,")
    print("         so the build is safe to ship meanwhile - just missing those features.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
