#include "pch.h"
#include "Render3DEventProvider.h"
#include "Renderer3DImpl.h"
#include "Renderer3DImplD3D12.h"



void Render3DEventProvider::onDirectXRenderEvent(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, SimpleMath::Vector2 screenSize, ID3D11RenderTargetView* pMainRenderTargetView)
{
	// HaloCER renders through onD3D12RenderEvent instead, and ImGuiManager never fires the D3D11 event on the
	// D3D12 path anyway - so this is unreachable there. It is a compile-time-constant-false test for every MCC
	// game, which is what makes it provably free of behaviour change for them.
	if (static_cast<GameState::Value>(mGame) == GameState::Value::HaloCER) return;

	if (gameIsValid == false) return;
	ScopedAtomicBool lock(currentlyRendering);

	// only fire 3D renderer event if anyone is actually subscribed to it - because updating the camera data is expensive
	if (mRender3DEvent->isEventSubscribed())
	{


#define log_null_and_throw(x) if (!x) { auto s = std::format("null pointer to {}", nameof(x)); PLOG_ERROR << s; throw HCMRuntimeException(s); }
		log_null_and_throw(pDevice);
		log_null_and_throw(pDeviceContext);
		log_null_and_throw(pMainRenderTargetView);
		log_null_and_throw(p3DRenderer);
		log_null_and_throw(mRender3DEvent);

		// Same call as before, just made on the concrete Renderer3DImpl<mGame> (bound in the constructor)
		// rather than through IRenderer3D, which no longer carries a D3D11-typed method.
		if (mUpdateCameraDataD3D11(pDevice, pDeviceContext, screenSize, pMainRenderTargetView))
		{
			mRender3DEvent->fireEvent(mGame, p3DRenderer.get());
		}

	}
}


// Halo Campaign Evolved / D3D12. Subscribed only for GameState::HaloCER.
//
// CONTRACT WITH D3D12Hook (see D3D12RenderEvent.h): pCommandList is already Reset() and recording, the back
// buffer is already in RENDER_TARGET state and bound, and we must NOT Close/Execute/Signal/barrier. A subscriber
// that throws costs the whole overlay AND kills HCM, so nothing here is allowed to propagate an exception.
void Render3DEventProvider::onD3D12RenderEvent(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, SimpleMath::Vector2 screenSize, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, DXGI_FORMAT backBufferFormat)
{
	if (gameIsValid == false) return;
	if (!p3DRendererD3D12) return;
	ScopedAtomicBool lock(currentlyRendering);

	if (!mRender3DEvent || !mRender3DEvent->isEventSubscribed()) return;

	// Never throws; returns false (and skips the frame) on anything it does not like, and false forever once
	// initialisation has failed - which is how consumers know to fall back to ImGui drawing.
	if (!p3DRendererD3D12->beginFrame(pDevice, pCommandList, screenSize, backBufferRTV, backBufferFormat)) return;

	try
	{
		mRender3DEvent->fireEvent(mGame, p3DRendererD3D12.get());
	}
	catch (HCMRuntimeException& ex)
	{
		// Consumers are expected to handle their own runtime exceptions; this is the last line of defence
		// before the exception would reach DXGI/UE5 frames, which is not survivable.
		LOG_ONCE(PLOG_ERROR << "A Render3DEvent subscriber threw an HCMRuntimeException on the D3D12 path; suppressing further reports");
		PLOG_VERBOSE << "Render3DEvent subscriber error: " << ex.what();
	}
	catch (...)
	{
		LOG_ONCE(PLOG_ERROR << "A Render3DEvent subscriber threw an unknown exception on the D3D12 path; suppressing further reports");
	}

	// MUST run even if a subscriber threw: it is what unbinds our depth buffer before
	// ImGui_ImplDX12_RenderDrawData records on the same list with a DSVFormat = UNKNOWN pipeline state.
	p3DRendererD3D12->endFrame();
}

