#include "pch.h"
#include "SMAAPostProcess.h"
#include "SettingsStateAndEvents.h"
#include <wrl/client.h>
#include <mutex>

// SMAA's lookup tables (MIT, SMAA\SMAA_LICENSE.txt): areaTexBytes 160x560 R8G8, searchTexBytes 64x16 R8
#include "SMAA\AreaTex.h"
#include "SMAA\SearchTex.h"
#include "SMAA\Generated\SMAAShaders.h"
// Intel's CMAA2 (Apache-2.0, CMAA2\CMAA2_LICENSE.txt), compute shaders precompiled by CMAA2\build_shaders.py
#include "CMAA2\Generated\CMAA2Shaders.h"
#include "GameDepthTap.h"

using Microsoft::WRL::ComPtr;

namespace
{
	// Everything a pass changes, saved before and put back after (as imgui_impl_dx11 does for its own draw).
	struct D3D11StateBackup
	{
		static constexpr UINT kSrvs = 7, kSamplers = 2;
		UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11RasterizerState* rs = nullptr;
		ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4] = {}; UINT sampleMask = 0;
		ID3D11DepthStencilState* dss = nullptr; UINT stencilRef = 0;
		ID3D11ShaderResourceView* psSrvs[kSrvs] = {};
		ID3D11SamplerState* psSamplers[kSamplers] = {};
		ID3D11Buffer* psCb0 = nullptr; ID3D11Buffer* vsCb0 = nullptr;
		ID3D11PixelShader* ps = nullptr; ID3D11VertexShader* vs = nullptr; ID3D11GeometryShader* gs = nullptr;
		ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
		ID3D11ClassInstance* psInst[256] = {}; ID3D11ClassInstance* vsInst[256] = {}; ID3D11ClassInstance* gsInst[256] = {};
		ID3D11ClassInstance* hsInst[256] = {}; ID3D11ClassInstance* dsInst[256] = {};
		UINT psInstCount = 256, vsInstCount = 256, gsInstCount = 256, hsInstCount = 256, dsInstCount = 256;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN; UINT ibOffset = 0;
		ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOffset = 0;
		ID3D11InputLayout* layout = nullptr;
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* dsv = nullptr;

		void save(ID3D11DeviceContext* c)
		{
			c->RSGetScissorRects(&scissorCount, scissors);
			c->RSGetViewports(&viewportCount, viewports);
			c->RSGetState(&rs);
			c->OMGetBlendState(&blend, blendFactor, &sampleMask);
			c->OMGetDepthStencilState(&dss, &stencilRef);
			c->PSGetShaderResources(0, kSrvs, psSrvs);
			c->PSGetSamplers(0, kSamplers, psSamplers);
			c->PSGetConstantBuffers(0, 1, &psCb0);
			c->VSGetConstantBuffers(0, 1, &vsCb0);
			c->PSGetShader(&ps, psInst, &psInstCount);
			c->VSGetShader(&vs, vsInst, &vsInstCount);
			c->GSGetShader(&gs, gsInst, &gsInstCount);
			c->HSGetShader(&hs, hsInst, &hsInstCount);
			c->DSGetShader(&ds, dsInst, &dsInstCount);
			c->IAGetPrimitiveTopology(&topology);
			c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
			c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
			c->IAGetInputLayout(&layout);
			c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		}

		template <typename T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

		void restore(ID3D11DeviceContext* c)
		{
			c->RSSetScissorRects(scissorCount, scissors);
			c->RSSetViewports(viewportCount, viewports);
			c->RSSetState(rs); rel(rs);
			c->OMSetBlendState(blend, blendFactor, sampleMask); rel(blend);
			c->OMSetDepthStencilState(dss, stencilRef); rel(dss);
			c->PSSetShaderResources(0, kSrvs, psSrvs); for (auto& s : psSrvs) rel(s);
			c->PSSetSamplers(0, kSamplers, psSamplers); for (auto& s : psSamplers) rel(s);
			c->PSSetConstantBuffers(0, 1, &psCb0); rel(psCb0);
			c->VSSetConstantBuffers(0, 1, &vsCb0); rel(vsCb0);
			c->PSSetShader(ps, psInst, psInstCount); rel(ps); for (UINT i = 0; i < psInstCount; ++i) rel(psInst[i]);
			c->VSSetShader(vs, vsInst, vsInstCount); rel(vs); for (UINT i = 0; i < vsInstCount; ++i) rel(vsInst[i]);
			c->GSSetShader(gs, gsInst, gsInstCount); rel(gs); for (UINT i = 0; i < gsInstCount; ++i) rel(gsInst[i]);
			c->HSSetShader(hs, hsInst, hsInstCount); rel(hs); for (UINT i = 0; i < hsInstCount; ++i) rel(hsInst[i]);
			c->DSSetShader(ds, dsInst, dsInstCount); rel(ds); for (UINT i = 0; i < dsInstCount; ++i) rel(dsInst[i]);
			c->IASetPrimitiveTopology(topology);
			c->IASetIndexBuffer(ib, ibFormat, ibOffset); rel(ib);
			c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset); rel(vb);
			c->IASetInputLayout(layout); rel(layout);
			c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
			for (auto& r : rtvs) rel(r);
			rel(dsv);
		}
	};
}

