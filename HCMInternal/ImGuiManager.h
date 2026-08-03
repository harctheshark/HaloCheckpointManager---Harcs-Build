#pragma once
#include "D3D11Hook.h"
#include "D3D12Hook.h"

// imgui
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_stdlib.h"
#include "MultilevelPointer.h"
#include "DirectXRenderEvent.h"
#include "D3D12RenderEvent.h"
// todo move stuff to impl

// Singleton: on construction, just subscribes to the graphics hook's presentHookEvent.
// The callback function will initialize ImGui resources (and WndPrc hook) and invoke an ImGuiRenderEvent for other classes to listen to.
// Destruction releases ImGui resources and unsubscribes to presentHookEvent.
//
// TWO BACKENDS, ONE MANAGER.
// MCC (MCC-Win64-Shipping.exe / MCCWinStore-Win64-Shipping.exe) is D3D11 and drives us from
// D3D11Hook. Halo Campaign Evolved (HaloCampaignEvolved.exe, UE 5.5.4) is D3D12 and drives us from
// D3D12Hook. Which one is chosen is decided ONCE, in App.h, from GetMCCVersion's process type, and
// is fixed for the lifetime of the object (mBackend is const).
// Everything that is not the renderer backend - the ImGui context, the win32 platform backend, the
// WndProc chain, the style, the fonts, and all four render events - is shared verbatim between the
// two paths. Only three things differ:
//   1. ImGui_ImplDX11_* vs ImGui_ImplDX12_*,
//   2. the D3D11-only OMSetRenderTargets (D3D12Hook already binds the RTV on the command list it
//      hands us, because only the hook knows which back buffer this frame is),
//   3. which foreground DirectX event is fired - see D3D12RenderEvent.h for why there are two.
class ImGuiManager : public std::enable_shared_from_this<ImGuiManager>
{
public:
	enum class RenderBackend { D3D11, D3D12 };

private:
	// Fixed at construction. There is exactly one graphics hook in the process and it never changes
	// backend, so this is const and every branch on it is a predictable, non-racing read.
	const RenderBackend mBackend;

	std::weak_ptr<D3D11Hook> m_d3d;    // non-empty iff mBackend == D3D11
	std::weak_ptr<D3D12Hook> m_d3d12;  // non-empty iff mBackend == D3D12



	static ImGuiManager* instance; 	// Private Singleton instance so static hooks/callbacks can access
	static inline std::mutex mDestructionGuard{};
	// Set at the TOP of ~ImGuiManager, before mDestructionGuard is taken. Checked by the D3D12
	// present handler after it acquires the guard.
	// WHY: removeCallback() cannot recall a callback whose body has already been entered. A render
	// thread that is inside onPresentHookEventD3D12 and blocked on mDestructionGuard will resume the
	// instant the destructor releases it - i.e. AFTER ImGui_ImplDX12_Shutdown() - and would then call
	// ImGui_ImplDX12_NewFrame() / RenderDrawData() with no renderer backend. Under D3D11 that is a
	// null deref inside imgui; under D3D12 it means recording draws that reference a freed PSO/root
	// signature into the game's command list, which faults the GPU and kills the GAME.
	// Only the D3D12 path consults it: the D3D11 handler is left exactly as it shipped.
	static inline std::atomic_bool mShuttingDown{ false };

	// WndProc hook, we use SetWindowLongPtr to set it up
	static LRESULT __stdcall mNewWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);  // Handles ImGui input
	static WNDPROC mOldWndProc; // original wndProc for input we don't care about
	HWND m_windowHandle = nullptr; // Needed for creating wndProc hook, will get from SwapChain description

	// When set, mNewWndProc stops forwarding INPUT messages to the game (see the switch in mNewWndProc).
	// This is the fallback "GUI showing blocks game input" for titles with no engine-level block-input service
	// (Halo Campaign Evolved). Set/cleared by HCMInternalGUI when its window opens/closes.
	static inline std::atomic<bool> sSwallowGameInput{ false };

public:
	// Fallback input blocking for games without a blockGameInputService. Safe to call from any thread.
	static void setSwallowGameInput(bool swallow) { sSwallowGameInput.store(swallow, std::memory_order_relaxed); }
private:

	// presentHookEvent handles. EXACTLY ONE of these is ever non-null, decided by which constructor
	// ran. They are unique_ptrs (rather than plain members, as the D3D11 one used to be) purely
	// because ScopedCallback has no default constructor and is neither copyable nor movable, so a
	// bare member would force every constructor to supply an event of that exact signature.
	using D3D11PresentEvent = eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>;
	std::unique_ptr<ScopedCallback<D3D11PresentEvent>> presentEventCallback;
	std::unique_ptr<ScopedCallback<D3D12PresentEvent>> presentEventCallbackD3D12;
	D3D11PresentEvent::Handle presentEventCallbackTest; // pre-existing, unused

	// What we run when presentHookEvent is invoked
	void onPresentHookEvent(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*);
	void onPresentHookEventD3D12(ID3D12Device*, ID3D12GraphicsCommandList*, IDXGISwapChain3*, D3D12_CPU_DESCRIPTOR_HANDLE);

	// initialise resources in the first onPresentHookEvent
	void initializeImGuiResources(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*);
	void initializeImGuiResourcesD3D12(IDXGISwapChain3*);
	// The backend-independent half of initialisation (context + win32 backend), and the style/font
	// setup. Shared by both paths so the GUI is pixel-identical under D3D11 and D3D12.
	void initializeImGuiContextAndPlatform(HWND windowHandle);
	void applyImGuiStyleAndFonts();
	bool m_isImguiInitialized = false;

	// Actual ImGui resources
	ImFont* mRescalableMonospacedFont = nullptr; // initialised in initializeImGuiResources, used in background overlays


	eventpp::ScopedRemover<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>> testPresentCallback;

