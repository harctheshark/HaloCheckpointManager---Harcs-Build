// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <fstream>
#include <string>
#include <atomic>   // gSessionRunning; not in pch.h
#include "WindowsUtilities.h"

#include "App.h"
#include "MCCInitialisationCheck.h"
#include "ImageResidencyGuard.h"
#include "ImGuiManager.h"


// Main Execution Loop
void RealMain(HMODULE dllHandle)
{
    App app = App(dllHandle); // app blocks at the end of it's constructor until it's kill condition is met
}

// ================================================================================================
// HCM IS PERMANENTLY RESIDENT, AND A SESSION IS RE-RUNNABLE.
//
// This DLL used to FreeLibraryAndExitThread on the way out, and pin itself only in the cases where
// unmapping would have been unsafe (a thread still inside our image, a WndProc left subclassed, or
// renderer hooks another overlay had patched over). Pinning then meant HCM could not be injected
// again until the game restarted - which is how a perfectly correct decision ("do not erase RivaTuner's
// hook") turned into "close RivaTuner before using HCM".
//
// It no longer unmaps AT ALL. That is the same thing RTSS, the Steam overlay, Discord and OBS all do,
// and it is broadly why they coexist: the dangerous operation is REMOVING a hook from a shared function,
// because your saved "original" bytes are only original if nobody hooked over you. Unmapping was also the
// direct cause of a measured, repeating crash class (faulting module "HCMInternal.dll_unloaded",
// 0xC0000005, 23 recorded occurrences). Deleting the unmap deletes that entire class.
//
// ⚠ HCM is not yet fully innocent here: ModuleHookManager still restores five kernel32 hooks
// (LoadLibraryA/W/ExA/ExW, FreeLibrary) at every session end with no "is it still ours" check. That is a
// blind restore over any other injector's loader hook and it is now paid once per session. See N1 in the
// residency audit.
//
// ⚠ COST, ACCEPTED DELIBERATELY: every process-lifetime static in HCMInternal is now SESSION state.
// The audit found five that were broken by this (the present-detour counter, the plog appender, LOG_ONCE's
// guards, ModuleHookManager's map entries, mWndProcLeftInstalled) and did not sweep exhaustively.
// "Does this static need resetting per session?" is now a permanent review question.
//
// ⚠ AND: the DLL is locked on disk for the life of the GAME process, not of HCM. Rebuilding HCMInternal
// while the game is running now fails with LNK1104 permanently rather than transiently - close the GAME,
// not just HCM, before building.
//
// Closing HCM therefore ends the SESSION, not the module: App is destroyed, hooks we still own are
// parked, hooks somebody hooked over are left forwarding, and the image stays mapped. Re-opening HCM
// starts a new session in the SAME image through HCMInternalStartSession below - D3D12Hook::beginHook
// adopts whatever is still installed rather than hooking over itself.
//
// ★ There is now ONE lifecycle instead of two. The old "unmap normally, pin in the awkward cases" split
// meant the coexistence path was the rare one, and rare paths are where the bugs live.
// ================================================================================================

static std::atomic<bool> gSessionRunning{ false };
static HMODULE gSelfModule = nullptr;

// Keeps this image mapped for the life of the process. Safe to call repeatedly.
// ⚠ Every safety argument in this file is conditional on this succeeding, so it is checked. A silent
// miss would surface as an unattributed access violation inside the game long afterwards.
static bool pinSelf()
{
    HMODULE pinned = NULL;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        (LPCWSTR)&RealMain, &pinned) != FALSE;
}

