#pragma once
#include "pch.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"

// imgui
// NOTE: we deliberately do NOT include imgui_impl_win32.h here, and we deliberately do NOT
// re-declare the scalar/ImVec4 operator overloads that live at header scope in D3D11Hook.h.
// D3D12Hook owns *only* the d3d12 renderer backend's resources; the ImGui context, the win32
// platform backend, the WndProc chain, the style and the fonts all stay owned by ImGuiManager
// exactly as they are today for the D3D11 path. If this header and D3D11Hook.h ever end up in
// the same translation unit, duplicating those operators here would be a hard compile error.
#include "imgui.h"
#include "imgui_impl_dx12.h"

// directx 12
#include <d3d12.h>
#include <dxgi1_2.h> // IDXGISwapChain1 (Present1), DXGI_PRESENT_PARAMETERS
#include <dxgi1_4.h> // IDXGISwapChain3 (GetCurrentBackBufferIndex, ResizeBuffers1)

#include <atomic>
#include <shared_mutex>

// Define the DX functions we're going to hook.
// These are COM methods, so the first parameter is the "this" pointer. On x64 __stdcall is the
// one and only native calling convention, so this matches the real ABI exactly.
typedef HRESULT __stdcall DX12Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
// UE5's D3D12 RHI (FD3D12Viewport::PresentInternal) presents through IDXGISwapChain1::Present1
// whenever it holds an IDXGISwapChain1 - which it always does on Win10+. Present1 is a SEPARATE
// vtable slot (22) with its own body in dxgi.dll, so a hook on Present (slot 8) alone would never
// fire and the overlay would silently never appear. We hook both into one shared implementation.
typedef HRESULT __stdcall DX12Present1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
typedef HRESULT __stdcall DX12ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
// Same story for resizes: UE5's FD3D12Viewport::Resize uses IDXGISwapChain3::ResizeBuffers1
// (slot 39) under an explicit multi-GPU node mask. Missing it would mean the game calls
// ResizeBuffers1 while we still hold back-buffer references -> DXGI_ERROR_INVALID_CALL ->
// VERIFYD3D12RESULT -> UE_LOG(Fatal), i.e. we would kill the GAME. Best-effort, never required.
typedef HRESULT __stdcall DX12ResizeBuffers1(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue);
typedef void __stdcall DX12ExecuteCommandLists(ID3D12CommandQueue* pCommandQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);


// The D3D12 analogue of D3D11Hook::presentHookEvent.
// D3D11 payload was (device, deviceContext, swapChain, renderTargetView).
// Under D3D12 the immediate context becomes a *command list* the subscriber records into, and the
// ID3D11RenderTargetView* becomes a CPU descriptor handle for this frame's back buffer RTV.
// Contract with subscribers (i.e. ImGuiManager's dx12 branch):
//   - the command list has already been Reset() for this frame,
//   - the back buffer is already transitioned to D3D12_RESOURCE_STATE_RENDER_TARGET,
//   - OMSetRenderTargets and SetDescriptorHeaps(srvHeap) have already been called on it,
//   - the subscriber MUST NOT Close(), Execute() or Signal() - D3D12Hook owns all of that,
//   - a subscriber that throws costs the overlay: we catch it, close the list and kill HCM, because
//     letting an exception unwind through DXGI/UE5 frames is not survivable for the game.
typedef eventpp::CallbackList<void(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, IDXGISwapChain3* pSwapChain, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV)> D3D12PresentEvent;


// Singleton. Plays exactly the role D3D11Hook plays for MCC, but for HaloCampaignEvolved.exe (UE 5.5.4, D3D12).
//
// Two-phase construction, same as D3D11Hook: the constructor installs NOTHING (so no callback can
// fire into a half-built service graph); beginHook() is called last, from App.h, once every service exists.
// On destruction the hooks are removed, the GPU is flushed, and every D3D12 object we own is released.
class D3D12Hook : public std::enable_shared_from_this<D3D12Hook>
{
private:
	static D3D12Hook* instance; // Private Singleton instance so static hooks/callbacks can access

