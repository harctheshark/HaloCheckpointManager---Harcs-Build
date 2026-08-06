#pragma once
#include "WinHandle.h"
#include "SharedMemoryInternal.h"
#include "SettingsStateAndEvents.h"

// Defined in HeartbeatTimer.cpp. Returns 0 if no process by that name is running.
// Declared here because App.h needs it to decide whether showing a modal error dialog is safe: if HCMExternal
// is gone, this instance is an orphan and a modal box on the game's thread would block the DLL from unloading.
DWORD findProcess(std::wstring targetProcessName);

class HeartbeatTimer
{
private:
    std::weak_ptr<SharedMemoryInternal> sharedMemWeak;
    std::weak_ptr<SettingsStateAndEvents> settingsWeak;

public:
    explicit HeartbeatTimer(std::weak_ptr<SharedMemoryInternal>, std::weak_ptr<SettingsStateAndEvents>);

    ~HeartbeatTimer();



private:
    std::thread _thd;
    HandlePtr HCMExternalHandle;
};

