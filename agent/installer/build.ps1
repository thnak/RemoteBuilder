# Publishes the WinUI tray monitor and compiles the RemoteBuilder installer.
#
# Usage:
#   .\build.ps1
#
# Requires: .NET SDK 10 and Inno Setup 7 (ISCC.exe).

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$trayProj = Join-Path $root "agent\tray-app\RemoteBuilder.Tray.csproj"

# Native tools write progress and warnings to stderr, and PowerShell 5.1
# turns stderr into a terminating error while ErrorActionPreference is
# Stop. Run them with that relaxed and check the exit code instead.
$script:lastExit = 0
function Invoke-Native {
  param([string]$File, [string[]]$Arguments)
  $previous = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  & $File @Arguments
  $script:lastExit = $LASTEXITCODE
  $ErrorActionPreference = $previous
}

Write-Host "==> Publishing tray monitor (self-contained win-x64)"
Invoke-Native "dotnet" @(
  "publish", $trayProj,
  "-c", "Release", "-p:Platform=x64", "-r", "win-x64",
  "--self-contained", "true", "--nologo")
if ($script:lastExit -ne 0) {
  throw "tray publish failed (exit $script:lastExit)"
}

$iscc = Get-ChildItem "C:\Program Files*\Inno Setup*\ISCC.exe" `
  -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe not found - install Inno Setup" }

Write-Host "==> Compiling installer with $($iscc.FullName)"
Invoke-Native $iscc.FullName @((Join-Path $PSScriptRoot "RemoteBuilder.iss"))
if ($script:lastExit -ne 0) {
  throw "installer compile failed (exit $script:lastExit)"
}

$setup = Join-Path $root "agent\build\RemoteBuilder-Setup.exe"
Write-Host ""
Write-Host "Installer ready: $setup"
