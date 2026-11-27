#pragma once
#include "ModuleHook.h"
#include "ModuleHookManager.h"
#include "PointerDataStore.h"
#include "OBSHookDiscovery.h"

// imgui
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_stdlib.h"
// directx 11
#include <d3d11.h>

// TODO: move shit to impl so we can hind all the headers

// Define the DX functions we're going to hook
typedef HRESULT __stdcall DX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT __stdcall DX11ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
// IDXGISwapChain1::Present1 (slot 22). The last parameter is const DXGI_PRESENT_PARAMETERS* - void* here so this header
// does not need dxgi1_2.h.
typedef HRESULT __stdcall DX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const void* pPresentParameters);

//SCALAR TO ImVec4 OPERATIONS
inline ImVec4 operator + (const ImVec4& v1, float s) { return ImVec4(v1.x + s, v1.y + s, v1.z + s, v1.w + s); }
inline ImVec4 operator - (const ImVec4& v1, float s) { return ImVec4(v1.x - s, v1.y - s, v1.z - s, v1.w - s); }
inline ImVec4 operator * (const ImVec4& v1, float s) { return ImVec4(v1.x * s, v1.y * s, v1.z * s, v1.w * s); }
inline ImVec4 operator / (const ImVec4& v1, float s) { return ImVec4(v1.x / s, v1.y / s, v1.z / s, v1.w / s); }


//ImVec2 TO ImVec4 OPERATIONS
inline ImVec4 operator + (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x + v2.x, v1.y + v2.y, v1.z + v2.z, v1.w + v2.w); }
inline ImVec4 operator - (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x - v2.x, v1.y - v2.y, v1.z - v2.z, v1.w - v2.w); }
inline ImVec4 operator * (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x * v2.x, v1.y * v2.y, v1.z * v2.z, v1.w * v2.w); }
inline ImVec4 operator / (const ImVec4& v1, const ImVec4& v2) { return ImVec4(v1.x / v2.x, v1.y / v2.y, v1.z / v2.z, v1.w / v2.w); }


// Singleton: on construction, attaches hooks (VMT hook so our stuff shows up in OBS etc) to the games Present and ResizeBuffers function.
// The hooks will invoke presentHookEvent for other classes to listen to.
// On destruction, hooks will be unattached.


class D3D11Hook : public std::enable_shared_from_this<D3D11Hook>
{
private:
	static D3D11Hook* instance; 	// Private Singleton instance so static hooks/callbacks can access
	//static inline std::mutex mDestructionGuard{}; // Protects against Singleton destruction while hooks are executing
	static inline std::atomic_bool presentHookRunning = false;
	// Our hook functions
	static DX11Present newDX11Present;
	static DX11ResizeBuffers newDX11ResizeBuffers;

	static DX11Present newDX11PresentOBSBypass;
	static DX11Present mandatoryManualRender;
	static DX11Present1 newDX11Present1;
	// the work every hooked Present does before the frame goes on (device init, the render events). false = the device
	// could not be initialised (HCM is shutting itself down) - the caller just presents.
	static bool presentWork(D3D11Hook* d3d, IDXGISwapChain* pSwapChain);

	// ---- the INLINE path (2026-11-24, Metro Redux): titles with no pIDXGISwapChain pointer data ----
	// A dummy swapchain on HCM's own hidden window gives dxgi's Present / Present1 entry points, and both stay inline-hooked.
	// The overlay draws on whichever D3D11 swapchain is presenting to a visible window of this process right now - Metro
	// presents its intro videos through one swapchain and the 3D game through another, so "the first one seen" is not
	// enough. The render target view is made each frame and released after it: HCM holds no reference to the game's back
	// buffers between frames, so the game can resize or recreate its swapchain freely (no ResizeBuffers hook needed).
	static DX11Present captureDX11Present;
	static DX11Present1 captureDX11Present1;
	static void inlinePresentWork(IDXGISwapChain* pSwapChain, bool viaPresent1);
	void beginHookByCapture();
	safetyhook::InlineHook mCapturePresent, mCapturePresent1;
	static inline std::atomic<DX11Present*> sOrigPresent{ nullptr };
	static inline std::atomic<DX11Present1*> sOrigPresent1{ nullptr };
	static inline IDXGISwapChain* sCurrent = nullptr;   // the swapchain the overlay draws on (Present thread only)
	bool mInlineMode = false;
	safetyhook::VmHook present1Hook; // (VMT path) Present1, when the game presents through it

	std::weak_ptr<PointerDataStore> pointerDataStoreWeak; // not required but used to attempt to resolve vmt entries without needing a dummy swapchain

	safetyhook::VmtHook DXGIHook;
	safetyhook::VmHook presentHook; //  present hook
	safetyhook::VmHook resizebuffersHook; // resizeBuffers hook
	std::shared_ptr<ModuleInlineHook> OBSPresentHook; // obs hook for obs bypass


	// D3D data
	ID3D11Device* m_pDevice = nullptr;
	ID3D11DeviceContext* m_pDeviceContext = nullptr;
	ID3D11RenderTargetView* m_pMainRenderTargetView = nullptr;

	void CreateDummySwapchain(IDXGISwapChain*& pDummySwapchain, ID3D11Device*& pDummyDevice);
	// dxgi's IDXGISwapChain::Present entry point - the function OBS Detours-hooks. Used to identify
	// OBS's trampoline pointer at runtime; we never hook this address ourselves. 0 on failure.
	uintptr_t getDxgiPresentEntry();
	void initializeD3Ddevice(IDXGISwapChain*);
	bool isD3DdeviceInitialized = false;

	static SimpleMath::Vector2 mScreenSize;
	static SimpleMath::Vector2 mScreenCenter;

public:

	explicit D3D11Hook(std::weak_ptr<PointerDataStore> pointerDataStore);
	~D3D11Hook();


	// Callback for present rendering
	std::shared_ptr<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>> presentHookEvent = std::make_shared<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>>();

	// 2026-11-21 (user: "make SMAA an exception to the 'bypass obs' rule ... so it can show up in recordings"): fired from the
	// swapchain VMT hook on EVERY frame, before presentHookEvent and before the frame reaches OBS's capture, whether the OBS
	// bypass is on or not. For post-processing that belongs to the game's picture (SMAA), never for HCM's own UI.
	std::shared_ptr<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>> preCapturePresentEvent = std::make_shared<eventpp::CallbackList<void(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*, ID3D11RenderTargetView*)>>();



	// Callback for window resize
	std::shared_ptr<eventpp::CallbackList<void(SimpleMath::Vector2 newScreenSize)>> resizeBuffersHookEvent = std::make_shared<eventpp::CallbackList<void(SimpleMath::Vector2 newScreenSize)>>();

	// Banned operations for singleton
	D3D11Hook(const D3D11Hook& arg) = delete; // Copy constructor
	D3D11Hook(const D3D11Hook&& arg) = delete;  // Move constructor
	D3D11Hook& operator=(const D3D11Hook& arg) = delete; // Assignment operator
	D3D11Hook& operator=(const D3D11Hook&& arg) = delete; // Move operator

	SimpleMath::Vector2 getScreenSize() { return mScreenSize; }
	SimpleMath::Vector2 getScreenCenter() { return mScreenCenter; }

	void beginHook();

	// Throws HCMRuntimeException if the bypass was asked for and could not be set up. Returns
	// DeferredModuleNotLoaded when OBS's Game Capture simply isn't in the process yet - that is a
	// normal state, not a failure: the hook stays registered and attaches by itself on LoadLibrary.
	OBSBypassResult setOBSBypass(bool enabled);
};
