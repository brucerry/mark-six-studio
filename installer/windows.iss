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
MinVersion=10.0.17763
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=MarkSixStudio-{#AppVersion}-windows-x64{#ArtifactSuffix}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\icon.ico
CloseApplications=yes
RestartApplications=no
SetupMutex=Global\MarkSixStudioInstaller-3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8
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
procedure CurStepChanged(CurStep: TSetupStep);
var
  ExitCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    ExitCode := 0;
    if not Exec(ExpandConstant('{app}\mark-six-maintenance.exe'), '--secure-install-registration', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) or (ExitCode <> 0) then
      RaiseException('Unable to protect the Mark Six Studio installation registration. Reinstall before using complete cleanup.');
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := not RegKeyExists(HKLM64, 'SOFTWARE\brucerry\MarkSixStudio\Maintenance');
  if not Result then
    MsgBox('Mark Six Studio cleanup is active. Finish or retry its maintenance helper before installing.', mbError, MB_OK);
end;

function InitializeUninstall(): Boolean;
var
  ExitCode: Integer;
  Parameters, Operation: String;
begin
  Result := True;
  if ExpandConstant('{param:M6FULLCLEANUP|0}') = '1' then begin
    { Explicit bootstrap only. The protected standalone helper independently
      discovers all accounts and requests final consent before any deletion. }
    Parameters := '--uninstall-bootstrap ' + ExpandConstant('{param:M6PIPE|}') + ' ' +
      ExpandConstant('{param:M6NONCE|}') + ' ' + ExpandConstant('{param:M6PID|}') + ' ' +
      ExpandConstant('{param:M6BIRTH|}') + ' ' + ExpandConstant('{param:M6ACCOUNT|}');
    Exec(ExpandConstant('{app}\mark-six-maintenance.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
    Result := False;
  end else if RegKeyExists(HKLM64, 'SOFTWARE\brucerry\MarkSixStudio\Maintenance') then begin
    { Ordinary uninstall keeps its data-retaining default. During complete
      cleanup only its already reviewed native provider can resume it. }
    Result := RegQueryStringValue(HKLM64, 'SOFTWARE\brucerry\MarkSixStudio\Maintenance', 'Operation', Operation) and
      (Operation = ExpandConstant('{param:M6BROKEROP|}'));
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
begin
  Result := '';
  ExitCode := 0;
  ExtractTemporaryFile('VC_redist.x64.exe');
  if not Exec(ExpandConstant('{tmp}\VC_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    Result := 'Unable to start the Microsoft Visual C++ runtime installer.';
  { 0x80070666 is HRESULT_FROM_WIN32(ERROR_PRODUCT_VERSION): a newer compatible v14 runtime is installed. }
  if (Result = '') and (ExitCode <> 0) and (ExitCode <> 1638) and (ExitCode <> -2147023258) and (ExitCode <> 3010) then
    Result := 'Microsoft Visual C++ runtime installation failed. Code: ' + IntToStr(ExitCode);
  if ExitCode = 3010 then NeedsRestart := True;
end;
