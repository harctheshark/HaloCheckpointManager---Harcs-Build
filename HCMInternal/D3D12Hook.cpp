#include "pch.h"
#include "D3D12Hook.h"
#include "HookGraveyard.h"
#include "ModuleCache.h"   // installOBSPreCapture asks whether graphics-hook64.dll is in the process yet

#include "GlobalKill.h"

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3d12")

#include "safetyhook.hpp"


D3D12Hook* D3D12Hook::instance = nullptr;
SimpleMath::Vector2 D3D12Hook::mScreenSize{ 1920, 1080 };
SimpleMath::Vector2 D3D12Hook::mScreenCenter{ 960, 540 };


#pragma region trampolines

// Cached trampolines for every hook we install.
//
// WHY file-static atomics instead of reaching through `instance` (which is what the first draft
// did, via d3d->presentHook.stdcall<>()):
//
//  1. CORRECTNESS. `instance` is null before beginHook() and after ~D3D12Hook. A detour that finds
//     it null has no way to reach the original, and the first draft returned 0 - which is S_OK.
//     The game then believes the frame was presented / the buffers were resized, and carries on
//     with stale-size buffers. That is corruption in the GAME, not in the overlay.
//  2. TEARDOWN. `presentHook = {}` in the destructor concurrently destroys the very object a
//     detour would be calling through.
//  3. PERFORMANCE. InlineHook::stdcall<>() takes a std::recursive_mutex on EVERY call
//     (inline_hook.hpp:225). These are inline hooks on dxgi's shared implementations, so that
//     would serialise every Present in the whole process against every other one.
//
// Publish order is: create the hook with StartDisabled -> store the trampoline here -> enable.
// That makes "the atomic is null" mean exactly one thing: the hook is not installed. Which in turn
// makes the fallback below provably non-recursive.
static std::atomic<DX12Present*> gOriginalPresent = nullptr;
static std::atomic<DX12Present1*> gOriginalPresent1 = nullptr;
static std::atomic<DX12ResizeBuffers*> gOriginalResizeBuffers = nullptr;
static std::atomic<DX12ResizeBuffers1*> gOriginalResizeBuffers1 = nullptr;
static std::atomic<DX12ExecuteCommandLists*> gOriginalExecuteCommandLists = nullptr;

// ---- OBS bypass: published trampolines and the retirement list -------------------------------------
// Same idea as the gOriginal* pointers above. A thread can be inside a bypass thunk, blocked on the
// shared lock, while teardownOBSBypass holds it exclusively; when it finally proceeds the hook object
// is gone. Reading the trampoline from an atomic published at install time gives that straggler
// somewhere to forward to, instead of it dropping the frame - which returned S_OK for a frame that was
// never presented, so the swapchain did not flip and the waitable object was never signalled.
//
// NOT cleared on teardown, deliberately: the hook is PARKED rather than freed, so the code these point
// at stays mapped forever and calling it is exactly right - it is OBS's own present.
static std::atomic<DX12Present*> gOBSBypassOriginalPresent = nullptr;
static std::atomic<DX12Present1*> gOBSBypassOriginalPresent1 = nullptr;
// Same teardown-race fallback for the pre-capture thunks. Separate slots, because a pre-capture hook sits
// on OBS's hook_present ENTRY while a bypass hook sits on its RealPresent trampoline - two different
// functions, and forwarding one through the other's original would skip or duplicate OBS's capture.
static std::atomic<DX12Present*> gOBSPreCaptureOriginalPresent = nullptr;
static std::atomic<DX12Present1*> gOBSPreCaptureOriginalPresent1 = nullptr;

// Leaked on purpose. See the long note on teardownOBSBypass: ModuleHookManager's deferred attach runs on
// the loader thread with no synchronisation, so a retired hook must outlive any thread that might still
// be inside attach() or resolve() on it. Function-local static so the vector itself is never destroyed.
static std::vector<std::shared_ptr<ModuleInlineHook>>& retiredOBSBypassHooks()
{
	static auto* v = new std::vector<std::shared_ptr<ModuleInlineHook>>();
	return *v;
}

// The fallbacks below call the COM method directly. That is safe precisely BECAUSE the trampoline
// atomic is null: safetyhook has already restored dxgi's original bytes (InlineHook::destroy ->
// disable() -> restore, then free the trampoline) and we never touched the vtable, so the interface
// method now points straight at the real implementation and cannot re-enter us.
// Reaching this at all means we lost a race with ~D3D12Hook; it must never silently drop the call.
static HRESULT callOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	if (auto* original = gOriginalPresent.load(std::memory_order_acquire))
		return original(pSwapChain, SyncInterval, Flags);
	return pSwapChain->Present(SyncInterval, Flags);
}

static HRESULT callOriginalPresent1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	if (auto* original = gOriginalPresent1.load(std::memory_order_acquire))
		return original(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
	return pSwapChain->Present1(SyncInterval, PresentFlags, pPresentParameters);
}

static HRESULT callOriginalResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	if (auto* original = gOriginalResizeBuffers.load(std::memory_order_acquire))
		return original(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	return pSwapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

static HRESULT callOriginalResizeBuffers1(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
	if (auto* original = gOriginalResizeBuffers1.load(std::memory_order_acquire))
		return original(pSwapChain, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
	return pSwapChain->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
}

static void callOriginalExecuteCommandLists(ID3D12CommandQueue* pCommandQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists)
{
	if (auto* original = gOriginalExecuteCommandLists.load(std::memory_order_acquire))
	{
		original(pCommandQueue, NumCommandLists, ppCommandLists);
		return;
	}
	pCommandQueue->ExecuteCommandLists(NumCommandLists, ppCommandLists);
}

#pragma endregion trampolines


// UNNAMED NAMESPACE (internal linkage) IS LOAD-BEARING: D3D11Hook.cpp declares its own
// `enum class IDXGISwapChainVMT` at namespace scope, and this one is no longer token-identical to it
// (we extend through IDXGISwapChain1/3). Two differing definitions of the same namespace-scope
// enumeration in one program is an ODR violation, no diagnostic required.
namespace
{

// IDXGISwapChain vtable layout (DXGI doesn't care which D3D version created the swapchain).
// Extended through IDXGISwapChain1 (Present1) and IDXGISwapChain3 (ResizeBuffers1); every index
// below was verified against the Windows SDK 10.0.26100 IDXGISwapChain3Vtbl struct.
enum class IDXGISwapChainVMT
{
	QueryInterface,
	AddRef,
	Release,
	SetPrivateData,
	SetPrivateDataInterface,
	GetPrivateData,
	GetParent,
	GetDevice,
	Present,			// 8
	GetBuffer,
	SetFullscreenState,
	GetFullscreenState,
	GetDesc,
	ResizeBuffers,		// 13
	ResizeTarget,
	GetContainingOutput,
	GetFrameStatistics,
	GetLastPresentCount,
	// ---- IDXGISwapChain1 ----
	GetDesc1,
	GetFullscreenDesc,
	GetHwnd,
	GetCoreWindow,
	Present1,			// 22
	IsTemporaryMonoSupported,
	GetRestrictToOutput,
	SetBackgroundColor,
	GetBackgroundColor,
	SetRotation,
	GetRotation,
	// ---- IDXGISwapChain2 ----
	SetSourceSize,
	GetSourceSize,
	SetMaximumFrameLatency,
	GetMaximumFrameLatency,
	GetFrameLatencyWaitableObject,
	SetMatrixTransform,
	GetMatrixTransform,
	// ---- IDXGISwapChain3 ----
	GetCurrentBackBufferIndex,
	CheckColorSpaceSupport,
	SetColorSpace1,
	ResizeBuffers1,		// 39
};

// ID3D12CommandQueue vtable layout.
// NOTE the private-data methods are in a DIFFERENT order to IDXGIObject's: ID3D12Object declares
// GetPrivateData first, IDXGIObject declares SetPrivateData first. Do not "tidy" these to match.
enum class ID3D12CommandQueueVMT
{
	QueryInterface,
	AddRef,
	Release,
	GetPrivateData,
	SetPrivateData,
	SetPrivateDataInterface,
	SetName,
	GetDevice,
	UpdateTileMappings,
	CopyTileMappings,
	ExecuteCommandLists,	// 10
	SetMarker,
	BeginEvent,
	EndEvent,
	Signal,
	Wait,
	GetTimestampFrequency,
	GetClockCalibration,
	GetDesc,
};

} // unnamed namespace


// How long we're willing to block the game's render thread waiting on our own fence.
// The reference waits INFINITE; on a removed/hung device that hangs the game forever, which is a
// much worse outcome than a missing overlay frame.
static constexpr DWORD kFenceWaitTimeoutMs = 1000;
static constexpr DWORD kGpuFlushTimeoutMs = 2000;
// Diagnostic only - how long we are willing to wait for the GAME'S captured queue to retire one fence at
// startup. See the queue probe in initializeD3Ddevice.
// (The old kQueueProbeTimeoutMs lived here. Removed with the blocking probe - see the render gate.)

// How often a swapchain we already rejected is re-tested. The rejection cache is a raw pointer
// compare, and a foreign swapchain that gets destroyed can have its address reused by the game's
// real one - so the cache must decay rather than blind us permanently.
static constexpr uint32_t kRejectedSwapChainRecheckInterval = 1024;


#pragma region detour entry guard

// Depth of our own swapchain detours on THIS thread. Two jobs:
//
//  1. DOUBLE-RENDER GUARD. dxgi's IDXGISwapChain::Present is free to forward to the same object's
//     Present1 inside dxgi.dll (and UE5 calls whichever it likes). Because we inline-hook BOTH
//     shared implementations, that forwarding re-enters us; without this the overlay would be
//     recorded and submitted twice for a single frame. The outermost entry owns the frame.
//  2. LOCK RE-ENTRANCY. MSVC's std::shared_mutex is an SRWLOCK. Taking it shared twice on one
//     thread deadlocks if a writer (i.e. ~D3D12Hook) queues in between. The nested entry does not
//     re-lock - it is covered by the outer frame's shared lock, which is exactly as strong.
static thread_local unsigned int tlDetourDepth = 0;

namespace
{
	class DetourEntryGuard
	{
		std::shared_lock<std::shared_mutex> mLock;
		bool mOutermost;

	public:
		explicit DetourEntryGuard(std::shared_mutex& guard)
			: mOutermost(tlDetourDepth == 0)
		{
			if (mOutermost)
				mLock = std::shared_lock<std::shared_mutex>(guard);
			++tlDetourDepth;
		}

		~DetourEntryGuard() { --tlDetourDepth; }

		DetourEntryGuard(const DetourEntryGuard&) = delete;
		DetourEntryGuard& operator=(const DetourEntryGuard&) = delete;

		bool isOutermost() const { return mOutermost; }
	};

	// Release, or - when the GPU could not be proven idle - deliberately LEAK.
	// Freeing a resource the GPU is still reading is the classic D3D12 unload crash, and it takes
	// the GAME down. Leaking a few MB during teardown is always the better trade.
	template <typename T>
	void releaseOrAbandon(T*& p, bool gpuIsIdle)
	{
		if (!p) return;
		if (gpuIsIdle)
			p->Release();
		else
			PLOG_ERROR << "Abandoning (leaking) a D3D12 object because the GPU could not be flushed";
		p = nullptr;
	}
}

#pragma endregion detour entry guard


#pragma region helpers

// Is this HWND a window belonging to our own process? Our Present/ResizeBuffers hooks are inline
// hooks on the shared dxgi implementation, so they see EVERY swapchain in the process (Steam
// overlay, Discord, RivaTuner, media foundation thumbnailers, ...). This is one of the two filters
// that narrow that down to the game's own swapchain (the other being "does GetDevice give us an
// ID3D12Device", which rejects every D3D11 swapchain for free).
bool D3D12Hook::isOwnedByThisProcess(HWND hwnd)
{
	if (!hwnd || !::IsWindow(hwnd))
		return false;

	DWORD windowProcessId = 0;
	::GetWindowThreadProcessId(hwnd, &windowProcessId);
	return windowProcessId == ::GetCurrentProcessId();
}

// COM canonical-identity comparison. Two interface pointers refer to the same object if and only
// if their IUnknown identities are equal - comparing (say) two ID3D12Device* directly is not valid,
// because a single object can hand out different pointers for different interfaces.
bool D3D12Hook::sameComObject(IUnknown* a, IUnknown* b)
{
	if (!a || !b)
		return false;

	IUnknown* identityA = nullptr;
	IUnknown* identityB = nullptr;
	a->QueryInterface(IID_PPV_ARGS(&identityA));
	b->QueryInterface(IID_PPV_ARGS(&identityB));

	const bool matches = (identityA != nullptr) && (identityA == identityB);

	safe_release(identityA);
	safe_release(identityB);

	return matches;
}

// This is what proves the queue we scraped out of ExecuteCommandLists actually belongs to the
// device that owns the swapchain we're about to draw into. Submitting our command list to a queue
// from a different device would be an immediate device-removal.
bool D3D12Hook::queueMatchesDevice(ID3D12CommandQueue* queue, ID3D12Device* expectedDevice)
{
	if (!queue || !expectedDevice)
		return false;

	ID3D12Device* queueDevice = nullptr;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !queueDevice)
		return false;

	const bool matches = sameComObject(queueDevice, expectedDevice);
	safe_release(queueDevice);
	return matches;
}

// Returns S_OK when the device is healthy. Callers MUST act on a non-S_OK result: a removed device
// fails identically on every subsequent frame, so retrying forever (with LOG_ONCE hiding it) just
// burns the render thread.
HRESULT D3D12Hook::checkDeviceRemoved(ID3D12Device* device, const char* context)
{
	if (!device) return S_OK;
	const HRESULT reason = device->GetDeviceRemovedReason();
	if (FAILED(reason))
		PLOG_ERROR << "D3D12 device removed reason at " << context << ": 0x" << std::hex << (ULONG)reason;
	return reason;
}

#pragma endregion helpers


#pragma region srv descriptor allocator

// imgui's dx12 backend does not own descriptor storage - it asks us for SRV descriptors (the font
// atlas, plus one per user texture) out of a heap we provide, and hands them back on shutdown.
// The reference implementation used a function-local "static UINT index" bump allocator with an
// empty free function, which leaked a slot on every re-init and would silently run off the end of
// the heap. This is a real free list so repeated shutdown/init cycles (HCM detaching and
// re-attaching) are safe.
//
// CRITICAL: imgui's backend keeps a raw, un-AddRef'd COPY of ImGui_ImplDX12_InitInfo, so the
// `info->Device` / `info->SrvDescriptorHeap` it hands back to us are only as valid as our heap.
// ImGui_ImplDX12_Shutdown() calls SrvDescriptorFreeFn, and it can run after we have released those
// objects. mSrvHeapLive is the gate; these callbacks must never trust `info` without it.
void D3D12Hook::resetSrvDescriptorAllocator()
{
	std::scoped_lock lock(mSrvFreeListMutex);
	mSrvFreeList.clear();
	mSrvFreeList.reserve(SRV_DESCRIPTOR_COUNT);
	// pushed in reverse so pop_back() hands out slot 0 first (nicer to read in a graphics debugger)
	for (UINT i = SRV_DESCRIPTOR_COUNT; i > 0; --i)
		mSrvFreeList.push_back(i - 1);
}

void D3D12Hook::srvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
{
	if (!mSrvHeapLive.load(std::memory_order_acquire))
	{
		// The heap (and possibly the device) behind `info` has been released. Hand back zeroed
		// handles rather than dereferencing freed COM objects.
		PLOG_ERROR << "srvDescriptorAlloc called after the SRV heap was released; ignoring";
		if (outCpu) *outCpu = D3D12_CPU_DESCRIPTOR_HANDLE{};
		if (outGpu) *outGpu = D3D12_GPU_DESCRIPTOR_HANDLE{};
		return;
	}

	if (!info || !info->Device || !info->SrvDescriptorHeap || !outCpu || !outGpu)
	{
		PLOG_ERROR << "srvDescriptorAlloc called with null info";
		return;
	}

	UINT index = 0;
	{
		std::scoped_lock lock(mSrvFreeListMutex);
		if (mSrvFreeList.empty())
		{
			// Handing back slot 0 keeps us inside the heap (no GPU fault); the overlay will just
			// draw the wrong texture. Better than an out-of-bounds descriptor.
			PLOG_ERROR << "SRV descriptor heap exhausted (" << SRV_DESCRIPTOR_COUNT << " descriptors); reusing slot 0";
		}
		else
		{
			index = mSrvFreeList.back();
			mSrvFreeList.pop_back();
		}
	}

	const UINT increment = info->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	*outCpu = info->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	*outGpu = info->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	outCpu->ptr += static_cast<SIZE_T>(index) * increment;
	outGpu->ptr += static_cast<UINT64>(index) * increment;
}

void D3D12Hook::srvDescriptorFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
	(void)gpu;

	// The common case on shutdown: the heap is gone, so there is nothing to give back and `info`
	// must not be dereferenced. See the comment on resetSrvDescriptorAllocator.
	if (!mSrvHeapLive.load(std::memory_order_acquire))
		return;

	if (!info || !info->Device || !info->SrvDescriptorHeap)
		return;

	const UINT increment = info->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	if (increment == 0) return;

	const SIZE_T heapStart = info->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;
	if (cpu.ptr < heapStart) return;

	const UINT index = static_cast<UINT>((cpu.ptr - heapStart) / increment);
	if (index >= SRV_DESCRIPTOR_COUNT) return;

	std::scoped_lock lock(mSrvFreeListMutex);
	// guard against a double free putting the same slot in the list twice
	if (std::find(mSrvFreeList.begin(), mSrvFreeList.end(), index) == mSrvFreeList.end())
		mSrvFreeList.push_back(index);
}

#pragma endregion srv descriptor allocator


D3D12Hook::D3D12Hook(std::weak_ptr<PointerDataStore> pointerDataStore)
	: pointerDataStoreWeak(pointerDataStore)
{
	if (instance != nullptr)
	{
		throw HCMInitException("Cannot have more than one D3D12Hook");
	}
	instance = this;

	// Every one of these is `static inline`, so it survives a previous D3D12Hook. Reset them here
	// rather than in the destructor: shuttingDown in particular must STAY true after teardown so a
	// straggling detour keeps forwarding.
	shuttingDown.store(false, std::memory_order_release);
	mQueueSearchActive.store(true, std::memory_order_relaxed);
	mRejectedSwapChain.store(nullptr, std::memory_order_relaxed);
	mRejectedSwapChainSkips.store(0, std::memory_order_relaxed);
	mSrvHeapLive.store(false, std::memory_order_release);

	// NOTE: deliberately no hooking here. Same two-phase pattern as D3D11Hook - the constructor
	// runs before most of HCM's services exist, so a detour firing now would call into a
	// half-constructed service graph. beginHook() is called last, from App.h.
	resetSrvDescriptorAllocator();
}


