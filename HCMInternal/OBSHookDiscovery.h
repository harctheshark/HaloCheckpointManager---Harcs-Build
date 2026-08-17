#pragma once
#include "pch.h"
#include <atomic>
#include "MultilevelPointer.h"
#include "IMessagesGUI.h"

// ====================================================================================================================
// FINDING OBS'S "RealPresent" AT RUNTIME, INSTEAD OF HARD-CODING ITS OFFSET.
//
// What the OBS bypass actually is
// -------------------------------
// OBS's graphics-hook64.dll Detours-hooks IDXGISwapChain::Present inside dxgi.dll. Its replacement
// (hook_present) does:
//
//      data.capture(backbuffer)      <- the clean frame OBS records
//      RealPresent(...)              <- Detours' trampoline back into dxgi
//
// where RealPresent is a plain GLOBAL FUNCTION POINTER VARIABLE in graphics-hook64's .data, which
// DetourAttach() rewrote in place to point at the trampoline it allocated.
//
// HCM's "Bypass OBS Capture" inline-hooks the ADDRESS THAT VARIABLE HOLDS. That puts HCM's overlay
// render strictly AFTER data.capture() has run, so the recording is clean and the streamer's local
// screen still shows the overlay. Nothing about that is D3D11-specific: OBS picks
// d3d11/d3d10/d3d12_capture behind this same Present hook, so the identical trick works for the
// D3D12 game (which additionally presents through Present1, hence RealPresent1).
//
// Why this file exists
// --------------------
// Until now the address of that variable was a hard-coded RVA in InternalPointerData.xml
// (dxgiInternalPresentFunction, 0x40EC0). That number is from an OBS build several years old. In OBS
// 32.2.1 the same RVA lands in .rdata and holds something that is not a pointer at all, so the hook
// silently failed to attach - ModuleInlineHook::attach() only PLOG_ERROR'd - and the toggle lit up
// while doing nothing at all. A new hard-coded number would rot again on the next OBS release, so
// instead we find the variable by what it IS rather than by where it was:
//
//      it is an 8-byte-aligned qword, in a writable initialised-data section of graphics-hook64.dll,
//      whose value points at executable memory OUTSIDE dxgi.dll, whose first few instructions are a
//      jump that lands back inside the first 0x20 bytes of dxgi's Present entry point.
//
// That last clause is the whole identification: only a Detours trampoline for THAT function looks
// like that. Detours relocates at most ~20 bytes of the prologue, so its jump-back always targets
// [entry, entry+0x20).
//
// Discovery is re-run on every enable (the .data section is ~8 KB, ~1000 candidates, microseconds),
// and OBSRealFnPointer re-runs it on every resolve() - which is what keeps HCM's existing
// "re-attach automatically when graphics-hook64.dll is loaded" behaviour working when the user
// starts a Game Capture source after HCM.
// ====================================================================================================================

enum class OBSDiscoveryStatus
{
	Ok,				// found it; rvaOut is the RVA of the VARIABLE (not its value)
	ModuleNotLoaded,	// graphics-hook64.dll isn't in the process. NOT an error - OBS just isn't capturing yet.
	NoDetourFound	// the module IS loaded but nothing in it looks like a trampoline pointer for this function
};

// What setOBSBypass(true) ended up doing. Anything worse than this throws.
enum class OBSBypassResult
{
	Installed,				// hook is live right now
	DeferredModuleNotLoaded,	// graphics-hook64.dll isn't loaded; the hook is registered and will attach on LoadLibrary
	Removed					// setOBSBypass(false)
};

// Everything the log needs to make a failed discovery diagnosable from a user's log file alone.
struct OBSDiscoveryDiagnostics
{
	uintptr_t moduleBase = 0;
	size_t moduleSize = 0;
	std::string sectionsScanned;	// e.g. ".data(0x1fe8) .didat(0x28)"
	size_t qwordsScanned = 0;		// every aligned qword we looked at
	size_t candidatesProbed = 0;	// ...that survived the cheap filters and got a VirtualQuery
	uintptr_t dxgiEntry = 0;
};

// Scans graphics-hook64.dll's writable data sections for the Detours trampoline pointer belonging to
// the dxgi function whose entry point is dxgiEntry. On success rvaOut receives the RVA OF THE
// VARIABLE - deliberately not its value, so callers re-read it every time and survive OBS
// re-attaching its hook.
OBSDiscoveryStatus discoverRealFnRva(uintptr_t dxgiEntry, uintptr_t& rvaOut, OBSDiscoveryDiagnostics* diagnosticsOut = nullptr);

// True if the bytes at dxgiEntry look like SOMEBODY has inline-hooked it (jmp rel32 / jmp rel8 /
// jmp [rip+disp32]). Used only to enrich the failure log - HCM's own D3D12 hook sits on the same
// bytes, so on that path a "yes" proves nothing about OBS.
bool looksInlineHooked(uintptr_t functionEntry);


