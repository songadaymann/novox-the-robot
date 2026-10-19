# Hosts: Pro Tools, Ableton Live, Logic

How the plugin gets into each DAW, and what still needs testing there. Last checked 2026-10-17.

| Host | Format | Status |
|---|---|---|
| Logic Pro | AU (two identities: Novox The Robot `Nvox`, and New Virtual Singer `Nvsg` for older projects) | In daily use; latest region-drag change not yet retested live |
| Ableton Live | AU or VST3 | Not tested yet; checklist below |
| Pro Tools | AAX (optional build) | Not released: builds, never loaded in Pro Tools, unsigned |

Releases (`scripts/package-mac.sh`, songaday.studio/plugins) ship the app, the AU (`Nvox` only) and
the VST3. Pro Tools is left out: an AAX that loads in retail Pro Tools needs Avid's developer
agreement and PACE signing, which isn't worth it for now. The notes below stay for anyone who
wants to take it on.

## Pro Tools (AAX)

### What exists now

`plugin/CMakeLists.txt` adds an AAX format to the main plugin when `NOVOX_AAX_SDK` is set, as a
CMake cache variable or an environment variable of the same name. When it is empty (the default)
the build is exactly what it was: no AAX target and no changed compiler flags.

- `NOVOX_AAX_SDK=bundled` uses the AAX SDK 2.9.0 that ships inside JUCE 8.0.15
  (`modules/juce_audio_plugin_client/AAX/SDK`). **You don't need to download anything to build.**
- `NOVOX_AAX_SDK=/absolute/path/to/aax-sdk-2-x-x` uses an SDK downloaded from Avid instead. That
  download also has the documentation and example plugins, which the JUCE copy leaves out. JUCE
  needs 2.6.1 or newer.
- Configure stops with a clear error if the path isn't an SDK folder (it checks for
  `Interfaces/AAX.h`, `Interfaces/AAX_Version.h`, `Interfaces/ACF/` and `Libs/AAXLibrary/source/`)
  or if the SDK is older than 2.6.1. The compatibility AU (`NewVirtualSingerCompat`) stays AU only.
- Plugin identity: AAX identifier `com.songaday.novoxtherobot`, manufacturer `Sngd`, product
  `Nvox`, category SW Generators (Pro Tools lists it as an instrument), multi-mono off (one stereo
  or mono instance per instrument track). **Don't change the identifier, manufacturer code or plugin code** once anyone has a
  Pro Tools session with it. Sessions find the plugin by them.

Verified on 2026-10-17 (macOS, Apple silicon): `NovoxTheRobot_AAX` builds with the bundled SDK
into an arm64 `Novox The Robot.aaxplugin` (17 MB) exporting the AAX entry points
(`ACFRegisterPlugin`, `ACFGetClassFactory`, ...). Configure also works with an SDK in a separate
folder. Without the variable, a reconfigure of `build-plugin/` left every generated build file
unchanged and `NovoxPluginTests` passed (123 checks).

### Steps

