#ifndef MyAppVersion
  #error MyAppVersion must be supplied by scripts\build-installer.ps1
#endif

#define MyAppName "MotionWallpaper"
#define MyAppPublisher "MotionWallpaper"
#define MyAppExeName "MotionWallpaper.exe"
#ifndef MyAppId
  #define MyAppId "{{F2984836-8AAC-4A5E-B137-69472F784A32}"
#endif
#ifndef LegacyDataRoot
  #define LegacyDataRoot "{localappdata}\MotionWallpaper"
#endif

[Setup]
AppId={#MyAppId}
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
Source: "..\build\*"; DestDir: "{app}\App"; Excludes: "Config\*,Wallpapers\*"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\App\{#MyAppExeName}"; WorkingDir: "{app}\App"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\App\{#MyAppExeName}"; WorkingDir: "{app}\App"; Tasks: desktopicon

[Run]
Filename: "{app}\App\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; WorkingDir: "{app}\App"; Flags: nowait postinstall skipifsilent

#ifndef InstallerSmokeTest
[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /F /T /IM MotionWallpaper.exe >nul 2>&1 & taskkill /F /T /IM motionwallpaper-agent.exe >nul 2>&1 & taskkill /F /T /IM motionwallpaper-renderer.exe >nul 2>&1 & exit /B 0"; Flags: runhidden waituntilterminated; RunOnceId: "StopMotionWallpaper"
Filename: "{cmd}"; Parameters: "/C reg delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v MotionWallpaper /f >nul 2>&1 & exit /B 0"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveStartupEntry"
#endif

[UninstallDelete]
Type: filesandordirs; Name: "{app}\App\Config"
Type: filesandordirs; Name: "{app}\App\Wallpapers"
Type: filesandordirs; Name: "{#LegacyDataRoot}"

[Code]
function CopyDirectoryContents(const SourceDir, TargetDir: String): Boolean;
var
  FindRec: TFindRec;
  SourcePath: String;
  TargetPath: String;
begin
  Result := ForceDirectories(TargetDir);
  if not Result then
    Exit;

  if FindFirst(AddBackslash(SourceDir) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          SourcePath := AddBackslash(SourceDir) + FindRec.Name;
          TargetPath := AddBackslash(TargetDir) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            Result := CopyDirectoryContents(SourcePath, TargetPath)
          else
            Result := CopyFile(SourcePath, TargetPath, False);
          if not Result then
            Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure MigrateLegacyDataDirectory(const DirectoryName: String);
var
  LegacyRoot: String;
  SourceDir: String;
  TargetDir: String;
begin
  LegacyRoot := ExpandConstant('{#LegacyDataRoot}');
  SourceDir := AddBackslash(LegacyRoot) + DirectoryName;
  TargetDir := ExpandConstant('{app}\App\') + DirectoryName;
  if not DirExists(SourceDir) then
    Exit;

  if DirExists(TargetDir) then
  begin
    Log('Skipping legacy data migration because the target already exists: ' + TargetDir);
    Exit;
  end;

  Log('Migrating MotionWallpaper data from ' + SourceDir + ' to ' + TargetDir);
  if CopyDirectoryContents(SourceDir, TargetDir) then
  begin
    if not DelTree(SourceDir, True, True, True) then
      Log('Migration copied successfully, but the old directory could not be removed: ' + SourceDir);
  end
  else
  begin
    Log('Migration failed; the original data was preserved at ' + SourceDir);
    DelTree(TargetDir, True, True, True);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    MigrateLegacyDataDirectory('Config');
    MigrateLegacyDataDirectory('Wallpapers');
    RemoveDir(ExpandConstant('{localappdata}\MotionWallpaper'));
  end;
end;
