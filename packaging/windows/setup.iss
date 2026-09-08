; RetComM Launcher — per-user Inno Setup installer (no admin).
; Built by packaging/windows/package.ps1
;
; Defines (passed via ISCC):
;   MyAppVersion, StageDir, OutputDir, Arch

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\dist\windows-stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif
#ifndef Arch
  #define Arch "x64"
#endif

#define MyAppName "RetComM Launcher"
#define MyAppPublisher "TechnicallyComputers"
#define MyAppURL "https://github.com/TechnicallyComputers/RetComM-Launcher"
#define MyAppExeName "retcomm-hub.exe"

; VersionInfoVersion must be purely numeric. The release workflow accepts a
; prerelease suffix (0.6.4-rc1), which ISCC would reject, so strip anything
; from the first '-' or '+'. AppVersion still shows the full string.
#define NumericVersion MyAppVersion
#if Pos("-", NumericVersion) > 0
  #define NumericVersion Copy(NumericVersion, 1, Pos("-", NumericVersion) - 1)
#endif
#if Pos("+", NumericVersion) > 0
  #define NumericVersion Copy(NumericVersion, 1, Pos("+", NumericVersion) - 1)
#endif

[Setup]
AppId={{A7E6C2B1-4D9F-4E8A-9C31-8F2B6D1E0A47}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\RetComM
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Version resource for setup.exe. Without these Inno stamps its own compiler
; version as FileVersion and leaves OriginalFilename and LegalCopyright blank,
; which gives SmartScreen nothing to attribute the download to and raises the
; score of Defender ML heuristics. Same reason the exes carry VERSIONINFO
; (packaging/windows/retcomm.rc.in).
; The uninstaller picks up company/product/copyright from these but keeps
; Inno's own FileVersion; that one is not ours to set.
VersionInfoVersion={#NumericVersion}
VersionInfoProductVersion={#NumericVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
VersionInfoCopyright=Copyright (C) {#MyAppPublisher}. MIT licensed.
VersionInfoOriginalFileName=RetComM-Launcher-windows-{#Arch}-setup.exe
AppCopyright=Copyright (C) {#MyAppPublisher}. MIT licensed.
UninstallDisplayName={#MyAppName}
OutputDir={#OutputDir}
; Stable download name (no version): AppVersion still carries MyAppVersion.
OutputBaseFilename=RetComM-Launcher-windows-{#Arch}-setup
SetupIconFile={#StageDir}\retcomm.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Hub self-update exits before setup runs; avoid Inno trying to kill the process.
CloseApplications=no
RestartApplications=no
; Authenticode: package.ps1 passes /S<name>=<signtool command> and
; /DSignToolName=<name> only when a certificate is configured, so an unsigned
; local build compiles this script unchanged.
#ifdef SignToolName
SignTool={#SignToolName}
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\RetComM CLI"; Filename: "{app}\retcomm.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
