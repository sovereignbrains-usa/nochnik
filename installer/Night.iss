; Обычный установщик Ночника (Inno Setup 6). Собирает installer.ps1: он же передаёт AppVersion и пути.
; Ставит в профиль пользователя, права администратора не нужны, сертификатов нет.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\build\Night.exe"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{6B0B1F2E-4C3A-4E7D-9A55-2D6E7C1A9F30}
AppName=Ночник
AppVersion={#AppVersion}
AppVerName=Ночник {#AppVersion}
AppPublisher=sovereignbrains
AppPublisherURL=https://github.com/sovereignbrains-usa/nochnik
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Night
DefaultGroupName=Ночник
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=Night-{#AppVersion}-setup
SetupIconFile=..\assets\night.ico
UninstallDisplayIcon={app}\Night.exe
UninstallDisplayName=Ночник
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Запускать при входе в Windows"; GroupDescription: "Дополнительно:"
Name: "desktopicon"; Description: "Ярлык на рабочем столе"; GroupDescription: "Дополнительно:"

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\fonts\OFL.txt"; DestDir: "{app}"; DestName: "Onest-OFL.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Ночник"; Filename: "{app}\Night.exe"
Name: "{autodesktop}\Ночник"; Filename: "{app}\Night.exe"; Tasks: desktopicon

; Значение то же, что пишет сам Night.exe, поэтому флажок в окне программы и установщик согласованы.
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Night"; ValueData: """{app}\Night.exe"" --background"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\Night.exe"; Description: "Запустить Ночник"; Flags: nowait postinstall skipifsilent

[Code]
const
  WM_APP_QUIT = $8004; // WM_APP + 4: выйти (см. night.cpp)

function FindWindowW(lpClassName, lpWindowName: String): LongInt;
  external 'FindWindowW@user32.dll stdcall';
function PostMessageW(hWnd: LongInt; Msg: Cardinal; wParam, lParam: LongInt): Boolean;
  external 'PostMessageW@user32.dll stdcall';

// Просим запущенный Ночник выйти сам (он вернёт цвет экрана), и только потом снимаем силой.
procedure QuitNight;
var
  Wnd, Tries, Code: Integer;
begin
  Tries := 0;
  Wnd := FindWindowW('NightMainWindow', '');
  while (Wnd <> 0) and (Tries < 25) do
  begin
    PostMessageW(Wnd, WM_APP_QUIT, 0, 0);
    Sleep(200);
    Tries := Tries + 1;
    Wnd := FindWindowW('NightMainWindow', '');
  end;
  if Wnd <> 0 then
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im Night.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// Если раньше ставили MSIX-версию, она заняла бы тот же ярлык запуска и автозапуск: убираем её.
procedure RemoveMsix;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -Command "Get-AppxPackage -Name Nochnik | Remove-AppxPackage"',
    '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  QuitNight;
  RemoveMsix;
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    QuitNight;
end;
