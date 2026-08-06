#include "pch.h"
#include "ImGuiManager.h"
#include "GlobalKill.h"
#include "ImageResidencyGuard.h"
#include "ProggyVectorRegularFont.h"
#include "imgui_internal.h"





ImGuiManager* ImGuiManager::instance = nullptr;

WNDPROC ImGuiManager::mOldWndProc = nullptr;
IMGUI_IMPL_API LRESULT  ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT __stdcall ImGuiManager::mNewWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// FIRST STATEMENT, before anything else can touch HCM state. Windows can dispatch a message into this
	// procedure at literally any instant, including while HCMInternal.dll is being unloaded - and until now
	// nothing waited for it. See ImageResidencyGuard.h. Re-entrancy on this thread is expected (the present
	// path pumps messages) and is handled by the guard's depth counter.
	ImageResidency::ScopedImageResidency residency;

	// Once shutdown has begun, get completely out of the game's way: forward every message straight to
	// the game's original WndProc and don't touch ImGui. During teardown the "return true" path below
	// can SWALLOW window messages (WM_SIZE / WM_DISPLAYCHANGE / activation etc.) that the game needs to
	// keep its D3D device consistent - a message swallowed there can leave the game's immediate context
	// in a bad state and crash the render thread mid-teardown. This also avoids touching an ImGui
	// context that may be mid-destruction.
	if (GlobalKill::isKillSet())
		return mOldWndProc ? CallWindowProc(mOldWndProc, hWnd, uMsg, wParam, lParam)
		                   : DefWindowProcW(hWnd, uMsg, wParam, lParam);

	//https://www.unknowncheats.me/forum/2488829-post5.html
	ImGuiIO& io = ImGui::GetIO();
	LRESULT res = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);


	switch (uMsg)
	{
	//case WM_CLOSE:
	case WM_DESTROY:
	case WM_NCDESTROY:
		GlobalKill::killMe();
		break;
	default:
		break;
	}

	// "GUI showing blocks game input" for games with no engine-level block-input service (Halo Campaign Evolved).
	// MCC gets this from blockGameInputService, which midhooks MCC itself; HCE has no equivalent, so instead we
	// simply don't forward input messages to the game while the HCM window is open. ImGui has already seen the
	// message above, so the overlay still works. Non-input messages (activation, sizing, paint, device changes)
	// MUST still reach the game or its D3D state goes inconsistent - so this only swallows input classes.
	if (sSwallowGameInput.load(std::memory_order_relaxed))
	{
		switch (uMsg)
		{
		case WM_INPUT:              // raw input - how UE5 does mouse-look. Blocking only WM_MOUSEMOVE is not enough.
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
		case WM_KEYDOWN: case WM_KEYUP:
		case WM_CHAR: case WM_DEADCHAR: case WM_UNICHAR:
			return 0;   // consumed: the game never sees it
			// NOTE: WM_SYSKEYDOWN/WM_SYSKEYUP are deliberately NOT swallowed, so Alt+Tab / Alt+F4 keep working.
		default:
			break;
		}
	}

	if (io.WantCaptureMouse == false)
	{
		// ImGui didn't handle the click so let MCC do it
		return CallWindowProc(ImGuiManager::mOldWndProc, hWnd, uMsg, wParam, lParam);
	}
	else
	{
		switch (uMsg)
		{
			// Certain messages we want to let MCC know about too
		case WM_ACTIVATE:
		case WM_ACTIVATEAPP:
		case WM_NCACTIVATE:
			return CallWindowProc(ImGuiManager::mOldWndProc, hWnd, uMsg, wParam, lParam);
			break;
		default:
			return true; // otherwise we just tell MCC to not worry about it
		}
	}





}

