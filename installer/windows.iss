#ifndef PackageDir
  #error PackageDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef VCRedist
  #error VCRedist is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifdef SignedBuild
  #define ArtifactSuffix ""
#else
  #define ArtifactSuffix "-unsigned"
#endif

[Setup]
AppId={{3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8}
AppName=Mark Six Studio
AppVersion={#AppVersion}
AppPublisher=brucerry
AppPublisherURL=https://github.com/brucerry/mark-six-studio
DefaultDirName={autopf}\Mark Six Studio
DefaultGroupName=Mark Six Studio
UninstallDisplayIcon={app}\mark-six-studio.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=MarkSixStudio-{#AppVersion}-windows-x64{#ArtifactSuffix}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\icon.ico
CloseApplications=yes
RestartApplications=no
#ifdef SignedBuild
SignTool=marksix
SignedUninstaller=yes
#endif

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#VCRedist}"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\Mark Six Studio"; Filename: "{app}\mark-six-studio.exe"
Name: "{autodesktop}\Mark Six Studio"; Filename: "{app}\mark-six-studio.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\mark-six-studio.exe"; Description: "Launch Mark Six Studio"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
begin
  Result := '';
  ExitCode := 0;
  ExtractTemporaryFile('VC_redist.x64.exe');
  if not Exec(ExpandConstant('{tmp}\VC_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    Result := 'Unable to start the Microsoft Visual C++ runtime installer.';
  if (Result = '') and (ExitCode <> 0) and (ExitCode <> 1638) and (ExitCode <> 3010) then
    Result := 'Microsoft Visual C++ runtime installation failed. Code: ' + IntToStr(ExitCode);
  if ExitCode = 3010 then NeedsRestart := True;
end;
