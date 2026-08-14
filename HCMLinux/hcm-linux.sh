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

# Wine's fixme/err chatter is not useful here and buries our own output. Override with HCM_DEBUG=1.
[ "${HCM_DEBUG:-0}" = "1" ] || export WINEDEBUG="${WINEDEBUG:--all}"

die() { echo "[hcm] ERROR: $*" >&2; exit 1; }

# ---- enumerate every Steam library ------------------------------------------------------------------------
steam_roots() {
  for r in "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
           "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/.steam/root"; do
    [ -d "$r/steamapps" ] && echo "$r"
  done
}
libraries() {
  local seen=""
  for root in $(steam_roots); do
    local base="$root/steamapps"
    case " $seen " in *" $base "*) continue;; esac
    seen="$seen $base"; echo "$base"
    local vdf="$base/libraryfolders.vdf"
    [ -f "$vdf" ] || continue
    grep -oP '"path"\s*"\K[^"]+' "$vdf" 2>/dev/null | while read -r p; do
      [ -d "$p/steamapps" ] && echo "$p/steamapps"
    done
  done
}
client_install_path() { steam_roots | head -1; }

# ---- find the game's AppID ---------------------------------------------------------------------------------
# ⚠ THIS WAS WRONG IN THE FIRST RELEASE. It iterated compatdata/*/pfx and then tested whether the GAME EXISTED
# IN THAT LIBRARY - true regardless of which prefix was being examined - so it always took the first, which on
# most installs is compatdata/0, a Steam placeholder with its own wineserver. HCM ran there looking perfectly
# healthy and could never have seen the game.
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
        echo "$id|$lib"; return 0
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
      echo "[hcm] found $GAME_DIR_NAME: appid $APPID  (library: ${found##*|})"
    fi
  fi
  if [ -n "$APPID" ]; then
    for lib in $(libraries); do
      [ -d "$lib/compatdata/$APPID/pfx" ] && PREFIX="$lib/compatdata/$APPID" && break
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
       If it still fails:  HCM_APPID=<appid> $0"
fi

case "$(basename "$PREFIX")" in
  0) die "resolved to compatdata/0, a Steam placeholder and NOT the game's prefix.
       Pass the real AppID:  HCM_APPID=<appid> $0";;
esac
[ -d "$PREFIX/pfx" ] || die "prefix has no pfx directory: $PREFIX/pfx"
echo "[hcm] prefix: $PREFIX/pfx"

[ -f "$HERE/HCMExternal.exe" ] || die "HCMExternal.exe not found next to this script"
cd "$HERE" || die "cannot cd to $HERE"

# ================================================================================================================
# ---- locate Proton -- AND REFUSE TO RUN THE WRONG ONE ----------------------------------------------------------
#
# ⚠⚠ THIS DAMAGED A USER'S PREFIX AND THE FIX IS TO REFUSE, NOT TO GUESS.
#
# The previous version tried to prefer the Proton the prefix was built with, but the loop assigned
# PROTON="$p" for EVERY candidate and only stopped on a match - and the match compared a directory name
# against the contents of config_info, which is not that. So when nothing matched it silently kept whatever
# came last, which on the reporting machine was GE-Proton9-5 from compatibilitytools.d. `proton run` then did
# what it is supposed to do with a mismatched prefix:
#
#     Proton: Upgrading prefix from 11.0-100 to GE-Proton9-5
#     Proton: Prefix has an invalid version?! You may want to back up user files and delete this prefix.
#     wine: configuration in ".../compatdata/2806050/pfx" has been updated.
#
# The game then crashed in its OWN code (null deref, no HCM module in the dump at all) because it was running
# in a prefix that had been rewritten for a different Proton.
#
# A prefix downgrade is not something a third-party tool may do by accident. So: read the version the prefix
# actually is, require an EXACT match, and if there is no match, STOP and say so. Running the wrong Proton is
# strictly worse than not running at all.
# ================================================================================================================
proton_version_of() {   # $1 = a proton install dir
  [ -f "$1/version" ] && head -1 "$1/version" 2>/dev/null | tr -d '\r\n' && return 0
  basename "$1"
}

PROTON="${HCM_PROTON:-}"
PREFIX_VER=""
[ -f "$PREFIX/version" ] && PREFIX_VER="$(head -1 "$PREFIX/version" 2>/dev/null | tr -d '\r\n')"

