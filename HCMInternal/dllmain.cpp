// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <fstream>
#include <string>
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

// This thread is created by the dll when loaded into the process, see RealMain() for the actual event loop.
// Do NOT put any allocations in this function because the call to FreeLibraryAndExitThread()
// will occur before they fall out of scope and will not be cleaned up properly! This is very
// important for being able to hotload the DLL multiple times without restarting the game.
DWORD WINAPI MainThread(HMODULE hDLL)
{


    auto mccIsInitialised = MCCInitialisationCheck(hDLL);
    while (mccIsInitialised.has_value() && mccIsInitialised.value() == false)
    {
        Sleep(100);
        mccIsInitialised = MCCInitialisationCheck(hDLL);
    }

    RealMain(hDLL);
    // Everything HCM owns has now been destroyed and every hook has been removed, so no NEW thread can enter
    // our code. What remains is threads that were already inside one.

    // ⚠ THIS REPLACED A BARE Sleep(200), WHICH WAS A GUESS. See ImageResidencyGuard.h: unmapping the image
    // while a thread is executing in it is a measured, repeating crash on this machine (faulting module
    // "HCMInternal.dll_unloaded", 0xC0000005, 23 recorded occurrences across HaloCER and MCC). The drain
    // waits for the guarded entry points to actually empty instead of hoping 200ms was enough.
    if (!ImageResidency::drain(3000))
    {
        // A detour is wedged. Unmapping now is a coin flip on the game's life, so do not: pin the module and
        // leave. A leaked DLL is survivable; an access violation in the user's game is not. Loud, because it
        // also means HCM cannot be cleanly reopened against this game session.
        PLOG_FATAL << "HCMInternal: a thread is STILL inside our image after 3s. Pinning the module rather "
            "than unmapping code that is executing - the game would otherwise crash. HCM will need the game "
            "restarted before it can be injected again.";
        HMODULE pinned = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)&MainThread, &pinned);
        ExitThread(0);
    }

    // Same reasoning for the WndProc: if something subclassed after us we deliberately left our proc
    // installed rather than corrupt their chain, so the image must stay resident under it.
    if (ImGuiManager::wndProcWasLeftInstalled())
    {
        PLOG_FATAL << "HCMInternal: our window procedure was left installed (another subclass sits above "
            "ours), so the image cannot be unmapped. Pinning. The game must be restarted before HCM can be "
            "injected again.";
        HMODULE pinned = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)&MainThread, &pinned);
        ExitThread(0);
    }

    // The drain cannot cover a thread's own "release the lock, then ret" epilogue - a few instructions it is
    // impossible to observe from outside. This short sleep covers that bounded window; the drain above is
    // what removed the UNBOUNDED one.
    Sleep(50);
    FreeLibraryAndExitThread(hDLL, NULL);
}


BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        CreateThread(0, 0x1000, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, NULL);
    }

    return TRUE;

}

