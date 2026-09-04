@echo off
setlocal enabledelayedexpansion

echo ================================================================
echo                   Rift - Uninstaller
echo ================================================================

set "INSTALL_DIR=%LOCALAPPDATA%\Rift"

echo [INFO] Removing Rift from User PATH...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$installPath = [System.Environment]::ExpandEnvironmentVariables('%LOCALAPPDATA%\Rift');" ^
    "$legacyPath = [System.Environment]::ExpandEnvironmentVariables('%LOCALAPPDATA%\ProxyClient');" ^
    "$userPath = [System.Environment]::GetEnvironmentVariable('Path', 'User');" ^
    "$paths = $userPath -split ';' | Where-Object { $_ -ne '' -and $_ -ne $installPath -and $_ -ne $legacyPath };" ^
    "$newPath = $paths -join ';';" ^
    "[System.Environment]::SetEnvironmentVariable('Path', $newPath, 'User');" ^
    "Write-Host '[SUCCESS] Removed from User PATH.';"

if exist "%INSTALL_DIR%" (
    echo [INFO] Deleting installation directory: %INSTALL_DIR%
    rd /s /q "%INSTALL_DIR%"
)
if exist "%LOCALAPPDATA%\ProxyClient" (
    rd /s /q "%LOCALAPPDATA%\ProxyClient"
)

echo.
echo ================================================================
echo   UNINSTALLATION COMPLETED!
echo ================================================================
pause
