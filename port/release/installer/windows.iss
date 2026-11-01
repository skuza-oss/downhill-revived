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
; The uninstaller removes what it installed and what updates put in the folder since (newer files,
; *.old), and never the player's files: those are in %APPDATA%\Downhill Revived, which it doesn't
; touch, and a dhd.ini, log or folder found in the install folder is left there too.

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
  Result := (N = 'dhd.ini') or (N = 'dhd.log') or (N = 'iso-check.txt') or (N = 'launcher.log');
end;

// After the logged files are gone: the files updates added to the install folder (a newer release's
// files, *.old) go too. Only plain files at the top: a folder there (saves\, textures\) is the
// player's, and stays, with the install folder around it.
procedure RemoveUpdateLeftovers(const Dir: String);
var
  Found: TFindRec;
begin
  if FindFirst(AddBackslash(Dir) + '*', Found) then
  begin
    try
      repeat
        if (Found.Attributes and FILE_ATTRIBUTE_DIRECTORY = 0) and not IsPlayerFile(Found.Name) then
          DeleteFile(AddBackslash(Dir) + Found.Name);
      until not FindNext(Found);
    finally
      FindClose(Found);
    end;
  end;
  RemoveDir(Dir);  // only when empty
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveUpdateLeftovers(ExpandConstant('{app}'));
end;
