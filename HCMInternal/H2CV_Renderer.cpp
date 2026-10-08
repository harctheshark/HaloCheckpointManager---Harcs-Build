#include "pch.h"
#include "H2CV_Renderer.h"
#include "H2CV_Log.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <cstring>
#include <cmath>

using namespace DirectX;

namespace h2cv
{
	namespace
	{
		template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

		const char* kShaderSrc = R"HLSL(
cbuffer CB : register(b0)
{
	row_major float4x4 gViewProj;
	float2 gViewport; float gLineWidth; float gPull;
	float gHiddenAlpha; float gHiddenPass; float gUseGame; float gGameEnc;
	float gGameNear; float gGameFar; float2 gGameScale;
	float gUseFp; float gFpClear; float gGameTol; float gStdZ;
};
Texture2D<float> gGameDepth : register(t0);
Texture2D<float> gFpDepth : register(t1);

struct VSIn { float3 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 clip : POSITION0; float4 col : COLOR; };
struct PSIn { float4 pos : SV_Position; float4 col : COLOR; float view : TEXCOORD0; };

VSOut VSLine(VSIn i) { VSOut o; o.clip = mul(float4(i.pos, 1), gViewProj); o.col = i.col; return o; }
float4 VSPos(float3 pos : POSITION) : SV_Position { return mul(float4(pos, 1), gViewProj); }
PSIn VSFill(VSIn i)
{
	PSIn o; o.pos = mul(float4(i.pos, 1), gViewProj); o.col = i.col; o.view = o.pos.w;
	o.pos.z = gStdZ > 0.5 ? o.pos.z - gPull * o.pos.w : o.pos.z * (1.0 + gPull);
	return o;
}

// reversed-Z infinite projection: z_clip = near (constant), w = view depth. Near plane: w >= z_clip.
PSIn mk(float4 p, float2 offPx, float4 col)
{
	PSIn o;
	float z = gStdZ > 0.5 ? p.z - gPull * p.w : p.z * (1.0 + gPull);
	o.pos = float4(p.xy + offPx * (2.0 / gViewport) * p.w, z, p.w);
	o.col = col; o.view = p.w;
	return o;
}

[maxvertexcount(4)]
void GSLine(line VSOut v[2], inout TriangleStream<PSIn> s)
{
	float4 p0 = v[0].clip, p1 = v[1].clip;
	float f0 = gStdZ > 0.5 ? p0.z : p0.w - p0.z;       // >= 0 in front of the near plane
	float f1 = gStdZ > 0.5 ? p1.z : p1.w - p1.z;
	if (f0 < 0 && f1 < 0) return;
	if (f0 < 0) p0 = lerp(p0, p1, f0 / (f0 - f1));
	else if (f1 < 0) p1 = lerp(p1, p0, f1 / (f1 - f0));
	float2 s0 = p0.xy / p0.w * gViewport * 0.5, s1 = p1.xy / p1.w * gViewport * 0.5;
	float2 d = s1 - s0; float len = length(d);
	d = len > 1e-4 ? d / len : float2(1, 0);
	float hw = gLineWidth * 0.5;
	float2 n = float2(-d.y, d.x) * hw, e = d * hw;
	s.Append(mk(p0, -e + n, v[0].col));
	s.Append(mk(p0, -e - n, v[0].col));
	s.Append(mk(p1,  e + n, v[1].col));
	s.Append(mk(p1,  e - n, v[1].col));
}

float linearGame(float z)
{
	float n = gGameNear, f = gGameFar;
	if (gGameEnc == 1) return f * n / max(f - z * (f - n), 1e-6);   // standard: near->0, far->1
	if (gGameEnc == 2) return n * f / max(z * (f - n) + n, 1e-6);   // reversed finite: near->1, far->0
	return n / max(z, 1e-9);                                        // reversed infinite
}

bool gameOccluded(float2 px, float view)
{
	int2 t = int2(px * gGameScale);
	bool occ = false;
	if (gUseFp > 0.5)
		occ = abs(gFpDepth.Load(int3(t, 0)) - gFpClear) > 1e-7;          // first-person viewmodel covers this pixel
	float z = gGameDepth.Load(int3(t, 0));
	bool cleared = gGameEnc == 1 ? (z >= 1.0) : (z <= 0.0);              // cleared / sky
	if (!cleared) occ = occ || view > linearGame(z) * (1.0 + gGameTol);
	return occ;
}

float4 PSLine(PSIn i) : SV_Target
{
	float4 c = i.col;
	if (gUseGame > 0.5)
	{
		bool occ = gameOccluded(i.pos.xy, i.view);
		if (gHiddenPass == 0 && occ) discard;
		if (gHiddenPass == 2 && !occ) discard;
	}
	if (gHiddenPass > 0) c.a *= gHiddenAlpha;
	return c;
}
float4 PSFill(PSIn i) : SV_Target { return i.col; }

// full-screen triangle + first-person merge: wherever the FP depth copy has geometry, force the world copy to "nearest"
Texture2D<float> gFpCopy : register(t2);
float4 VSFull(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float PSMerge(float4 pos : SV_Position) : SV_Depth
{
	float z = gFpCopy.Load(int3(pos.xy, 0));
	bool weapon = gStdZ > 0.5 ? (z < 1.0) : (z > 0.0);
	if (!weapon) discard;
	return gStdZ > 0.5 ? 0.0 : 1.0;
}
)HLSL";

