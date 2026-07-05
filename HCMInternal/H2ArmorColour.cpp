#include "pch.h"
#include "H2ArmorColour.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "GetPlayerDatum.h"
#include "GetObjectAddress.h"
#include "GameTickEventHook.h"
#include "IMakeOrGetCheat.h"
#include "DirPathContainer.h"
#include "DirectXRenderEvent.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "ModalDialogGuard.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <Windows.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <vector>
#include <mutex>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3dcompiler.lib")

// object_set_change_color(uint32 objectDatum, int colorIndex, real_rgb_color* rgb) = halo2.dll sub_1808D9BA0.
// Resolves the datum, writes the 12-byte RGB into both halves of the object's change-color block
// (obj+0x12A offset, obj+0x128 size), then invalidates the render-colour cache (sub_1807E5030). MUST run on
// the game thread (mutates gamestate + render-state) - hence GameTickEventHook, not a render/CE thread.
// On toggle-off we write the biped's ORIGINAL colours back (captured before our first override) so it
// returns to normal live - also on the game thread, for the same reason.
namespace
{
	constexpr uint32_t kSetChangeColorRVA = 0x8D9BA0;
	constexpr uint8_t  kSetChangeColorProlog[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x57 };
	constexpr uint16_t kBlockSizeOff   = 0x128; // int16: change-color block size (bytes)
	constexpr uint16_t kBlockOffsetOff = 0x12A; // int16: change-color block offset (relative to object base)

	typedef char(__fastcall* set_change_color_t)(uint32_t objectDatum, int colorIndex, void* rgb);

	// --- Emblem pixel-injection (TEST): hijack MCC's per-player emblem SRV override on the biped ---
	// halo2.dll sub_1809548A0(int16 slot, uint32 tagIdx, int16 sprite) binds the emblem sprite's
	// ID3D11ShaderResourceView via sub_180965BE0 -> ppImmediateContext->PSSetShaderResources(slot,1,&srv).
	// When tagIdx == the fg-emblem tag (cached @dword_180E190AC) and sprite != 0, we call original then rebind
	// OUR srv over the same slot (using the engine's own bind, so its qword_1819A28F0 cache stays consistent).
	// STEP 1 proof: rebind a solid test texture on ALL bipeds. See memory h2-emblem-system.
	constexpr uintptr_t kEmblemBindRVA  = 0x9548A0; // sub_1809548A0(int16 slot, uint32 tagIdx, int16 sprite)
	constexpr uintptr_t kBindTexRVA     = 0x965BE0; // sub_180965BE0(uint slot, void* srv)
	constexpr uintptr_t kFgEmblemTagRVA = 0xE190AC; // dword_180E190AC = cached fg emblem bitmap tag index
	// local-player scoping: qword_18197EEB8 = the per-player emblem object (*(player+8)) being drawn right now.
	// The player-config array: base = *(qword_180E80A28 + 72) + qword_180E80A28; entry stride 548; +44 = biped
	// object datum, +8 = emblem object. We find the local player's emblem object and only inject when they match.
	constexpr uintptr_t kInjectOverrideRVA  = 0x197EEB8; // qword_18197EEB8 (per-player emblem object, one-shot)
	constexpr uintptr_t kPlayerCtxGlobalRVA = 0xE80A28;  // qword_180E80A28
	constexpr int       kPlayerStride       = 548;
	constexpr int       kPlayerBipedOff     = 44;        // player+44 = biped object datum
	constexpr int       kPlayerEmblemObjOff = 8;         // player+8  = emblem object
	// sub_180980F20(int propType, float* out) = emblem render-property getter. For propType 21 & 22 the colour
	// written to out[0..2] comes from the EMBLEM OBJECT (*(player+8)) via device method 592 (v58 = fg-primary,
	// v57 = fg-secondary) - these are EMBLEM-SPECIFIC. (propTypes 3/4 are the player's armour change-colours -
	// do NOT touch those.) Forcing 21 & 22 to white makes our injected texture render untinted, armour intact.
	constexpr uintptr_t kEmblemPropRVA = 0x980F20; // sub_180980F20(int propType, float* out)
	using bindTex_t = char(__fastcall*)(uint32_t slot, uintptr_t srv);
	using emblemBind_t = char(__fastcall*)(int16_t slot, uint32_t tagIdx, int16_t sprite);