// The backend-independent half of ImGui bring-up: WndProc chain, context, io flags and the win32
// platform backend. Identical for D3D11 and D3D12 - only the renderer backend differs.
void ImGuiManager::initializeImGuiContextAndPlatform(HWND windowHandle)
{
	m_windowHandle = windowHandle;

	// Setup the imgui WndProc callback
	mOldWndProc = (WNDPROC)SetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC, (LONG_PTR)&mNewWndProc);


	// Setup ImGui stuff
	PLOG_DEBUG << "Initializing ImGui";
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;// | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

	if (!ImGui_ImplWin32_Init(m_windowHandle))
	{
		throw HCMInitException(std::format("ImGui_ImplWin32_Init failed w/ {} ", (uint64_t)m_windowHandle).c_str());
	};
}

void ImGuiManager::initializeImGuiResources(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* pMainRenderTargetView)
{
	PLOG_VERBOSE << "initializeImGuiResources";
	// Use swap chain description to get MCC window handle
	DXGI_SWAP_CHAIN_DESC swapChainDesc;
	pSwapChain->GetDesc(&swapChainDesc);
	if (!&swapChainDesc) throw HCMInitException("Failed to get swap chain description");

	initializeImGuiContextAndPlatform(swapChainDesc.OutputWindow);

	if (!ImGui_ImplDX11_Init(pDevice, pDeviceContext))
	{
		throw HCMInitException(std::format("ImGui_ImplDX11_Init failed w/ {}, {} ", (uint64_t)pDevice, (uint64_t)pDeviceContext).c_str());
	};

	PLOG_DEBUG << "ImGui Initialized";

	applyImGuiStyleAndFonts();
}


// Halo Campaign Evolved / D3D12. Everything the dx12 renderer backend needs (device, command queue,
// SRV heap, frames-in-flight, RTV format, and the SRV sub-allocator callbacks) lives on D3D12Hook,
// so we pull a ready-made ImGui_ImplDX12_InitInfo off it rather than widening the present event
// with init-only data.
void ImGuiManager::initializeImGuiResourcesD3D12(IDXGISwapChain3* pSwapChain)
{
	PLOG_VERBOSE << "initializeImGuiResourcesD3D12";

	auto d3d12 = m_d3d12.lock();
	if (!d3d12) throw HCMInitException("D3D12Hook was already destroyed when ImGui tried to initialize");

	// Prefer the hook's window handle: it is the one it validated with isOwnedByThisProcess, so we
	// can never end up hooking the WndProc of some other process's window. Fall back to the
	// swapchain description (which is what the D3D11 path does) if the hook somehow has none.
	HWND windowHandle = d3d12->getWindowHandle();
	if (!windowHandle && pSwapChain)
	{
		DXGI_SWAP_CHAIN_DESC swapChainDesc{};
		if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc)))
			windowHandle = swapChainDesc.OutputWindow;
	}
	if (!windowHandle) throw HCMInitException("Failed to get the D3D12 swap chain's window handle");

	ImGui_ImplDX12_InitInfo initInfo{};
	if (!d3d12->getImGuiInitInfo(initInfo))
		throw HCMInitException("D3D12Hook could not supply an ImGui_ImplDX12_InitInfo");

	initializeImGuiContextAndPlatform(windowHandle);

	if (!ImGui_ImplDX12_Init(&initInfo))
	{
		throw HCMInitException(std::format("ImGui_ImplDX12_Init failed w/ device {}, queue {}, srvHeap {}, framesInFlight {} ",
			(uint64_t)initInfo.Device, (uint64_t)initInfo.CommandQueue, (uint64_t)initInfo.SrvDescriptorHeap, initInfo.NumFramesInFlight).c_str());
	};

	PLOG_DEBUG << "ImGui Initialized (D3D12)";

	applyImGuiStyleAndFonts();
}


