#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - trigger-volume ACTIVITY tracker.
//
// Answers one question: "is the mission script currently testing this trigger volume?", so a live volume can be
// drawn one colour and a dormant one another.
//
// ---------------------------------------------------------------------------------------------------------------
// WHY THIS REPLACES THE SCRIPT-CORPUS INFERENCE
// ---------------------------------------------------------------------------------------------------------------
// The first version of the overlay answered this statically: it scraped `VOLUMES.<name>` references out of the
// 109 decompiled script tags and classified the enclosing call. That tells you whether a volume is referenced
// ANYWHERE in the level's script - not whether anything is watching it right now. A volume inside a goal the
// player finished an hour ago scored exactly the same as the one the current goal is waiting on, and a volume
// whose level was not in the corpus scored as nothing at all.
//
// The ground truth is the TEST CALL. A volume is live iff a running script is evaluating it:
//
//     active(v)  ==  (now - lastTested[v]) < window
//
// ---------------------------------------------------------------------------------------------------------------
// WHY HOOKING THE SCRIPT BINDINGS IS CORRECT HERE (AND WHY HaloCER COULD NOT DO THIS)
// ---------------------------------------------------------------------------------------------------------------
// HCETriggerActivity has to patch the eight HaloScript CALL SITES of trigger_volume_test_point and leave the
// other fifteen alone, because AI, object placement, damage and safe-zone code all call that same function every
// tick regardless of what the mission script is doing - a detour on its prologue would report every volume as
// live and destroy the distinction entirely.
//
// ★ Halo 5 does not have that problem, because its script functions are REGISTERED BY NAME and each has its own
// implementation. Hooking `volume_test_players` catches script polls and nothing else, by construction - the
// engine's own AI and damage systems do not route through the script binding. So we hook entry points directly
// and there is no call-site filtering to get wrong.
//
// The bindings were recovered from the engine's own registrar, not guessed. Registration looks like:
//     lea r9,  [rip+X]          ; implementation
//     lea r8,  [rip+Y]          ; name string
//     xor edx, edx
//     mov rcx, <registry>
//     call <register>
// Scanning the image for that pair yields 1104 named script functions, of which twelve are the volume-test
// family. See scratchpad/h5_script_functions.json.
//
// ---------------------------------------------------------------------------------------------------------------
// ⚠ THE VOLUME INDEX IS NOT IN THE SAME REGISTER FOR EVERY ONE OF THEM
// ---------------------------------------------------------------------------------------------------------------
// Each was checked individually by disassembling its prologue and finding the first instruction that consumes
// the argument. ECX for the volume_test_* family, EDX for the AI one:
//
//     0x00A855C0 volume_test_players              mov ebx, ecx        ECX
//     0x00A85650 volume_test_players_all          mov edi, ecx        ECX
//     0x00A855B0 volume_test_object               ecx passes through to the inner test untouched
//     0x00892150 volume_test_objects              mov edi, ecx        ECX
//     0x008920B0 volume_test_objects_all          mov edi, ecx        ECX
//     0x00A84BA0 volume_test_player_lookat        mov ebx, ecx        ECX
//     0x00A84C10 volume_test_players_lookat       mov esi, ecx        ECX
//     0x00A84A70 volume_test_players_all_lookat   mov ebp, ecx        ECX
//     0x0113A840 ai_get_all_in_trigger_volume     movsxd rsi, edx     EDX  (rcx is the AI reference)
//
// ⚠ THREE ARE DELIBERATELY NOT HOOKED. volume_test_players_mpteam (0x00A84530),
// volume_test_players_all_mpteam (0x00A84460) and volume_test_object_bounding_sphere_center (0x00A84440) all
// consume EDX first, which does NOT establish that EDX is the volume - their other argument (mp team / object)
// could be either way round. Rather than guess a register and silently mark the wrong volumes live, they are
// left out. The first two are multiplayer-team functions that campaign scripts do not call; the third is rare.
// Verify the argument order before adding any of them.
//
// ⚠ ABSENCE OF ACTIVITY IS NOT PROOF OF INERTNESS. A volume can be dormant simply because its goal has not
// started yet, and engine-driven volumes (kill volumes, zone-set switches) are never tested through a script
// binding at all, so they never light up here. The overlay must keep treating those by category, exactly as it
// already does - see the warning in H5TriggerOverlay about painting thirty live kill volumes as inert.
//
// Nothing is patched until setEnabled(true), and the constructor touches no game memory.
// ================================================================================================================
class H5TriggerActivity : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5TriggerActivity(GameState game, IDIContainer& dicon);
	~H5TriggerActivity();
	std::string_view getName() override { return nameof(H5TriggerActivity); }

	// Install / remove the script-binding hooks.
	void setEnabled(bool enable);

	// True if a script tested this scenario trigger-volume index within the last `windowMs`.
	bool isActive(int volumeIndex, uint32_t windowMs) const noexcept;

	// Milliseconds since this volume was last tested, or -1 if it never has been while we were watching.
	int64_t millisecondsSinceTested(int volumeIndex) const noexcept;

	// How many volumes have been seen tested at all. Useful to tell "nothing is live" from "the hooks failed".
	int everTestedCount() const noexcept;
};
