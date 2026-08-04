#pragma once
#include "pch.h"
#include "DIContainer.h"
#include "IRenderer3D.h"
#include "GameState.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "IMCCStateHook.h"
#include "SettingsStateAndEvents.h"
#include "HCEGetCameraData.h"
#include "HCEGetPlayerState.h"

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>
#include <array>
#include <vector>

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) ONLY - the D3D12 implementation of IRenderer3D.
//
// This is a PARALLEL implementation, not a refactor: Renderer3DImpl (D3D11/DirectXTK, the six MCC games) is
// untouched and the two share nothing but the IRenderer3D interface. Duplication between them is deliberate.
//
// HOW IT INTEGRATES WITH THE EXISTING D3D12 HOOK - NO SECOND HOOK
// ---------------------------------------------------------------
// D3D12Hook already owns the whole frame: it adopts the swap chain, captures the game's DIRECT command queue out
// of ExecuteCommandLists, keeps a command allocator ring fenced against the GPU, resets ONE command list per
// frame, transitions the back buffer to RENDER_TARGET, binds the RTV and its SRV heap, fires the present event,
// then barriers back to PRESENT, closes, executes and signals.
//
// We are a SUBSCRIBER of that event (via ImGuiManager::ForegroundD3D12RenderEvent -> Render3DEventProvider). We
// therefore only ever RECORD into a command list somebody else owns:
//   * we never Reset(), Close(), ExecuteCommandLists() or Signal(),
//   * we never transition the back buffer,
//   * we never touch the descriptor heap binding (which is exactly why textures are not implemented, see below),
//   * everything we DO change - viewport, scissor, render targets, root signature, PSO, topology, vertex/index
//     buffers - is state that ImGui_ImplDX12_RenderDrawData re-establishes for itself immediately after us. The
//     one exception is the render-target binding, because we bind our depth buffer to it and imgui's PSO declares
//     DSVFormat = UNKNOWN; endFrame() re-binds the RTV with NO depth so that can never mismatch.
//
// DEPTH: OUR OWN BUFFER, CLEARED EVERY FRAME - MATCHING MCC
// ---------------------------------------------------------
// Renderer3DImpl_updateCameraData.cpp creates its own depth-stencil texture and clears it to 1.0 every frame; the
// overlay is depth-tested against ITSELF and never against world geometry. We do exactly the same. Capturing
// UE5's scene depth buffer is explicitly out of scope - do not "fix" this.
//
// COORDINATE FRAME: everything is BLAM (right-handed, Z up, +Y LEFT), because that is the frame trigger volume tag
// data already lives in. The camera arrives already converted (HCEGetCameraData). Blam being right-handed is what
// makes DirectXMath's *RH matrix builders correct here with no mirroring - XMMatrixLookAtRH derives
// xaxis = cross(up, eye-at) = forward x up, which at yaw 0 is (0,-1,0), i.e. Blam's RIGHT. Do not swap in an LH
// builder "because D3D12"; a sign error here mirrors the whole overlay and has already happened once.
//
// FOV: the UE camera reports a HORIZONTAL field of view. XMMatrixPerspectiveFovRH wants a VERTICAL one.
// vfov = 2*atan(tan(hfov/2) * height/width). Feeding it the horizontal value directly is a real, silent bug.
//
// NOT IMPLEMENTED - TEXTURES AND SPRITES
// --------------------------------------
// drawSprite / drawCenteredSprite are no-ops that return a zero rect, and the optional `texture` argument of
// drawTriangle / drawTriangleCollection is IGNORED (the geometry still draws, untextured). Reasons, in order:
//   1. Nothing under HaloCER calls either. The MCC consumers that do (Waypoint3D's crosshair, TriggerOverlay's
//      "Patterned" interior style) are D3D11-only cheats that are never constructed in this process.
//   2. Binding a texture means calling SetDescriptorHeaps on the GAME's command list, which would unbind the SRV
//      heap D3D12Hook bound for imgui and that ImGui_ImplDX12_RenderDrawData depends on immediately after us. The
//      cost of getting that wrong is the game's own rendering, which is not a trade worth making for a feature
//      with no callers.
// They are no-ops rather than throws precisely so a future caller degrades instead of crashing.
// ================================================================================================================

