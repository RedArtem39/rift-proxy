$ErrorActionPreference = "Continue"

Write-Host "================================================================"
Write-Host "          PROXY PERFORMANCE & INTEGRATION BENCHMARK             "
Write-Host "================================================================"

$proxyHost = "152.232.171.125"
$proxyPort = 8000
$proxyUser = "Yz2063"
$proxyPass = "zxnbKF"

# -------------------------------------------------------------
# 1. ICMP / TCP PING TO PROXY HOST & GOOGLE
# -------------------------------------------------------------
Write-Host "`n[PHASE 1] Network Route & Ping Probes"
Write-Host "----------------------------------------------------------------"

Write-Host "1.1 Ping google.com (Direct ICMP):"
$pingGoogle = Test-Connection -ComputerName "google.com" -Count 4 -ErrorAction SilentlyContinue
if ($pingGoogle) {
    $avgGoogle = ($pingGoogle | Measure-Object -Property ResponseTime -Average).Average
    Write-Host ("    Roundtrip latency: {0:N2} ms (Min: {1}ms, Max: {2}ms)" -f $avgGoogle, ($pingGoogle | Measure-Object -Property ResponseTime -Minimum).Minimum, ($pingGoogle | Measure-Object -Property ResponseTime -Maximum).Maximum)
} else {
    Write-Host "    ICMP blocked or unreachable."
}

Write-Host "`n1.2 Ping Proxy Host $proxyHost (ICMP):"
$pingProxy = Test-Connection -ComputerName $proxyHost -Count 4 -ErrorAction SilentlyContinue
if ($pingProxy) {
    $avgProxy = ($pingProxy | Measure-Object -Property ResponseTime -Average).Average
    Write-Host ("    Roundtrip latency: {0:N2} ms (Min: {1}ms, Max: {2}ms)" -f $avgProxy, ($pingProxy | Measure-Object -Property ResponseTime -Minimum).Minimum, ($pingProxy | Measure-Object -Property ResponseTime -Maximum).Maximum)
} else {
    Write-Host "    ICMP Ping to $proxyHost dropped/blocked by remote firewall (normal for proxy servers)."
}

Write-Host "`n1.3 TCP Handshake Ping to $proxyHost`:$proxyPort (Direct Socket):"
$tcpTimer = [System.Diagnostics.Stopwatch]::StartNew()
$tcpClient = New-Object System.Net.Sockets.TcpClient
$connectTask = $tcpClient.ConnectAsync($proxyHost, $proxyPort)
$completed = $connectTask.Wait(5000)
$tcpTimer.Stop()
if ($completed -and $tcpClient.Connected) {
    Write-Host ("    TCP Connection established in {0} ms" -f $tcpTimer.ElapsedMilliseconds)
    $tcpClient.Close()
} else {
    Write-Host "    TCP Connection failed or timed out after 5000ms"
}

# -------------------------------------------------------------
# 2. DIRECT VS PROXY IP / GEO LOCATION TEST
# -------------------------------------------------------------
Write-Host "`n[PHASE 2] IP & Geolocation Identity Check"
Write-Host "----------------------------------------------------------------"

Write-Host "2.1 Direct IP check (Without Proxy):"
$directIpRaw = curl.exe -s --max-time 10 "http://ip-api.com/json"
Write-Host "    $directIpRaw"

Write-Host "`n2.2 Remote Proxy Direct SOCKS5 check ($proxyHost`:$proxyPort):"
$proxySocks5Url = "socks5://$proxyUser`:$proxyPass@$proxyHost`:$proxyPort"
$socks5IpRaw = curl.exe -s --max-time 15 -x $proxySocks5Url "http://ip-api.com/json"
Write-Host "    $socks5IpRaw"

Write-Host "`n2.3 Remote Proxy Direct HTTP/HTTPS check ($proxyHost`:$proxyPort):"
$proxyHttpUrl = "http://$proxyUser`:$proxyPass@$proxyHost`:$proxyPort"
$httpIpRaw = curl.exe -s --max-time 15 -x $proxyHttpUrl "http://ip-api.com/json"
Write-Host "    $httpIpRaw"