struct SMAAPostProcess::Impl
{
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>> presentEventWeak;
	eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>::Handle presentHandle;

	std::mutex mtx;            // held while a frame is processed; the destructor takes it after unsubscribing
	bool dead = false;         // under mtx: the destructor has run
	bool failed = false;       // a device resource could not be made: SMAA stays off for the session (logged once)

	// device resources (made on first use)
	ID3D11Device* device = nullptr;   // not owned: the one they were made on
	ComPtr<ID3D11VertexShader> edgeVS, blendVS, neighborVS;
	ComPtr<ID3D11PixelShader> lumaEdgePS, colorEdgePS, blendPS, blendNoDiagPS, neighborPS, edgeDebugPS;
	ComPtr<ID3D11PixelShader> depthLogPS, lumaEdgePredPS, colorEdgePredPS;   // depth predication (GameDepthTap)
	bool loggedDepth = false;
	ComPtr<ID3D11SamplerState> linearSampler, pointSampler;
	ComPtr<ID3D11ShaderResourceView> areaSRV, searchSRV;
	ComPtr<ID3D11Buffer> metricsCB;
	ComPtr<ID3D11RasterizerState> rasterizer;
	ComPtr<ID3D11DepthStencilState> noDepth;
	ComPtr<ID3D11BlendState> noBlend;
	// size-dependent
	UINT width = 0, height = 0; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	ComPtr<ID3D11Texture2D> colorCopy; ComPtr<ID3D11ShaderResourceView> colorSRV;
	ComPtr<ID3D11ShaderResourceView> colorLinearSRV;   // the copy read as sRGB (linear light) for the blend; = colorSRV when the format has no sRGB twin
	bool haveLinearView = false;
	// the constant buffer as last written (rewritten only when a value changes)
	struct Constants { float metrics[4]; float params[4]; float params2[4]; float pred[4]; float depthUv[4]; } lastConstants = {};
	ComPtr<ID3D11Texture2D> predTex; ComPtr<ID3D11RenderTargetView> predRTV; ComPtr<ID3D11ShaderResourceView> predSRV;   // log depth, back-buffer size
	ComPtr<ID3D11Texture2D> edgesTex; ComPtr<ID3D11RenderTargetView> edgesRTV; ComPtr<ID3D11ShaderResourceView> edgesSRV;
	ComPtr<ID3D11Texture2D> blendTex; ComPtr<ID3D11RenderTargetView> blendRTV; ComPtr<ID3D11ShaderResourceView> blendSRV;

	// ---- CMAA2 (2026-11-23, user: "Lets try CMAA2"): Intel's Conservative Morphological AA 2.3, the DX11 integration of its
	// sample (vaCMAA2DX11.cpp) on a copy of the frame: the copy read through an sRGB view (linear light), the anti-aliased
	// pixels written back with a typed UNORM store (encoded to sRGB in the shader), then copied onto the back buffer. Buffer
	// sizes as the sample's "99.99% safe" ones.
	ComPtr<ID3D11ComputeShader> cmaaEdges[4], cmaaProcess[4], cmaaApply[4], cmaaDispatchArgs, cmaaDebugEdges;
	bool cmaaFailed = false;   // could not run on this device / format: SMAA instead (logged once)
	UINT cmaaW = 0, cmaaH = 0; DXGI_FORMAT cmaaFormat = DXGI_FORMAT_UNKNOWN;
	ComPtr<ID3D11Texture2D> cmaaWork; ComPtr<ID3D11ShaderResourceView> cmaaWorkSRV; ComPtr<ID3D11UnorderedAccessView> cmaaWorkUAV;
	ComPtr<ID3D11Texture2D> cmaaEdgesTex, cmaaHeadsTex; ComPtr<ID3D11UnorderedAccessView> cmaaEdgesUAV, cmaaHeadsUAV;
	ComPtr<ID3D11Buffer> cmaaCandidates, cmaaItems, cmaaLocations, cmaaControl, cmaaIndirect;
	ComPtr<ID3D11UnorderedAccessView> cmaaCandidatesUAV, cmaaItemsUAV, cmaaLocationsUAV, cmaaControlUAV, cmaaIndirectUAV;