#pragma region address harvesting

// Optional fast path: if this build of the game ever gets a pIDXGISwapChain entry in
// InternalPointerData.xml (like MCC has), we can read Present/ResizeBuffers straight off the live
// object and skip creating a dummy swapchain entirely. Dummy swapchains are known to upset
// RivaTuner and friends, which is exactly why HCM prefers resolved pointers for the D3D11 path.
// HaloCampaignEvolved.exe has no such entry today, so this normally returns nullptr.
//
// NOTE it can only ever give us slots 8 and 13: reading slot 22 (Present1) or 39 (ResizeBuffers1)
// off an object we have not proven to be an IDXGISwapChain1/3 would be an out-of-bounds vtable read
// and we would end up hooking whatever happened to sit past the end of the table.
void** D3D12Hook::resolveLiveSwapChainVtable()
{
	try
	{
		auto ptr = pointerDataStoreWeak.lock();
		if (!ptr)
			return nullptr;

		uintptr_t pIDXGISwapChain = 0;
		auto mlp_IDXGISwapChain = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(pIDXGISwapChain));
		if (!mlp_IDXGISwapChain || !mlp_IDXGISwapChain->resolve(&pIDXGISwapChain) || pIDXGISwapChain == 0)
			return nullptr;

		PLOG_INFO << "Resolved a live IDXGISwapChain from pointer data: 0x" << std::hex << pIDXGISwapChain;
		return *reinterpret_cast<void***>(pIDXGISwapChain);
	}
	catch (...)
	{
		// getData throws HCMInitException when the datum simply isn't present for this build.
		// That is the expected case for HaloCampaignEvolved.exe - fall back to the dummy objects.
		PLOG_DEBUG << "No pIDXGISwapChain pointer data for this build; falling back to dummy D3D12 objects";
		return nullptr;
	}
}

// Port of the reference overlay's GetHookAddresses(), with its null-deref bug fixed (it read the
// factory vtable before checking the CreateDXGIFactory1 HRESULT) and with a WARP fallback added,
// mirroring the multi-attempt / accumulate-errors-then-throw structure of
// D3D11Hook::CreateDummySwapchain.
//
// We build a throwaway window + factory + device + queue + swapchain purely to read a handful of
// vtable entries. Those entries are the *shared* implementations inside dxgi.dll / d3d12.dll, so
// hooking them catches the game's real swapchain and command queue no matter when we were injected.
D3D12Hook::HookAddresses D3D12Hook::harvestHookAddresses()
{
	HookAddresses addresses{};
	std::vector<std::string> errorCodes;

	// NOTE: a lambda, not a macro. The previous macro expanded its argument twice, so every
	// std::format() in a call site was executed twice.
	auto logHarvestFailure = [&errorCodes](std::string message)
		{
			PLOG_ERROR << message;
			errorCodes.push_back(std::move(message));
		};

	const wchar_t* const dummyWindowClassName = L"HCM_TemporaryD3D12Window";
	HINSTANCE dummyWindowInstance = GetModuleHandleW(nullptr); // the EXE, matching the reference
	HWND dummyWindow = nullptr;
	bool registeredClass = false;

	IDXGIFactory4* factory = nullptr;
	ID3D12Device* dummyDevice = nullptr;
	ID3D12CommandQueue* dummyQueue = nullptr;
	IDXGISwapChain1* dummySwapChain = nullptr;
	IDXGISwapChain3* dummySwapChain3 = nullptr;
	IDXGIAdapter* warpAdapter = nullptr;

	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (FAILED(hr) || !factory)
	{
		logHarvestFailure(std::format("CreateDXGIFactory1 failed, error: {:x}", (ULONG)hr));
	}
	else
	{
		// attempt 1: default adapter (what the game itself will be using)
		hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice));
		if (FAILED(hr) || !dummyDevice)
		{
			logHarvestFailure(std::format("D3D12CreateDevice on the default adapter failed, error: {:x}", (ULONG)hr));

			// attempt 2: WARP. Slower to create but always present, and we only ever read vtables.
			if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))) && warpAdapter)
			{
				hr = D3D12CreateDevice(warpAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice));
				if (FAILED(hr) || !dummyDevice)
				{
					logHarvestFailure(std::format("D3D12CreateDevice on the WARP adapter failed, error: {:x}", (ULONG)hr));
				}
				else
				{
					// The ID3D12CommandQueue implementation is the D3D12 runtime's, so a WARP device
					// normally yields the same ExecuteCommandLists address as the game's hardware
					// device. That stops being true if the game's Agility SDK (UE5.5 ships the
					// D3D12Core.dll redist) has not yet been resolved process-wide when we run.
					PLOG_WARNING << "Harvested the D3D12 hook addresses from a WARP device. If the game "
						<< "uses a different D3D12 runtime (Agility SDK) the ExecuteCommandLists address "
						<< "may not match and the overlay will never find the game's command queue.";
				}
			}
		}
	}

	if (dummyDevice)
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

		hr = dummyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue));
		if (FAILED(hr) || !dummyQueue)
		{
			logHarvestFailure(std::format("CreateCommandQueue on the dummy device failed, error: {:x}", (ULONG)hr));
		}
	}

	if (dummyQueue)
	{
		void** queueVtable = *reinterpret_cast<void***>(dummyQueue);
		addresses.executeCommandLists = queueVtable[(size_t)ID3D12CommandQueueVMT::ExecuteCommandLists];
		PLOG_DEBUG << "ID3D12CommandQueue::ExecuteCommandLists @ 0x" << std::hex << (uintptr_t)addresses.executeCommandLists;
	}

	if (factory && dummyQueue)
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = dummyWindowInstance;
		wc.lpszClassName = dummyWindowClassName;

		if (!RegisterClassExW(&wc))
		{
			// A previous attach may have left the class registered; that's fine, carry on.
			const DWORD lastError = GetLastError();
			if (lastError != ERROR_CLASS_ALREADY_EXISTS)
			{
				logHarvestFailure(std::format("RegisterClassExW for the dummy window failed, error: {}", lastError));
			}
		}
		else
		{
			registeredClass = true;
		}

		dummyWindow = CreateWindowExW(0, dummyWindowClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, dummyWindowInstance, nullptr);
		if (!dummyWindow)
		{
			logHarvestFailure(std::format("CreateWindowExW for the dummy window failed, error: {}", GetLastError()));
		}
		else
		{
			DXGI_SWAP_CHAIN_DESC1 swapDesc{};
			swapDesc.Width = 100;
			swapDesc.Height = 100;
			swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			swapDesc.SampleDesc.Count = 1;
			swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			swapDesc.BufferCount = 2;
			swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

			// NOTE: this runs BEFORE our hooks are installed, so it cannot re-enter our detours.
			// If this call is ever moved after beginHook()'s create_inline calls, it will.
			hr = factory->CreateSwapChainForHwnd(dummyQueue, dummyWindow, &swapDesc, nullptr, nullptr, &dummySwapChain);
			if (FAILED(hr) || !dummySwapChain)
			{
				logHarvestFailure(std::format("CreateSwapChainForHwnd for the dummy swapchain failed, error: {:x}", (ULONG)hr));
			}
			else
			{
				void** swapChainVtable = *reinterpret_cast<void***>(dummySwapChain);
				addresses.present = swapChainVtable[(size_t)IDXGISwapChainVMT::Present];
				addresses.resizeBuffers = swapChainVtable[(size_t)IDXGISwapChainVMT::ResizeBuffers];
				// Safe: dummySwapChain is statically an IDXGISwapChain1*, so slot 22 is in bounds.
				addresses.present1 = swapChainVtable[(size_t)IDXGISwapChainVMT::Present1];
				PLOG_DEBUG << "IDXGISwapChain::Present @ 0x" << std::hex << (uintptr_t)addresses.present;
				PLOG_DEBUG << "IDXGISwapChain::ResizeBuffers @ 0x" << std::hex << (uintptr_t)addresses.resizeBuffers;
				PLOG_DEBUG << "IDXGISwapChain1::Present1 @ 0x" << std::hex << (uintptr_t)addresses.present1;

				// Slot 39 is only in bounds once the object has proven it implements IDXGISwapChain3.
				if (SUCCEEDED(dummySwapChain->QueryInterface(IID_PPV_ARGS(&dummySwapChain3))) && dummySwapChain3)
				{
					void** swapChain3Vtable = *reinterpret_cast<void***>(dummySwapChain3);
					addresses.resizeBuffers1 = swapChain3Vtable[(size_t)IDXGISwapChainVMT::ResizeBuffers1];
					PLOG_DEBUG << "IDXGISwapChain3::ResizeBuffers1 @ 0x" << std::hex << (uintptr_t)addresses.resizeBuffers1;
				}
				else
				{
					PLOG_WARNING << "The dummy swapchain does not implement IDXGISwapChain3; ResizeBuffers1 will not be hooked";
				}
			}
		}
	}

	// Fallback for the two required swapchain addresses only (see resolveLiveSwapChainVtable).
	if (!addresses.present || !addresses.resizeBuffers)
	{
		if (void** liveSwapChainVtable = resolveLiveSwapChainVtable())
		{
			if (!addresses.present)
				addresses.present = liveSwapChainVtable[(size_t)IDXGISwapChainVMT::Present];
			if (!addresses.resizeBuffers)
				addresses.resizeBuffers = liveSwapChainVtable[(size_t)IDXGISwapChainVMT::ResizeBuffers];
		}
	}

	safe_release(dummySwapChain3);
	safe_release(dummySwapChain);
	safe_release(dummyQueue);
	safe_release(dummyDevice);
	safe_release(warpAdapter);
	safe_release(factory);

	if (dummyWindow) DestroyWindow(dummyWindow);
	if (registeredClass) UnregisterClassW(dummyWindowClassName, dummyWindowInstance);

	if (!addresses.present || !addresses.resizeBuffers || !addresses.executeCommandLists)
	{
		std::string resultsString = "Failed to harvest the D3D12/DXGI function addresses HCM needs to hook.\n";
		for (auto& error : errorCodes)
			resultsString += error + "\n";
		throw HCMInitException(resultsString);
	}

	return addresses;
}

#pragma endregion address harvesting


void D3D12Hook::beginHook()
{
	PLOG_DEBUG << "D3D12Hook::beginHook";

	try
	{
		const HookAddresses addresses = harvestHookAddresses();
		// Remembered for the OBS bypass, which needs to know which dxgi entry points to look for
		// Detours trampolines back into. See setOBSBypass.
		mHarvestedAddresses = addresses;

		// Every hook below follows the same three-step dance:
		//     create (StartDisabled) -> publish the trampoline -> enable
		// so that a detour firing on another thread can never observe an installed hook whose
		// trampoline pointer has not been published yet. See the gOriginal* comment block.

		// ExecuteCommandLists first: it is the ONLY source of the game's command queue under late
		// injection, and Present refuses to render until it has produced one. Installing it before
		// Present just means we're more likely to already have a candidate on the first frame.
		executeCommandListsHook = safetyhook::create_inline(addresses.executeCommandLists, &newDX12ExecuteCommandLists, safetyhook::InlineHook::StartDisabled);
		if (!executeCommandListsHook)
			throw HCMInitException("Failed to hook ID3D12CommandQueue::ExecuteCommandLists");
		gOriginalExecuteCommandLists.store(executeCommandListsHook.original<DX12ExecuteCommandLists*>(), std::memory_order_release);
		if (!executeCommandListsHook.enable())
			throw HCMInitException("Failed to enable the ID3D12CommandQueue::ExecuteCommandLists hook");

		presentHook = safetyhook::create_inline(addresses.present, &newDX12Present, safetyhook::InlineHook::StartDisabled);
		if (!presentHook)
			throw HCMInitException("Failed to hook IDXGISwapChain::Present");
		gOriginalPresent.store(presentHook.original<DX12Present*>(), std::memory_order_release);
		if (!presentHook.enable())
			throw HCMInitException("Failed to enable the IDXGISwapChain::Present hook");

		// UE5's D3D12 RHI presents through Present1, so this one is what actually makes the overlay
		// appear on HaloCampaignEvolved.exe. It is still only best-effort: on the (impossible on
		// Win10+, but cheap to tolerate) chance that Present1 could not be harvested we keep the
		// slot-8 hook and log, rather than refusing to start.
		// If dxgi implements both slots with the same body, hooking it twice would corrupt the
		// prologue - so never install the second hook over the same address.
		if (addresses.present1 && addresses.present1 != addresses.present)
		{
			present1Hook = safetyhook::create_inline(addresses.present1, &newDX12Present1, safetyhook::InlineHook::StartDisabled);
			if (!present1Hook)
			{
				PLOG_ERROR << "Failed to hook IDXGISwapChain1::Present1; the overlay will only appear if the game presents through IDXGISwapChain::Present";
			}
			else
			{
				gOriginalPresent1.store(present1Hook.original<DX12Present1*>(), std::memory_order_release);
				if (!present1Hook.enable())
				{
					PLOG_ERROR << "Failed to enable the IDXGISwapChain1::Present1 hook";
					gOriginalPresent1.store(nullptr, std::memory_order_release);
					present1Hook = {};
				}
			}
		}
		else
		{
			PLOG_WARNING << "IDXGISwapChain1::Present1 was not hooked (address unavailable, or identical to Present)";
		}

		resizeBuffersHook = safetyhook::create_inline(addresses.resizeBuffers, &newDX12ResizeBuffers, safetyhook::InlineHook::StartDisabled);
		if (!resizeBuffersHook)
			throw HCMInitException("Failed to hook IDXGISwapChain::ResizeBuffers");
		gOriginalResizeBuffers.store(resizeBuffersHook.original<DX12ResizeBuffers*>(), std::memory_order_release);
		if (!resizeBuffersHook.enable())
			throw HCMInitException("Failed to enable the IDXGISwapChain::ResizeBuffers hook");

		// Best-effort, same reasoning as Present1: missing it means the game could resize while we
		// still hold back-buffer references, which fails its resize - so we want it if we can get it.
		if (addresses.resizeBuffers1 && addresses.resizeBuffers1 != addresses.resizeBuffers)
		{
			resizeBuffers1Hook = safetyhook::create_inline(addresses.resizeBuffers1, &newDX12ResizeBuffers1, safetyhook::InlineHook::StartDisabled);
			if (!resizeBuffers1Hook)
			{
				PLOG_ERROR << "Failed to hook IDXGISwapChain3::ResizeBuffers1";
			}
			else
			{
				gOriginalResizeBuffers1.store(resizeBuffers1Hook.original<DX12ResizeBuffers1*>(), std::memory_order_release);
				if (!resizeBuffers1Hook.enable())
				{
					PLOG_ERROR << "Failed to enable the IDXGISwapChain3::ResizeBuffers1 hook";
					gOriginalResizeBuffers1.store(nullptr, std::memory_order_release);
					resizeBuffers1Hook = {};
				}
			}
		}

		// ⚠ SNAPSHOT WHAT WE JUST WROTE. This is what makes the shutdown safe around other overlays.
		//
		// HCM inline-hooks the SHARED dxgi/D3D12Core functions, which RivaTuner, the Steam overlay and NVIDIA
		// Streamline all patch too. RTSS in particular RE-HOOKS PERIODICALLY: it can come along after us, write
		// its jump over ours, and relocate our jump into its own trampoline. If HCM then shuts down and blindly
		// restores the bytes it saved at install, it ERASES RTSS's jump - a hook RTSS still believes is live.
		//
        // That is a measured, reproducible crash, not a hypothesis: with RTSS running the game dies on HCM close
        // with `PresentInternal failed ... 0x80004004` and NO exception anywhere, because nothing faults - the
        // present chain is simply cut. Closing RTSS makes it stop.
		//
		// So remember our own bytes here, and at teardown compare before restoring. See snapshotHookSites().
		snapshotHookSites();

		// Frames may never reach us if another overlay owns the chain - see startRehookWatchdog().
		startRehookWatchdog();

		PLOG_DEBUG << "D3D12 hooks set";
	}
	catch (HCMRuntimeException ex)
	{
		// App.h only catches HCMInitException, and lockOrThrow/PointerDataStore throw runtime ones.
		throw HCMInitException(ex.what());
	}
}


// Records the first bytes of every hook site immediately after install, so teardown can tell "still ours" from
// "someone hooked over us". See the call site for why that distinction is load-bearing.
void D3D12Hook::snapshotHookSites()
{
	struct Site { safetyhook::InlineHook* hook; };
	safetyhook::InlineHook* hooks[kHookSiteCount] = {
		&presentHook, &present1Hook, &resizeBuffersHook, &resizeBuffers1Hook, &executeCommandListsHook };

	for (int i = 0; i < kHookSiteCount; i++)
	{
		mHookSites[i].target = nullptr;
		mHookSites[i].valid = false;
		if (!*hooks[i]) continue;

		uint8_t* t = hooks[i]->target();
		if (!t || IsBadReadPtr(t, kHookSiteSnapshotBytes)) continue;

		memcpy(mHookSites[i].bytes, t, kHookSiteSnapshotBytes);
		mHookSites[i].target = t;
		mHookSites[i].valid = true;
	}
}


// True when the site still contains exactly what we wrote. False means a third party patched over us, and
// restoring our saved "original" bytes would destroy THEIR hook.
bool D3D12Hook::hookSiteIsStillOurs(int index) const
{
	if (index < 0 || index >= kHookSiteCount) return true;   // unknown -> behave as before
	const auto& s = mHookSites[index];
	if (!s.valid || !s.target) return true;
	if (IsBadReadPtr(s.target, kHookSiteSnapshotBytes)) return true;
	return memcmp(s.target, s.bytes, kHookSiteSnapshotBytes) == 0;
}


#pragma region adoption