	// ---- detour lifetime gate ------------------------------------------------------------------
	// WHY NOT ScopedAtomicBool (which D3D11Hook uses): ScopedAtomicBool is `wait(true); atom = true;`
	// - a textbook TOCTOU, not a lock. Two threads can both observe false and both proceed. That was
	// benign for D3D11Hook because create_vmt hooks ONE swapchain object, so only the game's render
	// thread ever entered the detour. Our hooks are INLINE hooks on dxgi's shared implementations, so
	// every swapchain in the process (Steam / Discord / RivaTuner / MF thumbnailers) enters them on
	// its own thread. A real reader-writer lock is the only thing that lets ~D3D12Hook prove nobody
	// is inside a detour body before safetyhook frees the trampoline.
	//   detours:     std::shared_lock(swapChainHookGuard)
	//   destructor:  shuttingDown = true, then std::unique_lock(swapChainHookGuard)
	static inline std::shared_mutex swapChainHookGuard{};
	// ExecuteCommandLists runs on arbitrary game threads at a far higher rate; it gets its own guard
	// so it never serialises against our render work.
	static inline std::shared_mutex executeCommandListsGuard{};
	// Set BEFORE the destructor starts draining, so a detour that is already past its first
	// instruction stops touching D3D12/HCM state and just forwards.
	static inline std::atomic_bool shuttingDown{ false };

	// Our hook functions
	static DX12Present newDX12Present;
	static DX12Present1 newDX12Present1;
	static DX12ResizeBuffers newDX12ResizeBuffers;
	static DX12ResizeBuffers1 newDX12ResizeBuffers1;
	static DX12ExecuteCommandLists newDX12ExecuteCommandLists;

	// The one shared body behind both newDX12Present and newDX12Present1. Does everything except
	// call the original - the caller owns that, because the two originals have different signatures.
	static void renderOverlayFrame(IDXGISwapChain* pSwapChain, UINT presentFlags);
	// The shared body behind both newDX12ResizeBuffers and newDX12ResizeBuffers1.
	static bool prepareForResize(IDXGISwapChain* pSwapChain); // true => rebuild after the original returns
	static void finishResize(HRESULT originalResult);

	// not required, but used to attempt to resolve the swapchain vmt without needing a dummy swapchain
	std::weak_ptr<PointerDataStore> pointerDataStoreWeak;

	// ---- hooks ------------------------------------------------------------------------------
	// WHY INLINE AND NOT VMT (D3D11Hook uses safetyhook::create_vmt):
	// create_vmt needs a *live object instance* whose vptr it can swap. D3D11Hook gets one from
	// PointerDataStore (nameof(pIDXGISwapChain), see InternalPointerData.xml). There is no such
	// datum for HaloCampaignEvolved.exe, and HCMInternal.dll is injected into an already-running
	// game, so at beginHook() time we have no pointer to the game's swapchain or to its command
	// queue - and the reference overlay's CreateSwapChainForHwnd capture path is useless for the
	// same reason (the game created its swapchain long before we loaded).
	// So we hook the *shared implementations* in dxgi.dll / d3d12.dll, harvested from throwaway
	// dummy objects, which is what the proven HCE overlay does. That makes the detours
	// process-wide, so every detour filters on "is this the swapchain/queue we adopted".
	// If a pIDXGISwapChain equivalent is ever added to InternalPointerData.xml for HCE, only
	// harvestHookAddresses() needs to change (see resolveLiveSwapChainVtable()).
	//
	// Every one of these is installed with StartDisabled, its trampoline published to a file-static
	// atomic, and only then enabled. That ordering is what makes "trampoline pointer is null" mean
	// exactly one thing - "the hook has already been removed" - which the detours rely on.
	safetyhook::InlineHook presentHook;
	safetyhook::InlineHook present1Hook;
	safetyhook::InlineHook resizeBuffersHook;
	safetyhook::InlineHook resizeBuffers1Hook;
	safetyhook::InlineHook executeCommandListsHook;

	struct HookAddresses
	{
		void* present = nullptr;
		void* present1 = nullptr;         // optional
		void* resizeBuffers = nullptr;
		void* resizeBuffers1 = nullptr;   // optional
		void* executeCommandLists = nullptr;
	};
	HookAddresses harvestHookAddresses(); // throws HCMInitException if it cannot get the required three
	void** resolveLiveSwapChainVtable();  // optional PointerDataStore fast path; nullptr if unavailable

