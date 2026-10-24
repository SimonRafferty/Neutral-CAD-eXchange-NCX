; NCX for Autodesk Fusion: installer (Inno Setup 6). Built by fusion\package.ps1, which passes the version and the
; built files' folders; to build by hand from fusion\installer:
;   ISCC.exe /DAppVersion=0.1.0 NCX-Fusion.iss
;
; Installs for the current user only (no administrator rights): Fusion loads add-ins from the user's own
; %APPDATA%\Autodesk\Autodesk Fusion 360\API\AddIns, so the installer only copies the NCX folder there. The
; manifest says runOnStartup, so the add-in starts with Fusion; nothing is registered. Two clicks for the user:
; Install, then Finish (which can start Fusion). Fusion keeps NCX.dll in use while it runs, so the installer and the
; uninstaller ask for Fusion to be closed. Uninstall (Windows Settings > Apps) removes the folder and the add-in's
; log and temporary files.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build"
#endif
#ifndef Config
  #define Config "Release"
#endif

[Setup]
AppId={{5E0C1B8A-3D47-4F2B-9A61-2C7E8F0B4D93}
AppName=NCX for Autodesk Fusion
AppVersion={#AppVersion}
AppVerName=NCX for Autodesk Fusion {#AppVersion}
AppPublisher=NCX project
AppPublisherURL=https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX
DefaultDirName={userappdata}\Autodesk\Autodesk Fusion 360\API\AddIns\NCX
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableReadyPage=yes
PrivilegesRequired=lowest
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\out
OutputBaseFilename=NCX-Fusion-Setup-{#AppVersion}
UninstallDisplayName=NCX for Autodesk Fusion
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
SetupLogging=yes

[Messages]
WelcomeLabel2=This adds the NCX commands to Autodesk Fusion, so that you can open NCX files with their full feature history, and save your designs as NCX files.%n%nIf Fusion is open, please close it first.
FinishedHeadingLabel=NCX is installed
FinishedLabelNoIcons=Start Autodesk Fusion. You will find Import NCX... and Export NCX... in Fusion's File menu.
FinishedLabel=Start Autodesk Fusion. You will find Import NCX... and Export NCX... in Fusion's File menu.

[Files]
Source: "..\NCX\NCX.manifest"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\{#Config}\NCX.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\{#Config}\NcxFusionImpl.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\core\{#Config}\ncx_core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\NCX\resources\*"; DestDir: "{app}\resources"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{code:FusionLauncher}"; Description: "Start Autodesk Fusion now"; Flags: postinstall nowait skipifsilent; \
  Check: HasFusionLauncher

[UninstallDelete]
; the add-in's log and temporary files
Type: filesandordirs; Name: "{localappdata}\NcxFusion"
Type: dirifempty; Name: "{app}"

[Code]
{ Fusion installs per user: its launcher lives under a fixed id in its webdeploy folder }
function FusionLauncher(Param: String): String;
begin
  Result := ExpandConstant('{localappdata}\Autodesk\webdeploy\production\6a0c9611291d45bb9226980209917c3d\FusionLauncher.exe');
end;

function HasFusionLauncher: Boolean;
begin
  Result := FileExists(FusionLauncher(''));
end;

function FusionInstalled: Boolean;
begin
  Result := DirExists(ExpandConstant('{localappdata}\Autodesk\webdeploy\production')) or
            DirExists(ExpandConstant('{userappdata}\Autodesk\Autodesk Fusion 360'));
end;

function FusionRunning: Boolean;
var
  rc: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq Fusion360.exe" | find /I "Fusion360.exe"',
                 '', SW_HIDE, ewWaitUntilTerminated, rc) and (rc = 0);
end;

{ Fusion keeps the add-in's files in use while it runs: ask the user to close it }
function FusionClosed: Boolean;
begin
  Result := True;
  while FusionRunning do
    if MsgBox('Please close Autodesk Fusion, then click OK.' + #13#10#13#10 +
              'Save your work in Fusion first. Fusion keeps the NCX files in use while it is open.',
              mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeSetup: Boolean;
begin
  Result := False;
  if not FusionInstalled then
  begin
    MsgBox('NCX is an add-in for Autodesk Fusion, and Fusion was not found for this Windows user.' + #13#10#13#10 +
           'Install Autodesk Fusion and start it once, then run this installer again.', mbError, MB_OK);
    Exit;
  end;
  Result := True;
end;

{ checked when the user clicks Install (after the welcome page), so a Fusion closed while reading it counts }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpWelcome then Result := FusionClosed;
end;

{ a silent install shows no welcome page: refuse there rather than fail on a file in use }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if FusionRunning then Result := 'Please close Autodesk Fusion, then run this installer again.';
end;

function InitializeUninstall: Boolean;
begin
  { a silent uninstall cannot ask (a suppressed message box answers OK for ever): it refuses instead }
  if UninstallSilent then Result := not FusionRunning
  else Result := FusionClosed;
end;
