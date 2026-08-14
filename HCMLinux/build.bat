@echo off
REM Builds the HCM Linux launcher. Output MUST be named HCMExternal.exe - HCMInternal's
REM heartbeat searches for that process name and unloads itself if it is absent.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++20 /O2 /EHsc /W3 /DNDEBUG hcm_linux_launcher.cpp /Fe:HCMExternal.exe /link /SUBSYSTEM:CONSOLE
del *.obj 2>nul