// Called from the Present detour whenever we see a swapchain that isn't the one we've adopted.
// Returns true if (and only if) this swapchain is one we should be drawing into.
bool D3D12Hook::tryAdoptSwapChain(IDXGISwapChain* pSwapChain)
{
	if (!pSwapChain)
		return false;

	// Filter 1: is it a D3D12 swapchain at all? Every D3D11 swapchain in the process (Steam
	// overlay, RivaTuner, HCM's own dummy in other code paths) fails this for free.
	IDXGISwapChain3* candidateSwapChain3 = nullptr;
	if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&candidateSwapChain3))) || !candidateSwapChain3)
	{
		LOG_ONCE(PLOG_VERBOSE << "Present on a swapchain that isn't an IDXGISwapChain3; ignoring");
		return false;
	}

	ID3D12Device* candidateDevice = nullptr;
	if (FAILED(candidateSwapChain3->GetDevice(IID_PPV_ARGS(&candidateDevice))) || !candidateDevice)
	{
		LOG_ONCE(PLOG_VERBOSE << "Present on a non-D3D12 swapchain; ignoring");
		safe_release(candidateSwapChain3);
		return false;
	}

	// Filter 2: is the output window one of ours? Rejects out-of-process / thumbnail swapchains.
	DXGI_SWAP_CHAIN_DESC swapDesc{};
	if (FAILED(candidateSwapChain3->GetDesc(&swapDesc)) || !isOwnedByThisProcess(swapDesc.OutputWindow))
	{
		LOG_ONCE(PLOG_VERBOSE << "Present on a D3D12 swapchain whose window isn't ours; ignoring");
		safe_release(candidateDevice);
		safe_release(candidateSwapChain3);
		return false;
	}

	if (mAdoptedSwapChain != nullptr)
	{
		// Filter 3: we already have a swapchain. Only treat this as a REPLACEMENT if it targets the
		// same window - otherwise it is a second, unrelated swapchain (a secondary window, a video
		// capture surface) and adopting it would make us ping-pong between the two, tearing down and
		// rebuilding every D3D12 resource on alternate frames.
		if (swapDesc.OutputWindow != mWindowHandle)
		{
			LOG_ONCE(PLOG_INFO << "Ignoring a second D3D12 swapchain on a different window");
			safe_release(candidateDevice);
			safe_release(candidateSwapChain3);
			return false;
		}

		// The game replaced its swapchain under us (fullscreen transition on some drivers, or a
		// device-lost recovery). Everything we built is tied to the old one.
		PLOG_INFO << "Game swapchain changed (old: 0x" << std::hex << (uintptr_t)mAdoptedSwapChain
			<< ", new: 0x" << (uintptr_t)pSwapChain << "); rebuilding D3D12 resources";

		// COM identity, not pointer equality - see sameComObject.
		const bool sameDevice = sameComObject(mDevice, candidateDevice);
		const bool gpuIsIdle = waitForGpuIdle();

		if (!sameDevice && mHasFiredPresentEvent)
		{
			// imgui's dx12 backend is bound to the OLD device and to our SRV heap, and holds raw,
			// un-AddRef'd pointers to both. We cannot re-init it from here, and we must not free
			// them either - ~ImGuiManager still has to run ImGui_ImplDX12_Shutdown against them.
			// So: drop everything tied to the swapchain, keep the imgui-bound objects alive, and
			// stop touching D3D12 for the rest of the session.
			PLOG_FATAL << "The game's D3D12 device changed. imgui's dx12 backend is bound to the old "
				<< "device and cannot be re-initialised from D3D12Hook. (Fixing this properly needs "
				<< "ImGuiManager to tear down and re-init its dx12 backend in response to a "
				<< "device-changed event.)";
			releaseSwapChainResources(gpuIsIdle);
			disableOverlayPermanently("the game's D3D12 device changed under a live imgui backend");
			safe_release(candidateDevice);
			safe_release(candidateSwapChain3);
			return false;
		}

		if (sameDevice)
		{
			// Cheap path: keep the device, the command queue, the fence and - critically - the SRV
			// heap, because imgui's dx12 backend is holding raw pointers to the device and that heap
			// and we have no way to make it re-init from here.
			releaseSwapChainResources(gpuIsIdle);
		}
		else
		{
			// imgui was never bound, so a full rebuild on the new device is still possible.
			releaseD3Dresources(gpuIsIdle);
			releaseImGuiBoundResources(gpuIsIdle);
		}

		// releaseSwapChainResources deliberately does not touch mDevice, so drop our reference to the
		// old one HERE - otherwise the assignment below overwrites a still-AddRef'd pointer and we
		// leak one ID3D12Device (and with it the queue, heaps and allocators it owns) on every
		// fullscreen transition / swapchain recreation.
		// In the sameDevice case this is a plain refcount balance, not a destruction: candidateDevice
		// already holds a fresh reference to the very same COM object.
		safe_release(mDevice);
	}

	mAdoptedSwapChain = pSwapChain;   // identity only; kept valid by the ref mSwapChain3 holds
	mSwapChain3 = candidateSwapChain3; // takes ownership of the QueryInterface ref
	mDevice = candidateDevice;         // takes ownership of the GetDevice ref
	mWindowHandle = swapDesc.OutputWindow;
	isD3DdeviceInitialized = false;
	// A new swapchain has to re-earn the right to draw: its presenting queue may be a different object
	// entirely, so the previous verdict says nothing about it.
	mRenderGate.store(RenderGate::Discovering, std::memory_order_release);
	mLiveQueueRaw.store(nullptr, std::memory_order_relaxed);
	mPresentIntervalsObserved.store(0, std::memory_order_relaxed);
	mQueueSearchActive.store(true, std::memory_order_relaxed);

	PLOG_INFO << "Adopted the game's D3D12 swapchain: 0x" << std::hex << (uintptr_t)pSwapChain
		<< ", device: 0x" << (uintptr_t)mDevice
		<< ", hwnd: 0x" << (uintptr_t)mWindowHandle;

	return true;
}

// THE CRUX OF THE WHOLE PORT.
//
// D3D12 has no immediate context. To draw anything we must submit a command list to a command
// queue, and it has to be the SAME queue the game submitted its frame on - queue submissions are
// ordered, so submitting after the game's frame work on that queue is what guarantees the overlay
// lands on top of the rendered scene instead of racing it.
//
// There is no API to ask a swapchain for its queue. In D3D12 the queue is passed to
// CreateSwapChainForHwnd, so an overlay that loads BEFORE the game can hook that call and read it
// straight out of the arguments (that is what the reference overlay does). HCM is injected into an
// already-running game, so that call happened long ago and that path can never fire for us.
//
// The only remaining source is ID3D12CommandQueue::ExecuteCommandLists: every frame the game
// submits its work through it, and the "this" pointer of the most recent DIRECT-type submission is
// the queue it is about to present from. So the reference's dead-code hook becomes our load-bearing
// one. We then validate the candidate against the swapchain's device (COM identity, not pointer
// equality) before trusting it.
bool D3D12Hook::tryCaptureCommandQueue()
{
	// ⚠ SUPERSEDED BY THE RENDER GATE, kept only so nothing calls a missing symbol. The old body took
	// whatever DIRECT queue happened to be in a single global slot, checked only that it was on the
	// game's device, and latched it for the session. With RTSS + Steam + OBS + Streamline/DLSS-G there
	// are four to six such queues and all of them pass that check - a 1 ms coin flip that hung the GPU
	// in 3 of 8 measured sessions. Selection now goes through updateRenderGate(), which PROVES a queue
	// drains before anything is submitted on it.
	return mCommandQueue != nullptr;
}


// ================================================================================================
// THE RENDER GATE
//
// The invariant, and the only one that matters: HCM never submits a command list to an
// ID3D12CommandQueue that has not, on this device, retired a fence WE signalled on it - and it proves
// that without ever blocking a present thread.
// ================================================================================================

const char* D3D12Hook::renderGateName(RenderGate g)
{
	switch (g)
	{
	case RenderGate::Discovering: return "Discovering";
	case RenderGate::Verifying:   return "Verifying";
	case RenderGate::Live:        return "Live";
	case RenderGate::Degraded:    return "Degraded";
	case RenderGate::Refused:     return "Refused";
	}
	return "?";
}

void D3D12Hook::setRenderGate(RenderGate to, const char* why)
{
	const RenderGate from = mRenderGate.exchange(to, std::memory_order_acq_rel);
	if (from == to) return;
	PLOG_INFO << "HCM render gate: " << renderGateName(from) << " -> " << renderGateName(to) << " (" << why << ")";
}

void D3D12Hook::refuseToDraw(const char* why)
{
	setRenderGate(RenderGate::Refused, why);
	mLiveQueueRaw.store(nullptr, std::memory_order_relaxed);
	PLOG_ERROR << "HCM will NOT draw its overlay this session: " << why
		<< ". The game is unaffected and every frame is passed through untouched - this is deliberate, "
		"because submitting on a queue we cannot verify is what wedges the GPU. Reattach HCM to try again.";
}

// static. Runs on arbitrary game threads. Keeps ONE owned reference per distinct DIRECT queue.
//
// ⚠ WHY AN OWNED REFERENCE: the consumer runs on a present thread one or more frames later. Without
// the AddRef a transient DIRECT queue that submits once and is then released leaves that consumer
// calling a vtable on freed memory - a hard crash in the GAME, not in HCM.
void D3D12Hook::recordQueueSubmission(ID3D12CommandQueue* q)
{
	std::scoped_lock lock(mQueueTableMutex);

	for (uint32_t i = 0; i < mQueueTableCount; ++i)
	{
		if (mQueueTable[i].queue == q)
		{
			++mQueueTable[i].totalSubmissions;
			++mQueueTable[i].submissionsThisInterval;
			return;
		}
	}

	if (mQueueTableCount >= kMaxQueueCandidates)
	{
		static std::atomic_bool warned = false;
		if (!warned.exchange(true, std::memory_order_relaxed))
			PLOG_WARNING << "More than " << kMaxQueueCandidates
				<< " DIRECT command queues in this process; not tracking further candidates";
		return;
	}

	q->AddRef();
	QueueCandidate& c = mQueueTable[mQueueTableCount++];
	c = QueueCandidate{};
	c.queue = q;
	c.totalSubmissions = 1;
	c.submissionsThisInterval = 1;
}

void D3D12Hook::logQueueTable(const char* when)
{
	std::scoped_lock lock(mQueueTableMutex);
	const uint32_t intervals = mPresentIntervalsObserved.load(std::memory_order_relaxed);
	PLOG_INFO << "Command queue candidates (" << when << ", after " << intervals << " presents): "
		<< mQueueTableCount << " DIRECT queues on this process";
	for (uint32_t i = 0; i < mQueueTableCount; ++i)
	{
		const QueueCandidate& c = mQueueTable[i];
		PLOG_INFO << "   0x" << std::hex << (uintptr_t)c.queue << std::dec
			<< "  submissions " << c.totalSubmissions
			<< "  seen in " << c.intervalsSeenIn << "/" << intervals << " intervals"
			<< (c.verified ? "  [VERIFIED]" : "")
			<< (c.rejected ? std::string("  [REJECTED: ") + (c.rejectReason ? c.rejectReason : "?") + "]" : "");
	}
}

void D3D12Hook::rejectQueue(ID3D12CommandQueue* q, const char* why)
{
	std::scoped_lock lock(mQueueTableMutex);
	for (uint32_t i = 0; i < mQueueTableCount; ++i)
	{
		if (mQueueTable[i].queue == q)
		{
			mQueueTable[i].rejected = true;
			mQueueTable[i].verified = false;
			mQueueTable[i].rejectReason = why;
			PLOG_ERROR << "Rejected command queue 0x" << std::hex << (uintptr_t)q << std::dec
				<< ": " << why << ". HCM will NOT submit on it.";
			return;
		}
	}
}

bool D3D12Hook::queueIsRejected(ID3D12CommandQueue* q)
{
	std::scoped_lock lock(mQueueTableMutex);
	for (uint32_t i = 0; i < mQueueTableCount; ++i)
		if (mQueueTable[i].queue == q) return mQueueTable[i].rejected;
	return false;
}

void D3D12Hook::beginProof(ID3D12CommandQueue* q, const char* how, bool isRevalidation)
{
	if (!mProofFence)
	{
		if (FAILED(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mProofFence))) || !mProofFence)
		{
			refuseToDraw("the queue-verification fence could not be created");
			return;
		}
	}

	mProofQueue = q;
	mProofValue = ++mProofNextValue;
	mProofPresentsWaited = 0;
	mProofHow = how;
	mProofIsRevalidation = isRevalidation;

	if (FAILED(q->Signal(mProofFence, mProofValue)))
	{
		rejectQueue(q, "Signal() failed on it");
		mProofQueue = nullptr;
		return;
	}

	if (!isRevalidation)
		setRenderGate(RenderGate::Verifying, how);
}

// Ranks candidates and starts a NON-BLOCKING proof on the best one. Ranking preference:
//   1. must be on the swapchain's device (COM identity) - anything else is removed outright
//   2. must not already have failed a proof
//   3. prefer a queue that submitted in EVERY observed present interval. UE5's 3D queue does; an OSD
//      renderer, an 11on12 flush or an encoder running on its own cadence does not.
//   4. then the highest total submission count - UE submits many lists per frame, an overlay submits one.
bool D3D12Hook::beginQueueVerification()
{
	std::scoped_lock resourceLock(mResourceMutex);
	if (!mDevice) return false;

	ID3D12CommandQueue* best = nullptr;
	uint64_t bestSubs = 0;
	uint32_t bestIntervals = 0;
	const uint32_t intervals = mPresentIntervalsObserved.load(std::memory_order_relaxed);

	{
		std::scoped_lock lock(mQueueTableMutex);
		for (uint32_t i = 0; i < mQueueTableCount; ++i)
		{
			QueueCandidate& c = mQueueTable[i];
			if (c.rejected) continue;
			if (!queueMatchesDevice(c.queue, mDevice))
			{
				c.rejected = true;
				c.rejectReason = "belongs to a different D3D12 device";
				continue;
			}
			const bool everyInterval = (c.intervalsSeenIn >= intervals);
			const bool bestEveryInterval = (best && bestIntervals >= intervals);
			if (best && bestEveryInterval && !everyInterval) continue;
			if (!best || (everyInterval && !bestEveryInterval) || c.totalSubmissions > bestSubs)
			{
				best = c.queue; bestSubs = c.totalSubmissions; bestIntervals = c.intervalsSeenIn;
			}
		}
	}

	logQueueTable("ranking");

	if (!best)
	{
		// Be patient before giving up: a level load or an alt-tab legitimately produces no submissions.
		if (intervals > kQueueObserveIntervals * 40)
			refuseToDraw("no DIRECT command queue on the game's D3D12 device could be verified");
		return false;
	}

	beginProof(best, "submission ranking", false);
	return false;
}

// THE NON-BLOCKING PROOF.
//
// ⚠ A Signal on an IDLE queue retires IMMEDIATELY. So a queue that has not retired after the GAME has
// presented eight more times is, by definition, not keeping pace with presentation - whatever it
// belongs to. That is the correct test, and it is why this works WITH RTSS rather than around it: the
// old test was 250 ms of WALL CLOCK, and RTSS's limiter deliberately holds presentation, so wall clock
// is not a meaningful unit in this configuration. Presents are.
bool D3D12Hook::pollQueueVerification()
{
	std::scoped_lock resourceLock(mResourceMutex);

	if (!mProofQueue || !mProofFence)
	{
		beginQueueVerification();
		return false;
	}

	if (mProofFence->GetCompletedValue() >= mProofValue)
	{
		adoptVerifiedQueue(mProofQueue);
		mProofQueue = nullptr;
		return true;
	}

	if (++mProofPresentsWaited < kQueueProofPresents)
		return false;   // still waiting. NOT an error, and NOT a blocking wait.

	rejectQueue(mProofQueue, "did not retire a fence within 8 of the game's own presents");
	mProofQueue = nullptr;
	beginQueueVerification();   // straight on to the next-best candidate
	return false;
}

void D3D12Hook::adoptVerifiedQueue(ID3D12CommandQueue* q)
{
	std::scoped_lock resourceLock(mResourceMutex);

	if (mCommandQueue != q)
	{
		safe_release(mCommandQueue);
		q->AddRef();
		mCommandQueue = q;
	}
	mLiveQueueRaw.store(q, std::memory_order_relaxed);

	{
		std::scoped_lock lock(mQueueTableMutex);
		for (uint32_t i = 0; i < mQueueTableCount; ++i)
			if (mQueueTable[i].queue == q) mQueueTable[i].verified = true;
	}

	PLOG_INFO << "VERIFIED the presenting command queue: 0x" << std::hex << (uintptr_t)q << std::dec
		<< " - chosen by " << mProofHow << ", retired our fence within " << mProofPresentsWaited
		<< " of the game's presents. The overlay will submit on this queue and no other.";

	setRenderGate(RenderGate::Live, "queue verified");
}

// Keeps proving the live queue. One that was fine at attach can stop draining later: the user toggles
// frame generation, OBS starts or stops Game Capture, RTSS recreates its OSD renderer, the game does a
// device-lost recovery. Cheap, non-blocking, roughly once every 10 s.
void D3D12Hook::pollLiveQueueHealth()
{
	std::scoped_lock resourceLock(mResourceMutex);

	if (mProofIsRevalidation && mProofQueue && mProofFence)
	{
		if (mProofFence->GetCompletedValue() >= mProofValue)
		{
			mProofQueue = nullptr;
			mProofIsRevalidation = false;
			return;
		}
		if (++mProofPresentsWaited < kQueueProofPresents) return;

		PLOG_ERROR << "The verified presenting queue stopped retiring fences. Standing the overlay down "
			"rather than piling more work onto a queue that is not draining - that is exactly how the "
			"GPU ends up wedged.";
		rejectQueue(mCommandQueue, "stopped retiring while live");
		mProofQueue = nullptr;
		mProofIsRevalidation = false;
		mLiveQueueRaw.store(nullptr, std::memory_order_relaxed);
		mQueueSearchActive.store(true, std::memory_order_relaxed);   // re-arm discovery
		setRenderGate(RenderGate::Degraded, "the live queue stopped draining");
		return;
	}

	if (mCommandQueue && (mPresentIntervalsObserved.load(std::memory_order_relaxed) % kQueueRecheckPresents) == 0)
		beginProof(mCommandQueue, "periodic re-verification", true);
}