	// Decode a PNG (or any WIC format) file to an ID3D11ShaderResourceView (RGBA8). Returns nullptr on failure.
	ID3D11ShaderResourceView* loadImageToSRV(ID3D11Device* device, const wchar_t* path)
	{
		if (!device) return nullptr;
		HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // tolerate already-initialised COM
		ID3D11ShaderResourceView* outSRV = nullptr;
		IWICImagingFactory* factory = nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		{
			IWICBitmapDecoder* decoder = nullptr;
			if (SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
			{
				IWICBitmapFrameDecode* frame = nullptr;
				if (SUCCEEDED(decoder->GetFrame(0, &frame)))
				{
					IWICFormatConverter* conv = nullptr;
					if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
						SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
					{
						UINT w = 0, h = 0; conv->GetSize(&w, &h);
						if (w && h)
						{
							std::vector<uint8_t> pixels((size_t)w * h * 4);
							if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data())))
							{
								D3D11_TEXTURE2D_DESC td{};
								td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
								td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
								td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
								D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = pixels.data(); sd.SysMemPitch = w * 4;
								ID3D11Texture2D* tex = nullptr;
								if (SUCCEEDED(device->CreateTexture2D(&td, &sd, &tex)) && tex)
								{
									device->CreateShaderResourceView(tex, nullptr, &outSRV);
									tex->Release();
								}
							}
						}
						conv->Release();
					}
					frame->Release();
				}
				decoder->Release();
			}
			factory->Release();
		}
		if (SUCCEEDED(co)) CoUninitialize();
		return outSRV;
	}
}


template <GameState::Value gameT>
class H2ArmorColourImpl : public IH2ArmorColourImpl
{
private:
	GameState mGame;
	int mVerifyState = 0; // 0 = unchecked, 1 = ok, 2 = failed (wrong build)

	// original colours captured before our first override, restored on toggle-off (game thread)
	bool  mCapturedOriginal = false;
	float mOrigPrimary[3]   = { 0,0,0 };
	float mOrigSecondary[3] = { 0,0,0 };

	std::string mPresetDir; // <hcm dir>/armour_colours

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<GetPlayerDatum> getPlayerDatumWeak;
	std::weak_ptr<GetObjectAddress> getObjectAddressWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;

	std::shared_ptr<ObservedEvent<GameTickEvent>> mGameTickEvent;
	std::unique_ptr<ScopedCallback<GameTickEvent>> mGameTickEventCallback;
	ScopedCallback<ActionEvent> mSavePresetCallback;
	ScopedCallback<ActionEvent> mLoadPresetCallback;

	// --- Custom emblem (pixel-injection) --- inline-hooks the emblem sprite-bind while the Custom Emblem
	// toggle is on, and rebinds OUR loaded PNG texture whenever the fg emblem tag draws.
	std::shared_ptr<ModuleInlineHook> mEmblemHook;
	std::unique_ptr<ScopedCallback<DirectXRenderEvent>> mRenderCallback;
	ScopedCallback<ActionEvent> mLoadEmblemCallback;
	bool mEmblemHookWanted = false;
	std::mutex mEmblemPathMutex;
	std::wstring mEmblemPngPath;                    // chosen custom emblem image (set by the Load Emblem button)
	std::atomic_bool mEmblemReload{ false };        // render thread (re)loads the SRV from mEmblemPngPath
	static inline ModuleInlineHook* sEmblemHook = nullptr;                 // for the static detour to call original
	static inline std::atomic<ID3D11ShaderResourceView*> sTestSRV{ nullptr };
	static inline std::atomic_uintptr_t sHalo2Base{ 0 };
	// the local player's emblem object (*(myPlayer+8)); 0 = "couldn't resolve" -> inject on ALL bipeds (fallback)
	static inline std::atomic_uint64_t sMyEmblemObj{ 0 };

	// Find the local player's emblem object by matching the biped datum in the player-config array.
	static uint64_t resolveMyEmblemObject(uintptr_t base, Datum myBiped)
	{
		if (!base || myBiped.isNull()) return 0;
		if (IsBadReadPtr((void*)(base + kPlayerCtxGlobalRVA), 8)) return 0;
		uint64_t g = *(uint64_t*)(base + kPlayerCtxGlobalRVA);
		if (!g || IsBadReadPtr((void*)(g + 72), 8)) return 0;
		uint64_t off = *(uint64_t*)(g + 72);
		if (!off) return 0;
		uintptr_t playerBase = (uintptr_t)(off + g);
		uint16_t myIdx = myBiped.index;
		for (int i = 0; i < 16; ++i)
		{
			uintptr_t p = playerBase + (uintptr_t)kPlayerStride * i;
			if (IsBadReadPtr((void*)(p + kPlayerBipedOff), 4)) break;
			uint32_t pBiped = *(uint32_t*)(p + kPlayerBipedOff);
			if (pBiped != 0xFFFFFFFFu && (uint16_t)pBiped == myIdx)
			{
				if (IsBadReadPtr((void*)(p + kPlayerEmblemObjOff), 8)) return 0;
				return *(uint64_t*)(p + kPlayerEmblemObjOff);
			}
		}
		return 0;
	}

