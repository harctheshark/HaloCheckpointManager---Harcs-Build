#pragma once
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// Disable Fade From Black - Halo Campaign Evolved.
//
// Reverting a checkpoint fades the screen in from black before you get your view back. This removes that fade.
//
// MECHANISM (all rvas into HaloSimulation_tag_release.dll, imagebase 0x180000000):
//   The revert sub_18019D730 is called from the host sub_1801AD9F0 at 0x1ADAC1 with a1 = 4. On success, at
//   0x19DBEF, it calls sub_1803277D0, which at 0x327827 loops 33 post-restore callbacks from the table at
//   .rdata 0x8617A0 with the flags in ECX. Entry [10] is sub_180211660, gated `test cl, 0B0h` at 0x21166D -
//   taken because a1 & 4 set bit 0x80. With no cinematic active it reaches:
//
//       2117C8  41 B9 3C 00 00 00   mov  r9d, 3Ch          <- 60 script ticks. WE PATCH THE IMM AT 0x2117CA.
//       2117DA  E8 F1 19 00 00      call sub_1802131D0     ; fade_in(black, 60)
//
//   ★ Entry [10] is the ONLY one of the 33 callbacks that touches the fade, so this is as narrow as it gets.
//
//   fade_in (sub_1802131D0) stores the start tick at fadeStruct+0x28 and end = start + duration at +0x2C, and
//   sets the RESTING value at +0x30 to 0.0. The evaluator (sub_1802132B0) early-returns the resting value
//   whenever end <= now:
//
//       out.xyzw = S[0x30..0x3F];
//       if (gtg && *(u8*)gtg && S->end > gtg->tick) { ...interpolate... }
//
//   So a duration of 0 makes end == start, the interpolation (INCLUDING THE DIVISION) is skipped entirely, and
//   out.x - the black overlay's opacity - is 0.0 on the very first frame. No divide-by-zero is possible, and
//   duration 0 is engine-legal: the sibling branch at 0x2116D9 already passes 0 to fade_out.
//
// WHY A BYTE PATCH RATHER THAN A WRITE, for once: the alternative is poking the fade struct (*(uintptr*)
// (TLS+0xA8), end at +0x2C) every frame, which would also collapse CINEMATIC fade-ins. Patching this one
// immediate touches only the post-restore path and leaves script fade_in/fade_out and every cinematic_fade_*
// completely alone.
//
// ⚠ 1 byte, so the write is inherently atomic - unlike the 2-byte Pause patch there is no torn-write concern.
// IDA reports NO base relocations anywhere on page 0x211000, and the only code xref into 0x2117C0..0x2117E0
// lands on 0x2117C8 (the instruction start), so nothing can enter mid-instruction. The two .pdata entries that
// reference 0x2117C8 as an unwind-scope boundary stay valid because no instruction boundary moves.
//
// ⚠ HCE has NO VERSION RESOURCE - every build reports 0.0.0.0 - so the expected-original-byte check is the only
// thing standing between a game update and us writing 0x00 over whatever now lives there. DO NOT REMOVE IT.
//
// SCOPE: sub_1803277D0's other callers (sub_18019DF80, sub_18019EF80, sub_180389980) are almost certainly
// save-load / level-restart restores, so those fades go instant too. That is very likely what a user wants.
// UNVERIFIED: if part of the black time is a UE5 loading widget rather than this fade, this shortens the delay
// without eliminating it.
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
