; Setup for Sunbeam, built with Inno Setup 6 (https://jrsoftware.org/isinfo.php):
;   ISCC.exe installer\Sunbeam.iss
; after build.bat. Writes out\SunbeamSetup.exe.
;
; It installs for the current user only, without administrator rights, into
; %LOCALAPPDATA%\Programs\Sunbeam, and asks one thing: whether Sunbeam starts
; with Windows. A Sunbeam that is running from there is closed first, on an
; update as on removal. The settings in %LOCALAPPDATA%\Sunbeam stay when it is
; removed.

#define Exe SourcePath + "\..\out\Sunbeam.exe"
#define AppVersion GetStringFileInfo(Exe, "ProductVersion")

[Setup]
AppId={{F81BEBE8-BDF8-4ABA-A854-5E7974A7D3A0}
AppName=Sunbeam
AppVersion={#AppVersion}
AppVerName=Sunbeam {#AppVersion}
AppPublisher=DustinHab
AppPublisherURL=https://github.com/DustinHab/Sunbeam
AppSupportURL=https://github.com/DustinHab/Sunbeam/issues
AppUpdatesURL=https://github.com/DustinHab/Sunbeam/releases
VersionInfoVersion={#AppVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\Sunbeam
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
SetupIconFile=..\images\sunbeam.ico
UninstallDisplayIcon={app}\Sunbeam.exe
UninstallDisplayName=Sunbeam
OutputDir=..\out
OutputBaseFilename=SunbeamSetup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
english.Autostart=Start Sunbeam with Windows
german.Autostart=Sunbeam mit Windows starten
english.StillRunning=Sunbeam is still running and could not be closed. Quit it from the icon in the notification area and try again.
german.StillRunning=Sunbeam läuft noch und ließ sich nicht beenden. Beenden Sie es über das Symbol im Infobereich und versuchen Sie es erneut.

[Tasks]
Name: "autostart"; Description: "{cm:Autostart}"

[Files]
Source: "..\out\Sunbeam.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\CREDITS.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Sunbeam"; Filename: "{app}\Sunbeam.exe"

[Registry]
; The same entry that the switch "Start with Windows" in the program writes.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Sunbeam"; ValueData: """{app}\Sunbeam.exe"""; Tasks: autostart; Flags: uninsdeletevalue
; Removed with the program even if it was switched on in the program itself.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Sunbeam"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\Sunbeam.exe"; Description: "{cm:LaunchProgram,Sunbeam}"; Flags: nowait postinstall skipifsilent

[Code]
const
  WM_CLOSE = $0010;

// Whether the file is in use: a running program cannot be opened for writing.
function InUse(const Path: String): Boolean;
var
  S: TFileStream;
begin
  Result := False;
  if not FileExists(Path) then Exit;
  try
    S := TFileStream.Create(Path, fmOpenReadWrite or fmShareExclusive);
    S.Free;
  except
    Result := True;
  end;
end;

// Closes a Sunbeam that runs from this folder: its window takes WM_CLOSE.
// A Sunbeam that runs from somewhere else is left alone.
function CloseSunbeam(): Boolean;
var
  Path: String;
  Wnd: HWND;
  Tries: Integer;
begin
  Path := ExpandConstant('{app}\Sunbeam.exe');
  Result := not InUse(Path);
  if Result then Exit;
  Wnd := FindWindowByClassName('SunbeamHost');
  if Wnd <> 0 then PostMessage(Wnd, WM_CLOSE, 0, 0);
  for Tries := 1 to 50 do
  begin
    Sleep(100);
    if not InUse(Path) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if CloseSunbeam() then
    Result := ''
  else
    Result := CustomMessage('StillRunning');
end;

function InitializeUninstall(): Boolean;
begin
  CloseSunbeam();
  Result := True;
end;
