<#
  Creates (or refreshes) the RemoteBuilder agent Windows service.

  Run by RemoteBuilder-Setup.exe, but safe to run by hand in an elevated
  PowerShell for repairs:

    .\setup-service.ps1 -AppDir "C:\Program Files\RemoteBuilder" `
      -AgentName $env:COMPUTERNAME `
      -Root "C:\rb-agent\workspaces" `
      -UserTokenPath "$env:LOCALAPPDATA\rb-agent\token.txt" `
      -CommonTokenPath "$env:ProgramData\rb-agent\token.txt"

  The token is generated here (crypto RNG) and handed to the service with
  --token, so both the service and the tray monitors read the same value
  instead of depending on where a service account's %LOCALAPPDATA% lands.

  An existing token is reused (taken from the service's own --token, or
  from a token file) so reinstalling or upgrading keeps already-paired
  peers valid. Pass -Token to set one explicitly.
#>
param(
  [Parameter(Mandatory = $true)][string]$AppDir,
  [Parameter(Mandatory = $true)][string]$AgentName,
  [Parameter(Mandatory = $true)][string]$Root,
  [Parameter(Mandatory = $true)][string]$UserTokenPath,
  [Parameter(Mandatory = $true)][string]$CommonTokenPath,
  [int]$Port = 7333,
  [string]$ServiceName = 'RemoteBuilderAgent',
  [string]$Token = '',
  [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$logPath = Join-Path $env:TEMP 'rb-setup.log'
function Log($message) {
  $line = "$(Get-Date -Format 'HH:mm:ss') $message"
  Add-Content -LiteralPath $logPath -Value $line
  Write-Host $line
}
Log "setup-service start: Remove=$Remove AppDir=$AppDir"

function New-Token {
  $bytes = New-Object byte[] 16
  [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
  return (-join ($bytes | ForEach-Object { '{0:x2}' -f $_ }))
}

function Get-ExistingToken {
  # Prefer the token the service is already running with, then any token
  # file that is already on disk.
  $svc = Get-CimInstance Win32_Service `
    -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue
  if ($svc -and $svc.PathName -match '--token\s+(\S+)') {
    return $Matches[1]
  }
  foreach ($path in @($UserTokenPath, $CommonTokenPath)) {
    if (Test-Path -LiteralPath $path) {
      $existing = (Get-Content -LiteralPath $path -Raw).Trim()
      if ($existing) { return $existing }
    }
  }
  return ''
}

function Remove-ServiceIfPresent {
  if (-not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) {
    Log "service not present"
    return
  }
  Log "stopping service $ServiceName"
  Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
  for ($i = 0; $i -lt 20; $i++) {
    if ((Get-Service -Name $ServiceName -ErrorAction SilentlyContinue).Status -eq 'Stopped') { break }
    Start-Sleep -Milliseconds 500
  }
  Log "deleting service $ServiceName"
  & sc.exe delete $ServiceName | Out-Null
  for ($i = 0; $i -lt 20; $i++) {
    if (-not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 500
  }
  Log "service removed"
}

if ($Remove) {
  & taskkill.exe /F /IM RemoteBuilderTray.exe 2>$null | Out-Null
  Remove-ServiceIfPresent
  & netsh.exe advfirewall firewall delete rule name="RemoteBuilder Agent" | Out-Null
  exit 0
}

# Resolve the token before touching the service, so an existing one can be
# reused and paired peers keep working across reinstalls.
$token = $Token
if (-not $token) { $token = Get-ExistingToken }
if ($token) {
  Log "reusing the existing token"
} else {
  $token = New-Token
  Log "generated a new token"
}

foreach ($path in @($UserTokenPath, $CommonTokenPath)) {
  $dir = Split-Path -Parent $path
  if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
  }
  Set-Content -LiteralPath $path -Value $token -NoNewline
  Log "wrote token to $path"
}

if (-not (Test-Path $Root)) {
  New-Item -ItemType Directory -Force -Path $Root | Out-Null
}

# Stop the service gracefully first, then any manually started agent.
Remove-ServiceIfPresent
Get-Process -Name rbagent -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$exe = Join-Path $AppDir 'rbagent.exe'
if (-not (Test-Path $exe)) { throw "agent not found: $exe" }

# The inner \" keep the quoted path in the stored ImagePath (it has spaces).
$create = 'create ' + $ServiceName +
  ' binPath= "\"' + $exe + '\" --token ' + $token +
  ' --name ' + $AgentName + ' --root ' + $Root + '"' +
  ' start= auto DisplayName= "RemoteBuilder Agent"'
Log "creating service"
$sc = Start-Process -FilePath "$env:SystemRoot\System32\sc.exe" `
  -ArgumentList $create -Wait -NoNewWindow -PassThru
if ($sc.ExitCode -ne 0) {
  throw "sc create failed (exit $($sc.ExitCode))"
}

Log "starting service"
& sc.exe start $ServiceName | Out-Null

& netsh.exe advfirewall firewall delete rule name="RemoteBuilder Agent" | Out-Null
& netsh.exe advfirewall firewall add rule name="RemoteBuilder Agent" `
  dir=in action=allow protocol=TCP localport=$Port | Out-Null

Log "RemoteBuilder agent installed: service=$ServiceName port=$Port"
Write-Host "RemoteBuilder agent installed: service=$ServiceName port=$Port"
