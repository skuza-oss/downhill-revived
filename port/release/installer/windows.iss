; The Windows installer of Downhill Revived (finding 125): Inno Setup 7, compiled by
; port/tools/package_release.py (port/tools/innosetup.py fetches the pinned compiler), never by hand:
;
;   ISCC /DAppVersion=X.Y.Z /DSourceDir=<the checked release files> /DOutputDir=<dir>
;        /DOutputName=DownhillRevived-X.Y.Z-win-setup /DIconFile=<icon.ico> windows.iss
;
; SourceDir holds exactly the files of the -win.zip's folder, already checked by package_release, plus
; installed.txt: the marker that sends the player's files to %APPDATA%\Downhill Revived
; (port/launcher/src-tauri/src/game.rs INSTALLED_MARKER).
;
; Per user, no administrator: the game goes to %LOCALAPPDATA%\Programs\Downhill Revived ({autopf} when
; PrivilegesRequired=lowest), a folder the launcher can write to, so it keeps updating itself in place
; from the release zips (update.rs). Start menu and desktop shortcuts, an Apps & features entry (its
; key is game.rs UNINSTALL_KEY: the AppId below + "_is1") and an uninstaller.
;
; The uninstaller removes what it installed and what updates put in the folder since: the files the
; launcher lists in update-files.txt (update.rs UPDATE_LIST) and the .old copies of the game's files.
; Nothing else: a player may have chosen a folder that holds other files. The player's files are in
; %APPDATA%\Downhill Revived, which it doesn't touch, and a dhd.ini, log or folder found in the
; install folder is left there too. The directory page refuses a folder that already holds files
; but no Downhill Revived, and offers a "Downhill Revived" folder inside it instead.

#ifndef AppVersion
  #error AppVersion must be defined (package_release.py)
#endif

[Setup]
; never change: Windows finds the installed copy by it (and so does the launcher, game.rs)
AppId={{930C9A04-B028-4A3A-8027-E06E5D50C889}
AppName=Downhill Revived
AppVersion={#AppVersion}
AppVerName=Downhill Revived {#AppVersion}
AppPublisher=Downhill Revived contributors
AppPublisherURL=https://github.com/skuza-oss/downhill-revived
AppSupportURL=https://github.com/skuza-oss/downhill-revived/issues
AppUpdatesURL=https://github.com/skuza-oss/downhill-revived/releases
AppCopyright=GPL-3.0-or-later
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Downhill Revived
VersionInfoDescription=Downhill Revived installer
PrivilegesRequired=lowest
DefaultDirName={autopf}\Downhill Revived
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
UninstallDisplayName=Downhill Revived
UninstallDisplayIcon={app}\DownhillRevived.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SourceDir={#SourceDir}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
SetupIconFile={#IconFile}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; the launcher or the game running from an earlier install: closed first (Restart Manager)
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"

[Files]
; every file of the checked release folder (nothing else is in SourceDir)
Source: "*"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Downhill Revived"; Filename: "{app}\DownhillRevived.exe"
Name: "{autodesktop}\Downhill Revived"; Filename: "{app}\DownhillRevived.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\DownhillRevived.exe"; Description: "Start Downhill Revived"; Flags: nowait postinstall skipifsilent

[Code]
// The player's files, kept wherever they are (the same names as update.rs KEEP).
function IsPlayerFile(const Name: String): Boolean;
var
  N: String;
begin
  N := Lowercase(Name);
  Result := (N = 'dhd.ini') or (N = 'dhd.log') or (N = 'dhd-previous.log') or (N = 'iso-check.txt') or
            (N = 'launcher.log');
end;

// A name from update-files.txt that may be deleted: a plain file name, never a path or a player's file.
function IsGameFileName(const Name: String): Boolean;
begin
  Result := (Name <> '') and (Name <> '.') and (Name <> '..') and (Pos('\', Name) = 0) and
            (Pos('/', Name) = 0) and (Pos(':', Name) = 0) and not IsPlayerFile(Name);
end;

// Name and its .old copy, when they are plain files.
procedure DeleteGameFile(const Dir, Name: String);
begin
  if not IsGameFileName(Name) then exit;
  if FileExists(AddBackslash(Dir) + Name) then DeleteFile(AddBackslash(Dir) + Name);
  if FileExists(AddBackslash(Dir) + Name + '.old') then DeleteFile(AddBackslash(Dir) + Name + '.old');
end;

// After the logged files are gone: what updates added (update-files.txt, update.rs UPDATE_LIST) and
// the .old copies of the game's files go too, then the list. Any other file or folder in the install
// folder (saves\, textures\, a player's own files) stays, with the install folder around it.
procedure RemoveUpdateLeftovers(const Dir: String);
var
  Names: TArrayOfString;
  I: Integer;
begin
  if LoadStringsFromFile(AddBackslash(Dir) + 'update-files.txt', Names) then
    for I := 0 to GetArrayLength(Names) - 1 do
      DeleteGameFile(Dir, Trim(Names[I]));
  // the files this setup installed (their own .old copies: the launcher renames a file in use)
  DeleteGameFile(Dir, 'DownhillRevived.exe');
  DeleteGameFile(Dir, 'dhd.exe');
  DeleteGameFile(Dir, 'dxcompiler.dll');
  DeleteGameFile(Dir, 'update-files.txt');
  RemoveDir(Dir);  // only when empty
end;

// The directory page: a folder that already holds files, none of them Downhill Revived's, isn't used
// as it is (the uninstaller keeps other files, but a game folder of its own is tidier): the setup
// proposes a "Downhill Revived" folder inside it and stays on the page.
function FolderHoldsOtherFiles(const Dir: String): Boolean;
var
  Found: TFindRec;
begin
  Result := False;
  if not DirExists(Dir) or FileExists(AddBackslash(Dir) + 'DownhillRevived.exe') then exit;
  if FindFirst(AddBackslash(Dir) + '*', Found) then
  begin
    try
      repeat
        if (Found.Name <> '.') and (Found.Name <> '..') then
        begin
          Result := True;
          exit;
        end;
      until not FindNext(Found);
    finally
      FindClose(Found);
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir: String;
begin
  Result := True;
  if CurPageID <> wpSelectDir then exit;
  Dir := RemoveBackslashUnlessRoot(WizardForm.DirEdit.Text);
  if FolderHoldsOtherFiles(Dir) then
  begin
    WizardForm.DirEdit.Text := AddBackslash(Dir) + 'Downhill Revived';
    // a silent install (/DIR=) goes on into the new folder; the wizard shows it and stays on the page
    if WizardSilent then
      Log('The folder holds other files: installing into ' + WizardForm.DirEdit.Text)
    else
    begin
      MsgBox('That folder already holds other files. Downhill Revived gets a folder of its own inside it:' + #13#10#13#10 +
             WizardForm.DirEdit.Text, mbInformation, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveUpdateLeftovers(ExpandConstant('{app}'));
end;
