#ifndef MyAppVersion
  #error MyAppVersion must be supplied by scripts\build-installer.ps1
#endif

#define MyAppName "MotionWallpaper"
#define MyAppPublisher "MotionWallpaper"
#define MyAppExeName "MotionWallpaper.exe"

[Setup]
AppId={{F2984836-8AAC-4A5E-B137-69472F784A32}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousAppDir=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\artifacts
OutputBaseFilename=MotionWallpaper-v{#MyAppVersion}-setup-windows-x64
SetupIconFile=..\native\MotionWallpaper.App\Assets\motion-logo.ico
UninstallDisplayIcon={app}\App\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=MotionWallpaper.exe,motionwallpaper-agent.exe,motionwallpaper-renderer.exe
RestartApplications=no
SetupLogging=yes
LicenseFile=..\LICENSE
VersionInfoVersion=0.1.0.0
VersionInfoProductName={#MyAppName}
VersionInfoDescription=MotionWallpaper 安装程序
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Copyright (c) 2026 MotionWallpaper contributors

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\*"; DestDir: "{app}\App"; Excludes: "Config\*,Wallpapers\*,portable.mode"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\App\{#MyAppExeName}"; WorkingDir: "{app}\App"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\App\{#MyAppExeName}"; WorkingDir: "{app}\App"; Tasks: desktopicon

[Run]
Filename: "{app}\App\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; WorkingDir: "{app}\App"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /F /T /IM MotionWallpaper.exe >nul 2>&1 & taskkill /F /T /IM motionwallpaper-agent.exe >nul 2>&1 & taskkill /F /T /IM motionwallpaper-renderer.exe >nul 2>&1 & exit /B 0"; Flags: runhidden waituntilterminated; RunOnceId: "StopMotionWallpaper"
Filename: "{cmd}"; Parameters: "/C reg delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v MotionWallpaper /f >nul 2>&1 & exit /B 0"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveStartupEntry"