Render3DEventProvider::Render3DEventProvider(GameState gameImpl, IDIContainer& dicon)
	: mGame(gameImpl),
	gameIsValid(dicon.Resolve<IMCCStateHook>().lock()->isGameCurrentlyPlaying(gameImpl)),
	mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) {onGameStateChanged(s); }),
	directXRenderEventCallback(dicon.Resolve<DirectXRenderEvent>().lock(), [this](ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, SimpleMath::Vector2 screenSize, ID3D11RenderTargetView* pMainRenderTargetView) { onDirectXRenderEvent(pDevice, pDeviceContext, screenSize, pMainRenderTargetView); })
{
	// Binds mUpdateCameraDataD3D11 to the concrete instance. The lambda captures a RAW pointer that p3DRenderer
	// owns and that lives exactly as long as this object, so there is no lifetime question.
#define MAKE_D3D11_RENDERER(gameValue)                                                                          \
	{                                                                                                           \
		auto renderer = std::make_unique<Renderer3DImpl<gameValue>>(gameImpl, dicon);                           \
		auto* concrete = renderer.get();                                                                        \
		mUpdateCameraDataD3D11 = [concrete](ID3D11Device* a, ID3D11DeviceContext* b, SimpleMath::Vector2 c, ID3D11RenderTargetView* d) \
			{ return concrete->updateCameraData(a, b, c, d); };                                                 \
		p3DRenderer = std::move(renderer);                                                                      \
	}

	switch (gameImpl)
	{
	case GameState::Value::Halo1:
		MAKE_D3D11_RENDERER(GameState::Value::Halo1);
		break;

	case GameState::Value::Halo2:
		MAKE_D3D11_RENDERER(GameState::Value::Halo2);
		break;

	case GameState::Value::Halo3:
		MAKE_D3D11_RENDERER(GameState::Value::Halo3);
		break;

	case GameState::Value::Halo3ODST:
		MAKE_D3D11_RENDERER(GameState::Value::Halo3ODST);
		break;

	case GameState::Value::HaloReach:
		MAKE_D3D11_RENDERER(GameState::Value::HaloReach);
		break;

	case GameState::Value::Halo4:
		MAKE_D3D11_RENDERER(GameState::Value::Halo4);
		break;

	case GameState::Value::HaloCER:
	{
		// Halo Campaign Evolved: D3D12. Note this subscribes to a DIFFERENT event - ImGuiManager fires
		// ForegroundD3D12RenderEvent (and never the D3D11 one) whenever the graphics backend is D3D12.
		p3DRendererD3D12 = std::make_unique<Renderer3DImplD3D12>(gameImpl, dicon);
		d3d12RenderEventCallback = std::make_unique<ScopedCallback<D3D12RenderEvent>>(
			dicon.Resolve<D3D12RenderEvent>().lock(),
			[this](ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, SimpleMath::Vector2 screenSize, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, DXGI_FORMAT backBufferFormat)
			{ onD3D12RenderEvent(pDevice, pCommandList, screenSize, backBufferRTV, backBufferFormat); });
		break;
	}

	default:
		throw HCMInitException("not impl yet");
	}
#undef MAKE_D3D11_RENDERER
}

void Render3DEventProvider::onGameStateChanged(const MCCState& newMCCState)
{
	gameIsValid = (newMCCState.currentPlayState == PlayState::Ingame && newMCCState.currentGameState == mGame);
	PLOG_DEBUG << "updating game validity to " << (gameIsValid ? "true" : "false");
}

bool Render3DEventProvider::d3d12RendererHasFailed() const
{
	return p3DRendererD3D12 && p3DRendererD3D12->hasFailed();
}

Render3DEventProvider::~Render3DEventProvider()
{
	if (currentlyRendering)
	{
		currentlyRendering.wait(true);
	}
	// quickly manually kill the callback before it gets called again
	directXRenderEventCallback.removeCallback();
	if (d3d12RenderEventCallback) d3d12RenderEventCallback->removeCallback();

	PLOG_VERBOSE << "~" << getName();
}