# -------------------------------------------------------------
# 3. C++ PROXY CLIENT ENGINE INTEGRATION TEST
# -------------------------------------------------------------
Write-Host "`n[PHASE 3] Testing C++ Proxy Client Chaining Engine"
Write-Host "----------------------------------------------------------------"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$PSScriptRoot\build\Release\proxy_client.exe"
$psi.Arguments = "-c $PSScriptRoot\config_upstream_test.json -p 10800 -d"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 2

if ($proc.HasExited) {
    Write-Host "[ERROR] C++ Proxy Client failed to start!"
    Write-Host $proc.StandardError.ReadToEnd()
    Write-Host $proc.StandardOutput.ReadToEnd()
    exit 1
}

Write-Host "C++ Proxy Client running on 127.0.0.1:10800 (Upstream SOCKS5: $proxyHost`:$proxyPort)"

Write-Host "`n3.1 Query google.com via Local C++ Proxy (HTTP CONNECT -> Upstream SOCKS5):"
$googleViaLocalHttp = curl.exe -s -o /dev/null -w "HTTP Status: %{http_code} | Connect Time: %{time_connect}s | TTFB: %{time_starttransfer}s | Total: %{time_total}s`n" -x "http://127.0.0.1:10800" "https://www.google.com" --max-time 15
Write-Host "    $googleViaLocalHttp"

Write-Host "3.2 Query google.com via Local C++ Proxy (SOCKS5 -> Upstream SOCKS5):"
$googleViaLocalSocks = curl.exe -s -o /dev/null -w "HTTP Status: %{http_code} | Connect Time: %{time_connect}s | TTFB: %{time_starttransfer}s | Total: %{time_total}s`n" -x "socks5h://127.0.0.1:10800" "https://www.google.com" --max-time 15
Write-Host "    $googleViaLocalSocks"

# -------------------------------------------------------------
# 4. SPEED & LATENCY COMPARISON BENCHMARK
# -------------------------------------------------------------
Write-Host "`n[PHASE 4] Direct vs Proxy Performance Comparison"
Write-Host "----------------------------------------------------------------"

Write-Host "4.1 Direct Connection to google.com (No Proxy):"
$directGoogle = curl.exe -s -o /dev/null -w "HTTP Status: %{http_code} | Connect Time: %{time_connect}s | TTFB: %{time_starttransfer}s | Total: %{time_total}s`n" "https://www.google.com" --max-time 15
Write-Host "    $directGoogle"

Write-Host "4.2 Speed / Throughput Test (10 MB Payload Download):"

Write-Host "  -> Downloading 10MB directly (No Proxy)..."
$directSpeed = curl.exe -s -o /dev/null -w "Speed: %{speed_download} B/s | Size: %{size_download} bytes | Time: %{time_total}s`n" "https://proof.ovh.net/files/10Mb.dat" --max-time 30
$directSpeedStr = $directSpeed.Trim()
Write-Host "     Direct: $directSpeedStr"

Write-Host "  -> Downloading 10MB through C++ Proxy Client (Chained Upstream)..."
$proxySpeed = curl.exe -s -o /dev/null -w "Speed: %{speed_download} B/s | Size: %{size_download} bytes | Time: %{time_total}s`n" -x "http://127.0.0.1:10800" "https://proof.ovh.net/files/10Mb.dat" --max-time 30
$proxySpeedStr = $proxySpeed.Trim()
Write-Host "     Proxy:  $proxySpeedStr"

# -------------------------------------------------------------
# 5. CLI STATS & CLEANUP
# -------------------------------------------------------------
Write-Host "`n[PHASE 5] Proxy Server Telemetry & Shutdown"
Write-Host "----------------------------------------------------------------"

$proc.StandardInput.WriteLine("stats")
Start-Sleep -Milliseconds 300
$proc.StandardInput.WriteLine("conns")
Start-Sleep -Milliseconds 300
$proc.StandardInput.WriteLine("quit")
$proc.WaitForExit(3000)

$logs = $proc.StandardOutput.ReadToEnd()
Write-Host $logs

Write-Host "================================================================"
Write-Host "                    BENCHMARK COMPLETED                         "
Write-Host "================================================================"
