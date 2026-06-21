# HaloCheckpointManager — Harc's Build
*A practice tool for [Halo: The Master Chief Collection](https://store.steampowered.com/app/976730/Halo_The_Master_Chief_Collection/)*

> **This is "Harc's Build"** — a Halo 2-focused fork of [HaloCheckpointManager by Burnt-o](https://github.com/Burnt-o/HaloCheckpointManager). It adds the changes listed below on top of upstream HCM. Address-specific features target **MCC Steam build 1.3528**; other builds fall back to stock behaviour.

---**Harc's Build — what's new**---
* **Halo 2 multiplayer fixes**
   * The on-screen player info (game tick, position, velocity, look angle) now works in Halo 2 **multiplayer**, not just campaign.
   * Player info reads the correct local-player slot in MP, so it tracks **you** as a client — not just the host.
* **Replay system (pseudo-theater)** — record the player's per-tick inputs and replay them from a matching checkpoint (HCM's checkpoint system handles RNG/determinism), with an optional first-person "theater" camera. **Freecam-friendly:** while HCM freecam is active the replay yields the camera so you can free-cam / dollycam montages over a replay while the inputs keep playing.
* **Season 7 Physics** — toggle the physics/tickrate collision scalar (applied on level load).
* **Master Tickrate** — button to flip the simulation tickrate between **60 and 30 Hz**.
* **Far Clip Distance** — adjust the renderer far clip (±-512 world units per step, or ctrl+click to type a value).
* **Skull-enable mask auto-poke** — skull toggles take effect in Halo 2 multiplayer without the extra manual step.
* **Toggle Rocket Launcher Animation Fix** — fixes the first-person rocket launcher animation pop.

---**Download**---  
From the [Releases page](https://github.com/harctheshark/HaloCheckpointManager---Harcs-Build/releases) (Harc's Build) — or upstream HCM from the [original Releases page](https://github.com/Burnt-o/HaloCheckpointManager/releases).  

---**Features**---
* Back up (dump) to file the games checkpoints, so you can inject them again at any later time. 
* UI for managing your checkpoint collection (ordering, sorting, renaming etc).
* Single-player cheats to help you practice and route the games, such as
   * Force custom checkpoints & back them up
   * Invincibility
   * Speedhack (both slow-down and fast-forward)
   * Block the games natural checkpoints
   * Teleport and Launch the player
   * And more (specific games have different features)
 * Support for multiple downpatched versions of MCC - Season 7 (2448), Season 8 (2645), and of course current patch.

---**Building from source**---   
To build the source yourself, grab the latest version of [Visual Studio](https://visualstudio.microsoft.com/) and the following workloads/components:  
 * .NET desktop development (and .NET 7.0 runtime)  
 * Desktop development with C++  
 * Windows 10 SDK (or higher)
    
HCM uses vcpkg manifest files for C++ dependecies. With Visual Studio this should all be automatic. 
I haven't tried building outside of this VS environment so YMMV if you try otherwise. 

This project also makes heavy use of the amazing SafetyHook by cursey. You can think of it as like "Microsoft Detours but if it didn't suck". I've included the release I'm using in this repository so you shouldn't need to worry about it.

After successfully building all projects (besides HCMInternalTests, that doesn't matter), make sure to rebuild the entire solution to ensure all build events fire correctly.

Known build issue: Your build events may fail to copy over certain files to the correct directory due to macro issues: fix this by adjusting the build event macros for each project to make sure they're outputting to HCMExternal's binary output directory.
