#include "pch.h"
#include "D3D11Hook.h"

#include "GlobalKill.h"
#include <dxgi.h>
#pragma comment(lib, "dxgi")

#include "Renderer2D.h"
#include "Lapua.h"
#include "safetyhook.hpp"
#include "OBSHookDiscovery.h"
#include "ModuleCache.h" // not reachable via pch


D3D11Hook* D3D11Hook::instance = nullptr;
SimpleMath::Vector2 D3D11Hook::mScreenSize{1920, 1080};
SimpleMath::Vector2 D3D11Hook::mScreenCenter{960, 540};

struct rgba {
	float r, g, b, a;
	rgba(float ir, float ig, float ib, float ia) : r(ir), g(ig), b(ib), a(ia) {}
};


enum class IDXGISwapChainVMT {
	QueryInterface,
	AddRef,
	Release,
	SetPrivateData,
	SetPrivateDataInterface,
	GetPrivateData,
	GetParent,
	GetDevice,
	Present,
	GetBuffer,
	SetFullscreenState,
	GetFullscreenState,
	GetDesc,
	ResizeBuffers,
	ResizeTarget,
	GetContainingOutput,
	GetFrameStatistics,
	GetLastPresentCount,
};


const std::map<D3D_DRIVER_TYPE, std::string> driverToString
{
	{ D3D_DRIVER_TYPE_HARDWARE, "D3D_DRIVER_TYPE_HARDWARE"},
	{ D3D_DRIVER_TYPE_WARP, "D3D_DRIVER_TYPE_WARP" },
	{ D3D_DRIVER_TYPE_REFERENCE, "D3D_DRIVER_TYPE_REFERENCE" },
	{ D3D_DRIVER_TYPE_SOFTWARE, "D3D_DRIVER_TYPE_SOFTWARE" },
	{ D3D_DRIVER_TYPE_UNKNOWN, "D3D_DRIVER_TYPE_UNKNOWN" },

};


void D3D11Hook::CreateDummySwapchain(IDXGISwapChain*& pDummySwapchain, ID3D11Device*& pDummyDevice)
{

	std::vector<std::string> errorCodes;

#define logSwapChainFailure(x) PLOG_ERROR << x; errorCodes.push_back(x)

	D3D_FEATURE_LEVEL featLevel;
	DXGI_SWAP_CHAIN_DESC sd{ 0 };
	sd.BufferCount = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.Height = 800;
	sd.BufferDesc.Width = 600;
	sd.BufferDesc.RefreshRate = { 60, 1 };
	sd.OutputWindow = GetForegroundWindow();
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;



	// the following attempts at D3D11CreateDeviceAndSwapChain differ in the driver_type param
	// https://learn.microsoft.com/en-us/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_driver_type



	// use D3D_DRIVER_TYPE_HARDWARE
	// works for me
	{
		auto driverToTry = D3D_DRIVER_TYPE_HARDWARE;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}

	// use D3D_DRIVER_TYPE_WARP
	// Seems to be the most consistent
	{
		auto driverToTry = D3D_DRIVER_TYPE_WARP;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}

	// use D3D_DRIVER_TYPE_REFERENCE. The original code I used.
	// works for me but not gronchy
	{
		auto driverToTry = D3D_DRIVER_TYPE_REFERENCE;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}



	// use D3D_DRIVER_TYPE_SOFTWARE
	// does not work for me (E_INVALIDARG)
	{
		auto driverToTry = D3D_DRIVER_TYPE_SOFTWARE;
		PLOG_DEBUG << "Attempting to create dummy swap chain with driver type: " << driverToString.at(driverToTry);
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, driverToTry, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with driver type " << driverToString.at(driverToTry);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("6: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(driverToTry), (ULONG)hr));
		}
	}


	// the next call seems to need us to set the adapter parameter ourselves instead of using null ptr

#define UseDXGI1 TRUE