	// Detour: call original (binds MCC's emblem), then rebind our test SRV over the same slot for the fg tag.
	static char __fastcall emblemBindDetour(int16_t slot, uint32_t tagIdx, int16_t sprite)
	{
		uintptr_t base = sHalo2Base.load(std::memory_order_acquire);
		// capture which player's emblem is being drawn (the one-shot override) BEFORE original consumes it
		uint64_t drawingEmblemObj = base ? *(uint64_t*)(base + kInjectOverrideRVA) : 0;
		char result = sEmblemHook
			? sEmblemHook->getInlineHook().call<char, int16_t, uint32_t, int16_t>(slot, tagIdx, sprite)
			: 0;
		ID3D11ShaderResourceView* srv = sTestSRV.load(std::memory_order_acquire);
		if (srv && base && sprite != 0)
		{
			uint32_t fgTag = *(uint32_t*)(base + kFgEmblemTagRVA);
			uint64_t mine = sMyEmblemObj.load(std::memory_order_acquire);
			// scope to the local player's emblem; if we couldn't resolve it (mine==0), fall back to all bipeds
			bool isMine = (mine == 0) || (drawingEmblemObj == mine);
			if (tagIdx == fgTag && isMine)
			{
				auto bindTex = (bindTex_t)(base + kBindTexRVA);
				bindTex((uint32_t)(uint16_t)slot, (uintptr_t)srv);
				// FULL-COLOUR: arm the DrawIndexed hook so the NEXT draw uses our passthrough pixel shader
				// (outputs the texture RGB directly) instead of the emblem's luminance/2-tone shader.
				sEmblemArmed.store(true, std::memory_order_release);
			}
		}
		return result;
	}

	// --- FULL-COLOUR emblem: passthrough pixel shader swapped in for the emblem's own draw ---
	static inline ID3D11PixelShader* sPassPS = nullptr;
	static inline std::atomic_bool sEmblemArmed{ false };
	static inline std::atomic_bool sFullColour{ true }; // when off, keep the game's luminance/2-tone shader
	static inline safetyhook::VmtHook sCtxVmt;
	static inline safetyhook::VmHook  sDrawHook;
	static inline std::atomic_bool sDrawHookInit{ false };

