# macOS release

`scripts/package-mac.sh` builds the release installer, `out/release/Novox-The-Robot-macOS.pkg`. It
installs three things, each of which can be left out under Customize:

- the standalone app, in `/Applications`;
- the Audio Unit, in `/Library/Audio/Plug-Ins/Components`;
- the VST3, in `/Library/Audio/Plug-Ins/VST3`.

The build targets Apple Silicon on macOS 13.3 or later (whisper.cpp uses Accelerate calls new in 13.3). It goes into `build-release/` and never
touches the development plugins in `~/Library`. The compatibility AU ("New Virtual Singer") is not
released.

The download page (songaday.studio/plugins, in the `songaday-studios` repository) links to
`https://github.com/songadaymann/novox-the-robot/releases/latest/download/Novox-The-Robot-macOS.pkg`.
Attach the installer to each GitHub release under that exact name.

## Without an Apple Developer account

Without an account, the bundles are signed ad hoc and the installer is unsigned. macOS then blocks
the installer until the person clicks **Open Anyway** in System Settings › Privacy & Security. The
download page explains that step in its `data-unsigned` note.

The installed plugins carry no quarantine flag (the installer doesn't pass it on), so DAWs load them
normally.

## Signing and notarizing (once the account is approved)

1. Join the Apple Developer Program (developer.apple.com/programs, $99 a year).
2. Create two certificates, **Developer ID Application** and **Developer ID Installer**. Either:
   - in Xcode › Settings › Accounts › Manage Certificates; or
   - at developer.apple.com › Certificates, from a signing request made in Keychain Access.

   Install both into the login keychain, then check their exact names:

   ```bash
   security find-identity -v
   ```

3. Make an app-specific password at account.apple.com › Sign-In and Security. Then save the
   notarization credentials once:

   ```bash
   xcrun notarytool store-credentials novox-notary --apple-id YOUR_APPLE_ID --team-id YOUR_TEAM_ID
   ```

4. Build, sign, notarize and staple:

   ```bash
   NOVOX_SIGN_APP="Developer ID Application: Jonathan Mann (YOUR_TEAM_ID)" \
   NOVOX_SIGN_INSTALLER="Developer ID Installer: Jonathan Mann (YOUR_TEAM_ID)" \
   NOVOX_NOTARY_PROFILE=novox-notary scripts/package-mac.sh
   ```

   The script signs every bundle with the hardened runtime. The app gets the microphone entitlement
   (`app.entitlements`). The script then submits the installer, waits for Apple, staples the ticket
   and checks the result with `spctl`.
5. Delete the `data-unsigned` note from the download page.

Never put the certificates, the Apple ID password or the team's keys in this repository: the
keychain holds them.