#if UseDXGI1 == TRUE
#define IDXGIFactoryA IDXGIFactory1
#define CreateDXGIFactoryA CreateDXGIFactory1
#define IDXGIAdapterA IDXGIAdapter1
#define EnumAdaptersA EnumAdapters1
#else
#define IDXGIFactoryA IDXGIFactory
#define CreateDXGIFactoryA CreateDXGIFactory
#define IDXGIAdapterA IDXGIAdapter
#define EnumAdaptersA EnumAdapters
#endif

	IDXGIFactoryA* pFactory;
	HRESULT fhr = CreateDXGIFactoryA(__uuidof(IDXGIFactoryA), (void**)(&pFactory));

	std::vector <IDXGIAdapterA*> vAdapters;
	if (fhr != S_OK)
	{
		PLOG_ERROR << "Failed to create DXGIFactory: " << std::hex << (ULONG)fhr;
	}
	else
	{
		UINT i = 0;
		IDXGIAdapterA* pAdapter;
		while (pFactory->EnumAdaptersA(i, &pAdapter) != DXGI_ERROR_NOT_FOUND)
		{
			vAdapters.push_back(pAdapter);
			++i;
		}
		PLOG_DEBUG << "default adapter: " << std::hex << vAdapters.front();
		PLOG_DEBUG << "adapter count: " << vAdapters.size();

	}

	// use D3D_DRIVER_TYPE_UNKNOWN
	// works if I pass it the adapter manually
	// tries each adapter
	PLOG_DEBUG << "Attempting to create dummy swapchain with driver type " << driverToString.at(D3D_DRIVER_TYPE_UNKNOWN);
	for (auto adapter : vAdapters)
	{
		PLOG_INFO << "Attemtping D3D11CreateDeviceAndSwapChain with adapter: " << std::hex << adapter;

		HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pDummySwapchain, &pDummyDevice, &featLevel, nullptr);
		if (SUCCEEDED(hr))
		{
			PLOG_INFO << "Succesfully created dummy swapchain with " << driverToString.at(D3D_DRIVER_TYPE_UNKNOWN);
			return;
		}
		else
		{
			logSwapChainFailure(std::format("4: failed to create dummy d3d device and swapchain with driver type {}, error: {:x}", driverToString.at(D3D_DRIVER_TYPE_UNKNOWN), (ULONG)hr));
		}

	}



	// If execution got here, none of the createdevice calls succeeded. Throw an exception and log the error codes

	std::string resultsString;
	for (auto error : errorCodes)
	{
		resultsString += error + "\n";
	}
	throw HCMInitException(resultsString);


}


D3D11Hook::D3D11Hook(std::weak_ptr<PointerDataStore> pointerDataStore)
	: pointerDataStoreWeak(pointerDataStore)
{

	if (instance != nullptr)
	{
		throw HCMInitException("Cannot have more than one D3D11Hook");
	}
	instance = this;



#ifdef HCM_DEBUG
	// get pointers to present/resizeBuffers using a dummy swapchain.
	// then log em to the console so burnt can use ida/cheatengine to find the idxgi vmt 

	IDXGISwapChain* pDummySwapchain; 
	ID3D11Device* pDummyDevice;
	CreateDummySwapchain(pDummySwapchain, pDummyDevice);

	if (pDummySwapchain && pDummyDevice)
	{
		uintptr_t* dxgi_vmt = (uintptr_t*)::calloc(119, sizeof(uintptr_t));
		::memcpy(dxgi_vmt, *(uintptr_t**)pDummyDevice, magic_enum::enum_count< IDXGISwapChainVMT>() * sizeof(uintptr_t));

		PLOG_DEBUG << "DX11::Present @ 0x" << std::hex << dxgi_vmt[(int)IDXGISwapChainVMT::Present];
		PLOG_DEBUG << "DX11::ResizeBuffers @ 0x" << std::hex << dxgi_vmt[(int)IDXGISwapChainVMT::ResizeBuffers];
	}

	safe_release(pDummyDevice);
	safe_release(pDummySwapchain);

#endif


}





void D3D11Hook::initializeD3Ddevice(IDXGISwapChain* pSwapChain)
{

	PLOG_DEBUG << "Initializing D3Ddevice" << std::endl;

	if (!SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&m_pDevice)))
	{
		throw HCMInitException(std::format("Failed to get D3D11Device, pSwapChain: {:x}", (uint64_t)pSwapChain).c_str());

	}

	// Get Device Context
	m_pDevice->GetImmediateContext(&m_pDeviceContext);
	if (!m_pDeviceContext)
	{
		throw HCMInitException("Failed to get DeviceContext");
	}



	// Use backBuffer to get MainRenderTargetView
	ID3D11Texture2D* pBackBuffer;
	pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
	if (!pBackBuffer) throw HCMInitException("Failed to get BackBuffer");

	// Store the windows size
	D3D11_TEXTURE2D_DESC desc;
	pBackBuffer->GetDesc(&desc);
	PLOG_INFO << "Initializing screen size: " << desc.Width << ", " << desc.Height;
	mScreenSize = { ((float)desc.Width), ((float)desc.Height) };
	mScreenCenter = mScreenSize / 2;