// One HCM session. Returns when the session has ended; the module stays mapped either way.
// ⚠ Every exit path must go through the scope guard, so no ExitThread() in here - that would skip the
// destructor and leave gSessionRunning stuck true, locking out every future session.
DWORD WINAPI MainThread(HMODULE hDLL)
{
    if (hDLL) gSelfModule = hDLL; else hDLL = gSelfModule;
    if (!hDLL) return 0;

    if (!pinSelf())   // before anything installs a hook, so the image can never go away under one
    {
        PLOG_FATAL << "HCMInternal: could not pin the module. Refusing to install hooks that would outlive it.";
        return 0;
    }

    // ⚠ BOUNDED, AND OUTSIDE THE SESSION GUARD. This used to be an unbounded poll inside the guard; with
    // a resident module that is a permanent lockout, because nothing can ever LoadLibrary us again to
    // break the deadlock. On Halo Campaign Evolved there is no known init flag at all, so this loop can
    // never succeed there - it must be able to give up.
    {
        const ULONGLONG deadline = GetTickCount64() + 120000;
        auto mccIsInitialised = MCCInitialisationCheck(hDLL);
        while (mccIsInitialised.has_value() && mccIsInitialised.value() == false)
        {
            if (GetTickCount64() > deadline)
            {
                PLOG_WARNING << "HCMInternal: the game did not report initialisation within 120s; starting anyway";
                break;
            }
            Sleep(100);
            mccIsInitialised = MCCInitialisationCheck(hDLL);
        }
    }

    // ⚠ WAIT, DO NOT REFUSE. A previous session may still be unwinding - GPU flushes, ~200 cheat
    // destructors, the residency drain and a final Sleep - and HCMExternal publishes "shutting down"
    // BEFORE all of that, then re-injects within one ~1s tick. Refusing here surfaces to the user as
    // HookStateMachine's timeout, whose text tells them to fully close the game and relaunch: precisely
    // the outcome this whole change exists to delete. This runs on the injector's CreateRemoteThread
    // thread, which the injector deliberately does not wait on, so waiting costs it nothing.
    for (int waitedMs = 0; gSessionRunning.load(std::memory_order_acquire) && waitedMs < 15000; waitedMs += 50)
        Sleep(50);

    if (gSessionRunning.exchange(true, std::memory_order_acq_rel))
    {
        PLOG_ERROR << "HCMInternal: the previous session is still shutting down after 15s; refusing to start a second one";
        return 0;
    }
    // ⚠ NOTHING INSIDE THIS GUARD MAY BLOCK INDEFINITELY. If MainThread never returns, SessionGuard never
    // runs, gSessionRunning stays true, and every future session in this process is refused - with the
    // module resident, LoadLibrary can no longer rescue it. That is why the MessageBoxA in App.h had to
    // move off this thread.
    struct SessionGuard { ~SessionGuard() { gSessionRunning.store(false, std::memory_order_release); } } sessionGuard;

    // Re-arm every LOG_ONCE site for this session (they are per-callsite statics; see pch.h).
    LogOnceGeneration::newSession();

    RealMain(hDLL);

    // Everything this session owned has been destroyed. Threads that were already inside our code are
    // still draining - we wait for them not because the image might be unmapped (it will not be) but
    // because the NEXT session must not start installing hooks while stragglers are still running
    // through the old ones.
    if (!ImageResidency::drain(3000))
        PLOG_WARNING << "HCMInternal: a thread is still inside our image after 3s. The image stays mapped, so "
            "this is not a crash risk; the next session will adopt whatever is still installed.";

    if (ImGuiManager::wndProcWasLeftInstalled())
        PLOG_INFO << "HCMInternal: our window procedure was left installed because another subclass sits above "
            "ours. Removing it would break their chain, so it stays - the image is resident, so it remains valid.";

    // The drain cannot cover a thread's own "release the lock, then ret" epilogue - a few instructions it is
    // impossible to observe from outside. This short sleep covers that bounded window.
    Sleep(50);

    PLOG_INFO << "HCMInternal: session ended. The module stays resident; re-opening HCM starts a new session.";
    return 0;
}

// The injector calls this when HCMInternal is ALREADY loaded. LoadLibrary would only bump the reference
// count and never re-run DllMain, so without this a resident HCM could never be reopened - which was the
// whole "HCM cannot be injected again in this session" problem.
// ⚠ LPTHREAD_START_ROUTINE shape on purpose: it is invoked by CreateRemoteThread exactly like LoadLibraryA is.
extern "C" __declspec(dllexport) DWORD WINAPI HCMInternalStartSession(LPVOID)
{
    return MainThread(gSelfModule);
}


BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        gSelfModule = hModule;
        CreateThread(0, 0x1000, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, NULL);
    }

    return TRUE;

}