// static. Called ONCE per present on the adopted swapchain, before anything is recorded. Returns true
// only when it is safe to draw. It NEVER blocks: the state machine is driven entirely by the game's own
// presents, the only clock guaranteed to be running and the only one that stays correct under an RTSS
// framerate cap or a DLSS-G pacer.
bool D3D12Hook::updateRenderGate()
{
	D3D12Hook* d3d = instance;
	if (!d3d) return false;

	const uint32_t interval = mPresentIntervalsObserved.fetch_add(1, std::memory_order_relaxed) + 1;

	// Close the observation interval.
	{
		std::scoped_lock lock(mQueueTableMutex);
		for (uint32_t i = 0; i < mQueueTableCount; ++i)
		{
			if (mQueueTable[i].submissionsThisInterval > 0) ++mQueueTable[i].intervalsSeenIn;
			mQueueTable[i].submissionsThisInterval = 0;
		}
	}

	switch (mRenderGate.load(std::memory_order_relaxed))
	{
	case RenderGate::Discovering:
		// ⚠ SETTLE FIRST. The old code sampled its single global slot in the 1 ms window right after our
		// hooks went in - while the dummy device/window/swapchain used to harvest addresses were still
		// being torn down, and while every other overlay in the process was reacting to a new module
		// load. That window IS a large part of the defect. Watch eight of the game's own presents first.
		if (interval < kQueueObserveIntervals) return false;
		d3d->beginQueueVerification();
		return false;

	case RenderGate::Verifying:
	case RenderGate::Degraded:
		return d3d->pollQueueVerification();

	case RenderGate::Live:
		d3d->pollLiveQueueHealth();
		return mRenderGate.load(std::memory_order_relaxed) == RenderGate::Live;

	case RenderGate::Refused:
	default:
		return false;
	}
}

void D3D12Hook::releaseQueueTable()
{
	std::scoped_lock lock(mQueueTableMutex);
	for (uint32_t i = 0; i < mQueueTableCount; ++i)
		safe_release(mQueueTable[i].queue);
	mQueueTableCount = 0;
}

#pragma endregion adoption


#pragma region resource management

bool D3D12Hook::anyBackBufferHeld() const
{
	for (const auto& backBuffer : mBackBuffers)
	{
		if (backBuffer.resource)
			return true;
	}
	return false;
}

// Creates (or recreates) everything that depends on the swapchain's back buffers.
// Safe to call repeatedly - used both for first init and after ResizeBuffers.
// Caller MUST have flushed the GPU and released the old back buffer references first.
void D3D12Hook::createBackBufferResources()
{
	std::scoped_lock resourceLock(mResourceMutex);

	if (!mSwapChain3 || !mDevice)
		throw HCMInitException("createBackBufferResources called without a swapchain/device");

	DXGI_SWAP_CHAIN_DESC swapDesc{};
	if (FAILED(mSwapChain3->GetDesc(&swapDesc)))
		throw HCMInitException("Failed to get the swapchain description");

	const UINT newBufferCount = swapDesc.BufferCount;
	if (newBufferCount == 0 || newBufferCount > 16)
		throw HCMInitException(std::format("Implausible swapchain buffer count: {}", newBufferCount));

	++mResourceGeneration;

	// The number of FRAMES IN FLIGHT is whatever we told ImGui_ImplDX12_Init, and imgui's
	// FrameContext array is fixed at that size for the life of its backend. The swapchain's buffer
	// count, on the other hand, is free to change on a resize (UE5 does exactly that when toggling
	// fullscreen / frame pacing). Freezing our render ring at the value imgui knows about is what
	// keeps the two rings in step; growing it would make imgui recycle a frame too early.
	if (mFramesInFlight == 0)
	{
		mFramesInFlight = newBufferCount;
	}
	else if (newBufferCount != mFramesInFlight)
	{
		PLOG_ERROR << "Swapchain buffer count changed (" << mFramesInFlight << " -> " << newBufferCount
			<< ") but imgui's dx12 backend was initialised with NumFramesInFlight = " << mFramesInFlight
			<< " and cannot be re-negotiated from here. Keeping the original frames-in-flight count.";
	}

	// If the buffer count changed we have to rebuild the back buffers and the RTV heap.
	// The command allocators live in mRenderSlots and are deliberately NOT touched here - they are
	// sized by mFramesInFlight, and mCommandList holds an internal reference to mRenderSlots[0]'s.
	if (newBufferCount != mBufferCount)
	{
		for (auto& backBuffer : mBackBuffers)
			safe_release(backBuffer.resource);
		mBackBuffers.clear();
		safe_release(mRtvHeap);
		mBufferCount = newBufferCount;
	}

	mBackBuffers.resize(mBufferCount);
	mRenderSlots.resize(mFramesInFlight);

	if (!mRtvHeap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.NumDescriptors = mBufferCount;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap))) || !mRtvHeap)
			throw HCMInitException("Failed to create the RTV descriptor heap");
	}

	mRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < mBufferCount; ++i)
	{
		BackBuffer& backBuffer = mBackBuffers[i];

		safe_release(backBuffer.resource);
		if (FAILED(mSwapChain3->GetBuffer(i, IID_PPV_ARGS(&backBuffer.resource))) || !backBuffer.resource)
			throw HCMInitException(std::format("Failed to get back buffer {}", i));

		backBuffer.rtv = rtvHandle;

		const D3D12_RESOURCE_DESC resourceDesc = backBuffer.resource->GetDesc();

		// Build the RTV from the RESOURCE's format, and report that same format to imgui via
		// getImGuiInitInfo(). If those two ever disagree, imgui's PSO won't match the render target
		// and nothing draws (silently, unless the d3d12 debug layer is on).
		D3D12_RENDER_TARGET_VIEW_DESC rtvViewDesc{};
		rtvViewDesc.Format = resourceDesc.Format;
		rtvViewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rtvViewDesc.Texture2D.MipSlice = 0;
		rtvViewDesc.Texture2D.PlaneSlice = 0;
		mDevice->CreateRenderTargetView(backBuffer.resource, &rtvViewDesc, backBuffer.rtv);

		if (i == 0)
		{
			if (mRtvFormat != DXGI_FORMAT_UNKNOWN && mRtvFormat != resourceDesc.Format)
			{
				// imgui's pipeline state was built for the old format at ImGui_ImplDX12_Init time.
				// We cannot rebuild it from here - that needs ImGuiManager to re-init its backend.
				PLOG_ERROR << "Swapchain back buffer format changed (" << (int)mRtvFormat << " -> "
					<< (int)resourceDesc.Format << "). The imgui dx12 pipeline state no longer matches "
					<< "and the overlay may not draw until HCM is restarted.";
			}
			mRtvFormat = resourceDesc.Format;

			// Screen size comes from the BACK BUFFER, not from the window client rect that
			// ImGui_ImplWin32_NewFrame would give us - matching D3D11Hook, and correct under DPI
			// scaling / dynamic resolution.
			mScreenSize = { static_cast<float>(resourceDesc.Width), static_cast<float>(resourceDesc.Height) };
			mScreenCenter = mScreenSize / 2;
			PLOG_INFO << "Initializing screen size: " << resourceDesc.Width << ", " << resourceDesc.Height;
		}

		rtvHandle.ptr += mRtvDescriptorSize;
	}

	// One allocator PER FRAME IN FLIGHT. This is what makes the single-fence recycle correct: we
	// only ever reset an allocator once the GPU has finished the submission that used it.
	for (UINT i = 0; i < mFramesInFlight; ++i)
	{
		RenderSlot& slot = mRenderSlots[i];
		if (!slot.allocator)
		{
			if (FAILED(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) || !slot.allocator)
				throw HCMInitException(std::format("Failed to create command allocator {}", i));
			slot.fenceValue = 0;
		}
	}

	// ONE command list shared by every frame; the per-frame state lives in the allocators.
	// Created HERE and not only in initializeD3Ddevice, because newDX12ResizeBuffers calls
	// createBackBufferResources() directly - and releaseSwapChainResources() drops the list along
	// with the allocators it references.
	// D3D12 command lists are created in the recording state, so close it immediately - the Present
	// path always Resets it before recording.
	if (!mCommandList)
	{
		if (FAILED(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mRenderSlots[0].allocator, nullptr, IID_PPV_ARGS(&mCommandList))) || !mCommandList)
			throw HCMInitException("Failed to create the overlay command list");

		if (FAILED(mCommandList->Close()))
			throw HCMInitException("Failed to close the freshly created command list");
	}

	++mResourceGeneration;
}

// Drops ONLY the back buffer references. Mandatory before calling the game's ResizeBuffers:
// IDXGISwapChain::ResizeBuffers fails with DXGI_ERROR_INVALID_CALL while any outstanding back
// buffer reference exists, so an overlay that forgets this breaks every resolution change in the
// game. (The reference overlay does exactly that - it never hooks ResizeBuffers at all.)
void D3D12Hook::releaseBackBufferResources()
{
	std::scoped_lock resourceLock(mResourceMutex);
	++mResourceGeneration;
	for (auto& backBuffer : mBackBuffers)
		safe_release(backBuffer.resource);
}

// Releases everything tied to the current swapchain, keeping device-scoped objects alive.
// NOTE mCommandList goes too: it was created from mRenderSlots[0].allocator and some drivers keep a
// real internal reference to that allocator, so releasing the allocators without it trips
// EXECUTION_ERROR "command allocator ... still referenced by ID3D12GraphicsCommandList".
void D3D12Hook::releaseSwapChainResources(bool gpuIsIdle)
{
	std::scoped_lock resourceLock(mResourceMutex);
	++mResourceGeneration;

	releaseOrAbandon(mCommandList, gpuIsIdle);

	// ALWAYS a real release, never abandoned: these are references on resources the SWAPCHAIN owns,
	// so dropping ours cannot destroy anything the GPU might be reading - and keeping even one of
	// them makes every subsequent IDXGISwapChain::ResizeBuffers fail with DXGI_ERROR_INVALID_CALL,
	// which UE5 turns into UE_LOG(Fatal). Killing the game is never the safer option.
	for (auto& backBuffer : mBackBuffers)
		safe_release(backBuffer.resource);
	mBackBuffers.clear();

	for (auto& slot : mRenderSlots)
	{
		releaseOrAbandon(slot.allocator, gpuIsIdle);
		slot.fenceValue = 0;
	}
	mRenderSlots.clear();
	// Resetting the ordinal is safe only because every caller has just flushed the GPU: it changes
	// our slot index's constant offset from imgui's frameIndex, and the fresh slots all carry
	// fenceValue 0 (i.e. no wait) for the first cycle after a rebuild.
	mRenderOrdinal = 0;

	releaseOrAbandon(mRtvHeap, gpuIsIdle);
	safe_release(mSwapChain3); // a reference on the game's object; releasing ours destroys nothing
	mAdoptedSwapChain = nullptr;
	mBufferCount = 0;
	mRtvDescriptorSize = 0;
	isD3DdeviceInitialized = false;
	// ⚠ The proof belongs to the queue we just let go of. Clearing it means a later re-adoption has to
	// earn the right to draw again rather than inheriting a verdict about a different queue.
	mRenderGate.store(RenderGate::Discovering, std::memory_order_release);
	mLiveQueueRaw.store(nullptr, std::memory_order_relaxed);
	mProofQueue = nullptr;
	mProofIsRevalidation = false;
	safe_release(mProofFence);
	releaseQueueTable();
}

// Everything above PLUS the fence and the command queue.
// It deliberately does NOT touch mSrvHeap or mDevice once imgui's dx12 backend has been bound to
// them: the backend keeps a raw, un-AddRef'd copy of them in its ImGui_ImplDX12_InitInfo and
// dereferences them from ImGui_ImplDX12_Shutdown(). Only ~D3D12Hook - which runs AFTER
// ~ImGuiManager, because App.h declares `d3d` before `imm` - may free them.
void D3D12Hook::releaseD3Dresources(bool gpuIsIdle)
{
	std::scoped_lock resourceLock(mResourceMutex);

	releaseSwapChainResources(gpuIsIdle);

	releaseOrAbandon(mFence, gpuIsIdle); // ours exclusively, and the GPU may still be signalling it

	if (mFenceEvent)
	{
		// A pending SetEventOnCompletion still targets this handle, so only close it once we know
		// the fence has retired past it.
		if (gpuIsIdle)
			CloseHandle(mFenceEvent);
		else
			PLOG_ERROR << "Abandoning (leaking) the overlay fence event because the GPU could not be flushed";
		mFenceEvent = nullptr;
	}

	safe_release(mCommandQueue); // the game's queue; we only ever held a reference to it

	mNextFenceValue = 1;

	// The captured queue is gone, so re-arm the ExecuteCommandLists search - unless the overlay has
	// been permanently disabled, in which case leaving it armed would make the detour do a virtual
	// GetDesc() on every submission on every queue in the process for the rest of the session.
	if (ID3D12CommandQueue* staleCandidate = mCandidateDirectQueue.exchange(nullptr, std::memory_order_acq_rel))
		staleCandidate->Release();
	mQueueSearchActive.store(!overlayPermanentlyDisabled && !shuttingDown.load(std::memory_order_relaxed), std::memory_order_relaxed);

	if (!mHasFiredPresentEvent)
	{
		// imgui never bound anything to us, so there is nothing to keep alive.
		releaseImGuiBoundResources(gpuIsIdle);
	}
	else
	{
		LOG_ONCE(PLOG_INFO << "Keeping the D3D12 device and SRV heap alive: imgui's dx12 backend still holds "
			"raw pointers to them, and only ~D3D12Hook (which runs after ~ImGuiManager) may free them");
	}
}

// The objects imgui's dx12 backend holds raw pointers to. ~D3D12Hook only.
void D3D12Hook::releaseImGuiBoundResources(bool gpuIsIdle)
{
	std::scoped_lock resourceLock(mResourceMutex);

	// Close the gate BEFORE releasing: srvDescriptorAlloc/Free are static and are called with a
	// stale InitInfo copy, so this flag is the only thing standing between them and a freed heap.
	mSrvHeapLive.store(false, std::memory_order_release);
	releaseOrAbandon(mSrvHeap, gpuIsIdle); // ours exclusively - the GPU may still be sampling it
	// Ours exclusively too, and imgui held it RAW (never AddRef'd) until ImGui_ImplDX12_Shutdown - so it has to
	// be released HERE rather than in releaseD3Dresources, for the same reason mSrvHeap is.
	releaseOrAbandon(mFontUploadQueue, gpuIsIdle);
	safe_release(mDevice);                 // the game holds its own references; ours destroys nothing

	mWindowHandle = nullptr;
	mRtvFormat = DXGI_FORMAT_UNKNOWN;
	mFramesInFlight = 0;
}

// THE single entry point for "give up on the overlay, permanently".
//
// Latching overlayPermanentlyDisabled while we still hold back-buffer references is a GAME-killing
// bug, not an overlay bug: every path that checks the flag then passes ResizeBuffers straight
// through to DXGI, which refuses with DXGI_ERROR_INVALID_CALL while any outstanding back buffer
// reference exists - and UE5's FD3D12Viewport::Resize wraps that in VERIFYD3D12RESULT ->
// UE_LOG(Fatal). So: release first, latch second, always through here.
void D3D12Hook::disableOverlayPermanently(const char* reason)
{
	std::scoped_lock resourceLock(mResourceMutex);

	if (overlayPermanentlyDisabled)
		return;

	PLOG_FATAL << "Disabling the D3D12 overlay permanently: " << reason;

	overlayPermanentlyDisabled = true;
	isD3DdeviceInitialized = false;
	// Stop the per-submission work in the ExecuteCommandLists detour for good.
	mQueueSearchActive.store(false, std::memory_order_relaxed);

	const bool gpuIsIdle = waitForGpuIdle();
	releaseD3Dresources(gpuIsIdle);
}

// Signal + wait so no GPU work is still referencing anything we're about to release or reset.
// Skipping this before releasing command allocators is the classic D3D12 unload crash.
// Returns false when the flush could NOT be proven - the caller must then leak rather than free.
bool D3D12Hook::waitForGpuIdle()
{
	std::scoped_lock resourceLock(mResourceMutex);

	// Whatever happens below, the per-slot fence values must not survive.
	// A stale value that will never be reached makes newDX12Present burn the full
	// kFenceWaitTimeoutMs on the game's RENDER THREAD every single frame - i.e. the game runs at
	// 1 FPS - and skip the overlay forever.
	struct ClearFenceValuesOnExit
	{
		std::vector<RenderSlot>& slots;
		~ClearFenceValuesOnExit() { for (auto& slot : slots) slot.fenceValue = 0; }
	} clearOnExit{ mRenderSlots };

	if (!mFence || !mCommandQueue || !mFenceEvent)
		return true; // nothing of ours was ever submitted, so the GPU is trivially idle w.r.t. us

	const UINT64 flushValue = mNextFenceValue++;
	if (FAILED(mCommandQueue->Signal(mFence, flushValue)))
	{
		PLOG_ERROR << "Fence signal failed during GPU flush";
		checkDeviceRemoved(mDevice, "waitForGpuIdle");
		return false;
	}

	if (mFence->GetCompletedValue() < flushValue)
	{
		// The event is auto-reset, and a previously timed-out wait can leave it signalled by a
		// fence value we are no longer interested in. Clear it so this wait means what it says.
		ResetEvent(mFenceEvent);

		if (FAILED(mFence->SetEventOnCompletion(flushValue, mFenceEvent)))
		{
			PLOG_ERROR << "SetEventOnCompletion failed during GPU flush";
			checkDeviceRemoved(mDevice, "waitForGpuIdle");
			return false;
		}

		if (WaitForSingleObject(mFenceEvent, kGpuFlushTimeoutMs) != WAIT_OBJECT_0)
		{
			PLOG_ERROR << "Timed out waiting for the GPU to go idle; overlay resources will be abandoned rather than freed";
			return false;
		}
	}

	return true;
}

