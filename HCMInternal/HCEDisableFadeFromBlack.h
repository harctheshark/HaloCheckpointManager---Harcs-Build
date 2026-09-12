#pragma once
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// Disable Fade From Black - Halo Campaign Evolved.
//
// Reverting a checkpoint fades the screen in from black before you get your view back. This removes that fade.
//
// ⚠ HISTORY, because the obvious implementation is the one that was here and it was too narrow.
// This used to patch a single CALL SITE: the `mov r9d, 3Ch` at 0x2117D8 feeding fade_in from post-restore
// callback [10]. That does work - verified in a live process - but it is one of FOUR calls to fade_in, and the
// only one whose duration is a literal:
//
//     0x2117EA   duration = mov r9d, 3Ch     <- the old target, an immediate
//     0x1E2CB2   duration = [rax+0x0C]       <- a looked-up tag record
//     0x1E3086   duration = [rbx+0x68]       <- a struct field
//     0x1E33E2   duration = [rbx+0x17C]      <- a struct field
//
// The other three read their duration from DATA at runtime, so no byte patch can reach them. Every CER cinematic
// is a save/play/game_revert sandwich, so reverting into one replays the level script's own fade through one of
// those three - which is precisely why the feature "sometimes didn't work".
//
// MECHANISM NOW: patch fade_in ITSELF, once, and cover all four callers.
//
//     0x213252  41 C6 40 25 01   mov  byte [r8+0x25], 1   ; direction = fade FROM black
//     0x213257  42 8D 04 12      lea  eax, [rdx + r10]    ; end = start + duration   <- REPLACED
//     0x21325B  45 89 50 28      mov  [r8+0x28], r10d     ; start tick
//     0x213262  41 89 40 2C      mov  [r8+0x2C], eax      ; end tick
//
// `mov eax, r10d` (44 89 D0) plus one 0x90 to hold the length at 4 makes end == start. The evaluator
// sub_1802132B0 reads the resting value first and only interpolates `if (gtg && *(u8*)gtg && end > tick)`, so
// with end == start that test is false on the very first frame and it returns +0x30, which fade_in sets to 0.0 =
// fully transparent. The interpolation - INCLUDING ITS DIVISION - never executes, so duration 0 cannot divide by
// zero. rdx is dead afterwards: nothing reads it before 0x213266 overwrites edx with -1.
//
// ⚠⚠ fade_out IS DELIBERATELY LEFT ALONE, and it is byte-identical from the lea onward, 0xE0 earlier at
// 0x213172. The ONLY discriminator is the direction byte: fade_in writes 01 at +0x25 and 0.0 as its resting
// value; fade_out writes 00 and 1.0f. That is why the guard site is the direction-byte instruction rather than
// the lea itself, and why the anchor signature starts there. Patching fade_out too would collapse every
// intentional fade TO black.
//
// SCOPE: all fade-from-black is now instant, cinematic fade-ins included. That is a deliberate choice and what
// the toggle's name promises. Fades TO black are unaffected.
//
// ⚠ WRITE ATOMICITY: this is 4 bytes at 0x213257, which is NOT 4-byte aligned - so unlike the old 1-byte patch
// it is not trivially atomic. It is however entirely inside the 64-byte cache line at 0x213240 (offset 0x17,
// ending 0x1B), and x86 does not tear an access that stays within a cache line. The instruction length is
// unchanged, so no boundary moves and the .pdata unwind scopes covering this range stay valid.
//
// ⚠ HCE has NO VERSION RESOURCE - every build reports 0.0.0.0 - so the expected-original-byte check plus the
// HCEAnchors signature cross-check are the only things standing between a game update and us writing over
// whatever now lives there. DO NOT REMOVE THEM.
// ================================================================================================================
class HCEDisableFadeFromBlack : public IOptionalCheat
{
private:
	class HCEDisableFadeFromBlackImpl;
	std::unique_ptr<HCEDisableFadeFromBlackImpl> pimpl;

public:
	HCEDisableFadeFromBlack(GameState game, IDIContainer& dicon);
	~HCEDisableFadeFromBlack();
	virtual std::string_view getName() override { return nameof(HCEDisableFadeFromBlack); }
};