	// DrawIndexed detour: when the emblem was just bound (armed), swap in our passthrough PS + our SRV for
	// this one draw so the emblem renders full-colour, then restore the previous PS.
	static void __stdcall drawIndexedDetour(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
	{
		if (ctx && sPassPS && sFullColour.load(std::memory_order_acquire) && sEmblemArmed.exchange(false, std::memory_order_acq_rel))
		{
			ID3D11PixelShader* prevPS = nullptr;
			ctx->PSGetShader(&prevPS, nullptr, nullptr);
			// leave the game's already-bound emblem texture (t0) + sampler (s0) in place - matching them exactly
			// is what makes the framing/edges match the real emblem shader. We only replace the pixel shader.
			ctx->PSSetShader(sPassPS, nullptr, 0);
			sDrawHook.stdcall<void>(ctx, indexCount, startIndex, baseVertex);
			ctx->PSSetShader(prevPS, nullptr, 0);
			if (prevPS) prevPS->Release();
			return;
		}
		sDrawHook.stdcall<void>(ctx, indexCount, startIndex, baseVertex);
	}

	// One-time: compile the passthrough PS and VMT-hook the context's DrawIndexed. (We use the game's own
	// texture + sampler at draw time - no sampler of our own is needed.)
	void setupFullColour(ID3D11Device* device, ID3D11DeviceContext* ctx)
	{
		if (sDrawHookInit.load(std::memory_order_acquire) || !device || !ctx) return;
		// Match the real emblem PS (emblem_overlay_simple) input signature EXACTLY so tc0 (and its .w for the
		// projective divide) line up with the vertex shader's interpolators. The UV is tc0.xy / tc0.w. We just
		// output the texture RGB (the game shader instead treats R/G/B as separate change-colour masks).
		static const char* psSrc =
			"Texture2D s0tex : register(t0);\n"
			"SamplerState s0 : register(s0);\n"
			"float4 main(float4 pos : SV_POSITION, float4 color0 : COLOR0, float4 color1 : COLOR1,\n"
			"  float4 tc0 : TEXCOORD0, float4 tc1 : TEXCOORD1, float4 tc2 : TEXCOORD2, float4 tc3 : TEXCOORD3,\n"
			"  float4 tc4 : TEXCOORD4, float4 tc5 : TEXCOORD5, float4 tc6 : TEXCOORD6, float4 tc7 : TEXCOORD7) : SV_Target\n"
			"{ return s0tex.Sample(s0, tc0.xy / tc0.w); }\n";
		ID3DBlob* bc = nullptr; ID3DBlob* err = nullptr;
		if (SUCCEEDED(D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &bc, &err)) && bc)
		{
			device->CreatePixelShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &sPassPS);
			bc->Release();
		}
		if (err) err->Release();
		if (sPassPS)
		{
			sCtxVmt = safetyhook::create_vmt((void*)ctx);
			sDrawHook = safetyhook::create_vm(sCtxVmt, 12, &drawIndexedDetour); // ID3D11DeviceContext::DrawIndexed
		}
		sDrawHookInit.store(true, std::memory_order_release);
	}

	// (Re)load the chosen custom emblem PNG on the render thread (needs the live D3D11 device). Fires when the
	// user picks a new image via the Load Emblem button (mEmblemReload).
	void onRenderEvent(ID3D11Device* device, ID3D11DeviceContext* ctx, SimpleMath::Vector2, ID3D11RenderTargetView*)
	{
		// one-time full-colour setup once we have a device+context and a loaded emblem (limits overhead to users
		// who actually use the feature)
		if (device && ctx && sTestSRV.load(std::memory_order_acquire)) setupFullColour(device, ctx);

		if (!device || !mEmblemReload.exchange(false, std::memory_order_acq_rel)) return;
		std::wstring path;
		{ std::lock_guard<std::mutex> lk(mEmblemPathMutex); path = mEmblemPngPath; }
		if (path.empty()) return;
		ID3D11ShaderResourceView* srv = loadImageToSRV(device, path.c_str());
		if (srv)
		{
			if (auto* old = sTestSRV.exchange(srv, std::memory_order_acq_rel)) old->Release();
			if (auto m = messagesWeak.lock()) m->addMessage("Custom emblem loaded.");
		}
		else if (auto m = messagesWeak.lock()) m->addMessage("Custom emblem: couldn't load that image.");
	}

	// Load Emblem button: pick a PNG (spawned thread -> modal dialog is fine), queue a reload on the render thread.
	void onLoadEmblem()
	{
		// one shared file dialog at a time (this cheat is built once per game; the shared guard stops N dialogs) - see ModalDialogGuard
		auto claim = ModalDialogGuard::tryClaim();
		if (!claim) return;
		wchar_t fileBuf[MAX_PATH]; fileBuf[0] = L'\0';
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetForegroundWindow();
		ofn.lpstrFilter = L"Image files (*.png;*.jpg;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.bmp;*.dds\0All Files (*.*)\0*.*\0";
		ofn.lpstrFile = fileBuf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = L"Choose a custom emblem image";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameW(&ofn)) return; // cancelled
		{ std::lock_guard<std::mutex> lk(mEmblemPathMutex); mEmblemPngPath = fileBuf; }
		mEmblemReload.store(true, std::memory_order_release);
	}

	// Build the common OPENFILENAME for our .mccolour presets. These handlers fire on GUISimpleButton's own
	// spawned thread, so the modal dialog doesn't block the render thread.
	std::optional<std::filesystem::path> browsePreset(bool saving)
	{
		std::error_code ec; std::filesystem::create_directories(mPresetDir, ec);
		std::wstring initDir = std::filesystem::path(mPresetDir).wstring();
		wchar_t fileBuf[MAX_PATH]; fileBuf[0] = L'\0';
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetForegroundWindow();
		ofn.lpstrFilter = L"MasterChief Colour Preset (*.mccolour)\0*.mccolour\0All Files (*.*)\0*.*\0";
		ofn.lpstrFile = fileBuf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrInitialDir = initDir.c_str();
		ofn.lpstrDefExt = L"mccolour";
		if (saving)
		{
			ofn.lpstrTitle = L"Save MasterChief Colour Preset";
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
			if (GetSaveFileNameW(&ofn)) return std::filesystem::path(fileBuf);
		}
		else
		{
			ofn.lpstrTitle = L"Load MasterChief Colour Preset";
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
			if (GetOpenFileNameW(&ofn)) return std::filesystem::path(fileBuf);
		}
		return std::nullopt; // cancelled
	}

	void onSavePreset()
	{
		auto claim = ModalDialogGuard::tryClaim();
		if (!claim) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			auto p = settings->h2ArmorColourPrimary->GetValue();
			auto s = settings->h2ArmorColourSecondary->GetValue();
			auto chosen = browsePreset(true);
			if (!chosen) return;
			std::ofstream out(*chosen, std::ios::trunc);
			if (!out) { if (auto m = messagesWeak.lock()) m->addMessage("Colour preset: couldn't write file."); return; }
			out << "HCM_MC_Colour 1\n";
			out << "primary "   << p.x << ' ' << p.y << ' ' << p.z << '\n';
			out << "secondary " << s.x << ' ' << s.y << ' ' << s.z << '\n';
			if (auto m = messagesWeak.lock()) m->addMessage("Colour preset saved: " + chosen->filename().string());
		}
		catch (HCMRuntimeException&) {}
	}

	void onLoadPreset()
	{
		auto claim = ModalDialogGuard::tryClaim();
		if (!claim) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			auto chosen = browsePreset(false);
			if (!chosen) return;
			std::ifstream in(*chosen);
			if (!in) { if (auto m = messagesWeak.lock()) m->addMessage("Colour preset: couldn't open file."); return; }

			SimpleMath::Vector4 prim = settings->h2ArmorColourPrimary->GetValue();
			SimpleMath::Vector4 sec  = settings->h2ArmorColourSecondary->GetValue();
			std::string line, tag; float r, g, b;
			while (std::getline(in, line))
			{
				std::istringstream ss(line); ss >> tag;
				if (tag == "primary" && (ss >> r >> g >> b)) prim = { r, g, b, 1.0f };
				else if (tag == "secondary" && (ss >> r >> g >> b)) sec = { r, g, b, 1.0f };
			}
			// push into the settings so the pickers update (and valueChanged fires)
			settings->h2ArmorColourPrimary->GetValueDisplay() = prim;
			settings->h2ArmorColourPrimary->UpdateValueWithInput();
			settings->h2ArmorColourSecondary->GetValueDisplay() = sec;
			settings->h2ArmorColourSecondary->UpdateValueWithInput();
			if (auto m = messagesWeak.lock()) m->addMessage("Colour preset loaded: " + chosen->filename().string());
		}
		catch (HCMRuntimeException&) {}
	}

	bool setterOk(uintptr_t base)
	{
		if (mVerifyState == 1) return true;
		if (mVerifyState == 2) return false;
		mVerifyState = (memcmp((void*)(base + kSetChangeColorRVA), kSetChangeColorProlog, sizeof(kSetChangeColorProlog)) == 0) ? 1 : 2;
		if (mVerifyState == 2)
			PLOG_ERROR << "H2ArmorColour: object_set_change_color bytes don't match build 1.3528; feature disabled.";
		return mVerifyState == 1;
	}

	// read the biped's current slot-0/slot-1 colours (the "normal" ones) before we override them
	void captureOriginal(Datum player)
	{
		try
		{
			lockOrThrow(getObjectAddressWeak, getObjectAddress);
			uintptr_t obj = getObjectAddress->getObjectAddress(player);
			if (!obj) return;
			uint16_t size = *(uint16_t*)(obj + kBlockSizeOff);
			if (size < 24) return; // fewer than 2 colour slots - nothing meaningful to capture/restore
			float* block = (float*)(obj + *(uint16_t*)(obj + kBlockOffsetOff));
			for (int i = 0; i < 3; ++i) { mOrigPrimary[i] = block[i]; mOrigSecondary[i] = block[3 + i]; }
			mCapturedOriginal = true;
		}
		catch (HCMRuntimeException&) {}
	}

	void onGameTick(uint32_t)
	{
		try
		{
			lockOrThrow(settingsWeak, settings);

			// custom emblem: (dis)attach the injection + colour hooks with the Custom Emblem toggle
			bool wantEmblem = settings->h2ArmorEmblemToggle->GetValue();
			if (wantEmblem != mEmblemHookWanted && mEmblemHook)
			{
				mEmblemHook->setWantsToBeAttached(wantEmblem);
				mEmblemHookWanted = wantEmblem;
			}

			lockOrThrow(getPlayerDatumWeak, getPlayerDatum);

			Datum player = getPlayerDatum->getPlayerDatum();
			if (player.isNull()) return; // dead / not spawned

			uintptr_t base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return;
			// scope the custom emblem to the local player's biped (0 = unresolved -> inject on all, as before)
			sMyEmblemObj.store(resolveMyEmblemObject(base, player), std::memory_order_release);
			if (!setterOk(base)) return;
			auto setColor = (set_change_color_t)(base + kSetChangeColorRVA);

			if (settings->h2ArmorColourToggle->GetValue())
			{
				if (!mCapturedOriginal) captureOriginal(player); // snapshot the normal colours first
				auto pc = settings->h2ArmorColourPrimary->GetValue();
				auto sc = settings->h2ArmorColourSecondary->GetValue();
				float primary[3]   = { pc.x, pc.y, pc.z };
				float secondary[3] = { sc.x, sc.y, sc.z };
				setColor((uint32_t)player, 0, primary);   // primary armour
				setColor((uint32_t)player, 1, secondary);  // secondary / trim
			}
			else if (mCapturedOriginal)
			{
				// toggled off: put the original colours back, then stop until re-enabled
				setColor((uint32_t)player, 0, mOrigPrimary);
				setColor((uint32_t)player, 1, mOrigSecondary);
				mCapturedOriginal = false;
			}
		}
		catch (HCMRuntimeException&) {}
	}

