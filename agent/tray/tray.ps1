# RemoteBuilder agent - system tray monitor
#
# Shows the local (or remote) agent in the system tray:
# live inventory, job list with status/progress, balloon
# alerts on job events, and a log tail pane.
#
# Run:  .\tray.cmd          (or: powershell -STA -File tray.ps1)
# Config via env:
#   RB_AGENT_HOST   agent host   (default 127.0.0.1)
#   RB_AGENT_PORT   agent port   (default 7333)
#   RB_AGENT_TOKEN  auth token   (default: %LOCALAPPDATA%\rb-agent\token.txt)

param(
  [string]$AgentHost = "127.0.0.1",
  [int]$AgentPort = 7333
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$script:token = if ($env:RB_AGENT_TOKEN) {
  $env:RB_AGENT_TOKEN
} else {
  $tf = Join-Path $env:LOCALAPPDATA "rb-agent\token.txt"
  if (Test-Path $tf) { (Get-Content $tf -Raw).Trim() } else { "" }
}
$script:base = "http://$AgentHost`:$AgentPort"
$script:releasesUrl = "https://github.com/thnak/RemoteBuilder/releases"
$script:jobStates = @{}   # jobId -> last known status
$script:logOffset = @{}   # jobId -> bytes tailed so far

function Get-AgentJson([string]$path) {
  if (-not $script:token) { return $null }
  try {
    return Invoke-RestMethod -Uri "$($script:base)$path" `
      -Headers @{ Authorization = "Bearer $($script:token)" } `
      -TimeoutSec 4
  } catch {
    return $null
  }
}

function Get-LogTail([string]$jobId, [long]$offset) {
  try {
    $r = Invoke-WebRequest `
      -Uri "$($script:base)/jobs/$jobId/log?offset=$offset" `
      -Headers @{ Authorization = "Bearer $($script:token)" } `
      -TimeoutSec 4 -UseBasicParsing
    $text = [Text.Encoding]::UTF8.GetString($r.Content)
    $total = [long]$r.Headers["X-RB-Log-Bytes"]
    return @{ Text = $text; Total = $total }
  } catch {
    return $null
  }
}

# ---------- tray icon ----------

$exePath = Join-Path $PSScriptRoot "..\build\rbagent.exe"
$icon = if (Test-Path $exePath) {
  [System.Drawing.Icon]::ExtractAssociatedIcon($exePath)
} else {
  [System.Drawing.SystemIcons]::Application
}

$notify = New-Object System.Windows.Forms.NotifyIcon
$notify.Icon = $icon
$notify.Text = "RemoteBuilder agent"
$notify.Visible = $true

$menu = New-Object System.Windows.Forms.ContextMenuStrip
$mShow = $menu.Items.Add("Show jobs", $null, { Show-Form })
$mRefresh = $menu.Items.Add("Refresh now", $null, { Refresh-All })
$mUpdate = $menu.Items.Add("Check for updates", $null, { Check-Update })
$mReleases = $menu.Items.Add(
  "Open releases page", $null, { Start-Process $script:releasesUrl })
$menu.Items.Add("-")
$mExit = $menu.Items.Add("Exit", $null, { Exit-App })
$notify.ContextMenuStrip = $menu
$notify.Add_DoubleClick({ Show-Form })

# ---------- main window ----------

$form = New-Object System.Windows.Forms.Form
$form.Text = "RemoteBuilder agent"
$form.Size = New-Object System.Drawing.Size(760, 520)
$form.StartPosition = "CenterScreen"
$form.ShowInTaskbar = $false
$form.Add_FormClosing({
  if ($form.Visible) {
    $_.Cancel = $true
    $form.Hide()
  }
})

$info = New-Object System.Windows.Forms.Label
$info.Dock = "Top"
$info.Height = 30
$info.Font = New-Object System.Drawing.Font("Segoe UI", 10, "Bold")
$info.Text = "connecting..."
$form.Controls.Add($info)

$jobsView = New-Object System.Windows.Forms.ListView
$jobsView.Dock = "Top"
$jobsView.Height = 260
$jobsView.View = "Details"
$jobsView.FullRowSelect = $true
$jobsView.GridLines = $true
$jobsView.Columns.Add("Job", 50) | Out-Null
$jobsView.Columns.Add("Workspace", 90) | Out-Null
$jobsView.Columns.Add("Command", 260) | Out-Null
$jobsView.Columns.Add("Status", 70) | Out-Null
$jobsView.Columns.Add("Exit", 45) | Out-Null
$jobsView.Columns.Add("Log bytes", 80) | Out-Null
$form.Controls.Add($jobsView)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Dock = "Fill"
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = "Vertical"
$logBox.Font = New-Object System.Drawing.Font("Consolas", 9)
$logBox.BackColor = [System.Drawing.Color]::FromArgb(30, 30, 30)
$logBox.ForeColor = [System.Drawing.Color]::White
$form.Controls.Add($logBox)

$jobsView.Add_SelectedIndexChanged({ Update-LogPane })

function Show-Form {
  $form.ShowInTaskbar = $true
  $form.Show()
  $form.BringToFront()
  Refresh-All
}

function Exit-App {
  $timer.Stop()
  $notify.Visible = $false
  $form.Close()
  [System.Windows.Forms.Application]::Exit()
  exit 0
}

# ---------- refresh ----------

function Set-Info([string]$text, [bool]$ok) {
  $info.Text = $text
  $info.ForeColor = if ($ok) {
    [System.Drawing.Color]::FromArgb(0, 110, 60)
  } else {
    [System.Drawing.Color]::Firebrick
  }
  # NotifyIcon.Text is capped at 64 chars by WinForms
  $tip = "RemoteBuilder - $text"
  if ($tip.Length -gt 63) { $tip = $tip.Substring(0, 63) }
  $notify.Text = $tip
}

function Check-Update {
  $lines = @()
  try {
    $reg = Invoke-RestMethod `
      -Uri "https://registry.npmjs.org/remotebuilder" `
      -TimeoutSec 10 -UseBasicParsing
    $line = "client: npm latest $($reg.'dist-tags'.latest)"
    if ($reg.'dist-tags'.beta) {
      $line += " (beta $($reg.'dist-tags'.beta))"
    }
    $lines += $line
  } catch {
    $lines += "client: npm check failed (offline or not published yet)"
  }
  try {
    $rel = Invoke-RestMethod `
      -Uri "https://api.github.com/repos/thnak/RemoteBuilder/releases/latest" `
      -TimeoutSec 10 -UseBasicParsing
    $inv = Get-AgentJson "/inventory"
    if (-not $inv) {
      $lines += "agent: latest release $($rel.tag_name)"
    } else {
      $lines += "agent: this $($inv.agentVer), latest $($rel.tag_name)"
      if ($rel.tag_name -ne "v$($inv.agentVer)") {
        $lines += "AGENT UPDATE AVAILABLE:"
        $lines += $rel.html_url
      } else {
        $lines += "agent is up to date"
      }
    }
    # the tray app itself ships with the repo checkout
    $repoRoot = Split-Path -Parent `
      (Split-Path -Parent $PSScriptRoot)
    $pkgFile = Join-Path $repoRoot "package.json"
    if (Test-Path $pkgFile) {
      $localVer = (
        Get-Content $pkgFile -Raw | ConvertFrom-Json
      ).version
      if ($rel.tag_name -ne "v$localVer") {
        $lines += "tray app: local $localVer, latest $($rel.tag_name)"
        $lines += "TRAY UPDATE: git pull in $repoRoot"
      } else {
        $lines += "tray app: $localVer (up to date)"
      }
    }
  } catch {
    $lines += "agent: release check failed (offline?)"
  }
  $msg = $lines -join "`n"
  if ($msg.Length -gt 250) { $msg = $msg.Substring(0, 250) }
  [System.Windows.Forms.MessageBox]::Show(
    $form, $msg, "RemoteBuilder - check for updates") | Out-Null
}

function Refresh-All {
  $inv = Get-AgentJson "/inventory"
  if (-not $inv) {
    Set-Info "agent unreachable at $($script:base)" $false
    return
  }
  Set-Info ("{0} | {1} cores | load {2}% | mem {3}/{4} MB | jobs {5} running / {6} queued" -f `
      $inv.name, $inv.cores, $inv.cpuLoadPct, `
      $inv.memFreeMB, $inv.memTotalMB, `
      $inv.jobs.running, $inv.jobs.queued) $true

  $list = Get-AgentJson "/jobs"
  if (-not $list) { return }

  $selected = if ($jobsView.SelectedItems.Count -gt 0) {
    $jobsView.SelectedItems[0].Text
  } else { $null }

  $jobsView.Items.Clear()
  foreach ($j in $list.jobs) {
    $item = New-Object System.Windows.Forms.ListViewItem($j.jobId)
    $item.SubItems.Add($j.ws) | Out-Null
    $item.SubItems.Add($j.cmd) | Out-Null
    $item.SubItems.Add($j.status) | Out-Null
    $item.SubItems.Add([string]$j.exitCode) | Out-Null
    $item.SubItems.Add([string]$j.logBytes) | Out-Null
    switch ($j.status) {
      "running" { $item.ForeColor = [System.Drawing.Color]::DarkBlue }
      "queued" { $item.ForeColor = [System.Drawing.Color]::DarkOrange }
      "done" { $item.ForeColor = [System.Drawing.Color]::DarkGreen }
      "killed" { $item.ForeColor = [System.Drawing.Color]::DarkRed }
      "failed" { $item.ForeColor = [System.Drawing.Color]::Firebrick }
    }
    $jobsView.Items.Add($item) | Out-Null

    # balloon alerts on state transitions (tips cap at 255 chars)
    $prev = $script:jobStates[$j.jobId]
    if ($null -eq $prev) {
      if ($j.status -eq "queued" -or $j.status -eq "running") {
        $msg = "Job $($j.jobId) $($j.status): $($j.cmd)"
        if ($msg.Length -gt 250) { $msg = $msg.Substring(0, 250) }
        $notify.ShowBalloonTip(2500, "RemoteBuilder", `
          $msg, `
          [System.Windows.Forms.ToolTipIcon]::Info)
      }
    } elseif ($prev -ne $j.status) {
      if ($j.status -in @("done", "killed", "failed")) {
        $msg = "Job $($j.jobId) $($j.status) (exit $($j.exitCode))"
        if ($msg.Length -gt 250) { $msg = $msg.Substring(0, 250) }
        $notify.ShowBalloonTip(2500, "RemoteBuilder", `
          $msg, `
          [System.Windows.Forms.ToolTipIcon]::Info)
      }
    }
    $script:jobStates[$j.jobId] = $j.status
  }

  if ($selected) {
    $idx = [Array]::FindIndex(
      [object[]]$jobsView.Items, `
      [Predicate[object]]{ param($x) $x.Text -eq $selected })
    if ($idx -ge 0) { $jobsView.Items[$idx].Selected = $true }
  }
  Update-LogPane
}

function Update-LogPane {
  if ($jobsView.SelectedItems.Count -eq 0) { return }
  $id = $jobsView.SelectedItems[0].Text
  $off = 0
  if ($script:logOffset.ContainsKey($id)) {
    $off = $script:logOffset[$id]
  }
  $tail = Get-LogTail $id $off
  if (-not $tail) { return }
  if ($tail.Text.Length -gt 0) {
    if ($logBox.TextLength -gt 200000) { $logBox.Clear() }
    $logBox.AppendText($tail.Text)
    $logBox.SelectionStart = $logBox.TextLength
    $logBox.ScrollToCaret()
    $script:logOffset[$id] = $tail.Total
  }
}

# ---------- main loop ----------

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 2000
$timer.Add_Tick({ Refresh-All })
$timer.Start()

Refresh-All
[System.Windows.Forms.Application]::Run($form)
