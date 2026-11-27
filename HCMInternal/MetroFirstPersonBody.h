#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-25): First Person Body - the player's own full-body model (the one the third-person cameras show,
// animated by the engine) drawn in first person, with the upper body hidden so it does not block the camera.
// Static RE: Documents\Halo Mod And Tools\Metro Redux\binary_survey\agent_legs\.
//
//  * Showing it: every frame 0x261530 sets entity+0x10C bit 1 = !IsFirstPerson() (player vtbl +0xF70); the visibility step
//    0x6D7D30 registers an entity for the main view only with that bit. While on, the `test al,al` after the call
//    (0x261562, 84 C0) is `xor eax,eax` (31 C0): the player gets the bit like every other entity. Aim, eye position and the
//    HUD call IsFirstPerson elsewhere and are untouched; the first-eye camera reads its own params, not this.
//  * Hiding the upper body: the per-bone world composer 0x7CB9F0(M, u16 parent, local, out, flag) is hooked for the player's
//    skeleton. Bones under the hidden root get their 3x3 zeroed but keep their TRUE position (children are composed from the
//    parent's true matrix), so the mesh collapses to points while anything reading a bone position (the first-eye camera
//    reads the head bone's) is unaffected. Root = the lowest common ancestor of the head and both hands (chest: head, neck,
//    arms, upper torso hidden), or the head bone only.
class MetroFirstPersonBody : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroFirstPersonBody(GameState game, IDIContainer& dicon);
	~MetroFirstPersonBody();
	std::string_view getName() override { return nameof(MetroFirstPersonBody); }
};