#ifdef HCM_DEBUG
	DXGI_SWAP_CHAIN_DESC sd;
	pSwapChain->GetDesc(&sd);

	PLOG_DEBUG << "swapchain image format: " << sd.BufferDesc.Format;
	// result: DXGI_FORMAT_R8G8B8A8_UNORM
#endif

	PLOG_INFO << "Creating render target view";
	auto CreateRenderTargetViewResult = m_pDevice->CreateRenderTargetView(pBackBuffer, NULL, &m_pMainRenderTargetView);
	PLOG_DEBUG << "CreateRenderTargetViewResult: " << CreateRenderTargetViewResult;
	pBackBuffer->Release();
	if (CreateRenderTargetViewResult || !m_pMainRenderTargetView) throw HCMInitException(std::format("Failed to get MainRenderTargetView, error code: {}", CreateRenderTargetViewResult));




}

// static
HRESULT D3D11Hook::newDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{


	ScopedAtomicBool lock(presentHookRunning);
	//std::unique_lock<std::mutex> lock(mDestructionGuard); // Protects against D3D11Hook singleton destruction while hooks are executing
	LOG_ONCE(PLOG_DEBUG << "D3D11Hook::newDX11Present");
	auto d3d = instance;
	if (!d3d)
	{
		PLOG_FATAL << "d3d instance was null at newDX11Present";
		return 0;
	}

	// Once shutdown has begun, stop invoking HCM's render/overlay callbacks. The services they
	// call into are being destroyed on the shutdown thread; firing render events here races with
	// that teardown - it crashed on older builds, and now causes lock-contention that stalls
	// shutdown for tens of seconds. That stall prevents the DLL from unloading, which is why HCM
	// can't re-attach after being closed and reopened. Just pass the frame straight to the game.
	if (GlobalKill::isKillSet())
		return d3d->presentHook.stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);

	presentWork(d3d, pSwapChain);
	LOG_ONCE(PLOG_VERBOSE << "calling original present function");
	return d3d->presentHook.stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);
}

// static (2026-11-24): the same as newDX11Present, for a game that presents through IDXGISwapChain1::Present1
HRESULT D3D11Hook::newDX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const void* pParams)
{
	ScopedAtomicBool lock(presentHookRunning);
	LOG_ONCE(PLOG_DEBUG << "D3D11Hook::newDX11Present1");
	auto d3d = instance;
	if (!d3d) { PLOG_FATAL << "d3d instance was null at newDX11Present1"; return 0; }
	if (!GlobalKill::isKillSet()) presentWork(d3d, pSwapChain);
	return d3d->present1Hook.stdcall<HRESULT>(pSwapChain, SyncInterval, Flags, pParams);
}

// static
bool D3D11Hook::presentWork(D3D11Hook* d3d, IDXGISwapChain* pSwapChain)
{
	if (!d3d->isD3DdeviceInitialized)
	{
		PLOG_DEBUG << "Initializing d3d device @ present";
		try
		{
			d3d->initializeD3Ddevice(pSwapChain);
			d3d->isD3DdeviceInitialized = true;
			PLOG_DEBUG << "D3D device initialized t. newDX11Present";
			// fire a resizeborders event
			d3d->resizeBuffersHookEvent->operator()(mScreenSize);
			PLOG_DEBUG << "resizeBuffersHookEvent fired";
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize d3d device, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();

			// the caller calls the original present
			return false;


		}

	}
	LOG_ONCE(PLOG_VERBOSE << "invoking mandatoryPresentHookEvent callback");
	// The game picture's own post-processing (SMAA): here, in the swapchain VMT hook, which always runs BEFORE OBS's
	// inline Present hook captures the frame - so it is in recordings whether the OBS bypass is on or off.
	d3d->preCapturePresentEvent->operator()(d3d->m_pDevice, d3d->m_pDeviceContext, pSwapChain, d3d->m_pMainRenderTargetView);
	// Invoke the callback


	if (!d3d->OBSPresentHook || d3d->OBSPresentHook->isHookInstalled() == false || Renderer2D::good == false)
	{


		LOG_ONCE(PLOG_VERBOSE << "invoking mainPresentHookEvent callback via vmt");
		d3d->presentHookEvent->operator()(d3d->m_pDevice, d3d->m_pDeviceContext, pSwapChain, d3d->m_pMainRenderTargetView);
		Lapua::shouldRunForBypass = false;

		if (Lapua::shouldRunForWatermark)
		{
			if (Renderer2D::good == false)
			{
				Lapua::lapuaGood = false;
			}
			else
			{
				Renderer2D::render(d3d->m_pDevice, d3d->m_pDeviceContext, mScreenSize, d3d->m_pMainRenderTargetView);
			}
		}

	}
	else
	{
		Lapua::shouldRunForBypass = true;
		Renderer2D::render(d3d->m_pDevice, d3d->m_pDeviceContext, mScreenSize, d3d->m_pMainRenderTargetView);
	}

	return true;
}

