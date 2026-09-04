@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo             Building Rift (C++20)
echo ===================================================

:: Look for Visual Studio / Build Tools (including Preview/Insiders)
set "VS_DIR="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -property installationPath 2^>nul`) do (
    set "VS_DIR=%%i"
)

if not defined VS_DIR (
    if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders" (
        set "VS_DIR=C:\Program Files\Microsoft Visual Studio\18\Insiders"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community" (
        set "VS_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional" (
        set "VS_DIR=C:\Program Files\Microsoft Visual Studio\2022\Professional"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise" (
        set "VS_DIR=C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    )
)

if not defined VS_DIR (
    echo [ERROR] Visual Studio / MSVC Build Tools not found!
    pause
    exit /b 1
)

echo Found Visual Studio at: %VS_DIR%

:: Setup PATH to include VS bundled CMake if not in system PATH
if exist "%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" (
    set "PATH=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
)

call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Failed to initialize MSVC x64 environment!
    pause
    exit /b 1
)

if not exist build mkdir build
cd build

cmake .. -G "Visual Studio 18 2026" -A x64
if %errorlevel% neq 0 (
    echo [WARNING] Defaulting to standard cmake generation...
    cmake ..
)

cmake --build . --config Release
if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo ===================================================
echo   Build Successful! Output in build/Release/
echo ===================================================
copy /y ..\config.json Release\config.json >nul 2>&1
cd ..
pause
