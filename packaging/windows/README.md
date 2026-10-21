# Windows release

`novox.iss` is the [Inno Setup](https://jrsoftware.org/isinfo.php) script for
`Novox-The-Robot-Windows-Setup.exe`. It installs two things, either of which can be left out on its
Select Components page:

- the standalone app, in `C:\Program Files\Novox The Robot`, with a Start menu shortcut;
- the VST3, in `C:\Program Files\Common Files\VST3\Novox The Robot.vst3`.

It is for 64-bit Windows 10 and 11 on x64 PCs, needs administrator rights, and adds an uninstaller
(Settings › Apps). No Visual C++ Redistributable is needed: everything is linked with the static C
runtime. Lyrics recognition is built for CPUs with AVX2 (most PCs from 2015 on). It also installs on
Windows on Arm, which is untested: there the app runs under x64 emulation, and only x64 or Arm64EC
hosts can load the VST3.

The editor is a web page shown by Microsoft's **WebView2 Runtime**. Windows 11 and most Windows 10
PCs already have it. Where it is missing, the installer runs Microsoft's Evergreen bootstrapper,
which downloads and installs it (so that step needs the internet). Uninstalling Novox leaves the
runtime in place, since other apps share it.

Uninstalling also leaves each Windows user's own Novox folder, `%LOCALAPPDATA%\Novox`: the lyrics
model (574 MB, downloaded on first use), the editor's web view data (`WebView2`, which keeps the
import options) and the files dragged out of the editor (`Exports`). The uninstaller runs as an
administrator, so it can't reliably reach other users' folders, and a DAW project may still use a
dragged-out file. To remove everything, delete that folder in each user's account.

## How it is built

`.github/workflows/plugin-windows.yml` builds and tests the plugin on `windows-latest` for every push
and pull request, then builds the installer and keeps the VST3, the app and the installer as
workflow artifacts. On a `v*` tag it also attaches the installer to that tag's GitHub release as
`Novox-The-Robot-Windows-Setup.exe`, with its SHA-256 in `SHA256SUMS-Windows` (replacing earlier
uploads; the Mac installer's checksum stays in `SHA256SUMS`). Create the release first, as for the
Mac installer; if it doesn't exist yet, that job fails and can be re-run.

To build it yourself, in PowerShell 7 from the repository root, with Visual Studio 2022 or newer,
Python 3 and Inno Setup 6.3 or newer installed (these are the workflow's steps):

```powershell
cmake -S . -B build-plugin -A x64 -DNOVOX_BUILD_PLUGIN=ON
cmake --build build-plugin --config Release --parallel --target NovoxTheRobot_VST3 NovoxTheRobot_Standalone

# Microsoft's WebView2 bootstrapper, downloaded for each build and never committed.
New-Item -ItemType Directory -Force build-plugin/installer | Out-Null
Invoke-WebRequest 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile build-plugin/installer/MicrosoftEdgeWebview2Setup.exe

# The version, its label and the WebView2 SDK's folder come from the CMake build.
$cache = Get-Content build-plugin/CMakeCache.txt
$version = ($cache -match '^CMAKE_PROJECT_VERSION:') -replace '^[^=]*=', ''
$label = ($cache -match '^NOVOX_VERSION_LABEL:') -replace '^[^=]*=', ''
$webview2 = ($cache -match '^WebView2_root_dir:') -replace '^[^=]*=', ''

# The license page: our notices, the WebView2 SDK's license and notices (its loader is linked into
# the app and the VST3), then the AGPLv3.
$license = @(
  Get-Content -Raw packaging/macos/license.txt
  'The Microsoft WebView2 SDK license and notices (for the WebView2 loader):'
  Get-Content -Raw "$webview2/LICENSE.txt"
  Get-Content -Raw "$webview2/NOTICE.txt"
  Get-Content -Raw plugin/LICENSE-AGPL-3.0.txt
) | ForEach-Object { $_.Trim() }
Set-Content build-plugin/installer/license.txt (($license -join "`n`n") -replace "`r?`n", "`r`n") -Encoding utf8BOM

& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" packaging/windows/novox.iss "/DNovoxVersion=$version" "/DNovoxLabel=$label"
```

The installer is written to `out/release/`. The script's header lists the other `/D` settings, for
a build folder other than `build-plugin`.

## Unsigned for now

The installer, the app and the VST3 are not code-signed yet. When someone runs the installer,
Windows SmartScreen shows "Windows protected your PC": click **More info**, then **Run anyway**.
On PCs with **Smart App Control** turned on (some new Windows 11 installs), Windows may block unsigned
programs outright, and possibly the unsigned VST3 inside a DAW too; signing is the only fix for
those.

Signing options for an individual (prices as of October 2026):

- **Azure Artifact Signing** (formerly Trusted Signing): about $9.99 a month, open to individuals in
  the US and Canada. The certificate carries Jonathan's legal name. It signs from GitHub Actions
  (`azure/artifact-signing-action`, with OIDC login, so no secret is stored). The best fit if he
  qualifies.
- **An OV certificate** from a certificate authority: about $150–300 a year, worldwide. The key must
  live on a hardware token or a cloud HSM, which makes signing in CI more awkward.
- **Certum's open-source certificate**: about €49 (cloud) or €69 (card and reader). Its cloud
  signing needs one-time codes, which is awkward in CI.
- **SignPath Foundation**: free for open-source projects, but whether a JUCE-based project qualifies
  is unclear (JUCE is also sold under a commercial license), and each release needs manual approval.

An EV certificate no longer skips the SmartScreen warning, so it isn't worth the extra cost. With any
option, SmartScreen trusts the signature only after it builds up a reputation. Sign the app, the
VST3's DLL (`Novox The Robot.vst3\Contents\x86_64-win\Novox The Robot.vst3`) and the installer,
each with an RFC 3161 timestamp.

Never commit certificates, keys or signing credentials: they belong in the signing service or in
GitHub's encrypted secrets.
