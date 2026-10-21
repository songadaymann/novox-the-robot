; Windows installer for Novox The Robot (Inno Setup 6.3 or later): the standalone app and the VST3,
; for 64-bit Windows 10 and 11, installed for all users. .github/workflows/plugin-windows.yml builds
; it; packaging/windows/README.md has the steps to build it yourself. Pass with /D:
;   NovoxVersion    CMake's project version, e.g. 1.0.0 (CMAKE_PROJECT_VERSION in CMakeCache.txt)
;   NovoxLabel      the release name people see, e.g. "1.0 beta" (NOVOX_VERSION_LABEL)
; and, when they are not in the default places below (relative to this file):
;   Artefacts       <build>\plugin\NovoxTheRobot_artefacts\Release
;   LicenseFile     the license page: packaging\macos\license.txt, the WebView2 SDK's LICENSE.txt
;                   and NOTICE.txt, then plugin\LICENSE-AGPL-3.0.txt
;   WebView2Setup   Microsoft's Evergreen WebView2 bootstrapper, MicrosoftEdgeWebview2Setup.exe
;   OutputDir       where Novox-The-Robot-Windows-Setup.exe goes

#ifndef NovoxVersion
#error Pass the version from CMake, e.g. /DNovoxVersion=1.0.0
#endif
#ifndef NovoxLabel
#define NovoxLabel NovoxVersion
#endif
#ifndef Artefacts
#define Artefacts "..\..\build-plugin\plugin\NovoxTheRobot_artefacts\Release"
#endif
#ifndef LicenseFile
#define LicenseFile "..\..\build-plugin\installer\license.txt"
#endif
#ifndef WebView2Setup
#define WebView2Setup "..\..\build-plugin\installer\MicrosoftEdgeWebview2Setup.exe"
#endif
#ifndef OutputDir
#define OutputDir "..\..\out\release"
#endif

[Setup]
; Never change AppId: Windows finds the installed copy (to upgrade or uninstall it) by it.
AppId={{54E4FD60-5E7B-4813-8F16-14818CF9EAB0}
AppName=Novox The Robot
AppVersion={#NovoxVersion}
AppVerName=Novox The Robot {#NovoxLabel}
AppPublisher=Song A Day
AppPublisherURL=https://songaday.studio/plugins
AppSupportURL=https://github.com/songadaymann/novox-the-robot
VersionInfoVersion={#NovoxVersion}
VersionInfoProductTextVersion={#NovoxLabel}
DefaultDirName={autopf}\Novox The Robot
DisableProgramGroupPage=yes
UninstallDisplayName=Novox The Robot
UninstallDisplayIcon={app}\Novox The Robot.exe
; x64 builds only (x64compatible also lets Windows on Arm, which emulates x64, install them).
; Administrator rights: VST3 plugins belong in the shared Common Files\VST3 folder.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0
LicenseFile={#LicenseFile}
OutputDir={#OutputDir}
OutputBaseFilename=Novox-The-Robot-Windows-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Close a running copy of the app (or a DAW using the VST3) before replacing it, and don't
; start a DAW again afterwards.
CloseApplications=yes
RestartApplications=no

[Types]
Name: "full"; Description: "The app and the VST3 plugin"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

; Like the Mac installer, each part can be left out.
[Components]
Name: "app"; Description: "Novox The Robot app (standalone, no DAW needed)"; Types: full custom
Name: "vst3"; Description: "VST3 plugin (Ableton Live, Reaper and other VST3 hosts)"; Types: full custom

[InstallDelete]
; The bundle is replaced as a whole: nothing from an older version stays inside it.
Type: filesandordirs; Name: "{commoncf64}\VST3\Novox The Robot.vst3"; Components: vst3

[Files]
Source: "{#Artefacts}\Standalone\Novox The Robot.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "{#Artefacts}\VST3\Novox The Robot.vst3\*"; DestDir: "{commoncf64}\VST3\Novox The Robot.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#WebView2Setup}"; DestDir: "{tmp}"; DestName: "MicrosoftEdgeWebview2Setup.exe"; Flags: deleteafterinstall; Check: NeedsWebView2

[Icons]
Name: "{autoprograms}\Novox The Robot"; Filename: "{app}\Novox The Robot.exe"; Components: app

[Run]
; The editor is a web page shown by Microsoft's WebView2 Runtime. Windows 11 and most of Windows 10
; have it; the bootstrapper downloads it where it is missing (it needs the internet).
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "Installing the Microsoft Edge WebView2 Runtime..."; Flags: waituntilterminated; Check: NeedsWebView2
Filename: "{app}\Novox The Robot.exe"; Description: "Open Novox The Robot"; Flags: postinstall nowait skipifsilent runasoriginaluser; Components: app

[Code]
const
  WebView2Client = 'Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';

// The Evergreen WebView2 Runtime records its version (pv) per machine or per user. No value, an
// empty one or 0.0.0.0 means it is not installed.
function HasWebView2(RootKey: Integer; SubKey: String): Boolean;
var
  Version: String;
begin
  Result := RegQueryStringValue(RootKey, SubKey, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0');
end;

function NeedsWebView2: Boolean;
begin
  Result := not HasWebView2(HKLM, 'SOFTWARE\WOW6432Node\' + WebView2Client)
    and not HasWebView2(HKCU, 'Software\' + WebView2Client);
end;
