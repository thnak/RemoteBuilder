# Installs the RemoteBuilder agent as a Windows service and
# opens its port in Windows Firewall.
#
# Usage (run in an ELEVATED PowerShell):
#   .\install-service.ps1
#   .\install-service.ps1 -Name MYBUILDER -Port 7333
param(
  [string]$Name = $env:COMPUTERNAME,
  [int]$Port = 7333,
  [string]$Root = "C:\rb-agent\workspaces",
  [string]$Exe = ""
)

$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Error "Run this script as Administrator (elevated PowerShell)."
  exit 1
}

if (-not $Exe) {
  $Exe = Join-Path $PSScriptRoot "..\build\rbagent.exe"
}
if (-not (Test-Path $Exe)) {
  Write-Error "agent exe not found: $Exe (build it first: agent\build.ps1)"
  exit 1
}

# Stable token shared by the service and the tray app (the tray runs
# in the user session and reads the same file).
$tokenDir = Join-Path $env:LOCALAPPDATA "rb-agent"
$tokenFile = Join-Path $tokenDir "token.txt"
if (-not (Test-Path $tokenFile)) {
  $token = -join (1..16 | ForEach-Object {
    '{0:x2}' -f (Get-Random -Max 256)
  })
  New-Item -ItemType Directory -Force -Path $tokenDir | Out-Null
  Set-Content -Path $tokenFile -Value $token -NoNewline
}
$token = (Get-Content $tokenFile -Raw).Trim()

New-Item -ItemType Directory -Force -Path $Root | Out-Null

# A manually started rbagent would hold the port; stop it so
# the service can bind.
Get-Process -Name "rbagent" -ErrorAction SilentlyContinue |
  Stop-Process -Force

if (Get-Service -Name "RemoteBuilderAgent" -ErrorAction SilentlyContinue) {
  Write-Host "service already installed - restarting it"
  Restart-Service -Name "RemoteBuilderAgent"
} else {
  New-Service `
    -Name "RemoteBuilderAgent" `
    -BinaryPathName "`"$Exe`" --name $Name --token $token --root $Root" `
    -DisplayName "RemoteBuilder Agent" `
    -Description "RemoteBuilder build agent (HTTP on port $Port)" `
    -StartupType Automatic | Out-Null
  Write-Host "service installed"
}

Start-Service -Name "RemoteBuilderAgent"

netsh advfirewall firewall add rule `
  name="RemoteBuilder Agent" dir=in action=allow `
  protocol=TCP localport=$Port | Out-Null
Write-Host "firewall rule added for TCP $Port"

Start-Sleep -Milliseconds 500
try {
  $inv = Invoke-RestMethod `
    -Uri "http://127.0.0.1:$Port/inventory" `
    -Headers @{ Authorization = "Bearer $token" }
  Write-Host "agent OK: $($inv.name) cores=$($inv.cores) os=$($inv.os)"
} catch {
  Write-Warning "agent did not answer: $($_.Exception.Message)"
}
Write-Host ""
Write-Host "Peer token (put this in peers.json on your MCP client): $token"
Write-Host "Peer host: $((Get-NetIPAddress -AddressFamily IPv4 | `
  Where-Object { $_.InterfaceAlias -like 'Wi-Fi' -or `
    $_.InterfaceAlias -like 'Ethernet' } | Select-Object -First 1).IPAddress)"