// Style + fonts. Shared verbatim by both backends so HCM's GUI is pixel-identical on MCC and HCE.
void ImGuiManager::applyImGuiStyleAndFonts()
{
	ImGuiIO& io = ImGui::GetIO();

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	ImGuiStyle* style = &ImGui::GetStyle();
	style->FrameRounding = 0;
	style->WindowBorderSize = 0;
	style->WindowRounding = 0;
	style->FrameBorderSize = 1;

	// Colors
	style->Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.04f, 0.08f, 1.00f);
	style->Colors[ImGuiCol_ChildBg] = ImVec4(0.18f, 0.10f, 0.18f, 0.50f);
	style->Colors[ImGuiCol_Text] = ImVec4(0.80f, 0.90f, 0.90f, 1.00f);
	style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.29f, 0.29f, 1.00f);

	style->Colors[ImGuiCol_Border] = ImVec4(0.40f, 0.50f, 0.50f, 0.38f);
	style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.92f, 0.91f, 0.88f, 0.00f);

	style->Colors[ImGuiCol_FrameBg] = ImVec4(0.35f, 0.09f, 0.12f, 1.00f);
	//style->Colors[ImGuiCol_FrameBg] = ImVec4(0.95f, 0.99f, 0.12f, 1.00f);
	style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.55f, 0.23f, 0.29f, 1.00f);
	style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.81f, 0.23f, 0.29f, 1.00f);

	style->Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.9f, 0.6f, 0.7f);
	style->Colors[ImGuiCol_Button] = ImVec4(0.30f, 0.09f, 0.12f, 1.00f);
	style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.23f, 0.29f, 1.00f);
	style->Colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.23f, 0.29f, 1.00f);

	style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.75f, 0.43f);

	style->Colors[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.70f, 0.73f, 0.31f);
	style->Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.26f, 0.05f, 0.07f, 1.00f);

	style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.f, 0.f, 0.f, 0.f);
	style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.35f, 0.09f, 0.12f, 0.7f);

	style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.1f, 0.00f, 0.1f, 0.3f);

	style->IndentSpacing = 10.f;

	// setup font
	io.Fonts->AddFontDefault();
	mRescalableMonospacedFont = io.Fonts->AddFontFromMemoryCompressedTTF(ProggyVectorRegularFont_compressed_data, ProggyVectorRegularFont_compressed_size, 15.f * 2);

	//int my_image_width = 0;
	//int my_image_height = 0;

	//bool ret = LoadTextureFromMemory(103, pDevice, &my_texture, &my_image_width, &my_image_height);
	//// todo; overlay bypass stuff based on return value
	//IM_ASSERT(ret);


}