HRESULT D3D11Hook::newDX11PresentOBSBypass(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	LOG_ONCE(PLOGV << "newDX11PresentOBSBypass");

	// DON'T LOCK! already locked by present!
	//ScopedAtomicBool lock(presentHookRunning);
	auto d3d = instance;
	if (!d3d)
	{
		PLOG_FATAL << "d3d instance was null at newDX11PresentOBSBypass";
		return 0;
	}

	// During shutdown, pass through without firing HCM render callbacks. See newDX11Present.
	if (GlobalKill::isKillSet())
		return d3d->OBSPresentHook->getInlineHook().stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);

	LOG_ONCE(PLOG_VERBOSE << "invoking mainPresentHookEvent callback via inline obs bypass");
	checkOBSCaptureOverlaysWhileBypassed();   // OBS's "Capture third-party overlays" defeats any bypass: say so
	d3d->presentHookEvent->operator()(d3d->m_pDevice, d3d->m_pDeviceContext, pSwapChain, d3d->m_pMainRenderTargetView);
	auto hr = d3d->OBSPresentHook->getInlineHook().stdcall<HRESULT>(pSwapChain, SyncInterval, Flags);

	return hr;
}

// static
HRESULT D3D11Hook::newDX11ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	ScopedAtomicBool lock(presentHookRunning);
	//std::unique_lock<std::mutex> lock(mDestructionGuard); // Protects against D3D11Hook singleton destruction while hooks are executing
	auto d3d = instance;
	if (!d3d)
	{
		PLOG_FATAL << "d3d instance was null at newDX11ResizeBuffers";
	}

	// During shutdown, don't run our resize/render handling (which fires events into services
	// being torn down and can throw). Just pass through to the game. See newDX11Present.
	if (d3d && GlobalKill::isKillSet())
		return d3d->resizebuffersHook.stdcall<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

	//auto guard = d3d->shared_from_this();

	if (!d3d->isD3DdeviceInitialized)
	{
		PLOG_DEBUG << "Initializing d3d device @ resizeBuffers";
		try
		{
			d3d->initializeD3Ddevice(pSwapChain);
			d3d->isD3DdeviceInitialized = true;
			PLOG_DEBUG << "D3D device initialized t. newDX11ResizeBuffers";
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize d3d device, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();

			// Call original resize buffers
			return d3d->resizebuffersHook.stdcall<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);


		}

	}



	// Need to release mainRenderTargetView before calling ResizeBuffers
	if (d3d->m_pMainRenderTargetView != nullptr)
	{
		d3d->m_pDeviceContext->OMSetRenderTargets(0, 0, 0);
		d3d->m_pMainRenderTargetView->Release();
	}

	// Call original ResizeBuffers
	HRESULT hr = d3d->resizebuffersHook.stdcall<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);


	// Resetup the mainRenderTargetView
	ID3D11Texture2D* pBackBuffer;
	pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
	if (!pBackBuffer) throw HCMInitException("Failed to get BackBuffer");

	d3d->m_pDevice->CreateRenderTargetView(pBackBuffer, NULL, &d3d->m_pMainRenderTargetView);
	pBackBuffer->Release();
	if (!d3d->m_pMainRenderTargetView) throw HCMInitException("Failed to get MainRenderTargetView");

	// Grab the new windows size
	mScreenSize = { (float)Width, (float)Height };
	mScreenCenter = mScreenSize / 2;

	// fire screen resize event
	d3d->resizeBuffersHookEvent->operator()(mScreenSize);

	return hr;
}

