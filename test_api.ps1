$ErrorActionPreference = "Continue"

Write-Host "================================================================"
Write-Host "           TESTING REST API CONTROLLER (PORT 9090)              "
Write-Host "================================================================"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$PSScriptRoot\build\Release\proxy_client.exe"
$psi.Arguments = "-c $PSScriptRoot\config.json -p 10895 --api-port 9090 -d"
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

Write-Host "`n1. Query GET /api/status:"
$statusJson = curl.exe -s "http://127.0.0.1:9090/api/status"
Write-Host "   $statusJson"

Write-Host "`n2. Query GET /api/nodes:"
$nodesJson = curl.exe -s "http://127.0.0.1:9090/api/nodes"
Write-Host "   $nodesJson"

Write-Host "`n3. Testing POST /api/strategy (Switch strategy to best_latency):"
$stratRes = curl.exe -s -X POST -H "Content-Type: application/json" --data-raw "{\`"strategy\`":\`"best_latency\`"}" "http://127.0.0.1:9090/api/strategy"
Write-Host "   $stratRes"

Write-Host "`n4. Testing POST /api/switch (Switch node to Canada-Montreal):"
$switchRes = curl.exe -s -X POST -H "Content-Type: application/json" --data-raw "{\`"node\`":\`"Canada-Montreal\`"}" "http://127.0.0.1:9090/api/switch"
Write-Host "   $switchRes"

$proc.StandardInput.WriteLine("quit")
$proc.WaitForExit(3000)

Write-Host "`n================================================================"
Write-Host "                 REST API TESTS COMPLETED                       "
Write-Host "================================================================"
