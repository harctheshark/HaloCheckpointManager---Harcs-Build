#include "pch.h"
#include "H2Supersampling.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "IMCCStateHook.h"
#include "H2Overscan.h"

// ================================================================================================================================
// HOW (halo2.dll 1.3528; read 2026-11-21 in the MCC halo2.dll.i64).
//
// Halo 2 has TWO screen sizes:
//  - the RENDER rectangle (halo2+0x1994864 minus +0x199485C), set by sub_18095E9B0(windowW, windowH, i16 offX, i16 offY,
//    i16, u16, float uiScale, float renderScale): render size = window x renderScale. Its only caller sub_180954DD0 always
//    passes the shared constant 1.0 (flt_180C32824). It sizes the MCC scene targets (sub_180952780 / sub_180953390;
//    sample count hard-coded 1 - no MSAA).
//  - the WINDOW size (halo2+0x1994858 width, +0x199485C height), returned by three getters:
//      sub_18095E820(i16 rect[4]) = {0, 0, height, width}  - the player view rectangles (sub_1806F5C60 ...)
//      sub_18095E880() = height, sub_18095E890() = width  - the rasterizer target table's screen-sized targets
//        (sub_180961D20 / sub_180961B70: targets 0, 3, 24, 39) and the viewport / screen mapping set-up (sub_180955590)
//    and read DIRECTLY by the mouse (sub_1806D0B10), the UI and the last post pass (sub_180973620).
//
// v1 (build h2_ssaa_50) scaled only renderScale: the scene targets went 2x, the view rectangles and rasterizer targets
// stayed 1x - the world landed in the top-left quarter of a grey frame (user screenshot 2026-11-21, "my eyes! xD").
// v2: everything the 3D frame is built from goes 2x together - renderScale AND the three getters - while the HUD pass
// (sub_1807FFD70(a1, a2, i16* viewRect), drawn on the back buffer after the scene) gets its rectangle divided back to the
// window and unscaled getters while it runs. The direct readers (mouse, UI, last post pass) never see the scale.
// (The original-Xbox Spooky Ghost menu's Render Scale - github Fortune090/Xbox-Spooky-Ghost-Menu - lists the same set of
// things that must follow: target sizes, viewports, screen mapping, the view rectangle; its addresses are the Xbox's.)
//
// The engine sets these up only at a window / resolution change and keeps its targets until then, so a new factor takes
// effect at the next resolution change; the getters follow the factor the render rectangle was LAST built with
// (sApplied), so targets and views always agree.
// ================================================================================================================================
namespace
{
	constexpr uintptr_t kSetRenderRectRva = 0x95E9B0;   // sub_18095E9B0
	constexpr uintptr_t kWindowRectRva = 0x95E820;      // sub_18095E820
	constexpr uintptr_t kWindowHeightRva = 0x95E880;    // sub_18095E880
	constexpr uintptr_t kWindowWidthRva = 0x95E890;     // sub_18095E890
	constexpr uintptr_t kHudRva = 0x7FFD70;             // sub_1807FFD70
	// v3 (2026-11-22, user: "the whole camera is zoomed in", the crosshair in the bottom-right corner): the FINAL pass
	// sub_180975230(target) draws the finished classic frame (target 39) onto the output target with the global screen
	// rectangle halo2+0x165C290 (i16 top, left, bottom, right) as its viewport - built from the (2x) window getters, so the
	// 2x frame was drawn 1:1 into a viewport twice the output: only its top-left quarter showed. While it runs the rectangle
	// is the real window again (and the getters unscaled), so the 2x frame is drawn down onto the window: the supersampling.
	constexpr uintptr_t kFinalPassRva = 0x975230;       // sub_180975230
	constexpr uintptr_t kScreenRectRva = 0x165C290;     // xmmword_18165C290
	constexpr uintptr_t kPixelBoundsRva = 0x1996A58;    // xmmword_181996A58
	constexpr uintptr_t kFinalCopyFlagRva = 0x1996A17, kFinalCopyFlag2Rva = 0x186D300;   // the plain-copy path's flags
	// v4 (2026-11-22, user: "aliasing is 100% the same"): the scene IS 2x, but the final pass forces a POINT sampler on stage 0
	// (sub_1809614F0(0, 5 mag, 1) / (0, 6 min, 1) / (0, 7 mip, 1) at 0x97540A..0x975483) before its gamma shader samples the
	// frame - at 2:1 a point sample keeps one texel of every 2x2, no averaging. In the final pass the pixel-shader set-up
	// sub_1809735F0(3) runs after those sets and before the draw: there the engine's own setter makes mag / min LINEAR
	// (filter 0x14 MIN_MAG_LINEAR_MIP_POINT - not 0x15, which the engine turns into anisotropic x16), so each output pixel
	// (sampled at the corner between 4 texels) is the exact 2x2 average; at 1.5x a 0.75 / 0.25 blend. The engine's cache and
	// dirty flag stay right: the next pass that wants point sets it again and gets it.
	constexpr uintptr_t kSetPixelShaderRva = 0x9735F0;  // sub_1809735F0(int index)
	constexpr uintptr_t kSetSamplerStateRva = 0x9614F0; // sub_1809614F0(stage, type, value): D3D9-style sampler state emulation
	// v5 (2026-11-22, the HUD still 2x: the crosshair in the bottom-right corner, the grenade boxes twice their size): the HUD
	// is sub_180822190 (the only caller sub_180831CB0, from the per-view sub_1807E30D0) drawn onto the WINDOW-sized output after
	// the final pass - from the screen rectangles (viewport + ortho from 0x1996A58, anchors from 0x165C290 words 4..7) and the
	// HUD scale globals sub_18095EA50 computes at a resolution change from the render height (+0x1994860 / +0x1994864, i16):
	// text scale +0xE14F2C = h / 960 (>= 1 unless q[+0x1E8CE68]), widget scale +0xE14F28 = that x 0.9 / 0.8 / 1.0, margin
	// +0x187D860 = h x 0.05. The fade / flash overlay sub_180973620 after it also takes its ortho from 0x1996A58.
	// (sub_1807FFD70, hooked before as "the HUD", is the UI / menu pass that comes later.)
	// (the engine notify sub_180053FF0(this, 14 = video reset, data) must NEVER be called by HCM - see requestRebuild)
	constexpr uintptr_t kHudPassRva = 0x822190;         // sub_180822190 (void)
	constexpr uintptr_t kFadePassRva = 0x973620;        // sub_180973620 (void)
	constexpr uintptr_t kHudScaleSetupRva = 0x95EA50;   // sub_18095EA50(int, int) -> bool
	constexpr uintptr_t kRenderTopRva = 0x1994860, kRenderBottomRva = 0x1994864;
	constexpr uintptr_t kHudScaleRva = 0xE14F2C, kHudWidgetScaleRva = 0xE14F28, kHudMarginRva = 0x187D860, kHudNoClampRva = 0x1E8CE68;
	// v6 (2026-11-27, user: "figure out the video reset so we dont need to menu to do it"): the engine's OWN in-place rebuild
	// sub_180037ED0 = if (byte_181E8CE73 && qword_181A23008) { release sub_180953030; size = [qword_181A23008]->vf+0x60;
	// render rect sub_180954DD0(w, h); recreate sub_180953050 } - the video reset WITHOUT its MCC host call (the one that
	// deadlocked, see the note at requestRebuild). The engine calls it itself from sub_18003B4F0 inside the frame-boundary
	// function sub_18003CAB0, on Halo 2's thread. HCM runs it right after the event drain sub_18003A3C0 (also called from
	// sub_18003CAB0 - where the engine runs its own video reset) when a rebuild is pending.
	constexpr uintptr_t kEventDrainRva = 0x3A3C0;       // sub_18003A3C0 (void)
	constexpr uintptr_t kRebuildRva = 0x37ED0;          // sub_180037ED0 (the args are passed through, unused)