// Safely destroys hooks
// Releases D3D resources, if we acquired them
D3D11Hook::~D3D11Hook()
{

	if (presentHookRunning)
	{
		PLOG_INFO << "Waiting for presentHook to finish execution";
		presentHookRunning.wait(true);
	}

	// the inline path: disable the hooks, then PARK them (never freed): a Present thread may be between our detour and its
	// trampoline right now, and HCM stays resident, so the trampolines must stay mapped (as D3D12Hook does)
	if (mInlineMode)
	{
		mInlineMode = false;
		(void)mCapturePresent.disable();
		if (mCapturePresent1) (void)mCapturePresent1.disable();
		static auto* parked = new std::vector<safetyhook::InlineHook>();
		parked->push_back(std::move(mCapturePresent));
		if (mCapturePresent1) parked->push_back(std::move(mCapturePresent1));
		sCurrent = nullptr;
	}

	DXGIHook = {};
	OBSPresentHook = {};

	instance = nullptr;

	// D3D resource releasing:
	// need to call release on the device https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3dswapchain9-getdevice

	safe_release(m_pDevice);
	safe_release(m_pDeviceContext);
	safe_release(m_pMainRenderTargetView);



}


void D3D11Hook::beginHook()
{
	PLOG_DEBUG << "beginning vmt hook";


	// set up d3d11 hook. This requires us to get the vmt of Present and ResizeBuffers functions. 

	try
	{

		lockOrThrow(pointerDataStoreWeak, ptr);

		uintptr_t pIDXGISwapChain;

		// a title with no pIDXGISwapChain pointer data (Metro Redux, 2026-11-24): find the game's swapchain by watching
		// dxgi's Present instead
		std::shared_ptr<MultilevelPointer> mlp_IDXGISwapChain;
		try { mlp_IDXGISwapChain = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(pIDXGISwapChain)); }
		catch (HCMInitException&) {}
		catch (HCMRuntimeException&) {}
		if (!mlp_IDXGISwapChain)
		{
			PLOG_INFO << "No pIDXGISwapChain pointer data for this title: capturing the game's swapchain from dxgi's Present";
			beginHookByCapture();
			return;
		}
		if (!mlp_IDXGISwapChain->resolve(&pIDXGISwapChain)) throw HCMInitException(std::format("Could not resolve pIDXGISwapChain: {}-\nThis probably means this version of MCC is not supported by HCM", MultilevelPointer::GetLastError()));

		PLOG_INFO << "Successfully resolved multilevelpointer to IDXGISwapChain virtual method table!";
		PLOG_DEBUG << "pIDXGISwapChain: " << pIDXGISwapChain;

		DXGIHook = safetyhook::create_vmt((void*)pIDXGISwapChain);
		presentHook = safetyhook::create_vm(DXGIHook, (size_t)IDXGISwapChainVMT::Present, &newDX11Present);
		resizebuffersHook = safetyhook::create_vm(DXGIHook, (size_t)IDXGISwapChainVMT::ResizeBuffers, &newDX11ResizeBuffers);

		PLOG_DEBUG << "hooks set";

	}
	catch (HCMRuntimeException ex)
	{
		throw HCMInitException(ex.what());
	}




}



// ---- the inline path (2026-11-24, Metro Redux) ------------------------------------------------------------------------------
namespace
{
	// log each distinct swapchain once: what it is and where it presents (bring-up evidence for a new title)
	void logSwapchainOnce(IDXGISwapChain* sc, bool viaPresent1, bool isGame)
	{
		static std::mutex m;
		static std::vector<IDXGISwapChain*> seen;
		{
			std::scoped_lock lk(m);
			if (std::find(seen.begin(), seen.end(), sc) != seen.end() || seen.size() > 64) return;
			seen.push_back(sc);
		}
		DXGI_SWAP_CHAIN_DESC sd{};
		const bool desc = SUCCEEDED(sc->GetDesc(&sd));
		char title[128]{}, cls[128]{};
		if (desc && sd.OutputWindow) { GetWindowTextA(sd.OutputWindow, title, sizeof(title)); GetClassNameA(sd.OutputWindow, cls, sizeof(cls)); }
		IUnknown* dev = nullptr;
		const bool d3d11 = SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev;
		if (dev) { dev->Release(); dev = nullptr; }
		PLOG_INFO << "D3D11 inline: swapchain " << (void*)sc << " presents via " << (viaPresent1 ? "Present1" : "Present")
			<< " to window " << (void*)(desc ? sd.OutputWindow : nullptr) << " '" << title << "' (" << cls << "), "
			<< (desc ? std::format("{}x{} format {} buffers {} swap effect {}", sd.BufferDesc.Width, sd.BufferDesc.Height, (int)sd.BufferDesc.Format, sd.BufferCount, (int)sd.SwapEffect) : std::string("no desc"))
			<< ", device " << (d3d11 ? "D3D11" : "NOT D3D11") << (isGame ? " -> the overlay draws on it" : " -> ignored");
	}

