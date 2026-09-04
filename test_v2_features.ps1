$ErrorActionPreference = "Continue"

Write-Host "================================================================"
Write-Host "   TESTING PROXY CLIENT V2 (SMART ROUTING & PROXY POOL)        "
Write-Host "================================================================"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$PSScriptRoot\build\Release\proxy_client.exe"
$psi.Arguments = "-c $PSScriptRoot\config.json -p 10890 -d"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 2

if ($proc.HasExited) {
    Write-Host "[ERROR] Process failed to start."
    exit 1
}

Write-Host "`n1. Testing Smart Routing: PROXY rule (External IP check)..."
$proxyResult = curl.exe -s --max-time 15 -x "http://127.0.0.1:10890" "http://ip-api.com/json"
Write-Host "   Proxy Route Response: $proxyResult"

Write-Host "`n2. Executing CLI commands: nodes, rules, check, quit..."
$proc.StandardInput.WriteLine("nodes")
Start-Sleep -Milliseconds 300
$proc.StandardInput.WriteLine("rules")
Start-Sleep -Milliseconds 300
$proc.StandardInput.WriteLine("check")
Start-Sleep -Milliseconds 500
$proc.StandardInput.WriteLine("quit")
$proc.WaitForExit(3000)

Write-Host "`n--- PROXY CONSOLE OUTPUT ---"
$logs = $proc.StandardOutput.ReadToEnd()
Write-Host $logs

Write-Host "================================================================"
Write-Host "                 V2 FEATURE TESTS COMPLETED                     "
Write-Host "================================================================"
