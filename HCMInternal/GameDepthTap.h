#pragma once
#include "pch.h"
#include <atomic>
#include <d3d11.h>

// HCM addition (2026-11-21, user: "look into using the depth buffer for SMAA"). A copy of the game's WORLD depth, taken
// mid-frame by a game render hook and read at Present by SMAAPostProcess (predicated edge detection: real geometry edges
// keep SMAA's threshold, texture detail / HUD get a stricter one).
//
// Why mid-frame: at Present the game's depth is no longer the world's. Halo 2 (halo2+0x197EE70 SCENE_DSV, D32_FLOAT_S8X24,
// reversed-Z) is cleared for the first-person pass and holds only the gun by then. So the Collision Viewer's Halo 2
// first-person-pass hook (CollisionViewer.cpp fpDetour, sub_1807E0C60: entry = world depth complete, gun + HUD not drawn)
// calls capture() while SMAA wants depth. Other games: no tap yet (SMAA runs without depth there).
//
// Threading: capture() and the reads run on the game's render thread (the hook and Present are both on it).
namespace GameDepthTap
{
	// SMAA, every frame at Present: whether it wants depth (the hook skips the copy otherwise), and the frame counter the
	// copy is stamped with
	void setWanted(bool wanted);
	// 2026-11-23: Halo 2 TAA (H2TAA.cpp) wants the world depth too (its reprojection). wanted() = SMAA's OR TAA's.
	void setWantedByTAA(bool wanted);
	bool wanted();
	// number of copies taken so far (TAA: "was a copy taken for THIS frame's view" without relying on the Present counter)
	uint64_t captureCount();
	uint64_t frame();          // the current frame number (advanced by endFrame)
	void endFrame();           // SMAA, after it ran this Present

	// the hook: copy this depth view's texture now (the viewport is the game's scene viewport in that texture)
	void capture(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, const D3D11_VIEWPORT& vp, bool reversedZ);

	// SMAA: this frame's copy, if one was taken since the last Present on this device. uvScale / uvOffset map back-buffer
	// texture coordinates into the copy (render scale, viewport inside a larger target).
	struct View { ID3D11ShaderResourceView* srv = nullptr; float uvScale[2]{ 1.f, 1.f }, uvOffset[2]{ 0.f, 0.f }; bool reversedZ = true; };
	bool current(ID3D11Device* dev, View& out);

	void release();   // device change / shutdown
}