public:

	// MCC / D3D11. Unchanged signature - App.h's MCC branch calls this exactly as before.
	explicit ImGuiManager(std::shared_ptr<D3D11Hook> d3d, std::shared_ptr<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>> presentEvent)
		: mBackend(RenderBackend::D3D11),
		m_d3d(d3d)
	{
		// Built into a local FIRST so the sequence stays exactly what it was when the subscription
		// lived in the member-init list: subscribe, then the singleton check, then publish `instance`
		// last. (If construction of the callback ever threw, nothing would be left half-published.)
		auto callback = std::make_unique<ScopedCallback<D3D11PresentEvent>>(
			presentEvent,
			[this](ID3D11Device* a, ID3D11DeviceContext* b, IDXGISwapChain* c, ID3D11RenderTargetView* d) { onPresentHookEvent(a, b, c, d); });

		if (instance != nullptr)
		{
			throw HCMInitException("Cannot have more than one ImGuiManager");
		}
		presentEventCallback = std::move(callback);
		mShuttingDown.store(false, std::memory_order_release); // it is static, so reset it per instance
		instance = this;
	}

	// Halo Campaign Evolved / D3D12.
	explicit ImGuiManager(std::shared_ptr<D3D12Hook> d3d12, std::shared_ptr<D3D12PresentEvent> presentEvent)
		: mBackend(RenderBackend::D3D12),
		m_d3d12(d3d12)
	{
		auto callback = std::make_unique<ScopedCallback<D3D12PresentEvent>>(
			presentEvent,
			[this](ID3D12Device* a, ID3D12GraphicsCommandList* b, IDXGISwapChain3* c, D3D12_CPU_DESCRIPTOR_HANDLE d) { onPresentHookEventD3D12(a, b, c, d); });

		if (instance != nullptr)
		{
			throw HCMInitException("Cannot have more than one ImGuiManager");
		}
		presentEventCallbackD3D12 = std::move(callback);
		mShuttingDown.store(false, std::memory_order_release); // it is static, so reset it per instance
		instance = this;
	}



	~ImGuiManager(); // Destructor releases imgui resources and unsubscribes from PresentHookEvent



	// Our own events we will invoke in onPresentHookEvent
	// Things that want to render w/ ImGui will listen to this
	std::shared_ptr<RenderEvent> BackgroundRenderEvent = std::make_shared<RenderEvent>(); // for overlays
	std::shared_ptr<RenderEvent> MidgroundRenderEvent = std::make_shared<RenderEvent>(); // for main gui / messages
	std::shared_ptr<RenderEvent> ForegroundRenderEvent = std::make_shared<RenderEvent>(); // for modal dialogues / popups
	// Fired ONLY on the D3D11 path. Its payload is live D3D11 interfaces that do not exist under D3D12.
	std::shared_ptr<DirectXRenderEvent> ForegroundDirectXRenderEvent = std::make_shared<DirectXRenderEvent>(); // for modal dialogues / popups
	// Fired ONLY on the D3D12 path. See D3D12RenderEvent.h for why this is a separate event rather
	// than a widened/erased DirectXRenderEvent.
	std::shared_ptr<D3D12RenderEvent> ForegroundD3D12RenderEvent = std::make_shared<D3D12RenderEvent>();


	// Banned operations for singleton
	ImGuiManager(const ImGuiManager& arg) = delete; // Copy constructor
	ImGuiManager(const ImGuiManager&& arg) = delete;  // Move constructor
	ImGuiManager& operator=(const ImGuiManager& arg) = delete; // Assignment operator
	ImGuiManager& operator=(const ImGuiManager&& arg) = delete; // Move operator

	RenderBackend getRenderBackend() const { return mBackend; }

	SimpleMath::Vector2 getScreenSize() {
		if (mBackend == RenderBackend::D3D12)
		{
			if (m_d3d12.expired()) return { 0, 0 };
			return m_d3d12.lock()->getScreenSize();
		}
		if (m_d3d.expired()) return { 0, 0 };
		return m_d3d.lock()->getScreenSize();
	}
	SimpleMath::Vector2 getScreenCenter() {
		if (mBackend == RenderBackend::D3D12)
		{
			if (m_d3d12.expired()) return { 0, 0 };
			return m_d3d12.lock()->getScreenCenter();
		}
		if (m_d3d.expired()) return { 0, 0 };
		return m_d3d.lock()->getScreenCenter();
	}

	// Might be nullptr
	static ImFont * const getRescalableMonospacedFont()
	{
		if (!instance)
			return nullptr;
		return instance->mRescalableMonospacedFont; // still might be null
	}

};

