#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IRenderer3D.h"
#include "DirectXRenderEvent.h"
#include "D3D12RenderEvent.h"
#include "IMCCStateHook.h"
#include "ObservedEvent.h"

using Render3DEvent = eventpp::CallbackList<void(GameState, IRenderer3D*)>;

// HaloCER only. Forward declared so this header stays free of <d3d12.h>-heavy renderer internals; the
// definition is included by Render3DEventProvider.cpp, which is where ~Render3DEventProvider lives (which is
// what a unique_ptr to an incomplete type requires).
class Renderer3DImplD3D12;

// Fires a 3DRender event once per frame, passing a IRenderer3D by ref to subscribers.
// Will not fire event if the event has no subscribers (skips expensive camera calculations).
//
// TWO PARALLEL RENDERERS, SELECTED BY GAME.
// The six MCC games get Renderer3DImpl (D3D11 + DirectXTK), driven by ImGuiManager's
// ForegroundDirectXRenderEvent, exactly as they always have. Halo Campaign Evolved (HaloCER) is a
// D3D12 title, so it gets Renderer3DImplD3D12 driven by ForegroundD3D12RenderEvent instead. The two
// implementations share nothing but the IRenderer3D interface, and that is deliberate: the MCC path
// must stay behaviourally identical, so it is not refactored to accommodate the new one.
//
// Only ONE of the two branches is ever live in a given process (HaloCER is a separate title from MCC,
// and ImGuiManager fires exactly one of the two DirectX events for the lifetime of the process).
class Render3DEventProvider : public IOptionalCheat {
private:
	GameState mGame;

	// ---- MCC / D3D11 ------------------------------------------------------------------------------
	ScopedCallback<DirectXRenderEvent> directXRenderEventCallback; // fired by ImGuiManager
	void onDirectXRenderEvent(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, SimpleMath::Vector2 screenSize, ID3D11RenderTargetView* pMainRenderTargetView);

	// Bound at construction to the concrete Renderer3DImpl<mGame> that p3DRenderer owns.
	// WHY A std::function AND NOT A VIRTUAL: updateCameraData takes live D3D11 interfaces, so it cannot live on
	// IRenderer3D any more (a D3D12 implementation has to be able to sit behind that interface). Renderer3DImpl
	// is a class TEMPLATE, so "call it on the concrete type" means capturing the concrete pointer at the point
	// where the switch below already knows it. The BODY of updateCameraData is untouched.
	using UpdateCameraDataD3D11Fn = std::function<bool(ID3D11Device*, ID3D11DeviceContext*, SimpleMath::Vector2, ID3D11RenderTargetView*)>;
	UpdateCameraDataD3D11Fn mUpdateCameraDataD3D11;

	// ---- HaloCER / D3D12 --------------------------------------------------------------------------
	// unique_ptr because it is only subscribed on the HaloCER path, and ScopedCallback is neither default
	// constructible nor movable.
	std::unique_ptr<ScopedCallback<D3D12RenderEvent>> d3d12RenderEventCallback;
	void onD3D12RenderEvent(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, SimpleMath::Vector2 screenSize, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, DXGI_FORMAT backBufferFormat);

	std::atomic_bool currentlyRendering = false; // locked by above event and waited for by this classes destructer


	bool gameIsValid;
	void onGameStateChanged(const MCCState& newMCCState);
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

	// MCC only. Owned through the interface exactly as before - IRenderer3D deliberately has no virtual
	// destructor (see IRenderer3D.h), and giving it one now would start running ~Renderer3DImpl for the six MCC
	// games on a path that has never executed.
	std::unique_ptr<IRenderer3D> p3DRenderer;
	// HaloCER only. Owned as the CONCRETE type so ~Renderer3DImplD3D12 actually runs and its D3D12 objects are
	// dealt with properly.
	std::unique_ptr<Renderer3DImplD3D12> p3DRendererD3D12;

	std::shared_ptr<ObservedEvent<Render3DEvent>> mRender3DEvent = std::make_shared<ObservedEvent<Render3DEvent>>();
public:

	// Consumers should avoid subscribing to the render3DEvent unless they actually need to (since event provider will skip expensively updating camera data every frame IF no one is subscribed)
	std::shared_ptr<ObservedEvent<Render3DEvent>> getRender3DEvent() { return mRender3DEvent; }
	Render3DEventProvider(GameState game, IDIContainer& dicon);
	~Render3DEventProvider();

	std::string_view getName() override { return nameof(Render3DEventProvider); }

	// True when the D3D12 renderer exists but has latched an unrecoverable initialisation failure, i.e. the 3D
	// path will never draw anything and a consumer should fall back to ImGui. Always false on the MCC path.
	bool d3d12RendererHasFailed() const;
};