	void releaseCmaaSized()
	{
		cmaaWork.Reset(); cmaaWorkSRV.Reset(); cmaaWorkUAV.Reset(); cmaaEdgesTex.Reset(); cmaaHeadsTex.Reset(); cmaaEdgesUAV.Reset(); cmaaHeadsUAV.Reset();
		cmaaCandidates.Reset(); cmaaItems.Reset(); cmaaLocations.Reset(); cmaaControl.Reset(); cmaaIndirect.Reset();
		cmaaCandidatesUAV.Reset(); cmaaItemsUAV.Reset(); cmaaLocationsUAV.Reset(); cmaaControlUAV.Reset(); cmaaIndirectUAV.Reset();
		cmaaW = cmaaH = 0; cmaaFormat = DXGI_FORMAT_UNKNOWN;
	}
	void releaseCmaa()
	{
		for (int q = 0; q < 4; ++q) { cmaaEdges[q].Reset(); cmaaProcess[q].Reset(); cmaaApply[q].Reset(); }
		cmaaDispatchArgs.Reset(); cmaaDebugEdges.Reset();
		releaseCmaaSized();
	}

	void makeCmaa(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& back)
	{
		if (!cmaaDispatchArgs)
		{
			auto cs = [dev](const CMAA2ShaderBlob& b, ComPtr<ID3D11ComputeShader>& out, const char* what) { check(dev->CreateComputeShader(b.data, b.size, nullptr, &out), what); };
			for (int q = 0; q < 4; ++q) { cs(CMAA2Edges[q], cmaaEdges[q], "CMAA2 edges CS"); cs(CMAA2Process[q], cmaaProcess[q], "CMAA2 candidates CS"); cs(CMAA2Apply[q], cmaaApply[q], "CMAA2 apply CS"); }
			cs(CMAA2DispatchArgs, cmaaDispatchArgs, "CMAA2 dispatch args CS");
			cs(CMAA2DebugEdges, cmaaDebugEdges, "CMAA2 debug CS");
		}
		releaseCmaaSized();
		DXGI_FORMAT typeless, plain, srgb;
		if (!srgbTwin(back.Format, typeless, plain, srgb))
			throw HCMRuntimeException(std::format("CMAA2: the frame's format {} is not 8-bit RGBA / BGRA", (int)back.Format));
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2 = { plain };
		if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2))) || !(fs2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE))
			throw HCMRuntimeException(std::format("CMAA2: this GPU cannot write format {} from a compute shader", (int)plain));
		const UINT w = back.Width, h = back.Height;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		td.Format = typeless; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		check(dev->CreateTexture2D(&td, nullptr, &cmaaWork), "CMAA2 frame copy");
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Format = srgb;   // linear light in
		check(dev->CreateShaderResourceView(cmaaWork.Get(), &sv, &cmaaWorkSRV), "CMAA2 frame view");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uv.Format = plain;                                 // sRGB encoded out (in the shader)
		check(dev->CreateUnorderedAccessView(cmaaWork.Get(), &uv, &cmaaWorkUAV), "CMAA2 frame output");
		td.Format = DXGI_FORMAT_R8_UINT; td.Width = (w + 1) / 2; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;   // two 4-bit edge sets per texel
		check(dev->CreateTexture2D(&td, nullptr, &cmaaEdgesTex), "CMAA2 edges");
		check(dev->CreateUnorderedAccessView(cmaaEdgesTex.Get(), nullptr, &cmaaEdgesUAV), "CMAA2 edges view");
		td.Format = DXGI_FORMAT_R32_UINT; td.Width = (w + 1) / 2; td.Height = (h + 1) / 2;
		check(dev->CreateTexture2D(&td, nullptr, &cmaaHeadsTex), "CMAA2 list heads");
		check(dev->CreateUnorderedAccessView(cmaaHeadsTex.Get(), nullptr, &cmaaHeadsUAV), "CMAA2 list heads view");
		auto structured = [dev](UINT count, UINT stride, ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11UnorderedAccessView>& uav, const char* what)
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = count * stride; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = stride;
			check(dev->CreateBuffer(&bd, nullptr, &buf), what);
			D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = count;
			check(dev->CreateUnorderedAccessView(buf.Get(), &ud, &uav), what);
		};
		structured(w * h / 4, 4, cmaaCandidates, cmaaCandidatesUAV, "CMAA2 shape candidates");
		structured(w * h / 2, 8, cmaaItems, cmaaItemsUAV, "CMAA2 blend items");
		structured((w * h + 3) / 6, 4, cmaaLocations, cmaaLocationsUAV, "CMAA2 blend locations");
		auto raw = [dev](UINT words, UINT misc, ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11UnorderedAccessView>& uav, const char* what)
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = words * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | misc;
			const UINT zeros[16] = {};
			D3D11_SUBRESOURCE_DATA init = { zeros, 0, 0 };
			check(dev->CreateBuffer(&bd, &init, &buf), what);
			D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = words;
			ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			check(dev->CreateUnorderedAccessView(buf.Get(), &ud, &uav), what);
		};
		raw(16, 0, cmaaControl, cmaaControlUAV, "CMAA2 control buffer");
		raw(4, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, cmaaIndirect, cmaaIndirectUAV, "CMAA2 dispatch arguments");
		cmaaW = w; cmaaH = h; cmaaFormat = back.Format;
		PLOG_INFO << "CMAA2: " << w << "x" << h << " format " << (int)back.Format;
	}

	// false: CMAA2 cannot run here (logged once): the caller runs SMAA instead
	bool runCmaa(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back, const D3D11_TEXTURE2D_DESC& bd, int quality, bool showEdges)
	{
		if (cmaaFailed) return false;
		try
		{
			if (bd.Width != cmaaW || bd.Height != cmaaH || bd.Format != cmaaFormat) makeCmaa(dev, bd);
		}
		catch (HCMRuntimeException& ex)
		{
			cmaaFailed = true;
			releaseCmaa();
			PLOG_ERROR << ex.what() << " - CMAA2 is off until HCM restarts (SMAA is used instead)";
			return false;
		}
		const int q = std::clamp(quality, 0, 3);
		ctx->CopyResource(cmaaWork.Get(), back);

		// the compute state this touches, saved and put back (MCC caches device state between frames)
		ID3D11ComputeShader* oldCS = nullptr; ID3D11ClassInstance* oldInst[256] = {}; UINT oldInstN = 256;
		ctx->CSGetShader(&oldCS, oldInst, &oldInstN);
		ID3D11UnorderedAccessView* oldUAV[8] = {}; ctx->CSGetUnorderedAccessViews(0, 8, oldUAV);
		ID3D11ShaderResourceView* oldSRV[4] = {}; ctx->CSGetShaderResources(0, 4, oldSRV);
		ID3D11SamplerState* oldSamp = nullptr; ctx->CSGetSamplers(0, 1, &oldSamp);

		ID3D11SamplerState* samp = pointSampler.Get();
		ctx->CSSetSamplers(0, 1, &samp);
		ID3D11UnorderedAccessView* uavs[8] = { nullptr, cmaaEdgesUAV.Get(), cmaaCandidatesUAV.Get(), cmaaLocationsUAV.Get(), cmaaItemsUAV.Get(),
			cmaaHeadsUAV.Get(), cmaaControlUAV.Get(), cmaaIndirectUAV.Get() };
		ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		ID3D11ShaderResourceView* srvs[4] = { cmaaWorkSRV.Get(), nullptr, nullptr, nullptr };
		ctx->CSSetShaderResources(0, 4, srvs);
		// 1. edges (16x16 groups each writing 14x14 2x2 blocks)
		ctx->CSSetShader(cmaaEdges[q].Get(), nullptr, 0);
		ctx->Dispatch((cmaaW + 28 - 1) / 28, (cmaaH + 28 - 1) / 28, 1);
		// 2. candidates, dispatched indirectly
		ctx->CSSetShader(cmaaDispatchArgs.Get(), nullptr, 0);
		ctx->Dispatch(2, 1, 1);
		ctx->CSSetShader(cmaaProcess[q].Get(), nullptr, 0);
		ctx->DispatchIndirect(cmaaIndirect.Get(), 0);
		ctx->CSSetShader(cmaaDispatchArgs.Get(), nullptr, 0);
		ctx->Dispatch(1, 2, 1);
		// 3. the anti-aliased colours into the copy (its read view off first)
		srvs[0] = nullptr;
		ctx->CSSetShaderResources(0, 4, srvs);
		uavs[0] = cmaaWorkUAV.Get();
		ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		ctx->CSSetShader(cmaaApply[q].Get(), nullptr, 0);
		ctx->DispatchIndirect(cmaaIndirect.Get(), 0);
		if (showEdges)
		{
			ctx->CSSetShader(cmaaDebugEdges.Get(), nullptr, 0);
			ctx->Dispatch((cmaaW + 15) / 16, (cmaaH + 15) / 16, 1);
		}

		ID3D11UnorderedAccessView* noUAV[8] = {};
		ctx->CSSetUnorderedAccessViews(0, 8, noUAV, nullptr);
		ctx->CSSetShader(oldCS, oldInst, oldInstN);
		ctx->CSSetUnorderedAccessViews(0, 8, oldUAV, nullptr);
		ctx->CSSetShaderResources(0, 4, oldSRV);
		ctx->CSSetSamplers(0, 1, &oldSamp);
		if (oldCS) oldCS->Release();
		for (UINT i = 0; i < oldInstN && i < 256; ++i) if (oldInst[i]) oldInst[i]->Release();
		for (auto* u : oldUAV) if (u) u->Release();
		for (auto* s : oldSRV) if (s) s->Release();
		if (oldSamp) oldSamp->Release();

		ctx->CopyResource(back, cmaaWork.Get());
		return true;
	}

	void releaseAll()
	{
		releaseCmaa();
		edgeVS.Reset(); blendVS.Reset(); neighborVS.Reset();
		lumaEdgePS.Reset(); colorEdgePS.Reset(); blendPS.Reset(); blendNoDiagPS.Reset(); neighborPS.Reset(); edgeDebugPS.Reset();
		depthLogPS.Reset(); lumaEdgePredPS.Reset(); colorEdgePredPS.Reset();
		GameDepthTap::release();
		linearSampler.Reset(); pointSampler.Reset(); areaSRV.Reset(); searchSRV.Reset(); metricsCB.Reset();
		rasterizer.Reset(); noDepth.Reset(); noBlend.Reset();
		releaseSized();
		device = nullptr;
	}
	void releaseSized()
	{
		colorCopy.Reset(); colorSRV.Reset(); colorLinearSRV.Reset(); haveLinearView = false; predTex.Reset(); predRTV.Reset(); predSRV.Reset(); edgesTex.Reset(); edgesRTV.Reset(); edgesSRV.Reset(); blendTex.Reset(); blendRTV.Reset(); blendSRV.Reset();
		width = height = 0; format = DXGI_FORMAT_UNKNOWN;
	}

	static void check(HRESULT hr, const char* what)
	{
		if (FAILED(hr)) throw HCMRuntimeException(std::format("SMAA: {} failed (hr {:#x})", what, (uint32_t)hr));
	}

	void makeStatic(ID3D11Device* dev)
	{
		D3D11_SAMPLER_DESC sd = {};
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		check(dev->CreateSamplerState(&sd, &linearSampler), "linear sampler");
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		check(dev->CreateSamplerState(&sd, &pointSampler), "point sampler");

		auto table = [dev](UINT w, UINT h, DXGI_FORMAT f, UINT pitch, const void* bytes, ComPtr<ID3D11ShaderResourceView>& srv, const char* what)
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init = { bytes, pitch, 0 };
			ComPtr<ID3D11Texture2D> t;
			check(dev->CreateTexture2D(&td, &init, &t), what);
			check(dev->CreateShaderResourceView(t.Get(), nullptr, &srv), what);
		};
		table(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, AREATEX_PITCH, areaTexBytes, areaSRV, "area table");
		table(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, SEARCHTEX_PITCH, searchTexBytes, searchSRV, "search table");

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(Constants); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		check(dev->CreateBuffer(&bd, nullptr, &metricsCB), "constant buffer");

		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		check(dev->CreateRasterizerState(&rd, &rasterizer), "rasterizer state");
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS; dd.StencilEnable = FALSE;
		check(dev->CreateDepthStencilState(&dd, &noDepth), "depth state");
		D3D11_BLEND_DESC bld = {};
		bld.RenderTarget[0].BlendEnable = FALSE; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		check(dev->CreateBlendState(&bld, &noBlend), "blend state");
	}

	void makeShaders(ID3D11Device* dev)
	{
		const SMAAShaderSet& s = SMAAShaders;
		auto vs = [dev](const SMAAShaderBlob& b, ComPtr<ID3D11VertexShader>& out, const char* what) { check(dev->CreateVertexShader(b.data, b.size, nullptr, &out), what); };
		auto ps = [dev](const SMAAShaderBlob& b, ComPtr<ID3D11PixelShader>& out, const char* what) { check(dev->CreatePixelShader(b.data, b.size, nullptr, &out), what); };
		vs(s.edgeVS, edgeVS, "edge VS"); ps(s.lumaEdgePS, lumaEdgePS, "luma edge PS"); ps(s.colorEdgePS, colorEdgePS, "colour edge PS");
		vs(s.blendVS, blendVS, "blend VS"); ps(s.blendPS, blendPS, "blend PS"); ps(s.blendNoDiagPS, blendNoDiagPS, "blend PS (no diagonals)");
		vs(s.neighborVS, neighborVS, "neighbor VS"); ps(s.neighborPS, neighborPS, "neighbor PS"); ps(s.edgeDebugPS, edgeDebugPS, "edge debug PS");
		ps(s.depthLogPS, depthLogPS, "depth log PS"); ps(s.lumaEdgePredPS, lumaEdgePredPS, "luma edge PS (depth)"); ps(s.colorEdgePredPS, colorEdgePredPS, "colour edge PS (depth)");
	}

	// an 8-bit RGBA back buffer: the copy is made TYPELESS so it can be read both as is (edge detection, in gamma space as SMAA
	// wants) and as sRGB (the blend, in linear light). Other formats: one plain view, blended as is.
	static bool srgbTwin(DXGI_FORMAT f, DXGI_FORMAT& typeless, DXGI_FORMAT& plain, DXGI_FORMAT& srgb)
	{
		switch (f)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS; plain = DXGI_FORMAT_R8G8B8A8_UNORM; srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; return true;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS; plain = DXGI_FORMAT_B8G8R8A8_UNORM; srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; return true;
		default: return false;
		}
	}

	void makeSized(ID3D11Device* dev, ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& back)
	{
		releaseSized();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = back.Width; td.Height = back.Height; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		// the frame's copy: the back buffer's format family (CopyResource needs it), read as a texture
		DXGI_FORMAT typeless, plain, srgb;
		haveLinearView = srgbTwin(back.Format, typeless, plain, srgb);
		td.Format = haveLinearView ? typeless : back.Format; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		check(dev->CreateTexture2D(&td, nullptr, &colorCopy), "frame copy");
		if (haveLinearView)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
			sv.Format = plain;
			check(dev->CreateShaderResourceView(colorCopy.Get(), &sv, &colorSRV), "frame copy view");
			sv.Format = srgb;
			check(dev->CreateShaderResourceView(colorCopy.Get(), &sv, &colorLinearSRV), "frame copy sRGB view");
		}
		else
		{
			check(dev->CreateShaderResourceView(colorCopy.Get(), nullptr, &colorSRV), "frame copy view");
			colorLinearSRV = colorSRV;
		}
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.Format = DXGI_FORMAT_R8G8_UNORM;
		check(dev->CreateTexture2D(&td, nullptr, &edgesTex), "edges texture");
		check(dev->CreateRenderTargetView(edgesTex.Get(), nullptr, &edgesRTV), "edges target");
		check(dev->CreateShaderResourceView(edgesTex.Get(), nullptr, &edgesSRV), "edges view");
		td.Format = DXGI_FORMAT_R32_FLOAT;
		check(dev->CreateTexture2D(&td, nullptr, &predTex), "depth texture");
		check(dev->CreateRenderTargetView(predTex.Get(), nullptr, &predRTV), "depth target");
		check(dev->CreateShaderResourceView(predTex.Get(), nullptr, &predSRV), "depth view");
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		check(dev->CreateTexture2D(&td, nullptr, &blendTex), "weights texture");
		check(dev->CreateRenderTargetView(blendTex.Get(), nullptr, &blendRTV), "weights target");
		check(dev->CreateShaderResourceView(blendTex.Get(), nullptr, &blendSRV), "weights view");
		width = back.Width; height = back.Height; format = back.Format;
		lastConstants = {};   // rewritten on the next frame
		PLOG_INFO << "SMAA: " << width << "x" << height << " format " << (int)format << (haveLinearView ? " (gamma-correct blend)" : " (blended as is)");
	}

	void onPresent(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* swap, ID3D11RenderTargetView* backRTV)
	{
		auto settings = settingsWeak.lock();
		// the depth tap copies the game's depth mid-frame only while this asks for it; the frame counter stamps each copy
		struct FrameEnd { ~FrameEnd() { GameDepthTap::endFrame(); } } frameEnd;
		const bool cmaa = settings && settings->postAAMethod->GetValue() == SettingsEnums::PostAAMethod::CMAA2;
		// 2026-11-24: method TAA = Halo 2's temporal AA, done inside the game's frame by H2TAA.cpp - nothing to do here
		const bool taa = settings && settings->postAAMethod->GetValue() == SettingsEnums::PostAAMethod::TAA;
		GameDepthTap::setWanted(settings && settings->smaaToggle->GetValue() && !taa && settings->smaaUseDepth->GetValue() && (!cmaa || cmaaFailed));
		if (!settings || !settings->smaaToggle->GetValue() || taa) return;
		if (!dev || !ctx || !swap || !backRTV) return;
		std::unique_lock lk(mtx, std::try_to_lock);
		if (!lk.owns_lock() || dead || failed) return;
		try
		{
			ComPtr<ID3D11Texture2D> back;
			check(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "back buffer");
			D3D11_TEXTURE2D_DESC bd;
			back->GetDesc(&bd);
			if (bd.SampleDesc.Count != 1) return;   // a multisampled back buffer: not ours to resolve

			if (device != dev) { releaseAll(); device = dev; makeStatic(dev); makeShaders(dev); }
			// CMAA2 (2026-11-23): instead of SMAA when chosen and it can run here
			if (cmaa && runCmaa(dev, ctx, back.Get(), bd, (int)settings->smaaQuality->GetValue(), settings->smaaShowEdges->GetValue())) return;
			if (bd.Width != width || bd.Height != height || bd.Format != format) makeSized(dev, ctx, bd);

			// the tuning (2026-11-21): SMAA's preset search lengths, the user's threshold and edge detector
			struct Q { float steps, diagSteps, corner; };
			static constexpr Q kQuality[4] = { { 4.f, 0.f, 100.f }, { 8.f, 0.f, 100.f }, { 16.f, 8.f, 25.f }, { 32.f, 16.f, 25.f } };   // corner 100 = none (SMAA's Low / Medium)
			const Q q = kQuality[std::clamp((int)settings->smaaQuality->GetValue(), 0, 3)];
			const bool colourEdges = settings->smaaEdgeDetection->GetValue() == SettingsEnums::SMAAEdgeDetection::Colour;
			const bool showEdges = settings->smaaShowEdges->GetValue();
			// sRGB output: the blend reads linear light; encode it back unless the target view is sRGB itself
			bool encodeOut = false;
			if (haveLinearView)
			{
				D3D11_RENDER_TARGET_VIEW_DESC rd = {};
				backRTV->GetDesc(&rd);
				encodeOut = rd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && rd.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			}
			Constants k = {};
			k.metrics[0] = 1.f / width; k.metrics[1] = 1.f / height; k.metrics[2] = (float)width; k.metrics[3] = (float)height;
			k.params[0] = std::clamp(settings->smaaThreshold->GetValue(), 0.01f, 0.5f); k.params[1] = q.steps; k.params[2] = q.diagSteps; k.params[3] = q.corner;
			k.params2[0] = 2.f; k.params2[1] = encodeOut ? 1.f : 0.f;
			// depth predication: geometry edges keep the threshold (scale x (1 - strength) = 1), the rest x scale
			GameDepthTap::View depth;
			const bool useDepth = settings->smaaUseDepth->GetValue() && GameDepthTap::current(dev, depth);
			if (useDepth != loggedDepth) { loggedDepth = useDepth; PLOG_INFO << "SMAA: " << (useDepth ? "using the game's depth (predicated edges)" : "no game depth this frame - plain edges"); }
			{
				const float sc = std::clamp(settings->smaaDepthStrength->GetValue(), 1.f, 10.f);
				k.pred[0] = 0.07f; k.pred[1] = sc; k.pred[2] = 1.f - 1.f / sc; k.pred[3] = depth.reversedZ ? 1.f : 0.f;
				k.depthUv[0] = depth.uvScale[0]; k.depthUv[1] = depth.uvScale[1]; k.depthUv[2] = depth.uvOffset[0]; k.depthUv[3] = depth.uvOffset[1];
			}
			if (memcmp(&k, &lastConstants, sizeof(k)) != 0) { ctx->UpdateSubresource(metricsCB.Get(), 0, nullptr, &k, 0, 0); lastConstants = k; }

			ctx->CopyResource(colorCopy.Get(), back.Get());

			D3D11StateBackup saved;
			saved.save(ctx);

			const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
			ctx->RSSetViewports(1, &vp);
			ctx->RSSetState(rasterizer.Get());
			const FLOAT zero4[4] = { 0.f, 0.f, 0.f, 0.f };
			ctx->OMSetBlendState(noBlend.Get(), zero4, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(noDepth.Get(), 0);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->IASetInputLayout(nullptr);
			ID3D11Buffer* noVB = nullptr; UINT zeroU = 0;
			ctx->IASetVertexBuffers(0, 1, &noVB, &zeroU, &zeroU);
			ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
			ID3D11Buffer* cb = metricsCB.Get();
			ctx->VSSetConstantBuffers(0, 1, &cb);
			ctx->PSSetConstantBuffers(0, 1, &cb);
			ID3D11SamplerState* samplers[2] = { linearSampler.Get(), pointSampler.Get() };
			ctx->PSSetSamplers(0, 2, samplers);
			ID3D11ShaderResourceView* none[7] = {};

			ID3D11RenderTargetView* rt = nullptr;
			// 0. (with depth) the game's depth -> log depth at back-buffer size
			if (useDepth)
			{
				ctx->PSSetShaderResources(0, 7, none);
				rt = predRTV.Get();
				ctx->OMSetRenderTargets(1, &rt, nullptr);
				ID3D11ShaderResourceView* p0[7] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, depth.srv };
				ctx->PSSetShaderResources(0, 7, p0);
				ctx->VSSetShader(neighborVS.Get(), nullptr, 0);
				ctx->PSSetShader(depthLogPS.Get(), nullptr, 0);
				ctx->Draw(3, 0);
			}

			// 1. edges (cleared: the pass discards edgeless pixels)
			ctx->PSSetShaderResources(0, 7, none);
			rt = edgesRTV.Get();
			ctx->OMSetRenderTargets(1, &rt, nullptr);
			ctx->ClearRenderTargetView(edgesRTV.Get(), zero4);
			ID3D11ShaderResourceView* p1[6] = { colorSRV.Get(), nullptr, nullptr, nullptr, nullptr, useDepth ? predSRV.Get() : nullptr };
			ctx->PSSetShaderResources(0, 6, p1);
			ctx->VSSetShader(edgeVS.Get(), nullptr, 0);
			ctx->PSSetShader(useDepth ? (colourEdges ? colorEdgePredPS.Get() : lumaEdgePredPS.Get()) : (colourEdges ? colorEdgePS.Get() : lumaEdgePS.Get()), nullptr, 0);
			ctx->Draw(3, 0);

			if (showEdges)
			{
				// the tuning view: the edges over the dimmed frame, straight into the back buffer (no smoothing)
				rt = backRTV;
				ctx->OMSetRenderTargets(1, &rt, nullptr);
				ID3D11ShaderResourceView* pd[5] = { colorSRV.Get(), edgesSRV.Get(), nullptr, nullptr, nullptr };
				ctx->PSSetShaderResources(0, 5, pd);
				ctx->VSSetShader(neighborVS.Get(), nullptr, 0);
				ctx->PSSetShader(edgeDebugPS.Get(), nullptr, 0);
				ctx->Draw(3, 0);
				ctx->PSSetShaderResources(0, 7, none);
				saved.restore(ctx);
				return;
			}

			// 2. blending weights
			rt = blendRTV.Get();
			ctx->OMSetRenderTargets(1, &rt, nullptr);   // unbinds edges as a target before it is read
			ctx->ClearRenderTargetView(blendRTV.Get(), zero4);
			ID3D11ShaderResourceView* p2[5] = { nullptr, edgesSRV.Get(), nullptr, areaSRV.Get(), searchSRV.Get() };
			ctx->PSSetShaderResources(0, 5, p2);
			ctx->VSSetShader(blendVS.Get(), nullptr, 0);
			ctx->PSSetShader(q.diagSteps > 0.f ? blendPS.Get() : blendNoDiagPS.Get(), nullptr, 0);
			ctx->Draw(3, 0);

			// 3. neighbourhood blending into the back buffer
			rt = backRTV;
			ctx->OMSetRenderTargets(1, &rt, nullptr);
			ID3D11ShaderResourceView* p3[5] = { colorLinearSRV.Get(), nullptr, blendSRV.Get(), nullptr, nullptr };   // linear light when the format allows
			ctx->PSSetShaderResources(0, 5, p3);
			ctx->VSSetShader(neighborVS.Get(), nullptr, 0);
			ctx->PSSetShader(neighborPS.Get(), nullptr, 0);
			ctx->Draw(3, 0);

			ctx->PSSetShaderResources(0, 7, none);
			saved.restore(ctx);
		}
		catch (HCMRuntimeException& ex)
		{
			failed = true;
			releaseAll();
			PLOG_ERROR << ex.what() << " - SMAA is off until HCM restarts";
		}
	}
};