	struct Probe { uintptr_t rva; uint8_t bytes[20]; size_t n; };
	const Probe kProbes[] = {
		{ kSetRenderRectRva, { 0x33, 0xC0, 0x66, 0x0F, 0x6E, 0xC1, 0x0F, 0x5B, 0xC0, 0x89, 0x05 }, 11 },
		{ kWindowRectRva, { 0x33, 0xC0, 0x89, 0x01, 0x0F, 0xB7, 0x05 }, 7 },
		{ kWindowHeightRva, { 0x8B, 0x05 }, 2 },
		{ kWindowHeightRva + 6, { 0xC3, 0xCC }, 2 },
		{ kWindowWidthRva, { 0x8B, 0x05 }, 2 },
		{ kWindowWidthRva + 6, { 0xC3, 0xCC }, 2 },
		{ kHudRva, { 0x40, 0x53, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x4D, 0x8B, 0xF0, 0x8B, 0xF2, 0x8B, 0xD9, 0xE8 }, 17 },
		{ kFinalPassRva, { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x80, 0x3D }, 15 },
		{ kSetPixelShaderRva, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xD9, 0xE8 }, 10 },
		{ kSetSamplerStateRva, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0xFF, 0x05 }, 7 },
		{ kHudPassRva, { 0x48, 0x83, 0xEC, 0x28, 0x80, 0x3D }, 6 },
		{ kFadePassRva, { 0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0xA8, 0x48, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01, 0x00, 0x00 }, 18 },
		{ kHudScaleSetupRva, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0xBF, 0x05 }, 18 },
		{ kEventDrainRva, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD8, 0xFC, 0xFF, 0xFF }, 17 },
		// sub rsp,78h; cmp byte_181E8CE73,0; jz; cmp qword_181A23008,0; jz; call sub_180953030 (relative operands skipped)
		{ kRebuildRva, { 0x48, 0x83, 0xEC, 0x78, 0x80, 0x3D }, 6 },
		{ kRebuildRva + 0x0B, { 0x74, 0x37, 0x48, 0x83, 0x3D }, 5 },
		{ kRebuildRva + 0x15, { 0x74, 0x2D, 0xE8 }, 3 },
	};

	using RebuildFn = int64_t(__fastcall*)(int64_t, int64_t);
	bool sehRebuild(RebuildFn fn)
	{
		__try { fn(0, 0); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool bytesAt(uintptr_t a, const uint8_t* expect, size_t n)
	{
		__try { return memcmp((const void*)a, expect, n) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	float factorOf(SettingsEnums::H2Supersampling v)
	{
		switch (v)
		{
		case SettingsEnums::H2Supersampling::x1_5: return 1.5f;
		case SettingsEnums::H2Supersampling::x2: return 2.f;
		default: return 1.f;
		}
	}

	bool safeCopy(void* dst, const void* src, size_t n)
	{
		__try { memcpy(dst, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	thread_local int tUnscaled = 0;   // > 0: inside the HUD pass - the getters answer the real window
	thread_local int tFinalPass = 0;  // > 0: inside the final pass (sub_180975230)

	// TAA overscan (H2Overscan.h): the final pass's full-screen quad builder - the only caller whose getter calls must answer
	// the TARGET size while the final pass runs (its texture coordinates = pixel bounds / getters, so the crop in target pixels
	// becomes exact texture coordinates); the rest of the final pass keeps the window.
	constexpr uintptr_t kFinalQuadUvRva = 0x976010, kFinalQuadUvEnd = 0x976115;   // sub_180976010 (size 0x105)

	// ret = _ReturnAddress() taken IN the getter detour (= the getter's caller)
	bool isFinalQuadCaller(uintptr_t ret)
	{
		const uintptr_t mod = (uintptr_t)GetModuleHandleW(L"halo2.dll");
		return mod && ret >= mod + kFinalQuadUvRva && ret < mod + kFinalQuadUvEnd;
	}

	// percent per side -> the factor each axis grows by (5 % per side = 1.1); capped at 25 % per side
	float overscanFactor(float percent)
	{
		if (!std::isfinite(percent) || percent <= 0.f) return 1.f;
		return 1.f + 2.f * (std::min)(percent, 25.f) / 100.f;
	}
}

class H2Supersampling::H2SupersamplingImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::mutex mMutex;
	std::unique_ptr<ModuleInlineHook> mRectHook, mWinRectHook, mWinHHook, mWinWHook, mHudHook, mFinalHook, mPixelShaderHook, mHudPassHook, mFadePassHook, mHudScaleHook, mDrainHook;
	static inline std::atomic<bool> sRebuildPending{ false };   // run the engine's in-place rebuild at the next frame boundary
	static inline std::atomic<uint64_t> sRebuildPostedMs{ 0 };
	bool mVerified = false, mVerifyFailed = false;
	ScopedCallback<eventpp::CallbackList<void(SettingsEnums::H2Supersampling&)>> mChangeCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	float mWanted = 1.f;

	static inline std::atomic<float> sFactor{ 1.f };    // the Supersampling setting
	// the factors the render rectangle was last built with, per axis (supersampling x overscan); sApplied = the larger one,
	// > 1 = "the 3D frame is scaled" (every hook acts)
	static inline std::atomic<float> sApplied{ 1.f }, sAppliedX{ 1.f }, sAppliedY{ 1.f };
	// TAA overscan in percent per side (H2Overscan.h), set by H2TAA: X = left / right, Y = top / bottom
	static inline std::atomic<float> sOverscanX{ 0.f }, sOverscanY{ 0.f };
	static inline std::atomic<H2SupersamplingImpl*> sSelf{ nullptr };
	static inline std::atomic<int> sInFlight{ 0 };
	// render rect globals sub_18095E9B0 writes (i16): +0x1994864 height, +0x1994866 width; +0x1994868 an inner rect
	// {offY, offX, height - offY, width - offX}. Both sizes come from ONE scale, so for separate X / Y factors the height
	// words are rewritten after it (the targets are created later, from these words).
	static constexpr uintptr_t kRenderHeightRva = 0x1994864, kRenderInnerBottomRva = 0x199486C;

	static bool scaledNow() { return sApplied.load(std::memory_order_relaxed) > 1.f && tUnscaled == 0; }

	// a scaled screen coordinate back to the window (x = a left / right word, else top / bottom): the exact window size for
	// the exact target size, otherwise / the axis's factor
	static int16_t toWindow(int16_t v, bool x)
	{
		if (h2os::gActive.load(std::memory_order_acquire))
		{
			const h2os::Layout& L = h2os::gLayout;
			if (x && v == (int16_t)L.targetW) return (int16_t)L.windowW;
			if (!x && v == (int16_t)L.targetH) return (int16_t)L.windowH;
		}
		const float f = x ? sAppliedX.load() : sAppliedY.load();
		return (int16_t)std::lround(v / (f > 0.f ? f : 1.f));
	}

	// a window-size value as the scaled 3D frame sees it: the EXACT target size the engine made for the window's width /
	// height (the layout - computed with the engine's own rounding), otherwise value x factor
	static uint32_t scaledSize(uint32_t v, bool width)
	{
		if (h2os::gActive.load(std::memory_order_acquire))
		{
			const h2os::Layout& L = h2os::gLayout;
			if (width && (int)v == L.windowW) return (uint32_t)L.targetW;
			if (!width && (int)v == L.windowH) return (uint32_t)L.targetH;
		}
		return (uint32_t)std::lround((double)v * (width ? sAppliedX.load() : sAppliedY.load()));
	}

	static int64_t __fastcall rectDetour(int64_t w, int64_t h, int64_t offX, int64_t offY, int64_t a5, int64_t a6, float uiScale, float renderScale)
	{
		++sInFlight;
		int64_t r = 0;
		float f = sFactor.load();
		float ox = overscanFactor(sOverscanX.load()), oy = overscanFactor(sOverscanY.load());
		const bool scaleOk = std::isfinite(renderScale) && renderScale > 0.f;
		// D3D11's texture limit is 16384 a side: a bigger render rect fails to create its targets and the next frame crashes
		// (the engine ignores the failure) - the overscan goes first, then the supersampling factor is cut to fit
		if ((f > 1.f || ox > 1.f || oy > 1.f) && scaleOk)
		{
			const float sw = (float)(int)w * renderScale, sh = (float)(int)h * renderScale;
			if (sw * f * ox > 16384.f || sh * f * oy > 16384.f) { ox = oy = 1.f; PLOG_WARNING << "Halo 2 TAA overscan: the targets would pass 16384 pixels - no overscan"; }
			const float side = (std::max)(sw, sh);
			if (side > 0.f && side * f > 16384.f) f = (std::max)(1.f, 16384.f / side);
		}
		const float fx = f * ox, fy = f * oy;
		const float scaledX = (scaleOk && fx > 1.f) ? renderScale * fx : renderScale;
		const float scaledY = (scaleOk && fy > 1.f) ? renderScale * fy : renderScale;

		// the layout (H2Overscan.h) whenever the frame is scaled - supersampling, overscan or both - with the engine's own
		// rounding (render size = (int)(float)(window x scale)). Without overscan the shown part is the whole target.
		h2os::gActive.store(false, std::memory_order_release);
		h2os::gOverscan.store(false, std::memory_order_release);
		if (scaleOk && (fx > 1.f || fy > 1.f))
		{
			h2os::Layout L;
			L.windowW = (int)w; L.windowH = (int)h;
			L.targetW = (int)((float)(int)w * scaledX);
			L.targetH = (int)((float)(int)h * scaledY);
			L.innerW = (ox > 1.f) ? (int)std::lround((double)(int)w * renderScale * f) : L.targetW;
			L.innerH = (oy > 1.f) ? (int)std::lround((double)(int)h * renderScale * f) : L.targetH;
			L.offX = (L.targetW - L.innerW) / 2;
			L.offY = (L.targetH - L.innerH) / 2;
			if (L.innerW > 0 && L.innerH > 0 && L.targetW >= L.innerW && L.targetH >= L.innerH)
			{
				h2os::gLayout = L;
				h2os::gOverscan.store(L.targetW > L.innerW || L.targetH > L.innerH, std::memory_order_release);
				h2os::gActive.store(true, std::memory_order_release);
				if (h2os::gOverscan.load())
					PLOG_INFO << "Halo 2 TAA overscan: " << sOverscanX.load() << "% left/right, " << sOverscanY.load() << "% top/bottom - render "
						<< L.targetW << "x" << L.targetH << ", shown " << L.innerW << "x" << L.innerH << " at (" << L.offX << ", " << L.offY << ") onto the "
						<< L.windowW << "x" << L.windowH << " window";
			}
		}
		sAppliedX = fx > 1.f ? fx : 1.f;
		sAppliedY = fy > 1.f ? fy : 1.f;
		sApplied = (std::max)(sAppliedX.load(), sAppliedY.load());
		PLOG_INFO << "Halo 2 supersampling: window " << (int)w << "x" << (int)h << ", render scale " << renderScale << " -> " << scaledX << " x / " << scaledY
			<< " y (render " << (int)((float)(int)w * scaledX) << "x" << (int)((float)(int)h * scaledY) << ")";
		if (H2SupersamplingImpl* self = sSelf.load())
		{
			r = self->mRectHook->getInlineHook().call<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, float, float>(w, h, offX, offY, a5, a6, uiScale, scaledX);
			// the engine made both sizes with the X scale: the height again with the Y scale (its own formula and rounding)
			if (scaledY != scaledX)
				if (const HMODULE mod = GetModuleHandleW(L"halo2.dll"))
				{
					const int16_t hh = (int16_t)(int)((float)(int)h * scaledY);
					const int16_t inner = (int16_t)(hh - (int16_t)offY);
					safeCopy((uint8_t*)mod + kRenderHeightRva, &hh, 2);
					safeCopy((uint8_t*)mod + kRenderInnerBottomRva, &inner, 2);
				}
		}
		--sInFlight;
		return r;
	}

	static void __fastcall winRectDetour(int16_t* rect)
	{
		++sInFlight;
		if (H2SupersamplingImpl* self = sSelf.load())
		{
			self->mWinRectHook->getInlineHook().call<void, int16_t*>(rect);
			if (scaledNow() && rect)
			{
				rect[2] = (int16_t)scaledSize((uint32_t)(uint16_t)rect[2], false);   // bottom (height)
				rect[3] = (int16_t)scaledSize((uint32_t)(uint16_t)rect[3], true);    // right (width)
			}
		}
		--sInFlight;
	}

	// the getters: the scaled size for the 3D frame; the window inside the HUD / final pass - except for the final pass's
	// quad builder, which divides the shown part (target pixels) by the target size: exact texture coordinates
	static uint32_t __fastcall winHDetour()
	{
		++sInFlight;
		const uintptr_t ret = (uintptr_t)_ReturnAddress();
		uint32_t v = 0;
		if (H2SupersamplingImpl* self = sSelf.load()) v = self->mWinHHook->getInlineHook().call<uint32_t>();
		if (scaledNow()) v = scaledSize(v, false);
		else if (tFinalPass > 0 && h2os::gActive.load(std::memory_order_acquire) && isFinalQuadCaller(ret)) v = (uint32_t)h2os::gLayout.targetH;
		--sInFlight;
		return v;
	}

	static uint32_t __fastcall winWDetour()
	{
		++sInFlight;
		const uintptr_t ret = (uintptr_t)_ReturnAddress();
		uint32_t v = 0;
		if (H2SupersamplingImpl* self = sSelf.load()) v = self->mWinWHook->getInlineHook().call<uint32_t>();
		if (scaledNow()) v = scaledSize(v, true);
		else if (tFinalPass > 0 && h2os::gActive.load(std::memory_order_acquire) && isFinalQuadCaller(ret)) v = (uint32_t)h2os::gLayout.targetW;
		--sInFlight;
		return v;
	}

	// the two global screen rectangles (i16 top, left, bottom, right) the screen passes work in - halo2+0x165C290 (the
	// viewport) and halo2+0x181996A58 (pixel bounds: a full-screen quad's corners, its texture coordinates = corners / the
	// window getters) - set to the real window while a pass that draws onto the window runs, then put back (unless the pass
	// rewrote them meanwhile). Both were built from the 2x getters: the final pass drew the frame into a quarter of the
	// screen (h2_ssaa_62: its texture coordinates ran 0..2) and the HUD laid out on the 2x screen (the crosshair in the
	// bottom-right corner).
	// v5 (2026-11-22): halo2+0x165C290 is EIGHT i16 - words 0..3 the viewport bounds, words 4..7 the WINDOW bounds the HUD
	// anchors on (the crosshair = their centre, sub_180829490; text / messages read them too); all 8 are divided. 0x1996A58:
	// 4 (the rest is never read).
	struct WindowRects
	{
		static constexpr int kWords[2] = { 8, 4 };
		int16_t* rect[2]{};
		int16_t saved[2][8]{}, down[2][8]{};
		bool set[2]{};
		void begin(HMODULE mod)
		{
			const uintptr_t rva[2] = { kScreenRectRva, kPixelBoundsRva };
			for (int k = 0; k < 2; ++k)
			{
				const size_t n = sizeof(int16_t) * kWords[k];
				rect[k] = (int16_t*)((uint8_t*)mod + rva[k]);
				if (!safeCopy(saved[k], rect[k], n)) continue;
				for (int i = 0; i < kWords[k]; ++i) down[k][i] = toWindow(saved[k][i], (i & 1) != 0);   // words: top, left, bottom, right
				set[k] = safeCopy(rect[k], down[k], n);
			}
		}
		void end()
		{
			for (int k = 0; k < 2; ++k)
			{
				const size_t n = sizeof(int16_t) * kWords[k];
				int16_t now[8];
				if (set[k] && safeCopy(now, rect[k], n) && memcmp(now, down[k], n) == 0) safeCopy(rect[k], saved[k], n);
			}
		}
	};

	// a void() pass drawn onto the window after the final pass (the HUD sub_180822190, the fade / flash overlay
	// sub_180973620): the screen rectangles at the window and the getters unscaled while it runs
	template <std::unique_ptr<ModuleInlineHook> H2SupersamplingImpl::* Hook>
	static void __fastcall windowPassDetour()
	{
		++sInFlight;
		H2SupersamplingImpl* self = sSelf.load();
		const float f = sApplied.load();
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (self)
		{
			if (f > 1.f && mod)
			{
				WindowRects w;
				w.begin(mod);
				++tUnscaled;
				(self->*Hook)->getInlineHook().call<void>();
				--tUnscaled;
				w.end();
			}
			else (self->*Hook)->getInlineHook().call<void>();
		}
		--sInFlight;
	}

	// v5: the HUD / text scale globals, computed at a resolution change by sub_18095EA50 from the render height (2x: the HUD
	// elements twice their size): recomputed from the window height, with the engine's own clamp and ratio
	static bool __fastcall hudScaleDetour(int32_t a1, int32_t a2)
	{
		++sInFlight;
		bool r = false;
		H2SupersamplingImpl* self = sSelf.load();
		if (self) r = self->mHudScaleHook->getInlineHook().call<bool, int32_t, int32_t>(a1, a2);
		const float f = sAppliedY.load();   // the HUD scale follows the render HEIGHT
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (self && mod && f > 1.f)
		{
			uint8_t* m = (uint8_t*)mod;
			int16_t top = 0, bot = 0; uint64_t noClamp = 0; float k2C = 0.f, k28 = 0.f, margin = 0.f;
			if (safeCopy(&top, m + kRenderTopRva, 2) && safeCopy(&bot, m + kRenderBottomRva, 2) && safeCopy(&noClamp, m + kHudNoClampRva, 8)
				&& safeCopy(&k2C, m + kHudScaleRva, 4) && safeCopy(&k28, m + kHudWidgetScaleRva, 4) && safeCopy(&margin, m + kHudMarginRva, 4))
			{
				float s = ((float)(bot - top) / f) / 960.f;
				if (noClamp == 0 && s <= 1.f) s = 1.f;   // the engine's clamp
				const float ratio = k2C != 0.f ? k28 / k2C : 0.8f;   // 0.9 / 0.8 / 1.0 by a setting
				const float n2C = s, n28 = s * ratio, nMargin = margin / f;
				safeCopy(m + kHudScaleRva, &n2C, 4);
				safeCopy(m + kHudWidgetScaleRva, &n28, 4);
				safeCopy(m + kHudMarginRva, &nMargin, 4);
				PLOG_INFO << "Halo 2 supersampling: HUD scale " << k2C << " / " << k28 << " -> " << n2C << " / " << n28 << " (window height " << (bot - top) / f << ")";
			}
		}
		--sInFlight;
		return r;
	}

	// the HUD: drawn on the back buffer (window size) after the scene - its view rectangle and the screen rectangles back to
	// the window, and the getters unscaled while it runs
	static void __fastcall hudDetour(uint32_t a1, uint32_t a2, int16_t* viewRect)
	{
		++sInFlight;
		H2SupersamplingImpl* self = sSelf.load();
		const float f = sApplied.load();
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (self)
		{
			if (f > 1.f && viewRect && mod)
			{
				alignas(16) int16_t copy[32];
				if (safeCopy(copy, viewRect, sizeof(copy)))
				{
					for (int i = 0; i < 8; ++i) copy[i] = toWindow(copy[i], (i & 1) != 0);   // the camera rect: 8 words, like 0x165C290
					WindowRects w;
					w.begin(mod);
					++tUnscaled;
					self->mHudHook->getInlineHook().call<void, uint32_t, uint32_t, int16_t*>(a1, a2, copy);
					--tUnscaled;
					w.end();
				}
				else self->mHudHook->getInlineHook().call<void, uint32_t, uint32_t, int16_t*>(a1, a2, viewRect);
			}
			else self->mHudHook->getInlineHook().call<void, uint32_t, uint32_t, int16_t*>(a1, a2, viewRect);
		}
		--sInFlight;
	}

	// the final pass (see kFinalPassRva): the screen rectangle and the getters at the real window while it draws
	static int64_t __fastcall finalDetour(uint16_t target)
	{
		++sInFlight;
		int64_t r = 0;
		H2SupersamplingImpl* self = sSelf.load();
		const float f = sApplied.load();
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (self && mod && f > 1.f)
		{
			static bool s_said = false;
			if (!s_said)
			{
				s_said = true;
				uint8_t a = 0, b = 0;
				safeCopy(&a, (const uint8_t*)mod + kFinalCopyFlagRva, 1);
				safeCopy(&b, (const uint8_t*)mod + kFinalCopyFlag2Rva, 1);
				PLOG_INFO << "Halo 2 supersampling: final pass of target " << target << " - "
					<< ((a && !b) ? "the plain-copy path (not scaled down: the frame stays cropped)" : "the drawn path (scaled down onto the window)");
			}
			WindowRects w;
			w.begin(mod);
			// TAA overscan: the quad samples only the centre of the frame - the pixel bounds (i16 top, left, bottom, right)
			// become the crop in TARGET pixels; the quad builder divides them by the target size (isFinalQuadCaller), so its
			// texture coordinates hit texel centres exactly; the viewport (0x165C290, divided above) stays the window
			if (h2os::gActive.load(std::memory_order_acquire) && w.rect[1] && w.set[1])   // set[1]: the original was saved
			{
				const h2os::Layout& L = h2os::gLayout;
				const int16_t crop[4] = { (int16_t)L.offY, (int16_t)L.offX, (int16_t)(L.offY + L.innerH), (int16_t)(L.offX + L.innerW) };
				if (safeCopy(w.rect[1], crop, sizeof(crop)))
				{
					memcpy(w.down[1], crop, sizeof(crop));
					w.set[1] = true;
					static bool s_saidCrop = false;
					if (!s_saidCrop) { s_saidCrop = true; PLOG_INFO << "Halo 2 TAA overscan: the final pass shows target pixels (" << L.offX << ", " << L.offY << ") .. (" << L.offX + L.innerW << ", " << L.offY + L.innerH << ")"; }
				}
			}
			++tUnscaled; ++tFinalPass;
			r = self->mFinalHook->getInlineHook().call<int64_t, uint16_t>(target);
			--tUnscaled; --tFinalPass;
			w.end();
		}
		else if (self) r = self->mFinalHook->getInlineHook().call<int64_t, uint16_t>(target);
		--sInFlight;
		return r;
	}

	// v6: the frame-boundary event drain (see kEventDrainRva) - after it, the engine's own in-place rebuild when one is pending
	static uint64_t __fastcall drainDetour()
	{
		++sInFlight;
		uint64_t result = 0;
		H2SupersamplingImpl* self = sSelf.load();
		if (self)
		{
			result = self->mDrainHook->getInlineHook().call<uint64_t>();
			if (sRebuildPending.exchange(false))
			{
				const HMODULE mod = GetModuleHandleW(L"halo2.dll");
				if (mod)
				{
					const uint64_t waited = GetTickCount64() - sRebuildPostedMs.load();
					const bool ok = sehRebuild(reinterpret_cast<RebuildFn>((uint8_t*)mod + kRebuildRva));
					if (ok) PLOG_INFO << "Halo 2 supersampling: render targets rebuilt in place by the engine (sub_180037ED0), " << waited << " ms after the request";
					else PLOG_ERROR << "Halo 2 supersampling: the engine's in-place rebuild faulted - change MCC's resolution and back to recover";
				}
			}
		}
		--sInFlight;
		return result;
	}

	// the final pass's pixel shader set-up (see kSetPixelShaderRva): stage 0 mag / min filter linear before its draw
	static int64_t __fastcall pixelShaderDetour(int32_t index)
	{
		++sInFlight;
		int64_t r = 0;
		H2SupersamplingImpl* self = sSelf.load();
		if (self)
		{
			if (tFinalPass > 0 && index == 3)
			{
				if (const HMODULE mod = GetModuleHandleW(L"halo2.dll"))
				{
					using SetSampler = int64_t(__fastcall*)(uint32_t stage, uint32_t type, uint32_t value);
					const auto setSampler = reinterpret_cast<SetSampler>((uint8_t*)mod + kSetSamplerStateRva);
					setSampler(0, 5, 2);   // magfilter linear
					setSampler(0, 6, 2);   // minfilter linear (mip stays point: filter 0x14)
					static bool s_said = false;
					if (!s_said) { s_said = true; PLOG_INFO << "Halo 2 supersampling: the final pass samples the frame with a linear filter (2x2 average)"; }
				}
			}
			r = self->mPixelShaderHook->getInlineHook().call<int64_t, int32_t>(index);
		}
		--sInFlight;
		return r;
	}

	void say(const std::string& text)
	{
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(text); }
		catch (HCMRuntimeException&) {}
	}

	// 1 hooked, 0 halo2.dll not loaded (attached once it is), -1 other build
	int ensureHooks()
	{
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (!mod) return 0;
		if (!mVerified)
		{
			if (mVerifyFailed) return -1;
			const bool anyInstalled = mRectHook->isHookInstalled() || mWinRectHook->isHookInstalled() || mWinHHook->isHookInstalled()
				|| mWinWHook->isHookInstalled() || mHudHook->isHookInstalled() || mFinalHook->isHookInstalled() || mPixelShaderHook->isHookInstalled() || mHudPassHook->isHookInstalled() || mFadePassHook->isHookInstalled() || mHudScaleHook->isHookInstalled() || mDrainHook->isHookInstalled();
			if (!anyInstalled)
				for (const Probe& p : kProbes)
					if (!bytesAt((uintptr_t)mod + p.rva, p.bytes, p.n))
					{
						mVerifyFailed = true;
						PLOG_ERROR << "Halo 2 supersampling: halo2.dll+" << std::hex << p.rva << " is not what this was made for (MCC 1.3528) - not applied";
						return -1;
					}
			mVerified = true;
		}
		for (auto* h : { &mHudHook, &mFinalHook, &mPixelShaderHook, &mHudPassHook, &mFadePassHook, &mHudScaleHook, &mWinRectHook, &mWinHHook, &mWinWHook, &mRectHook, &mDrainHook }) (*h)->setWantsToBeAttached(true);
		return mRectHook->isHookInstalled() ? 1 : 0;
	}

	void onChange(SettingsEnums::H2Supersampling& v)
	{
		std::scoped_lock lk(mMutex);
		const float f = factorOf(v);
		sFactor = f; mWanted = f;
		const int r = (f > 1.f || sOverscanX.load() > 0.f || sOverscanY.load() > 0.f) ? ensureHooks() : 1;
		if (r < 0) { say("Halo 2 Supersampling: this Halo 2 build is not supported (made for MCC 1.3528)"); return; }
		if (requestRebuild())
			say(f > 1.f ? std::format("Halo 2 Supersampling {}x applied", f) : std::string("Halo 2 Supersampling off"));
		else if (f > 1.f)
			say(std::format("Halo 2 Supersampling {}x set. It takes effect once Halo 2 is loaded (or at the next resolution change).", f));
		else
			say("Halo 2 Supersampling off - back to normal at the next resolution change (change MCC's resolution and back).");
	}

	// ⚠⚠ NEVER POST THE ENGINE'S VIDEO RESET EVENT. sub_180053FF0(this, 14, data) - what MCC's resolution change does - HUNG
	// MCC FOR GOOD every time HCM posted it (2026-11-24 once, 2026-11-27 twice). The engine runs it between frames on Halo 2's
	// thread: case 14 of sub_18003A3C0 releases the targets (sub_180953030) and calls the MCC host [qword_181E8CE68]+0x10, which
	// waits for MCC's main thread - and MCC's main thread is waiting in its UI frame rendezvous (mcc+BA08A5 -> D3CAB7) for Halo
	// 2's next FrameEnd: a deadlock (snapshot: MCC Hang Tools\snapshots\h2_overscan_184239.txt). When MCC itself changes the
	// resolution its main thread is not in that wait.
	// Instead (v6): the engine's own in-place rebuild sub_180037ED0 (no host call), run on Halo 2's thread right after the
	// frame-boundary event drain (drainDetour). The window size is unchanged, so MCC's swap chain needs nothing.
	// True when the rebuild will run at the next frame boundary.
	bool requestRebuild()
	{
		if (!mDrainHook || !mDrainHook->isHookInstalled() || !mRectHook->isHookInstalled()) return false;
		sRebuildPostedMs = GetTickCount64();
		sRebuildPending = true;
		PLOG_INFO << "Halo 2 supersampling: in-place rebuild of the render targets requested (runs at the next frame boundary)";
		return true;
	}

	void onMCCStateChanged(const MCCState& s)
	{
		if (s.currentGameState != mGame) return;
		std::scoped_lock lk(mMutex);
		if (mWanted > 1.f || sOverscanX.load() > 0.f || sOverscanY.load() > 0.f) ensureHooks();
	}

	// TAA overscan (H2Overscan.h), called by H2TAA with the wanted percent per side, left / right and top / bottom (0 = off):
	// the targets are rebuilt in place at the next frame boundary (requestRebuild). 1 applied (or already as wanted), 0 set -
	// applies once it can be rebuilt (Halo 2 not loaded yet), -1 other Halo 2 build.
	int requestOverscan(float px, float py)
	{
		std::scoped_lock lk(mMutex);
		if (!std::isfinite(px) || px < 0.f) px = 0.f;
		if (!std::isfinite(py) || py < 0.f) py = 0.f;
		if (px == sOverscanX.load() && py == sOverscanY.load()) return 1;   // nothing changed
		const float bx = sOverscanX.exchange(px), by = sOverscanY.exchange(py);
		const bool needHooks = px > 0.f || py > 0.f || sFactor.load() > 1.f;
		if (needHooks && ensureHooks() < 0) { sOverscanX = bx; sOverscanY = by; return -1; }
		if (!needHooks && !h2os::gActive.load()) return 1;   // off, and the targets are already normal
		return requestRebuild() ? 1 : 0;
	}

	static int requestOverscanStatic(float px, float py)
	{
		H2SupersamplingImpl* self = sSelf.load();
		return self ? self->requestOverscan(px, py) : -1;
	}

	std::unique_ptr<ModuleInlineHook> makeHook(uintptr_t rva, void* detour)
	{
		auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)rva });
		return ModuleInlineHook::make(L"halo2.dll", ptr, detour, false);
	}

public:
	H2SupersamplingImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		mChangeCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2Supersampling->valueChangedEvent,
			[this](SettingsEnums::H2Supersampling& v) { onChange(v); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		if (game.operator GameState::Value() != GameState::Value::Halo2) throw HCMInitException("Halo 2 supersampling is Halo 2 only");
		if (sSelf.load()) throw HCMInitException("Halo 2 supersampling already exists");
		mRectHook = makeHook(kSetRenderRectRva, (void*)&rectDetour);
		mWinRectHook = makeHook(kWindowRectRva, (void*)&winRectDetour);
		mWinHHook = makeHook(kWindowHeightRva, (void*)&winHDetour);
		mWinWHook = makeHook(kWindowWidthRva, (void*)&winWDetour);
		mHudHook = makeHook(kHudRva, (void*)&hudDetour);
		mFinalHook = makeHook(kFinalPassRva, (void*)&finalDetour);
		mPixelShaderHook = makeHook(kSetPixelShaderRva, (void*)&pixelShaderDetour);
		mHudPassHook = makeHook(kHudPassRva, (void*)&windowPassDetour<&H2SupersamplingImpl::mHudPassHook>);
		mFadePassHook = makeHook(kFadePassRva, (void*)&windowPassDetour<&H2SupersamplingImpl::mFadePassHook>);
		mHudScaleHook = makeHook(kHudScaleSetupRva, (void*)&hudScaleDetour);
		mDrainHook = makeHook(kEventDrainRva, (void*)&drainDetour);
		sSelf = this;
		const float f = factorOf(dicon.Resolve<SettingsStateAndEvents>().lock()->h2Supersampling->GetValue());
		sFactor = f; mWanted = f;
		if (f > 1.f) { std::scoped_lock lk(mMutex); ensureHooks(); }
		h2os::gRequest = &requestOverscanStatic;
	}

	~H2SupersamplingImpl()
	{
		h2os::gRequest = nullptr;
		// HCM closing with scaled targets: have the engine rebuild them at normal size first (our drain hook runs it on Halo 2's
		// thread at the next frame boundary; with the factors at 1 the rebuild comes out normal). Wait up to 0.5 s - a paused or
		// loading game may not reach a frame boundary, then the targets stay as they are until the next resolution change.
		const bool wasScaled = sApplied.load() > 1.f;
		sFactor = 1.f; sOverscanX = 0.f; sOverscanY = 0.f;
		if (wasScaled)
		{
			bool posted = false;
			{ std::scoped_lock lk(mMutex); posted = requestRebuild(); }
			for (int i = 0; posted && i < 500 && sRebuildPending.load(); ++i) Sleep(1);
			if (posted && sRebuildPending.exchange(false)) PLOG_WARNING << "Halo 2 supersampling: no frame boundary within 0.5 s of closing - the targets stay scaled until the next resolution change";
		}
		h2os::gActive = false;
		sApplied = 1.f; sAppliedX = 1.f; sAppliedY = 1.f; h2os::gOverscan = false;
		for (auto* h : { &mRectHook, &mWinRectHook, &mWinHHook, &mWinWHook, &mHudHook, &mFinalHook, &mPixelShaderHook, &mHudPassHook, &mFadePassHook, &mHudScaleHook, &mDrainHook }) if (*h) (*h)->setWantsToBeAttached(false);
		for (int i = 0; i < 500 && sInFlight.load() > 0; ++i) Sleep(1);
		sSelf = nullptr;
	}
};

H2Supersampling::H2Supersampling(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<H2SupersamplingImpl>(gameImpl, dicon))
{
}

H2Supersampling::~H2Supersampling() = default;
