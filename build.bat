@echo off
setlocal enabledelayedexpansion

:: Detection of VsDevCmd.bat
set "VS_DEV_CMD="

:: Check if already in a Dev Command Prompt
if defined VCINSTALLDIR (
    echo [INFO] Already in a Developer Command Prompt.
) else (
    :: Common paths for VS 2026
    set "PATHS[0]=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
    set "PATHS[1]=C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat"
    set "PATHS[2]=C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat"
    set "PATHS[3]=C:\Program Files\Microsoft Visual Studio\18\Preview\Common7\Tools\VsDevCmd.bat"

    for /L %%i in (0,1,3) do (
        if not defined VS_DEV_CMD (
            if exist "!PATHS[%%i]!" set "VS_DEV_CMD=!PATHS[%%i]!"
        )
    )

    if not defined VS_DEV_CMD (
        echo [ERROR] VsDevCmd.bat not found. 
        echo Please edit build.bat with your VS installation path or run from Developer Command Prompt.
        exit /b 1
    )

    echo [INFO] Found VsDevCmd.bat at: !VS_DEV_CMD!
    call "!VS_DEV_CMD!" -arch=x64
)

:: Check if cl.exe is in PATH
where cl.exe >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [!] Error: cl.exe not found. 
    echo Please run this script from a 'Developer Command Prompt for VS'.
    exit /b 1
)

if not exist "build" mkdir build

cl /nologo /O2 /MD /EHsc /LD ^
    /I "deps/imgui" ^
    /I "deps/imgui/backends" ^
    /I "deps/IL2CPP_Resolver" ^
    /I "deps/minhook/include" ^
    /D "IMGUI_IMPL_WIN32_DISABLE_GAMEPAD" ^
    src/main.cpp ^
    deps/imgui/imgui.cpp ^
    deps/imgui/imgui_draw.cpp ^
    deps/imgui/imgui_widgets.cpp ^
    deps/imgui/imgui_tables.cpp ^
    deps/imgui/imgui_demo.cpp ^
    deps/imgui/backends/imgui_impl_dx11.cpp ^
    deps/imgui/backends/imgui_impl_win32.cpp ^
    deps/minhook/src/buffer.c ^
    deps/minhook/src/hook.c ^
    deps/minhook/src/trampoline.c ^
    deps/minhook/src/hde/hde64.c ^
    deps/minhook/src/hde/hde32.c ^
    /Fe:build/fubuki_tld.dll ^
    d3d11.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib

if %ERRORLEVEL% equ 0 (
    echo Success: build/fubuki_tld.dll created.
) else (
    echo Failure: Build failed with error %ERRORLEVEL%.
)

endlocal
