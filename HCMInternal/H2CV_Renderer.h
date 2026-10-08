#pragma once
// D3D11 overlay renderer. Draws at Present, on the final LDR back buffer -> nothing goes through the game's
// bloom/HDR post-processing (the old mid-scene draw did, which is what "bloomed").
//
// Occlusion sources (combinable):
//  * OWN depth: collision triangles (+ Havok capsules of bipeds/vehicles) rasterised into our own reversed-Z depth
//    buffer with exactly the same camera as the lines -> always self-consistent, independent of the game's buffers.
//  * GAME depth: a copy of the game's scene depth captured at an engine hook point (set via setGameDepth), compared
//    per pixel after linearising both depths -> occlusion by render-only geometry, the player's model, the gun.
//
// Lines are expanded to screen-space quads of N pixels in a geometry shader (D3D11 1-px lines vanish at distance).
#include <d3d11.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include "H2CV_Collision.h"

namespace h2cv
{
	struct LineVertex { float x, y, z; uint32_t rgba; };

	struct GameDepthInput
	{
		ID3D11ShaderResourceView* srv = nullptr; // R32_FLOAT (or typeless D32 viewed as R32) copy of the scene depth
		int encoding = 0;   // 0 none, 1 standard [near->0, far->1], 2 reversed finite, 3 reversed infinite
		float nearClip = 0.0625f, farClip = 1000.f;
		float width = 0, height = 0; // texture size (may differ from the back buffer with resolution scaling)
		ID3D11ShaderResourceView* fpSrv = nullptr; // optional first-person viewmodel depth (any non-cleared texel occludes)
		float fpClearValue = 0.f;
	};

	class Renderer
	{
	public:
		bool init(ID3D11Device* dev);
		void shutdown();

		// per frame
		bool begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height,
		           const h2::Vec3& camPos, const h2::Vec3& camFwd, const h2::Vec3& camUp, float vfov, float nearClip);
		void end();
		// draw INTO THE GAME'S FRAME: its scene RTV + world DSV (reversed-Z, GREATER_EQUAL), its viewport, a given view-proj
		bool beginExternal(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv,
		                   const D3D11_VIEWPORT& vp, const DirectX::XMFLOAT4X4& viewProj);

		// occlusion (own depth)
		void clearOwnDepth();
		void setStaticOccluders(const std::vector<Tri>& tris, uintptr_t version); // uploaded once per version
		void drawStaticOccluders();
		void drawOccluders(const std::vector<Tri>& tris);

		// lines; colour is 0xAABBGGRR. useOwnDepth/useGameDepth choose the occlusion sources.
		void drawLines(const LineVertex* v, size_t count, float widthPx, float pull, bool useOwnDepth, bool useGameDepth,
		               float hiddenAlpha);
		void drawTris(const LineVertex* v, size_t count, bool depthTest); // filled, alpha-blended

		void setGameDepth(const GameDepthInput& in) { mGameDepth = in; }

		// post-bloom mode: W = copy of the game's world depth (taken at the first-person pass, DSV-bindable),
		// F = copy of the first-person depth (taken after it, sampled) -> merge: W := nearest wherever the gun is.
		bool copyWorldDepth(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* gameDsv);
		bool copyFpDepthAndMerge(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* gameDsv, const D3D11_VIEWPORT& vp, bool reversed);
		ID3D11DepthStencilView* worldCopyDsv() const { return mWDsv; }
		// depth convention of the bound DSV: reversed (default; GREATER_EQUAL, pull z*=1+p) or standard (LESS_EQUAL, z-=p*w)
		void setStandardZ(bool std) { mStdZ = std; }
		bool ready() const { return mReady; }

		// projection helpers for 2D labels
		bool project(const h2::Vec3& p, float& sx, float& sy) const;
		const DirectX::XMFLOAT4X4& viewProj() const { return mViewProj; }

	private:
		bool compile();
		void ensureDepth(uint32_t w, uint32_t h);
		void upload(const void* data, size_t bytes);
		void setCB(float widthPx, float pull, float hiddenAlpha, float hiddenPass, bool useGameDepth);
		void saveState();
		void restoreState();

		ID3D11Device* mDev = nullptr;
		ID3D11DeviceContext* mCtx = nullptr;
		ID3D11RenderTargetView* mRtv = nullptr;
		uint32_t mW = 0, mH = 0; float mVpX = 0, mVpY = 0;
		DirectX::XMFLOAT4X4 mViewProj{};
		h2::Vec3 mCamPos{};

		ID3D11VertexShader* mVsLine = nullptr; ID3D11GeometryShader* mGsLine = nullptr; ID3D11PixelShader* mPsLine = nullptr;
		ID3D11VertexShader* mVsPos = nullptr; ID3D11PixelShader* mPsFill = nullptr; ID3D11VertexShader* mVsFill = nullptr;
		ID3D11InputLayout* mLayoutLine = nullptr; ID3D11InputLayout* mLayoutPos = nullptr;
		ID3D11Buffer* mCB = nullptr;
		ID3D11Buffer* mVB = nullptr; size_t mVBBytes = 0;
		ID3D11Buffer* mStaticVB = nullptr; uint32_t mStaticCount = 0; uintptr_t mStaticVersion = ~uintptr_t(0);
		ID3D11Texture2D* mDepthTex = nullptr; ID3D11DepthStencilView* mDsv = nullptr; uint32_t mDepthW = 0, mDepthH = 0;
		ID3D11DepthStencilState* mDsWrite = nullptr;   // occluder prepass: GREATER, write
		ID3D11DepthStencilState* mDsTest = nullptr;    // visible lines: GREATER_EQUAL, no write
		ID3D11DepthStencilState* mDsHidden = nullptr;  // hidden lines: LESS, no write
		ID3D11DepthStencilState* mDsNone = nullptr;
		ID3D11DepthStencilState* mDsTestStd = nullptr;   // standard-Z: LESS_EQUAL
		ID3D11DepthStencilState* mDsHiddenStd = nullptr; // standard-Z: GREATER
		bool mStdZ = false;
		bool ensureCopies(ID3D11Texture2D* src);
		ID3D11Texture2D* mWTex = nullptr; ID3D11DepthStencilView* mWDsv = nullptr;
		ID3D11Texture2D* mFTex = nullptr; ID3D11ShaderResourceView* mFSrv = nullptr;
		D3D11_TEXTURE2D_DESC mCopyDesc{};
		ID3D11VertexShader* mVsFull = nullptr; ID3D11PixelShader* mPsMerge = nullptr;
		ID3D11DepthStencilState* mDsAlwaysWrite = nullptr;
		ID3D11BlendState* mBlendAlpha = nullptr; ID3D11BlendState* mBlendNoColor = nullptr;
		ID3D11RasterizerState* mRsNoCull = nullptr;
		ID3D11SamplerState* mPointClamp = nullptr;
		GameDepthInput mGameDepth;
		bool mReady = false;

		struct Saved;
		Saved* mSaved = nullptr;
	};
}