1. **Avid developer account.** Sign up as an AAX developer at
   [developer.avid.com/aax](https://developer.avid.com/aax/): accept the click-through agreement, which leads to the
   "Download Evaluation Toolkit" page ([my.avid.com/cpp/sdk/aax](https://my.avid.com/cpp/sdk/aax)). There's no need
   to download the SDK from there: the build uses the copy bundled with JUCE (`bundled` below). You also need an
   iLok account ([ilok.com](https://www.ilok.com)), which Pro Tools uses for authorization.
2. **Pro Tools Developer.** Retail Pro Tools won't load unsigned plugins. Email
   [devauth@avid.com](mailto:devauth@avid.com) asking for a "Pro Tools Developer Bundle activation code" (JUCE's
   README gives this wording), then download Pro Tools Developer from your Avid developer account. It loads
   unsigned AAX plugins, so testing needs no signing and no iLok USB key. Developers report replies taking from a
   few days to a few weeks.
3. **Build:**
   ```bash
   cmake -S . -B build-plugin -DNOVOX_AAX_SDK=bundled     # or =/absolute/path/to/aax-sdk-2-9-0
   cmake --build build-plugin -j --target NovoxTheRobot_AAX
   ```
   The bundle is at `build-plugin/plugin/NovoxTheRobot_artefacts/Release/AAX/` and is copied to
   `/Library/Application Support/Avid/Audio/Plug-Ins` (Pro Tools only looks there; the build
   fails at the copy step if that folder isn't writable). `NovoxTheRobot_All` builds AAX too
   once it's enabled. To turn AAX off again: `-DNOVOX_AAX_SDK=` (and unset the environment
   variable). For a release, build both architectures with
   `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`: current Pro Tools runs natively on Apple silicon,
   but Pro Tools on an Intel Mac or under Rosetta needs x86_64. On Windows JUCE builds AAX only
   as x64 and not with GCC. Neither has been tried.
4. **Test in Pro Tools Developer.** New Instrument track, insert Novox The Robot from the instrument plug-ins,
   then go through the Live checklist below (playback, loop, save/reopen, window size, import,
   Detailed notes, singing capture). Pro Tools needs microphone permission for singing capture.
5. **Ask for the signing tools.** Email [audiosdk@avid.com](mailto:audiosdk@avid.com) with the subject "PACE Eden Signing Tools
   Request": an overview of the plugin, a screen recording of it running in Pro Tools Developer
   (with audio if possible), company name, admin full name and phone number. PACE then contacts
   you. The tools are free from Avid. PACE gives you an account and a wrap configuration GUID for the product.
   **iLok USB key:** Avid's page and PACE's getting-started guide say the signing certificate lives on an iLok USB
   key, and developers on the JUCE forum report local signing still wants the physical key even with iLok Cloud.
   PACE also offers cloud signing for build servers without hardware; ask PACE whether it's available to you.
   Only signing needs the key: people using Novox never do (signing isn't copy protection).
6. **Sign** (macOS). Sign last: changing anything in the bundle afterwards breaks the signature.
   The build's ad-hoc signature isn't enough: wraptool signs with your Apple Developer ID
   certificate. Some developers report wraptool wants an existing codesign signature first. The typical
   form (check PACE's own docs, which come under their agreement):
   ```bash
   codesign --force --sign "Developer ID Application: <name> (<team>)" --timestamp "Novox The Robot.aaxplugin"
   wraptool sign --verbose --account <iLok account> --wcguid <GUID from PACE> \
     --signid "Developer ID Application: <name> (<team>)" \
     --in "Novox The Robot.aaxplugin" --out "Novox The Robot.aaxplugin"
   wraptool verify --verbose --in "Novox The Robot.aaxplugin"
   ```
   Then notarize the installer or zip as usual, and test the signed copy in retail Pro Tools.

### License notes

- **The AAX SDK can be used under GPLv3.** The license file in the SDK that JUCE bundles
  (`AAX/SDK/LICENSE.txt`, Avid, 2024) says the SDK is "subject to commercial or open-source
  licensing" and that you may instead use the code under GPL v3. JUCE's `LICENSE.md` lists it as
  "Proprietary Avid AAX License/GPLv3". A JUCE forum post dates the new banner to SDK 2.8.0, in
  July 2024. Avid's AAX page itself still only describes the click-through license and says to
  contact audiosdk@avid.com to commercialize.
- **That fits this repo's AGPLv3 plugin.** GPLv3 section 13 and AGPLv3 section 13 each allow
  combining code under the other license into one work. Each part keeps its own license, and the
  AGPL terms (including source for network users) apply to the AGPL part. The plugin source,
  including the AAX build setup, stays public as it is now. JUCE's AGPLv3 option already covers the AAX wrapper code.
- **What the GPL option doesn't cover:** Pro Tools Developer, the PACE signing tools, the signing
  certificate and Avid's SDK documentation all come through Avid's and PACE's own developer
  agreements. Forum posts describe NDAs there, so don't commit anything from the PACE kit, the
  wrap GUID or any credentials. Keep them in `.env` or your keychain.
- **Signed binaries.** Retail Pro Tools loads only PACE-signed builds, so a stranger's modified
  build runs in Pro Tools Developer or after they get their own signing. GPLv3's
  "Installation Information" rule (section 6) is written for consumer hardware ("User Products"),
  not plugins on a general-purpose computer, so publishing the full source looks sufficient. This is
  a reading, not legal advice. Ask Avid (audiosdk@avid.com) if you plan to sell it or put it on the Avid Marketplace.

### Untested

Everything inside Pro Tools: loading, MIDI timing and the playhead (bar 1 vs session start),
loop playback, session save/reopen, the WebView editor and resizing, microphone access, file drops,
and whether Pro Tools offers a meaningless AudioSuite version of a synth. Also untested: universal
or Windows AAX builds, wraptool signing, notarization, and building against an SDK downloaded from Avid
(only a copy of the bundled SDK was tried).

Sources: [Avid AAX developer page](https://developer.avid.com/aax/);
JUCE 8.0.15 `README.md` ("AAX Plug-Ins"), `LICENSE.md` and `CHANGE_LIST.md` (8.0 bundled the SDK,
8.0.11 updated it to 2.9.0), in `build-plugin/_deps/juce-src/`;
[JUCE forum: AAX open source release](https://forum.juce.com/t/aax-open-source-release/44017) (2.8.0
license banner, July 2024); wraptool usage from
[HISE forum](https://forum.hise.audio/post/57862) and
[KVR notarization how-to](https://www.kvraudio.com/forum/viewtopic.php?p=7518248);
[GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) and [AGPLv3](https://www.gnu.org/licenses/agpl-3.0.html) section 13;
iLok key and cloud signing: [PACE: getting started with AAX code signing](https://paceap.com/getting-started-with-aax-code-signing-for-pro-tools-plugins/),
[PACE: cloud AAX code signing](https://paceap.com/cloud-aax-code-signing-with-ci-build-systems/),
[JUCE forum: do I need the iLok USB?](https://forum.juce.com/t/signing-aax-plugin-do-i-need-the-ilok-usb/48016)
(checked 2026-10-17).

## Ableton Live test checklist

Live hosts AU and VST3, and `NovoxTheRobot_All` installs both
(`~/Library/Audio/Plug-Ins/Components` and `.../VST3`). In Live's Settings (Preferences in older
versions) > Plug-Ins, turn on Audio Units and VST3 system folders and click Rescan. Novox appears
under Plug-Ins > Audio Units > Song A Day and Plug-Ins > VST3 > Song A Day. Run the list once with
the AU and once with the VST3, each on its own **MIDI track**, and note any difference.

- [ ] **Load and window.** Drop it on a MIDI track and open the editor. Resize it with the corner
      grip, close and reopen: the size should stick. Check that typing in the Lyrics box and Cmd-A
      reach the plugin rather than Live's shortcuts.
- [ ] **Playback sync.** Draw notes at bar 1 and bar 3 and play from bar 1, then from the middle
      of a note. Singing should be on the beat, consonants slightly early, and nothing doubled.
      Change Live's tempo while stopped and while playing: the song should re-render to the new tempo.
- [ ] **Transport and loop.** Loop bars 2 to 4: each pass should start cleanly with no hanging
      vowel. Stop, start and click elsewhere in the arrangement while playing. Try Live's
      start-marker and "Back to Arrangement".
- [ ] **Record from MIDI.** Arm the keyboard icon and play a MIDI clip or a keyboard into the track
      while the transport runs. Notes should land on the beats played.
- [ ] **Import by file.** Use Import... with a WAV, an MP3 and a MIDI file. Also drop each from the
      Finder and from Live's browser. Notes should start at bar 1 unless the file carries a time stamp.
- [ ] **Drag a clip from the arrangement.** Drag an audio clip that starts at bar 5, then a MIDI
      clip, onto the editor. Expected: notes land at **bar 1**. Live's clips carry no Logic-style
      timestamp, so this isn't a bug unless the notes land somewhere else. Report:
      - which bar the notes land on, and what the LCD says
      - what file Live handed over: its name, and whether it's the clip's whole source sample, only
        the clip's region, and warped or unwarped. Compare the imported length with the clip.
      - if the editor says the region didn't provide a file, launch Live from Terminal and repeat. A
        line starting `Novox region drop types:` lists what Live offered.
- [ ] **Detailed notes.** Import the same sung take with the checkbox under Import... off, then on.
      On should give the same notes plus extra splits.
- [ ] **Singing capture.** Allow microphone access for Live (System Settings > Privacy & Security >
      Microphone). Record a phrase with the singing icon, then stop: notes and lyrics should appear.
      Cancel should discard it. The plugin records the system microphone, not Live's input.
- [ ] **Export back into Live.** Drag WAV and Drag MIDI onto an audio track and a MIDI track, and
      check that Export WAV/MIDI saves files.
- [ ] **Save and reopen.** Save the Set, quit Live, reopen. Song, lyrics, voice, window size and
      imported original audio (the Original button) should all come back. Also try two instances
      in one Set.

## Logic notes

- Logic hosts AU only. Both AUs run the same code: **Novox The Robot** for new projects and the
  compatibility **New Virtual Singer** so older projects still open. After rebuilding, quit and
  reopen Logic to load the new build. `auval -v aumu Nvox Sngd` checks it outside Logic.
- **Region drags carry their position.** An audio region dragged onto the plugin arrives as a
  temporary AIFF named after the region. Logic writes no Broadcast WAV `bext`. Its position is
  in the AIFF `MARK` chunk as marker names (`Tempo: 123.0`, `Start`, `End`,
  `Timestamp: <samples since 00:00:00:00 at the file's rate>`), with a `COMT` comment
  `Creator: Logic Pro 12.4`. The plugin reads those (`AudioImport::timelineStamp`), takes bar 1 to
  be at Logic's default 01:00:00:00, and places the notes at the region's bar. WAV files with a
  Broadcast WAV time stamp are placed the same way, and files without a stamp start at bar 1.
- MIDI region drags only work when Logic supplies a file or standard MIDI bytes. Logic's private
  region payloads aren't decoded, and the editor asks you to export the region and drop the file.
- Still to verify live: drag a region again after reloading the plugin. If the notes still land
  at bar 1, note the seconds the status shows. The placement assumes the AU's playhead seconds
  count from bar 1.
- **Transport from the plugin (MMC).** No plugin API starts, stops or moves a host's transport, so
  each plugin instance opens a MIDI port named **Novox The Robot** and sends MIDI Machine Control
  over it (`plugin/Source/HostMmc.*`):
  - **Messages:** Play, Stop, and for a ruler click an MTC Full Frame plus an MMC Locate, all to
    device 7F.
  - **Logic setting:** turn on **File › Project Settings › Synchronization › MIDI › Listen to MIDI
    Machine Control (MMC) input**. Logic saves it per project, so set it in your template.
  - **What it does:** a click on the plugin's ruler moves Logic's playhead there, and Space plays
    and stops Logic.
  - **How the plugin checks it:** it watches where a click lands and saves what it learns with the
    project:
    - **It moved:** MMC works.
    - **It landed elsewhere:** the miss gives the project's frame rate, or where bar 1 sits in
      timecode, and the locate goes again.
    - **It didn't move:** the display says to turn the setting on. It can tell only while Logic is
      calling the plugin; Logic stops calling one on a track it isn't playing.
  - **Until MMC is known to work,** Space reaches Logic as a key press.
  - **Other hosts:** Live does not follow MMC, and GarageBand has no MMC input.
