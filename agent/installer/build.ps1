# Publishes the WinUI tray monitor and compiles the RemoteBuilder installer.
#
# Usage:
#   .\build.ps1
#
# Requires: .NET SDK 10 and Inno Setup 7 (ISCC.exe).

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$trayProj = Join-Path $root "agent\tray-app\RemoteBuilder.Tray.csproj"

Write-Host "==> Publishing tray monitor (self-contained win-x64)"
dotnet publish $trayProj `
  -c Release -p:Platform=x64 -r win-x64 --self-contained true --nologo
if ($LASTEXITCODE -ne 0) { throw "tray publish failed" }

$iscc = Get-ChildItem "C:\Program Files*\Inno Setup*\ISCC.exe" `
  -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe not found - install Inno Setup" }

Write-Host "==> Compiling installer with $($iscc.FullName)"
& $iscc.FullName (Join-Path $PSScriptRoot "RemoteBuilder.iss")
if ($LASTEXITCODE -ne 0) { throw "installer compile failed" }

$setup = Join-Path $root "agent\build\RemoteBuilder-Setup.exe"
Write-Host ""
Write-Host "Installer ready: $setup"
