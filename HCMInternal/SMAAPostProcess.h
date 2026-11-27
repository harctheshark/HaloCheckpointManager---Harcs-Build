#pragma once
#include "pch.h"
#include "D3D11Hook.h"

class SettingsStateAndEvents;

// HCM addition (2026-11-20, user: "Did we ever add SMAA to HCM? It'd be nice to add ... i do not want to have to launch
// reshade seperately"). SMAA 1x (Jorge Jimenez et al., MIT - SMAA\SMAA_LICENSE.txt) on MCC's back buffer, in HCM's own
// D3D11 Present hook (since 2026-11-21 on D3D11Hook::preCapturePresentEvent: before OBS's capture even with the OBS bypass on,
// so recordings are smoothed too): it runs before every other Present listener, so the game's frame and HUD are smoothed and
// HCM's own window / overlays drawn after it are not. Like ReShade's SMAA it works on the finished frame (luma edges, no
// depth). Options: smaaToggle (+ hotkey) and smaaQuality (SMAA's Low / Medium / High / Ultra presets; shaders precompiled
// per preset in SMAA\Generated\SMAAShaders.h by SMAA\build_shaders.py). MCC (D3D11) only.
// Saves and restores every bit of D3D11 state it touches: MCC's renderer caches device state between frames.
class SMAAPostProcess
{
	struct Impl;
	std::shared_ptr<Impl> pimpl;   // shared: a Present call already in flight keeps it alive
public:
	SMAAPostProcess(std::shared_ptr<D3D11Hook> d3d, std::shared_ptr<SettingsStateAndEvents> settings);
	~SMAAPostProcess();
};