// ====================================================================================================================
// WHO IS OUTERMOST ON dxgi's Present - and the fix for "OBS never captures HCM at all" on D3D12.
//
// On MCC (D3D11) HCM's own present hook is a VMT hook on the game's swapchain object, which sits
// unconditionally OUTSIDE any inline detour on dxgi's shared body. OBS therefore always captures a frame
// that already contains the overlay, whoever installed first.
//
// On HaloCER (D3D12) HCM inline-patches the very bytes OBS Detours. Order is decided purely by who
// installed LAST, and OBS's graphics-hook64.dll injects when a Game Capture source starts - normally after
// HCM is already open. OBS then owns the entry point, so its hook_present runs FIRST:
//
//      dxgi Present entry -> OBS hook_present -> data.capture()   <- no overlay in the recording
//                                             -> RealPresent      -> HCM's detour -> HCM draws
//
// which is exactly the reported symptom, and it is independent of the bypass toggle.
//
// This function reports who currently owns the entry point by decoding the jump that sits there and asking
// which module it lands in. `landsInModule` is the module the jump target belongs to, so the caller can
// tell "OBS is outermost" (graphics-hook64.dll) from "we are outermost" (HCMInternal) from "a third
// overlay got there" (anything else) - three cases that need three different responses.
bool followEntryDetour(uintptr_t functionEntry, uintptr_t& targetOut, std::wstring& landsInModuleOut);

// Where OBSRealFnPointer sends its user-facing complaints. Set once, from OBSBypassManager.
// It exists because the deferred re-attach happens on the LoadLibrary detour's thread, deep inside
// ModuleInlineHook::attach(), with no service graph in reach and no way to throw.
void setOBSDiscoveryMessageSink(std::weak_ptr<IMessagesGUI> sink);


// A MultilevelPointer whose resolve() runs discovery and then dereferences the variable it found -
// i.e. it yields the address of the Detours trampoline, which is what we inline-hook.
//
// Why a MultilevelPointer at all: ModuleInlineHook::attach() takes one and re-resolves it on EVERY
// attach, including the automatic re-attach that ModuleHookManager performs when graphics-hook64.dll
// is loaded. Expressing discovery as a pointer is what makes "enable the toggle before OBS is even
// running, and have it engage by itself later" keep working.
class OBSRealFnPointer : public MultilevelPointer
{
private:
	uintptr_t mDxgiEntry;
	const char* mFunctionName;	// "Present" / "Present1", for logs only
	// Discovery is retried on every attach; without this a user whose OBS version we cannot read
	// would get the same on-screen complaint every time any DLL loads.
	mutable std::atomic_bool mReportedFailure{ false };
	// Called (once) when discovery fails on a DEFERRED attach, so the owner can stop wanting to be
	// attached. Cannot throw from there - we are inside LoadLibrary.
	std::function<void()> mOnPermanentFailure;

public:
	explicit OBSRealFnPointer(uintptr_t dxgiEntry, const char* functionName)
		: MultilevelPointer(0), mDxgiEntry(dxgiEntry), mFunctionName(functionName)
	{
	}

	bool resolve(uintptr_t* resolvedOut) const override;

	void setOnPermanentFailure(std::function<void()> callback) { mOnPermanentFailure = std::move(callback); }
	uintptr_t getDxgiEntry() const { return mDxgiEntry; }
};


// The PRE-CAPTURE counterpart of OBSRealFnPointer, and the exact mirror of it.
//
//   OBSRealFnPointer  -> the trampoline OBS calls AFTER data.capture()  -> drawing there is INVISIBLE to OBS
//   OBSHookEntryPointer -> the entry of OBS's hook_present, BEFORE data.capture() -> drawing there is CAPTURED
//
// Which one HCM installs is what the "Bypass OBS Capture" toggle should actually select on D3D12, because
// hook order alone otherwise decides it and the user has no say (see followEntryDetour's comment).
//
// Resolution re-runs on every attach rather than caching, for the same reason the sibling does:
// graphics-hook64.dll comes and goes with the Game Capture source, and its hook can be re-applied at a
// different address. It refuses to resolve unless the jump at the dxgi entry lands inside
// graphics-hook64.dll - if HCM is already outermost, or some third overlay is, there is nothing here to
// hook and pretending otherwise would put the overlay somewhere arbitrary.
class OBSHookEntryPointer : public MultilevelPointer
{
private:
	uintptr_t mDxgiEntry;
	const char* mFunctionName;	// "Present" / "Present1", for logs only
	mutable std::atomic_bool mReportedFailure{ false };

public:
	explicit OBSHookEntryPointer(uintptr_t dxgiEntry, const char* functionName)
		: MultilevelPointer(0), mDxgiEntry(dxgiEntry), mFunctionName(functionName)
	{
	}

	bool resolve(uintptr_t* resolvedOut) const override;

	uintptr_t getDxgiEntry() const { return mDxgiEntry; }
};
