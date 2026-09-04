$ErrorActionPreference = "Continue"

Write-Host "========================================"
Write-Host "   PROXY CLIENT AUTOMATED TEST SUITE    "
Write-Host "========================================"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$PSScriptRoot\build\Release\proxy_client.exe"
$psi.Arguments = "-p 10880 -d"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 2

if ($proc.HasExited) {
    Write-Host "[ERROR] Process exited prematurely."
    exit 1
}

Write-Host "[1/4] Probing SOCKS5 HTTPS tunnel (1.1.1.1:443)..."
$socks5Result = curl.exe -s --max-time 10 -x "socks5h://127.0.0.1:10880" "https://1.1.1.1/cdn-cgi/trace"
if ($socks5Result -match "colo=") {
    Write-Host "[PASS] SOCKS5 connection established successfully."
} else {
    Write-Host "[FAIL] SOCKS5 connection failed."
}

Write-Host "[2/4] Probing HTTP CONNECT HTTPS tunnel (1.1.1.1:443)..."
$httpResult = curl.exe -s --max-time 10 -x "http://127.0.0.1:10880" "https://1.1.1.1/cdn-cgi/trace"
if ($httpResult -match "colo=") {
    Write-Host "[PASS] HTTP CONNECT connection established successfully."
} else {
    Write-Host "[FAIL] HTTP CONNECT connection failed."
}

Write-Host "[3/4] Probing unreachable endpoint to verify timeout diagnostics (192.0.2.1:80)..."
$failResult = curl.exe -s --max-time 3 -x "http://127.0.0.1:10880" "http://192.0.2.1:80"

Write-Host "[4/4] Executing CLI inspection commands (status, stats, conns, quit)..."
$proc.StandardInput.WriteLine("status")
Start-Sleep -Milliseconds 200
$proc.StandardInput.WriteLine("stats")
Start-Sleep -Milliseconds 200
$proc.StandardInput.WriteLine("conns")
Start-Sleep -Milliseconds 200
$proc.StandardInput.WriteLine("quit")
$proc.WaitForExit(3000)

Write-Host "`n--- PROXY CONSOLE OUTPUT ---"
$logs = $proc.StandardOutput.ReadToEnd()
Write-Host $logs

Write-Host "========================================"
Write-Host "         TEST SUITE FINISHED            "
Write-Host "========================================"