SMAAPostProcess::SMAAPostProcess(std::shared_ptr<D3D11Hook> d3d, std::shared_ptr<SettingsStateAndEvents> settings)
	: pimpl(std::make_shared<Impl>())
{
	pimpl->settingsWeak = settings;
	// 2026-11-21: the pre-capture event (D3D11Hook::preCapturePresentEvent), not presentHookEvent - SMAA is part of the game's
	// picture, so it stays in OBS recordings with the OBS bypass on (the bypass moves presentHookEvent past OBS's capture)
	if (!d3d || !d3d->preCapturePresentEvent) return;
	pimpl->presentEventWeak = d3d->preCapturePresentEvent;
	// FIRST in the list: before HCM's own window / overlays draw into the frame
	pimpl->presentHandle = d3d->preCapturePresentEvent->prepend(
		[impl = pimpl](ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* swap, ID3D11RenderTargetView* rtv)
		{ impl->onPresent(dev, ctx, swap, rtv); });
	PLOG_INFO << "SMAA: listening on D3D11 Present";
}

SMAAPostProcess::~SMAAPostProcess()
{
	if (auto ev = pimpl->presentEventWeak.lock(); ev && pimpl->presentHandle)
		ev->remove(pimpl->presentHandle);
	std::scoped_lock lk(pimpl->mtx);   // waits for a frame in progress
	pimpl->dead = true;
	pimpl->releaseAll();
}