class Renderer3DImplD3D12 : public IRenderer3D
{
public:
	Renderer3DImplD3D12(GameState game, IDIContainer& dicon);
	~Renderer3DImplD3D12();

	Renderer3DImplD3D12(const Renderer3DImplD3D12&) = delete;
	Renderer3DImplD3D12& operator=(const Renderer3DImplD3D12&) = delete;

	// ---- per-frame, called by Render3DEventProvider ONLY (it owns the concrete type) ----

	// Updates the camera, (re)creates anything size-dependent, and records the frame's setup commands.
	// NEVER THROWS - returns false and the frame is skipped, which is always better than unwinding through
	// DXGI/UE5 frames. Returns false forever once initialisation has failed, so the ImGui fallback can take over.
	bool beginFrame(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
		SimpleMath::Vector2 screenSize, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, DXGI_FORMAT backBufferFormat);

	// Restores the render-target binding WITHOUT our depth buffer, then forgets the command list. Must be called
	// exactly once after every beginFrame() that returned true. Never throws.
	void endFrame();

	// True once beginFrame has latched an unrecoverable initialisation failure. Consumers use this to fall back
	// to ImGui drawing rather than silently rendering nothing.
	bool hasFailed() const { return mInitFailed; }
	// True if a frame was successfully begun recently (used by consumers to decide whether the 3D path is live).
	bool isRendering() const { return mCommandList != nullptr; }

	// ---- IRenderer3D ----

	SimpleMath::Vector3 worldPointToScreenPosition(SimpleMath::Vector3 worldPointPosition, bool shouldFlipBehind) override;
	SimpleMath::Vector3 worldPointToScreenPositionClamped(SimpleMath::Vector3 worldPointPosition, int screenEdgeOffset, bool* appliedClamp) override;
	float cameraDistanceToWorldPoint(SimpleMath::Vector3 worldPointPosition) override;
	bool pointOnScreen(const SimpleMath::Vector3& worldPointPosition) override;
	bool pointBehindCamera(const SimpleMath::Vector3& worldPointPosition) override;

	RECTF drawSprite(TextureEnum texture, SimpleMath::Vector2 screenPosition, float spriteScale, SimpleMath::Vector4 spriteColor) override;
	RECTF drawCenteredSprite(TextureEnum texture, SimpleMath::Vector2 screenPosition, float spriteScale, SimpleMath::Vector4 spriteColor) override;

	void renderSphere(const SimpleMath::Vector3& position, const SimpleMath::Vector4& color, const float& scale, const bool& isWireframe) override;

	const SimpleMath::Vector3 getCameraPosition() override { return mCameraPosition; }
	const DirectX::BoundingFrustum& getCameraFrustum() override { return mFrustumViewWorld; }

	void drawTriangle(const std::array<SimpleMath::Vector3, 3>& vertexPositions, const SimpleMath::Vector4& color, CullingOption cullingOption, std::optional<TextureEnum> texture) override;
	void drawTriangleCollection(const IModelTriangles* model, const SimpleMath::Vector4& color, CullingOption cullingOption, std::optional<TextureEnum> texture) override;
	void drawEdge(const SimpleMath::Vector3& edgeStart, const SimpleMath::Vector3& edgeEnd, const SimpleMath::Vector4& color) override;
	void drawEdgeCollection(const IModelEdges* model, const SimpleMath::Vector4& color) override;

private:
	template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

