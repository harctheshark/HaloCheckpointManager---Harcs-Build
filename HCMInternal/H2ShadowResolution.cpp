#include "pch.h"
#include "H2ShadowResolution.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "DirectXRenderEvent.h"
#include "ScopedThreadSuspender.h"
#include <Windows.h>
#include <vector>
#include <atomic>
#include <cstring>

// ============================================================================
//  Halo 2 dynamic shadow-buffer resolution (MCC halo2.dll, build 1.3528).
//  RE map + sapien original in memory h2-shadow-buffer-resolution.
//
//  The whole projected-shadow pipeline is sized off ONE global, dword_180E13490:
//    sub_180961D20 (rasterizer_targets_initialize): target_shadow_buffer(9) = res x res,
//      target_shadow/scratch(10/11/12) = res>>2, alias_swizzled(13) = res>>1, (21/22) = res x res>>1.
//    sub_1809772B0 / sub_180977540 (shadowbuffer.cpp): shadow texel math samples 1/res.
//  The value is chosen by the detail-preset sub_1807E1430, which writes one of 256/512/1024 to that
//  global. We patch those three immediates to the requested square resolution (PERSISTENCE - so any
//  engine-driven rasterizer reset re-derives the chosen size), then trigger a LIVE rebuild ourselves.
//
//  Live rebuild: the shadow render-targets are cached (target_create sub_180961730 early-outs if the
//  descriptor's d3d resource != 0), and a normal window/fullscreen toggle doesn't release them - only a
//  real resolution change does. So we do the minimal rebuild ourselves ON THE RENDER THREAD (via HCM's
//  ForegroundDirectXRenderEvent, fired during Present): write the global, release ONLY the shadow targets
//  (sub_180961A00 for 9/10/11/12/13/21/22 - zeroes descriptor+0), then re-run targets-init (sub_180961D20;
//  the non-shadow targets early-out, so only the shadow ones rebuild at the new size). This never touches
//  the backbuffer/primary target that ImGui is drawing to. Fully reverted on Retail / disable / unload.
// ============================================================================

namespace
{
	// --- preset writer sites: three `C7 05 <disp32> <imm32>` in sub_1807E1430; imm32 at instr+6 ---
	struct Site { uint32_t instrRVA; uint32_t immRVA; uint32_t stockVal; };
	constexpr Site kSites[] = {
		{ 0x7E14FE, 0x7E1504, 1024 }, // high/default branch
		{ 0x7E1577, 0x7E157D, 256  }, // lowest branch (detail == 2)
		{ 0x7E1590, 0x7E1596, 512  }, // low branch    (detail == 1)
	};

	// --- engine RVAs (build 1.3528) ---
	constexpr uint32_t kShadowResGlobalRVA = 0xE13490; // dword_180E13490
	constexpr uint32_t kReleaseTargetRVA   = 0x961A00; // sub_180961A00(int idx): release one rasterizer target
	constexpr uint32_t kTargetsInitRVA     = 0x961D20; // sub_180961D20(): rebuild all targets (cached ones early-out)
	// shadow targets sized off the global (target indices)
	constexpr int kShadowTargets[] = { 9, 10, 11, 12, 13, 21, 22 };

	// --- "Force L6 cinematic LOD" patch (auto-engaged when shadow res >= this many px) ---
	// In the object LOD-setup fn sub_1807EC700, the ONLY path that yields detail level 5 (L6) is the
	// "cinematic" bypass: `test r9b,r9b; jnz *a7=5` (r9b = object_flags bit 0x80000). Everything else is
	// clamped by the detail cap (dword_18165C4DC = 4). Forcing that jnz unconditional makes every object
	// render at L6, skipping the cap - i.e. cutscene-quality geometry always. LOD is recomputed per object
	// every frame, so this applies live with no rebuild.
	constexpr uint32_t kLodForceThresholdPx = 2048;
	constexpr uint32_t kLodJnzRVA = 0x7EC8C8; // jnz loc_1807EC963  (0F 85 95 00 00 00)
	constexpr uint8_t  kLodJnzOrig[6] = { 0x0F, 0x85, 0x95, 0x00, 0x00, 0x00 };
	constexpr uint8_t  kLodJmpNew[6]  = { 0xE9, 0x96, 0x00, 0x00, 0x00, 0x90 }; // jmp loc_1807EC963 + nop

