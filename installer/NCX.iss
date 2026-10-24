; NCX Translator for SOLIDWORKS: installer (Inno Setup 6). Built by installer\build.bat.
;
; Installs the add-in (Ncx.AddIn.dll with Ncx.Sw.dll and the shared core ncx_core.dll) and registers it with
; RegAsm /codebase, which adds it to SOLIDWORKS' add-ins and loads it at start-up. The add-in is compiled against
; the SOLIDWORKS 2018 API with the interop types embedded, so one copy serves SOLIDWORKS 2018 and every later
; version; the user ticks the versions it should be active in (HKLM\SOFTWARE\NCX Translator, SolidWorksVersions),
; and in the others it stays dormant. Uninstall unregisters it and removes everything it put there.

#ifndef AppVersion
  #define AppVersion "0.5.1"
#endif

[Setup]
AppId={{3BFA0246-4189-47C8-B413-C8B41D2937CF}
AppName=NCX Translator for SOLIDWORKS
AppVersion={#AppVersion}
AppVerName=NCX Translator {#AppVersion}
AppPublisher=NCX project
AppPublisherURL=https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX
DefaultDirName={autopf}\NCX Translator
DefaultGroupName=NCX Translator
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=NCX-Translator-Setup-{#AppVersion}
UninstallDisplayName=NCX Translator for SOLIDWORKS
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
SetupLogging=yes

[Files]
Source: "..\sw\bin\Ncx.AddIn.dll"; DestDir: "{app}\addin"; Flags: ignoreversion
Source: "..\sw\bin\Ncx.Sw.dll"; DestDir: "{app}\addin"; Flags: ignoreversion
Source: "..\sw\bin\ncx_core.dll"; DestDir: "{app}\addin"; Flags: ignoreversion
; RegAsm loads these to register the add-in (SOLIDWORKS does not put its interop DLLs in the GAC)
Source: "..\sw\bin\SolidWorks.Interop.sldworks.dll"; DestDir: "{app}\addin"; Flags: ignoreversion
Source: "..\sw\bin\SolidWorks.Interop.swconst.dll"; DestDir: "{app}\addin"; Flags: ignoreversion
Source: "..\sw\bin\SolidWorks.Interop.swpublished.dll"; DestDir: "{app}\addin"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\NCX Translator - read me"; Filename: "{app}\README.md"
Name: "{group}\NCX project on GitHub"; Filename: "https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX"
Name: "{group}\Uninstall NCX Translator"; Filename: "{uninstallexe}"

[Run]
Filename: "{dotnet4064}\RegAsm.exe"; Parameters: "/codebase ""{app}\addin\Ncx.AddIn.dll"""; \
  StatusMsg: "Registering the add-in with SOLIDWORKS..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{dotnet4064}\RegAsm.exe"; Parameters: "/unregister ""{app}\addin\Ncx.AddIn.dll"""; \
  RunOnceId: "UnregisterNcx"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: dirifempty; Name: "{app}\addin"
Type: dirifempty; Name: "{app}"

[Registry]
; which SOLIDWORKS versions the add-in is active in (SOLIDWORKS registers an add-in for every version at once, so
; the add-in itself reads this and stays dormant in the others)
Root: HKLM64; Subkey: "SOFTWARE\NCX Translator"; ValueType: string; ValueName: "SolidWorksVersions"; \
  ValueData: "{code:ChosenVersions}"; Flags: uninsdeletekey

[Code]
const
  MIN_YEAR = 2018;   { the add-in is compiled against the SOLIDWORKS 2018 API }
  MAX_YEAR = 2040;
  NCX_KEY = 'SOFTWARE\NCX Translator';

var
  VersionPage: TInputOptionWizardPage;
  SWYears: array of Integer;   { installed SOLIDWORKS versions, newest first }

{ Installed SOLIDWORKS 2018 and later: a registry entry whose folder holds an SLDWORKS.exe of THAT version
  (upgrades leave stale entries pointing at another version's folder) }
function DetectSolidWorks: Integer;
var
  y, major: Integer;
  dir, ver: String;
begin
  SetArrayLength(SWYears, 0);
  for y := MAX_YEAR downto MIN_YEAR do
    if RegQueryStringValue(HKLM64, 'SOFTWARE\SolidWorks\SOLIDWORKS ' + IntToStr(y) + '\Setup', 'SolidWorks Folder', dir) and
       GetVersionNumbersString(AddBackslash(dir) + 'SLDWORKS.exe', ver) then
    begin
      major := StrToIntDef(Copy(ver, 1, Pos('.', ver) - 1), 0);
      if major + 1992 = y then
      begin
        SetArrayLength(SWYears, GetArrayLength(SWYears) + 1);
        SWYears[GetArrayLength(SWYears) - 1] := y;
      end;
    end;
  Result := GetArrayLength(SWYears);
end;

function SolidWorksInstalled: Boolean;
begin
  Result := DetectSolidWorks > 0;
end;

{ The ticked versions, as "2026,2018" }
function ChosenVersions(Param: String): String;
var
  i: Integer;
begin
  Result := '';
  for i := 0 to GetArrayLength(SWYears) - 1 do
    if (GetArrayLength(SWYears) = 1) or VersionPage.Values[i] then
    begin
      if Result <> '' then Result := Result + ',';
      Result := Result + IntToStr(SWYears[i]);
    end;
end;

procedure InitializeWizard;
var
  i: Integer;
  prev: String;
begin
  VersionPage := CreateInputOptionPage(wpSelectDir, 'SOLIDWORKS versions',
    'Which SOLIDWORKS versions should NCX Translator be added to?',
    'The NCX Translator menu will appear only in the versions ticked below. To change this later, run this installer again.',
    False, False);
  { the previous choice when the installer is run again, else every version }
  if not RegQueryStringValue(HKLM64, NCX_KEY, 'SolidWorksVersions', prev) then prev := '';
  for i := 0 to GetArrayLength(SWYears) - 1 do
  begin
    VersionPage.Add('SOLIDWORKS ' + IntToStr(SWYears[i]));
    VersionPage.Values[i] := (prev = '') or (Pos(IntToStr(SWYears[i]), prev) > 0);
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  { nothing to choose with one version }
  Result := (PageID = VersionPage.ID) and (GetArrayLength(SWYears) <= 1);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = VersionPage.ID) and (ChosenVersions('') = '') then
  begin
    MsgBox('Tick at least one SOLIDWORKS version.', mbError, MB_OK);
    Result := False;
  end;
end;

function SolidWorksRunning: Boolean;
var
  rc: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq SLDWORKS.exe" | find /I "SLDWORKS.exe"',
                 '', SW_HIDE, ewWaitUntilTerminated, rc) and (rc = 0);
end;

{ SOLIDWORKS holds the add-in's files while it runs: ask the user to close it }
function SolidWorksClosed: Boolean;
begin
  Result := True;
  while SolidWorksRunning do
    if MsgBox('Please close SOLIDWORKS (every version that is open), then click OK.' + #13#10#13#10 +
              'SOLIDWORKS keeps the add-in''s files in use while it is running.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeSetup: Boolean;
begin
  Result := False;
  if not SolidWorksInstalled then
  begin
    MsgBox('NCX Translator is an add-in for SOLIDWORKS 2018 or later, and no such SOLIDWORKS was found on this computer.' + #13#10#13#10 +
           'Install SOLIDWORKS first, then run this installer again.', mbError, MB_OK);
    Exit;
  end;
  Result := SolidWorksClosed;
end;

function InitializeUninstall: Boolean;
begin
  Result := SolidWorksClosed;
end;