	// the game's own swapchain: presents to a visible window of this process, with a D3D11 device
	bool isGameSwapchain(IDXGISwapChain* sc)
	{
		DXGI_SWAP_CHAIN_DESC sd{};
		if (!sc || FAILED(sc->GetDesc(&sd)) || !sd.OutputWindow) return false;
		DWORD pid = 0;
		GetWindowThreadProcessId(sd.OutputWindow, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(sd.OutputWindow)) return false;
		ID3D11Device* dev = nullptr;
		if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return false;
		dev->Release();
		return true;
	}
}

// static: on the game's Present thread, before its frame goes on
void D3D11Hook::inlinePresentWork(IDXGISwapChain* sc, bool viaPresent1)
{
	ScopedAtomicBool lock(presentHookRunning);
	auto d3d = instance;
	if (!d3d || !d3d->mInlineMode || GlobalKill::isKillSet()) return;
	const bool game = isGameSwapchain(sc);
	logSwapchainOnce(sc, viaPresent1, game);
	if (!game) return;

	if (sc != sCurrent)
	{
		// a new swapchain (the intro videos' one, then the 3D game's): start over on it
		if (sCurrent) PLOG_INFO << "D3D11 inline: the overlay moves from swapchain " << (void*)sCurrent << " to " << (void*)sc;
		safe_release(d3d->m_pMainRenderTargetView);
		safe_release(d3d->m_pDeviceContext);
		safe_release(d3d->m_pDevice);
		d3d->m_pMainRenderTargetView = nullptr; d3d->m_pDeviceContext = nullptr; d3d->m_pDevice = nullptr;
		d3d->isD3DdeviceInitialized = false;
		sCurrent = sc;
	}
	else if (d3d->isD3DdeviceInitialized && !d3d->m_pMainRenderTargetView)
	{
		// this frame's render target view (released again after the frame - see the header)
		ID3D11Texture2D* back = nullptr;
		if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		const HRESULT hr = d3d->m_pDevice->CreateRenderTargetView(back, nullptr, &d3d->m_pMainRenderTargetView);
		back->Release();
		if (FAILED(hr) || !d3d->m_pMainRenderTargetView) { d3d->m_pMainRenderTargetView = nullptr; return; }
		if ((float)bd.Width != mScreenSize.x || (float)bd.Height != mScreenSize.y)
		{
			mScreenSize = { (float)bd.Width, (float)bd.Height };
			mScreenCenter = mScreenSize / 2;
			PLOG_INFO << "D3D11 inline: screen size " << bd.Width << "x" << bd.Height;
			d3d->resizeBuffersHookEvent->operator()(mScreenSize);
		}
	}
	presentWork(d3d, sc);
	safe_release(d3d->m_pMainRenderTargetView);
	d3d->m_pMainRenderTargetView = nullptr;
}

HRESULT D3D11Hook::captureDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	inlinePresentWork(pSwapChain, false);
	auto orig = sOrigPresent.load(std::memory_order_acquire);
	return orig ? orig(pSwapChain, SyncInterval, Flags) : DXGI_ERROR_INVALID_CALL;
}

HRESULT D3D11Hook::captureDX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const void* pParams)
{
	inlinePresentWork(pSwapChain, true);
	auto orig = sOrigPresent1.load(std::memory_order_acquire);
	return orig ? orig(pSwapChain, SyncInterval, Flags, pParams) : DXGI_ERROR_INVALID_CALL;
}

