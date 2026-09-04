@echo off
REM Double-click me after Halo Campaign Evolved updates.
REM Finds the game, checks all of HCM's byte signatures against the new build, and writes
REM anchor_report.txt (what still works) and anchor_repair.txt (proposed fixes).
setlocal
cd /d "%~dp0"

where py >nul 2>&1
if errorlevel 1 (
    echo.
    echo Python is not installed. Get it from https://www.python.org/downloads/
    echo Tick "Add python.exe to PATH" in the installer.
    echo.
    pause
    exit /b 1
)

py hce_anchor_update.py %*
set RC=%ERRORLEVEL%

echo.
if %RC%==0 echo Nothing to do - every anchor still resolves on this build.
if %RC%==1 echo Some anchors need attention. Open anchor_repair.txt.
if %RC% GEQ 2 echo The tool could not run. Read the message above.
echo.
pause