ImGuiManager::~ImGuiManager()
{
	PLOG_DEBUG << "~ImGuiManager()";
	// Set BEFORE mDestructionGuard is taken, so a present handler that is already inside its body and
	// merely waiting on the guard sees it and bails instead of resuming into a shut-down backend.
	mShuttingDown.store(true, std::memory_order_release);
	//presentEventCallback.~ScopedCallback();
	if (presentEventCallback) presentEventCallback->removeCallback(); // no new callback invokes
	if (presentEventCallbackD3D12) presentEventCallbackD3D12->removeCallback();
	if (mOldWndProc) 		// restore the original wndProc
	{
		// ⚠ ONLY RESTORE IF WE ARE STILL THE TOP OF THE CHAIN. WndProc subclassing is a linked list, and this
		// used to write mOldWndProc back unconditionally. If anything subclassed AFTER us - RTSS, the Steam
		// overlay, Streamline, another injector - then the window's current proc is THEIRS, and overwriting it
		// with our saved pointer silently deletes them from the chain and hands the game a proc that skips
		// their state. That corrupts a process we do not own, and it is exactly the class of "HCM closed and
		// the game died later" bug this whole investigation is about.
		//
		// If we are not on top, the safe move is to leave the chain alone: our proc stays live, and the
		// residency drain below (plus the GlobalKill early-out at the top of mNewWndProc) keeps it harmless.
		const LONG_PTR current = GetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC);
		if (current == (LONG_PTR)&ImGuiManager::mNewWndProc)
		{
			SetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC, (LONG_PTR)mOldWndProc);
			mOldWndProc = nullptr;
		}
		else
		{
			PLOG_ERROR << "ImGuiManager: NOT restoring the window procedure - something else subclassed after "
				"us (current proc is 0x" << std::hex << current << std::dec << ", ours is not on top). "
				"Restoring would delete that subclass from the chain. Our proc stays installed and inert; "
				"HCMInternal.dll must therefore not be unloaded from under it.";
			mWndProcLeftInstalled.store(true, std::memory_order_release);
		}
	}

	std::unique_lock<std::mutex> lock(mDestructionGuard); // block until callbacks finish executing



	// Cleanup
	if (m_isImguiInitialized)
	{
		if (mBackend == RenderBackend::D3D12)
		{
			// MANDATORY GPU FLUSH BEFORE ImGui_ImplDX12_Shutdown().
			// ImGui_ImplDX12_Shutdown -> InvalidateDeviceObjects releases imgui's PSO, root
			// signature, font texture and the whole per-frame vertex/index buffer ring, with no GPU
			// wait of its own (the dx12 backend never fences on shutdown). D3D12 does NOT defer
			// destruction until the GPU is done with a resource, and the very last thing D3D12Hook
			// submitted was an overlay draw referencing exactly those objects. Freeing them while
			// that command list is still executing is a GPU-side use-after-free -> device removed ->
			// the GAME dies, which is precisely the failure mode we must never cause.
			//
			// App.h declares the graphics hook BEFORE `imm`, so the hook is guaranteed to still be
			// alive here (destruction is reverse declaration order). If the flush cannot be PROVEN
			// (Signal failed, or the wait timed out - both mean device removal or a hung GPU) we
			// deliberately LEAK imgui's dx12 backend rather than free memory the GPU may be reading.
			// That is the same "abandon rather than free" policy D3D12Hook uses internally.
			bool gpuIsIdle = false;
			if (auto d3d12 = m_d3d12.lock())
			{
				gpuIsIdle = d3d12->waitForGpuIdle();
			}
			else
			{
				PLOG_ERROR << "D3D12Hook was already destroyed at ~ImGuiManager; cannot prove the GPU is idle";
			}

			if (gpuIsIdle)
			{
				ImGui_ImplDX12_Shutdown();
				ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext();
			}
			else
			{
				// Leak the renderer backend AND the context: DestroyContext with a live renderer
				// backend still attached trips imgui's "forgot to shutdown backend" assert and frees
				// the font atlas the backend's texture still refers to. A leak in a process that is
				// about to lose HCM entirely is free; a GPU fault is not.
				PLOG_FATAL << "Could not prove the GPU was idle; abandoning (leaking) the ImGui D3D12 backend rather than freeing resources the GPU may still be reading";
				ImGui_ImplWin32_Shutdown();
			}
		}
		else
		{
			ImGui_ImplDX11_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		}

		m_isImguiInitialized = false;
	}

	instance = nullptr;
}



//void ImGuiManager::lapuaTest(SimpleMath::Vector2 ss)
//{
//	constexpr auto strength = 0x01FFFFFF;
//	constexpr auto zero = ImVec2(0, 0);
//	constexpr auto one = ImVec2(1, 1);
//	ImGui::GetBackgroundDrawList()->AddImage(my_texture, zero, ss, zero, one, strength);
//
//}

void ImGuiManager::onPresentHookEvent(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* pMainRenderTargetView)
{
	LOG_ONCE(PLOG_DEBUG << "ImGuiManager::onPresentHookEvent running");
	std::unique_lock<std::mutex> lock(mDestructionGuard);
#pragma region init


	if (!instance->m_isImguiInitialized)
	{
		PLOG_INFO << "Initializing ImGuiManager";
		try
		{
			instance->initializeImGuiResources(pDevice, pDeviceContext, pSwapChain, pMainRenderTargetView);
			instance->m_isImguiInitialized = true;
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize ImGui, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();
			return;
		}
	}
#pragma endregion init

	//TODO: check if this is necessary
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);

	}


	// Start ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	GImGui->NavWindowingTarget = nullptr; // prevent navinputs altogether

	// I don't 100% understand what this does, but it must be done before we try to render
	pDeviceContext->OMSetRenderTargets(1, &pMainRenderTargetView, NULL);
	auto screenSize = getScreenSize();


	// invoke callback of anything that wants to render with ImGui
	BackgroundRenderEvent->operator()(screenSize); // for overlays.
	MidgroundRenderEvent->operator()(screenSize);
	ForegroundRenderEvent->operator()(screenSize);
	ForegroundDirectXRenderEvent->operator()(pDevice, pDeviceContext, screenSize, pMainRenderTargetView);

	// Finish ImGui frame
	ImGui::EndFrame();
	ImGui::Render();

	// Render it !
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}