// Creates the D3D12 objects the overlay needs. Throws HCMInitException on any failure.
// The caller is responsible for either latching the failure or rolling back - we must never allow
// a partially-initialised renderer to be retried frame after frame (the reference did, and its
// second pass re-installed a WndProc on top of itself, giving infinite recursion).
void D3D12Hook::initializeD3Ddevice(IDXGISwapChain* pSwapChain)
{
	std::scoped_lock resourceLock(mResourceMutex);

	PLOG_DEBUG << "Initializing D3D12 renderer, swapchain: 0x" << std::hex << (uintptr_t)pSwapChain;

	if (!mSwapChain3 || !mDevice || !mCommandQueue)
		throw HCMInitException("initializeD3Ddevice called before the swapchain/device/queue were adopted");

	createBackBufferResources();

	if (mRenderSlots.empty() || !mRenderSlots[0].allocator || !mCommandList)
		throw HCMInitException("No command allocator / command list after createBackBufferResources");

	// Shader-visible SRV heap - imgui's font atlas SRV lives here and is sampled by its pixel
	// shader, so SHADER_VISIBLE is not optional.
	if (!mSrvHeap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.NumDescriptors = SRV_DESCRIPTOR_COUNT;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(mDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap))) || !mSrvHeap)
			throw HCMInitException("Failed to create the shader-visible SRV descriptor heap");

		resetSrvDescriptorAllocator();
	}
	// Opens the gate for the static SRV callbacks (see resetSrvDescriptorAllocator).
	mSrvHeapLive.store(true, std::memory_order_release);

	// A queue of our own, used for exactly one thing: imgui's one-shot font-texture upload. See the long note
	// on mFontUploadQueue in the header - handing imgui the GAME'S queue is what froze the game, because the
	// backend waits on that upload with an INFINITE timeout from inside our Present detour.
	if (!mFontUploadQueue)
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		// MUST be DIRECT: the backend records the copy on a DIRECT allocator and command list, and issues a
		// COPY_DEST -> PIXEL_SHADER_RESOURCE barrier, which a COPY queue is not allowed to execute.
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 1;
		if (FAILED(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mFontUploadQueue))) || !mFontUploadQueue)
			throw HCMInitException("Failed to create the private font-upload command queue");
	}

	if (!mFence)
	{
		if (FAILED(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence))) || !mFence)
			throw HCMInitException("Failed to create the overlay fence");
	}

	if (!mFenceEvent)
	{
		mFenceEvent = CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
		if (!mFenceEvent)
			throw HCMInitException(std::format("CreateEventW for the fence event failed, error: {}", GetLastError()));
	}

	// ⚠ The old blocking 250 ms queue probe lived here and has been REMOVED, not weakened. Two reasons:
	// it blocked a present thread, and wall-clock milliseconds are the wrong unit when RTSS is holding
	// presentation and DLSS-G is pacing. Its job is now done by the render gate in renderOverlayFrame,
	// which proves the same property non-blockingly, clocked on the game's own presents, BEFORE anything
	// is submitted rather than after the renderer is already built.

	PLOG_INFO << "D3D12 renderer initialized: " << mBufferCount << " back buffers, "
		<< mFramesInFlight << " frames in flight, format " << (int)mRtvFormat;
}

#pragma endregion resource management


bool D3D12Hook::getImGuiInitInfo(ImGui_ImplDX12_InitInfo& outInfo) const
{
	if (!isD3DdeviceInitialized || !mDevice || !mCommandQueue || !mFontUploadQueue || !mSrvHeap || mFramesInFlight == 0)
	{
		PLOG_ERROR << "getImGuiInitInfo called before the D3D12 renderer was initialized";
		return false;
	}

	outInfo = ImGui_ImplDX12_InitInfo{}; // its default ctor memsets, so every field is defined
	outInfo.Device = mDevice;
	// ⚠ OURS, NEVER THE GAME'S - see the note on mFontUploadQueue in the header. The backend uses this ONLY for
	// the font-texture upload, which it waits on with an INFINITE timeout from inside our Present detour; giving
	// it the game's queue is what froze the game. The overlay's actual draws still go to mCommandQueue in
	// renderOverlayFrame, which is what keeps them composited on top of the scene.
	outInfo.CommandQueue = mFontUploadQueue;
	// Must match mRenderSlots.size() exactly - see the comment on RenderSlot.
	outInfo.NumFramesInFlight = static_cast<int>(mFramesInFlight);
	outInfo.RTVFormat = mRtvFormat;
	outInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
	outInfo.SrvDescriptorHeap = mSrvHeap;
	outInfo.SrvDescriptorAllocFn = &D3D12Hook::srvDescriptorAlloc;
	outInfo.SrvDescriptorFreeFn = &D3D12Hook::srvDescriptorFree;
	return true;
}


#pragma region detours

// static
// The one shared body behind newDX12Present and newDX12Present1. Records and submits the overlay
// for this frame; every failure path simply returns, leaving the caller to forward the call to the
// game unchanged. It NEVER throws and it NEVER calls an original - the caller owns that.
void D3D12Hook::renderOverlayFrame(IDXGISwapChain* pSwapChain, UINT presentFlags)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::renderOverlayFrame");

	D3D12Hook* d3d = instance;
	if (!d3d)
		return;

	// Set before ~D3D12Hook starts draining, so a detour that got in just ahead of the drain stops
	// touching D3D12 and HCM state immediately.
	if (shuttingDown.load(std::memory_order_acquire))
		return;


	// Once shutdown has begun, stop invoking HCM's render/overlay callbacks. The services they
	// call into are being destroyed on the shutdown thread; firing render events here races with
	// that teardown - it crashed on older builds, and now causes lock-contention that stalls
	// shutdown for tens of seconds. That stall prevents the DLL from unloading, which is why HCM
	// can't re-attach after being closed and reopened.
	// We ALSO drop our back-buffer references exactly once here: the game keeps running (and can be
	// alt-tabbed / resolution-changed) throughout that window, and ResizeBuffers fails while any
	// outstanding back buffer reference exists.
	if (GlobalKill::isKillSet())
	{
		if (!d3d->mReleasedForShutdown)
		{
			d3d->mReleasedForShutdown = true;
			PLOG_INFO << "GlobalKill is set; releasing the D3D12 overlay's swapchain resources and passing every frame through";
			const bool gpuIsIdle = d3d->waitForGpuIdle();
			// NOT releaseD3Dresources: imgui's backend is still bound to mDevice/mSrvHeap and
			// ~ImGuiManager has not run yet.
			d3d->releaseSwapChainResources(gpuIsIdle);
			d3d->mQueueSearchActive.store(false, std::memory_order_relaxed);
		}
		return;
	}

	// Latched after an unrecoverable failure. We keep the hooks installed (removing them from a
	// detour is not safe) but never touch D3D12 again.
	if (d3d->overlayPermanentlyDisabled)
		return;

	// DXGI_PRESENT_TEST is an occlusion poll: nothing is presented and GetCurrentBackBufferIndex()
	// does NOT advance. DXGI_PRESENT_DO_NOT_SEQUENCE likewise does not flip. Rendering on those
	// would advance imgui's frames-in-flight ring (and our own) against a back buffer that never
	// moves, so imgui would overwrite a vertex/index buffer whose previous submission may still be
	// executing. UE5/DXGI issue these routinely while the window is occluded.
	if (presentFlags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE))
		return;

	// These are INLINE hooks on dxgi's shared Present/Present1, so we see every swapchain in the
	// process. Everything below this point only runs for the one swapchain we adopted.
	if (pSwapChain != d3d->mAdoptedSwapChain)
	{
		// Negative cache: without it every foreign swapchain (Steam / Discord / RTSS / EOS / any
		// MediaFoundation surface) pays the full QueryInterface + GetDevice + GetDesc + IsWindow +
		// GetWindowThreadProcessId battery on every one of its frames, forever.
		if (pSwapChain == mRejectedSwapChain.load(std::memory_order_relaxed))
		{
			// ...but decay it: this is a raw pointer compare, and a rejected swapchain that gets
			// destroyed can have its address reused by the game's real one.
			if (mRejectedSwapChainSkips.fetch_add(1, std::memory_order_relaxed) + 1 < kRejectedSwapChainRecheckInterval)
				return;
			mRejectedSwapChainSkips.store(0, std::memory_order_relaxed);
		}

		if (!d3d->tryAdoptSwapChain(pSwapChain))
		{
			mRejectedSwapChain.store(pSwapChain, std::memory_order_relaxed);
			return;
		}

		mRejectedSwapChain.store(nullptr, std::memory_order_relaxed);
		mRejectedSwapChainSkips.store(0, std::memory_order_relaxed);
	}

	// ⚠⚠ THE RENDER GATE. Nothing below this line runs until a DIRECT queue on this swapchain's device
	// has PROVEN it drains, by retiring a fence we signalled on it within eight of the game's own
	// presents. It never blocks, and it is clocked on PRESENTS rather than wall time - which is what
	// makes it correct while RTSS is capping the framerate and DLSS-G is pacing.
	//
	// ⚠ POSITION IS LOAD-BEARING, and getting it wrong is easy:
	//   * it must be AFTER the adopted-swapchain filter above, or foreign swapchains (RTSS's and OBS's
	//     own, Streamline's) would tick the present clock and corrupt the interval counts this ranks on;
	//   * it must be AFTER the GlobalKill block, or an early return here would skip the one-shot
	//     back-buffer release, and ResizeBuffers fails while any back-buffer reference is outstanding;
	//   * it must be BEFORE initializeD3Ddevice, because the whole point is to not build or submit
	//     anything on a queue we have not proven.
	//
	// Measured: submitting on an unverified queue hung the GPU in 3 of 8 sessions. Returning here
	// forwards the frame to the game untouched - the overlay appears a few frames later once a queue is
	// proven, or never, if none can be. Losing the overlay is recoverable; losing the GPU is not.
	if (!updateRenderGate())
		return;

	if (!d3d->isD3DdeviceInitialized)
	{
		PLOG_DEBUG << "Initializing d3d device @ present";
		try
		{
			d3d->initializeD3Ddevice(pSwapChain);
			d3d->isD3DdeviceInitialized = true;
			PLOG_DEBUG << "D3D device initialized t. newDX12Present";
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize d3d device, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";

			// Roll all the way back rather than retrying a half-built renderer next frame.
			d3d->disableOverlayPermanently("the D3D12 renderer failed to initialize");
			GlobalKill::killMe();
			return;
		}

		// fire a resizeborders event. Outside the try above so an exception from a SUBSCRIBER is not
		// mistaken for an init failure, and never allowed to unwind into DXGI.
		try
		{
			d3d->resizeBuffersHookEvent->operator()(mScreenSize);
			PLOG_DEBUG << "resizeBuffersHookEvent fired";
		}
		catch (...)
		{
			PLOG_ERROR << "A resizeBuffersHookEvent subscriber threw; ignoring";
		}
	}

	const UINT backBufferIndex = d3d->mSwapChain3->GetCurrentBackBufferIndex();
	if (backBufferIndex >= d3d->mBackBuffers.size())
	{
		LOG_ONCE(PLOG_ERROR << "Back buffer index out of range; skipping the overlay this frame");
		return;
	}

	if (d3d->mRenderSlots.empty() || !d3d->mCommandList || !d3d->mFence || !d3d->mFenceEvent)
	{
		LOG_ONCE(PLOG_ERROR << "Frame resources missing; skipping the overlay this frame");
		return;
	}

	BackBuffer& backBuffer = d3d->mBackBuffers[backBufferIndex];
	// Our render ring is keyed on OUR ordinal, not on the back buffer index, so that it stays in
	// lockstep with imgui's (which only advances on frames it actually renders). See RenderSlot.
	const size_t slotIndex = static_cast<size_t>(d3d->mRenderOrdinal % d3d->mRenderSlots.size());
	RenderSlot& slot = d3d->mRenderSlots[slotIndex];

	if (!backBuffer.resource || !slot.allocator)
	{
		LOG_ONCE(PLOG_ERROR << "Frame resources missing; skipping the overlay this frame");
		return;
	}

	// Wait until the GPU has finished the last submission that used THIS render slot's allocator -
	// which is also the last submission that touched imgui's vertex/index buffer for this slot.
	// That single wait is the whole synchronisation model: one command list, N allocators, one
	// monotonically increasing fence.
	if (slot.fenceValue != 0 && d3d->mFence->GetCompletedValue() < slot.fenceValue)
	{
		ResetEvent(d3d->mFenceEvent); // auto-reset event, may be stale from a previous timeout

		if (FAILED(d3d->mFence->SetEventOnCompletion(slot.fenceValue, d3d->mFenceEvent)))
		{
			LOG_ONCE(PLOG_ERROR << "SetEventOnCompletion failed; skipping the overlay this frame");
			if (FAILED(checkDeviceRemoved(d3d->mDevice, "SetEventOnCompletion")))
				d3d->disableOverlayPermanently("the D3D12 device was removed");
			return;
		}

		// Bounded, unlike the reference's INFINITE wait: hanging the game's render thread forever
		// on a wedged device is far worse than a dropped overlay frame.
		if (WaitForSingleObject(d3d->mFenceEvent, kFenceWaitTimeoutMs) != WAIT_OBJECT_0)
		{
			LOG_ONCE(PLOG_ERROR << "Timed out waiting on the overlay fence; skipping the overlay this frame");
			if (FAILED(checkDeviceRemoved(d3d->mDevice, "overlay fence wait")))
				d3d->disableOverlayPermanently("the D3D12 device was removed");
			return;
		}
	}

	if (FAILED(slot.allocator->Reset()))
	{
		LOG_ONCE(PLOG_ERROR << "Command allocator reset failed; skipping the overlay this frame");
		// A removed device fails this identically forever, so latch instead of retrying at 1 FPS.
		if (FAILED(checkDeviceRemoved(d3d->mDevice, "allocator->Reset")))
			d3d->disableOverlayPermanently("the D3D12 device was removed");
		return;
	}

	if (FAILED(d3d->mCommandList->Reset(slot.allocator, nullptr)))
	{
		LOG_ONCE(PLOG_ERROR << "Command list reset failed; skipping the overlay this frame");
		if (FAILED(checkDeviceRemoved(d3d->mDevice, "commandList->Reset")))
			d3d->disableOverlayPermanently("the D3D12 device was removed");
		return;
	}

	// Snapshot everything we will still need after the render event, and remember the resource
	// generation. ImGuiManager's present handler runs a PeekMessage/DispatchMessage pump, so the
	// game's WndProc - and therefore ResizeBuffers - can run re-entrantly from inside the event.
	// If that happens our back buffers are released and rebuilt underneath us, and submitting this
	// command list would have the GPU read freed memory.
	ID3D12Resource* const backBufferResource = backBuffer.resource;
	const D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = backBuffer.rtv;
	const uint64_t generationBefore = d3d->mResourceGeneration;

	D3D12_RESOURCE_BARRIER toRenderTarget{};
	toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRenderTarget.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	toRenderTarget.Transition.pResource = backBufferResource;
	toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	d3d->mCommandList->ResourceBarrier(1, &toRenderTarget);

	// No viewport / scissor / clear needed: imgui's dx12 backend sets its own in RenderDrawData.
	// This is the D3D12 equivalent of D3D11's pDeviceContext->OMSetRenderTargets in
	// ImGuiManager::onPresentHookEvent - done here, by the hook, because only the hook knows which
	// back buffer this frame is.
	d3d->mCommandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);

	// Must be bound on the list BEFORE any draw references a GPU handle into it.
	ID3D12DescriptorHeap* descriptorHeaps[] = { d3d->mSrvHeap };
	d3d->mCommandList->SetDescriptorHeaps(1, descriptorHeaps);

	LOG_ONCE(PLOG_VERBOSE << "invoking presentHookEvent callback");
	// Set BEFORE the call: from here on we must assume imgui's dx12 backend may be bound to our
	// device and SRV heap, so neither may be released outside ~D3D12Hook.
	d3d->mHasFiredPresentEvent = true;
	try
	{
		// Subscriber (ImGuiManager's dx12 branch) does:
		//   ImGui_ImplDX12_NewFrame / ImGui_ImplWin32_NewFrame / ImGui::NewFrame
		//   ... the four HCM render events ...
		//   ImGui::EndFrame / ImGui::Render
		//   ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCommandList)
		d3d->presentHookEvent->operator()(d3d->mDevice, d3d->mCommandList, d3d->mSwapChain3, backBufferRtv);
	}
	catch (...)
	{
		// HCM cheats throw HCMRuntimeException freely and ImGuiManager does not guard the render
		// event chain. Letting that unwind through DXGI/UE5 frames is not survivable, and leaving
		// the command list in the recording state would make every subsequent Reset() fail with
		// E_FAIL - i.e. the overlay would never come back.
		PLOG_FATAL << "A presentHookEvent subscriber threw; abandoning the overlay frame and shutting HCM down";
		if (d3d->mCommandList)
			d3d->mCommandList->Close();
		GlobalKill::killMe();
		return;
	}

	if (d3d->mResourceGeneration != generationBefore)
	{
		// A ResizeBuffers ran re-entrantly from inside the render event. Our back buffer reference
		// is stale; close the list and drop the frame rather than submitting it.
		PLOG_ERROR << "Overlay resources were rebuilt from inside the present event; abandoning this frame";
		if (d3d->mCommandList)
			d3d->mCommandList->Close();
		return;
	}

	D3D12_RESOURCE_BARRIER toPresent{};
	toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	toPresent.Transition.pResource = backBufferResource;
	toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	d3d->mCommandList->ResourceBarrier(1, &toPresent);

	if (FAILED(d3d->mCommandList->Close()))
	{
		// Nothing was submitted, so the unbalanced barrier never reaches the GPU. Next frame's
		// Reset recovers the list.
		LOG_ONCE(PLOG_ERROR << "Command list close failed; skipping the overlay this frame");
		if (FAILED(checkDeviceRemoved(d3d->mDevice, "commandList->Close")))
			d3d->disableOverlayPermanently("the D3D12 device was removed");
		return;
	}

	// NOTE: this re-enters newDX12ExecuteCommandLists. That is harmless and cheap - by this point
	// mQueueSearchActive is false, so the detour is one relaxed load and a tail call.
	ID3D12CommandList* commandLists[] = { d3d->mCommandList };
	d3d->mCommandQueue->ExecuteCommandLists(1, commandLists);

	const UINT64 signalValue = d3d->mNextFenceValue++;
	if (FAILED(d3d->mCommandQueue->Signal(d3d->mFence, signalValue)))
	{
		// The work IS submitted but nothing tracks it, so we can never safely reset this
		// allocator again. Signal only fails on device removal, so disable rather than risk
		// resetting an allocator the GPU is still reading. disableOverlayPermanently is what makes
		// sure our back buffer references are dropped along with the flag.
		PLOG_FATAL << "Fence signal failed; disabling the D3D12 overlay";
		checkDeviceRemoved(d3d->mDevice, "commandQueue->Signal");
		d3d->disableOverlayPermanently("the overlay fence could not be signalled");
		return;
	}

	slot.fenceValue = signalValue;
	// ONLY on frames we actually submitted, so this stays in lockstep with imgui's frameIndex.
	++d3d->mRenderOrdinal;
}