	// ---- per-back-buffer D3D12 state ---------------------------------------------------------
	// What we draw INTO. Indexed by IDXGISwapChain3::GetCurrentBackBufferIndex().
	struct BackBuffer
	{
		ID3D12Resource* resource = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
	};
	std::vector<BackBuffer> mBackBuffers;

	// What we record WITH. Indexed by our OWN render ordinal, NOT by the back buffer index.
	// This is load-bearing: imgui's dx12 backend cycles its vertex/index buffer ring on every
	// ImGui_ImplDX12_RenderDrawData call (bd->frameIndex % NumFramesInFlight), i.e. only on frames
	// we actually render. Keying our fence ring on the back buffer index instead would drift out of
	// step with imgui's ring on every skipped frame and on every DXGI_PRESENT_TEST (which does not
	// flip), after which waiting on a back buffer's fence no longer proves that the submission which
	// last touched imgui's buffers has retired - the CPU then memcpys a buffer the GPU is reading.
	struct RenderSlot
	{
		ID3D12CommandAllocator* allocator = nullptr;
		UINT64 fenceValue = 0; // last fence value signalled for work recorded with this allocator
	};
	std::vector<RenderSlot> mRenderSlots;
	UINT64 mRenderOrdinal = 0; // incremented ONLY on frames we actually submitted

	// identity only, never dereferenced for anything but the pointer compare in the detours.
	// It stays valid because mSwapChain3 (the same COM object) holds a reference.
	IDXGISwapChain* mAdoptedSwapChain = nullptr;
	IDXGISwapChain3* mSwapChain3 = nullptr;  // we own a ref. Needed for GetCurrentBackBufferIndex.
	ID3D12Device* mDevice = nullptr;         // we own a ref
	ID3D12CommandQueue* mCommandQueue = nullptr; // we own a ref
	ID3D12GraphicsCommandList* mCommandList = nullptr;
	ID3D12DescriptorHeap* mRtvHeap = nullptr;
	ID3D12DescriptorHeap* mSrvHeap = nullptr; // shader-visible; imgui's font SRV lives here
	ID3D12Fence* mFence = nullptr;
	HANDLE mFenceEvent = nullptr;
	UINT64 mNextFenceValue = 1;
	UINT mRtvDescriptorSize = 0;
	UINT mBufferCount = 0;      // swapchain back buffer count; may change on resize
	UINT mFramesInFlight = 0;   // FROZEN at the value handed to ImGui_ImplDX12_Init - see mRenderSlots
	DXGI_FORMAT mRtvFormat = DXGI_FORMAT_UNKNOWN;
	HWND mWindowHandle = nullptr;
	// Bumped whenever our back buffers / swapchain resources are torn down or rebuilt. The present
	// path samples it around the render event so that a ResizeBuffers re-entered from inside that
	// event (via ImGuiManager's PeekMessage pump -> WndProc) can never leave us submitting a command
	// list that references a released back buffer.
	uint64_t mResourceGeneration = 0;

	// Guards every mutation of the D3D12 objects above, and waitForGpuIdle().
	// It exists because waitForGpuIdle() is PUBLIC (~ImGuiManager calls it on the HCM shutdown
	// thread) while everything else runs on the game's render thread, and the two would otherwise
	// race on mRenderSlots.
	// RECURSIVE because disableOverlayPermanently -> waitForGpuIdle -> releaseD3Dresources ->
	// releaseSwapChainResources all nest.
	// DEADLOCK NOTE: nothing inside this lock may call into ImGuiManager. ImGuiManager takes its own
	// mDestructionGuard around both its present handler and its destructor, so holding this across
	// presentHookEvent would be an ABBA inversion against ~ImGuiManager's waitForGpuIdle() call.
	// The render event is deliberately fired OUTSIDE it.
	std::recursive_mutex mResourceMutex;

