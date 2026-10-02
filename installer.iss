; Team Launcher - Inno Setup Script
; Requires Inno Setup 6.3+

#define MyAppName "Team Launcher"
#define MyAppVersion "4.2.2"
#define MyAppPublisher "Team Launcher"
#define MyAppURL "https://github.com/teamstarwars-dev/Team-Launcher"
#define MyAppExeName "TeamLauncher.exe"
#define MyAppId "{{B5E3A8D2-7F4A-4E9C-A1D3-6B2E8F0C9D5A}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=D:\projet\perso\Team Launcher\installer-output
OutputBaseFilename=TeamLauncher-{#MyAppVersion}-Setup
SetupIconFile=src\TeamLauncher\TeamLauncher.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=force
CloseApplicationsFilter=TeamLauncher.exe
RestartApplications=no
LicenseFile=LICENSE.txt

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startmenuicon"; Description: "Créer un raccourci dans le Menu Démarrer"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "dist\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\*.deps.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\*.runtimeconfig.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\default.env"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Lancer {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Registry]
; Register protocol handler (teamlauncher://)
Root: HKA; Subkey: "Software\Classes\teamlauncher"; ValueType: "string"; ValueName: ""; ValueData: "URL:Team Launcher Protocol"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\teamlauncher"; ValueType: "string"; ValueName: "URL Protocol"; ValueData: ""; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\teamlauncher\shell\open\command"; ValueType: "string"; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey

[Code]
// Check if Team Launcher is running before install
function InitializeSetup: Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  // Try to close existing instance
  Exec('taskkill', '/f /im TeamLauncher.exe', '', 0, ewWaitUntilTerminated, ResultCode);
  // Wait a moment for process to fully close
  Sleep(1000);
end;

// Check .NET 8 Desktop Runtime is installed
function IsDotNet8Installed: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec('cmd', '/c dotnet --list-runtimes | findstr "Microsoft.WindowsDesktop.App 8."', '', 0, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InitializeUpgrade: Boolean;
begin
  Result := True;
end;

// Clean up old portable data if user wants to migrate
procedure CurStepChanged(CurStep: TSetupStep);
var
  DataDir: String;
begin
  if CurStep = ssPostInstall then
  begin
    DataDir := ExpandConstant('{localappdata}\TeamLauncher');
    if not DirExists(DataDir) then
      CreateDir(DataDir);
  end;
end;