	// ---- pipeline variants -------------------------------------------------------------------------------
	// Ten PSOs, all built up front so nothing is ever created on the render thread.
	//
	// Blend/depth pairs mirror what DirectXTK does on the MCC path:
	//   * "Alpha"  = alpha blend + DepthRead   (DepthEnable, WriteMask ZERO, LESS_EQUAL). Every triangle and
	//                edge draw uses this, which is exactly what Renderer3DImpl_Geometry.cpp does.
	//   * "Opaque" = blend off   + DepthDefault(DepthEnable, WriteMask ALL,  LESS_EQUAL). Only renderSphere with
	//                a fully opaque colour uses it, matching GeometricPrimitive::Draw's own rule.
	// NOTE the cull-mode names are the D3D ones, NOT CullingOption's. They do not line up, and pretending they do
	// is how you silently invert culling: DirectXTK's CommonStates::CullClockwise() is D3D11_CULL_FRONT and
	// CullCounterClockwise() is D3D11_CULL_BACK (they name the winding that gets REMOVED, with
	// FrontCounterClockwise = FALSE). Renderer3DImpl_Geometry.cpp maps CullingOption::CullBack -> CullClockwise
	// and CullingOption::CullFront -> CullCounterClockwise, so trianglePsoFor() reproduces exactly that.
	enum class Pso : size_t
	{
		TriangleCullNoneAlpha = 0,
		TriangleCullFrontFacesAlpha,     // D3D12_CULL_MODE_FRONT  == DXTK CullClockwise        == CullingOption::CullBack
		TriangleCullBackFacesAlpha,      // D3D12_CULL_MODE_BACK   == DXTK CullCounterClockwise == CullingOption::CullFront
		TriangleCullNoneOpaque,
		TriangleCullFrontFacesOpaque,
		TriangleCullBackFacesOpaque,
		WireframeAlpha,
		WireframeOpaque,
		LineAlpha,
		LineOpaque,
		Count
	};

	// ---- per-frame upload ring ---------------------------------------------------------------------------
	// WHY 16 SLOTS: D3D12Hook waits on its fence for render slot (ordinal % framesInFlight) before it resets
	// that allocator, so by the time our N-th frame starts, submission N - framesInFlight has provably retired.
	// framesInFlight is the swap chain's buffer count, and DXGI caps that at DXGI_MAX_SWAP_CHAIN_BUFFERS = 16.
	// A 16-deep ring is therefore not a guess, it is a proof: the buffer we are about to overwrite was last
	// submitted at least 16 hook frames ago. (We advance our ordinal only on frames we actually record, and the
	// hook's ordinal advances at least as often, so our gap is never smaller than the hook's.)
	static constexpr size_t kUploadRingSize = 16;

	struct UploadBuffer
	{
		ComPtr<ID3D12Resource> resource;
		uint8_t* cpu = nullptr;   // persistently mapped; upload heaps are designed for this
		UINT64 capacity = 0;
		UINT64 used = 0;
	};

	struct UploadSlot
	{
		UploadBuffer vertices;
		UploadBuffer indices;
	};

	// An object that has been REPLACED (an upload buffer grown, the depth texture resized, a PSO rebuilt after a
	// back buffer format change) while a command list the GPU has not finished may still reference it. D3D12
	// command lists do NOT hold references to the resources or pipeline states they use - keeping them alive
	// until the GPU is done is the application's job - so dropping the last reference here would be a GPU-side
	// use-after-free, i.e. device removed, i.e. the GAME dies.
	//
	// Held as IUnknown so one list covers resources and pipeline states alike. Released once the frame ordinal
	// has advanced a whole ring past the frame it was retired on (see collectRetiredResources).
	struct RetiredResource
	{
		ComPtr<IUnknown> object;
		uint64_t retiredAtOrdinal = 0;
	};

	// ---- init -------------------------------------------------------------------------------------------
	bool initialise(ID3D12Device* pDevice, DXGI_FORMAT backBufferFormat);   // never throws
	bool createRootSignatureAndShaders();
	bool createPipelineStates(DXGI_FORMAT backBufferFormat);
	bool createDepthStencil(UINT width, UINT height);
	bool ensureUploadCapacity(UploadBuffer& buffer, UINT64 bytesNeeded, UINT64 initialCapacity);
	void collectRetiredResources();
	void fail(const char* what);   // latch mInitFailed and log once

	// ---- draw helpers -----------------------------------------------------------------------------------
	bool beginDraw(Pso pso, const SimpleMath::Matrix& worldViewProjection, const SimpleMath::Vector4& color);
	bool appendVertices(const DirectX::VertexPosition* vertices, size_t count, D3D12_VERTEX_BUFFER_VIEW& outView);
	bool appendIndices(const uint16_t* indices, size_t count, D3D12_INDEX_BUFFER_VIEW& outView);
	bool drawIndexed(Pso pso, const SimpleMath::Matrix& worldViewProjection, const SimpleMath::Vector4& color,
		const DirectX::VertexPosition* vertices, size_t vertexCount, const uint16_t* indices, size_t indexCount,
		D3D_PRIMITIVE_TOPOLOGY topology);
	static Pso trianglePsoFor(CullingOption cullingOption);
	bool updateCamera(SimpleMath::Vector2 screenSize);