public:
	H2ArmorColourImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		getPlayerDatumWeak(resolveDependentCheat(GetPlayerDatum)),
		getObjectAddressWeak(resolveDependentCheat(GetObjectAddress)),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		mSavePresetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2ArmorColourSavePresetEvent, [this]() { onSavePreset(); }),
		mLoadPresetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2ArmorColourLoadPresetEvent, [this]() { onLoadPreset(); }),
		mLoadEmblemCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2ArmorEmblemLoadEvent, [this]() { onLoadEmblem(); })
	{
		mPresetDir = (std::filesystem::path(dicon.Resolve<DirPathContainer>().lock()->dirPath) / "armour_colours").string();

		auto gameTickEventHook = resolveDependentCheat(GameTickEventHook);
		mGameTickEvent = gameTickEventHook->getGameTickEvent();
		// tick fires only while a game is running; the handler gates on the toggle + a live player datum
		mGameTickEventCallback = mGameTickEvent->subscribe([this](uint32_t t) { onGameTick(t); });

		// --- emblem-injection test setup ---
		sHalo2Base.store((uintptr_t)GetModuleHandleW(L"halo2.dll"), std::memory_order_release);
		auto emblemBindFn = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(
			mGame.toModuleName(), std::vector<int64_t>{ (int64_t)kEmblemBindRVA });
		mEmblemHook = ModuleInlineHook::make(mGame.toModuleName(), emblemBindFn, &emblemBindDetour);
		sEmblemHook = mEmblemHook.get();
		mRenderCallback = std::make_unique<ScopedCallback<DirectXRenderEvent>>(
			dicon.Resolve<DirectXRenderEvent>().lock(),
			[this](ID3D11Device* d, ID3D11DeviceContext* c, SimpleMath::Vector2 s, ID3D11RenderTargetView* r) { onRenderEvent(d, c, s, r); });
	}

	~H2ArmorColourImpl()
	{
		mGameTickEventCallback.reset();
		if (mEmblemHook) mEmblemHook->setWantsToBeAttached(false);
		sEmblemArmed.store(false);
		sDrawHook = {};   // unhook DrawIndexed
		sCtxVmt = {};
		sEmblemHook = nullptr;
		if (auto* srv = sTestSRV.exchange(nullptr)) srv->Release();
		if (sPassPS) { sPassPS->Release(); sPassPS = nullptr; }
		sDrawHookInit.store(false);
	}
};


H2ArmorColour::H2ArmorColour(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<H2ArmorColourImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("H2ArmorColour not impl for this game");
	}
}

H2ArmorColour::~H2ArmorColour()
{
	PLOG_DEBUG << "~" << getName();
}
