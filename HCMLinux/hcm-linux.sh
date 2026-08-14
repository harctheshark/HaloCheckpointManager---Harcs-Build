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
GAME_DIR_NAME="Halo Campaign Evolved"

die() { echo "[hcm] ERROR: $*" >&2; exit 1; }

# ---- enumerate every Steam library ------------------------------------------------------------------------
# libraryfolders.vdf is the authority: Steam libraries are frequently on other drives, and hardcoding the
# usual three paths misses them.
steam_roots() {
  for r in "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
           "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/.steam/root"; do
    [ -d "$r/steamapps" ] && echo "$r/steamapps"
  done
}
libraries() {
  local seen=""
  for base in $(steam_roots); do
    case " $seen " in *" $base "*) continue;; esac
    seen="$seen $base"; echo "$base"
    local vdf="$base/libraryfolders.vdf"
    [ -f "$vdf" ] || continue
    # "path"  "/mnt/games/SteamLibrary"  ->  /mnt/games/SteamLibrary/steamapps
    grep -oP '"path"\s*"\K[^"]+' "$vdf" 2>/dev/null | while read -r p; do
      [ -d "$p/steamapps" ] && echo "$p/steamapps"
    done
  done
}

# ---- find the game's AppID ---------------------------------------------------------------------------------
# ⚠ THIS IS THE PART THAT WAS WRONG IN THE FIRST RELEASE. It used to iterate compatdata/*/pfx and then test
# whether the GAME EXISTED IN THAT LIBRARY - a condition that is true no matter which prefix is being looked
# at - so it always picked the first one, which on most installs is compatdata/0, a Steam placeholder. HCM then
# ran in a prefix with its own separate wineserver and could never see the game process.
#
# The AppID has to come from the app manifests, which are the only thing that actually maps a name to an ID.
find_appid() {
  local lib manifest id installdir name
  for lib in $(libraries); do
    for manifest in "$lib"/appmanifest_*.acf; do
      [ -f "$manifest" ] || continue
      installdir="$(grep -oP '"installdir"\s*"\K[^"]+' "$manifest" 2>/dev/null | head -1)"
      name="$(grep -oP '"name"\s*"\K[^"]+' "$manifest" 2>/dev/null | head -1)"
      if [ "$installdir" = "$GAME_DIR_NAME" ] || [ "$name" = "$GAME_DIR_NAME" ]; then
        id="$(grep -oP '"appid"\s*"\K[0-9]+' "$manifest" 2>/dev/null | head -1)"
        [ -n "$id" ] || id="$(basename "$manifest" .acf | sed 's/appmanifest_//')"
        echo "$id|$lib"
        return 0
      fi
    done
  done
  return 1
}

if [ -z "$PREFIX" ]; then
  if [ -z "$APPID" ]; then
    found="$(find_appid || true)"
    if [ -n "$found" ]; then
      APPID="${found%%|*}"
      FOUND_LIB="${found##*|}"
      echo "[hcm] found $GAME_DIR_NAME: appid $APPID  (library: $FOUND_LIB)"
    fi
  fi

  if [ -n "$APPID" ]; then
    for lib in $(libraries); do
      if [ -d "$lib/compatdata/$APPID/pfx" ]; then PREFIX="$lib/compatdata/$APPID"; break; fi
    done
  fi
fi

if [ -z "$PREFIX" ]; then
  echo "[hcm] could not find the game's Proton prefix." >&2
  echo "[hcm] Steam libraries seen:" >&2
  for lib in $(libraries); do echo "        $lib" >&2; done
  echo "[hcm] compatdata prefixes present:" >&2
  for lib in $(libraries); do
    for d in "$lib"/compatdata/*/; do [ -d "$d" ] && echo "        $(basename "$d")  ($lib)" >&2; done
  done
  die "run the game once through Steam so the prefix is created, then retry.
       If it still fails, pass the AppID explicitly:  HCM_APPID=<appid> $0"
fi

# ⚠ Refuse the placeholder rather than run in it. appid 0 is not a game and has its own wineserver, so HCM
# would start, look healthy, and never find the game - which is exactly the failure this check exists to stop.
case "$(basename "$PREFIX")" in
  0) die "resolved to compatdata/0, which is a Steam placeholder and NOT the game's prefix.
       Pass the real AppID:  HCM_APPID=<appid> $0
       (find it in steamapps/compatdata/ - it is the numbered folder for the game)";;
esac

export WINEPREFIX="$PREFIX/pfx"
[ -d "$WINEPREFIX" ] || die "prefix has no pfx directory: $WINEPREFIX"
echo "[hcm] prefix: $WINEPREFIX"

# ---- pick a wine ------------------------------------------------------------------------------------------
# Proton's own bundled wine is the best match, because the prefix was built by it.
WINE="${HCM_WINE:-}"
if [ -z "$WINE" ]; then
  for lib in $(libraries); do
    for p in "$lib/common"/Proton*/files/bin/wine "$lib/common"/Proton*/dist/bin/wine; do
      [ -x "$p" ] && WINE="$p" && break 2
    done
  done
fi
if [ -z "$WINE" ]; then
  for p in "$HOME/.steam/steam/compatibilitytools.d"/*/files/bin/wine; do [ -x "$p" ] && WINE="$p" && break; done
fi
[ -n "$WINE" ] || WINE="$(command -v wine || true)"
[ -n "$WINE" ] || die "no wine found. Install wine, or set HCM_WINE=/path/to/wine"
echo "[hcm] wine:   $WINE"

# Proton's wine needs its own runtime on the library path; without it you get missing-.so errors that look
# like a broken install rather than a missing environment variable.
WINEDIR="$(dirname "$(dirname "$WINE")")"
for libdir in "$WINEDIR/lib" "$WINEDIR/lib64"; do
  [ -d "$libdir" ] && export LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
done

# ---- go -----------------------------------------------------------------------------------------------------
cd "$HERE" || die "cannot cd to $HERE"
[ -f "$HERE/HCMExternal.exe" ] || die "HCMExternal.exe not found next to this script"

echo "[hcm] starting. Launch the game (or leave it running); HCM injects automatically."
echo "[hcm] Ctrl+C here unloads HCM cleanly - do that rather than killing the terminal."
exec "$WINE" "$HERE/HCMExternal.exe" "$@"
