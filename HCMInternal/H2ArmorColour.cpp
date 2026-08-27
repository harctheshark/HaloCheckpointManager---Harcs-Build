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
#include <d3d11_4.h>     // ID3D11Multithread (protect the device for shared use by MF's decode thread)
#include <d3dcompiler.h>
#include <wincodec.h>
#include <Windows.h>
#include <commdlg.h>
#include <mfapi.h>          // Media Foundation - decode MP4 (and other) video tracks for animated emblems
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <vector>
#include <mutex>
#include <thread>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

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

	// One decoded emblem frame: its SRV + how long to show it (ms). Static images = a single frame (delay 0).
	struct EmblemFrame { ID3D11ShaderResourceView* srv; uint32_t delayMs; };

	// Upload an RGBA8 buffer to a texture + SRV. Returns nullptr on failure.
	ID3D11ShaderResourceView* createSRVFromRGBA(ID3D11Device* device, UINT w, UINT h, const uint8_t* rgba)
	{
		if (!device || !w || !h || !rgba) return nullptr;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = rgba; sd.SysMemPitch = w * 4;
		ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
		if (SUCCEEDED(device->CreateTexture2D(&td, &sd, &tex)) && tex)
		{
			device->CreateShaderResourceView(tex, nullptr, &srv);
			tex->Release();
		}
		return srv;
	}

	// Read a WIC metadata UINT (UI1/UI2), or a default if absent.
	UINT readMetaUint(IWICMetadataQueryReader* r, const wchar_t* name, UINT def)
	{
		if (!r) return def;
		PROPVARIANT pv; PropVariantInit(&pv);
		UINT val = def;
		if (SUCCEEDED(r->GetMetadataByName(name, &pv)))
		{
			if (pv.vt == VT_UI2) val = pv.uiVal;
			else if (pv.vt == VT_UI1) val = pv.bVal;
		}
		PropVariantClear(&pv);
		return val;
	}

	// Decode an image file to one or more RGBA frames. Static formats (PNG/JPG/BMP/DDS, single-frame GIF) return one
	// frame; animated GIFs return every frame, disposal-composited onto a full-canvas so partial/optimised GIFs are
	// correct. Caller owns the returned SRVs. Returns empty on failure.
	std::vector<EmblemFrame> loadImageFrames(ID3D11Device* device, const wchar_t* path)
	{
		std::vector<EmblemFrame> frames;
		if (!device) return frames;
		HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // tolerate already-initialised COM
		IWICImagingFactory* factory = nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		{
			IWICBitmapDecoder* decoder = nullptr;
			if (SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
			{
				UINT frameCount = 0; decoder->GetFrameCount(&frameCount);
				if (frameCount > 256) frameCount = 256; // cap VRAM: each animated frame becomes a full-canvas texture

				if (frameCount <= 1)
				{
					// static image: decode frame 0 at its own size
					IWICBitmapFrameDecode* frame = nullptr;
					if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame)
					{
						IWICFormatConverter* conv = nullptr;
						if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
							SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
						{
							UINT w = 0, h = 0; conv->GetSize(&w, &h);
							if (w && h)
							{
								std::vector<uint8_t> px((size_t)w * h * 4);
								if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())))
									if (auto* srv = createSRVFromRGBA(device, w, h, px.data())) frames.push_back({ srv, 0 });
							}
							conv->Release();
						}
						frame->Release();
					}
				}
				else
				{
					// animated GIF: composite each frame onto a persistent canvas (disposal-aware)
					UINT canvasW = 0, canvasH = 0;
					IWICMetadataQueryReader* gmeta = nullptr;
					if (SUCCEEDED(decoder->GetMetadataQueryReader(&gmeta)) && gmeta)
					{
						canvasW = readMetaUint(gmeta, L"/logscrdesc/Width", 0);
						canvasH = readMetaUint(gmeta, L"/logscrdesc/Height", 0);
						gmeta->Release();
					}
					if (!canvasW || !canvasH) // fallback: first-frame size
					{
						IWICBitmapFrameDecode* f0 = nullptr;
						if (SUCCEEDED(decoder->GetFrame(0, &f0)) && f0) { f0->GetSize(&canvasW, &canvasH); f0->Release(); }
					}
					if (canvasW && canvasH && canvasW <= 4096 && canvasH <= 4096)
					{
						std::vector<uint8_t> canvas((size_t)canvasW * canvasH * 4, 0); // transparent start
						std::vector<uint8_t> prevCanvas;                               // for disposal=3 (restore-previous)
						for (UINT i = 0; i < frameCount; ++i)
						{
							IWICBitmapFrameDecode* fr = nullptr;
							if (FAILED(decoder->GetFrame(i, &fr)) || !fr) break;

							UINT fx = 0, fy = 0, delayCs = 0, disposal = 0;
							IWICMetadataQueryReader* fmeta = nullptr;
							if (SUCCEEDED(fr->GetMetadataQueryReader(&fmeta)) && fmeta)
							{
								fx = readMetaUint(fmeta, L"/imgdesc/Left", 0);
								fy = readMetaUint(fmeta, L"/imgdesc/Top", 0);
								delayCs = readMetaUint(fmeta, L"/grctlext/Delay", 0);
								disposal = readMetaUint(fmeta, L"/grctlext/Disposal", 0);
								fmeta->Release();
							}

							UINT fw = 0, fh = 0; fr->GetSize(&fw, &fh);
							std::vector<uint8_t> fpx;
							IWICFormatConverter* conv = nullptr;
							if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
								SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
							{
								fpx.resize((size_t)fw * fh * 4);
								if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)fpx.size(), fpx.data()))) fpx.clear();
								conv->Release();
							}

							if (disposal == 3) prevCanvas = canvas; // save state to restore after this frame

							// composite (alpha-test: overwrite where the frame pixel is not transparent)
							if (!fpx.empty())
								for (UINT y = 0; y < fh; ++y)
								{
									UINT cy = fy + y; if (cy >= canvasH) break;
									for (UINT x = 0; x < fw; ++x)
									{
										UINT cx = fx + x; if (cx >= canvasW) continue;
										const uint8_t* s = &fpx[((size_t)y * fw + x) * 4];
										if (s[3] == 0) continue;
										uint8_t* d = &canvas[((size_t)cy * canvasW + cx) * 4];
										d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
									}
								}

							if (auto* srv = createSRVFromRGBA(device, canvasW, canvasH, canvas.data()))
							{
								uint32_t ms = delayCs * 10; if (ms < 20) ms = 100; // clamp 0/tiny like browsers do
								frames.push_back({ srv, ms });
							}

							// prepare the canvas for the next frame per THIS frame's disposal method
							if (disposal == 2) // restore to background: clear this frame's rect to transparent
								for (UINT y = 0; y < fh; ++y)
								{
									UINT cy = fy + y; if (cy >= canvasH) break;
									for (UINT x = 0; x < fw; ++x) { UINT cx = fx + x; if (cx >= canvasW) continue;
										uint8_t* d = &canvas[((size_t)cy * canvasW + cx) * 4]; d[0] = d[1] = d[2] = d[3] = 0; }
								}
							else if (disposal == 3 && !prevCanvas.empty())
								canvas = prevCanvas;

							fr->Release();
						}
					}
				}
				decoder->Release();
			}
			factory->Release();
		}
		if (SUCCEEDED(co)) CoUninitialize();
		return frames;
	}

	// True for container extensions Media Foundation can decode video from (so we stream them instead of trying WIC).
	bool isVideoFile(const std::wstring& path)
	{
		auto ext = std::filesystem::path(path).extension().wstring();
		for (auto& c : ext) c = (wchar_t)towlower(c);
		return ext == L".mp4" || ext == L".m4v" || ext == L".mov" || ext == L".avi"
			|| ext == L".wmv" || ext == L".mkv" || ext == L".webm";
	}

	// Streams a video file's VIDEO track (no audio) as emblem frames. A background thread decodes with Media
	// Foundation, paced to real time and looping at EOF. When a D3D11 device manager is supplied it uses HARDWARE
	// (DXGI) decode + GPU colour-convert/resize and copies the decoded GPU frame straight into our own texture (no
	// CPU roundtrip = fast enough for 1080p); otherwise it falls back to software decode -> a system-memory frame
	// the render thread uploads. Output is downscaled to fit 1080p (ample for a small on-screen emblem).
	class EmblemVideoPlayer
	{
		std::thread mThread;
		std::atomic_bool mRunning{ false };
		// Set when we refuse a source rather than decode it at native size (see decodeLoop). Read and
		// cleared once by the render thread so the user gets exactly one message per rejected file.
		std::atomic_bool mRejected{ false };
		std::atomic<UINT> mRejW{ 0 }, mRejH{ 0 };
		std::wstring mPath;

		// D3D (AddRef'd in start, released in stop). mDevMgr non-null => try the hardware/GPU decode path.
		ID3D11Device* mDevice = nullptr;
		ID3D11DeviceContext* mContext = nullptr; // immediate context (caller must have made it multithread-protected)
		IMFDXGIDeviceManager* mDevMgr = nullptr;
		std::atomic_bool mGpu{ false };          // true once a hardware frame has been copied into mGpuTex

		// GPU-path output (owned; created on the decode thread, sampled on the render thread via mGpuSRV)
		ID3D11Texture2D* mGpuTex = nullptr;
		std::atomic<ID3D11ShaderResourceView*> mGpuSRV{ nullptr };
		UINT mGpuW = 0, mGpuH = 0; DXGI_FORMAT mGpuFmt = DXGI_FORMAT_UNKNOWN;

		// CPU-path output (system-memory BGRA frame handed to the render thread)
		std::mutex mMutex;
		std::vector<uint8_t> mLatest;
		UINT mW = 0, mH = 0; bool mHasNew = false;

		// Copy a hardware-decoded frame texture into our own (GPU->GPU). Decode thread; uses the (protected) context.
		// The decoder/processor SURFACE is often padded past the real frame (macroblock / power-of-2 alignment), so
		// we size our texture to the FRAME (frameW x frameH) and copy only that top-left region - otherwise the
		// padding shows as black bars on the emblem. frameW/H come from the output media type's MF_MT_FRAME_SIZE.
		void updateGpu(ID3D11Texture2D* srcTex, UINT sub, UINT apX, UINT apY, UINT apW, UINT apH)
		{
			if (!srcTex || !mDevice || !mContext) return;
			D3D11_TEXTURE2D_DESC sd{}; srcTex->GetDesc(&sd);
			if (!apW || apX + apW > sd.Width)  { apX = 0; apW = sd.Width; }   // clamp the aperture to the surface
			if (!apH || apY + apH > sd.Height) { apY = 0; apH = sd.Height; }
			UINT w = apW, h = apH;
			if (!mGpuTex || w != mGpuW || h != mGpuH || sd.Format != mGpuFmt)
			{
				if (auto* s = mGpuSRV.exchange(nullptr)) s->Release();
				if (mGpuTex) { mGpuTex->Release(); mGpuTex = nullptr; }
				D3D11_TEXTURE2D_DESC td{};
				td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
				td.Format = sd.Format; td.SampleDesc.Count = 1;   // match the source so CopySubresourceRegion is legal
				td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				if (FAILED(mDevice->CreateTexture2D(&td, nullptr, &mGpuTex)) || !mGpuTex) return;
				ID3D11ShaderResourceView* srv = nullptr;
				if (FAILED(mDevice->CreateShaderResourceView(mGpuTex, nullptr, &srv)) || !srv) { mGpuTex->Release(); mGpuTex = nullptr; return; }
				mGpuW = w; mGpuH = h; mGpuFmt = sd.Format;
				mGpuSRV.store(srv, std::memory_order_release);
			}
			D3D11_BOX box{ apX, apY, 0, apX + apW, apY + apH, 1 }; // copy only the visible aperture, not the padding
			mContext->CopySubresourceRegion(mGpuTex, 0, 0, 0, 0, srcTex, sub, &box);
		}

		void decodeLoop()
		{
			if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return;

			// Build the reader with the full video processor (convert + RESIZE) and, when we have one, the D3D
			// device manager for hardware decode. If creating it WITH the manager fails (no HW decode for this
			// codec/GPU), retry without -> software decode.
			auto makeReader = [&](bool useMgr) -> IMFSourceReader*
			{
				IMFAttributes* a = nullptr; IMFSourceReader* r = nullptr;
				if (SUCCEEDED(MFCreateAttributes(&a, 2)) && a)
				{
					a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
					if (useMgr && mDevMgr) a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mDevMgr);
				}
				MFCreateSourceReaderFromURL(mPath.c_str(), a, &r);
				if (a) a->Release();
				return r;
			};
			IMFSourceReader* reader = mDevMgr ? makeReader(true) : nullptr;
			if (!reader) reader = makeReader(false); // software fallback
			if (reader)
			{
				// only the first video stream, decoded to RGB32 (= BGRA byte order)
				reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
				reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

				// Downscale to fit within 1024 on the long edge, PRESERVING the video's aspect ratio - do NOT force a
				// square, or the video processor letterboxes (bakes black bars into the texture) which the emblem then
				// shows. The emblem stretches the frame to its own shape, exactly like it does for images/GIFs.
				UINT natW = 0, natH = 0;
				IMFMediaType* nat = nullptr;
				if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nat)) && nat)
				{ MFGetAttributeSize(nat, MF_MT_FRAME_SIZE, &natW, &natH); nat->Release(); }
				UINT outW = natW, outH = natH, longEdge = natW > natH ? natW : natH;
				if (longEdge > 1024 && longEdge)
				{ double sc = 1024.0 / longEdge; outW = (UINT)(natW * sc); outH = (UINT)(natH * sc); }
				outW &= ~1u; outH &= ~1u; if (outW < 2) outW = 2; if (outH < 2) outH = 2;
				auto setOutput = [&](UINT w, UINT h) -> bool
				{
					IMFMediaType* want = nullptr; bool ok = false;
					if (SUCCEEDED(MFCreateMediaType(&want)) && want)
					{
						want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
						want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
						if (w && h) MFSetAttributeSize(want, MF_MT_FRAME_SIZE, w, h);
						ok = SUCCEEDED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want));
						want->Release();
					}
					return ok;
				};
				// ⚠⚠ DO NOT fall back to native resolution here. A decoder that refuses the downscale would leave us
				// decoding at full source size - a 4K frame is ~33 MB and every frame becomes a full texture, which is
				// an unbounded allocation that can take the game down. Refuse the file and report it: a broadcast
				// machine failing loudly at setup beats crashing mid-tournament.
				if (!setOutput(outW, outH))
				{
					PLOG_ERROR << "Emblem video: decoder refused the " << outW << "x" << outH
						<< " downscale (source " << natW << "x" << natH << "); refusing rather than decoding at native size.";
					mRejW.store(natW, std::memory_order_relaxed);
					mRejH.store(natH, std::memory_order_relaxed);
					mRejected.store(true, std::memory_order_release);
					reader->Release();
					MFShutdown();
					return;
				}

				// fw/fh = frame size; apX/apY/apW/apH = the actual visible region (geometric aperture) within it.
				// Hardware decoders can report a padded coded FRAME_SIZE with the real picture inset in the aperture,
				// so we crop to the aperture to avoid black padding bars on the emblem.
				UINT fw = 0, fh = 0; LONG defStride = 0;
				UINT apX = 0, apY = 0, apW = 0, apH = 0;
				auto readFormat = [&]()
				{
					IMFMediaType* t = nullptr;
					if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t)) && t)
					{
						MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &fw, &fh);
						UINT32 s = 0; defStride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? (LONG)s : 0;
						apX = apY = 0; apW = fw; apH = fh; // default: whole frame
						MFVideoArea area{}; UINT32 got = 0;
						if ((SUCCEEDED(t->GetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8*)&area, sizeof(area), &got))
							|| SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), &got)))
							&& got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0)
						{
							apX = (UINT)(area.OffsetX.value > 0 ? area.OffsetX.value : 0);
							apY = (UINT)(area.OffsetY.value > 0 ? area.OffsetY.value : 0);
							apW = (UINT)area.Area.cx; apH = (UINT)area.Area.cy;
						}
						t->Release();
					}
				};
				readFormat();

				uint64_t startMs = 0; LONGLONG firstPts = 0; bool timed = false;
				while (mRunning.load(std::memory_order_acquire))
				{
					DWORD flags = 0; LONGLONG ts = 0; IMFSample* sample = nullptr;
					if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample)))
						break;
					if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readFormat();
					if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
					{
						PROPVARIANT pos; PropVariantInit(&pos); pos.vt = VT_I8; pos.hVal.QuadPart = 0;
						reader->SetCurrentPosition(GUID_NULL, pos); // loop
						PropVariantClear(&pos);
						timed = false;
						if (sample) sample->Release();
						continue;
					}
					if (sample)
					{
						IMFMediaBuffer* buf = nullptr;
						if (SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && buf)
						{
							IMFDXGIBuffer* dxgi = nullptr;
							if (mDevMgr && SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi)
							{
								// hardware frame: copy the decoded GPU texture straight into ours (no CPU roundtrip)
								ID3D11Texture2D* srcTex = nullptr; UINT sub = 0;
								if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&srcTex))) && srcTex)
								{
									dxgi->GetSubresourceIndex(&sub);
									updateGpu(srcTex, sub, apX, apY, apW, apH);
									mGpu.store(true, std::memory_order_release);
									srcTex->Release();
								}
								dxgi->Release();
							}
							else if (fw && fh)
							{
								// software frame: system-memory RGB32 -> a BGRA frame for the render thread to upload
								IMFMediaBuffer* contig = nullptr;
								if (SUCCEEDED(sample->ConvertToContiguousBuffer(&contig)) && contig)
								{
									BYTE* data = nullptr; DWORD maxLen = 0, curLen = 0;
									if (SUCCEEDED(contig->Lock(&data, &maxLen, &curLen)))
									{
										UINT absStride = defStride ? (UINT)(defStride < 0 ? -defStride : defStride) : fw * 4;
										bool bottomUp = defStride < 0;
										// crop to the visible aperture (offset apX/apY, size apW/apH), like the GPU path
										UINT ox = apX, oy = apY, cw = apW ? apW : fw, ch = apH ? apH : fh;
										if (ox + cw > fw) { ox = 0; cw = fw; }
										if (oy + ch > fh) { oy = 0; ch = fh; }
										UINT dstRowBytes = cw * 4;
										std::vector<uint8_t> frame((size_t)cw * ch * 4);
										for (UINT y = 0; y < ch; ++y)
										{
											UINT sy = bottomUp ? (fh - 1 - (oy + y)) : (oy + y);
											const uint8_t* srcRow = data + (size_t)sy * absStride + (size_t)ox * 4;
											uint8_t* dstRow = &frame[(size_t)y * dstRowBytes];
											memcpy(dstRow, srcRow, dstRowBytes);
											for (UINT x = 0; x < cw; ++x) dstRow[x * 4 + 3] = 255; // RGB32 X -> opaque
										}
										contig->Unlock();
										std::lock_guard<std::mutex> lk(mMutex);
										mLatest = std::move(frame); mW = cw; mH = ch; mHasNew = true;
									}
									contig->Release();
								}
							}
							buf->Release();
						}
					}
					if (sample) sample->Release();

					// pace to the sample's presentation time so playback runs at real speed
					if (!timed) { startMs = GetTickCount64(); firstPts = ts; timed = true; }
					else
					{
						LONGLONG targetMs = (LONGLONG)startMs + (ts - firstPts) / 10000; // 100ns -> ms
						LONGLONG nowMs = (LONGLONG)GetTickCount64();
						LONGLONG waitMs = targetMs - nowMs;
						if (waitMs > 0 && waitMs < 1000) Sleep((DWORD)waitMs);
					}
				}
				reader->Release();
			}
			MFShutdown();
		}

	public:
		// device/ctx/devMgr may be null (-> software decode + system-memory frames). devMgr non-null tries HW decode.
		bool start(const std::wstring& path, ID3D11Device* device, ID3D11DeviceContext* ctx, IMFDXGIDeviceManager* devMgr)
		{
			stop();
			mPath = path;
			mDevice = device; if (mDevice) mDevice->AddRef();
			mContext = ctx; if (mContext) mContext->AddRef();
			mDevMgr = devMgr; if (mDevMgr) mDevMgr->AddRef();
			mGpu.store(false, std::memory_order_release);
			mRunning.store(true, std::memory_order_release);
			mThread = std::thread([this]() { decodeLoop(); });
			return true;
		}

		// One-shot: true exactly once after a source was refused, so the caller can report it and move on.
		bool takeRejection(UINT& w, UINT& h)
		{
			if (!mRejected.exchange(false, std::memory_order_acq_rel)) return false;
			w = mRejW.load(std::memory_order_relaxed);
			h = mRejH.load(std::memory_order_relaxed);
			return true;
		}
		bool isGpu() const { return mGpu.load(std::memory_order_acquire); }
		ID3D11ShaderResourceView* gpuSRV() const { return mGpuSRV.load(std::memory_order_acquire); }

		void stop()
		{
			mRunning.store(false, std::memory_order_release);
			if (mThread.joinable()) mThread.join(); // decode thread gone -> nothing else touches the members below
			if (auto* s = mGpuSRV.exchange(nullptr)) s->Release();
			if (mGpuTex) { mGpuTex->Release(); mGpuTex = nullptr; }
			mGpuW = mGpuH = 0; mGpuFmt = DXGI_FORMAT_UNKNOWN;
			if (mDevMgr) { mDevMgr->Release(); mDevMgr = nullptr; }
			if (mContext) { mContext->Release(); mContext = nullptr; }
			if (mDevice) { mDevice->Release(); mDevice = nullptr; }
			mGpu.store(false, std::memory_order_release);
			std::lock_guard<std::mutex> lk(mMutex);
			mLatest.clear(); mW = mH = 0; mHasNew = false;
		}

		bool active() const { return mRunning.load(std::memory_order_acquire); }

		// render thread: copy out the newest decoded frame (BGRA) if one arrived since the last fetch.
		bool fetch(std::vector<uint8_t>& out, UINT& w, UINT& h)
		{
			std::lock_guard<std::mutex> lk(mMutex);
			if (!mHasNew || mLatest.empty()) return false;
			out = mLatest; w = mW; h = mH; mHasNew = false;
			return true;
		}

		~EmblemVideoPlayer() { stop(); }
	};
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
	static inline std::atomic<ID3D11ShaderResourceView*> sTestSRV{ nullptr }; // CURRENT emblem frame (non-owning; owned by mEmblemFrames)
	// animated-emblem playback (render-thread owned). For a static image this holds 1 frame; for a GIF, all frames.
	std::vector<EmblemFrame> mEmblemFrames;
	size_t   mEmblemCurFrame = 0;
	uint64_t mEmblemFrameStartTick = 0;

	// video-emblem playback (render-thread owned): a background decoder + (software path) a reusable dynamic texture
	EmblemVideoPlayer mVideoPlayer;
	ID3D11Texture2D* mVideoTex = nullptr;
	ID3D11ShaderResourceView* mVideoSRV = nullptr;
	UINT mVideoTexW = 0, mVideoTexH = 0;
	std::vector<uint8_t> mVideoFrameBuf; // reused scratch for the software fetch() path

	// D3D11 device manager for HARDWARE video decode (created once from the game's device; null = software decode).
	IMFDXGIDeviceManager* mDevMgr = nullptr;
	UINT mDevMgrToken = 0;
	bool mDevMgrTried = false;

	// Create the MF device manager once, after making the game's immediate context multithread-protected (so MF's
	// decode thread and the render thread can safely share it). Returns null (-> software decode) if unavailable.
	IMFDXGIDeviceManager* ensureDeviceManager(ID3D11Device* device, ID3D11DeviceContext* ctx)
	{
		if (mDevMgr) return mDevMgr;
		if (mDevMgrTried || !device || !ctx) return mDevMgr;
		mDevMgrTried = true;
		ID3D11Multithread* mt = nullptr;
		if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&mt))) || !mt) return nullptr; // no protection -> don't share the device
		mt->SetMultithreadProtected(TRUE);
		mt->Release();
		IMFDXGIDeviceManager* mgr = nullptr; UINT token = 0;
		if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr)) && mgr)
		{
			if (SUCCEEDED(mgr->ResetDevice(device, token))) { mDevMgr = mgr; mDevMgrToken = token; }
			else mgr->Release();
		}
		return mDevMgr;
	}

	void releaseVideoTex()
	{
		if (mVideoSRV) { mVideoSRV->Release(); mVideoSRV = nullptr; }
		if (mVideoTex) { mVideoTex->Release(); mVideoTex = nullptr; }
		mVideoTexW = mVideoTexH = 0;
	}

	// Upload a BGRA video frame into the reusable dynamic texture (recreating it if the size changed), leaving the
	// current frame in mVideoSRV. Render thread only (needs the immediate context to Map).
	void uploadVideoFrame(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h, const uint8_t* bgra)
	{
		if (!device || !ctx || !w || !h || !bgra) return;
		if (!mVideoTex || w != mVideoTexW || h != mVideoTexH)
		{
			releaseVideoTex();
			D3D11_TEXTURE2D_DESC td{};
			td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &mVideoTex)) || !mVideoTex) return;
			device->CreateShaderResourceView(mVideoTex, nullptr, &mVideoSRV);
			mVideoTexW = w; mVideoTexH = h;
		}
		if (!mVideoSRV) return;
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(mVideoTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		{
			for (UINT y = 0; y < h; ++y)
				memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, bgra + (size_t)y * w * 4, (size_t)w * 4);
			ctx->Unmap(mVideoTex, 0);
		}
	}
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
		if (srv && base)
		{
			uint32_t fgTag = *(uint32_t*)(base + kFgEmblemTagRVA);
			uint64_t mine = sMyEmblemObj.load(std::memory_order_acquire);
			// scope to the local player's emblem; if we couldn't resolve it (mine==0), fall back to all bipeds
			bool isMine = (mine == 0) || (drawingEmblemObj == mine);
			// This is the player's emblem being drawn on the fg-emblem tag when EITHER:
			//   - sprite != 0  (MP: the player picked a non-default emblem; the game applies its own override), OR
			//   - sprite == 0 AND the per-player override object is set (campaign: MC's emblem_info fg index is 0,
			//     so the DEFAULT sprite-0 is drawn, but the override object is still present -> it IS a player emblem).
			// This also fixes MP for players whose chosen emblem is index 0. A stray sprite-0 bind with no override
			// (drawingEmblemObj==0) on the fg tag is skipped, so non-emblem sprite-0 binds are unaffected.
			bool isEmblemDraw = (tagIdx == fgTag) && (sprite != 0 || drawingEmblemObj != 0);
			if (isEmblemDraw && isMine)
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

	// --- Video -> filled square: the emblem shows a non-square texture letterboxed (black bars); videos are wide,
	// so we stretch each decoded frame into a 1024x1024 square render target that fills the emblem like a square image. ---
	static inline ID3D11VertexShader* sBlitVS = nullptr;
	static inline ID3D11PixelShader*  sBlitPS = nullptr;
	static inline ID3D11SamplerState* sBlitSampler = nullptr;
	ID3D11Texture2D* mSquareTex = nullptr;            // 1024x1024 RGBA render target (render-thread owned)
	ID3D11RenderTargetView* mSquareRTV = nullptr;
	ID3D11ShaderResourceView* mSquareSRV = nullptr;

	void releaseSquare()
	{
		if (mSquareSRV) { mSquareSRV->Release(); mSquareSRV = nullptr; }
		if (mSquareRTV) { mSquareRTV->Release(); mSquareRTV = nullptr; }
		if (mSquareTex) { mSquareTex->Release(); mSquareTex = nullptr; }
	}

	// Stretch srcSRV to fill a 1024x1024 square RT (state saved/restored). Returns the square SRV to bind as the emblem.
	ID3D11ShaderResourceView* blitToSquare(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSRV)
	{
		if (!device || !ctx || !srcSRV) return nullptr;
		if (!sBlitVS || !sBlitPS || !sBlitSampler)
		{
			// fullscreen-triangle VS + sample-and-force-opaque PS
			static const char* vs =
				"struct VO{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};\n"
				"VO main(uint id:SV_VertexID){VO o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}\n";
			static const char* ps =
				"Texture2D t:register(t0);SamplerState s:register(s0);\n"
				"float4 main(float4 p:SV_POSITION,float2 uv:TEXCOORD0):SV_Target{return float4(t.Sample(s,uv).rgb,1.0);}\n";
			ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
			if (SUCCEEDED(D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &b, &e)) && b)
			{ device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &sBlitVS); b->Release(); }
			if (e) { e->Release(); e = nullptr; } b = nullptr;
			if (SUCCEEDED(D3DCompile(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e)) && b)
			{ device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &sBlitPS); b->Release(); }
			if (e) e->Release();
			D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			device->CreateSamplerState(&sd, &sBlitSampler);
		}
		if (!sBlitVS || !sBlitPS || !sBlitSampler) return nullptr;
		if (!mSquareTex)
		{
			D3D11_TEXTURE2D_DESC td{};
			td.Width = 1024; td.Height = 1024; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &mSquareTex)) || !mSquareTex) return nullptr;
			device->CreateRenderTargetView(mSquareTex, nullptr, &mSquareRTV);
			device->CreateShaderResourceView(mSquareTex, nullptr, &mSquareSRV);
			if (!mSquareRTV || !mSquareSRV) { releaseSquare(); return nullptr; }
		}

		// --- save the pipeline state we touch ---
		ID3D11RenderTargetView* oldRTV[8]{}; ID3D11DepthStencilView* oldDSV = nullptr;
		ctx->OMGetRenderTargets(8, oldRTV, &oldDSV);
		UINT nVP = 1; D3D11_VIEWPORT oldVP[1]{}; ctx->RSGetViewports(&nVP, oldVP);
		ID3D11VertexShader* oVS = nullptr; ID3D11PixelShader* oPS = nullptr;
		ctx->VSGetShader(&oVS, nullptr, nullptr); ctx->PSGetShader(&oPS, nullptr, nullptr);
		ID3D11InputLayout* oIL = nullptr; ctx->IAGetInputLayout(&oIL);
		D3D11_PRIMITIVE_TOPOLOGY oTopo; ctx->IAGetPrimitiveTopology(&oTopo);
		ID3D11ShaderResourceView* oSRV = nullptr; ctx->PSGetShaderResources(0, 1, &oSRV);
		ID3D11SamplerState* oSamp = nullptr; ctx->PSGetSamplers(0, 1, &oSamp);
		ID3D11BlendState* oBlend = nullptr; FLOAT oBF[4]; UINT oMask = 0; ctx->OMGetBlendState(&oBlend, oBF, &oMask);
		ID3D11DepthStencilState* oDSS = nullptr; UINT oStencil = 0; ctx->OMGetDepthStencilState(&oDSS, &oStencil);
		ID3D11RasterizerState* oRS = nullptr; ctx->RSGetState(&oRS);

		// --- draw the stretch ---
		ctx->OMSetRenderTargets(1, &mSquareRTV, nullptr);
		D3D11_VIEWPORT vp{ 0, 0, 1024, 1024, 0, 1 }; ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ID3D11Buffer* noVB = nullptr; UINT z = 0; ctx->IASetVertexBuffers(0, 1, &noVB, &z, &z);
		ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(sBlitVS, nullptr, 0); ctx->PSSetShader(sBlitPS, nullptr, 0);
		ctx->PSSetShaderResources(0, 1, &srcSRV); ctx->PSSetSamplers(0, 1, &sBlitSampler);
		ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(nullptr, 0); ctx->RSSetState(nullptr);
		ctx->Draw(3, 0);

		// --- restore ---
		ctx->OMSetRenderTargets(8, oldRTV, oldDSV);
		for (auto* r : oldRTV) if (r) r->Release();
		if (oldDSV) oldDSV->Release();
		ctx->RSSetViewports(nVP, oldVP);
		ctx->VSSetShader(oVS, nullptr, 0); if (oVS) oVS->Release();
		ctx->PSSetShader(oPS, nullptr, 0); if (oPS) oPS->Release();
		ctx->IASetInputLayout(oIL); if (oIL) oIL->Release();
		ctx->IASetPrimitiveTopology(oTopo);
		ctx->PSSetShaderResources(0, 1, &oSRV); if (oSRV) oSRV->Release();
		ctx->PSSetSamplers(0, 1, &oSamp); if (oSamp) oSamp->Release();
		ctx->OMSetBlendState(oBlend, oBF, oMask); if (oBlend) oBlend->Release();
		ctx->OMSetDepthStencilState(oDSS, oStencil); if (oDSS) oDSS->Release();
		ctx->RSSetState(oRS); if (oRS) oRS->Release();
		return mSquareSRV;
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

		// (re)load when the user picks a new file
		if (device && mEmblemReload.exchange(false, std::memory_order_acq_rel))
		{
			std::wstring path;
			{ std::lock_guard<std::mutex> lk(mEmblemPathMutex); path = mEmblemPngPath; }
			if (!path.empty())
			{
				if (isVideoFile(path))
				{
					// stream the video track: drop any image/GIF frames, start the background decoder
					sTestSRV.store(nullptr, std::memory_order_release);
					for (auto& f : mEmblemFrames) if (f.srv) f.srv->Release();
					mEmblemFrames.clear();
					releaseVideoTex(); // free any software-path texture from a prior video
						IMFDXGIDeviceManager* mgr = ensureDeviceManager(device, ctx);
						mVideoPlayer.start(path, device, ctx, mgr);
					if (auto m = messagesWeak.lock()) m->addMessage(mgr ? "Custom emblem: playing video (GPU decode, looping)..." : "Custom emblem: playing video (looping)...");
				}
				else
				{
					mVideoPlayer.stop(); releaseVideoTex(); // leave any prior video mode
					auto newFrames = loadImageFrames(device, path.c_str());
					if (!newFrames.empty())
					{
						sTestSRV.store(nullptr, std::memory_order_release); // stop the detour reading a frame we're about to free
						for (auto& f : mEmblemFrames) if (f.srv) f.srv->Release();
						mEmblemFrames = std::move(newFrames);
						mEmblemCurFrame = 0;
						mEmblemFrameStartTick = GetTickCount64();
						sTestSRV.store(mEmblemFrames[0].srv, std::memory_order_release);
						if (auto m = messagesWeak.lock())
							m->addMessage(mEmblemFrames.size() > 1
								? ("Custom emblem loaded (" + std::to_string(mEmblemFrames.size()) + " frames, animated).")
								: std::string("Custom emblem loaded."));
					}
					else if (auto m = messagesWeak.lock()) m->addMessage("Custom emblem: couldn't load that image.");
				}
			}
		}

		if (mVideoPlayer.active())
		{
			// get the latest decoded frame's SRV (hardware = decode thread's texture; software = our upload)
			ID3D11ShaderResourceView* videoSRV = nullptr;
			if (mVideoPlayer.isGpu())
				videoSRV = mVideoPlayer.gpuSRV();
			else
			{
				UINT rw = 0, rh = 0;
				if (mVideoPlayer.takeRejection(rw, rh))
					if (auto m = messagesWeak.lock())
						m->addMessage("Custom emblem: that video is too large (" + std::to_string(rw) + "x"
							+ std::to_string(rh) + ") - the decoder would not downscale it. Try a smaller file.");

				UINT vw = 0, vh = 0;
				if (device && ctx && mVideoPlayer.fetch(mVideoFrameBuf, vw, vh))
					uploadVideoFrame(device, ctx, vw, vh, mVideoFrameBuf.data());
				videoSRV = mVideoSRV;
			}
			// stretch it into a filled 1024x1024 square so the emblem fills (a wide/non-square frame otherwise shows
			// letterbox black on the emblem), then point the emblem at the square.
			if (videoSRV && device && ctx)
				if (auto* sq = blitToSquare(device, ctx, videoSRV)) sTestSRV.store(sq, std::memory_order_release);
		}
		else if (mEmblemFrames.size() > 1)
		{
			// GIF: show the current frame for its delay, then step to the next (looping)
			uint64_t now = GetTickCount64();
			if (now - mEmblemFrameStartTick >= mEmblemFrames[mEmblemCurFrame].delayMs)
			{
				mEmblemCurFrame = (mEmblemCurFrame + 1) % mEmblemFrames.size();
				mEmblemFrameStartTick = now;
				sTestSRV.store(mEmblemFrames[mEmblemCurFrame].srv, std::memory_order_release);
			}
		}
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
		ofn.lpstrFilter = L"Image / Video (*.png;*.gif;*.jpg;*.bmp;*.dds;*.mp4;*.mov;*.mkv;*.avi;*.webm)\0*.png;*.gif;*.jpg;*.jpeg;*.bmp;*.dds;*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm\0All Files (*.*)\0*.*\0";
		ofn.lpstrFile = fileBuf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = L"Choose a custom emblem (PNG, animated GIF, or video)";
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
		mRenderCallback.reset(); // stop onRenderEvent first, so the render thread can't touch mEmblemFrames while we free it
		mGameTickEventCallback.reset();
		if (mEmblemHook) mEmblemHook->setWantsToBeAttached(false);
		sEmblemArmed.store(false);
		sDrawHook = {};   // unhook DrawIndexed
		sCtxVmt = {};
		sEmblemHook = nullptr;
		sTestSRV.store(nullptr); // non-owning; the frames / video texture own the SRVs
		mVideoPlayer.stop();     // join the decode thread before we free anything it feeds (also releases its devMgr ref)
		releaseVideoTex();
		releaseSquare();
		if (sBlitVS) { sBlitVS->Release(); sBlitVS = nullptr; }
		if (sBlitPS) { sBlitPS->Release(); sBlitPS = nullptr; }
		if (sBlitSampler) { sBlitSampler->Release(); sBlitSampler = nullptr; }
		if (mDevMgr) { mDevMgr->Release(); mDevMgr = nullptr; }
		for (auto& f : mEmblemFrames) if (f.srv) f.srv->Release();
		mEmblemFrames.clear();
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