void D3D11Hook::beginHookByCapture()
{
	// dxgi's Present / Present1 from a dummy swapchain on HCM's OWN hidden window (never a second swapchain on the game's
	// window - D3D11Hook::CreateDummySwapchain uses the foreground window, which here would be the game)
	const wchar_t* cls = L"HCM_TemporaryD3D11Window";
	WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
	wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
	RegisterClassExW(&wc);
	HWND wnd = CreateWindowExW(0, cls, L"HCM", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
	if (!wnd) throw HCMInitException(std::format("D3D11 inline: could not create the dummy window ({})", GetLastError()));
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64; sd.OutputWindow = wnd; sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.SampleDesc.Count = 1;
	IDXGISwapChain* dummySwap = nullptr; ID3D11Device* dummyDev = nullptr; D3D_FEATURE_LEVEL fl{};
	HRESULT hr = E_FAIL;
	for (D3D_DRIVER_TYPE dt : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP })
	{
		hr = D3D11CreateDeviceAndSwapChain(nullptr, dt, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &dummySwap, &dummyDev, &fl, nullptr);
		if (SUCCEEDED(hr)) break;
	}
	void* presentEntry = nullptr; void* present1Entry = nullptr;
	if (SUCCEEDED(hr) && dummySwap)
	{
		void** vt = *(void***)dummySwap;
		presentEntry = vt[(size_t)IDXGISwapChainVMT::Present];
		IUnknown* sc1 = nullptr;
		// IDXGISwapChain1 {790a45f7-0d42-4876-983a-0a55cfe6f4aa}: Present1 is slot 22 of its vtable
		static const GUID kIIDSwapChain1 = { 0x790a45f7, 0x0d42, 0x4876, { 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa } };
		if (SUCCEEDED(dummySwap->QueryInterface(kIIDSwapChain1, (void**)&sc1)) && sc1)
		{
			present1Entry = (*(void***)sc1)[22];
			sc1->Release();
		}
	}
	if (dummySwap) dummySwap->Release();
	if (dummyDev) dummyDev->Release();
	DestroyWindow(wnd);
	UnregisterClassW(cls, wc.hInstance);
	if (!presentEntry) throw HCMInitException(std::format("D3D11 inline: could not create a dummy swapchain (hr {:x})", (ULONG)hr));
	PLOG_INFO << "D3D11 inline: dxgi Present @ " << presentEntry << ", Present1 @ " << present1Entry;

	// publish order (as D3D12Hook): create disabled -> publish the trampoline -> enable
	mInlineMode = true;
	auto p = safetyhook::InlineHook::create(presentEntry, (void*)&captureDX11Present, safetyhook::InlineHook::StartDisabled);
	if (!p) throw HCMInitException("D3D11 inline: could not hook dxgi's Present");
	mCapturePresent = std::move(*p);
	sOrigPresent.store(mCapturePresent.original<DX11Present*>(), std::memory_order_release);
	if (!mCapturePresent.enable()) throw HCMInitException("D3D11 inline: could not enable the Present hook");
	if (present1Entry && present1Entry != presentEntry)
		if (auto p1 = safetyhook::InlineHook::create(present1Entry, (void*)&captureDX11Present1, safetyhook::InlineHook::StartDisabled))
		{
			mCapturePresent1 = std::move(*p1);
			sOrigPresent1.store(mCapturePresent1.original<DX11Present1*>(), std::memory_order_release);
			(void)mCapturePresent1.enable();
		}
	PLOG_INFO << "D3D11 inline: Present" << (mCapturePresent1 ? " and Present1" : "") << " hooked; the overlay follows the game's swapchain";
}


// The entry point of dxgi.dll's IDXGISwapChain::Present - the function OBS Detours-hooks. We need it
// to RECOGNISE OBS's trampoline (see OBSHookDiscovery.h), not to hook it ourselves.
//
// Our own present hook is a VMT hook on the game's swapchain, so its "original" IS dxgi's shared
// implementation, untouched by us. Falling back to reading vtable slot 8 of pIDXGISwapChain covers
// the (impossible in practice - setOBSBypass can only be reached from the GUI, long after
// beginHook()) case where the vmt hook is not installed.
uintptr_t D3D11Hook::getDxgiPresentEntry()
{
	if (auto* fromVmtHook = presentHook.original<void*>())
		return (uintptr_t)fromVmtHook;

	PLOG_WARNING << "The present vmt hook had no original; falling back to reading the swapchain vtable directly";

	auto ptr = pointerDataStoreWeak.lock();
	if (!ptr) return 0;

	uintptr_t pIDXGISwapChain = 0;
	auto mlp_IDXGISwapChain = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(pIDXGISwapChain));
	if (!mlp_IDXGISwapChain || !mlp_IDXGISwapChain->resolve(&pIDXGISwapChain) || !pIDXGISwapChain)
		return 0;

	// pIDXGISwapChain is the OBJECT; its first qword is the vtable.
	if (IsBadReadPtr((void*)pIDXGISwapChain, sizeof(void*))) return 0;
	void** vtable = *(void***)pIDXGISwapChain;
	if (!vtable || IsBadReadPtr(vtable, sizeof(void*) * ((size_t)IDXGISwapChainVMT::Present + 1))) return 0;

	return (uintptr_t)vtable[(size_t)IDXGISwapChainVMT::Present];
}