	// ---- command queue capture (the crux, see D3D12Hook.cpp) ---------------------------------
	// Written by newDX12ExecuteCommandLists on the game's submitting thread, consumed by
	// newDX12Present on the render thread - a different thread, one or more frames later. The
	// published pointer therefore carries an OWNED reference (AddRef in the detour); without it, a
	// transient DIRECT queue that submits once and is then released would leave the consumer calling
	// a vtable on freed memory, which is a hard crash in the GAME.
	static inline std::atomic<ID3D12CommandQueue*> mCandidateDirectQueue = nullptr;
	// While true the ECL detour does the (cheap) work of recording candidates. Cleared once we have a
	// validated queue AND whenever the overlay is disabled, so the detour cannot keep doing a virtual
	// GetDesc() on every submission on every queue in the process for the rest of the session.
	static inline std::atomic_bool mQueueSearchActive = true;

	// Single-entry negative cache for the swapchain filter. Our Present detours are inline hooks on
	// dxgi's shared implementation, so without this every foreign swapchain in the process pays the
	// full QueryInterface + GetDevice + GetDesc + IsWindow + GetWindowThreadProcessId battery on
	// EVERY one of its frames, forever. Identity compare only - never dereferenced.
	static inline std::atomic<IDXGISwapChain*> mRejectedSwapChain = nullptr;
	static inline std::atomic_uint32_t mRejectedSwapChainSkips = 0;

	bool isD3DdeviceInitialized = false;
	// Latched after an unrecoverable failure (device removed, device changed under a live imgui
	// backend, ...). We keep the hooks installed - removing a hook from inside its own detour is
	// not safe - but never touch D3D12 again, so the game keeps rendering without an overlay.
	// ALWAYS latch it through disableOverlayPermanently(): latching it while we still hold back
	// buffer references would make every subsequent IDXGISwapChain::ResizeBuffers fail with
	// DXGI_ERROR_INVALID_CALL, which UE5 turns into UE_LOG(Fatal) - killing the game.
	bool overlayPermanentlyDisabled = false;
	// True once ImGuiManager has bound its dx12 backend to our device/heap, which is the point
	// after which we can no longer silently rebuild on a different device - and after which mDevice
	// and mSrvHeap may only be released by ~D3D12Hook (which runs after ~ImGuiManager).
	bool mHasFiredPresentEvent = false;
	// GlobalKill releases our back buffers exactly once, then every frame passes straight through.
	bool mReleasedForShutdown = false;

	void initializeD3Ddevice(IDXGISwapChain* pSwapChain); // throws HCMInitException
	void createBackBufferResources();                    // throws HCMInitException
	void releaseBackBufferResources();                   // back buffer refs only (for ResizeBuffers)
	void releaseSwapChainResources(bool gpuIsIdle);      // + allocators, command list, rtv heap, swapchain
	void releaseD3Dresources(bool gpuIsIdle);            // + fence/queue (keeps imgui-bound objects)
	void releaseImGuiBoundResources(bool gpuIsIdle);     // mSrvHeap + mDevice; ~D3D12Hook only
	void disableOverlayPermanently(const char* reason);
	bool anyBackBufferHeld() const;
	bool tryAdoptSwapChain(IDXGISwapChain* pSwapChain);
	bool tryCaptureCommandQueue();
	static bool sameComObject(IUnknown* a, IUnknown* b);
	static bool queueMatchesDevice(ID3D12CommandQueue* queue, ID3D12Device* expectedDevice);
	static bool isOwnedByThisProcess(HWND hwnd);
	// Returns the device removed reason (S_OK when healthy) and logs it when it isn't.
	static HRESULT checkDeviceRemoved(ID3D12Device* device, const char* context);

	// ---- SRV descriptor sub-allocator handed to imgui's dx12 backend -------------------------
	// The backend asks us for descriptors out of mSrvHeap (font atlas + any user textures) and
	// hands them back on shutdown. The reference used a never-resetting bump index which leaked a
	// slot on every re-init; this is a real free list so repeated init/shutdown cycles are safe.
	// These callbacks are static and are held (with a raw copy of the InitInfo) by imgui's backend,
	// so they can outlive the heap and even the instance - they must never trust `info`. mSrvHeapLive
	// is the gate: it is false whenever info->SrvDescriptorHeap / info->Device may be dangling.
	static constexpr UINT SRV_DESCRIPTOR_COUNT = 64;
	static inline std::vector<UINT> mSrvFreeList{};
	static inline std::mutex mSrvFreeListMutex{};
	static inline std::atomic_bool mSrvHeapLive{ false };
	void resetSrvDescriptorAllocator();
	static void srvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu);
	static void srvDescriptorFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

	static SimpleMath::Vector2 mScreenSize;
	static SimpleMath::Vector2 mScreenCenter;