// Halo Campaign Evolved / D3D12 equivalent of onPresentHookEvent above.
//
// Contract with D3D12Hook (see D3D12Hook.h): pCommandList is already Reset() and recording, the
// back buffer is already in RENDER_TARGET state, OMSetRenderTargets(backBufferRTV) and
// SetDescriptorHeaps(srvHeap) have already been issued on it, and we must NOT Close(), Execute() or
// Signal() - the hook owns all of that. Which is exactly why there is no OMSetRenderTargets here:
// the D3D11 path's `pDeviceContext->OMSetRenderTargets(...)` has already been done for us, by the
// only code that knows which back buffer this frame is.
void ImGuiManager::onPresentHookEventD3D12(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, IDXGISwapChain3* pSwapChain, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV)
{
	LOG_ONCE(PLOG_DEBUG << "ImGuiManager::onPresentHookEventD3D12 running");
	if (mShuttingDown.load(std::memory_order_acquire)) return; // don't even queue up behind the destructor
	std::unique_lock<std::mutex> lock(mDestructionGuard);
	// Authoritative check: the destructor sets the flag BEFORE it takes the guard, so if it started
	// while we were blocked above, we see it here and get out without touching the imgui backend.
	if (mShuttingDown.load(std::memory_order_acquire)) return;
#pragma region init


	// `this`, not `instance` (which the D3D11 handler uses): they are always the same object, but the
	// subscription is published a hair before `instance` is, so `this` is the one that is never null.
	if (!m_isImguiInitialized)
	{
		PLOG_INFO << "Initializing ImGuiManager (D3D12)";
		try
		{
			initializeImGuiResourcesD3D12(pSwapChain);
			m_isImguiInitialized = true;
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize ImGui, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();
			return;
		}
	}
#pragma endregion init

	//TODO: check if this is necessary
	// NOTE: this dispatches window messages, so the game's WndProc - and therefore ResizeBuffers -
	// can run RE-ENTRANTLY from right here. D3D12Hook handles that: it samples mResourceGeneration
	// around this event and abandons the frame (closing the command list) rather than submitting one
	// that references a back buffer released underneath us. It also deliberately does NOT hold its
	// resource mutex across this event, so the re-entry cannot deadlock.
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);

	}


	// Start ImGui frame
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	GImGui->NavWindowingTarget = nullptr; // prevent navinputs altogether

	// (no OMSetRenderTargets - D3D12Hook already bound backBufferRTV on pCommandList)
	auto screenSize = getScreenSize();


	// invoke callback of anything that wants to render with ImGui.
	// The three RenderEvents below are backend-agnostic (they only carry the screen size) and carry
	// HCM's entire GUI, so everything that renders on MCC renders here too.
	BackgroundRenderEvent->operator()(screenSize); // for overlays.
	MidgroundRenderEvent->operator()(screenSize);
	ForegroundRenderEvent->operator()(screenSize);
	// ForegroundDirectXRenderEvent is deliberately NOT fired: its payload is live ID3D11Device /
	// ID3D11DeviceContext / ID3D11RenderTargetView pointers which do not exist under D3D12, and every
	// one of its subscribers dereferences them immediately. Its D3D12 counterpart is fired instead.
	// The back buffer format comes off the hook: a D3D12 subscriber that records its own draws needs
	// it to build a PSO, and it cannot be recovered from the RTV descriptor handle. DXGI_FORMAT_UNKNOWN
	// if the hook has gone away, which subscribers must treat as "do not render this frame".
	DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;
	if (auto d3d12 = m_d3d12.lock()) backBufferFormat = d3d12->getRenderTargetFormat();
	ForegroundD3D12RenderEvent->operator()(pDevice, pCommandList, screenSize, backBufferRTV, backBufferFormat);

	// Finish ImGui frame
	ImGui::EndFrame();
	ImGui::Render();

	// Render it !
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCommandList);
}