// static
HRESULT __stdcall D3D12Hook::newDX12Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::newDX12Present");

	// Proof of life for the re-hook watchdog. Counted before anything can return early, because the question it
	// answers is "did a present reach our code AT ALL", not "did we render".
	mPresentDetourFires.fetch_add(1, std::memory_order_relaxed);

	// Must be the first statement. It is a real reader-writer lock (not the old wait-then-set
	// atomic), which is what lets ~D3D12Hook prove nobody is inside a detour body before safetyhook
	// frees the trampolines - and it doubles as the double-render guard for Present/Present1.
	DetourEntryGuard entry(swapChainHookGuard);

	if (!entry.isOutermost())
	{
		// dxgi forwarded Present -> Present1 (or the game did). The outer entry already rendered
		// and submitted this frame's overlay; rendering again would submit it twice.
		LOG_ONCE(PLOG_DEBUG << "Nested Present detour entry; the outer frame owns the overlay");
	}
	else if (obsBypassOwnsFrame(false) || obsPreCaptureOwnsFrame(false))
	{
		// The OBS bypass thunk on RealPresent is live, so IT renders this frame - later in the chain,
		// after OBS has taken its clean capture. Rendering here as well would submit the overlay twice
		// and (in the install order where OBS's hook is outermost) put it back into the recording.
		LOG_ONCE(PLOG_DEBUG << "OBS bypass owns the Present frame; skipping the overlay in newDX12Present");
	}
	else
	{
		renderOverlayFrame(pSwapChain, Flags);
	}

	return callOriginalPresent(pSwapChain, SyncInterval, Flags);
}


// static
// UE5's FD3D12Viewport::PresentInternal calls IDXGISwapChain1::Present1 whenever it holds an
// IDXGISwapChain1, which on Win10+ is always. This is therefore the detour that actually fires on
// HaloCampaignEvolved.exe; the slot-8 hook above is kept for anything that presents the old way.
HRESULT __stdcall D3D12Hook::newDX12Present1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::newDX12Present1");

	DetourEntryGuard entry(swapChainHookGuard);

	if (!entry.isOutermost())
	{
		LOG_ONCE(PLOG_DEBUG << "Nested Present1 detour entry; the outer frame owns the overlay");
	}
	else if (obsBypassOwnsFrame(true) || obsPreCaptureOwnsFrame(true))
	{
		LOG_ONCE(PLOG_DEBUG << "OBS bypass owns the Present1 frame; skipping the overlay in newDX12Present1");
	}
	else
	{
		// Upcast is a no-op in this single-inheritance COM chain, and mAdoptedSwapChain is only
		// ever compared, never dereferenced through this type.
		renderOverlayFrame(static_cast<IDXGISwapChain*>(pSwapChain), PresentFlags);
	}

	return callOriginalPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
}


#pragma region OBS bypass thunks

// Depth of our own OBS-bypass thunks on this thread. Same double-render problem as tlDetourDepth, one
// level further down the chain: OBS hooks BOTH Present and Present1, and dxgi is free to forward one
// to the other, so both trampolines can be entered for a single frame.
static thread_local unsigned int tlOBSBypassDepth = 0;

namespace
{
	class OBSBypassDepthGuard
	{
		bool mOutermost;
	public:
		OBSBypassDepthGuard() : mOutermost(tlOBSBypassDepth == 0) { ++tlOBSBypassDepth; }
		~OBSBypassDepthGuard() { --tlOBSBypassDepth; }
		OBSBypassDepthGuard(const OBSBypassDepthGuard&) = delete;
		OBSBypassDepthGuard& operator=(const OBSBypassDepthGuard&) = delete;
		bool isOutermost() const { return mOutermost; }
	};
}


// static
// ⚠ CALLERS MUST HOLD swapChainHookGuard (every detour does, via DetourEntryGuard). That is what makes
// reading these shared_ptrs safe against teardownOBSBypass, which takes the same lock exclusively.
bool D3D12Hook::obsBypassOwnsFrame(bool forPresent1)
{
	D3D12Hook* d3d = instance;
	if (!d3d) return false;

	const std::shared_ptr<ModuleInlineHook>& hook = forPresent1 ? d3d->mOBSPresent1Hook : d3d->mOBSPresentHook;
	return hook && hook->isHookInstalled();
}


// static
// Installed over the VALUE of OBS's RealPresent - i.e. Detours' trampoline back into dxgi, which
// OBS's hook_present calls immediately AFTER data.capture(). Rendering here is therefore rendering
// into a frame OBS has already recorded without us.
HRESULT __stdcall D3D12Hook::newDX12PresentOBSBypass(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::newDX12PresentOBSBypass");

	OBSBypassDepthGuard bypassDepth;
	// If our own dxgi Present detour is already on this thread's stack, this does not re-lock - it is
	// covered by that frame's shared lock, which is held across its call to the original. Either way
	// the lock is held for the whole of this function, which is what teardownOBSBypass drains against.
	DetourEntryGuard entry(swapChainHookGuard);

	if (bypassDepth.isOutermost())
		renderOverlayFrame(pSwapChain, Flags);

	D3D12Hook* d3d = instance;
	if (d3d && d3d->mOBSPresentHook)
	{
		if (auto* original = d3d->mOBSPresentHook->getInlineHook().original<DX12Present*>())
			return original(pSwapChain, SyncInterval, Flags);
	}

	// NOT unreachable, as this comment used to claim. A thread can already be inside this function,
	// blocked on the shared lock above, while teardownOBSBypass holds it exclusively; by the time it
	// proceeds the hook object is gone. That is an ordinary teardown race - toggling the bypass off,
	// re-arming, or closing HCM - not a bug, and dropping the frame here was the wrong answer: it
	// returned S_OK for a frame that was never presented, so the swapchain never flipped.
	//
	// The trampoline published at install time still points at parked, still-mapped code, so forward
	// through it. Calling pSwapChain->Present() instead would re-enter OBS's hook and recurse forever.
	if (auto* published = gOBSBypassOriginalPresent.load(std::memory_order_acquire))
		return published(pSwapChain, SyncInterval, Flags);

	LOG_ONCE(PLOG_ERROR << "OBS bypass thunk (Present) could not reach OBS's original function and no "
		"trampoline was published; dropping the frame");
	return S_OK;
}


// static
// UE5's D3D12 RHI presents through Present1, so on HaloCampaignEvolved this is the one that matters.
// Separate function rather than a shared body because the signature differs.
HRESULT __stdcall D3D12Hook::newDX12Present1OBSBypass(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::newDX12Present1OBSBypass");

	OBSBypassDepthGuard bypassDepth;
	DetourEntryGuard entry(swapChainHookGuard);

	if (bypassDepth.isOutermost())
		renderOverlayFrame(static_cast<IDXGISwapChain*>(pSwapChain), PresentFlags);

	D3D12Hook* d3d = instance;
	if (d3d && d3d->mOBSPresent1Hook)
	{
		if (auto* original = d3d->mOBSPresent1Hook->getInlineHook().original<DX12Present1*>())
			return original(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
	}

	// Same teardown race as the Present thunk - and this is the one HaloCER actually presents through,
	// so it is the likelier of the two to be hit. Forward through the published trampoline rather than
	// dropping a frame that we then report as S_OK.
	if (auto* published = gOBSBypassOriginalPresent1.load(std::memory_order_acquire))
		return published(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);

	LOG_ONCE(PLOG_ERROR << "OBS bypass thunk (Present1) could not reach OBS's original function and no "
		"trampoline was published; dropping the frame");
	return S_OK;
}

// static
// Installed over the ENTRY of OBS's hook_present - i.e. in front of data.capture(). Rendering here puts the
// overlay into the frame OBS is about to record. The exact mirror of newDX12PresentOBSBypass, which renders
// on the far side of that same capture.
HRESULT __stdcall D3D12Hook::newDX12PresentOBSPreCapture(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::newDX12PresentOBSPreCapture");

	OBSBypassDepthGuard bypassDepth;
	DetourEntryGuard entry(swapChainHookGuard);

	if (bypassDepth.isOutermost())
		renderOverlayFrame(pSwapChain, Flags);

	D3D12Hook* d3d = instance;
	if (d3d && d3d->mOBSPreCapturePresentHook)
	{
		if (auto* original = d3d->mOBSPreCapturePresentHook->getInlineHook().original<DX12Present*>())
			return original(pSwapChain, SyncInterval, Flags);
	}

	// Same teardown race the bypass thunks document: a thread can be inside here, blocked on the shared
	// lock, while teardown holds it exclusively and destroys the hook. Forward through the parked
	// trampoline rather than dropping a frame we would then report as presented.
	if (auto* published = gOBSPreCaptureOriginalPresent.load(std::memory_order_acquire))
		return published(pSwapChain, SyncInterval, Flags);

	LOG_ONCE(PLOG_ERROR << "OBS pre-capture thunk (Present) could not reach OBS's hook_present and no "
		"trampoline was published; dropping the frame");
	return S_OK;
}


// static
HRESULT __stdcall D3D12Hook::newDX12Present1OBSPreCapture(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	LOG_ONCE(PLOG_DEBUG << "D3D12Hook::newDX12Present1OBSPreCapture");

	OBSBypassDepthGuard bypassDepth;
	DetourEntryGuard entry(swapChainHookGuard);

	if (bypassDepth.isOutermost())
		renderOverlayFrame(static_cast<IDXGISwapChain*>(pSwapChain), PresentFlags);

	D3D12Hook* d3d = instance;
	if (d3d && d3d->mOBSPreCapturePresent1Hook)
	{
		if (auto* original = d3d->mOBSPreCapturePresent1Hook->getInlineHook().original<DX12Present1*>())
			return original(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
	}

	if (auto* published = gOBSPreCaptureOriginalPresent1.load(std::memory_order_acquire))
		return published(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);

	LOG_ONCE(PLOG_ERROR << "OBS pre-capture thunk (Present1) could not reach OBS's hook_present and no "
		"trampoline was published; dropping the frame");
	return S_OK;
}

#pragma endregion OBS bypass thunks


// static
// Shared front half of both resize detours.
// Returns true if the caller should rebuild our resources once the game's resize has returned.
//
// Whatever else happens, this MUST leave us holding no back-buffer reference: DXGI refuses
// ResizeBuffers with DXGI_ERROR_INVALID_CALL while one is outstanding, and UE5 turns that into
// UE_LOG(Fatal). That is why the "we're shutting down / disabled / not initialised" cases release
// first and only then bail out - the first draft returned early with the references still held.
bool D3D12Hook::prepareForResize(IDXGISwapChain* pSwapChain)
{
	D3D12Hook* d3d = instance;
	if (!d3d)
		return false;

	// Checked BEFORE any member access: ~D3D12Hook nulls `instance` and only then starts releasing,
	// so a detour that was blocked on swapChainHookGuard could otherwise touch the object as it is
	// being destroyed. This costs us nothing - renderOverlayFrame's GlobalKill branch has already
	// dropped every back buffer reference long before shutdown reaches the destructor.
	if (shuttingDown.load(std::memory_order_acquire))
		return false;

	if (pSwapChain != d3d->mAdoptedSwapChain)
		return false;

	if (d3d->anyBackBufferHeld())
	{
		//  1. flush the GPU first - those back buffers (and the allocators recording into them) may
		//     still be in flight;
		//  2. then drop our back buffer references.
		const bool gpuIsIdle = d3d->waitForGpuIdle();
		if (!gpuIsIdle)
		{
			// We could not prove the GPU is done with them, but we cannot keep them either without
			// breaking the game's resize. Release anyway and give up on the overlay: this only
			// happens on a wedged/removed device, where the overlay is finished regardless.
			PLOG_ERROR << "Could not flush the GPU before the game's ResizeBuffers";
			d3d->releaseBackBufferResources();
			d3d->disableOverlayPermanently("the GPU could not be flushed before a swapchain resize");
			return false;
		}
		d3d->releaseBackBufferResources();
	}

	if (GlobalKill::isKillSet() || d3d->overlayPermanentlyDisabled)
	{
		// One way: we have dropped the back buffers, and we will not be rebuilding them.
		d3d->isD3DdeviceInitialized = false;
		return false;
	}

	if (!d3d->isD3DdeviceInitialized)
		return false;

	return true;
}


// static
void D3D12Hook::finishResize(HRESULT originalResult)
{
	D3D12Hook* d3d = instance;
	if (!d3d)
		return;

	if (FAILED(originalResult))
		PLOG_ERROR << "The game's ResizeBuffers failed with 0x" << std::hex << (ULONG)originalResult << "; rebuilding overlay resources anyway";

	try
	{
		// Rebuilt from the swapchain description rather than the arguments: BufferCount == 0 means
		// "keep the current count" and Width/Height == 0 mean "use the window's client area", so
		// the arguments are not authoritative. D3D11Hook takes mScreenSize from the arguments and
		// gets this subtly wrong.
		d3d->createBackBufferResources();
	}
	catch (HCMInitException& ex)
	{
		// Throwing across the game's render thread is the exact failure mode we must avoid, so
		// swallow it and shut HCM down the orderly way instead. disableOverlayPermanently is what
		// guarantees we are not still holding back buffers when the flag goes up.
		PLOG_FATAL << "Failed to rebuild overlay resources after ResizeBuffers: " << ex.what();
		d3d->disableOverlayPermanently("overlay resources could not be rebuilt after a swapchain resize");
		GlobalKill::killMe();
		return;
	}

	// fire screen resize event
	try
	{
		d3d->resizeBuffersHookEvent->operator()(mScreenSize);
	}
	catch (...)
	{
		PLOG_ERROR << "A resizeBuffersHookEvent subscriber threw; ignoring";
	}
}


// static
HRESULT __stdcall D3D12Hook::newDX12ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	// NOTE, unlike Present, the work still runs when this is a NESTED entry (the game's WndProc can
	// be reached from the PeekMessage pump ImGuiManager runs inside our present event). Skipping it
	// would leave our back-buffer references outstanding across the game's resize, which fails it.
	// The outer present frame notices via mResourceGeneration and abandons its command list.
	DetourEntryGuard entry(swapChainHookGuard);

	if (!shuttingDown.load(std::memory_order_acquire) && instance && pSwapChain == instance->mAdoptedSwapChain)
		PLOG_INFO << "newDX12ResizeBuffers: " << Width << "x" << Height << ", buffers: " << BufferCount << ", format: " << (int)NewFormat;

	const bool rebuild = prepareForResize(pSwapChain);

	const HRESULT hr = callOriginalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

	if (rebuild)
		finishResize(hr);

	return hr;
}


// static
// UE5's FD3D12Viewport::Resize uses IDXGISwapChain3::ResizeBuffers1 under an explicit multi-GPU
// node mask. Same contract as newDX12ResizeBuffers - we only care that our back-buffer references
// are gone before the call and rebuilt after it.
HRESULT __stdcall D3D12Hook::newDX12ResizeBuffers1(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
	DetourEntryGuard entry(swapChainHookGuard);

	IDXGISwapChain* const asSwapChain = static_cast<IDXGISwapChain*>(pSwapChain);

	if (!shuttingDown.load(std::memory_order_acquire) && instance && asSwapChain == instance->mAdoptedSwapChain)
		PLOG_INFO << "newDX12ResizeBuffers1: " << Width << "x" << Height << ", buffers: " << BufferCount << ", format: " << (int)Format;

	const bool rebuild = prepareForResize(asSwapChain);

	const HRESULT hr = callOriginalResizeBuffers1(pSwapChain, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);

	if (rebuild)
		finishResize(hr);

	return hr;
}


// static
// Runs on whatever thread the game submits from, for EVERY command queue in the process.
// Deliberately does NOT take swapChainHookGuard (that would serialise every submission in the
// process against our render work). Instead it takes executeCommandListsGuard shared, which is what
// lets ~D3D12Hook guarantee no thread is inside this function when safetyhook frees the trampoline.
void __stdcall D3D12Hook::newDX12ExecuteCommandLists(ID3D12CommandQueue* pCommandQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists)
{
	std::shared_lock<std::shared_mutex> guard(executeCommandListsGuard);

	if (pCommandQueue != nullptr
		&& !shuttingDown.load(std::memory_order_relaxed)
		&& !GlobalKill::isKillSet())
	{
		if (mQueueSearchActive.load(std::memory_order_relaxed))
		{
			// FULL PATH - only while discovering or re-verifying. Every DIRECT queue that submits gets
			// its own row in the candidate table, with a count of how many PRESENT INTERVALS it appeared
			// in. That interval count is what separates UE5's 3D queue (submits every frame) from an OSD
			// renderer, an 11on12 flush or an encoder running on its own cadence.
			// ⚠ The old code kept only the LAST submitter in one global slot and the first present took
			// whatever was there - a 1 ms race between four to six indistinguishable queues.
			// COMPUTE and COPY queues cannot execute the graphics list we build, so they are filtered.
			const D3D12_COMMAND_QUEUE_DESC queueDesc = pCommandQueue->GetDesc();
			if (queueDesc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
				recordQueueSubmission(pCommandQueue);
		}
		else if (pCommandQueue == mLiveQueueRaw.load(std::memory_order_relaxed))
		{
			// HOT PATH once Live: one relaxed load and a pointer compare, no virtual GetDesc(). Keeps
			// the health check fed without taxing every submission in the process for the whole session.
			std::scoped_lock lock(mQueueTableMutex);
			for (uint32_t i = 0; i < mQueueTableCount; ++i)
				if (mQueueTable[i].queue == pCommandQueue)
				{
					++mQueueTable[i].totalSubmissions;
					++mQueueTable[i].submissionsThisInterval;
					break;
				}
		}
	}

	callOriginalExecuteCommandLists(pCommandQueue, NumCommandLists, ppCommandLists);
}

#pragma endregion detours


// ====================================================================================================================
// RE-HOOK WATCHDOG. See the declaration block in D3D12Hook.h for the measurement that motivated it.
// ====================================================================================================================

// Tears the four swapchain hooks down and installs them again from a fresh harvest. Present/Present1 are what the
// overlay actually needs; ExecuteCommandLists is left alone because it is only used to discover the command queue
// and re-hooking it would drop a candidate we may already hold.
bool D3D12Hook::reinstallSwapChainHooks()
{
	// ⚠⚠ HARVEST BEFORE TAKING THE LOCK. THE FIRST VERSION DID IT THE OTHER WAY ROUND AND FROZE THE GAME.
	//
	// harvestHookAddresses() registers a window class, creates a dummy window, a D3D12 device AND a swapchain,
	// just to read vtable entries. That is slow - tens of milliseconds at best - and it calls into DXGI, which is
	// entitled to call the very functions we have hooked.
	//
	// Holding swapChainHookGuard EXCLUSIVELY across all of that is fatal twice over: every Present on the render
	// thread blocks on the shared lock for the whole duration, so the game visibly hangs; and if DXGI re-enters
	// one of our detours on THIS thread, DetourEntryGuard asks for the shared lock we already hold exclusively -
	// std::shared_mutex is not recursive, so that is an instant, permanent deadlock.
	//
	// Measured as "opening HCM the first time froze my game". The harvest touches nothing of ours, so it belongs
	// outside the lock entirely; the lock only needs to cover the actual swap.
	HookAddresses addresses{};
	try { addresses = harvestHookAddresses(); }
	catch (...) { PLOG_ERROR << "re-hook: could not re-harvest the swapchain addresses"; return false; }

	// The OBS bypass thunks render the frame on behalf of the detours we are about to replace, and
	// teardownOBSBypass takes the guard below exclusively - so they come off first, outside the lock.
	// The user can re-enable the toggle afterwards; silently keeping a bypass alive across a re-hook
	// would leave two renderers disagreeing about who owns the frame.
	teardownOBSBypass("the swapchain hooks are being reinstalled");
	teardownOBSPreCapture("the swapchain hooks are being reinstalled");

	std::unique_lock<std::shared_mutex> guard(swapChainHookGuard);

	// Down first, honouring the same "never erase a third party's hook" rule as teardown. A site that is no
	// longer ours must be left alone here too - re-hooking on top of somebody else's patch is how hook wars
	// escalate, and it would not help anyway.
	auto downIfOurs = [&](int index, const char* name, safetyhook::InlineHook& hook)
	{
		if (!hook) return true;
		if (!hookSiteIsStillOurs(index))
		{
			PLOG_WARNING << "re-hook: " << name << " is no longer ours; leaving it and giving up on this site";
			return false;
		}
		HookGraveyard::park(hook);   // disable + leak the trampoline; never free under a live thread
		return true;
	};

	downIfOurs(0, "Present",        presentHook);
	downIfOurs(1, "Present1",       present1Hook);
	downIfOurs(2, "ResizeBuffers",  resizeBuffersHook);
	downIfOurs(3, "ResizeBuffers1", resizeBuffers1Hook);

	gOriginalPresent.store(nullptr, std::memory_order_release);
	gOriginalPresent1.store(nullptr, std::memory_order_release);
	gOriginalResizeBuffers.store(nullptr, std::memory_order_release);
	gOriginalResizeBuffers1.store(nullptr, std::memory_order_release);

	// Same create -> publish -> enable ordering as beginHook, for the same reason: a detour firing on another
	// thread must never observe an installed hook whose trampoline pointer has not been published yet.
	//
	// Written out per hook rather than through one generic helper. Each needs its own original<T>() function
	// pointer type, and threading that through a lambda only buys dependent-type noise for no shared logic.
	bool present = false, present1 = false;

	if (addresses.present)
	{
		presentHook = safetyhook::create_inline(addresses.present, &newDX12Present, safetyhook::InlineHook::StartDisabled);
		if (presentHook)
		{
			gOriginalPresent.store(presentHook.original<DX12Present*>(), std::memory_order_release);
			if (presentHook.enable()) present = true;
			else { PLOG_ERROR << "re-hook: failed to enable Present"; presentHook = {}; }
		}
		else PLOG_ERROR << "re-hook: failed to create the Present hook";
	}

	if (addresses.present1)
	{
		present1Hook = safetyhook::create_inline(addresses.present1, &newDX12Present1, safetyhook::InlineHook::StartDisabled);
		if (present1Hook)
		{
			gOriginalPresent1.store(present1Hook.original<DX12Present1*>(), std::memory_order_release);
			if (present1Hook.enable()) present1 = true;
			else { PLOG_ERROR << "re-hook: failed to enable Present1"; present1Hook = {}; }
		}
		else PLOG_ERROR << "re-hook: failed to create the Present1 hook";
	}

	if (addresses.resizeBuffers)
	{
		resizeBuffersHook = safetyhook::create_inline(addresses.resizeBuffers, &newDX12ResizeBuffers, safetyhook::InlineHook::StartDisabled);
		if (resizeBuffersHook)
		{
			gOriginalResizeBuffers.store(resizeBuffersHook.original<DX12ResizeBuffers*>(), std::memory_order_release);
			if (!resizeBuffersHook.enable()) { PLOG_ERROR << "re-hook: failed to enable ResizeBuffers"; resizeBuffersHook = {}; }
		}
	}

	if (addresses.resizeBuffers1)
	{
		resizeBuffers1Hook = safetyhook::create_inline(addresses.resizeBuffers1, &newDX12ResizeBuffers1, safetyhook::InlineHook::StartDisabled);
		if (resizeBuffers1Hook)
		{
			gOriginalResizeBuffers1.store(resizeBuffers1Hook.original<DX12ResizeBuffers1*>(), std::memory_order_release);
			if (!resizeBuffers1Hook.enable()) { PLOG_ERROR << "re-hook: failed to enable ResizeBuffers1"; resizeBuffers1Hook = {}; }
		}
	}

	snapshotHookSites();   // the new bytes are what teardown must compare against from now on
	mHarvestedAddresses = addresses;
	return present || present1;
}


void D3D12Hook::startRehookWatchdog()
{
	if (mWatchdogRunning.exchange(true)) return;

	mWatchdogThread = std::thread([this]()
		{
			// Generous first wait. A game that is loading, alt-tabbed or at a menu may legitimately not present
			// for a while, and re-hooking underneath a busy renderer for no reason is its own risk.
			// Generous: a level load, an alt-tab or a menu can legitimately produce no presents for a while, and
			// this only ever prints a diagnostic, so there is no reason to be twitchy about it.
			constexpr int kFirstCheckMs = 15000;

			// ⚠⚠ THIS WATCHDOG OBSERVES. IT DOES NOT RE-HOOK, AND THE FIRST VERSION DID.
			//
			// That version re-installed the swapchain hooks whenever no present had arrived within four seconds.
			// Two things were wrong with it, and together they FROZE THE GAME ON THE FIRST OPEN:
			//
			//   1. Four seconds of silence does not mean the hooks are broken. A level load, an alt-tab or a menu
			//      is enough. So it fired against a perfectly healthy renderer.
			//   2. Re-installing means re-harvesting, which builds a dummy window, D3D12 device and swapchain -
			//      slow, and it calls into DXGI, which may re-enter our own detours. Done while holding the
			//      exclusive hook guard, that stalls every Present and can self-deadlock outright.
			//
			// Repairing (2) alone would still leave (1): a watchdog that rebuilds a working renderer because the
			// game paused to load. Since re-hooking was never likely to fix the real complaint anyway - a reopened
			// HCM sees ZERO presents, not a few, which points at the chain routing around our address rather than
			// at a bad install - the honest version of this feature is to SAY so and change nothing.
			//
			// The fix for the underlying problem is per-object swapchain vtable hooking, not retrying the same
			// process-wide patch harder.
			int waited = 0;
			while (mWatchdogRunning.load() && !GlobalKill::isKillSet())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				waited += 250;
				if (waited < kFirstCheckMs) continue;

				if (shuttingDown.load(std::memory_order_acquire)) return;

				if (mPresentDetourFires.load(std::memory_order_relaxed) > 0)
				{
					PLOG_INFO << "D3D12 overlay watchdog: presents are reaching HCM; standing down.";
					mWatchdogRunning.store(false);
					return;
				}

				PLOG_ERROR << "D3D12 OVERLAY IS NOT RECEIVING FRAMES. Our Present hook installed successfully but "
					"nothing has called it in " << (kFirstCheckMs / 1000) << " seconds. If the game is loading or "
					"minimised this is harmless and will clear on its own. If HCM's menu never draws, another "
					"overlay in this process (RivaTuner, the Steam overlay, or NVIDIA Streamline / DLSS frame "
					"generation) owns the present chain and is not routing through the code we patched - restart "
					"the GAME, not just HCM, to recover it.";

				// Said once. Keep watching in case frames start later, but never repeat the wall of text.
				while (mWatchdogRunning.load() && !GlobalKill::isKillSet())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
					if (shuttingDown.load(std::memory_order_acquire)) return;
					if (mPresentDetourFires.load(std::memory_order_relaxed) > 0)
					{
						PLOG_INFO << "D3D12 overlay watchdog: presents started arriving after all; standing down.";
						mWatchdogRunning.store(false);
						return;
					}
				}
				return;
			}
		});
}


