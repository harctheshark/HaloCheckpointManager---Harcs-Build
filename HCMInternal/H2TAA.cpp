#include "pch.h"
#include "H2TAA.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "IMCCStateHook.h"
#include "GameDepthTap.h"
#include "H2PassTaps.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include "H2TAAObjects.h"
#include "H2Overscan.h"
#include "TAA\Generated\TAAShaders.h"
#include <wrl/client.h>
#include <intrin.h>
#include <array>
#include <cmath>
#include <mutex>
#include <chrono>
#include <unordered_map>

// ================================================================================================================================
// Halo 2 TAA v1 (halo2.dll 1.3528, PE stamp 0x68A0F0F2; RE 2026-11-23 in the MCC halo2.dll.i64 - research dumps in
// Documents\Halo Mod And Tools\H2 Render Research (TAA, SSAA, HUD, reset)\agent_taa\).
//
// 1. JITTER. collision_viewport_scope_rect_compute (halo2+0x7DFCD0, (camera*, float rect[4])) writes the projection window
//    {l, r, b, t} (default -1, 1, -1, 1) that collision_view_frustum_build_from_camera (0x7DF7A0) turns into the projection:
//    P[2][0] = (l + r) / (r - l), P[2][1] = (b + t) / (t - b) (halo2+0x1996BA8 / +0x1996BAC once the view is set up), and
//    clip = view * P (row vector), w = -view.z, so ndc.x = Sx * x / w - P[2][0] and ndc.y = Sy * y / w - P[2][1].
//    l, r += -jx * (r - l) / W and b, t += +jy * (t - b) / H therefore move the picture by (+jx, +jy) pixels (x right, y down,
//    D3D's viewport mapping - the same one the Collision Viewer draws its in-frame lines with). Only two call sites are
//    jittered, told apart by the return address: 0x7E231A (sub_1807E2130, the per-player view; that rect builds the view's
//    projection at 0x1996B10) and 0x7E0FCB (the first-person pass sub_1807E0C60 rebuilding the projection for the gun).
//    W / H = the camera's viewport (i16 top, left, bottom, right at camera +0x30) - render pixels (supersampling included).
//    Samples: Halton (2, 3), 8 of them, in [-0.5, 0.5]. A frame is jittered ONLY when the previous frame was resolved
//    (mFpSinceResolve == 0), so the picture can never be left jittered: no resolve -> no jitter from the next frame on, and
//    turning TAA off stops it at once (the hook checks the flag on every call).
// 2. MATRICES. No velocity buffer and no previous matrices exist. At the first-person pass ENTRY (H2PassTaps
//    firstPersonPass, right after GameDepthTap copied the world depth) the main view's world_to_view (0x1996B10,
//    real_matrix4x3) and projection (0x1996B88, 4x4 row vector, STANDARD z) are read with h2::readFrame - the same data the
//    Collision Viewer draws with in that hook. The device depth is REVERSED (1 near, 0 far = 1 - z/w; checked against
//    sub_180971F40's CPU projection: depth = (1 - f/(f-n)) + (f n/(f-n)) / w), folded into the view-projection as the
//    Collision Viewer does (clip.z' = clip.w - clip.z), so the raw device depth reconstructs the point. The jitter is taken
//    out of the snapshot (P[2][0] / P[2][1] put back to the un-jittered window). Reprojection matrix (double on the CPU):
//    inverse(VP current) * VP previous; sky / far (depth at the clear value): the rotation-only version (a direction).
//    The uploaded VS constants c0..c3 (halo2+0x197ED80, sub_180954ED0 / sub_180955590) are only logged for comparison.
// 3. RESOLVE at sub_180951EC0 ENTRY (H2PassTaps beforeResolve, from the Collision Viewer's existing hook there - not a second
//    inline hook): the scene target (halo2+0x197EE60 RTV) holds world + gun, before the engine copies it into target 39 for
//    bloom / lens flares / the final pass and before the HUD - so bloom works on the anti-aliased frame and the HUD is never
//    blended. The scene depth (+0x197EE70) holds only the gun there (the first-person pass cleared it to 1 - 1 = 0 with
//    reversed z, flt_180C32824): gun pixels = depth strictly between 0 and 1, zero motion.
// 4. Reset / off: first frame, size / format change, a frame without a resolve in between, camera jump (> 2 world units or
//    > ~45 degrees in a frame), game tick going backwards (revert / load), cinematics (cinematic globals 0x15F4928 + 10),
//    texture-camera views (0x1996A17), split screen (a view index 1..3 at 0x165C170 / 0x165C394: TAA pauses).
//
// v2 (2026-11-23, user: "lets do per-object motion for v2, bipeds, vehicles, crates, projectiles"):
// 5. OBJECT MOTION (H2TAAObjects.h has the whole story). At the first-person pass entry, right after the camera snapshot,
//    h2taa::ObjectMotion::capture reads every live object's node matrices as the renderer drew them this frame (the
//    engine's interpolated ones) and gives one proxy per node that moved since last frame: its collision-model box, the
//    rigid transform back to last frame, its screen rectangle. Before the resolve, the proxies are drawn (instanced
//    rectangles, HCM_TAAProxy.hlsl) into an RG16F motion target with a D16 priority buffer; the resolve reads it (t4) and
//    object pixels use their own motion instead of the camera's. The resolve also takes each pixel's motion from the
//    nearest-depth pixel of its 3x3 neighbourhood now (closest-depth velocity dilation) so silhouettes follow the object
//    in front. No proxies on a frame whose history was reset. Setting h2TAAObjectMotion (off = v1's camera-only motion).
//    2026-11-24: objects with a collision model are no longer boxes - the boxes took the floor and the wall around the
//    object with them (user's debug-view screenshots). Their collision TRIANGLES are drawn instead (a static vertex buffer
//    per object definition, this frame's node matrices in a structured buffer, one draw per moving object) and a pixel is
//    the object's only where the scene surface is within kTriTolerance of the triangle. Boxes remain for objects with no
//    collision model (weapons, projectiles, equipment) and lose to any triangle.
// ================================================================================================================================

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr uintptr_t kRectRva = 0x7DFCD0;                                  // collision_viewport_scope_rect_compute
	constexpr uintptr_t kMainViewCallRva = 0x7E2315, kMainViewRetRva = 0x7E231A; // in sub_1807E2130 (per player view)
	constexpr uintptr_t kFpCallRva = 0x7E0FC6, kFpRetRva = 0x7E0FCB;           // in sub_1807E0C60 (first-person pass)
	constexpr uintptr_t kCinematicGetterRva = 0x6F4A20;                        // cinematic_globals_get_flag_byte
	constexpr uintptr_t kCinematicGlobalsRva = 0x15F4928;                      // qword: cinematic globals (+10 byte = the flag)
	constexpr uintptr_t kBuildViewIndexRva = 0x165C170;                        // dword: view index sub_1807E2130 is building (stored at 0x7E2172)
	constexpr uintptr_t kDrawViewIndexRva = 0x165C394;                         // dword: view index being drawn (stored at 0x7E317B)
	constexpr uintptr_t kUploadedVPRva = 0x197ED80;                            // VS constants c0..c3 (logged only)
	constexpr uintptr_t kSceneRtvRva = 0x197EE60, kSceneDsvRva = 0x197EE70;
	constexpr uintptr_t kTexCamRva = 0x1996A17;
	constexpr uint32_t kPeStamp = 0x68A0F0F2;
	constexpr int kSplitHoldFrames = 120;
	// the first-person model (see captureFirstPerson)
	constexpr uintptr_t kFrustumBuildRva = 0x7DF7A0;                          // collision_view_frustum_build_from_camera(camera*, float rect[4], frame* out)
	constexpr uintptr_t kFpModelCountRva = 0x165C504, kFpModelsRva = 0x165C508;
	constexpr uintptr_t kFpModelStride = 3340;
	constexpr int kFpMaxModels = 4, kFpMaxBones = 64;
	constexpr size_t kMaxFpNodes = 256;
	constexpr float kFpMaxDistance = 4.f;                                     // wu from the camera: further is not an arm or a gun

	struct Probe { uintptr_t rva; uint8_t bytes[20]; size_t n; };
	const Probe kProbes[] = {
		{ kRectRva, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xBF, 0x41, 0x3A, 0x48, 0x8B, 0xF9 }, 17 },
		{ kMainViewCallRva, { 0xE8, 0xB6, 0xD9, 0xFF, 0xFF, 0x8B, 0x4B, 0x08 }, 8 },   // call 0x7DFCD0
		{ kFpCallRva, { 0xE8, 0x05, 0xED, 0xFF, 0xFF, 0x4C, 0x8D, 0x05 }, 8 },         // call 0x7DFCD0
		{ kCinematicGetterRva, { 0x48, 0x8B, 0x05, 0x01, 0xFF, 0xEF, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x05, 0x0F, 0xB6, 0x40, 0x0A, 0xC3 }, 17 },
		{ 0x7E2172, { 0x89, 0x05, 0xF8, 0x9F, 0xE7, 0x00 }, 6 },                      // mov [0x165C170], eax
		{ 0x7E317B, { 0x89, 0x0D, 0x13, 0x92, 0xE7, 0x00 }, 6 },                      // mov [0x165C394], ecx
		{ kFrustumBuildRva, { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x57, 0x48, 0x8D, 0x68, 0xA8, 0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00 }, 17 },
		{ 0x7E498E, { 0x48, 0x8D, 0x05, 0x73, 0x7B, 0xE7, 0x00 }, 7 },                                      // lea rax, [0x165C508] (the first-person model table)
	};

	// Halton (2, 3), centred: pixels
	constexpr float kHalton[8][2] = {
		{ 0.5f - 0.5f, 1.f / 3.f - 0.5f }, { 0.25f - 0.5f, 2.f / 3.f - 0.5f }, { 0.75f - 0.5f, 1.f / 9.f - 0.5f }, { 0.125f - 0.5f, 4.f / 9.f - 0.5f },
		{ 0.625f - 0.5f, 7.f / 9.f - 0.5f }, { 0.375f - 0.5f, 2.f / 9.f - 0.5f }, { 0.875f - 0.5f, 5.f / 9.f - 0.5f }, { 0.0625f - 0.5f, 8.f / 9.f - 0.5f },
	};

	bool bytesAt(uintptr_t a, const uint8_t* expect, size_t n)
	{
		__try { return memcmp((const void*)a, expect, n) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// the engine's collision_view_frustum_build_from_camera(camera*, float rect[4], frame* out) on a buffer of ours
	bool sehBuildFrame(uintptr_t fn, uintptr_t cam, float* rect, uint8_t* out)
	{
		__try
		{
			reinterpret_cast<int64_t(__fastcall*)(int64_t, float*, int64_t)>(fn)((int64_t)cam, rect, (int64_t)out);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	uint32_t peStamp(uintptr_t mod)
	{
		const int32_t lfanew = mem::i32(mod + 0x3C, 0);
		if (lfanew <= 0 || lfanew > 0x1000) return 0;
		return mem::u32(mod + lfanew + 8, 0);   // IMAGE_NT_HEADERS.FileHeader.TimeDateStamp
	}

	bool comOk(const void* p)
	{
		const uintptr_t a = reinterpret_cast<uintptr_t>(p);
		if (!mem::okp(a)) return false;
		const uintptr_t vt = mem::q(a);
		return mem::okp(vt) && mem::okp(mem::q(vt));
	}

	// ---- 4x4 matrices, row-major, row vector (v' = v * M), in double ----
	using M4 = std::array<double, 16>;

	M4 mul(const M4& a, const M4& b)
	{
		M4 r{};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
			{
				double s = 0;
				for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
				r[i * 4 + j] = s;
			}
		return r;
	}

	bool inverse(const M4& m, M4& out)
	{
		double a[4][8];
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
		for (int c = 0; c < 4; ++c)
		{
			int piv = c;
			for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
			if (std::fabs(a[piv][c]) < 1e-12) return false;
			if (piv != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
			const double d = a[c][c];
			for (int j = 0; j < 8; ++j) a[c][j] /= d;
			for (int r = 0; r < 4; ++r)
			{
				if (r == c) continue;
				const double f = a[r][c];
				if (f != 0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
			}
		}
		for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
		return true;
	}

	// world -> clip of one Halo 2 view: world_to_view {scale, forward, left, up, position} (view = s * (x F + y L + z U) + P)
	// times the projection (row vector), the reversed depth folded in as the Collision Viewer's gameViewProj does
	M4 viewProj(const float w2v[13], const float proj[16], bool reversed, bool rotationOnly)
	{
		const double s = w2v[0];
		const M4 W = {
			s * w2v[1], s * w2v[2], s * w2v[3], 0,
			s * w2v[4], s * w2v[5], s * w2v[6], 0,
			s * w2v[7], s * w2v[8], s * w2v[9], 0,
			rotationOnly ? 0.0 : (double)w2v[10], rotationOnly ? 0.0 : (double)w2v[11], rotationOnly ? 0.0 : (double)w2v[12], 1 };
		M4 P;
		for (int i = 0; i < 16; ++i) P[i] = proj[i];
		M4 vp = mul(W, P);
		if (reversed) for (int i = 0; i < 4; ++i) vp[i * 4 + 2] = vp[i * 4 + 3] - vp[i * 4 + 2];
		return vp;
	}

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

	// Everything the pass changes, saved before and put back after - the same as SMAAPostProcess.cpp's D3D11StateBackup
	// (duplicated, not shared, so SMAA's file stays untouched). Halo 2 caches its D3D state between draws.
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
		static constexpr UINT kVsSrvs = 3;
		ID3D11ShaderResourceView* vsSrvs[kVsSrvs] = {};   // v2: the proxy pass reads its proxies / nodes in the vertex shader

		void save(ID3D11DeviceContext* c)
		{
			c->VSGetShaderResources(0, kVsSrvs, vsSrvs);
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
			c->VSSetShaderResources(0, kVsSrvs, vsSrvs); for (auto& s : vsSrvs) rel(s);
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

	struct Constants { float reproj[16]; float reprojSky[16]; float rect[4]; float tex[4]; float params[4]; float depth[4]; float params2[4];
		float fpInvVP[16]; float fpPrevVP[16]; float fp[4]; };
	static_assert(sizeof(Constants) == 352, "TAA constant buffer layout");
	// HCM_TAA.hlsl's FpNode
	struct FpGpuNode { float origin[4]; float delta[12]; };
	static_assert(sizeof(FpGpuNode) == 64, "FpGpuNode = 4 float4");
	// HCM_TAAProxy.hlsl
	struct ProxyConstants { float invVP[16]; float prevVP[16]; float curVP[16]; float rect[4]; float depth[4]; float params[4]; float draw[4]; };
	static_assert(sizeof(ProxyConstants) == 256, "TAA proxy constant buffer layout");
	constexpr UINT kMaxProxies = 2048;       // boxes a frame (objects with no collision model)
	constexpr UINT kMaxNodes = 8192;         // nodes a frame of the objects drawn as collision triangles
	constexpr UINT kMaxTriDraws = 512;       // ... and how many of those objects
	// How far the scene surface may be from a collision triangle and still be that object (world units; 1 wu = 10 feet).
	// The collision model follows the drawn mesh to a few hundredths; the floor under a biped is further than this from
	// everything but the soles.
	constexpr float kTriTolerance = 0.08f;
	// ... and how far IN FRONT of a collision triangle it may be: parts of the drawn mesh that stick out of the hull
	constexpr float kTriFrontTolerance = 0.32f;
	// the screen speed (pixels a frame) at which a pixel takes the full TAA Motion Weight; ramped from 0 below it. Sub-pixel
	// drift keeps nearly the whole history (Unreal's TSR uses 1 pixel).
	constexpr float kMotionFullSpeed = 2.f;

	void check(HRESULT hr, const char* what)
	{
		if (FAILED(hr)) throw HCMRuntimeException(std::format("Halo 2 TAA: {} failed (hr {:#x})", what, (uint32_t)hr));
	}
}

class H2TAA::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::unique_ptr<ModuleInlineHook> mRectHook;
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mToggleCallback;                           // the Anti Aliasing toggle
	ScopedCallback<eventpp::CallbackList<void(SettingsEnums::PostAAMethod&)>> mMethodCallback;    // the Anti-Aliasing Method
	std::atomic<bool> mWanted{ false };   // toggle on and method TAA, as last acted on
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mDebugCallback;
	ScopedCallback<eventpp::CallbackList<void(float&)>> mOverscanCallback, mOverscanVCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	std::mutex mToggleMutex;          // toggles / MCC state (GUI threads)
	std::mutex mGpuMutex;             // held while the resolve runs; the destructor takes it after unhooking
	bool mVerified = false, mVerifyFailed = false;

	static inline std::atomic<Impl*> sSelf{ nullptr };
	static inline std::atomic<bool> sEnabled{ false };   // the setting (and the build is supported)
	static inline std::atomic<bool> sActive{ false };    // Halo 2 is the game MCC is running
	static inline std::atomic<bool> sFailed{ false };    // a D3D11 failure: TAA off until HCM restarts
	static inline std::atomic<bool> sResetRequested{ true };
	static inline std::atomic<int> sInFlight{ 0 };

	// ---- render-thread state (the hook, the first-person pass tap and the resolve all run on the game's render thread) ----
	struct Jitter { bool valid = false; float jx = 0, jy = 0; int w = 0, h = 0; float before[4]{}, after[4]{}; };
	struct Snapshot
	{
		bool fresh = false, jittered = false, depthOk = false;
		float w2v[13]{}, proj[16]{};
		int16_t vp[4]{};
		h2::Vec3 pos{}, fwd{};
		uint32_t tick = 0;
		float jx = 0, jy = 0;
	};
	Jitter mJit;
	Snapshot mSnap, mPrev;
	// the first-person model: this frame's bones + view (taken in the first-person pass) and last resolved frame's
	struct FpModel { uint32_t tag = 0xFFFFFFFFu; int count = 0; float m[kFpMaxBones * 13]{}; };
	struct FpCapture { bool fresh = false; int models = 0; FpModel model[kFpMaxModels]; float w2v[13]{}; float proj[16]{}; };
	FpCapture mFp, mFpPrev;
	std::vector<FpGpuNode> mFpGpu;
	ComPtr<ID3D11Buffer> mFpBoneBuf; ComPtr<ID3D11ShaderResourceView> mFpBoneSrv;
	bool mLoggedFp = false, mLoggedFpFault = false;
	bool mHistoryValid = false;
	int mFpSinceResolve = 1;      // first-person passes since the last resolve: 0 = the last frame was resolved (jitter allowed)
	int mSplitHold = 0;           // frames left before TAA resumes after a second player's view was seen
	uint32_t mJitterIndex = 0;
	uint64_t mLastDepthCount = 0;
	uint64_t mResolveCount = 0;
	uint64_t mEnabledAtMs = 0;
	// once-per-session logs
	bool mLoggedJitter = false, mLoggedJitterMiss = false, mLoggedVP = false, mLoggedNoDepth = false, mLoggedNoResolve = false;
	bool mLoggedSplit = false, mLoggedViewIndex = false, mLoggedFirstResolve = false, mLoggedSkip[8]{};

	// ---- D3D11 (made on first use, under mGpuMutex) ----
	ID3D11Device* mDevice = nullptr;   // not owned: the one the resources were made on
	ComPtr<ID3D11VertexShader> mVS; ComPtr<ID3D11PixelShader> mPS;
	ComPtr<ID3D11SamplerState> mLinear;
	ComPtr<ID3D11Buffer> mCB;
	ComPtr<ID3D11RasterizerState> mRS; ComPtr<ID3D11DepthStencilState> mDSS; ComPtr<ID3D11BlendState> mBS;
	UINT mW = 0, mH = 0; DXGI_FORMAT mFmt = DXGI_FORMAT_UNKNOWN, mViewFmt = DXGI_FORMAT_UNKNOWN;
	ComPtr<ID3D11Texture2D> mCur; ComPtr<ID3D11ShaderResourceView> mCurSrv;
	ComPtr<ID3D11Texture2D> mHist[2]; ComPtr<ID3D11RenderTargetView> mHistRtv[2]; ComPtr<ID3D11ShaderResourceView> mHistSrv[2];
	int mHistIndex = 0;
	ComPtr<ID3D11Texture2D> mFpSource;   // the scene depth texture the gun mask was set up for
	ComPtr<ID3D11Texture2D> mFpCopy;     // its copy when it cannot be read directly
	ComPtr<ID3D11ShaderResourceView> mFpSrv;

	// ---- v2: object motion ----
	static inline std::atomic<bool> sObjectsResetRequested{ true };   // TAA was off / paused: last frame's poses are stale
	static inline std::atomic<bool> sLogObjectStats{ false };         // the debug view was switched on: one stats line
	h2taa::ObjectMotion mObjects;                 // render thread
	h2taa::ObjectFrame mObjFrame;                 // of the view in mSnap (filled at the first-person pass, drawn at the resolve)
	bool mProxiesFresh = false;
	ComPtr<ID3D11VertexShader> mTriVS; ComPtr<ID3D11PixelShader> mTriPS; ComPtr<ID3D11InputLayout> mTriLayout;
	ComPtr<ID3D11Buffer> mNodeBuf; ComPtr<ID3D11ShaderResourceView> mNodeSrv;
	// the pass before the triangles: the farthest collision face at each pixel (view depth, blend MAX) - what lies between a
	// triangle and that face is inside the object's hull
	ComPtr<ID3D11PixelShader> mTriExtentPS; ComPtr<ID3D11BlendState> mMaxBS;
	ComPtr<ID3D11Texture2D> mExtTex; ComPtr<ID3D11RenderTargetView> mExtRtv; ComPtr<ID3D11ShaderResourceView> mExtSrv;
	struct MeshVB { ComPtr<ID3D11Buffer> vb; UINT count = 0; };
	std::unordered_map<uint32_t, MeshVB> mMeshes;   // collision triangles by object definition (an entry with no buffer: could not be made)
	uint64_t mMeshGeneration = 0;                   // h2taa::ObjectMotion::generation() the buffers were made under
	bool mLoggedFirstMesh = false;
	uint64_t mLastCaptureMs = 0;
	long long mCaptureUs = 0, mCaptureUsMax = 0;     // how long reading the objects' poses took (this frame / worst so far)
	double mLastResolveSec = 0;                    // steady clock at the last resolve tap (the camera-cut test is per second)
	bool mObjectsFailed = false;                  // the proxy pass could not be set up: camera motion only (logged once)
	bool mLoggedFirstProxies = false;
	ComPtr<ID3D11VertexShader> mProxyVS; ComPtr<ID3D11PixelShader> mProxyPS;
	ComPtr<ID3D11Buffer> mProxyCB, mProxyBuf;
	ComPtr<ID3D11ShaderResourceView> mProxySrv;
	ComPtr<ID3D11DepthStencilState> mProxyDSS;
	ComPtr<ID3D11Texture2D> mObjTex, mObjDepth;
	ComPtr<ID3D11RenderTargetView> mObjRtv; ComPtr<ID3D11ShaderResourceView> mObjSrv; ComPtr<ID3D11DepthStencilView> mObjDsv;

	// ---- the flight recorder (user 2026-11-26: rare "the screen edges stick for a few frames, then jump; a bright area stays
	// too long", only the first time through a spot). Every main-view resolve tap is recorded in a ring; when something
	// abnormal happens (a slow frame, the game tick held, the camera held then jumping, a history reset in play, TAA itself
	// slow) the frames around it are written to the log in one block, so a rare glitch leaves hard data behind.
	enum RecStatus : uint8_t { kRecResolved, kRecReset, kRecNotResolved, kRecTexCam, kRecSplit, kRecCinematic, kRecNoDepth, kRecBusy, kRecGfxError, kRecCount };
	struct FlightFrame
	{
		double t = 0;               // steady clock (s)
		float dtMs = 0;             // since the previous recorded frame
		uint32_t tick = 0;          // game tick
		float moveWu = 0, turnDeg = 0;   // camera since the previous recorded frame
		uint8_t status = kRecResolved;
		char resetWhy = '-';        // R requested, H no valid history, F a frame without a resolve, C camera cut, T tick jump
		bool jittered = false;
		int fpSince = 0;            // first-person passes since the last resolve (1 = normal)
		uint32_t objBoxes = 0, objDraws = 0;
		float taaUs = 0, captureUs = 0;   // CPU time of the resolve call / of reading the object poses
	};
	static constexpr int kRecSize = 1024, kRecBefore = 150, kRecAfter = 40, kRecMaxDumps = 40;
	std::array<FlightFrame, kRecSize> mRec{};
	uint64_t mRecCount = 0;
	h2::Vec3 mRecPos{}, mRecFwd{};
	bool mRecHavePose = false, mRecLoggedOn = false;
	int mRecFrozen = 0;                  // frames in a row the camera has not changed at all
	double mRecFrozenSince = 0;
	float mRecMoveBefore = 0, mRecTurnBefore = 0, mRecLastMove = 0, mRecLastTurn = 0;
	uint32_t mRecTick = 0; double mRecTickSince = 0; int mRecTickFrames = 0;
	bool mRecPending = false; uint64_t mRecAnomalyAt = 0, mRecDumpAt = 0; std::string mRecReasons;
	double mRecLastDumpT = -1e9; int mRecDumps = 0;

	static const char* recStatusText(uint8_t s)
	{
		static const char* k[kRecCount] = { "ok", "RESET", "not-resolved", "tex-cam", "split", "cinematic", "no-depth", "busy", "gfx-error" };
		return s < kRecCount ? k[s] : "?";
	}

	void recFlag(const std::string& why, double t)
	{
		if (mRecPending)
		{
			if (mRecReasons.size() < 600) mRecReasons += "; " + why;
			return;
		}
		if (t - mRecLastDumpT < 3.0 || mRecDumps >= kRecMaxDumps) return;
		mRecPending = true;
		mRecAnomalyAt = mRecCount - 1;
		mRecDumpAt = mRecCount + kRecAfter;
		mRecReasons = why;
	}

	void recDump()
	{
		mRecPending = false;
		++mRecDumps;
		const uint64_t first = (std::max)({ mRecAnomalyAt >= (uint64_t)kRecBefore ? mRecAnomalyAt - kRecBefore : 0ull,
			mRecCount > (uint64_t)kRecSize ? mRecCount - kRecSize + 1 : 0ull });
		const double t0 = mRec[mRecAnomalyAt % kRecSize].t;
		mRecLastDumpT = mRec[(mRecCount - 1) % kRecSize].t;
		std::string out = std::format("Halo 2 TAA recorder #{}: {} - {} frames around it (time in ms from the frame marked >>; "
			"columns: time, frame ms, game tick, camera move wu, turn deg, TAA status, reset why, passes since resolve, jittered, "
			"objects sent boxes/triangle draws, TAA CPU us, pose read us)", mRecDumps, mRecReasons, mRecCount - first);
		for (uint64_t i = first; i < mRecCount; ++i)
		{
			const FlightFrame& f = mRec[i % kRecSize];
			out += std::format("\n{}{:9.1f} {:7.2f} {:8} {:8.4f} {:7.3f} {:>12} {} {} {} {:4}/{:<4} {:7.0f} {:6.0f}",
				i == mRecAnomalyAt ? ">>" : "  ", (f.t - t0) * 1000.0, f.dtMs, f.tick, f.moveWu, f.turnDeg, recStatusText(f.status),
				f.resetWhy, f.fpSince, f.jittered ? 'j' : '.', f.objBoxes, f.objDraws, f.taaUs, f.captureUs);
		}
		PLOG_INFO << out;
		if (mRecDumps == kRecMaxDumps) PLOG_INFO << "Halo 2 TAA recorder: " << kRecMaxDumps << " records written this session - no more until HCM restarts";
	}

	// OFF since 2026-11-27 (user: "turn off TAA logging" before the release). Flip to true to record again.
	static constexpr bool kRecorderOn = false;

	void recordFrame(const Snapshot& s, uint8_t status, char resetWhy, int fpSince, float taaUs)
	{
		if constexpr (!kRecorderOn) return;
		const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		FlightFrame f;
		f.t = t;
		f.tick = s.tick;
		f.status = status; f.resetWhy = resetWhy; f.fpSince = fpSince;
		f.jittered = s.jittered;
		f.objBoxes = (uint32_t)mObjFrame.boxes.size(); f.objDraws = (uint32_t)mObjFrame.draws.size();
		f.taaUs = taaUs; f.captureUs = (float)mCaptureUs;
		const bool havePrev = mRecCount > 0;
		const FlightFrame* prev = havePrev ? &mRec[(mRecCount - 1) % kRecSize] : nullptr;
		f.dtMs = prev ? (float)((t - prev->t) * 1000.0) : 0.f;
		bool samePose = false;
		if (mRecHavePose)
		{
			const float dx = s.pos.x - mRecPos.x, dy = s.pos.y - mRecPos.y, dz = s.pos.z - mRecPos.z;
			f.moveWu = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float d = std::clamp(s.fwd.x * mRecFwd.x + s.fwd.y * mRecFwd.y + s.fwd.z * mRecFwd.z, -1.f, 1.f);
			f.turnDeg = std::acos(d) * 57.29578f;
			samePose = s.pos.x == mRecPos.x && s.pos.y == mRecPos.y && s.pos.z == mRecPos.z
				&& s.fwd.x == mRecFwd.x && s.fwd.y == mRecFwd.y && s.fwd.z == mRecFwd.z;
		}
		mRecPos = s.pos; mRecFwd = s.fwd; mRecHavePose = true;
		mRec[mRecCount % kRecSize] = f;
		++mRecCount;
		if (!mRecLoggedOn)
		{
			mRecLoggedOn = true;
			PLOG_INFO << "Halo 2 TAA recorder: on - slow frames, a held game tick, the camera held then jumping, history resets in play and slow TAA work are written to this log with the frames around them";
		}

		// a slow frame: more than 2.5x the median of the last 60 (and over 40 ms); longer than 3 s = a pause / alt-tab
		if (mRecCount > 61 && f.dtMs < 3000.f)
		{
			std::array<float, 60> dts;
			for (int i = 0; i < 60; ++i) dts[i] = mRec[(mRecCount - 2 - i) % kRecSize].dtMs;
			std::nth_element(dts.begin(), dts.begin() + 30, dts.end());
			const float median = dts[30];
			if (f.dtMs > (std::max)(40.f, 2.5f * median)) recFlag(std::format("slow frame {:.1f} ms (median {:.1f} ms)", f.dtMs, median), t);
		}
		// the game tick held while frames kept coming (the game's own hitch; a pause is longer than 3 s)
		if (f.tick != mRecTick)
		{
			const double held = t - mRecTickSince;
			if (mRecTick != 0 && f.tick == mRecTick + 1 && held > 0.1 && held < 3.0 && mRecTickFrames >= 3)
				recFlag(std::format("game tick {} held {:.0f} ms over {} frames", mRecTick, held * 1000.0, mRecTickFrames), t);
			mRecTick = f.tick; mRecTickSince = t; mRecTickFrames = 0;
		}
		else ++mRecTickFrames;
		// the camera did not change at all for a while, then moved more than it had been moving per frame (a catch-up)
		if (samePose)
		{
			if (mRecFrozen == 0) { mRecFrozenSince = prev ? prev->t : t; mRecMoveBefore = mRecLastMove; mRecTurnBefore = mRecLastTurn; }
			++mRecFrozen;
		}
		else if (mRecHavePose && havePrev)
		{
			const double held = t - mRecFrozenSince;
			if (mRecFrozen >= 2 && held > 0.08 && held < 2.0 && (mRecMoveBefore > 0.01f || mRecTurnBefore > 0.3f)
				&& (f.moveWu > 1.8f * mRecMoveBefore + 0.02f || f.turnDeg > 1.8f * mRecTurnBefore + 0.5f))
				recFlag(std::format("camera held {} frames ({:.0f} ms) then jumped {:.3f} wu / {:.2f} deg (was {:.3f} wu / {:.2f} deg a frame)",
					mRecFrozen, held * 1000.0, f.moveWu, f.turnDeg, mRecMoveBefore, mRecTurnBefore), t);
			mRecFrozen = 0;
			mRecLastMove = f.moveWu; mRecLastTurn = f.turnDeg;
		}
		// history dropped during play, a frame not resolved, TAA's own CPU work slow
		if (status == kRecReset && (resetWhy == 'F' || resetWhy == 'C' || resetWhy == 'H') && prev && prev->status == kRecResolved)
			recFlag(std::format("history reset ({})", resetWhy == 'F' ? "a frame went by without a resolve" : resetWhy == 'C' ? "camera cut" : "no valid history"), t);
		if ((status == kRecNotResolved || status == kRecNoDepth || status == kRecBusy) && prev && prev->status <= kRecReset)
			recFlag(std::format("frame not resolved ({})", recStatusText(status)), t);
		if (taaUs > 3000.f) recFlag(std::format("TAA CPU work {:.0f} us", taaUs), t);
		if (mCaptureUs > 3000) recFlag(std::format("object pose read {} us", mCaptureUs), t);

		if (mRecPending && mRecCount >= mRecDumpAt) recDump();
	}

	void say(const std::string& text)
	{
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(text); }
		catch (HCMRuntimeException&) {}
	}

	static bool inCinematic(uintptr_t b)
	{
		const uintptr_t g = mem::q(b + kCinematicGlobalsRva);
		return mem::okp(g) && mem::u8(g + 10, 0) != 0;
	}

	static uint32_t gameTick(uintptr_t b)
	{
		const uintptr_t gt = mem::q(b + h2::rva::GAME_TIME);
		return mem::okp(gt) ? mem::u32(gt + 8, 0) : 0;
	}

	// ================================ the jitter hook ================================
	static char __fastcall rectDetour(int64_t cam, float* rect)
	{
		++sInFlight;
		char r = 0;
		Impl* self = sSelf.load();
		if (self)
		{
			r = self->mRectHook->getInlineHook().call<char, int64_t, float*>(cam, rect);
			const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
			const uintptr_t b = h2::base();
			if (b && rect && cam)
			{
				const bool main = ret == b + kMainViewRetRva, fp = ret == b + kFpRetRva;
				if (main || fp)
				{
					// TAA overscan (H2Overscan.h): while the targets are overscanned, the main view and the gun are drawn
					// that much wider - whether TAA is on or not (the targets follow at the next video reset), and before
					// the jitter, which then works in the overscanned view's pixels
					int16_t vr[4];
					if (h2os::gOverscan.load(std::memory_order_acquire) && mem::copy(vr, (uintptr_t)cam + 0x30, sizeof(vr))
						&& std::isfinite(rect[0] + rect[1] + rect[2] + rect[3]))
						h2os::widen(rect, vr[3] - vr[1], vr[2] - vr[0]);
					if (main) self->pollOverscan();
				}
				if (main) self->onMainViewRect(b, (uintptr_t)cam, rect);
				else if (fp) self->onFirstPersonRect(b, (uintptr_t)cam, rect);
			}
		}
		--sInFlight;
		return r;
	}

	void onMainViewRect(uintptr_t b, uintptr_t cam, float* rect)
	{
		mJit.valid = false;
		if (!sEnabled.load() || !sActive.load() || sFailed.load()) return;
		const int32_t viewIdx = mem::i32(b + kBuildViewIndexRva, -1);
		if (!mLoggedViewIndex) { mLoggedViewIndex = true; PLOG_INFO << "Halo 2 TAA: first main view, view index " << viewIdx; }
		if (viewIdx >= 1 && viewIdx <= 3) { mSplitHold = kSplitHoldFrames; return; }   // a second player's view: split screen
		// watchdog: the jitter hook runs but nothing is resolving
		if (mResolveCount == 0 && !mLoggedNoResolve && mEnabledAtMs && GetTickCount64() - mEnabledAtMs > 5000)
		{
			mLoggedNoResolve = true;
			PLOG_ERROR << "Halo 2 TAA: no frame resolved 5 s after turning it on - the resolve runs from the Collision Viewer's Halo 2 render hooks "
				"(look for '[CollisionViewer] render hooks armed' / 'render-pass entry bytes differ' above); no jitter is applied meanwhile";
		}
		if (mSplitHold > 0 || mFpSinceResolve != 0 || inCinematic(b)) return;
		int16_t vr[4];
		if (!mem::copy(vr, cam + 0x30, sizeof(vr))) return;
		const int w = vr[3] - vr[1], h = vr[2] - vr[0];
		if (w < 16 || h < 16) return;
		if (!(rect[1] - rect[0] != 0.f) || !(rect[3] - rect[2] != 0.f) || !std::isfinite(rect[0] + rect[1] + rect[2] + rect[3])) return;
		Jitter j;
		j.jx = kHalton[mJitterIndex & 7][0];
		j.jy = kHalton[mJitterIndex & 7][1];
		j.w = w; j.h = h;
		memcpy(j.before, rect, sizeof(j.before));
		const float dx = -j.jx * (rect[1] - rect[0]) / (float)w;
		const float dy = j.jy * (rect[3] - rect[2]) / (float)h;
		rect[0] += dx; rect[1] += dx; rect[2] += dy; rect[3] += dy;
		memcpy(j.after, rect, sizeof(j.after));
		j.valid = true;
		mJit = j;
	}

	// the first-person pass rebuilds the projection for the gun: the same jitter (in this camera's pixels)
	void onFirstPersonRect(uintptr_t b, uintptr_t cam, float* rect)
	{
		if (!sEnabled.load() || !sActive.load() || sFailed.load()) return;
		captureFirstPerson(b, cam, rect);   // with the rect as the engine made it: un-jittered
		if (!mJit.valid) return;
		int16_t vr[4];
		if (!mem::copy(vr, cam + 0x30, sizeof(vr))) return;
		const int w = vr[3] - vr[1], h = vr[2] - vr[0];
		if (w < 16 || h < 16 || !std::isfinite(rect[0] + rect[1] + rect[2] + rect[3])) return;
		const float dx = -mJit.jx * (rect[1] - rect[0]) / (float)w;
		const float dy = mJit.jy * (rect[3] - rect[2]) / (float)h;
		rect[0] += dx; rect[1] += dx; rect[2] += dy; rect[3] += dy;
	}

	// The first-person model of this frame (2026-11-24, user: "look at first person model implementation"). Called inside
	// the first-person pass, where the camera (halo2+0x1996A28) already has the gun's field of view and clip planes:
	//  - the bones: sub_1807E5430 filled the table at halo2+0x165C508 for this frame (count +0x165C504, up to 4 models of
	//    3340 bytes: render model tag +0, object +4, flags +8, real_matrix4x3[64] +12, WORLD space - built from the camera's
	//    position and axes by sub_1808181F0; sub_1807E4970's first-person branch hands exactly these to the renderer);
	//  - the view: the engine's own collision_view_frustum_build_from_camera (halo2+0x7DF7A0, a pure function) on the same
	//    camera and the un-jittered rect, into a buffer of ours - the same layout as the frame at +0x1996B10.
	void captureFirstPerson(uintptr_t b, uintptr_t cam, const float* rect)
	{
		mFp.fresh = false;
		if (!mSnap.fresh || !std::isfinite(rect[0] + rect[1] + rect[2] + rect[3])) return;   // no main-view snapshot this frame
		const int32_t n = mem::i32(b + kFpModelCountRva, 0);
		if (n <= 0 || n > kFpMaxModels) return;   // no first-person model this frame (third person, zoomed, a vehicle)
		h2::TagPools pools;
		if (!h2::tagPools(pools)) return;
		int bones = 0;
		for (int i = 0; i < n; ++i)
		{
			FpModel& m = mFp.model[i];
			m.count = 0;
			const uintptr_t e = b + kFpModelsRva + (uintptr_t)i * kFpModelStride;
			m.tag = mem::u32(e, 0xFFFFFFFFu);
			if (m.tag == 0xFFFFFFFFu) continue;
			const uintptr_t mode = h2::tagData((uint16_t)(m.tag & 0xFFFF), pools);
			if (!mem::okp(mode)) continue;
			const int cnt = std::clamp(mem::i32(mode + 72, 0), 0, kFpMaxBones);   // the render model's node count, as the renderer caps it
			if (cnt > 0 && mem::copy(m.m, e + 12, (size_t)cnt * 52)) m.count = cnt;
			bones += m.count;
		}
		mFp.models = n;
		if (!bones) return;
		alignas(16) float r[4] = { rect[0], rect[1], rect[2], rect[3] };
		alignas(16) uint8_t frame[0x140] = {};
		if (!sehBuildFrame(b + kFrustumBuildRva, cam, r, frame))
		{
			if (!mLoggedFpFault) { mLoggedFpFault = true; PLOG_ERROR << "Halo 2 TAA: building the first-person view faulted - the gun keeps zero motion"; }
			return;
		}
		memcpy(mFp.w2v, frame, sizeof(mFp.w2v));
		memcpy(mFp.proj, frame + 0x78, sizeof(mFp.proj));
		for (float v : mFp.w2v) if (!std::isfinite(v)) return;
		for (float v : mFp.proj) if (!std::isfinite(v)) return;
		mFp.fresh = true;
	}

	// ================================ the first-person pass entry ================================
	static void fpTap()
	{
		++sInFlight;
		if (Impl* self = sSelf.load())
		{
			try { self->onFirstPersonPass(); }
			catch (...) { PLOG_ERROR << "Halo 2 TAA: exception in the first-person pass tap"; }
		}
		--sInFlight;
	}

	void onFirstPersonPass()
	{
		if (!sEnabled.load() || sFailed.load()) return;
		const uintptr_t b = h2::base();
		if (!b) return;
		h2::GameFrame f;
		if (!h2::readFrame(f) || f.textureCamera) return;
		const int32_t viewIdx = mem::i32(b + kDrawViewIndexRva, -1);
		if (viewIdx >= 1 && viewIdx <= 3)
		{
			if (!mLoggedSplit) { mLoggedSplit = true; PLOG_INFO << "Halo 2 TAA: split screen (view index " << viewIdx << ") - TAA pauses while there is more than one view"; }
			mSplitHold = kSplitHoldFrames;
			mSnap.fresh = false;
			return;
		}
		if (mSplitHold > 0) --mSplitHold;
		if (mFpSinceResolve < 1000000) ++mFpSinceResolve;
		Snapshot s;
		memcpy(s.w2v, f.w2v, sizeof(s.w2v));
		memcpy(s.proj, f.proj, sizeof(s.proj));
		memcpy(s.vp, f.vp, sizeof(s.vp));
		s.pos = f.pos; s.fwd = f.fwd;
		s.tick = gameTick(b);
		const uint64_t dc = GameDepthTap::captureCount();
		s.depthOk = dc != mLastDepthCount;   // the depth tap copied this view's world depth just before this call
		mLastDepthCount = dc;
		// take the jitter back out of the projection (P[2][0] / P[2][1] = the window centre terms)
		if (mJit.valid)
		{
			const Jitter& j = mJit;
			const float p20j = (j.after[0] + j.after[1]) / (j.after[1] - j.after[0]), p21j = (j.after[2] + j.after[3]) / (j.after[3] - j.after[2]);
			const float p20u = (j.before[0] + j.before[1]) / (j.before[1] - j.before[0]), p21u = (j.before[2] + j.before[3]) / (j.before[3] - j.before[2]);
			const bool carried = std::fabs(f.proj[8] - p20j) < 1e-4f && std::fabs(f.proj[9] - p21j) < 1e-4f;
			if (!mLoggedJitter)
			{
				mLoggedJitter = true;
				PLOG_INFO << std::format("Halo 2 TAA jitter (first frame): sample ({:.4f}, {:.4f}) px on a {}x{} view, rect l r b t {{{:.6f} {:.6f} {:.6f} {:.6f}}} -> {{{:.6f} {:.6f} {:.6f} {:.6f}}}",
					j.jx, j.jy, j.w, j.h, j.before[0], j.before[1], j.before[2], j.before[3], j.after[0], j.after[1], j.after[2], j.after[3]);
				PLOG_INFO << std::format("Halo 2 TAA jitter: projection P20 / P21 (halo2+0x1996BA8 / +0x1996BAC) read at the first-person pass = {:.6f} / {:.6f}; "
					"expected jittered {:.6f} / {:.6f} (un-jittered {:.6f} / {:.6f}) -> {}. Convention: picture moved by (+jx right, +jy down) pixels",
					f.proj[8], f.proj[9], p20j, p21j, p20u, p21u, carried ? "the jitter reached the projection" : "MISMATCH (the projection does not carry the jitter: used as is)");
			}
			if (carried)
			{
				s.proj[8] = p20u; s.proj[9] = p21u;
				s.jittered = true; s.jx = j.jx; s.jy = j.jy;
			}
			else if (!mLoggedJitterMiss && mLoggedJitter)
			{
				mLoggedJitterMiss = true;
				PLOG_ERROR << std::format("Halo 2 TAA: the projection did not carry the jitter (P20 / P21 {:.6f} / {:.6f}, expected {:.6f} / {:.6f}) - that frame is treated as un-jittered",
					f.proj[8], f.proj[9], p20j, p21j);
			}
		}
		if (!mLoggedVP)
		{
			mLoggedVP = true;
			const M4 vp = viewProj(s.w2v, s.proj, false, false);
			float up[16]{};
			const bool haveUp = mem::copy(up, b + kUploadedVPRva, sizeof(up));
			double maxDiff = 0;
			// the uploaded c0..c3: row i = the coefficients of clip component i = column i of the row-vector matrix
			if (haveUp) for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) maxDiff = (std::max)(maxDiff, std::fabs((double)up[i * 4 + k] - vp[k * 4 + i]));
			PLOG_INFO << std::format("Halo 2 TAA: first view-projection (un-jittered, standard z) rows {{{:.5f} {:.5f} {:.5f} {:.5f}}} {{{:.5f} {:.5f} {:.5f} {:.5f}}} {{{:.5f} {:.5f} {:.5f} {:.5f}}} {{{:.4f} {:.4f} {:.4f} {:.4f}}}, "
				"near {} far {}, view rect t l b r {} {} {} {}, camera ({:.3f}, {:.3f}, {:.3f})",
				vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10], vp[11], vp[12], vp[13], vp[14], vp[15],
				f.nearClip, f.farClip, s.vp[0], s.vp[1], s.vp[2], s.vp[3], s.pos.x, s.pos.y, s.pos.z);
			if (haveUp)
				PLOG_INFO << std::format("Halo 2 TAA: uploaded VS constants c0..c3 (halo2+0x197ED80) vs that matrix (transposed): max difference {:.6f} ({}; jitter / the gun's own projection account for small ones)",
					maxDiff, maxDiff < 1e-3 ? "same layout" : "different");
		}
		s.fresh = true;
		mSnap = s;

		// v2: the objects' poses of this frame against last frame's -> proxies for the resolve
		mProxiesFresh = false;
		mObjFrame.clear();
		auto settings = settingsWeak.lock();
		if (settings && settings->h2TAAObjectMotion->GetValue() && !mObjectsFailed)
		{
			const uint64_t now = GetTickCount64();
			if (sObjectsResetRequested.exchange(false) || now - mLastCaptureMs > 250) mObjects.reset();   // TAA was off / paused: no "last frame"
			mLastCaptureMs = now;
			const M4 vp = viewProj(s.w2v, s.proj, false, false);   // un-jittered; x, y, w only
			const float cam[3] = { s.pos.x, s.pos.y, s.pos.z };
			const auto t0 = std::chrono::steady_clock::now();
			mObjects.capture(vp.data(), cam, mObjFrame, kMaxProxies, kMaxNodes, kMaxTriDraws);
			mCaptureUs = (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
			mCaptureUsMax = (std::max)(mCaptureUsMax, mCaptureUs);
			mProxiesFresh = true;
		}
		else sObjectsResetRequested = true;
	}

	// ================================ the resolve (sub_180951EC0 entry) ================================
	static void resolveTap()
	{
		++sInFlight;
		if (Impl* self = sSelf.load())
		{
			try { self->onBeforeResolve(); }
			catch (...) { PLOG_ERROR << "Halo 2 TAA: exception in the resolve"; }
		}
		--sInFlight;
	}

	void skipOnce(int why, const char* text)
	{
		if (why >= 0 && why < 8 && !mLoggedSkip[why]) { mLoggedSkip[why] = true; PLOG_INFO << "Halo 2 TAA: frame not resolved: " << text << " (logged once)"; }
	}

	void onBeforeResolve()
	{
		const Snapshot s = mSnap;
		mSnap.fresh = false;
		mJit.valid = false;   // this view's jitter is used up either way
		if (!sEnabled.load() || sFailed.load()) { mHistoryValid = false; return; }
		const uintptr_t b = h2::base();
		if (!b) return;
		if (!s.fresh) return;   // no snapshot for this view (texture camera, split screen, the pass did not run)
		// the flight recorder: one entry per main-view tap, written whichever way this returns
		struct RecGuard
		{
			Impl* self; const Snapshot& s; uint8_t status = kRecNotResolved; char why = '-'; int fpSince; float us = 0;
			~RecGuard() { try { self->recordFrame(s, status, why, fpSince, us); } catch (...) {} }
		} rec{ this, s, kRecNotResolved, '-', mFpSinceResolve };
		if (mem::u8(b + kTexCamRva, 0) != 0) { rec.status = kRecTexCam; skipOnce(0, "texture-camera view"); return; }
		if (mSplitHold > 0) { rec.status = kRecSplit; mHistoryValid = false; skipOnce(1, "split screen"); return; }
		if (inCinematic(b)) { rec.status = kRecCinematic; mHistoryValid = false; skipOnce(2, "cinematic"); return; }
		if (!s.depthOk)
		{
			rec.status = kRecNoDepth;
			mHistoryValid = false;
			if (!mLoggedNoDepth) { mLoggedNoDepth = true; PLOG_ERROR << "Halo 2 TAA: no world depth copy for this view (GameDepthTap) - not resolved (logged once)"; }
			return;
		}
		std::unique_lock lk(mGpuMutex, std::try_to_lock);
		if (!lk.owns_lock()) { rec.status = kRecBusy; return; }
		// history reset
		const bool requested = sResetRequested.exchange(false);
		bool reset = requested || !mHistoryValid || mFpSinceResolve != 1;
		rec.why = requested ? 'R' : !mHistoryValid ? 'H' : mFpSinceResolve != 1 ? 'F' : '-';
		const double nowSec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const double frameSec = std::clamp(nowSec - mLastResolveSec, 0.002, 0.1);   // since the last resolve tap
		mLastResolveSec = nowSec;
		if (!reset)
		{
			const float dx = s.pos.x - mPrev.pos.x, dy = s.pos.y - mPrev.pos.y, dz = s.pos.z - mPrev.pos.z;
			const float dot = s.fwd.x * mPrev.fwd.x + s.fwd.y * mPrev.fwd.y + s.fwd.z * mPrev.fwd.z;
			// A camera cut / teleport - not speed: being launched at the engine's 100 wu/s is 3.3 wu a frame at 30 fps, and the
			// reprojection is exact at any speed. Only more than 400 wu/s (and more than 2 wu) in a frame drops the history.
			const float jump = (std::max)(2.f, (float)(400.0 * frameSec));
			if (dx * dx + dy * dy + dz * dz > jump * jump || dot < 0.7f || !std::isfinite(dot)) { reset = true; rec.why = 'C'; }
			if (s.tick < mPrev.tick || s.tick > mPrev.tick + 600) { reset = true; rec.why = 'T'; }           // revert / load
		}
		try
		{
			const auto rt0 = std::chrono::steady_clock::now();
			const int r = runResolve(s, reset);
			rec.us = (float)std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - rt0).count();
			rec.status = r > 0 ? (reset ? kRecReset : kRecResolved) : kRecNotResolved;
			if (r > 0)
			{
				mFpSinceResolve = 0;
				++mJitterIndex;
				++mResolveCount;
				mPrev = s;
				mHistoryValid = true;
				mFpPrev = mFp;   // (not fresh when the first-person model was not drawn this frame)
			}
			else { mHistoryValid = false; mFpPrev.fresh = false; }
			mFp.fresh = false;
		}
		catch (HCMRuntimeException& ex)
		{
			rec.status = kRecGfxError;
			sFailed = true;
			mHistoryValid = false;
			releaseAll();
			PLOG_ERROR << ex.what() << " - Halo 2 TAA is off until HCM restarts";
			say("Halo 2 TAA: a graphics error turned it off (see the HCM log) - it stays off until HCM restarts");
		}
	}

	void releaseSized()
	{
		mCur.Reset(); mCurSrv.Reset();
		for (int i = 0; i < 2; ++i) { mHist[i].Reset(); mHistRtv[i].Reset(); mHistSrv[i].Reset(); }
		mW = mH = 0; mFmt = mViewFmt = DXGI_FORMAT_UNKNOWN;
		mFpSource.Reset(); mFpCopy.Reset(); mFpSrv.Reset();
		mObjTex.Reset(); mObjDepth.Reset(); mObjRtv.Reset(); mObjSrv.Reset(); mObjDsv.Reset(); mExtTex.Reset(); mExtRtv.Reset(); mExtSrv.Reset();
	}

	void releaseAll()
	{
		releaseSized();
		mVS.Reset(); mPS.Reset(); mLinear.Reset(); mCB.Reset(); mRS.Reset(); mDSS.Reset(); mBS.Reset(); mFpBoneBuf.Reset(); mFpBoneSrv.Reset();
		releaseProxyStatic();
		mDevice = nullptr;
	}

	void releaseProxyStatic()
	{
		mProxyVS.Reset(); mProxyPS.Reset(); mProxyCB.Reset(); mProxyBuf.Reset(); mProxySrv.Reset(); mProxyDSS.Reset();
		mTriVS.Reset(); mTriPS.Reset(); mTriLayout.Reset(); mNodeBuf.Reset(); mNodeSrv.Reset(); mTriExtentPS.Reset(); mMaxBS.Reset();
		mMeshes.clear(); mMeshGeneration = 0;
	}

	// the collision triangles of one object definition as a vertex buffer (made once per map); nullptr = none
	const MeshVB* meshBuffer(ID3D11Device* dev, uint32_t def)
	{
		if (mObjects.generation() != mMeshGeneration) { mMeshes.clear(); mMeshGeneration = mObjects.generation(); }
		auto it = mMeshes.find(def);
		if (it == mMeshes.end())
		{
			MeshVB m;
			const std::vector<h2taa::TriVertex>* verts = mObjects.mesh(def);
			if (verts && !verts->empty())
			{
				D3D11_BUFFER_DESC bd = {};
				bd.ByteWidth = (UINT)(verts->size() * sizeof(h2taa::TriVertex)); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				D3D11_SUBRESOURCE_DATA init = {};
				init.pSysMem = verts->data();
				if (SUCCEEDED(dev->CreateBuffer(&bd, &init, &m.vb))) m.count = (UINT)verts->size();
				else { m.vb.Reset(); PLOG_ERROR << "Halo 2 TAA objects: no vertex buffer for definition " << std::hex << def << " (" << std::dec << verts->size() << " vertices) - that object keeps the camera's motion"; }
			}
			it = mMeshes.emplace(def, std::move(m)).first;
		}
		return it->second.vb ? &it->second : nullptr;
	}

	// v2: the object motion pass's resources. A failure here only turns the object motion off (camera motion as v1).
	void makeProxyStatic(ID3D11Device* dev)
	{
		if (mObjectsFailed || mProxyVS) return;
		try
		{
			check(dev->CreateVertexShader(TAAShaders.proxyVS.data, TAAShaders.proxyVS.size, nullptr, &mProxyVS), "proxy vertex shader");
			check(dev->CreatePixelShader(TAAShaders.proxyPS.data, TAAShaders.proxyPS.size, nullptr, &mProxyPS), "proxy pixel shader");
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(ProxyConstants); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			check(dev->CreateBuffer(&bd, nullptr, &mProxyCB), "proxy constant buffer");
			D3D11_BUFFER_DESC sb = {};
			sb.ByteWidth = sizeof(h2taa::GpuProxy) * kMaxProxies; sb.Usage = D3D11_USAGE_DYNAMIC; sb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			sb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sb.StructureByteStride = sizeof(h2taa::GpuProxy);
			check(dev->CreateBuffer(&sb, nullptr, &mProxyBuf), "proxy buffer");
			D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.FirstElement = 0; sv.Buffer.NumElements = kMaxProxies;
			check(dev->CreateShaderResourceView(mProxyBuf.Get(), &sv, &mProxySrv), "proxy buffer view");
			D3D11_DEPTH_STENCIL_DESC dd = {};
			dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS; dd.StencilEnable = FALSE;
			check(dev->CreateDepthStencilState(&dd, &mProxyDSS), "proxy depth state");
			// the collision-triangle proxies
			check(dev->CreateVertexShader(TAAShaders.triVS.data, TAAShaders.triVS.size, nullptr, &mTriVS), "triangle vertex shader");
			check(dev->CreatePixelShader(TAAShaders.triPS.data, TAAShaders.triPS.size, nullptr, &mTriPS), "triangle pixel shader");
			check(dev->CreatePixelShader(TAAShaders.triExtentPS.data, TAAShaders.triExtentPS.size, nullptr, &mTriExtentPS), "hull extent pixel shader");
			D3D11_BLEND_DESC mb = {};
			mb.RenderTarget[0].BlendEnable = TRUE;
			mb.RenderTarget[0].SrcBlend = mb.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; mb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
			mb.RenderTarget[0].SrcBlendAlpha = mb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; mb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
			mb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			check(dev->CreateBlendState(&mb, &mMaxBS), "hull extent blend state");
			const D3D11_INPUT_ELEMENT_DESC il[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "SLOT", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			check(dev->CreateInputLayout(il, 2, TAAShaders.triVS.data, TAAShaders.triVS.size, &mTriLayout), "triangle input layout");
			D3D11_BUFFER_DESC nb = {};
			nb.ByteWidth = sizeof(h2taa::GpuNode) * kMaxNodes; nb.Usage = D3D11_USAGE_DYNAMIC; nb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			nb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; nb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; nb.StructureByteStride = sizeof(h2taa::GpuNode);
			check(dev->CreateBuffer(&nb, nullptr, &mNodeBuf), "node buffer");
			D3D11_SHADER_RESOURCE_VIEW_DESC nv = {};
			nv.Format = DXGI_FORMAT_UNKNOWN; nv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; nv.Buffer.FirstElement = 0; nv.Buffer.NumElements = kMaxNodes;
			check(dev->CreateShaderResourceView(mNodeBuf.Get(), &nv, &mNodeSrv), "node buffer view");
		}
		catch (HCMRuntimeException& ex)
		{
			objectsOff(ex.what());
		}
	}

	void makeProxySized(ID3D11Device* dev, UINT w, UINT h)
	{
		if (mObjectsFailed || mObjTex) return;
		try
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
			td.Format = DXGI_FORMAT_R16G16_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			check(dev->CreateTexture2D(&td, nullptr, &mObjTex), "object motion target");
			check(dev->CreateRenderTargetView(mObjTex.Get(), nullptr, &mObjRtv), "object motion target view");
			check(dev->CreateShaderResourceView(mObjTex.Get(), nullptr, &mObjSrv), "object motion view");
			td.Format = DXGI_FORMAT_D16_UNORM; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			check(dev->CreateTexture2D(&td, nullptr, &mObjDepth), "object motion priority buffer");
			check(dev->CreateDepthStencilView(mObjDepth.Get(), nullptr, &mObjDsv), "object motion priority view");
			// the hull extent: view depth, blended MAX (32-bit float where the device blends it, else 16-bit)
			UINT support = 0;
			const bool blend32 = SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &support)) && (support & D3D11_FORMAT_SUPPORT_BLENDABLE) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
			td.Format = blend32 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			check(dev->CreateTexture2D(&td, nullptr, &mExtTex), "hull extent target");
			check(dev->CreateRenderTargetView(mExtTex.Get(), nullptr, &mExtRtv), "hull extent target view");
			check(dev->CreateShaderResourceView(mExtTex.Get(), nullptr, &mExtSrv), "hull extent view");
			PLOG_INFO << "Halo 2 TAA: object motion target " << w << "x" << h << " RG16F + D16 priority + " << (blend32 ? "R32F" : "R16F") << " hull extent; collision triangles within " << kTriTolerance
				<< " wu of the scene surface (up to " << kMaxTriDraws << " objects / " << kMaxNodes << " nodes a frame), boxes for objects with no collision model (up to " << kMaxProxies << ")";
		}
		catch (HCMRuntimeException& ex)
		{
			objectsOff(ex.what());
		}
	}

	void objectsOff(const char* why)
	{
		mObjectsFailed = true;
		releaseProxyStatic();
		mObjTex.Reset(); mObjDepth.Reset(); mObjRtv.Reset(); mObjSrv.Reset(); mObjDsv.Reset(); mExtTex.Reset(); mExtRtv.Reset(); mExtSrv.Reset();
		PLOG_ERROR << why << " - Halo 2 TAA object motion is off until HCM restarts (camera motion only)";
	}

	void makeStatic(ID3D11Device* dev)
	{
		check(dev->CreateVertexShader(TAAShaders.fullscreenVS.data, TAAShaders.fullscreenVS.size, nullptr, &mVS), "vertex shader");
		check(dev->CreatePixelShader(TAAShaders.resolvePS.data, TAAShaders.resolvePS.size, nullptr, &mPS), "pixel shader");
		D3D11_SAMPLER_DESC sd = {};
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		check(dev->CreateSamplerState(&sd, &mLinear), "sampler");
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(Constants); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		check(dev->CreateBuffer(&bd, nullptr, &mCB), "constant buffer");
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		check(dev->CreateRasterizerState(&rd, &mRS), "rasterizer state");
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS; dd.StencilEnable = FALSE;
		check(dev->CreateDepthStencilState(&dd, &mDSS), "depth state");
		D3D11_BLEND_DESC bld = {};
		bld.IndependentBlendEnable = FALSE;
		bld.RenderTarget[0].BlendEnable = FALSE; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		check(dev->CreateBlendState(&bld, &mBS), "blend state");
		// the first-person model's bones for the resolve (HCM_TAA.hlsl gFpNodes)
		D3D11_BUFFER_DESC fb = {};
		fb.ByteWidth = (UINT)(sizeof(FpGpuNode) * kMaxFpNodes); fb.Usage = D3D11_USAGE_DYNAMIC; fb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		fb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; fb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; fb.StructureByteStride = sizeof(FpGpuNode);
		check(dev->CreateBuffer(&fb, nullptr, &mFpBoneBuf), "first-person bone buffer");
		D3D11_SHADER_RESOURCE_VIEW_DESC fv = {};
		fv.Format = DXGI_FORMAT_UNKNOWN; fv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; fv.Buffer.FirstElement = 0; fv.Buffer.NumElements = (UINT)kMaxFpNodes;
		check(dev->CreateShaderResourceView(mFpBoneBuf.Get(), &fv, &mFpBoneSrv), "first-person bone buffer view");
	}

	void makeSized(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& sd, DXGI_FORMAT viewFmt)
	{
		releaseSized();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = sd.Width; td.Height = sd.Height; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		// the current frame's copy: the scene texture's own format (CopyResource), read through the render target's view format
		td.Format = sd.Format; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		check(dev->CreateTexture2D(&td, nullptr, &mCur), "frame copy");
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Format = viewFmt;
		check(dev->CreateShaderResourceView(mCur.Get(), &sv, &mCurSrv), "frame copy view");
		// the history: 16-bit float (8-bit history cannot converge at a 0.1 blend)
		td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		for (int i = 0; i < 2; ++i)
		{
			check(dev->CreateTexture2D(&td, nullptr, &mHist[i]), "history");
			check(dev->CreateRenderTargetView(mHist[i].Get(), nullptr, &mHistRtv[i]), "history target");
			check(dev->CreateShaderResourceView(mHist[i].Get(), nullptr, &mHistSrv[i]), "history view");
		}
		mW = sd.Width; mH = sd.Height; mFmt = sd.Format; mViewFmt = viewFmt;
		PLOG_INFO << "Halo 2 TAA: scene target " << mW << "x" << mH << " format " << (int)mFmt << " (view format " << (int)viewFmt << "), history 2x RGBA16F";
	}

	// the gun mask: the scene depth at resolve time (read directly when the texture allows it, else copied)
	ID3D11ShaderResourceView* gunDepth(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT& w, UINT& h)
	{
		ComPtr<ID3D11Resource> res;
		dsv->GetResource(&res);
		ComPtr<ID3D11Texture2D> tex;
		if (!res || FAILED(res.As(&tex)) || !tex) throw HCMRuntimeException("Halo 2 TAA: the scene depth view has no 2D texture");
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		w = d.Width; h = d.Height;
		if (tex.Get() != mFpSource.Get() || !mFpSrv)
		{
			mFpCopy.Reset(); mFpSrv.Reset(); mFpSource.Reset();
			DXGI_FORMAT typeless, view;
			if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || !depthFormats(d.Format, typeless, view))
				throw HCMRuntimeException(std::format("Halo 2 TAA: scene depth format {} / {} samples not handled", (int)d.Format, d.SampleDesc.Count));
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = view; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
			const bool direct = (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && d.Format == typeless;
			if (direct) check(dev->CreateShaderResourceView(tex.Get(), &sv, &mFpSrv), "scene depth view");
			else
			{
				D3D11_TEXTURE2D_DESC cd{};
				cd.Width = d.Width; cd.Height = d.Height; cd.MipLevels = 1; cd.ArraySize = 1; cd.Format = typeless; cd.SampleDesc.Count = 1;
				cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				check(dev->CreateTexture2D(&cd, nullptr, &mFpCopy), "scene depth copy");
				check(dev->CreateShaderResourceView(mFpCopy.Get(), &sv, &mFpSrv), "scene depth copy view");
			}
			mFpSource = tex;
			PLOG_INFO << "Halo 2 TAA: gun mask from the scene depth " << d.Width << "x" << d.Height << " format " << (int)d.Format << (direct ? " (read directly)" : " (copied each frame)");
		}
		if (mFpCopy) ctx->CopyResource(mFpCopy.Get(), tex.Get());
		return mFpSrv.Get();
	}

	// 1 resolved, 0 skipped this frame, throws on a D3D11 failure
	int runResolve(const Snapshot& s, bool reset)
	{
		const uintptr_t b = h2::base();
		auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(mem::q(b + kSceneRtvRva));
		auto* dsv = reinterpret_cast<ID3D11DepthStencilView*>(mem::q(b + kSceneDsvRva));
		if (!comOk(rtv) || !comOk(dsv)) { skipOnce(3, "scene target / depth not readable"); return 0; }
		ComPtr<ID3D11Device> dev;
		rtv->GetDevice(&dev);
		if (!dev) return 0;
		ComPtr<ID3D11DeviceContext> ctx;
		dev->GetImmediateContext(&ctx);
		if (!ctx) return 0;
		GameDepthTap::View depth;
		if (!GameDepthTap::current(dev.Get(), depth) || !depth.srv) { skipOnce(4, "no world depth copy this frame"); return 0; }

		ComPtr<ID3D11Resource> sceneRes;
		rtv->GetResource(&sceneRes);
		ComPtr<ID3D11Texture2D> scene;
		if (!sceneRes || FAILED(sceneRes.As(&scene)) || !scene) throw HCMRuntimeException("Halo 2 TAA: the scene target has no 2D texture");
		D3D11_TEXTURE2D_DESC sd{};
		scene->GetDesc(&sd);
		D3D11_RENDER_TARGET_VIEW_DESC rd{};
		rtv->GetDesc(&rd);
		if (sd.SampleDesc.Count != 1 || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || sd.MipLevels != 1 || sd.ArraySize != 1)
			throw HCMRuntimeException(std::format("Halo 2 TAA: scene target layout not handled ({} samples, view dimension {}, {} mips)", sd.SampleDesc.Count, (int)rd.ViewDimension, sd.MipLevels));

		if (mDevice != dev.Get()) { releaseAll(); mDevice = dev.Get(); makeStatic(dev.Get()); reset = true; }
		if (sd.Width != mW || sd.Height != mH || sd.Format != mFmt || rd.Format != mViewFmt) { makeSized(dev.Get(), sd, rd.Format); reset = true; }
		UINT fpW = 0, fpH = 0;
		ID3D11ShaderResourceView* fpSrv = gunDepth(dev.Get(), ctx.Get(), dsv, fpW, fpH);
		UINT zW = 0, zH = 0;
		{
			ComPtr<ID3D11Resource> zr; depth.srv->GetResource(&zr);
			ComPtr<ID3D11Texture2D> zt;
			if (zr && SUCCEEDED(zr.As(&zt)) && zt) { D3D11_TEXTURE2D_DESC zd{}; zt->GetDesc(&zd); zW = zd.Width; zH = zd.Height; }
		}
		if (!zW || !zH) { skipOnce(5, "world depth size unknown"); return 0; }

		// the view inside the scene target
		float left = (float)s.vp[1], top = (float)s.vp[0], vw = (float)(s.vp[3] - s.vp[1]), vh = (float)(s.vp[2] - s.vp[0]);
		left = std::clamp(left, 0.f, (float)mW); top = std::clamp(top, 0.f, (float)mH);
		vw = std::clamp(vw, 0.f, (float)mW - left); vh = std::clamp(vh, 0.f, (float)mH - top);
		if (vw < 16.f || vh < 16.f) { skipOnce(6, "view rectangle too small"); return 0; }

		// matrices: current clip -> previous clip (both un-jittered)
		const bool reversed = depth.reversedZ;
		const Snapshot& p = reset ? s : mPrev;
		M4 invCur, invCurRot;
		if (!inverse(viewProj(s.w2v, s.proj, reversed, false), invCur) || !inverse(viewProj(s.w2v, s.proj, reversed, true), invCurRot))
		{
			skipOnce(7, "view-projection not invertible");
			return 0;
		}
		const M4 curVP = viewProj(s.w2v, s.proj, reversed, false);
		const M4 prevVP = viewProj(p.w2v, p.proj, reversed, false);
		const M4 reproj = mul(invCur, prevVP);
		const M4 reprojSky = mul(invCurRot, viewProj(p.w2v, p.proj, reversed, true));

		auto settings = settingsWeak.lock();
		const float blend = settings ? std::clamp(settings->h2TAABlend->GetValue(), 0.05f, 0.3f) : 0.1f;
		const bool debug = settings && settings->h2TAADebug->GetValue();

		// v2: the objects that moved by themselves (no "last frame" on a reset frame)
		const bool wantObjects = settings && settings->h2TAAObjectMotion->GetValue() && !mObjectsFailed;
		if (wantObjects) { makeProxyStatic(dev.Get()); makeProxySized(dev.Get(), mW, mH); }
		const bool objectsUsable = wantObjects && !mObjectsFailed && !reset && mProxiesFresh && mProxyVS && mTriVS && mTriExtentPS && mObjRtv && mExtRtv;
		const UINT proxyCount = objectsUsable ? (UINT)(std::min)(mObjFrame.boxes.size(), (size_t)kMaxProxies) : 0;
		const UINT nodeCount = objectsUsable ? (UINT)(std::min)(mObjFrame.nodes.size(), (size_t)kMaxNodes) : 0;
		const UINT triDrawCount = (objectsUsable && nodeCount == mObjFrame.nodes.size()) ? (UINT)(std::min)(mObjFrame.draws.size(), (size_t)kMaxTriDraws) : 0;
		const bool drawObjects = proxyCount > 0 || triDrawCount > 0;
		mProxiesFresh = false;
		if (sLogObjectStats.exchange(false) || (drawObjects && !mLoggedFirstProxies))
		{
			mLoggedFirstProxies = mLoggedFirstProxies || drawObjects;
			const h2taa::ObjectStats& os = mObjects.stats();
			PLOG_INFO << std::format("Halo 2 TAA objects: {} tracked ({} interpolated poses), {} with a changed pose, {} sent this frame: {} as collision triangles ({} nodes), {} boxes; "
				"{} left out (the first-person player and what they hold / camera inside a box); reading the poses took {} us (worst {} us); object motion {}",
				os.objects, os.interpolated, os.withShape, os.moving, os.meshObjects, os.nodes, os.boxes, os.cameraInside, mCaptureUs, mCaptureUsMax,
				!wantObjects ? (mObjectsFailed ? "failed (off)" : "off") : drawObjects ? "drawn" : reset ? "not drawn (history reset this frame)" : "nothing moving");
		}
		Constants k{};
		for (int i = 0; i < 16; ++i) { k.reproj[i] = (float)reproj[i]; k.reprojSky[i] = (float)reprojSky[i]; }
		k.rect[0] = left; k.rect[1] = top; k.rect[2] = vw; k.rect[3] = vh;
		k.tex[0] = 1.f / (float)mW; k.tex[1] = 1.f / (float)mH; k.tex[2] = (float)mW; k.tex[3] = (float)mH;
		k.params[0] = blend; k.params[1] = reset ? 1.f : 0.f; k.params[2] = debug ? 1.f : 0.f; k.params[3] = reversed ? 1.f : 0.f;
		k.depth[0] = (float)zW / (float)mW; k.depth[1] = (float)zH / (float)mH; k.depth[2] = (float)fpW / (float)mW; k.depth[3] = (float)fpH / (float)mH;
		k.params2[0] = drawObjects ? 1.f : 0.f;
		// the speed limit on the history: moving pixels take this much of the new frame (reached at kMotionFullSpeed pixels a frame)
		const float motionBlend = settings ? std::clamp(settings->h2TAAMotionBlend->GetValue(), 0.05f, 1.f) : 0.4f;
		k.params2[1] = motionBlend; k.params2[2] = kMotionFullSpeed;

		// The first-person model: how each bone moved since last frame. Everything relative to this frame's camera position
		// (world coordinates near the edge of a map leave too few float bits for a gun a pixel of which is 0.0005 wu).
		UINT fpNodeCount = 0;
		if (mFp.fresh && mFpPrev.fresh && !reset && mFpBoneBuf && mFpBoneSrv)
		{
			M4 T{};   // camera-relative -> world
			T[0] = T[5] = T[10] = T[15] = 1.0; T[12] = s.pos.x; T[13] = s.pos.y; T[14] = s.pos.z;
			const M4 fpCur = mul(T, viewProj(mFp.w2v, mFp.proj, reversed, false));
			const M4 fpPrev = mul(T, viewProj(mFpPrev.w2v, mFpPrev.proj, reversed, false));
			M4 fpInv;
			if (inverse(fpCur, fpInv))
			{
				mFpGpu.clear();
				const double c[3] = { s.pos.x, s.pos.y, s.pos.z };
				for (int i = 0; i < mFp.models && i < mFpPrev.models; ++i)
				{
					const FpModel& a = mFp.model[i];
					const FpModel& p = mFpPrev.model[i];
					if (a.tag != p.tag || a.count != p.count) continue;   // another model in this slot (a weapon swap): zero motion this frame
					for (int j = 0; j < a.count && mFpGpu.size() < kMaxFpNodes; ++j)
					{
						const float* nc = &a.m[(size_t)j * 13];
						double d[12];
						if (!h2taa::nodeDelta(nc, &p.m[(size_t)j * 13], d)) continue;
						FpGpuNode g{};
						for (int r = 0; r < 3; ++r)
						{
							g.origin[r] = (float)((double)nc[10 + r] - c[r]);
							g.delta[r * 4 + 0] = (float)d[r * 4 + 0]; g.delta[r * 4 + 1] = (float)d[r * 4 + 1]; g.delta[r * 4 + 2] = (float)d[r * 4 + 2];
							g.delta[r * 4 + 3] = (float)(d[r * 4 + 0] * c[0] + d[r * 4 + 1] * c[1] + d[r * 4 + 2] * c[2] + d[r * 4 + 3] - c[r]);
						}
						g.origin[3] = 1.f;
						mFpGpu.push_back(g);
					}
				}
				D3D11_MAPPED_SUBRESOURCE fmap{};
				if (!mFpGpu.empty() && SUCCEEDED(ctx->Map(mFpBoneBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &fmap)) && fmap.pData)
				{
					memcpy(fmap.pData, mFpGpu.data(), mFpGpu.size() * sizeof(FpGpuNode));
					ctx->Unmap(mFpBoneBuf.Get(), 0);
					fpNodeCount = (UINT)mFpGpu.size();
					for (int i = 0; i < 16; ++i) { k.fpInvVP[i] = (float)fpInv[i]; k.fpPrevVP[i] = (float)fpPrev[i]; }
					if (!mLoggedFp)
					{
						mLoggedFp = true;
						PLOG_INFO << std::format("Halo 2 TAA: first-person model - {} models, {} bones followed (table halo2+0x165C508); its projection x / y scale {:.4f} / {:.4f} "
							"(the world's {:.4f} / {:.4f})", mFp.models, fpNodeCount, mFp.proj[0], mFp.proj[5], s.proj[0], s.proj[5]);
					}
				}
			}
		}
		k.fp[0] = (float)fpNodeCount; k.fp[1] = kFpMaxDistance;
		ctx->UpdateSubresource(mCB.Get(), 0, nullptr, &k, 0, 0);
		ProxyConstants pk{};
		if (drawObjects)
		{
			for (int i = 0; i < 16; ++i) { pk.invVP[i] = (float)invCur[i]; pk.prevVP[i] = (float)prevVP[i]; pk.curVP[i] = (float)curVP[i]; }
			pk.rect[0] = left; pk.rect[1] = top; pk.rect[2] = vw; pk.rect[3] = vh;
			pk.depth[0] = k.depth[0]; pk.depth[1] = k.depth[1];
			pk.params[0] = reversed ? 1.f : 0.f;
			pk.params[1] = kTriTolerance;
			// the frame was drawn moved by (+jx right, +jy down) pixels: the triangles cover the same pixels
			const float jx = s.jittered ? s.jx : 0.f, jy = s.jittered ? s.jy : 0.f;
			pk.params[2] = jx * 2.f / vw; pk.params[3] = -jy * 2.f / vh;
			pk.draw[1] = jx; pk.draw[2] = jy; pk.draw[3] = kTriFrontTolerance;
			ctx->UpdateSubresource(mProxyCB.Get(), 0, nullptr, &pk, 0, 0);
		}

		if (!mLoggedFirstResolve)
		{
			mLoggedFirstResolve = true;
			PLOG_INFO << std::format("Halo 2 TAA: first resolve - view {}x{} at ({}, {}) in the {}x{} scene target, world depth {}x{} ({}), gun depth {}x{}, blend {:.2f}, frame {}",
				vw, vh, left, top, mW, mH, zW, zH, reversed ? "reversed z" : "standard z", fpW, fpH, blend, s.jittered ? "jittered" : "not jittered");
			PLOG_INFO << std::format("Halo 2 TAA: speed limit on the history - moving pixels take {:.2f} of the new frame from {} px a frame ({})", motionBlend, kMotionFullSpeed, motionBlend > blend ? "on" : "off: not above the blend");
		}

		ctx->CopyResource(mCur.Get(), scene.Get());
		const int cur = mHistIndex, prev = mHistIndex ^ 1;

		D3D11StateBackup saved;
		saved.save(ctx.Get());
		const D3D11_VIEWPORT vp = { left, top, vw, vh, 0.f, 1.f };
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(mRS.Get());
		const FLOAT zero4[4] = { 0.f, 0.f, 0.f, 0.f };
		ctx->OMSetBlendState(mBS.Get(), zero4, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(mDSS.Get(), 0);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->IASetInputLayout(nullptr);
		ID3D11Buffer* noVB = nullptr; UINT zeroU = 0;
		ctx->IASetVertexBuffers(0, 1, &noVB, &zeroU, &zeroU);
		ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		ctx->GSSetShader(nullptr, nullptr, 0); ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
		ID3D11ShaderResourceView* none[D3D11StateBackup::kSrvs] = {};
		ctx->PSSetShaderResources(0, D3D11StateBackup::kSrvs, none);

		// v2: the object motion pass into the motion target (cleared to "none" first, so the resolve reads "none" even if an
		// upload fails): the collision triangles of every moving object that has them, then a rectangle per box
		if (drawObjects)
		{
			const FLOAT noMotion[4] = { 30000.f, 30000.f, 0.f, 0.f };
			ctx->ClearRenderTargetView(mObjRtv.Get(), noMotion);
			ctx->ClearDepthStencilView(mObjDsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
			ID3D11RenderTargetView* ort = mObjRtv.Get();
			ctx->OMSetRenderTargets(1, &ort, mObjDsv.Get());
			ctx->OMSetDepthStencilState(mProxyDSS.Get(), 0);
			ID3D11Buffer* pcb = mProxyCB.Get();
			ctx->VSSetConstantBuffers(0, 1, &pcb);
			ctx->PSSetConstantBuffers(0, 1, &pcb);
			ID3D11ShaderResourceView* noVs[D3D11StateBackup::kVsSrvs] = {};
			D3D11_MAPPED_SUBRESOURCE map{};
			UINT meshesDrawn = 0, vertsDrawn = 0;
			if (triDrawCount > 0 && SUCCEEDED(ctx->Map(mNodeBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)) && map.pData)
			{
				memcpy(map.pData, mObjFrame.nodes.data(), (size_t)nodeCount * sizeof(h2taa::GpuNode));
				ctx->Unmap(mNodeBuf.Get(), 0);
				ctx->IASetInputLayout(mTriLayout.Get());
				ctx->VSSetShader(mTriVS.Get(), nullptr, 0);
				ID3D11ShaderResourceView* vsrv[3] = { nullptr, nullptr, mNodeSrv.Get() };
				ctx->VSSetShaderResources(0, 3, vsrv);
				const auto drawMeshes = [&]()
				{
					meshesDrawn = 0; vertsDrawn = 0;
					for (UINT i = 0; i < triDrawCount; ++i)
					{
						const h2taa::TriDraw& d = mObjFrame.draws[i];
						const MeshVB* m = meshBuffer(dev.Get(), d.def);
						if (!m) continue;
						pk.draw[0] = (float)d.firstNode;
						ctx->UpdateSubresource(mProxyCB.Get(), 0, nullptr, &pk, 0, 0);
						ID3D11Buffer* vb = m->vb.Get();
						const UINT stride = sizeof(h2taa::TriVertex), offset = 0;
						ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
						ctx->Draw(m->count, 0);
						++meshesDrawn; vertsDrawn += m->count;
					}
				};
				// 1. how far the collision hulls reach at each pixel (farthest face, blend MAX; 0 = no hull there)
				ctx->ClearRenderTargetView(mExtRtv.Get(), zero4);
				ID3D11RenderTargetView* ert = mExtRtv.Get();
				ctx->OMSetRenderTargets(1, &ert, nullptr);
				ctx->OMSetDepthStencilState(mDSS.Get(), 0);
				ctx->OMSetBlendState(mMaxBS.Get(), zero4, 0xFFFFFFFF);
				ctx->PSSetShader(mTriExtentPS.Get(), nullptr, 0);
				drawMeshes();
				// 2. the motion: a pixel is the object's where the scene surface is at a triangle or inside the hull
				ctx->OMSetRenderTargets(1, &ort, mObjDsv.Get());
				ctx->OMSetDepthStencilState(mProxyDSS.Get(), 0);
				ctx->OMSetBlendState(mBS.Get(), zero4, 0xFFFFFFFF);
				ctx->PSSetShader(mTriPS.Get(), nullptr, 0);
				ID3D11ShaderResourceView* psrv[4] = { nullptr, depth.srv, mNodeSrv.Get(), mExtSrv.Get() };
				ctx->PSSetShaderResources(0, 4, psrv);
				drawMeshes();
				ctx->IASetVertexBuffers(0, 1, &noVB, &zeroU, &zeroU);
				ctx->IASetInputLayout(nullptr);
				ctx->PSSetShaderResources(0, D3D11StateBackup::kSrvs, none);
				ctx->VSSetShaderResources(0, D3D11StateBackup::kVsSrvs, noVs);
				if (!mLoggedFirstMesh && meshesDrawn > 0)
				{
					mLoggedFirstMesh = true;
					PLOG_INFO << "Halo 2 TAA objects: first collision-triangle pass - " << meshesDrawn << " objects, " << vertsDrawn / 3 << " triangles, " << nodeCount << " nodes";
				}
			}
			map = {};
			if (proxyCount > 0 && SUCCEEDED(ctx->Map(mProxyBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)) && map.pData)
			{
				memcpy(map.pData, mObjFrame.boxes.data(), (size_t)proxyCount * sizeof(h2taa::GpuProxy));
				ctx->Unmap(mProxyBuf.Get(), 0);
				ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				ctx->VSSetShader(mProxyVS.Get(), nullptr, 0);
				ctx->PSSetShader(mProxyPS.Get(), nullptr, 0);
				ID3D11ShaderResourceView* vsrv = mProxySrv.Get();
				ctx->VSSetShaderResources(0, 1, &vsrv);
				ID3D11ShaderResourceView* psrv[2] = { mProxySrv.Get(), depth.srv };
				ctx->PSSetShaderResources(0, 2, psrv);
				ctx->DrawInstanced(4, proxyCount, 0, 0);
				ctx->PSSetShaderResources(0, D3D11StateBackup::kSrvs, none);
				ctx->VSSetShaderResources(0, D3D11StateBackup::kVsSrvs, noVs);
				ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			}
			ctx->OMSetDepthStencilState(mDSS.Get(), 0);
		}

		ctx->VSSetShader(mVS.Get(), nullptr, 0);
		ctx->PSSetShader(mPS.Get(), nullptr, 0);
		ID3D11Buffer* cb = mCB.Get();
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ID3D11SamplerState* samp = mLinear.Get();
		ctx->PSSetSamplers(0, 1, &samp);
		// targets first (unbinds the scene depth and the motion target from the output so they can be read), then the inputs
		ID3D11RenderTargetView* rts[2] = { mHistRtv[cur].Get(), rtv };
		ctx->OMSetRenderTargets(2, rts, nullptr);
		ID3D11ShaderResourceView* srvs[6] = { mCurSrv.Get(), mHistSrv[prev].Get(), depth.srv, fpSrv, drawObjects ? mObjSrv.Get() : nullptr,
			fpNodeCount ? mFpBoneSrv.Get() : nullptr };
		ctx->PSSetShaderResources(0, 6, srvs);
		ctx->Draw(3, 0);
		ctx->PSSetShaderResources(0, D3D11StateBackup::kSrvs, none);
		saved.restore(ctx.Get());

		mHistIndex = prev;   // this frame's history is read next frame
		return 1;
	}

	// ================================ settings ================================
	// 1 ready, 0 halo2.dll not loaded yet, -1 another build
	int ensureHook()
	{
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (!mod) return 0;
		if (!mVerified)
		{
			if (mVerifyFailed) return -1;
			const uintptr_t b = (uintptr_t)mod;
			if (const uint32_t st = peStamp(b); st != kPeStamp)
			{
				mVerifyFailed = true;
				PLOG_ERROR << "Halo 2 TAA: halo2.dll PE stamp " << std::hex << st << " is not 1.3528's " << kPeStamp << " - not applied";
				return -1;
			}
			if (!mRectHook->isHookInstalled())
				for (const Probe& p : kProbes)
					if (!bytesAt(b + p.rva, p.bytes, p.n))
					{
						mVerifyFailed = true;
						PLOG_ERROR << "Halo 2 TAA: halo2.dll+" << std::hex << p.rva << " is not what this was made for (MCC 1.3528) - not applied";
						return -1;
					}
			mVerified = true;
		}
		mRectHook->setWantsToBeAttached(true);
		const bool ok = mRectHook->isHookInstalled();
		static bool s_said = false;
		if (ok && !s_said)
		{
			s_said = true;
			PLOG_INFO << "Halo 2 TAA: jitter hook on halo2+0x7DFCD0 installed (main view return 0x7E231A, first-person return 0x7E0FCB); "
				"matrix tap at the first-person pass 0x7E0C60 and the resolve at 0x951EC0 entry through the Collision Viewer's hooks";
		}
		return ok ? 1 : 0;
	}

	void apply(bool on, bool fromToggle)
	{
		std::scoped_lock lk(mToggleMutex);
		if (!on)
		{
			sEnabled = false;   // the jitter stops at the very next projection
			GameDepthTap::setWantedByTAA(false);
			sResetRequested = true;
			if (fromToggle) say("Halo 2 TAA off");
			return;
		}
		const int r = ensureHook();
		if (r < 0) { sEnabled = false; GameDepthTap::setWantedByTAA(false); if (fromToggle) say("Halo 2 TAA: this Halo 2 build is not supported (made for MCC 1.3528)"); return; }
		if (sFailed.load()) { if (fromToggle) say("Halo 2 TAA: turned off by an earlier graphics error - restart HCM to use it again"); return; }
		sResetRequested = true;
		sObjectsResetRequested = true;
		mEnabledAtMs = GetTickCount64();
		GameDepthTap::setWantedByTAA(true);   // arms the Collision Viewer's Halo 2 pass hooks and the world depth copy
		sEnabled = true;
		if (fromToggle) say(r > 0 ? "Halo 2 TAA on (experimental)" : "Halo 2 TAA on - starts once Halo 2 is loaded");
		PLOG_INFO << "Halo 2 TAA: on (hook " << (r > 0 ? "installed" : "pending: halo2.dll not loaded") << ")";
	}

	// TAA overscan (H2Overscan.h): what TAA wants is checked every main view (render thread) and handed to H2Supersampling,
	// which rebuilds the targets; a slider change is applied once it has been left alone for 0.6 s (each apply is a video
	// reset). Off whenever TAA is off.
	float mOsRequestedX = -1.f, mOsRequestedY = -1.f;   // the last percents handed over (-1 = none yet)
	std::atomic<uint64_t> mOsChangedAt{ 0 };

	void pollOverscan()
	{
		auto settings = settingsWeak.lock();
		float wx = 0.f, wy = 0.f;
		if (settings && sEnabled.load() && sActive.load() && !sFailed.load())
		{
			wx = std::round(std::clamp(settings->h2TAAOverscan->GetValue(), 0.f, 15.f) * 2.f) / 2.f;            // half-percent steps
			wy = std::round(std::clamp(settings->h2TAAOverscanVertical->GetValue(), 0.f, 15.f) * 2.f) / 2.f;
		}
		if (wx == mOsRequestedX && wy == mOsRequestedY) return;
		if (mOsRequestedX < 0.f && wx == 0.f && wy == 0.f) { mOsRequestedX = mOsRequestedY = 0.f; return; }   // start-up, off: nothing to do
		if (GetTickCount64() - mOsChangedAt.load() < 600) return;   // still being dragged
		const bool wasOn = mOsRequestedX > 0.f || mOsRequestedY > 0.f;
		mOsRequestedX = wx; mOsRequestedY = wy;
		const h2os::RequestFn fn = h2os::gRequest.load();
		if (!fn)
		{
			if (wx > 0.f || wy > 0.f) say("Halo 2 TAA Overscan: it needs Halo 2 Supersampling, which did not load - not applied");
			return;
		}
		const int r = fn(wx, wy);
		const float cost = (1.f + 2.f * wx / 100.f) * (1.f + 2.f * wy / 100.f);
		if (r < 0) say("Halo 2 TAA Overscan: this Halo 2 build is not supported (made for MCC 1.3528)");
		else if (r == 0) say(std::format("Halo 2 TAA Overscan {}% / {}% set - it applies once Halo 2 is running", wx, wy));
		else if (wx > 0.f || wy > 0.f) say(std::format("Halo 2 TAA Overscan applied: {}% left/right, {}% top/bottom ({:.2f}x the pixels)", wx, wy, cost));
		else if (wasOn) say("Halo 2 TAA Overscan off");
		PLOG_INFO << "Halo 2 TAA overscan: requested " << wx << "% left/right, " << wy << "% top/bottom (result " << r << ")";
	}

	// 2026-11-24 (user: "TAA goes under AA method"): TAA has no toggle of its own - it runs while the Anti Aliasing
	// toggle is on and the method is TAA.
	static bool wanted(const std::shared_ptr<SettingsStateAndEvents>& settings)
	{
		return settings && settings->smaaToggle->GetValue() && settings->postAAMethod->GetValue() == SettingsEnums::PostAAMethod::TAA;
	}

	// the toggle or the method changed: act (and say so) only when that changes whether TAA should run
	void onSettingChanged()
	{
		const bool want = wanted(settingsWeak.lock());
		if (mWanted.exchange(want) == want) return;
		apply(want, sActive.load());   // the on / off message only while Halo 2 is the running game
	}

	void onMCCStateChanged(const MCCState& s)
	{
		sActive = s.currentGameState == mGame;
		if (sActive.load() && wanted(settingsWeak.lock()) && !sEnabled.load()) apply(true, false);
		sResetRequested = true;
	}

	std::unique_ptr<ModuleInlineHook> makeHook(uintptr_t rva, void* detour)
	{
		auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)rva });
		return ModuleInlineHook::make(L"halo2.dll", ptr, detour, false);
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->smaaToggle->valueChangedEvent, [this](bool&) { onSettingChanged(); }),
		mMethodCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->postAAMethod->valueChangedEvent, [this](SettingsEnums::PostAAMethod&) { onSettingChanged(); }),
		mDebugCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2TAADebug->valueChangedEvent, [this](bool& on) { PLOG_INFO << "Halo 2 TAA: debug view " << (on ? "on" : "off"); if (on) sLogObjectStats = true; }),
		mOverscanCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2TAAOverscan->valueChangedEvent, [this](float&) { mOsChangedAt = GetTickCount64(); }),
		mOverscanVCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2TAAOverscanVertical->valueChangedEvent, [this](float&) { mOsChangedAt = GetTickCount64(); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		if (game.operator GameState::Value() != GameState::Value::Halo2) throw HCMInitException("Halo 2 TAA is Halo 2 only");
		if (sSelf.load()) throw HCMInitException("Halo 2 TAA already exists");
		mRectHook = makeHook(kRectRva, (void*)&rectDetour);
		sSelf = this;
		if (auto mcc = mccWeak.lock()) sActive = mcc->getCurrentMCCState().currentGameState == mGame;
		H2PassTaps::firstPersonPass = &fpTap;
		H2PassTaps::beforeResolve = &resolveTap;
		mWanted = wanted(dicon.Resolve<SettingsStateAndEvents>().lock());
		if (mWanted.load()) apply(true, false);
	}

	~Impl()
	{
		sEnabled = false;
		GameDepthTap::setWantedByTAA(false);
		H2PassTaps::firstPersonPass = nullptr;
		H2PassTaps::beforeResolve = nullptr;
		if (mRectHook) mRectHook->setWantsToBeAttached(false);
		for (int i = 0; i < 500 && sInFlight.load() > 0; ++i) Sleep(1);
		{
			std::scoped_lock lk(mGpuMutex);
			releaseAll();
		}
		sSelf = nullptr;
	}
};

H2TAA::H2TAA(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(gameImpl, dicon))
{
}

H2TAA::~H2TAA() = default;