	// --- "Disable object distance-culling" patch (engaged with the L6-LOD force) ---
	// SOLVED 2026-07-03 (memory h2-object-distance-cull): the object "cull" is NOT a visibility test - it's the
	// per-object LOD FADE-ALPHA snapping to 0. In the fade tail, beyond the model's disappear distance the code
	// does `xorps xmm1,xmm1` (alpha 0), then `mulss xmm1,255.0 ; cvttss2si eax,xmm1 ; mov [r15],al` stores a 0
	// alpha byte -> object invisible (H2's smooth fade is broken so it just snaps out). Forcing that alpha byte
	// to 0xFF (fully opaque) makes every object draw at all distances. We overwrite the `cvttss2si eax,xmm1`
	// with `mov al,0xFF ; nop ; nop`; the following `mov [r15],al` then always stores 255. Recomputed per object
	// every frame, so it applies live with no rebuild. Proven in-game.
	constexpr uint32_t kCullRVA = 0x7EBC1F;                             // cvttss2si eax,xmm1
	constexpr uint8_t  kCullOrig[4]   = { 0xF3, 0x0F, 0x2C, 0xC1 };     // cvttss2si eax,xmm1
	constexpr uint8_t  kCullNew[4]    = { 0xB0, 0xFF, 0x90, 0x90 };     // mov al,0xFF ; nop ; nop
	constexpr uint8_t  kCullAnchor[3] = { 0x41, 0x88, 0x07 };           // mov [r15],al (follows, build-guard)

	class Patcher
	{
		uintptr_t base = 0;
		bool applied = false;

		struct Save { uintptr_t addr; std::vector<uint8_t> orig; };
		std::vector<Save> saves;

		void writeSaved(uintptr_t addr, const uint8_t* data, size_t len)
		{
			Save s; s.addr = addr; s.orig.resize(len);
			memcpy(s.orig.data(), (void*)addr, len);
			saves.push_back(std::move(s));
			DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, len);
			VirtualProtect((void*)addr, len, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
		}

		bool verify()
		{
			for (const Site& s : kSites)
			{
				const uint8_t* instr = (const uint8_t*)(base + s.instrRVA);
				if (instr[0] != 0xC7 || instr[1] != 0x05) return false;
			}
			// LOD-force site: the jnz + the `mov byte[rsi],5` target it jumps to
			if (memcmp((void*)(base + kLodJnzRVA), kLodJnzOrig, sizeof(kLodJnzOrig)) != 0) return false;
			static const uint8_t movTgt[3] = { 0xC6, 0x06, 0x05 };
			if (memcmp((void*)(base + 0x7EC963), movTgt, sizeof(movTgt)) != 0) return false;
			// distance-cull (fade-alpha) site: cvttss2si eax,xmm1 followed by mov [r15],al
			if (memcmp((void*)(base + kCullRVA), kCullOrig, sizeof(kCullOrig)) != 0) return false;
			if (memcmp((void*)(base + kCullRVA + 4), kCullAnchor, sizeof(kCullAnchor)) != 0) return false;
			return true;
		}

	public:
		uintptr_t getBase() const { return base; }
		bool isApplied() const { return applied; }

