@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo   Building C++ Proxy Client
echo ===================================================

:: Look for Visual Studio / Build Tools
set "VS_DIR="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    set "VS_DIR=%%i"
)

if not defined VS_DIR (
    echo [ERROR] Visual Studio / MSVC Build Tools not found!
    pause
    exit /b 1
)

echo Found Visual Studio at: %VS_DIR%

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
