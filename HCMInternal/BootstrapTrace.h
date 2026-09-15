#pragma once
#include <Windows.h>

// ================================================================================================
// BOOTSTRAP TRACE - the only diagnostic available before plog exists.
//
// ⚠ WHY THIS IS NEEDED AT ALL. Everything before App's logging->initFileLogging() is invisible,
// because the log directory itself comes out of shared memory. In a normal host that is survivable:
// if shared memory fails, App pops a MessageBox. In an APPCONTAINER host (Halo 5: Forge is a UWP
// title) it is not - a sandboxed process generally cannot put a window on the interactive desktop,
// so that MessageBox never appears. The failure is then perfectly silent: the injector reports
// success, nothing initialises, and there is no log and no dialog to read.
//
// ⚠⚠ WHERE IT WRITES, AND WHY NOT SOMEWHERE OBVIOUS. An AppContainer cannot write to an arbitrary
// path just because it is world-writable - it needs an explicit ACE for its own package SID or for
// ALL APPLICATION PACKAGES (S-1-15-2-1). C:\Users\Public looked like the safe choice and is NOT:
// it carries no AppContainer ACE, so a trace written there silently does nothing, which is a
// diagnostic that lies to you. So this writes next to the DLL, in the Logs folder HCM already
// requires write access to.
//
// The path is derived from OUR OWN MODULE, not from shared memory - that is the whole point, since
// shared memory is one of the things being diagnosed.
// ================================================================================================
inline void bootstrapTrace(const char* step)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&bootstrapTrace, &self) || !self)
        return;

    char path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    // Truncate to the directory, then append the Logs subfolder.
    char* lastSlash = nullptr;
    for (char* p = path; *p; ++p) if (*p == '\\') lastSlash = p;
    if (!lastSlash) return;
    *(lastSlash + 1) = '\0';

    char full[MAX_PATH]{};
    if (wsprintfA(full, "%sLogs\\HCM_bootstrap.txt", path) <= 0) return;

    HANDLE h = CreateFileA(full, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char line[1024];
    const int n = wsprintfA(line, "[pid %lu] %s\r\n", GetCurrentProcessId(), step);
    DWORD written = 0;
    if (n > 0) WriteFile(h, line, (DWORD)n, &written, nullptr);
    CloseHandle(h);
}