	// ---- LATCHED camera diagnostic -----------------------------------------------------------------------
	// Camera failures on this title are silent by nature (a wrong offset reads as a plausible number, a
	// missing hook reads as "not ready yet"), and chasing them without this cost several rounds on the ImGui
	// version. So the renderer states which source it resolved and where that put the camera - but ONLY when
	// the answer CHANGES. There is deliberately no per-frame logging on this path.
	enum class CameraSource { Unknown, UePov, SimFallback, NoCameraService, NoFallbackAvailable, FallbackThrew, RejectedValues, OriginRejected };
	CameraSource mLoggedCameraSource = CameraSource::Unknown;
	// Rate limit on top of the change-latch - see logCameraSourceIfChanged for why the latch alone is not enough.
	uint32_t mLastCameraSourceLogTick = 0;
	static constexpr uint32_t kCameraSourceLogMinIntervalMs = 3000;
	void logCameraSourceIfChanged(CameraSource source, const SimpleMath::Vector3& position, float horizontalFovDegrees);

	// Latched: the sim camera-entry position read back degenerate and we substituted the player position.
	bool mLoggedFallbackEyeSubstitution = false;

	// ---- owned D3D12 objects ----------------------------------------------------------------------------
	ID3D12Device* mDevice = nullptr;   // NOT owned - belongs to the game. Identity check only, plus resource creation.
	ComPtr<ID3D12RootSignature> mRootSignature;
	std::array<ComPtr<ID3D12PipelineState>, (size_t)Pso::Count> mPipelineStates{};
	ComPtr<ID3D12DescriptorHeap> mDsvHeap;
	ComPtr<ID3D12Resource> mDepthStencil;
	D3D12_CPU_DESCRIPTOR_HANDLE mDsvHandle{};
	std::vector<uint8_t> mVertexShaderBytecode;
	std::vector<uint8_t> mPixelShaderBytecode;

	std::array<UploadSlot, kUploadRingSize> mUploadRing{};
	std::vector<RetiredResource> mRetiredResources;
	uint64_t mFrameOrdinal = 0;

	// ---- per-frame, valid only between beginFrame and endFrame -------------------------------------------
	ID3D12GraphicsCommandList* mCommandList = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE mBackBufferRtv{};
	size_t mSlotIndex = 0;
	Pso mCurrentPso = Pso::Count;

	// ---- state -------------------------------------------------------------------------------------------
	bool mInitialised = false;
	bool mInitFailed = false;
	bool mLoggedTextureStub = false;
	DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	UINT mDepthWidth = 0;
	UINT mDepthHeight = 0;

	// ---- camera ------------------------------------------------------------------------------------------
	SimpleMath::Vector3 mCameraPosition{};
	SimpleMath::Vector3 mCameraForward{ 1.f, 0.f, 0.f };
	SimpleMath::Vector3 mCameraUp{ 0.f, 0.f, 1.f };
	SimpleMath::Matrix mViewMatrix;
	SimpleMath::Matrix mProjectionMatrix;
	SimpleMath::Matrix mViewProjectionMatrix;
	SimpleMath::Vector2 mScreenSize{ 0.f, 0.f };
	SimpleMath::Vector2 mScreenCenter{ 0.f, 0.f };
	DirectX::BoundingFrustum mFrustumViewWorld;

	// A unit sphere (radius 0.5, i.e. diameter 1) so renderSphere matches GeometricPrimitive::CreateSphere's
	// default size on the MCC path exactly. Built once on the CPU; there is no DirectXTK12 in this project.
	std::vector<DirectX::VertexPosition> mSphereVertices;
	std::vector<uint16_t> mSphereIndices;

	// ---- injected services -------------------------------------------------------------------------------
	std::weak_ptr<HCEGetCameraData> mCameraDataWeak;
	std::optional<std::weak_ptr<HCEGetPlayerState>> mPlayerStateOptionalWeak;   // fallback camera only
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;
	std::weak_ptr<IMCCStateHook> mMccStateHookWeak;
	std::weak_ptr<IMessagesGUI> mMessagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> mRuntimeExceptions;
};
