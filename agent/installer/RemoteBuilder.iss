; RemoteBuilder installer (Inno Setup 7+)
;
; Installs the build agent as an auto-start Windows service and the
; WinUI tray monitor into the user's Startup folder. Token generation
; and service registration live in setup-service.ps1 so the service and
; the tray monitors share one token regardless of service-account
; profile paths.
;
; Build:  "C:\Program Files\Inno Setup 7\ISCC.exe" RemoteBuilder.iss
; Prereq: agent\build\rbagent.exe and the tray publish output must exist.

#define AppName "RemoteBuilder"
#define AppVersion "0.1.0"
#define AgentPort "7333"
#define AgentRoot "C:\rb-agent\workspaces"
#define TrayRel "..\tray-app\bin\x64\Release\net10.0-windows10.0.18362.0\win-x64\publish"

[Setup]
AppId={{8F4C1D2E-9B3A-4E7C-A6F1-2D5E8B9C0A41}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=thnak
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\build
OutputBaseFilename=RemoteBuilder-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
CloseApplications=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\tray\RemoteBuilderTray.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\build\rbagent.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "setup-service.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#TrayRel}\*"; DestDir: "{app}\tray"; \
  Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName} Tray"; Filename: "{app}\tray\RemoteBuilderTray.exe"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{userstartup}\{#AppName} Tray"; Filename: "{app}\tray\RemoteBuilderTray.exe"

[Run]
Filename: "{app}\tray\RemoteBuilderTray.exe"; \
  Description: "Start the {#AppName} tray monitor now"; \
  Flags: nowait postinstall skipifsilent

[Code]
function SetupArgs(const Extra: String): String;
begin
  Result := '-NoProfile -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{app}\setup-service.ps1') + '"' +
    ' -AppDir "' + ExpandConstant('{app}') + '"' +
    ' -AgentName "' + ExpandConstant('{computername}') + '"' +
    ' -Root "{#AgentRoot}"' +
    ' -UserTokenPath "' +
      ExpandConstant('{localappdata}\rb-agent\token.txt') + '"' +
    ' -CommonTokenPath "' +
      ExpandConstant('{commonappdata}\rb-agent\token.txt') + '"' +
    ' -Port {#AgentPort}' + Extra;
end;

{ Runs before files are copied: the tray monitor and the agent service
  both hold files in the install dir, so stop them or the upgrade cannot
  replace the binaries. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  Exec(ExpandConstant('{sys}\taskkill.exe'),
    '/F /IM RemoteBuilderTray.exe', '', SW_HIDE, ewWaitUntilTerminated,
    ResultCode);
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop RemoteBuilderAgent',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1500);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    exit;

  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      SetupArgs(''), '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or
     (ResultCode <> 0) then
  begin
    MsgBox('RemoteBuilder agent setup failed (exit ' +
      IntToStr(ResultCode) + '). Run ' + ExpandConstant('{app}') +
      '\setup-service.ps1 in an elevated PowerShell to repair.',
      mbError, MB_OK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep <> usUninstall then
    exit;

  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    SetupArgs(' -Remove'), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;