public:

	explicit D3D12Hook(std::weak_ptr<PointerDataStore> pointerDataStore);
	~D3D12Hook();

	// Callback for present rendering. Same role as D3D11Hook::presentHookEvent.
	std::shared_ptr<D3D12PresentEvent> presentHookEvent = std::make_shared<D3D12PresentEvent>();

	// Callback for window resize. Identical type to D3D11Hook's, so existing subscribers work unchanged.
	std::shared_ptr<eventpp::CallbackList<void(SimpleMath::Vector2 newScreenSize)>> resizeBuffersHookEvent = std::make_shared<eventpp::CallbackList<void(SimpleMath::Vector2 newScreenSize)>>();

	// Banned operations for singleton
	D3D12Hook(const D3D12Hook& arg) = delete; // Copy constructor
	D3D12Hook(const D3D12Hook&& arg) = delete;  // Move constructor
	D3D12Hook& operator=(const D3D12Hook& arg) = delete; // Assignment operator
	D3D12Hook& operator=(const D3D12Hook&& arg) = delete; // Move operator

	SimpleMath::Vector2 getScreenSize() { return mScreenSize; }
	SimpleMath::Vector2 getScreenCenter() { return mScreenCenter; }

	void beginHook();

	// Exists purely so OBSBypassManager can be constructed against this hook type. There is no
	// D3D12 capture path in graphics-hook64.dll that we have offsets for, so enabling throws a
	// clean runtime exception (which OBSBypassManager catches and turns into a user message +
	// resets the toggle). Notably it does NOT touch Lapua::lapuaGood.
	void setOBSBypass(bool enabled);

	// Signal + wait until the GPU has retired everything we submitted. Returns false if the flush
	// could not be proven (Signal failed, or the wait timed out) - in which case the caller must
	// NOT free anything the GPU might still be reading.
	//
	// PUBLIC because ~ImGuiManager must call it before ImGui_ImplDX12_Shutdown(). App.h declares
	// `d3d` before `imm`, so imm is destroyed FIRST: ImGui_ImplDX12_Shutdown() ->
	// InvalidateDeviceObjects() releases imgui's PSO, root signature, per-frame vertex/index buffers
	// and font texture with no GPU wait of its own (the dx12 backend never fences on shutdown), and
	// the last thing we submitted was an overlay draw referencing exactly those objects. D3D12 does
	// not defer destruction on GPU usage.
	bool waitForGpuIdle();

	// ---- accessors for ImGuiManager's dx12 branch --------------------------------------------
	// ImGui_ImplDX12_Init needs things the D3D11 path never had to pass around (an SRV heap, the
	// frames-in-flight count, the RTV format). Rather than widen the present event with init-only
	// data, ImGuiManager pulls a ready-made InitInfo off the hook on its first present event.
	// Returns false if the D3D12 device isn't up yet (which cannot happen from inside a present
	// event, but is checked anyway).
	bool getImGuiInitInfo(ImGui_ImplDX12_InitInfo& outInfo) const;

	ID3D12Device* getDevice() const { return mDevice; }
	ID3D12CommandQueue* getCommandQueue() const { return mCommandQueue; }
	ID3D12DescriptorHeap* getSrvDescriptorHeap() const { return mSrvHeap; }
	UINT getBackBufferCount() const { return mBufferCount; }
	// The NumFramesInFlight imgui's dx12 backend was initialised with. Frozen for the lifetime of
	// that backend, which is why it is a separate number from getBackBufferCount().
	UINT getFramesInFlight() const { return mFramesInFlight; }
	DXGI_FORMAT getRenderTargetFormat() const { return mRtvFormat; }
	HWND getWindowHandle() const { return mWindowHandle; }
	bool isInitialized() const { return isD3DdeviceInitialized; }
};
