#!/usr/bin/env bash
# ================================================================================================================
# HCM for Halo Campaign Evolved on Linux (Proton / Wine).
#
# HCM is a set of WINDOWS binaries and that is not a shortcut - it is the only thing that can work. The game is
# a Windows binary running under Proton, and HCMInternal.dll hooks its D3D12 swapchain from inside that process.
# So HCM has to run in THE SAME WINE PREFIX as the game, which is what this script arranges.
#
# ⚠ THE GAME MUST HAVE BEEN LAUNCHED AT LEAST ONCE through Steam/Proton, so the prefix exists.
# ⚠ Leave this script running for as long as you want HCM active. HCMInternal's heartbeat looks for the
#   launcher process and unloads itself within ~3 seconds if it is gone. Ctrl+C unloads HCM cleanly.
# ================================================================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPID="${HCM_APPID:-}"          # override: HCM_APPID=1234567 ./hcm-linux.sh
PREFIX="${STEAM_COMPAT_DATA_PATH:-}"

die() { echo "[hcm] ERROR: $*" >&2; exit 1; }

# ---- locate the game's Proton prefix ---------------------------------------------------------------------
if [ -z "$PREFIX" ]; then
  for lib in \
      "$HOME/.steam/steam/steamapps" \
      "$HOME/.local/share/Steam/steamapps" \
      "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps" \
      /run/media/*/steamapps /mnt/*/steamapps; do
    [ -d "$lib/compatdata" ] || continue
    if [ -n "$APPID" ]; then
      [ -d "$lib/compatdata/$APPID" ] && PREFIX="$lib/compatdata/$APPID" && break
    else
      # Find the prefix that actually contains the game.
      for candidate in "$lib"/compatdata/*/pfx; do
        [ -d "$candidate" ] || continue
        if compgen -G "$lib/common/Halo Campaign Evolved" >/dev/null 2>&1; then
          PREFIX="$(dirname "$candidate")"; break 2
        fi
      done
    fi
  done
fi

[ -n "$PREFIX" ] || die "could not find the game's Proton prefix.
       Run the game once through Steam, then re-run this. If it still fails, pass the AppID:
           HCM_APPID=<appid> $0
       (the AppID is the number in steamapps/compatdata/ for Halo Campaign Evolved)"

export WINEPREFIX="$PREFIX/pfx"
[ -d "$WINEPREFIX" ] || die "prefix has no pfx directory: $WINEPREFIX"
echo "[hcm] prefix: $WINEPREFIX"

# ---- pick a wine ------------------------------------------------------------------------------------------
# Proton's own bundled wine is the best match for the prefix, because the prefix was built by it. A system
# wine against a Proton-built prefix can work but is more likely to complain about version mismatches.
WINE="${HCM_WINE:-}"
if [ -z "$WINE" ]; then
  for p in "$HOME/.steam/steam/steamapps/common"/Proton*/files/bin/wine \
           "$HOME/.local/share/Steam/steamapps/common"/Proton*/files/bin/wine \
           "$HOME/.steam/steam/compatibilitytools.d"/*/files/bin/wine; do
    [ -x "$p" ] && WINE="$p" && break
  done
fi
[ -n "$WINE" ] || WINE="$(command -v wine || true)"
[ -n "$WINE" ] || die "no wine found. Install wine, or set HCM_WINE=/path/to/wine"
echo "[hcm] wine:   $WINE"

# ---- go -----------------------------------------------------------------------------------------------------
cd "$HERE" || die "cannot cd to $HERE"
[ -f "$HERE/HCMExternal.exe" ] || die "HCMExternal.exe not found next to this script"

echo "[hcm] starting. Launch the game (or leave it running); HCM injects automatically."
echo "[hcm] Ctrl+C here unloads HCM cleanly - do that rather than killing the terminal."
exec "$WINE" "$HERE/HCMExternal.exe" "$@"