void D3D12Hook::stopRehookWatchdog()
{
	mWatchdogRunning.store(false);
	if (mWatchdogThread.joinable()) mWatchdogThread.join();
}


// See the declaration in D3D12Hook.h. Pure diagnostic - reads only, changes nothing.
void D3D12Hook::logSwapChainHookSiteBytes(const char* when)
{
	struct Site { const char* name; safetyhook::InlineHook* hook; };
	const Site sites[] = {
		{ "Present",          &presentHook },
		{ "Present1",         &present1Hook },
		{ "ResizeBuffers",    &resizeBuffersHook },
		{ "ResizeBuffers1",   &resizeBuffers1Hook },
		{ "ExecuteCmdLists",  &executeCommandListsHook },
	};

	// ⚠ THE ADDRESSES ARE CACHED ON THE FIRST CALL, AND THE FIRST VERSION DID NOT DO THIS.
	// park() MOVES the hook out, so by the "after restore" pass every hook object is empty and target() is gone -
	// the four swapchain sites silently printed nothing, which is exactly the half that tests whether the restore
	// put sane bytes back. Remembering the addresses makes the after-pass possible at all.
	static uint8_t* cachedTargets[5] = {};
	for (int i = 0; i < 5; i++)
		if (!cachedTargets[i] && *sites[i].hook) cachedTargets[i] = sites[i].hook->target();

	for (int i = 0; i < 5; i++)
	{
		const auto& s = sites[i];
		uint8_t* target = cachedTargets[i];
		if (!target || IsBadReadPtr(target, 16)) continue;

		std::string bytes;
		for (int i = 0; i < 16; i++) bytes += std::format("{:02X} ", target[i]);

		// Whose code is the site's first jump aimed at? If it is not our trampoline, someone hooked over us.
		std::string owner = "?";
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)target, &mod) && mod)
		{
			char path[MAX_PATH]{};
			if (GetModuleFileNameA(mod, path, MAX_PATH))
			{
				const char* leaf = strrchr(path, '\\');
				owner = leaf ? leaf + 1 : path;
			}
		}

		// Follow an E9 rel32 and name what it lands in. "no module" means a trampoline/island page - ours if it
		// matches safetyhook's allocator pattern, someone else's otherwise. This is what distinguishes "still
		// hooked, by us" from "still hooked, by a third party" from "genuinely restored".
		std::string jumpsTo;
		if (target[0] == 0xE9)
		{
			const int32_t rel = *reinterpret_cast<const int32_t*>(target + 1);
			const uintptr_t dest = (uintptr_t)target + 5 + rel;
			HMODULE dm = nullptr;
			std::string destOwner = "<no module - trampoline/island>";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)dest, &dm) && dm)
			{
				char dp[MAX_PATH]{};
				if (GetModuleFileNameA(dm, dp, MAX_PATH))
				{
					const char* dl = strrchr(dp, '\\');
					destOwner = std::format("{}+0x{:X}", dl ? dl + 1 : dp, dest - (uintptr_t)dm);
				}
			}
			jumpsTo = std::format("  -> jmp {:#X} ({})", dest, destOwner);
		}

		PLOG_INFO << "hook site [" << when << "] " << s.name << " @ " << std::format("0x{:X}", (uintptr_t)target)
			<< " in " << owner << " : " << bytes << jumpsTo;
	}

	// Only worth naming once per call, and only the modules that plausibly share these functions with us.
	static const char* kInteresting[] = {
		"sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "nvngx_dlssg.dll",
		"GFSDK_Aftermath_Lib.x64.dll", "graphics-hook64.dll", "RTSSHooks64.dll",
		"DiscordHook64.dll", "GameOverlayRenderer64.dll", "AMDDVR.dll",
	};
	std::string present;
	for (const char* m : kInteresting)
		if (GetModuleHandleA(m)) { if (!present.empty()) present += ", "; present += m; }

	PLOG_INFO << "hook site [" << when << "] other overlay/injector DLLs loaded: "
		<< (present.empty() ? std::string("(none detected)") : present);
}


// Safely destroys hooks, flushes the GPU, and releases every D3D12 object we acquired.
D3D12Hook::~D3D12Hook()
{
	PLOG_DEBUG << "~D3D12Hook()";

	// 1. Tell every detour to stop touching HCM/D3D12 state, BEFORE we start draining. A detour that
	//    is already past its first instruction will see this and just forward to the original.
	//    (It stays true afterwards on purpose - a straggler must never come back to life.)
	// Stop the watchdog BEFORE anything is torn down: it re-installs hooks, and doing that concurrently with
	// teardown would be a race against the very guard teardown relies on.
	stopRehookWatchdog();

	shuttingDown.store(true, std::memory_order_release);
	mQueueSearchActive.store(false, std::memory_order_relaxed);

	// 1b. The OBS bypass thunks sit on OBS's trampoline pointers, one link further down the same
	//     present chain, and they call renderOverlayFrame just like the detours do. They must come off
	//     BEFORE the guard block below, because teardownOBSBypass takes that same guard exclusively.
	teardownOBSBypass("D3D12Hook is shutting down");
	// ⚠ AND the pre-capture thunks, for exactly the same reason and with more urgency: they sit inside
	// OBS's own module, so leaving one installed past our destruction leaves OBS calling a thunk whose
	// `instance` is gone. Same exclusive-guard rule, so it is also outside the block below.
	teardownOBSPreCapture("D3D12Hook is shutting down");

	// 2. Drain, then remove the swapchain hooks. Taking the guard EXCLUSIVELY is what proves no
	//    thread is inside a detour body: the old ScopedAtomicBool was wait-then-set, so two threads
	//    could both observe false and both proceed, and a new entrant could slip in between the
	//    wait and the unhook. Assigning an empty hook destroys the existing one, which restores the
	//    original bytes; safetyhook freezes threads and fixes any instruction pointer sitting inside
	//    the relocated prologue while it does so.
	{
		PLOG_INFO << "Waiting for the D3D12 present/resize detours to finish execution";
		std::unique_lock<std::shared_mutex> guard(swapChainHookGuard);

		// ⚠ PARKED, NOT DESTROYED - and these are the ones that matter most.
		//
		// The exclusive guard above proves no thread is inside a DETOUR BODY. It says nothing about the
		// trampoline: a thread can be in the relocated prologue, or returning through it, having already left
		// the body and released its shared lock. Destroying the hook frees that page, and the first-chance log
		// caught precisely that during teardown - 0xC0000005, "tried to EXECUTE", at an address with no owning
		// module.
		//
		// These four sit on the hottest functions in the process. Present alone runs every frame on the render
		// thread, so the odds of someone being inside one at teardown are far better than even, which is why
		// this crash kept coming back after the ModuleHook hooks were already being parked - these bypass
		// ModuleHook entirely. See HookGraveyard.h.
		// ================================================================================================
		// EVIDENCE FOR THE E_ABORT PRESENT CRASH.
		//
		// The game dies just after HCM closes with:
		//     LowLevelFatalError [D3D12Util.cpp:1012] PresentInternal(SyncInterval) failed ... 0x80004004
		// and - now that the diagnostic filter is clean - with ZERO first-chance exceptions anywhere in the
		// run. Nothing faults. So this is not memory being freed under someone; a call in the present chain
		// is returning a failed HRESULT.
		//
		// The live suspect is hook ORDERING. HCM inline-hooks the SHARED IDXGISwapChain::Present inside
		// dxgi.dll - process-wide, and the very same function NVIDIA Streamline wraps for DLSS / frame
		// generation. If Streamline installed ITS hook after ours, our bytes are what it relocated, and
		// disable() writing dxgi's true original bytes back over the top silently destroys its dispatch.
		// No crash, no exception - just a present chain that no longer works, exactly what E_ABORT with a
		// clean log looks like.
		//
		// So: dump what is actually AT the target before and after we restore it, and name who else is
		// loaded. If the "before" bytes are not a jump to our own trampoline, someone is hooked on top of
		// us and restoring is the bug. If they are ours, this theory is dead and the answer is elsewhere.
		logSwapChainHookSiteBytes("before disable");

		// ⚠ RESTORE ONLY WHAT IS STILL OURS. See snapshotHookSites().
		//
		// A site whose bytes have changed since we installed belongs to somebody else now - RTSS re-hooks on a
		// timer and lands on top of us. Writing our saved "original" bytes there would erase a live third-party
		// hook, cut the present chain, and kill the game with 0x80004004 and no exception to explain it.
		//
		// In that case we LEAVE OUR HOOK INSTALLED, inside their chain, still forwarding. That means our detour
		// code must remain mapped, so the module is pinned below - HCM stays resident and cannot be re-injected
		// until the game restarts. That is the price of not breaking someone else's renderer, and it is the
		// cheaper of the two outcomes by a wide margin.
		bool leftInstalled = false;
		auto parkIfOurs = [&](int index, const char* name, safetyhook::InlineHook& hook)
		{
			if (!hook) return;
			if (!hookSiteIsStillOurs(index))
			{
				PLOG_WARNING << "hook site " << name << " no longer contains HCM's patch - another overlay "
					"(RivaTuner / Steam overlay / Streamline) hooked over us. LEAVING our hook installed rather "
					"than erasing theirs.";
				leftInstalled = true;
				return;   // deliberately not disabled, not parked: it stays live and forwarding
			}
			if (!HookGraveyard::park(hook)) { PLOG_ERROR << "could not disable the " << name << " hook; destroying it"; hook = {}; }
		};

		parkIfOurs(0, "Present",        presentHook);
		parkIfOurs(1, "Present1",       present1Hook);
		parkIfOurs(2, "ResizeBuffers",  resizeBuffersHook);
		parkIfOurs(3, "ResizeBuffers1", resizeBuffers1Hook);

		logSwapChainHookSiteBytes("after restore");

		// ⚠ ONLY null the trampoline pointers for hooks we actually took down. A hook we left installed is still
		// going to fire, and its detour forwards through exactly these - nulling them would strand the call.
		if (!presentHook)        gOriginalPresent.store(nullptr, std::memory_order_release);
		if (!present1Hook)       gOriginalPresent1.store(nullptr, std::memory_order_release);
		if (!resizeBuffersHook)  gOriginalResizeBuffers.store(nullptr, std::memory_order_release);
		if (!resizeBuffers1Hook) gOriginalResizeBuffers1.store(nullptr, std::memory_order_release);

		if (leftInstalled) mMustStayResident = true;
		PLOG_INFO << "D3D12 swapchain detours handled (trampolines deliberately leaked)"
			<< (leftInstalled ? "; SOME WERE LEFT INSTALLED - see the warnings above" : "");
	}

	// 3. Same for ExecuteCommandLists, under its own guard so it never serialised against Present.
	{
		std::unique_lock<std::shared_mutex> guard(executeCommandListsGuard);
		if (!hookSiteIsStillOurs(4))
		{
			PLOG_WARNING << "hook site ExecuteCommandLists no longer contains HCM's patch - another overlay "
				"hooked over us. LEAVING our hook installed rather than erasing theirs.";
			mMustStayResident = true;
		}
		else
		{
			if (!HookGraveyard::park(executeCommandListsHook))
			{
				PLOG_ERROR << "could not disable the ExecuteCommandLists hook; destroying it";
				executeCommandListsHook = {};
			}
			gOriginalExecuteCommandLists.store(nullptr, std::memory_order_release);
		}
	}

	// ⚠ PIN. A hook we left installed points at detour code inside this image. If HCM unmaps, the next frame the
	// game renders calls into a hole. Pinning is the only way to keep that address valid, and it is why this path
	// costs the user a game restart before HCM can be injected again - stated plainly in the message below rather
	// than left as a mystery "stuck on injecting".
	if (mMustStayResident)
	{
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			(LPCWSTR)&newDX12Present, &pinned);
		PLOG_FATAL << "HCM left one or more renderer hooks installed because another overlay (most likely "
			"RivaTuner Statistics Server / MSI Afterburner, or the Steam overlay) patched over them while HCM was "
			"running. Removing ours would have erased theirs and crashed the game. HCMInternal.dll has therefore "
			"been PINNED and will stay loaded until the game is restarted - HCM cannot be injected again in this "
			"session. To avoid this entirely, close RivaTuner before using HCM.";
	}

	// All five are now down. Anything still showing an E9 here was NOT restored - which, with five other hooking
	// parties in this process, is the difference between "we left the chain as we found it" and "we did not".
	logSwapChainHookSiteBytes("all hooks down");

	if (ID3D12CommandQueue* staleCandidate = mCandidateDirectQueue.exchange(nullptr, std::memory_order_acq_rel))
		staleCandidate->Release();

	// 4. No detour can reach us any more.
	instance = nullptr;

	// 5. Flush the GPU BEFORE releasing anything. Releasing a command allocator, a descriptor heap
	//    or a back buffer while the GPU is still reading it is the classic D3D12 unload crash.
	//    If the flush cannot be proven we LEAK instead of freeing - see releaseOrAbandon.
	//    (~ImGuiManager has already run by this point - App.h declares d3d before imm, so imm is
	//    destroyed first - which means ImGui_ImplDX12_Shutdown has already given back its
	//    descriptors and released its own device objects. It is also the reason this is the only
	//    place allowed to free mSrvHeap and mDevice.)
	const bool gpuIsIdle = waitForGpuIdle();
	releaseD3Dresources(gpuIsIdle);
	releaseImGuiBoundResources(gpuIsIdle);

	mRejectedSwapChain.store(nullptr, std::memory_order_relaxed);
	mRejectedSwapChainSkips.store(0, std::memory_order_relaxed);
}


