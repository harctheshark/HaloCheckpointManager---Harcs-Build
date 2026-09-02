#pragma once
#include "pch.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "ModuleHook.h"          // ModuleInlineHook, for the OBS bypass
#include "OBSHookDiscovery.h"    // OBSBypassResult / OBSRealFnPointer

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

	// ---- GROUND TRUTH: the presenting queue, by definition rather than by inference ---------------
	// For a D3D12 swapchain the FIRST PARAMETER of every DXGI creation call IS the presenting
	// ID3D12CommandQueue - that is what the runtime orders presents against, and it is exactly the
	// thing the submission statistic can only guess at.
	//
	// ⚠ THIS DOES NOT FIRE ON A LATE ATTACH. HCM usually injects into a process whose swapchain was
	// created long ago, so on attach there is nothing to observe and the statistic still decides.
	// What makes it worth having anyway is that the game RECREATES the swapchain whenever the render
	// mode changes - DLSS <-> FSR <-> XeSS <-> TSR <-> Native, and every frame-generation toggle,
	// because FG swaps the whole swapchain provider (see SwapChainProvider in GameUserSettings.ini).
	// So the first mode change after attach upgrades us from a guess to a fact, permanently.
	static HRESULT __stdcall newCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice,
		DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
	static HRESULT __stdcall newCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
		const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
		IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

	// Records `queue` as the authoritative presenting queue for `swapChain`. Safe to call from any
	// thread and with anything (a D3D11 device, null, a foreign-device queue) - it filters.
	static void recordAuthoritativeQueue(IUnknown* device, IDXGISwapChain* swapChain, const char* source);
	static void clearAuthoritativeQueue();

	// Set only while harvestHookAddresses is creating its throwaway swapchain, so the creation detours
	// can tell our own dummy apart from the game's real one. See recordAuthoritativeQueue.
	static inline std::atomic_bool sHarvesting{ false };

	// The queue DXGI itself associates with a swapchain, and which swapchain that was.
	// ⚠ mAuthoritativeQueue holds a REFERENCE (AddRef'd via QueryInterface) - it is not borrowed. The
	// owning swapchain dies on exactly the events this exists to catch, and swapchain addresses get
	// REUSED, so a raw borrow would let us Signal() a freed queue. Both are guarded by the mutex rather
	// than being atomics, because the pair has to move together.
	static inline std::mutex mAuthoritativeMutex{};
	static inline ID3D12CommandQueue* mAuthoritativeQueue = nullptr;
	static inline IDXGISwapChain* mAuthoritativeFor = nullptr;

	// ---- OBS bypass -----------------------------------------------------------------------------
	// These do NOT hook dxgi. They hook the ADDRESS HELD BY OBS's RealPresent / RealPresent1 function
	// pointers inside graphics-hook64.dll, i.e. the Detours trampolines that OBS's hook_present calls
	// AFTER it has taken its clean capture. Rendering there is what puts HCM's overlay on the
	// streamer's screen but not in the recording. Located at runtime - see OBSHookDiscovery.h.
	//
	// ⚠ ORDERING WITH OUR OWN HOOKS. We already inline-hook dxgi's Present/Present1 entry points -
	// the very bytes OBS Detours-patches. Depending on who installed first, the OBS trampoline sits
	// either inside or outside our own detour, but EITHER WAY both our detour and this thunk would
	// render the same frame. So while a bypass thunk is installed, the matching newDX12Present /
	// newDX12Present1 stops rendering and only forwards. Same suppression D3D11Hook.cpp does for its
	// vmt path, just per-entry-point because Present and Present1 can be bypassed independently.
	static DX12Present newDX12PresentOBSBypass;
	static DX12Present1 newDX12Present1OBSBypass;
	std::shared_ptr<ModuleInlineHook> mOBSPresentHook;
	std::shared_ptr<ModuleInlineHook> mOBSPresent1Hook;
	// "Is the bypass thunk for this entry point live right now?" Deliberately asks the hook object
	// rather than caching a flag: the hook can attach LATER, by itself, when graphics-hook64.dll is
	// loaded (ModuleHookManager does that), and a cached flag would miss it and double-render.
	// ⚠ Only valid to call with swapChainHookGuard held (which every detour does), because that is
	// what teardownOBSBypass takes exclusively before destroying these.
	static bool obsBypassOwnsFrame(bool forPresent1);
	// Removes both thunks. Takes swapChainHookGuard EXCLUSIVELY, so it must never be called with that
	// lock already held. Safe to call when nothing is installed.
	void teardownOBSBypass(const char* reason);

	// ---- OBS PRE-CAPTURE: the exact mirror of the bypass, and the fix for "OBS never captures HCM" ----
	//
	// The bypass draws AFTER OBS's data.capture(), so the overlay is local-only. This draws BEFORE it, so
	// the overlay IS in the recording. Both are needed on D3D12 because hook order otherwise decides the
	// answer and the user has no say in it:
	//
	//   MCC / D3D11   our present hook is a VMT hook on the swapchain, which is unconditionally OUTSIDE
	//                 any inline detour on dxgi's shared body -> OBS always captures the overlay.
	//   HaloCER/D3D12 we inline-patch the same bytes OBS Detours. graphics-hook64.dll injects when the
	//                 Game Capture source starts, i.e. usually AFTER HCM is open, so OBS lands on top and
	//                 its hook_present captures the frame before our detour has drawn anything. The
	//                 overlay is then absent from the recording no matter what the toggle says.
	//
	// So when the bypass is OFF and OBS owns the entry point, we install our draw at the ENTRY of OBS's
	// hook_present - in front of its capture - and the ordinary detour stands down for that frame.
	//
	// ⚠ ADDITIVE, NOT A HOOK WAR. We never re-patch dxgi to take the outermost slot back;
	// reinstallSwapChainHooks() deliberately refuses to do that ("re-hooking on top of somebody else's
	// patch is how hook wars escalate") and it is right. Sitting in front of OBS's own function touches
	// nobody else's bytes and disappears cleanly when OBS unloads.
	static DX12Present newDX12PresentOBSPreCapture;
	static DX12Present1 newDX12Present1OBSPreCapture;
	std::shared_ptr<ModuleInlineHook> mOBSPreCapturePresentHook;
	std::shared_ptr<ModuleInlineHook> mOBSPreCapturePresent1Hook;
	// Same contract as obsBypassOwnsFrame: asks the hook object, not a cached flag, and requires
	// swapChainHookGuard to be held.
	static bool obsPreCaptureOwnsFrame(bool forPresent1);
	void teardownOBSPreCapture(const char* reason);
	// Installs the pre-capture thunks if - and only if - OBS currently owns the dxgi entry points.
	// Silent no-op otherwise, because "we are already outermost" needs no fixing.
	void installOBSPreCapture();

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
	safetyhook::InlineHook createSwapChainHook;
	safetyhook::InlineHook createSwapChainForHwndHook;

	// Diagnostic for the E_ABORT Present crash: dumps the bytes actually sitting at each swapchain hook site,
	// plus which overlay/injector DLLs are loaded. Called either side of the restore in ~D3D12Hook, so a
	// third-party hook installed on top of ours is visible rather than inferred. See the call sites.
	void logSwapChainHookSiteBytes(const char* when);

	// Co-existing with other overlays. HCM patches the SHARED dxgi / D3D12Core functions, which RivaTuner, the
	// Steam overlay and Streamline patch as well - and RTSS re-hooks on a timer, so it can land ON TOP of us
	// after we install. Restoring our saved bytes in that state erases their hook and cuts the present chain,
	// which is the measured cause of "game dies on HCM close with 0x80004004 and no exception".
	//
	// So we snapshot what we wrote and compare before restoring. Order of these five matches the park order in
	// ~D3D12Hook; keep them in step.
	// 7 = the original five, plus the two swapchain-CREATION sites. ⚠ The creation sites MUST be in here:
	// RTSS and the Steam overlay both patch CreateSwapChainForHwnd, and restoring our saved bytes over
	// theirs at teardown is the measured "game dies on HCM close" failure. See hookSiteIsStillOurs.
	static constexpr int kHookSiteCount = 7;
	static constexpr int kHookSiteSnapshotBytes = 5;   // an E9 rel32 is the whole story

	struct HookSiteSnapshot
	{
		uint8_t* target = nullptr;
		uint8_t  bytes[kHookSiteSnapshotBytes]{};
		bool     valid = false;
	};
	HookSiteSnapshot mHookSites[kHookSiteCount];

	void snapshotHookSites();
	bool hookSiteIsStillOurs(int index) const;

	// Set when a hook had to be left installed inside a third party's chain, which means our detour code must
	// never be unmapped. Drives the PIN at the end of ~D3D12Hook.
	bool mMustStayResident = false;

	// ================================================================================================================
	// THE RE-HOOK WATCHDOG - because "hooks installed" is not the same as "frames arriving".
	//
	// MEASURED. Close HCM and reopen it against the SAME running game and the overlay never comes back, while
	// HCMExternal still reports "Internal connected" - truthfully, because the interproc link, checkpoint detours
	// and state hook all work fine. Comparing the two sessions' logs:
	//
	//                          first run    reopened run
	//     swapchain adopted        1             0
	//     Present detour fired     9             0
	//
	// The hooks install without error and harvest the identical addresses. Nothing ever calls them. This process
	// has FIVE other parties on the same functions - sl.interposer, nvngx_dlssg, GFSDK_Aftermath, RTSSHooks64 and
	// GameOverlayRenderer64 - and whoever ends up owning the outermost hook can dispatch through a pointer it
	// captured earlier rather than through the shared code we patched.
	//
	// A dead overlay that reports success is the worst outcome, so this watchdog notices and retries: if no
	// present has reached us while a game is running, tear the hooks down and install them again from a fresh
	// harvest. A racy or stale install is fixed by that. A structural one is not - and then it says so plainly
	// instead of leaving the user to guess, which is what happened here.
	// ================================================================================================================
	static inline std::atomic<uint64_t> mPresentDetourFires{ 0 };
	std::thread mWatchdogThread;
	std::atomic<bool> mWatchdogRunning{ false };
	int mRehookAttempts = 0;

	void startRehookWatchdog();
	void stopRehookWatchdog();
	bool reinstallSwapChainHooks();

	struct HookAddresses
	{
		void* present = nullptr;
		void* present1 = nullptr;         // optional
		void* resizeBuffers = nullptr;
		void* resizeBuffers1 = nullptr;   // optional
		void* executeCommandLists = nullptr;
		// ---- GROUND TRUTH. All optional; the statistic still runs if these cannot be harvested. ----
		void* createSwapChain = nullptr;             // IDXGIFactory::CreateSwapChain
		void* createSwapChainForHwnd = nullptr;      // IDXGIFactory2::CreateSwapChainForHwnd
		void* createSwapChainForComposition = nullptr;
	};
	HookAddresses harvestHookAddresses(); // throws HCMInitException if it cannot get the required three
	void** resolveLiveSwapChainVtable();  // optional PointerDataStore fast path; nullptr if unavailable

	// The addresses harvested by the last successful (re)hook. Kept so the OBS bypass can name the
	// dxgi entry points whose Detours trampolines it is hunting for, without paying for another
	// harvest (which creates a device, a window and a swapchain just to read two vtable slots).
	HookAddresses mHarvestedAddresses{};

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

	// ⚠⚠ NEVER HAND mCommandQueue TO IMGUI. ImGui_ImplDX12_CreateFontsTexture submits its one-shot font upload
	// to InitInfo.CommandQueue, signals a fence ON THAT QUEUE, then does WaitForSingleObject(event, INFINITE) -
	// and it does all of that from inside our Present detour, BEFORE we forward Present. So if the game's queue
	// cannot retire at that instant, the game's own presenting thread never returns: picture frozen, overlay
	// never drawn, no exception, no crash dialog, and the window still reports Responding because a DIFFERENT
	// thread owns the message pump. Measured 7 times across 242 D3D12 sessions, first seen 2026-08-07.
	//
	// This queue is ours alone, so nothing can be queued ahead of that upload and the wait is always
	// satisfiable. imgui's own legacy Init overload creates exactly this (DIRECT, NodeMask 1) for the same
	// reason. ⚠ imgui stores it RAW and never AddRefs it - bd->commandQueueOwned is only set by that legacy
	// overload - so it must outlive ImGui_ImplDX12_Shutdown: released in releaseImGuiBoundResources next to
	// mSrvHeap, NOT in releaseD3Dresources.
	ID3D12CommandQueue* mFontUploadQueue = nullptr; // ours exclusively

	// ================================================================================================
	// THE RENDER GATE. ⚠⚠ THIS IS WHAT STOPS HCM HANGING THE GPU.
	//
	// MEASURED across 8 instrumented sessions on this title: "QUEUE PROBE FAILED" predicted a GPU hang
	// 3 times out of 3, and its absence predicted a clean session 4 times out of 4. Zero exceptions.
	// The probe was ALREADY detecting the fatal condition - and then the code submitted on that queue
	// anyway. This flag makes the probe AUTHORITATIVE instead of advisory.
	//
	// WHY A FAILED PROBE IS FATAL RATHER THAN COSMETIC. A Signal on an IDLE D3D12 queue retires
	// immediately, so "did not retire a fence in 250 ms" does NOT mean "foreign and quiet" - it means
	// BUSY OR BLOCKED: there is work ahead of our signal that is not completing. HCM's overlay command
	// list carries back-buffer state transitions (PRESENT->RENDER_TARGET, draw, RENDER_TARGET->PRESENT).
	// Submitting those on a queue that is not ordered against the queue the frame was actually built on
	// leaves two unsynchronised timelines transitioning the SAME back-buffer subresource. That is
	// undefined in D3D12, and on NVIDIA it presents as the graphics queue ceasing to retire - which is
	// exactly the captured breadcrumb: every named UE pass finished, then the frame-tail submission of
	// frame 125027 never completed, and UE synthesised GPUCrash 0x00008000 with no CPU fault.
	//
	// ⚠ WHY THE WRONG QUEUE GETS PICKED AT ALL: the candidate is whatever DIRECT queue submitted last
	// inside a ~1 ms window at attach, and the only filter is "is it on the game's device". With RTSS,
	// the Steam overlay, OBS (which brings d3d11on12) and Streamline/DLSS-G all resident, there are
	// four to six DIRECT queues on that device and every one of them passes. It is a coin flip, which
	// is precisely why the hang is intermittent.
	//
	// FAILING SAFE IS THE POINT: a session with no overlay is recoverable - the logs show every failed
	// attach was followed by a relaunch that worked - whereas a hung GPU costs the whole session.
	// The state machine that enforces this is the RenderGate / candidate table declared below.

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

	// ================================================================================================
	// THE CANDIDATE TABLE. Replaces the single mCandidateDirectQueue slot above for SELECTION - that
	// slot held whatever DIRECT queue submitted last inside a ~1 ms window at attach, and the only
	// filter was "is it on the game's device". With RTSS, the Steam overlay, OBS (which brings
	// d3d11on12) and Streamline/DLSS-G all resident there are FOUR TO SIX DIRECT queues on that device
	// and every one of them passes. Measured: the window between "recorded a candidate" and "captured"
	// was 1 ms. That coin flip is why the hang was intermittent - 3 of 8 sessions.
	//
	// ⚠⚠ THE RANKING BELOW IS A HEURISTIC AND IS NEVER TRUSTED. It decides only the ORDER in which
	// candidates are PROVEN. Nothing is ever drawn on a queue that has not retired a fence we signalled
	// on it. That is what makes this deterministic instead of a better guess, and vendor-agnostic: a
	// bad candidate is handled identically whether it belongs to Streamline, RTSS, OBS, or is the
	// game's own queue momentarily wedged behind frame-generation pacing.
	static constexpr uint32_t kMaxQueueCandidates    = 16;
	// ⚠ 8 WAS NOISE-LIMITED AND IT COST US THREE GPU HANGS. Interval coverage is the only signal here with
	// a causal story - a queue that misses a present interval cannot be the one presenting - but over 8
	// samples the render queue ALIASES to full coverage often enough to matter: across 11 logged sessions
	// it hit 8/8 three times (and 7/8 once, one interval from the same outcome), and those three are
	// exactly the three that hung. 32 buys resolution for ~0.1 s of extra attach latency.
	static constexpr uint32_t kQueueObserveIntervals = 32;  // presents to watch before ranking anything
	static constexpr uint32_t kQueueProofPresents    = 8;   // presents a candidate gets to retire the proof
	static constexpr uint32_t kQueueRecheckPresents  = 600; // ~10 s at 60 fps: re-prove the live queue

	struct QueueCandidate
	{
		ID3D12CommandQueue* queue = nullptr;   // OWNED reference, held until releaseQueueTable()
		uint64_t totalSubmissions = 0;
		uint32_t intervalsSeenIn = 0;          // present intervals it submitted in at least once
		uint32_t submissionsThisInterval = 0;
		bool rejected = false;
		const char* rejectReason = nullptr;
		bool verified = false;
	};
	static inline std::mutex mQueueTableMutex{};
	static inline QueueCandidate mQueueTable[kMaxQueueCandidates]{};
	static inline uint32_t mQueueTableCount = 0;
	static inline std::atomic_uint32_t mPresentIntervalsObserved{ 0 };
	// Once live, the ECL detour drops to one relaxed load and a pointer compare instead of a mutex and
	// a virtual GetDesc() on every submission in the process.
	static inline std::atomic<ID3D12CommandQueue*> mLiveQueueRaw{ nullptr };

	// THE RENDER GATE. See the measurement note above the candidate table.
	enum class RenderGate : uint32_t
	{
		Discovering = 0, // watching presents and submissions; NOT drawing
		Verifying,       // a candidate is chosen, its liveness proof is in flight; NOT drawing
		Live,            // proven; drawing
		Degraded,        // was live, stopped retiring; NOT drawing, re-verifying
		Refused          // no safe position could be established; will never draw this session
	};
	static inline std::atomic<RenderGate> mRenderGate{ RenderGate::Discovering };

	// Liveness proof. mProofFence is OURS and separate from mFence, so a proof can run against a queue
	// we have not adopted and can never be confused with an overlay submission.
	ID3D12Fence* mProofFence = nullptr;
	ID3D12CommandQueue* mProofQueue = nullptr;   // NOT owned - the table owns the reference
	UINT64 mProofValue = 0;
	UINT64 mProofNextValue = 0;
	uint32_t mProofPresentsWaited = 0;
	const char* mProofHow = "";
	bool mProofIsRevalidation = false;

	static const char* renderGateName(RenderGate g);
	void setRenderGate(RenderGate to, const char* why);
	void refuseToDraw(const char* why);
	static void recordQueueSubmission(ID3D12CommandQueue* q);
	static bool updateRenderGate();   // once per present on the adopted swapchain. NEVER blocks.
	bool beginQueueVerification();
	bool pollQueueVerification();
	void pollLiveQueueHealth();
	void beginProof(ID3D12CommandQueue* q, const char* how, bool isRevalidation);
	void adoptVerifiedQueue(ID3D12CommandQueue* q);
	void rejectQueue(ID3D12CommandQueue* q, const char* why);
	bool queueIsRejected(ID3D12CommandQueue* q);
	void logQueueTable(const char* when);
	void releaseQueueTable();

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

	// The OBS bypass, D3D12 edition. OBS does NOT have a separate "D3D12 hook" to defeat: it selects
	// d3d11 / d3d10 / d3d12_capture behind the SAME Present hook, so this is the same trick the D3D11
	// path uses - inline-hook whatever OBS's RealPresent / RealPresent1 pointers hold, so our overlay
	// renders after OBS's capture. The two riders are that UE5 presents through Present1 (so we need
	// RealPresent1 as well, which has its own signature), and that our own dxgi hooks sit on the same
	// bytes - see the mOBSPresentHook block above for how double-rendering is prevented.
	//
	// Throws HCMRuntimeException if the bypass was asked for and could not be set up. Returns
	// DeferredModuleNotLoaded when OBS Game Capture simply isn't in the process yet, which is a normal
	// state and not a failure. Notably it does NOT touch Lapua::lapuaGood.
	OBSBypassResult setOBSBypass(bool enabled);

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