if [ -z "$PROTON" ]; then
  if [ -z "$PREFIX_VER" ]; then
    die "cannot tell which Proton built this prefix ($PREFIX/version is missing).
       Refusing to guess - running the wrong Proton REWRITES the prefix and breaks the game.
       Launch the game once through Steam, then retry, or name it explicitly:
           HCM_PROTON=/path/to/Proton/proton $0"
  fi
  echo "[hcm] prefix was built by Proton: $PREFIX_VER"

  for lib in $(libraries); do
    for p in "$lib/common"/Proton*/proton; do
      [ -f "$p" ] || continue
      v="$(proton_version_of "$(dirname "$p")")"
      if [ "$v" = "$PREFIX_VER" ]; then PROTON="$p"; break 2; fi
    done
  done
  if [ -z "$PROTON" ]; then
    for p in "$HOME/.steam/steam/compatibilitytools.d"/*/proton \
             "$HOME/.local/share/Steam/compatibilitytools.d"/*/proton; do
      [ -f "$p" ] || continue
      v="$(proton_version_of "$(dirname "$p")")"
      if [ "$v" = "$PREFIX_VER" ]; then PROTON="$p"; break; fi
    done
  fi

  if [ -z "$PROTON" ]; then
    echo "[hcm] could not find the Proton that built this prefix ($PREFIX_VER)." >&2
    echo "[hcm] Proton installs found:" >&2
    for lib in $(libraries); do
      for p in "$lib/common"/Proton*/proton; do
        [ -f "$p" ] && echo "        $(proton_version_of "$(dirname "$p")")   $(dirname "$p")" >&2
      done
    done
    for p in "$HOME/.steam/steam/compatibilitytools.d"/*/proton; do
      [ -f "$p" ] && echo "        $(proton_version_of "$(dirname "$p")")   $(dirname "$p")" >&2
    done
    die "REFUSING TO CONTINUE. Running a different Proton would rewrite the prefix and break the game.
       Install/select the matching Proton in Steam, or override deliberately:
           HCM_PROTON=/path/to/proton $0"
  fi
else
  # Explicit override still gets checked, because the failure mode is a broken game, not a broken tool.
  if [ -n "$PREFIX_VER" ]; then
    v="$(proton_version_of "$(dirname "$PROTON")")"
    if [ "$v" != "$PREFIX_VER" ]; then
      echo "[hcm] WARNING: HCM_PROTON is '$v' but the prefix was built by '$PREFIX_VER'." >&2
      echo "[hcm]          Proton WILL rewrite the prefix. Ctrl+C now unless you meant this." >&2
      sleep 5
    fi
  fi
fi

# ================================================================================================================
# ⚠ RUN THROUGH `proton run`, NOT BY CALLING PROTON'S wine BINARY DIRECTLY.
#
# The first release did the latter, and the result was a screenful of
#     err:setupapi:create_dest_file failed to create L"C:\\windows\\system32\\..." (error=80)
# That is wineboot deciding the prefix needs rebuilding and then failing on every file because it already
# exists (error 80 = ERROR_FILE_EXISTS). It happened to be harmless - nothing was overwritten - but it is
# wineboot writing to the GAME'S prefix, which is not something a third-party tool should be provoking, and on
# a version mismatch it can rewrite the prefix's registry and break the game's Proton setup.
#
# `proton run` is the supported entry point: it sets the prefix up the way Steam does and does not trigger a
# naive rebuild. Direct wine remains only as a last resort, with a warning.
# ================================================================================================================
if [ -n "$PROTON" ] && [ -f "$PROTON" ]; then
  echo "[hcm] proton: $PROTON"
  export STEAM_COMPAT_DATA_PATH="$PREFIX"
  export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$(client_install_path)}"
  echo "[hcm] starting. Launch the game (or leave it running); HCM injects automatically."
  echo "[hcm] Ctrl+C here unloads HCM cleanly - do that rather than killing the terminal."
  exec "$PROTON" run "$HERE/HCMExternal.exe" "$@"
fi

echo "[hcm] WARNING: no Proton 'proton' script found; falling back to calling wine directly." >&2
echo "[hcm]          This can make wineboot try to update the game's prefix (harmless-looking" >&2
echo "[hcm]          setupapi errors, but it is writing to the prefix). Set HCM_PROTON=/path/to/proton" >&2
echo "[hcm]          to avoid it." >&2

WINE="${HCM_WINE:-}"
if [ -z "$WINE" ]; then
  for lib in $(libraries); do
    for p in "$lib/common"/Proton*/files/bin/wine "$lib/common"/Proton*/dist/bin/wine; do
      [ -x "$p" ] && WINE="$p" && break 2
    done
  done
fi
[ -n "$WINE" ] || WINE="$(command -v wine || true)"
[ -n "$WINE" ] || die "no wine found. Install wine, or set HCM_WINE=/path/to/wine"
echo "[hcm] wine:   $WINE"

export WINEPREFIX="$PREFIX/pfx"
WINEDIR="$(dirname "$(dirname "$WINE")")"
for libdir in "$WINEDIR/lib" "$WINEDIR/lib64"; do
  [ -d "$libdir" ] && export LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
done

echo "[hcm] starting. Launch the game (or leave it running); HCM injects automatically."
echo "[hcm] Ctrl+C here unloads HCM cleanly - do that rather than killing the terminal."
exec "$WINE" "$HERE/HCMExternal.exe" "$@"