		struct CBData
		{
			XMFLOAT4X4 viewProj;
			float viewport[2]; float lineWidth; float pull;
			float hiddenAlpha; float hiddenPass; float useGame; float gameEnc;
			float gameNear; float gameFar; float gameScale[2];
			float useFp; float fpClear; float gameTol; float stdZ;
		};
	}

	struct Renderer::Saved
	{
		UINT scissorCount = 0, viewportCount = 0;
		D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ID3D11RasterizerState* rs = nullptr;
		ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4]{}; UINT sampleMask = 0;
		ID3D11DepthStencilState* ds = nullptr; UINT stencilRef = 0;
		ID3D11ShaderResourceView* psSrv[3]{};
		ID3D11SamplerState* psSampler = nullptr;
		ID3D11PixelShader* ps = nullptr; ID3D11VertexShader* vs = nullptr; ID3D11GeometryShader* gs = nullptr;
		ID3D11ClassInstance* psInst[256]{}; ID3D11ClassInstance* vsInst[256]{}; ID3D11ClassInstance* gsInst[256]{};
		UINT psInstCount = 256, vsInstCount = 256, gsInstCount = 256;
		ID3D11Buffer* vsCB = nullptr; ID3D11Buffer* psCB = nullptr; ID3D11Buffer* gsCB = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology{};
		ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFormat{}; UINT ibOffset = 0;
		ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOffset = 0;
		ID3D11InputLayout* layout = nullptr;
		ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv = nullptr;
	};

	bool Renderer::compile()
	{
		auto comp = [&](const char* entry, const char* target, ID3DBlob** out) -> bool
		{
			ID3DBlob* err = nullptr;
			HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "h2cv", nullptr, nullptr, entry, target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
			if (FAILED(hr)) { LOGF("shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?"); rel(err); return false; }
			rel(err);
			return true;
		};
		ID3DBlob *vsl = nullptr, *gsl = nullptr, *psl = nullptr, *vsp = nullptr, *psf = nullptr, *vsf = nullptr, *vsu = nullptr, *psm = nullptr;
		bool ok = comp("VSLine", "vs_5_0", &vsl) && comp("GSLine", "gs_5_0", &gsl) && comp("PSLine", "ps_5_0", &psl)
			&& comp("VSPos", "vs_5_0", &vsp) && comp("PSFill", "ps_5_0", &psf) && comp("VSFill", "vs_5_0", &vsf)
			&& comp("VSFull", "vs_5_0", &vsu) && comp("PSMerge", "ps_5_0", &psm);
		if (ok)
		{
			ok = SUCCEEDED(mDev->CreateVertexShader(vsl->GetBufferPointer(), vsl->GetBufferSize(), nullptr, &mVsLine))
				&& SUCCEEDED(mDev->CreateGeometryShader(gsl->GetBufferPointer(), gsl->GetBufferSize(), nullptr, &mGsLine))
				&& SUCCEEDED(mDev->CreatePixelShader(psl->GetBufferPointer(), psl->GetBufferSize(), nullptr, &mPsLine))
				&& SUCCEEDED(mDev->CreateVertexShader(vsp->GetBufferPointer(), vsp->GetBufferSize(), nullptr, &mVsPos))
				&& SUCCEEDED(mDev->CreatePixelShader(psf->GetBufferPointer(), psf->GetBufferSize(), nullptr, &mPsFill))
				&& SUCCEEDED(mDev->CreateVertexShader(vsf->GetBufferPointer(), vsf->GetBufferSize(), nullptr, &mVsFill))
				&& SUCCEEDED(mDev->CreateVertexShader(vsu->GetBufferPointer(), vsu->GetBufferSize(), nullptr, &mVsFull))
				&& SUCCEEDED(mDev->CreatePixelShader(psm->GetBufferPointer(), psm->GetBufferSize(), nullptr, &mPsMerge));
			const D3D11_INPUT_ELEMENT_DESC le[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
			const D3D11_INPUT_ELEMENT_DESC pe[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
			ok = ok && SUCCEEDED(mDev->CreateInputLayout(le, 2, vsl->GetBufferPointer(), vsl->GetBufferSize(), &mLayoutLine))
				&& SUCCEEDED(mDev->CreateInputLayout(pe, 1, vsp->GetBufferPointer(), vsp->GetBufferSize(), &mLayoutPos));
		}
		rel(vsl); rel(gsl); rel(psl); rel(vsp); rel(psf); rel(vsf); rel(vsu); rel(psm);
		return ok;
	}

	bool Renderer::init(ID3D11Device* dev)
	{
		mDev = dev;
		if (!compile()) return false;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(CBData); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(mDev->CreateBuffer(&bd, nullptr, &mCB))) return false;

		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_GREATER;
		mDev->CreateDepthStencilState(&dd, &mDsWrite);
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
		mDev->CreateDepthStencilState(&dd, &mDsTest);
		dd.DepthFunc = D3D11_COMPARISON_LESS;
		mDev->CreateDepthStencilState(&dd, &mDsHidden);
		dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		mDev->CreateDepthStencilState(&dd, &mDsTestStd);
		dd.DepthFunc = D3D11_COMPARISON_GREATER;
		mDev->CreateDepthStencilState(&dd, &mDsHiddenStd);
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		mDev->CreateDepthStencilState(&dd, &mDsAlwaysWrite);
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthEnable = FALSE;
		mDev->CreateDepthStencilState(&dd, &mDsNone);

		D3D11_BLEND_DESC bl{};
		bl.RenderTarget[0].BlendEnable = TRUE;
		bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		mDev->CreateBlendState(&bl, &mBlendAlpha);
		bl.RenderTarget[0].BlendEnable = FALSE; bl.RenderTarget[0].RenderTargetWriteMask = 0;
		mDev->CreateBlendState(&bl, &mBlendNoColor);

		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		mDev->CreateRasterizerState(&rd, &mRsNoCull);

		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		mDev->CreateSamplerState(&sd, &mPointClamp);

		mSaved = new Saved();
		mReady = mDsWrite && mDsTest && mDsHidden && mDsNone && mBlendAlpha && mBlendNoColor && mRsNoCull;
		LOGF("renderer init %s", mReady ? "OK" : "FAILED");
		return mReady;
	}

	void Renderer::shutdown()
	{
		rel(mVsLine); rel(mGsLine); rel(mPsLine); rel(mVsPos); rel(mPsFill); rel(mVsFill); rel(mVsFull); rel(mPsMerge); rel(mDsAlwaysWrite);
		rel(mWDsv); rel(mWTex); rel(mFSrv); rel(mFTex);
		rel(mLayoutLine); rel(mLayoutPos); rel(mCB); rel(mVB); rel(mStaticVB);
		rel(mDsv); rel(mDepthTex);
		rel(mDsWrite); rel(mDsTest); rel(mDsHidden); rel(mDsNone); rel(mDsTestStd); rel(mDsHiddenStd);
		rel(mBlendAlpha); rel(mBlendNoColor); rel(mRsNoCull); rel(mPointClamp);
		delete mSaved; mSaved = nullptr;
		mReady = false;
	}

	void Renderer::ensureDepth(uint32_t w, uint32_t h)
	{
		if (mDsv && w == mDepthW && h == mDepthH) return;
		rel(mDsv); rel(mDepthTex);
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_D32_FLOAT;
		td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (SUCCEEDED(mDev->CreateTexture2D(&td, nullptr, &mDepthTex)))
			mDev->CreateDepthStencilView(mDepthTex, nullptr, &mDsv);
		mDepthW = w; mDepthH = h;
	}

	void Renderer::saveState()
	{
		Saved& s = *mSaved;
		ID3D11DeviceContext* c = mCtx;
		s.scissorCount = s.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		c->RSGetScissorRects(&s.scissorCount, s.scissors);
		c->RSGetViewports(&s.viewportCount, s.viewports);
		c->RSGetState(&s.rs);
		c->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
		c->OMGetDepthStencilState(&s.ds, &s.stencilRef);
		c->PSGetShaderResources(0, 3, s.psSrv);
		c->PSGetSamplers(0, 1, &s.psSampler);
		s.psInstCount = s.vsInstCount = s.gsInstCount = 256;
		c->PSGetShader(&s.ps, s.psInst, &s.psInstCount);
		c->VSGetShader(&s.vs, s.vsInst, &s.vsInstCount);
		c->GSGetShader(&s.gs, s.gsInst, &s.gsInstCount);
		c->VSGetConstantBuffers(0, 1, &s.vsCB);
		c->PSGetConstantBuffers(0, 1, &s.psCB);
		c->GSGetConstantBuffers(0, 1, &s.gsCB);
		c->IAGetPrimitiveTopology(&s.topology);
		c->IAGetIndexBuffer(&s.ib, &s.ibFormat, &s.ibOffset);
		c->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
		c->IAGetInputLayout(&s.layout);
		c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
	}

	void Renderer::restoreState()
	{
		Saved& s = *mSaved;
		ID3D11DeviceContext* c = mCtx;
		c->RSSetScissorRects(s.scissorCount, s.scissors);
		c->RSSetViewports(s.viewportCount, s.viewports);
		c->RSSetState(s.rs); rel(s.rs);
		c->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask); rel(s.blend);
		c->OMSetDepthStencilState(s.ds, s.stencilRef); rel(s.ds);
		c->PSSetShaderResources(0, 3, s.psSrv); rel(s.psSrv[0]); rel(s.psSrv[1]); rel(s.psSrv[2]);
		c->PSSetSamplers(0, 1, &s.psSampler); rel(s.psSampler);
		c->PSSetShader(s.ps, s.psInst, s.psInstCount); rel(s.ps);
		for (UINT i = 0; i < s.psInstCount; i++) rel(s.psInst[i]);
		c->VSSetShader(s.vs, s.vsInst, s.vsInstCount); rel(s.vs);
		for (UINT i = 0; i < s.vsInstCount; i++) rel(s.vsInst[i]);
		c->GSSetShader(s.gs, s.gsInst, s.gsInstCount); rel(s.gs);
		for (UINT i = 0; i < s.gsInstCount; i++) rel(s.gsInst[i]);
		c->VSSetConstantBuffers(0, 1, &s.vsCB); rel(s.vsCB);
		c->PSSetConstantBuffers(0, 1, &s.psCB); rel(s.psCB);
		c->GSSetConstantBuffers(0, 1, &s.gsCB); rel(s.gsCB);
		c->IASetPrimitiveTopology(s.topology);
		c->IASetIndexBuffer(s.ib, s.ibFormat, s.ibOffset); rel(s.ib);
		c->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset); rel(s.vb);
		c->IASetInputLayout(s.layout); rel(s.layout);
		c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
		for (auto& r : s.rtv) rel(r);
		rel(s.dsv);
	}

	bool Renderer::begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height,
	                     const h2::Vec3& camPos, const h2::Vec3& camFwd, const h2::Vec3& camUp, float vfov, float nearClip)
	{
		if (!mReady || !ctx || !rtv || width == 0 || height == 0) return false;
		mCtx = ctx; mRtv = rtv; mW = width; mH = height; mVpX = mVpY = 0; mCamPos = camPos; mStdZ = false;
		ensureDepth(width, height);
		if (!mDsv) return false;

		// view: right-handed look-to (Halo world: x fwd, y left, z up). projection: reversed-Z, infinite far.
		const XMMATRIX view = XMMatrixLookToRH(XMVectorSet(camPos.x, camPos.y, camPos.z, 1),
			XMVectorSet(camFwd.x, camFwd.y, camFwd.z, 0), XMVectorSet(camUp.x, camUp.y, camUp.z, 0));
		const float ys = 1.0f / std::tan(vfov * 0.5f), xs = ys * (float)height / (float)width;
		const float n = nearClip > 0.0001f ? nearClip : 0.01f;
		const XMMATRIX proj(xs, 0, 0, 0,
		                    0, ys, 0, 0,
		                    0, 0, 0, -1,
		                    0, 0, n, 0);
		XMStoreFloat4x4(&mViewProj, view * proj);

		saveState();
		D3D11_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(mRsNoCull);
		ctx->OMSetRenderTargets(1, &mRtv, mDsv);
		ctx->PSSetSamplers(0, 1, &mPointClamp);
		return true;
	}

	bool Renderer::beginExternal(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv,
	                             const D3D11_VIEWPORT& vp, const DirectX::XMFLOAT4X4& viewProj)
	{
		if (!mReady || !ctx || !rtv || !dsv || vp.Width < 16 || vp.Height < 16) return false;
		mCtx = ctx; mRtv = rtv; mW = (uint32_t)vp.Width; mH = (uint32_t)vp.Height; mVpX = vp.TopLeftX; mVpY = vp.TopLeftY;
		mViewProj = viewProj;
		saveState();
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(mRsNoCull);
		ctx->OMSetRenderTargets(1, &mRtv, dsv);
		ctx->PSSetSamplers(0, 1, &mPointClamp);
		return true;
	}

	void Renderer::end()
	{
		if (!mCtx) return;
		ID3D11ShaderResourceView* none[2] = {};
		mCtx->PSSetShaderResources(0, 2, none);
		restoreState();
		mCtx = nullptr; mRtv = nullptr;
	}

	void Renderer::clearOwnDepth()
	{
		if (mCtx && mDsv) mCtx->ClearDepthStencilView(mDsv, D3D11_CLEAR_DEPTH, 0.0f, 0); // reversed-Z: far = 0
	}

	void Renderer::upload(const void* data, size_t bytes)
	{
		if (bytes > mVBBytes)
		{
			rel(mVB);
			size_t sz = 1 << 20; while (sz < bytes) sz <<= 1;
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = (UINT)sz; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(mDev->CreateBuffer(&bd, nullptr, &mVB))) { mVBBytes = 0; return; }
			mVBBytes = sz;
		}
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(mCtx->Map(mVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, data, bytes); mCtx->Unmap(mVB, 0); }
	}

	void Renderer::setCB(float widthPx, float pull, float hiddenAlpha, float hiddenPass, bool useGameDepth)
	{
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(mCtx->Map(mCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
		CBData* d = (CBData*)m.pData;
		d->viewProj = mViewProj;
		d->viewport[0] = (float)mW; d->viewport[1] = (float)mH; d->lineWidth = widthPx; d->pull = pull;
		d->hiddenAlpha = hiddenAlpha; d->hiddenPass = hiddenPass; d->useGame = useGameDepth ? 1.f : 0.f; d->gameEnc = (float)mGameDepth.encoding;
		d->gameNear = mGameDepth.nearClip; d->gameFar = mGameDepth.farClip;
		d->gameScale[0] = mGameDepth.width > 0 ? mGameDepth.width / (float)mW : 1.f;
		d->gameScale[1] = mGameDepth.height > 0 ? mGameDepth.height / (float)mH : 1.f;
		d->useFp = mGameDepth.fpSrv ? 1.f : 0.f; d->fpClear = mGameDepth.fpClearValue; d->gameTol = pull; d->stdZ = mStdZ ? 1.f : 0.f;
		mCtx->Unmap(mCB, 0);
	}

	void Renderer::setStaticOccluders(const std::vector<Tri>& tris, uintptr_t version)
	{
		if (version == mStaticVersion) return;
		mStaticVersion = version;
		rel(mStaticVB); mStaticCount = 0;
		if (tris.empty()) return;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (UINT)(tris.size() * sizeof(Tri)); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA sd{ tris.data(), 0, 0 };
		if (SUCCEEDED(mDev->CreateBuffer(&bd, &sd, &mStaticVB))) mStaticCount = (uint32_t)(tris.size() * 3);
	}

	void Renderer::drawStaticOccluders()
	{
		if (!mCtx || !mStaticVB || !mStaticCount) return;
		setCB(1, 0, 0, 0.f, false);
		const UINT stride = 12, off = 0;
		mCtx->IASetInputLayout(mLayoutPos);
		mCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		mCtx->IASetVertexBuffers(0, 1, &mStaticVB, &stride, &off);
		mCtx->VSSetShader(mVsPos, nullptr, 0); mCtx->VSSetConstantBuffers(0, 1, &mCB);
		mCtx->GSSetShader(nullptr, nullptr, 0);
		mCtx->PSSetShader(nullptr, nullptr, 0);
		mCtx->OMSetBlendState(mBlendNoColor, nullptr, 0xFFFFFFFF);
		mCtx->OMSetDepthStencilState(mDsWrite, 0);
		mCtx->Draw(mStaticCount, 0);
	}

	void Renderer::drawOccluders(const std::vector<Tri>& tris)
	{
		if (!mCtx || tris.empty()) return;
		setCB(1, 0, 0, 0.f, false);
		upload(tris.data(), tris.size() * sizeof(Tri));
		if (!mVB) return;
		const UINT stride = 12, off = 0;
		mCtx->IASetInputLayout(mLayoutPos);
		mCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		mCtx->IASetVertexBuffers(0, 1, &mVB, &stride, &off);
		mCtx->VSSetShader(mVsPos, nullptr, 0); mCtx->VSSetConstantBuffers(0, 1, &mCB);
		mCtx->GSSetShader(nullptr, nullptr, 0);
		mCtx->PSSetShader(nullptr, nullptr, 0);
		mCtx->OMSetBlendState(mBlendNoColor, nullptr, 0xFFFFFFFF);
		mCtx->OMSetDepthStencilState(mDsWrite, 0);
		mCtx->Draw((UINT)(tris.size() * 3), 0);
	}

	void Renderer::drawLines(const LineVertex* v, size_t count, float widthPx, float pull, bool useOwnDepth, bool useGameDepth,
	                         float hiddenAlpha)
	{
		if (!mCtx || !count) return;
		const bool game = useGameDepth && mGameDepth.srv && mGameDepth.encoding != 0;
		upload(v, count * sizeof(LineVertex));
		if (!mVB) return;
		const UINT stride = sizeof(LineVertex), off = 0;
		mCtx->IASetInputLayout(mLayoutLine);
		mCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
		mCtx->IASetVertexBuffers(0, 1, &mVB, &stride, &off);
		mCtx->VSSetShader(mVsLine, nullptr, 0); mCtx->VSSetConstantBuffers(0, 1, &mCB);
		mCtx->GSSetShader(mGsLine, nullptr, 0); mCtx->GSSetConstantBuffers(0, 1, &mCB);
		mCtx->PSSetShader(mPsLine, nullptr, 0); mCtx->PSSetConstantBuffers(0, 1, &mCB);
		ID3D11ShaderResourceView* srvs[2] = { game ? mGameDepth.srv : nullptr, game ? mGameDepth.fpSrv : nullptr };
		mCtx->PSSetShaderResources(0, 2, srvs);
		mCtx->OMSetBlendState(mBlendAlpha, nullptr, 0xFFFFFFFF);

		auto pass = [&](ID3D11DepthStencilState* ds, float hiddenPass)
		{
			setCB(widthPx, pull, hiddenAlpha, hiddenPass, game);
			mCtx->OMSetDepthStencilState(ds, 0);
			mCtx->Draw((UINT)count, 0);
		};
		ID3D11DepthStencilState* test = mStdZ ? mDsTestStd : mDsTest;
		ID3D11DepthStencilState* hidden = mStdZ ? mDsHiddenStd : mDsHidden;
		if (hiddenAlpha > 0.001f)
		{
			if (useOwnDepth) pass(hidden, 1);              // behind the occluders
			if (game) pass(useOwnDepth ? test : mDsNone, 2); // in front of collision, behind the game scene
		}
		pass(useOwnDepth ? test : mDsNone, 0);
		mCtx->GSSetShader(nullptr, nullptr, 0);
	}

	void Renderer::drawTris(const LineVertex* v, size_t count, bool depthTest)
	{
		if (!mCtx || !count) return;
		setCB(1, 0, 0, 0.f, false);
		upload(v, count * sizeof(LineVertex));
		if (!mVB) return;
		const UINT stride = sizeof(LineVertex), off = 0;
		mCtx->IASetInputLayout(mLayoutLine);
		mCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		mCtx->IASetVertexBuffers(0, 1, &mVB, &stride, &off);
		mCtx->VSSetShader(mVsFill, nullptr, 0); mCtx->VSSetConstantBuffers(0, 1, &mCB);
		mCtx->GSSetShader(nullptr, nullptr, 0);
		mCtx->PSSetShader(mPsFill, nullptr, 0);
		mCtx->OMSetBlendState(mBlendAlpha, nullptr, 0xFFFFFFFF);
		mCtx->OMSetDepthStencilState(depthTest ? (mStdZ ? mDsTestStd : mDsTest) : mDsNone, 0);
		mCtx->Draw((UINT)count, 0);
	}

	// typeless group + typed views for the game's depth format (W1: D32_FLOAT_S8X24_UINT; W3 saw D32_FLOAT)
	static bool depthFormats(DXGI_FORMAT f, DXGI_FORMAT& typeless, DXGI_FORMAT& dsv, DXGI_FORMAT& srv)
	{
		switch (f)
		{
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
			typeless = DXGI_FORMAT_R32G8X24_TYPELESS; dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
		case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
			typeless = DXGI_FORMAT_R32_TYPELESS; dsv = DXGI_FORMAT_D32_FLOAT; srv = DXGI_FORMAT_R32_FLOAT; return true;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
			typeless = DXGI_FORMAT_R24G8_TYPELESS; dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
		default: return false;
		}
	}

	bool Renderer::ensureCopies(ID3D11Texture2D* src)
	{
		D3D11_TEXTURE2D_DESC sd{};
		src->GetDesc(&sd);
		if (mWTex && sd.Width == mCopyDesc.Width && sd.Height == mCopyDesc.Height && sd.Format == mCopyDesc.Format) return true;
		rel(mWDsv); rel(mWTex); rel(mFSrv); rel(mFTex);
		mCopyDesc = sd;
		DXGI_FORMAT tl, dsvF, srvF;
		if (sd.SampleDesc.Count != 1 || sd.ArraySize != 1 || !depthFormats(sd.Format, tl, dsvF, srvF))
		{
			LOGF("post-bloom: unsupported game depth texture (fmt %d, %u samples)", (int)sd.Format, sd.SampleDesc.Count);
			return false;
		}
		D3D11_TEXTURE2D_DESC td = sd;
		td.Format = tl; td.MipLevels = 1; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (FAILED(mDev->CreateTexture2D(&td, nullptr, &mWTex))) return false;
		D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = dsvF; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (FAILED(mDev->CreateDepthStencilView(mWTex, &dv, &mWDsv))) return false;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(mDev->CreateTexture2D(&td, nullptr, &mFTex))) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = srvF; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
		if (FAILED(mDev->CreateShaderResourceView(mFTex, &sv, &mFSrv))) return false;
		LOGF("post-bloom depth copies: %ux%u fmt %d", sd.Width, sd.Height, (int)sd.Format);
		return true;
	}

	static ID3D11Texture2D* dsvTexture(ID3D11DepthStencilView* dsv)
	{
		ID3D11Resource* res = nullptr;
		dsv->GetResource(&res);
		if (!res) return nullptr;
		ID3D11Texture2D* tex = nullptr;
		res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
		res->Release();
		return tex;
	}

	bool Renderer::copyWorldDepth(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* gameDsv)
	{
		if (!mReady || !ctx || !gameDsv) return false;
		ID3D11Texture2D* tex = dsvTexture(gameDsv);
		if (!tex) return false;
		const bool ok = ensureCopies(tex);
		if (ok) ctx->CopyResource(mWTex, tex);
		tex->Release();
		return ok;
	}

	bool Renderer::copyFpDepthAndMerge(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* gameDsv, const D3D11_VIEWPORT& vp, bool reversed)
	{
		if (!mReady || !ctx || !gameDsv || !mWTex || !mFTex) return false;
		ID3D11Texture2D* tex = dsvTexture(gameDsv);
		if (!tex) return false;
		D3D11_TEXTURE2D_DESC sd{}; tex->GetDesc(&sd);
		const bool same = sd.Width == mCopyDesc.Width && sd.Height == mCopyDesc.Height && sd.Format == mCopyDesc.Format;
		if (same) ctx->CopyResource(mFTex, tex);
		tex->Release();
		if (!same) return false;

		mCtx = ctx;
		mStdZ = !reversed;
		saveState();
		setCB(1, 0, 0, 0.f, false);
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(mRsNoCull);
		ctx->OMSetRenderTargets(0, nullptr, mWDsv);
		ctx->OMSetBlendState(mBlendNoColor, nullptr, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(mDsAlwaysWrite, 0);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(mVsFull, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->PSSetShader(mPsMerge, nullptr, 0); ctx->PSSetConstantBuffers(0, 1, &mCB);
		ctx->PSSetShaderResources(2, 1, &mFSrv);
		ctx->Draw(3, 0);
		ID3D11ShaderResourceView* none = nullptr;
		ctx->PSSetShaderResources(2, 1, &none);
		restoreState();
		mCtx = nullptr;
		return true;
	}

	bool Renderer::project(const h2::Vec3& p, float& sx, float& sy) const
	{
		const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), XMLoadFloat4x4(&mViewProj));
		const float w = XMVectorGetW(c);
		if (w < 0.01f) return false;
		sx = mVpX + (XMVectorGetX(c) / w * 0.5f + 0.5f) * (float)mW;
		sy = mVpY + (0.5f - XMVectorGetY(c) / w * 0.5f) * (float)mH;
		return true;
	}
}
