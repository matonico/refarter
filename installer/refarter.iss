; Refarter setup, built by scripts\build-installer.ps1 with Inno Setup 6:
;   ISCC /DAppVersion=0.1.0-alpha /DAppNumericVersion=0.1.0 /DSourceDir=<build\Release> /DRepoDir=<repo> refarter.iss
; Installs for the current user only (no administrator prompt), into
; %LOCALAPPDATA%\Programs\Refarter, next to the settings in %LOCALAPPDATA%\Refarter.
; Everything Refarter needs is in Refarter.exe (static C++ runtime, RNNoise and the
; player's minimal FFmpeg with dav1d) or part of Windows, except Media Foundation
; on Windows "N" editions.

#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef AppNumericVersion
  #define AppNumericVersion "0.0.0"
#endif
; The identity of the application being installed. Only the installer test script overrides
; these (ISCC /DAppExeName="..." /DAppWindowClass="...") so that its test setups can never
; detect, close or terminate a real Refarter. Detection, waiting and termination all use them.
#ifndef AppExeName
  #define AppExeName "Refarter.exe"
#endif
#ifndef AppWindowClass
  #define AppWindowClass "RefarterDesktopShell"
#endif

; Inno's own spelling of the AppId, with the opening brace doubled. Only
; perf\check_installer.ps1 overrides it (ISCC /DAppId={{<guid>}) so that its test setups can
; never share, or overwrite, the uninstall entry of a real Refarter.
#ifndef AppId
  #define AppId "{{466BADE6-2371-4684-9C8E-D1FFB7F55737}"
#endif

[Setup]
AppId={#AppId}
AppName=Refarter
AppVersion={#AppVersion}
AppVerName=Refarter {#AppVersion}
AppPublisher=Refarter
AppPublisherURL=https://github.com/matonico/Refarter
AppSupportURL=https://github.com/matonico/Refarter/issues
VersionInfoVersion={#AppNumericVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\Refarter
DisableDirPage=auto
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 version 2004 or later; Windows Graphics Capture is needed for recording.
MinVersion=10.0.19041
SetupIconFile={#RepoDir}\assets\refarter.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName=Refarter
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; A running Refarter is closed by the code below, not by Restart Manager:
; with close-to-tray on, a close request only hides the window.
CloseApplications=no
OutputBaseFilename=Refarter-{#AppVersion}-setup

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\Refarter.exe"; DestDir: "{app}"; DestName: "{#AppExeName}"; Flags: ignoreversion
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\.deps\ffmpeg\FFMPEG-COPYING.LGPLv2.1"; DestDir: "{app}"; DestName: "FFMPEG-LICENSE.txt"; Flags: ignoreversion

[InstallDelete]
; Remove only the obsolete movie asset installed by earlier versions.
Type: files; Name: "{app}\rf.mp4"

[Icons]
Name: "{autoprograms}\Refarter"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\Refarter"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Start Refarter"; Flags: nowait postinstall skipifsilent

[Registry]
; Record cleanup even when startup is first enabled later from the app.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Refarter"; Flags: uninsdeletevalue

[Code]
const
  StartupRunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  MediaPackUrl = 'https://support.microsoft.com/topic/media-feature-pack-list-for-windows-n-editions-c1c6fffa-d052-8338-7a79-a4bb980a700a';

function MediaFoundationPresent: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\mfplat.dll'));
end;

// Windows "N" editions ship without Media Foundation; it can be added as an
// optional Windows capability, which needs administrator rights once.
procedure OfferMediaFeaturePack;
var
  Dism: String;
  Code: Integer;
begin
  if MsgBox('This looks like a Windows "N" edition without the Media Feature Pack. Refarter needs it to record and save clips.' + #13#10#13#10 +
            'Add it now? Windows asks for administrator permission, and it can take a few minutes.', mbConfirmation, MB_YESNO) = IDYES then
  begin
    // 64-bit DISM: this setup process is 32-bit, so use Sysnative where it exists.
    Dism := ExpandConstant('{win}\Sysnative\dism.exe');
    if not FileExists(Dism) then Dism := ExpandConstant('{sys}\dism.exe');
    if ShellExec('runas', Dism, '/Online /Add-Capability /CapabilityName:Media.MediaFeaturePack~~~~0.0.1.0 /NoRestart', '', SW_SHOW, ewWaitUntilTerminated, Code)
       and ((Code = 0) or (Code = 3010)) then
      MsgBox('The Media Feature Pack was added. Restart Windows before you start Refarter.', mbInformation, MB_OK)
    else if MsgBox('The Media Feature Pack could not be added automatically. Open Microsoft''s instructions?', mbError, MB_YESNO) = IDYES then
      ShellExec('open', MediaPackUrl, '', '', SW_SHOWNORMAL, ewNoWait, Code);
  end
  else
    ShellExec('open', MediaPackUrl, '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  if MediaFoundationPresent then Exit;
  if WizardSilent then
    Log('Media Foundation is missing: install the Media Feature Pack for Windows N.')
  else
    OfferMediaFeaturePack;
end;

const
  ShellWindowClass = '{#AppWindowClass}';
  QuitWaitMs = 30000;
  QuitSliceMs = 250;
  KillWaitMs = 5000;
  PROCESS_TERMINATE = $0001;
  PROCESS_QUERY_LIMITED_INFORMATION = $1000;
  SYNCHRONIZE = $00100000;
  WAIT_OBJECT_0 = 0;

function GetWindowThreadProcessId(hWnd: HWND; var ProcessId: Cardinal): Cardinal;
  external 'GetWindowThreadProcessId@user32.dll stdcall';
function OpenProcess(DesiredAccess: Cardinal; InheritHandle: BOOL; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function TerminateProcess(Handle: THandle; ExitCode: Cardinal): BOOL;
  external 'TerminateProcess@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

// Finds the running application by its window class and opens that exact process.
// 0 = no window, so nothing to close; 1 = Process is an open handle to wait on;
// -1 = a window exists but its process could not be opened, so the exit cannot be tracked.
function OpenRefarterProcess(var Process: THandle): Integer;
var
  Window: HWND;
  ProcessId: Cardinal;
begin
  Process := 0;
  Result := 0;
  Window := FindWindowByClassName(ShellWindowClass);
  if Window = 0 then Exit;
  Result := -1;
  ProcessId := 0;
  GetWindowThreadProcessId(Window, ProcessId);
  if ProcessId = 0 then Exit;
  Process := OpenProcess(SYNCHRONIZE or PROCESS_QUERY_LIMITED_INFORMATION or PROCESS_TERMINATE, False, ProcessId);
  if Process <> 0 then Result := 1;
end;

// Waits for the process behind the handle to end in bounded Sleep slices. Refarter destroys its window only after queued saves finish, and the
// process then exits; the handle also covers a process that outlives its window.
function WaitForProcessExit(Process: THandle; TimeoutMs: Integer): Boolean;
var
  Waited: Integer;
begin
  Waited := 0;
  Result := WaitForSingleObject(Process, 0) = WAIT_OBJECT_0;
  while not Result and (Waited < TimeoutMs) do
  begin
    Sleep(QuitSliceMs);
    Waited := Waited + QuitSliceMs;
    Result := WaitForSingleObject(Process, 0) = WAIT_OBJECT_0;
  end;
end;

// Keep the exact process handle through every Retry, even after its window disappears.
// Visible setup never terminates it; silent setup may terminate only this captured process
// after the grace period. Always release the handle on success, cancellation or failure.
function CloseRunningRefarter(Silent: Boolean): Boolean;
var
  Code, State: Integer;
  Exe: String;
  Process: THandle;
  Quit: Boolean;
begin
  Result := False;
  Process := 0;
  try
    State := OpenRefarterProcess(Process);
    if State = 0 then
    begin
      Result := True;
      Exit;
    end;
    if not Silent and (MsgBox('Refarter is running. Close it now? It finishes any save in progress first, but a replay that has not been saved is lost.', mbConfirmation, MB_OKCANCEL) <> IDOK) then Exit;
    Exe := ExpandConstant('{app}\{#AppExeName}');
    while True do
    begin
      if State < 0 then
      begin
        if Silent then
        begin
          Log('Refarter is running but its process could not be identified or opened, so its exit cannot be confirmed; not continuing.');
          Exit;
        end;
        if MsgBox('Refarter is running but setup cannot tell when it has closed. Close it from its tray icon, then choose Retry.', mbError, MB_RETRYCANCEL) <> IDRETRY then Exit;
        // No handle was obtained. Retry detection only in this case.
        State := OpenRefarterProcess(Process);
        if State = 0 then
        begin
          Result := True;
          Exit;
        end;
      end
      else
      begin
        // A fresh install may have no helper; still wait for a portable copy to exit.
        // Do not launch a helper again once the tracked process has already exited.
        Quit := WaitForProcessExit(Process, 0);
        if not Quit then
        begin
          if FileExists(Exe) then
            Exec(Exe, '--quit', '', SW_HIDE, ewNoWait, Code);
          Quit := WaitForProcessExit(Process, QuitWaitMs);
        end;
        if not Quit and Silent then
        begin
          Log('Refarter did not quit within ' + IntToStr(QuitWaitMs div 1000) + ' seconds; ending that process. A save in progress may be lost.');
          TerminateProcess(Process, 1);
          Quit := WaitForProcessExit(Process, KillWaitMs);
        end;
        if Quit then
        begin
          Result := True;
          Exit;
        end;
        if Silent then
        begin
          Log('Refarter is still running after the termination attempt.');
          Exit;
        end;
        if MsgBox('Refarter is still saving or could not be closed automatically. Wait for it, or close it from its tray icon, then choose Retry.', mbError, MB_RETRYCANCEL) <> IDRETRY then Exit;
        // Keep Process and State: losing the window must never imply process exit.
      end;
    end;
  finally
    if Process <> 0 then CloseHandle(Process);
  end;
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not CloseRunningRefarter(WizardSilent) then
    Result := 'Refarter is still running. Quit it from its tray icon and run setup again.';
end;

function InitializeUninstall: Boolean;
begin
  Result := CloseRunningRefarter(UninstallSilent);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  SettingsFile, Command: String;
  Success: Boolean;
begin
  if CurStep <> ssPostInstall then Exit;
  SettingsFile := ExpandConstant('{localappdata}\Refarter\refarter.ini');
  // Missing/old settings remain off. Upgrade/reinstall retains explicit intent
  // and updates the executable path, without touching Windows' disabled state.
  if CompareText(GetIniString('window', 'start_with_windows', 'off', SettingsFile), 'on') = 0 then
  begin
    // Visibility is read by the app from start_minimised (missing: on); keep the command stable.
    Command := '"' + ExpandConstant('{app}\{#AppExeName}') + '" --startup';
    Success := (Length(Command) <= 260) and RegWriteStringValue(HKCU, StartupRunKey, 'Refarter', Command);
  end
  else
  begin
    Success := True;
    if RegValueExists(HKCU, StartupRunKey, 'Refarter') then
      Success := RegDeleteValue(HKCU, StartupRunKey, 'Refarter');
  end;
  if not Success then
  begin
    Log('Refarter startup registration could not be updated.');
    if not WizardSilent then
      MsgBox('Your Windows startup setting could not be updated. Check it in Refarter settings after installation.', mbError, MB_OK);
  end;
end;

// Settings, favourites, logs and crash dumps live outside the program folder; clips are
// never touched. A silent uninstall leaves all of it in place.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Settings: String;
begin
  Settings := ExpandConstant('{localappdata}\Refarter');
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent and DirExists(Settings) then
    if MsgBox('Also remove your Refarter settings, favourites, logs and crash dumps? Your clips are never touched.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(Settings, True, True, True);
end;
