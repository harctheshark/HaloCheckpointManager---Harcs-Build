#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "H2CV_Collision.h"
#include "H2CV_Renderer.h"

namespace h2cv
{
	// InFrame: drawn inside the game's frame at the first-person pass, against the game's real world depth (default).
	// Own: drawn at Present against the viewer's own collision depth (automatic fallback when InFrame can't run,
	//      e.g. remastered graphics). XRay: no occlusion.
	enum class Occlusion : int { InFrame = 0, PostBloom, Own, XRay, Count };
	const char* occlusionName(Occlusion o);

	struct Settings
	{
		std::atomic_bool show[kCatCount] = { true, false, true, true, false, false, false, false, true };
		std::atomic_bool fills{ true };            // translucent invisible-barrier and kill-trigger faces
		std::atomic<float> barrierOpacity{ 0.03f };
		std::atomic<float> killOpacity{ 0.04f };
		std::atomic_int occlusion{ (int)Occlusion::InFrame };
		std::atomic_bool gameZReversed{ true };   // F12 diagnostic: game depth stored as 1 - z/w (W1: reversed-Z on by default)
		std::atomic<float> lineWidth{ 2.0f };     // px
		std::atomic<float> pull{ 0.0015f };       // relative depth pull (0.15% of distance)
		std::atomic<float> hiddenAlpha{ 0.0f };   // 0 = hidden lines invisible
		std::atomic<float> nearRadius{ 60.0f };   // wu around the camera
		std::atomic_bool tim{ true };             // TIM overlay (player's level-pair children)
		std::atomic_bool timLabels{ true };
		std::atomic_bool pills{ true };           // Havok capsules of bipeds
		std::atomic_bool bipedOccluders{ true };  // capsules of OTHER bipeds/vehicles occlude lines
		std::atomic_bool hud{ true };
	};

	class Viewer
	{
	public:
		Settings settings;

		// called from HCM's ForegroundDirectXRenderEvent: inside Present, ImGui frame open, HCM's UI not drawn yet
		void onRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h);
		void onFirstPersonPass(); // engine hook: entry of sub_1807E0C60 (render thread)
		void onResolveDone();     // engine hook: exit of sub_180951EC0 (bloom + composite done, HUD not yet)
		void restoreBloom();      // put the game's bloom gate back (unload / mode change)
		void beforeResize();
		void shutdown(); // render thread must be stopped first

		// "game depth" input set by the engine hooks (see EngineDepth)
		void setGameDepth(const GameDepthInput& g) { mGame = g; }

	private:
		bool initDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx);
		void drawCollision(const h2::Vec3& camPos, bool depthTest);
		void drawTim();
		void drawPills();
		void drawHud(const h2::Camera& cam);

		ID3D11Device* mDev = nullptr;
		ID3D11DeviceContext* mCtx = nullptr;
		ID3D11RenderTargetView* mRtv = nullptr;
		uint32_t mW = 0, mH = 0;
		bool mImgui = false;
		Renderer mR;
		CollisionWorld mWorld;
		GameDepthInput mGame;
		std::vector<LineVertex> mScratch;
		uint64_t mLastTime = 0;

		// HUD stats
		struct TimRow { uint32_t key; int cat; uint32_t inst, surf; float tim; int manifold; uint8_t kind; float trueDist; };
		std::vector<TimRow> mTimRows;
		int32_t mPairS = 0; bool mPairFound = false; bool mPairWelder = false; uint32_t mTick = 0;
		std::string mStatus;
		// in-frame bookkeeping
		uint32_t mPresentCount = 0, mInFrameCount = 0, mLastInFramePresent = 0;
		bool mInFrameThisFrame = false;
		std::string mInFrameNote;
		float mLabelScaleX = 1.f, mLabelScaleY = 1.f;
		// post-bloom: per-view snapshot taken at the first-person pass, consumed after the resolve
		bool mPbPending = false; h2::GameFrame mPbFrame{}; D3D11_VIEWPORT mPbVp{};
		// bloom gate dword_180E19438 (single reader in sub_180951EC0; 0 = bloom on)
		bool mBloomTouched = false; uint32_t mBloomOrig = 0;
		bool drawViewInto(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, const h2::GameFrame& f, const D3D11_VIEWPORT& vp);
		void setBloomSuppressed(bool off);
	};
}