		// Patch the three preset immediates to sizePx (0 = restore stock), and engage the L6-cinematic LOD
		// force when sizePx >= kLodForceThresholdPx. Returns false (no throw) if halo2.dll isn't loaded yet;
		// throws on a real byte mismatch.
		// sizePx: 0 = leave engine default, else force that square resolution. forceLOD: engage the
		// L6-cinematic-LOD patch. Returns false (no throw) if halo2.dll isn't loaded; throws on byte mismatch.
		bool apply(uint32_t sizePx, bool forceLOD)
		{
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;

			revert(); // restore stock first, so verify() and `saves` see the true originals

			if (!verify())
				throw HCMRuntimeException("H2ShadowResolution: halo2.dll bytes don't match build 1.3528; refusing to patch.");

			if (sizePx == 0 && !forceLOD) return true; // Retail

			// shadow-buffer resolution (three preset immediates)
			if (sizePx != 0)
			{
				uint8_t nb[4]; memcpy(nb, &sizePx, 4);
				for (const Site& s : kSites)
					writeSaved(base + s.immRVA, nb, 4);
			}

			// force L6 cinematic LOD + disable object distance-culling (fade-alpha -> 0xFF)
			if (forceLOD)
			{
				writeSaved(base + kLodJnzRVA, kLodJmpNew, sizeof(kLodJmpNew));
				writeSaved(base + kCullRVA, kCullNew, sizeof(kCullNew));
			}

			applied = true;
			return true;
		}

		void revert()
		{
			// restore patched bytes with other threads suspended so the render thread can't execute a
			// half-restored instruction mid-revert (races and can crash the game). Only the memory
			// writes are inside the suspend window; saves.clear() (heap) stays outside. (ScopedThreadSuspender.)
			{
				ScopedThreadSuspender suspend;
				for (auto it = saves.rbegin(); it != saves.rend(); ++it)
				{
					size_t len = it->orig.size();
					DWORD o; VirtualProtect((void*)it->addr, len, PAGE_EXECUTE_READWRITE, &o);
					memcpy((void*)it->addr, it->orig.data(), len);
					VirtualProtect((void*)it->addr, len, o, &o);
					FlushInstructionCache(GetCurrentProcess(), (void*)it->addr, len);
				}
			}
			saves.clear();
			applied = false;
		}

		~Patcher() { if (applied && GetModuleHandleA("halo2.dll")) revert(); }
	};

	uint32_t sizeForEnum(SettingsEnums::H2ShadowResolution v)
	{
		using E = SettingsEnums::H2ShadowResolution;
		switch (v)
		{
		case E::RetailLOD: return 1024;
		case E::x2048:     return 2048;
		case E::x4096:     return 4096;
		case E::x8192:     return 8192;
		case E::Retail:
		default:           return 0; // leave engine default
		}
	}

	// everything except Retail forces L6 LOD and disables object distance-culling
	bool extrasFor(SettingsEnums::H2ShadowResolution v) { return v != SettingsEnums::H2ShadowResolution::Retail; }

	const char* labelForEnum(SettingsEnums::H2ShadowResolution v)
	{
		using E = SettingsEnums::H2ShadowResolution;
		switch (v)
		{
		case E::RetailLOD: return "Shadow: Retail (1024) + max LOD + no distance cull";
		case E::x2048:     return "Shadow 2048 + max LOD + no distance cull";
		case E::x4096:     return "Shadow 4096 + max LOD + no distance cull";
		case E::x8192:     return "Shadow 8192 + max LOD + no distance cull (extreme VRAM)";
		case E::Retail:
		default:           return "Shadow resolution: Retail (engine default)";
		}
	}
} // namespace


template <GameState::Value gameT>
class H2ShadowResolutionImpl : public IH2ShadowResolutionImpl
{
private:
	GameState mGame;
	Patcher mPatcher;
	SettingsEnums::H2ShadowResolution mDesired = SettingsEnums::H2ShadowResolution::Retail;

	// live-rebuild state (written on the settings/UI thread, consumed on the render thread)
	std::atomic<bool> mPendingRebuild{ false };
	uint32_t mPendingGlobal = 0;   // value to write into dword_180E13490 before the rebuild
	uint32_t mStockGlobal = 0;     // captured launch-time global (for Retail restore)
	bool mStockCaptured = false;

