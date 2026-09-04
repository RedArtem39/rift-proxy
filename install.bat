@echo off
setlocal enabledelayedexpansion

echo ================================================================
echo             Rift - Automated Windows Installer
echo ================================================================

set "INSTALL_DIR=%LOCALAPPDATA%\Rift"

:: Locate binaries (standalone release package vs source build)
if exist "%~dp0rift.exe" (
    set "BIN_SOURCE=%~dp0rift.exe"
    set "CFG_SOURCE=%~dp0config.json"
    set "TOOL_SOURCE=%~dp0configrif.exe"
) else if exist "%~dp0build\Release\rift.exe" (
    set "BIN_SOURCE=%~dp0build\Release\rift.exe"
    set "CFG_SOURCE=%~dp0config.json"
    set "TOOL_SOURCE=%~dp0build\Release\configrif.exe"
) else (
    echo [INFO] Binaries not found. Starting automatic build with MSVC...
    call "%~dp0build.bat"
    set "BIN_SOURCE=%~dp0build\Release\rift.exe"
    set "CFG_SOURCE=%~dp0config.json"
    set "TOOL_SOURCE=%~dp0build\Release\configrif.exe"
    if not exist "!BIN_SOURCE!" (
        echo [ERROR] Build failed. Installation aborted.
        pause
        exit /b 1
    )
)

:: 2. Create target installation directory
echo [INFO] Creating directory: %INSTALL_DIR%
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

:: 3. Copy binary and config
echo [INFO] Copying executables and configuration...
copy /y "%BIN_SOURCE%" "%INSTALL_DIR%\rift.exe" >nul
copy /y "%BIN_SOURCE%" "%INSTALL_DIR%\rft.exe" >nul
if exist "%TOOL_SOURCE%" (
    copy /y "%TOOL_SOURCE%" "%INSTALL_DIR%\configrif.exe" >nul
)
if not exist "%INSTALL_DIR%\config.json" (
    copy /y "%CFG_SOURCE%" "%INSTALL_DIR%\config.json" >nul
)

:: 4. Add to User PATH via PowerShell (safe against 1024-character setx limits)
echo [INFO] Registering Rift in User PATH...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$installPath = [System.Environment]::ExpandEnvironmentVariables('%LOCALAPPDATA%\Rift');" ^
    "$userPath = [System.Environment]::GetEnvironmentVariable('Path', 'User');" ^
    "$paths = $userPath -split ';' | Where-Object { $_ -ne '' };" ^
    "if ($paths -notcontains $installPath) {" ^
    "    $newPath = ($paths + $installPath) -join ';';" ^
    "    [System.Environment]::SetEnvironmentVariable('Path', $newPath, 'User');" ^
    "    Write-Host '[SUCCESS] Added to User PATH successfully.';" ^
    "} else {" ^
    "    Write-Host '[INFO] Already present in User PATH.';" ^
    "}"

echo.
echo ================================================================
echo   INSTALLATION COMPLETED SUCCESSFULLY!
echo ================================================================
echo   Location: %INSTALL_DIR%
echo   Executables: rift.exe, rft.exe, configrif.exe
echo.
echo   You can now open any terminal (cmd / PowerShell) and use:
echo.
echo       rft status            - Show proxy status and traffic
echo       rft nodes             - List proxy pool latency
echo       rft switch 1          - Switch active proxy node
echo       rft sysproxy on       - Enable Windows system proxy
echo       rft run               - Start Rift server daemon
echo       configrif             - Interactive visual config manager
echo       rft help              - Show full command reference
echo.
pause
