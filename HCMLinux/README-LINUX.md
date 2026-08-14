# HCM on Linux — Halo Campaign Evolved

**Status: EXPERIMENTAL, and untested on Linux.** It was built and sanity-checked on Windows only. Please
report what happens — the failure modes below are predictions, not observations.

## Quick start

```bash
chmod +x hcm-linux.sh
./hcm-linux.sh
```

Then launch Halo Campaign Evolved (or leave it running — order doesn't matter). HCM injects automatically and
keeps retrying. **All of HCM's interface is the in-game overlay** — open it there, same key as on Windows.

Press **Ctrl+C** in the terminal to unload HCM cleanly. Do that rather than closing the terminal or killing
the process.

## Why there is no "native" Linux build

Halo Campaign Evolved is a Windows game; on Linux it runs under Proton (Wine + VKD3D-Proton), so the game
process is a **Windows** process. HCM's injected component hooks that process's D3D12 swapchain from inside
it. A native Linux `.so` can't take part in that — no `LoadLibrary` semantics, no way to hook Wine's PE
modules, and none of the hooking machinery applies.

So everything here is Windows binaries running **inside the game's own Wine prefix**. That's not a shortcut;
it's the only arrangement that can work.

The one piece that genuinely can't come along is the GUI: HCM's launcher is WPF, which has never been ported
to Linux and behaves badly under Wine. It isn't much of a loss — HCM renders its real UI as an ImGui overlay
*inside the game*. `HCMExternal.exe` here is a small console replacement that does what the WPF app did minus
the window: set up shared memory, find the game, inject, and stay alive.

## Requirements

- The game **launched at least once through Steam/Proton**, so the prefix exists.
- `wine` — the script prefers Proton's bundled wine (it matches the prefix that Proton built).

Overrides, if auto-detection misses:

```bash
HCM_APPID=1234567 ./hcm-linux.sh          # force a specific Steam AppID
HCM_WINE=/usr/bin/wine ./hcm-linux.sh     # force a specific wine
```

## Two things that will bite you

**Leave the terminal running.** HCM's injected DLL has a heartbeat that looks for a process named
`HCMExternal.exe` or `HaloCheckpointManager.exe`. If it's gone for ~3 seconds, the DLL **unloads itself** —
deliberately, so an orphaned DLL can't pin itself inside your game. Close the terminal and HCM disappears
from the game.

**Don't rename `HCMExternal.exe`.** Same reason. The launcher warns you if you have.

## What's supported

Halo Campaign Evolved only, for now. The MCC titles are not covered by this build — not because they can't
be, but because nothing here has been tested against them.

## Expected failure modes (untested — please report)

| Symptom | Likely cause |
|---|---|
| HCM injects, then vanishes after ~3 s | The launcher process exited, or was renamed. Keep the terminal open. |
| Overlay never appears, game runs fine | The D3D12 hook didn't take against VKD3D-Proton. This is the biggest unknown, and the most likely thing to be broken. |
| `could not find the game's Proton prefix` | Run the game once through Steam first, or pass `HCM_APPID=`. |
| Injects but the game crashes on overlay open | ImGui's D3D12 backend vs VKD3D. Report the log. |
| `interproc failed to initialise` | Another HCM already running in that prefix. |

Logs land in `Logs/` next to the launcher, exactly as on Windows — attach those to any report.

## What is genuinely unknown

The D3D12 hook is the real question. On Windows it hooks a real D3D12 swapchain; under Proton that swapchain
is VKD3D-Proton's implementation on top of Vulkan. Hooking `Present` on it *should* work, since it's a normal
COM vtable — but "should" is doing a lot of work in that sentence, and nobody has run it yet.