	ScopedCallback<eventpp::CallbackList<void(SettingsEnums::H2ShadowResolution&)>> mValueChangedCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	ScopedCallback<DirectXRenderEvent> mRenderCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	// Patch the preset immediates (persistence) and queue a live rebuild. Returns false if halo2 not loaded.
	bool applyValue(SettingsEnums::H2ShadowResolution v)
	{
		uint32_t sizePx = sizeForEnum(v);
		if (!mPatcher.apply(sizePx, extrasFor(v))) return false; // deferred (halo2.dll not loaded)

		uintptr_t base = mPatcher.getBase();
		if (!mStockCaptured)
		{
			mStockGlobal = *(volatile uint32_t*)(base + kShadowResGlobalRVA); // stock value, before any rebuild
			mStockCaptured = true;
		}
		mPendingGlobal = sizePx ? sizePx : mStockGlobal;
		mPendingRebuild.store(true, std::memory_order_release);
		return true;
	}

	// Runs on the render thread (Present). Does the minimal shadow-target rebuild once when queued.
	void onRenderEvent(ID3D11Device*, ID3D11DeviceContext*, SimpleMath::Vector2, ID3D11RenderTargetView*)
	{
		if (!mPendingRebuild.load(std::memory_order_acquire)) return;

		uintptr_t base = (uintptr_t)GetModuleHandleA("halo2.dll");
		if (!base) { mPendingRebuild.store(false); return; }

		// 1) set the resolution global (drives both the target alloc size and the shadow texel math)
		*(volatile uint32_t*)(base + kShadowResGlobalRVA) = mPendingGlobal;

		// 2) release ONLY the shadow targets (zeroes their descriptor so target_create will rebuild them)
		auto releaseTarget = (void(__fastcall*)(int))(base + kReleaseTargetRVA);
		for (int idx : kShadowTargets) releaseTarget(idx);

		// 3) re-run targets-init: recreates the released shadow targets at the new size; every other target
		//    still exists so it early-outs. Never touches the backbuffer/primary that ImGui is drawing to.
		auto targetsInit = (char(__fastcall*)())(base + kTargetsInitRVA);
		targetsInit();

		mPendingRebuild.store(false, std::memory_order_release);
	}

	void onValueChanged(SettingsEnums::H2ShadowResolution& newValue)
	{
		mDesired = newValue;
		PLOG_DEBUG << "H2ShadowResolution onValueChanged: " << (int)newValue;
		try
		{
			if (!applyValue(newValue))
				PLOG_DEBUG << "H2ShadowResolution: deferred (halo2.dll not loaded yet)";
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << ex.what();
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {}
			return;
		}

		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(labelForEnum(newValue)); }
		catch (HCMRuntimeException&) {}
	}

	// Map (re)loaded: all patches are halo2.dll code bytes (shadow presets, L6 LOD, distance-cull alpha) so they
	// persist across map loads - we only need to (re)apply if they were deferred because halo2.dll wasn't loaded
	// when the setting was first chosen.
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return;
		if (mDesired == SettingsEnums::H2ShadowResolution::Retail) return;
		if (mPatcher.isApplied()) return;
		try { applyValue(mDesired); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "H2ShadowResolution: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	H2ShadowResolutionImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mValueChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2ShadowResolutionSetting->valueChangedEvent, [this](SettingsEnums::H2ShadowResolution& n) { onValueChanged(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		mRenderCallback(dicon.Resolve<DirectXRenderEvent>().lock(), [this](ID3D11Device* d, ID3D11DeviceContext* c, SimpleMath::Vector2 s, ID3D11RenderTargetView* r) { onRenderEvent(d, c, s, r); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
		if (auto settings = settingsWeak.lock())
			mDesired = settings->h2ShadowResolutionSetting->GetValue();
	}

	~H2ShadowResolutionImpl()
	{
		// Stop the render callback from acting, then restore the stock preset immediates. We can't do a
		// live rebuild here (the render callback unsubscribes as this object dies); the live targets return
		// to stock on the next natural rasterizer reset. To revert live in-session, pick "Retail" instead.
		mPendingRebuild.store(false);
		try { mPatcher.revert(); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


H2ShadowResolution::H2ShadowResolution(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<H2ShadowResolutionImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("H2ShadowResolution not impl for this game");
	}
}

H2ShadowResolution::~H2ShadowResolution()
{
	PLOG_DEBUG << "~" << getName();
}
