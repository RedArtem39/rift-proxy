$ErrorActionPreference = "Continue"

$targetUrl = "https://speed.cloudflare.com/__down?bytes=5000000"

Write-Host "1. Testing Direct Download (5 MB)..."
$sw1 = [System.Diagnostics.Stopwatch]::StartNew()
curl.exe -s -o "test_5mb_dir.bin" $targetUrl
$sw1.Stop()
$size1 = (Get-Item "test_5mb_dir.bin").Length
Remove-Item "test_5mb_dir.bin" -ErrorAction SilentlyContinue

Write-Host "2. Testing Proxy Download via SOCKS5 (5 MB)..."
$sw2 = [System.Diagnostics.Stopwatch]::StartNew()
curl.exe -s -o "test_5mb_proxy.bin" -x "socks5://Yz2063:zxnbKF@152.232.171.125:8000" $targetUrl
$sw2.Stop()
$size2 = (Get-Item "test_5mb_proxy.bin").Length
Remove-Item "test_5mb_proxy.bin" -ErrorAction SilentlyContinue

$d_mb = $size1 / 1048576.0
$d_sec = $sw1.ElapsedMilliseconds / 1000.0
$d_speed = $d_mb / $d_sec
$d_mbps = $d_speed * 8.0

$p_mb = $size2 / 1048576.0
$p_sec = $sw2.ElapsedMilliseconds / 1000.0
$p_speed = $p_mb / $p_sec
$p_mbps = $p_speed * 8.0

Write-Host ("`nDirect Speed: {0:N2} MB/s ({1:N2} Mbps) in {2:N2}s" -f $d_speed, $d_mbps, $d_sec)
Write-Host ("Proxy Speed:  {0:N2} MB/s ({1:N2} Mbps) in {2:N2}s" -f $p_speed, $p_mbps, $p_sec)
