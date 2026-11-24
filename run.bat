@echo off
rem ASTRO CTR: build the latest source and start the app from run\ (always the newest version).
rem Needs MSYS2 UCRT64 (g++, cmake, ninja) in C:\msys64.
setlocal
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
set "OUT=%~dp0run"

tasklist /FI "IMAGENAME eq ASTRO_CTR.exe" | find /I "ASTRO_CTR.exe" >nul
if not errorlevel 1 (
    echo ASTRO is already open. Close the ASTRO window, then run this again.
    pause
    exit /b 1
)

if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :fail
ninja -C build astro_app astro_ctr astro_v2 || goto :fail

if not exist "%OUT%" mkdir "%OUT%"
copy /y build\ASTRO_CTR.exe "%OUT%\" >nul || goto :fail
copy /y build\ASTROCTRV2.exe "%OUT%\" >nul
copy /y build\VkLayer_ASTRO_ctr_implicit.json "%OUT%\" >nul
copy /y src\layer\vfx_catalog.json "%OUT%\" >nul
copy /y data\wheel_catalog.json "%OUT%\" >nul
copy /y data\sky_catalog.json "%OUT%\" >nul
copy /y data\snow_catalog.json "%OUT%\" >nul
copy /y data\pad_catalog.json "%OUT%\" >nul
copy /y data\lava_catalog.json "%OUT%\" >nul
copy /y data\roo_catalog.json "%OUT%\" >nul
copy /y data\driver_catalog.json "%OUT%\" >nul
copy /y data\items_catalog.json "%OUT%\" >nul
copy /y data\mesh_catalog.json "%OUT%\" >nul
copy /y data\gamepad_sheet.png "%OUT%\" >nul
copy /y data\gamepad_sheet.rgba "%OUT%\" >nul
xcopy /y /q /i data\wheels "%OUT%\wheels" >nul
rem the game keeps astro_ctr.dll open while it runs: then the new one waits as astro_ctr.new.dll and the app
rem swaps it in as soon as the game is closed
copy /y build\astro_ctr.dll "%OUT%\" >nul 2>&1
if errorlevel 1 (
    copy /y build\astro_ctr.dll "%OUT%\astro_ctr.new.dll" >nul
    echo The game is running: the new DLL is used from the next game start.
)

echo Starting ASTRO CTR from %OUT%
start "" "%OUT%\ASTRO_CTR.exe"
exit /b 0

:fail
echo.
echo BUILD FAILED - see the messages above.
pause
exit /b 1
