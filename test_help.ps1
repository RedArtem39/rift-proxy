$ErrorActionPreference = "Continue"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$PSScriptRoot\build\Release\proxy_client.exe"
$psi.Arguments = "-p 10899 -d"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 1

$proc.StandardInput.WriteLine("help")
Start-Sleep -Milliseconds 200
$proc.StandardInput.WriteLine("help switch")
Start-Sleep -Milliseconds 200
$proc.StandardInput.WriteLine("help rules")
Start-Sleep -Milliseconds 200
$proc.StandardInput.WriteLine("quit")
$proc.WaitForExit(3000)

Write-Host $proc.StandardOutput.ReadToEnd()
