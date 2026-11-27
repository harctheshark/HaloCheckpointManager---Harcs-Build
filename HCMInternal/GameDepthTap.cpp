#include "pch.h"
#include "GameDepthTap.h"
#include <wrl/client.h>
#include <mutex>

// See GameDepthTap.h.
using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<bool> gWanted{ false };
	std::atomic<bool> gWantedTAA{ false };   // 2026-11-23: Halo 2 TAA (H2TAA.cpp)
	std::atomic<uint64_t> gCaptures{ 0 };
	std::atomic<uint64_t> gFrame{ 1 };
	std::mutex gM;
	ComPtr<ID3D11Texture2D> gCopy;
	ComPtr<ID3D11ShaderResourceView> gSrv;
	ID3D11Device* gDevice = nullptr;   // not owned: the device gCopy was made on
	UINT gW = 0, gH = 0; DXGI_FORMAT gFmt = DXGI_FORMAT_UNKNOWN;
	uint64_t gTakenFrame = 0;
	D3D11_VIEWPORT gVp{};
	bool gReversed = true;
	bool gLoggedFormat = false;

	// depth formats -> (typeless copy, readable view)
	bool depthFormats(DXGI_FORMAT f, DXGI_FORMAT& typeless, DXGI_FORMAT& view)
	{
		switch (f)
		{
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
			typeless = DXGI_FORMAT_R32G8X24_TYPELESS; view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
		case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
			typeless = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; return true;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
			typeless = DXGI_FORMAT_R24G8_TYPELESS; view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
		case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
			typeless = DXGI_FORMAT_R16_TYPELESS; view = DXGI_FORMAT_R16_UNORM; return true;
		default: return false;
		}
	}
}

namespace GameDepthTap
{
	void setWanted(bool wanted) { gWanted.store(wanted, std::memory_order_relaxed); }
	void setWantedByTAA(bool wanted) { gWantedTAA.store(wanted, std::memory_order_relaxed); }
	bool wanted() { return gWanted.load(std::memory_order_relaxed) || gWantedTAA.load(std::memory_order_relaxed); }
	uint64_t captureCount() { return gCaptures.load(std::memory_order_relaxed); }
	uint64_t frame() { return gFrame.load(std::memory_order_relaxed); }
	void endFrame() { gFrame.fetch_add(1, std::memory_order_relaxed); }

	void capture(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, const D3D11_VIEWPORT& vp, bool reversedZ)
	{
		if (!ctx || !dsv || !wanted()) return;
		std::unique_lock lk(gM, std::try_to_lock);
		if (!lk.owns_lock()) return;
		ComPtr<ID3D11Resource> res;
		dsv->GetResource(&res);
		ComPtr<ID3D11Texture2D> tex;
		if (!res || FAILED(res.As(&tex)) || !tex) return;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		if (d.SampleDesc.Count != 1 || d.ArraySize != 1) return;   // MSAA / array depth: not handled
		DXGI_FORMAT typeless, view;
		if (!depthFormats(d.Format, typeless, view))
		{
			if (!gLoggedFormat) { gLoggedFormat = true; PLOG_ERROR << "GameDepthTap: depth format " << (int)d.Format << " not handled - SMAA runs without depth"; }
			return;
		}
		ComPtr<ID3D11Device> dev;
		tex->GetDevice(&dev);
		if (!gCopy || gDevice != dev.Get() || gW != d.Width || gH != d.Height || gFmt != d.Format)
		{
			gCopy.Reset(); gSrv.Reset(); gDevice = nullptr;
			D3D11_TEXTURE2D_DESC cd{};
			cd.Width = d.Width; cd.Height = d.Height; cd.MipLevels = 1; cd.ArraySize = 1; cd.Format = typeless; cd.SampleDesc.Count = 1;
			cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(dev->CreateTexture2D(&cd, nullptr, &gCopy))) { gCopy.Reset(); return; }
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = view; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
			if (FAILED(dev->CreateShaderResourceView(gCopy.Get(), &sv, &gSrv))) { gCopy.Reset(); gSrv.Reset(); return; }
			gDevice = dev.Get(); gW = d.Width; gH = d.Height; gFmt = d.Format;
			PLOG_INFO << "GameDepthTap: copying " << d.Width << "x" << d.Height << " depth (format " << (int)d.Format << ")";
		}
		ctx->CopyResource(gCopy.Get(), tex.Get());
		gVp = vp; gReversed = reversedZ;
		gTakenFrame = frame();
		gCaptures.fetch_add(1, std::memory_order_relaxed);
	}

	bool current(ID3D11Device* dev, View& out)
	{
		std::unique_lock lk(gM, std::try_to_lock);
		if (!lk.owns_lock()) return false;
		if (!gSrv || gDevice != dev || gTakenFrame != frame() || !gW || !gH) return false;
		out.srv = gSrv.Get();
		const float vw = gVp.Width > 0.f ? gVp.Width : (float)gW, vh = gVp.Height > 0.f ? gVp.Height : (float)gH;
		out.uvScale[0] = vw / (float)gW; out.uvScale[1] = vh / (float)gH;
		out.uvOffset[0] = gVp.TopLeftX / (float)gW; out.uvOffset[1] = gVp.TopLeftY / (float)gH;
		out.reversedZ = gReversed;
		return true;
	}

	void release()
	{
		std::scoped_lock lk(gM);
		gCopy.Reset(); gSrv.Reset(); gDevice = nullptr; gW = gH = 0; gFmt = DXGI_FORMAT_UNKNOWN; gTakenFrame = 0;
	}
}