#pragma region OBS bypass

// Removes both bypass thunks, restoring OBS's own trampoline pointers.
//
// ⚠ TAKES swapChainHookGuard EXCLUSIVELY. Never call it while holding that lock.
//
// ⚠⚠ IT RETIRES THE HOOKS, IT DOES NOT DESTROY THEM, AND THE EXCLUSIVE LOCK IS NOT ENOUGH ON ITS OWN.
// The lock proves no thread is inside a THUNK, because thunks run under a shared hold of it. It proves
// nothing about ModuleHookManager's deferred attach, which is the whole point of arming the bypass
// before OBS exists: when graphics-hook64.dll finally loads, the LOADER thread runs
// postModuleLoad_UpdateHooks -> ModuleInlineHook::attach() -> OBSRealFnPointer::resolve() -> the memory
// scan, and it takes NO lock, because ModuleHookManager has no mutex at all.
//
// So destroying the shared_ptr here could free the ModuleInlineHook - and the OBSRealFnPointer it owns,
// and the on-permanent-failure callback that captures the hook by raw pointer - underneath a loader
// thread that is mid-scan. Toggling the bypass off, or closing HCM, at the moment OBS injects was
// enough. Retiring instead of freeing costs two leaked objects per teardown and removes the race
// entirely; it is the same trade HookGraveyard already makes for the dxgi detours.
void D3D12Hook::teardownOBSBypass(const char* reason)
{
	std::unique_lock<std::shared_mutex> guard(swapChainHookGuard);

	if (!mOBSPresentHook && !mOBSPresent1Hook)
		return;

	PLOG_INFO << "Retiring the OBS bypass hooks: " << reason;

	// Park the inner safetyhook so OBS's own pointer is restored right now and no NEW caller can enter a
	// thunk, then hand the wrapper to the retirement list so nothing is freed. The published trampolines
	// below are deliberately left pointing at the parked (still mapped) code, so a thread already inside
	// a thunk has somewhere to forward to - see the fallback in the thunks.
	const auto retire = [](std::shared_ptr<ModuleInlineHook>& hook)
	{
		if (!hook) return;
		HookGraveyard::park(hook->getInlineHook());
		retiredOBSBypassHooks().push_back(std::move(hook));
		hook.reset();
	};

	retire(mOBSPresentHook);
	retire(mOBSPresent1Hook);
}


bool D3D12Hook::obsPreCaptureOwnsFrame(bool forPresent1)
{
	D3D12Hook* d3d = instance;
	if (!d3d) return false;

	const std::shared_ptr<ModuleInlineHook>& hook =
		forPresent1 ? d3d->mOBSPreCapturePresent1Hook : d3d->mOBSPreCapturePresentHook;
	return hook && hook->isHookInstalled();
}


void D3D12Hook::teardownOBSPreCapture(const char* reason)
{
	std::unique_lock<std::shared_mutex> guard(swapChainHookGuard);

	if (!mOBSPreCapturePresentHook && !mOBSPreCapturePresent1Hook)
		return;

	PLOG_INFO << "Retiring the OBS pre-capture hooks: " << reason;

	// Identical retirement policy to the bypass: park (so OBS's bytes are restored immediately and no new
	// caller can enter) and leak the wrapper, because ModuleHookManager's deferred attach runs lock-free on
	// the loader thread and a destroyed-but-registered hook is a use-after-free waiting for the next
	// LoadLibrary. The published trampolines stay valid, pointing at parked but still-mapped code.
	const auto retire = [](std::shared_ptr<ModuleInlineHook>& hook)
	{
		if (!hook) return;
		HookGraveyard::park(hook->getInlineHook());
		retiredOBSBypassHooks().push_back(std::move(hook));
		hook.reset();
	};

	retire(mOBSPreCapturePresentHook);
	retire(mOBSPreCapturePresent1Hook);
}


// Puts HCM's draw in front of OBS's capture, but ONLY when OBS is actually the outermost hook on the dxgi
// entry points. See the header for why that is the whole problem on D3D12.
//
// Deliberately quiet and best-effort: this runs whenever the bypass is switched off, which is also the
// default state, so it must not throw, must not complain when OBS is not running, and must not complain
// when HCM is already outermost - that last case is the one where everything already works.
void D3D12Hook::installOBSPreCapture()
{
	const uintptr_t presentEntry = (uintptr_t)mHarvestedAddresses.present;
	const uintptr_t present1Entry = (uintptr_t)mHarvestedAddresses.present1;
	if (!presentEntry && !present1Entry) return;

	// Nothing to sit in front of until OBS's hook module is actually in the process. Unlike the bypass we
	// do NOT arm a deferred attach here: the address we would hook is inside OBS's code and is only
	// discoverable once it has patched dxgi, so there is nothing to resolve ahead of time. The toggle is
	// re-applied when the user flips it, and OBS loading mid-session is picked up the next time they do.
	if (!ModuleCache::getModuleInfo(L"graphics-hook64.dll").has_value()) return;

	const auto install = [&](uintptr_t dxgiEntry, const char* name, auto* thunk,
		std::shared_ptr<ModuleInlineHook>& out)
	{
		if (!dxgiEntry) return;

		auto pointer = std::make_shared<OBSHookEntryPointer>(dxgiEntry, name);
		uintptr_t probe = 0;
		if (!pointer->resolve(&probe)) return;   // OBS does not own this entry; nothing to do, and it said why

		out = ModuleInlineHook::make(L"graphics-hook64.dll", pointer, thunk, true);
	};

	const auto publish = [](const std::shared_ptr<ModuleInlineHook>& hook, auto& slot)
	{
		if (!hook || !hook->isHookInstalled()) return;
		using Fn = std::remove_reference_t<decltype(slot)>::value_type;
		if (auto* original = hook->getInlineHook().original<Fn>())
			slot.store(original, std::memory_order_release);
	};

	{
		std::unique_lock<std::shared_mutex> guard(swapChainHookGuard);

		install(presentEntry, "Present", newDX12PresentOBSPreCapture, mOBSPreCapturePresentHook);
		if (present1Entry && present1Entry != presentEntry)
			install(present1Entry, "Present1", newDX12Present1OBSPreCapture, mOBSPreCapturePresent1Hook);

		publish(mOBSPreCapturePresentHook, gOBSPreCaptureOriginalPresent);
		publish(mOBSPreCapturePresent1Hook, gOBSPreCaptureOriginalPresent1);
	}

	const bool present = mOBSPreCapturePresentHook && mOBSPreCapturePresentHook->isHookInstalled();
	const bool present1 = mOBSPreCapturePresent1Hook && mOBSPreCapturePresent1Hook->isHookInstalled();

	if (present || present1)
		PLOG_INFO << std::format("OBS pre-capture installed (Present: {}, Present1: {}) - OBS Game Capture "
			"hooked dxgi after HCM did, so the overlay is now drawn in front of its capture instead of behind it.",
			present ? "yes" : "no", present1 ? "yes" : "no");
}


OBSBypassResult D3D12Hook::setOBSBypass(bool enabled)
{
	PLOGV << "D3D12Hook::setOBSBypass called with value: " << enabled;

	if (!enabled)
	{
		teardownOBSBypass("the user turned the bypass off");
		// ...and put the overlay in FRONT of OBS's capture instead. Without this, "bypass off" does not
		// mean "visible in OBS" on D3D12 - it means "whatever hook order happened to be", which for the
		// usual sequence (open HCM, then start Game Capture) is still invisible. No-op when OBS is not
		// running or when HCM is already the outermost hook.
		installOBSPreCapture();
		return OBSBypassResult::Removed;
	}

	// The two are mutually exclusive by construction: one draws before OBS's capture, the other after, and
	// having both live would render the overlay twice and defeat the bypass at the same time.
	teardownOBSPreCapture("the bypass is being turned on");

	// OBS does not have a separate D3D12 hook to defeat - d3d12_capture is selected behind the same
	// Present hook that d3d11_capture is - so this is the D3D11 mechanism applied to two entry points.
	const uintptr_t presentEntry = (uintptr_t)mHarvestedAddresses.present;
	const uintptr_t present1Entry = (uintptr_t)mHarvestedAddresses.present1;

	if (!presentEntry && !present1Entry)
	{
		PLOG_ERROR << "OBS bypass: no harvested dxgi Present/Present1 addresses, so there is nothing to match OBS's trampolines against. beginHook() must run first.";
		throw HCMRuntimeException("HCM has not finished setting up its renderer hooks, so the OBS bypass cannot be enabled yet.");
	}

	PLOG_DEBUG << std::format("OBS bypass: dxgi entries Present 0x{:X} (inline-hooked: {}), Present1 0x{:X} (inline-hooked: {})",
		presentEntry, looksInlineHooked(presentEntry) ? "yes" : "no",
		present1Entry, looksInlineHooked(present1Entry) ? "yes" : "no");
	// NOTE: unlike the D3D11 path, "inline-hooked" proves nothing about OBS here - WE hook these same
	// bytes ourselves. It is logged only because it tells a reader whether the chain is patched at all.

	// Start from a clean slate; re-enabling over a live bypass would leak a hook and double-render.
	teardownOBSBypass("re-arming");

	uintptr_t presentRva = 0, present1Rva = 0;
	OBSDiscoveryDiagnostics presentDiagnostics{}, present1Diagnostics{};
	OBSDiscoveryStatus presentStatus = OBSDiscoveryStatus::NoDetourFound;
	OBSDiscoveryStatus present1Status = OBSDiscoveryStatus::NoDetourFound;

	if (presentEntry)
		presentStatus = discoverRealFnRva(presentEntry, presentRva, &presentDiagnostics);
	if (present1Entry && present1Entry != presentEntry)
		present1Status = discoverRealFnRva(present1Entry, present1Rva, &present1Diagnostics);

	const bool moduleNotLoaded =
		(presentEntry && presentStatus == OBSDiscoveryStatus::ModuleNotLoaded) ||
		(present1Entry && present1Status == OBSDiscoveryStatus::ModuleNotLoaded);

	const bool foundSomething =
		presentStatus == OBSDiscoveryStatus::Ok || present1Status == OBSDiscoveryStatus::Ok;

	if (!moduleNotLoaded && !foundSomething)
	{
		const OBSDiscoveryDiagnostics& d = presentEntry ? presentDiagnostics : present1Diagnostics;
		PLOG_ERROR << std::format(
			"OBS bypass: graphics-hook64.dll is loaded (base 0x{:X}, size 0x{:X}) but neither a Present nor a Present1 "
			"trampoline pointer could be found in it. Sections scanned: {}. {} aligned qwords examined, {} probed. "
			"dxgi entries used: Present 0x{:X}, Present1 0x{:X}.",
			d.moduleBase, (uint64_t)d.moduleSize, d.sectionsScanned, d.qwordsScanned, d.candidatesProbed,
			presentEntry, present1Entry);
		throw HCMRuntimeException("OBS is running but HCM could not locate its Present hook (unsupported OBS version). Report this with your OBS version.");
	}

	// Install. Both hooks start enabled so that, when graphics-hook64.dll is not loaded yet, they stay
	// registered with ModuleHookManager and attach by themselves the moment OBS Game Capture injects.
	const auto install = [](uintptr_t dxgiEntry, const char* name, void* thunk, std::shared_ptr<ModuleInlineHook>& out)
	{
		if (!dxgiEntry) return;

		auto realFnPointer = std::make_shared<OBSRealFnPointer>(dxgiEntry, name);
		out = ModuleInlineHook::make(L"graphics-hook64.dll", realFnPointer, thunk, true);
		if (!out) return;

		// A LATER (deferred) discovery failure happens inside LoadLibrary, where throwing is not an
		// option. Stop wanting to be attached instead of retrying on every DLL load forever.
		//
		// A RAW pointer, captured by value: `out` is a reference parameter that dies with this call,
		// and the callback outlives it. The hook owns the OBSRealFnPointer that owns this callback, so
		// the callback can only ever run while the hook is alive.
		ModuleInlineHook* hook = out.get();
		realFnPointer->setOnPermanentFailure([hook]() { hook->setWantsToBeAttached(false); });
	};

	// Publish the trampolines for the thunks' teardown-race fallback. Done after install, and only if the
	// hook actually attached - when OBS is not running yet there is no trampoline to publish, and the
	// deferred attach will not come back through here. That is fine: with no hook installed nothing can be
	// inside a thunk either, so there is no straggler to rescue.
	const auto publish = [](const std::shared_ptr<ModuleInlineHook>& hook, auto& slot)
	{
		if (!hook || !hook->isHookInstalled()) return;
		using Fn = std::remove_reference_t<decltype(slot)>::value_type;
		if (auto* original = hook->getInlineHook().original<Fn>())
			slot.store(original, std::memory_order_release);
	};

	{
		// Same exclusive hold as teardown: the detours read these shared_ptrs under a shared hold.
		std::unique_lock<std::shared_mutex> guard(swapChainHookGuard);

		if (presentStatus != OBSDiscoveryStatus::NoDetourFound)
			install(presentEntry, "Present", newDX12PresentOBSBypass, mOBSPresentHook);
		if (present1Status != OBSDiscoveryStatus::NoDetourFound)
			install(present1Entry, "Present1", newDX12Present1OBSBypass, mOBSPresent1Hook);

		publish(mOBSPresentHook, gOBSBypassOriginalPresent);
		publish(mOBSPresent1Hook, gOBSBypassOriginalPresent1);
	}

	if (moduleNotLoaded)
	{
		PLOG_INFO << "OBS bypass armed, but graphics-hook64.dll is not loaded - it will attach automatically when OBS Game Capture injects";
		return OBSBypassResult::DeferredModuleNotLoaded;
	}

	// Discovery said yes for at least one entry point, so at least one hook must actually be installed.
	const bool presentInstalled = mOBSPresentHook && mOBSPresentHook->isHookInstalled();
	const bool present1Installed = mOBSPresent1Hook && mOBSPresent1Hook->isHookInstalled();

	if (!presentInstalled && !present1Installed)
	{
		PLOG_ERROR << std::format(
			"OBS bypass: discovery succeeded (Present rva 0x{:X}, Present1 rva 0x{:X}) but neither inline hook installed. "
			"See the attach log above.", presentRva, present1Rva);
		teardownOBSBypass("installation failed");
		throw HCMRuntimeException("HCM found OBS's Present hook but could not install the bypass over it. Another program may have hooked it first.");
	}

	// UE5 presents through Present1, so a Present-only bypass will not do anything on this game. Worth
	// saying out loud rather than leaving the user to wonder why the overlay is still in the recording.
	if (!present1Installed && present1Entry && present1Entry != presentEntry)
		PLOG_WARNING << "OBS bypass: only the Present trampoline was bypassed. This game presents through Present1, so the overlay may still appear in the capture.";

	PLOG_INFO << std::format("OBS bypass installed (Present: {}, Present1: {})",
		presentInstalled ? std::format("graphics-hook64.dll+0x{:X}", presentRva) : std::string("not installed"),
		present1Installed ? std::format("graphics-hook64.dll+0x{:X}", present1Rva) : std::string("not installed"));

	return OBSBypassResult::Installed;
}

#pragma endregion OBS bypass
