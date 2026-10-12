#define AppName "Mikraf"
#define AppVersion "1.0.0"
#define Publisher "ruwiss"
#define Src "..\hazir"

[Setup]
AppId={{B3A7C1F2-5D48-4E0B-9C61-7A2E8D4F1B95}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#Publisher}
AppPublisherURL=https://github.com/ruwiss/mikraf
AppSupportURL=https://github.com/ruwiss/mikraf/issues
DefaultDirName={autopf}\Mikraf
DefaultGroupName=Mikraf
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=Mikraf-Setup-{#AppVersion}
SetupIconFile=..\assets\mikraf.ico
UninstallDisplayIcon={app}\Mikraf.exe
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#Publisher}
VersionInfoDescription=Mikraf kurulum

[Languages]
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#Src}\Mikraf.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Src}\mikraf-apo.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Src}\mikraf-speex.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Src}\mikraf-rnnoise.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Mikraf"; Filename: "{app}\Mikraf.exe"
Name: "{group}\Mikraf'i kaldır"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Mikraf"; Filename: "{app}\Mikraf.exe"; Tasks: desktopicon

[Run]
Description: "Mikraf'i başlat"; Filename: "{app}\Mikraf.exe"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
; Mikrofon kaydındaki efekt bağlantısını temizler; yoksa kaldırdıktan sonra Windows eksik DLL arar.
Filename: "{sys}\rundll32.exe"; Parameters: """{app}\mikraf-apo.dll"",RemoveApo"; RunOnceId: "RemoveApo"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\Mikraf"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Key: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Key := 'Software\Microsoft\Windows\CurrentVersion\Run';
    RegDeleteValue(HKEY_CURRENT_USER, Key, 'Mikraf');
  end;
end;
