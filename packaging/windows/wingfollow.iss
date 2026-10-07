; Windows installer for WING Follow (Inno Setup 6).
; Built in CI:  ISCC.exe /DAppVersion=1.2.3 /DDllPath=..\..\build\Release\reaper_wingfollow.dll /O<outdir> wingfollow.iss
;
; Per-user install (no admin) into REAPER's UserPlugins folder. The folder page is shown so a
; portable REAPER install can be targeted instead.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef DllPath
  #define DllPath "..\..\build\Release\reaper_wingfollow.dll"
#endif

[Setup]
AppId={{6E0B5A7C-3D47-4F2A-9B1E-57C1A2D9F0B4}
AppName=WING Follow for REAPER
AppVersion={#AppVersion}
AppPublisher=WING Follow
DefaultDirName={userappdata}\REAPER\UserPlugins
DisableDirPage=no
DirExistsWarning=no
AppendDefaultDirName=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=WING-Follow-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
Uninstallable=yes
UninstallDisplayName=WING Follow for REAPER
CloseApplications=no

[Messages]
SelectDirLabel3=Setup will install the WING Follow extension into REAPER's UserPlugins folder.%n%nFor a portable REAPER install, choose that install's UserPlugins folder instead.
FinishedLabel=WING Follow is installed. Restart REAPER, then open Extensions > WING Follow > Show window.

[Files]
Source: "{#DllPath}"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
; The extension installs its mixer-strip JSFX next to UserPlugins on first run.
Type: files; Name: "{app}\..\Effects\WING Follow\wing_follow.jsfx"
Type: dirifempty; Name: "{app}\..\Effects\WING Follow"

[Code]
function IsReaperRunning(): Boolean;
var
  ResultCode: Integer;
begin
  // tasklist exits 0 either way; findstr sets the result when REAPER is found.
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq reaper.exe" | findstr /I reaper.exe >nul',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  while IsReaperRunning() do
  begin
    if MsgBox('REAPER is running. Please close REAPER so the extension can be installed, then click OK.' + #13#10#13#10 +
              'Click Cancel to stop the installation.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := 'REAPER was still running.';
      exit;
    end;
  end;
end;