OBSBypassResult D3D11Hook::setOBSBypass(bool enabled)
{
	PLOGV << "D3D11Hook::setOBSBypass called with value: " << enabled;
	try
	{
		if (enabled)
		{
			if (presentHookRunning)
			{
				PLOG_INFO << "Waiting for presentHook to finish execution A";
				presentHookRunning.wait(true);
			}

			const uintptr_t dxgiEntry = getDxgiPresentEntry();
			if (!dxgiEntry)
				throw HCMRuntimeException("HCM could not determine the address of DirectX's Present function, so the OBS bypass cannot be set up.");

			PLOG_DEBUG << std::format("OBS bypass: dxgi IDXGISwapChain::Present entry is 0x{:X} (first byte suggests it is {}inline-hooked)",
				dxgiEntry, looksInlineHooked(dxgiEntry) ? "" : "NOT ");

			auto realPresentPointer = std::make_shared<OBSRealFnPointer>(dxgiEntry, "Present");

			// Run discovery ONCE here purely so we can tell the three outcomes apart and report them.
			// The hook re-runs it on every attach through OBSRealFnPointer::resolve().
			uintptr_t rva = 0;
			OBSDiscoveryDiagnostics diagnostics{};
			const OBSDiscoveryStatus status = discoverRealFnRva(dxgiEntry, rva, &diagnostics);

			if (status == OBSDiscoveryStatus::NoDetourFound)
			{
				PLOG_ERROR << std::format(
					"OBS bypass: graphics-hook64.dll is loaded (base 0x{:X}, size 0x{:X}) but no Present trampoline pointer was found. "
					"Sections scanned: {}. {} aligned qwords examined, {} probed. dxgi Present entry used: 0x{:X}.",
					diagnostics.moduleBase, (uint64_t)diagnostics.moduleSize, diagnostics.sectionsScanned,
					diagnostics.qwordsScanned, diagnostics.candidatesProbed, diagnostics.dxgiEntry);
				throw HCMRuntimeException("OBS is running but HCM could not locate its Present hook (unsupported OBS version). Report this with your OBS version.");
			}

			// startEnabled = true. When graphics-hook64.dll isn't loaded yet this attach is a no-op,
			// but the hook stays registered with ModuleHookManager and attaches by itself the moment
			// OBS's Game Capture injects. That deferred behaviour is the whole reason this is a
			// ModuleInlineHook and not a bare safetyhook.
			OBSPresentHook = ModuleInlineHook::make(L"graphics-hook64.dll", realPresentPointer, newDX11PresentOBSBypass, true);

			// If a LATER (deferred) discovery fails there is nowhere to throw to - we would be inside
			// LoadLibrary - so stop wanting to be attached instead of retrying on every DLL load.
			realPresentPointer->setOnPermanentFailure([this]()
				{
					if (OBSPresentHook) OBSPresentHook->setWantsToBeAttached(false);
				});
			// OBS 32.x hooks Present a moment after graphics-hook64.dll loads: the background retry calls this
			// when the hook appears (re-runs attach, which resolves through discovery again)
			realPresentPointer->setOnLateSuccess([this]()
				{
					if (OBSPresentHook && OBSPresentHook->getWantsToBeAttached() && !OBSPresentHook->isHookInstalled())
					{
						OBSPresentHook->setWantsToBeAttached(false);
						OBSPresentHook->setWantsToBeAttached(true);
					}
				});

			if (status == OBSDiscoveryStatus::ModuleNotLoaded)
			{
				PLOG_INFO << "OBS bypass armed, but graphics-hook64.dll is not loaded - it will attach automatically when OBS Game Capture injects";
				return OBSBypassResult::DeferredModuleNotLoaded;
			}

			// Discovery said yes, so the hook must actually be installed. Anything else is a real
			// failure and must NOT leave the toggle looking successful.
			if (!OBSPresentHook || !OBSPresentHook->isHookInstalled())
			{
				PLOG_ERROR << std::format(
					"OBS bypass: discovery succeeded (graphics-hook64.dll+0x{:X}) but the inline hook did not install. See the attach log above.", rva);
				OBSPresentHook.reset();
				throw HCMRuntimeException("HCM found OBS's Present hook but could not install the bypass over it. Another program may have hooked it first.");
			}

			PLOG_INFO << std::format("OBS bypass installed over graphics-hook64.dll+0x{:X}", rva);
			return OBSBypassResult::Installed;
		}
		else
		{
			if (!OBSPresentHook) return OBSBypassResult::Removed; // don't need to reset if it's already reset

			if (presentHookRunning)
			{
				PLOG_INFO << "Waiting for presentHook to finish execution B";
				presentHookRunning.wait(true);
			}


			OBSPresentHook.reset();
			return OBSBypassResult::Removed;
		}

	}
	catch (HCMInitException ex)
	{
		throw HCMRuntimeException(ex);
	}
}