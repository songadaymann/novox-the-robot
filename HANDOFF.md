# Novox The Robot: handoff

Last updated 2026-10-16 (latest work is the last section). Read this first in a new session, then `README.md` (how to build and use)
and `docs/PLAN.md` (design notes, tuning history, roadmap).

## What this is

Novox The Robot is a modern, open-source take on Myriad's **Virtual Singer** from Harmony Assistant.
It's a formant-synthesis singing voice: you give it lyrics and a melody, and it sings them.
Jonathan Mann (Song A Day) is building it. It's MIT licensed except the plugin, which is AGPLv3
because it's built on JUCE.

Everything exists and works: a C++ engine, the `novox` CLI, Python and JavaScript/WebAssembly
packages, an Aqua-style web demo, and an AU/VST3/standalone plugin with a piano-roll editor.
Jonathan has tested the plugin in Logic and said it's "looking and working really great". He said
the voices now match Harmony's ("I think you got it", with female "almost dead on").

## State of the repo

Committed checkpoints (git log):

| Commit | What |
|---|---|
| `e3f6fb6` | Engine, tuning tools, UI mockups |
| `842b658` | Streaming API, embedded dictionary, MIDI/MusicXML import, tests |
| `6372f25` | Python wheel + CLI, WebAssembly/npm package, Aqua web demo, CI/release workflows |
| `e0be5cb` | JUCE plugin (AU/VST3/Standalone) + Aqua piano-roll editor |
| `c2d1d8f` | Rename to Novox The Robot |

**Uncommitted: audio-in (sung audio → notes + lyrics).** All of it is built and tested (1210
engine checks pass), but Jonathan hasn't approved a commit yet. **Ask before committing.** The
changed files:
- new: `engine/src/transcribe.{hpp,cpp}`, `tools/transcribe_eval.py`
- modified: `engine/include/novox.h` (`novox_transcribe`), `engine/src/novox.cpp`, `engine/cli/main.cpp`
  (`novox transcribe` + a WAV reader), `bindings/python/novox/__init__.py` (`Singer.transcribe`),
  `tests/engine_tests.cpp`, `CMakeLists.txt`

## Next task: the real-voice test (what Jonathan is about to do)

So far, audio-in has only been tested on Novox singing its own renders. Here are the results
(`tools/transcribe_eval.py`, demo song, 21 notes):

| voice | notes right / found | onset error | lyrics after Whisper alignment |
|---|---|---|---|
| std | 86% / 90% | 19 ms | near perfect |
| female | 90% / 90% | 19 ms | word-perfect |
| crooner | 95% / 95% | 11 ms | word-perfect |
| kid | 90% / 90% | 12 ms | Whisper misheard "Hugh"→"Yu", "sing"→"send" |
| robot | 95% / 90% | 10 ms | near perfect |
| choir | 66% / 90% | 13 ms | (over-splits; not a realistic input) |

**Jonathan will record a 20–40 second a cappella voice memo** and put it in `reference/`, which is
gitignored. He was asked to include held notes, a fast run of words, and a note repeated on one
pitch ("la la la"). Voice Memos files are `.m4a`, so convert them first:

```bash
cd "path/to/novox"   # the repo root
ffmpeg -i reference/take.m4a -ac 1 -ar 44100 reference/take.wav

# melody only:
build/novox transcribe reference/take.wav -o out/take_score.json

# melody + lyrics (Whisper word timestamps via MLX, then align):
tools/.venv/bin/python - <<'EOF'
import sys, json, numpy as np, librosa, mlx_whisper
sys.path.insert(0, "bindings/python"); import novox
y, sr = librosa.load("reference/take.wav", sr=44100)
r = mlx_whisper.transcribe(librosa.resample(y, orig_sr=sr, target_sr=16000),
        path_or_hf_repo="mlx-community/whisper-large-v3-turbo", language="en", word_timestamps=True)
words = [{"word": w["word"].strip(), "start": w["start"], "end": w["end"],
          "probability": w.get("probability", 1)} for s in r["segments"] for w in s.get("words", [])]
s = novox.Singer(44100)
score = s.transcribe(y, sr, words=words)
json.dump(score, open("out/take_score.json", "w"), indent=1)
for n in score["notes"]: print(f'{n["start"]:6.2f} {n["dur"]:5.2f} {n["pitch"]:5.1f} {n.get("confidence",1):.2f} {n.get("lyric","")}')
s.save_wav("out/take_novox.wav", s.render(dict(score, voice="std")))   # hear Novox sing it back
EOF
```

Then compare `out/take_novox.wav` against the take by ear (make an A/B with ffmpeg as before), and
check the note list. Tuning knobs live in `MelodyOptions` (`engine/src/transcribe.hpp`):
`minNoteSeconds`, `splitSemitones`, `onsetDb`, `silenceDb`. The Python and C API also accept them
as options. Likely real-voice problems:
- breath and noise getting read as notes (raise `silenceDb`, check the voicing threshold `clarity > 0.7`)
- vibrato wider than 0.7 semitones splitting notes
- scoops at note starts
- consonant gaps over 40 ms splitting a syllable
- Whisper word timestamps drifting against the notes

How transcription works (`engine/src/transcribe.cpp`):
1. **Pitch:** YIN pitch tracking at 16 kHz (32 ms window, 8 ms hop) with octave-jump correction.
2. **Splitting into notes:** a note ends at a pitch jump (held for 3 frames, cut at the glide
   midpoint), at a dropout of 16 ms or more, at a level dip (a same-pitch syllable), or at a gap
   over 40 ms.
3. **Cleanup:** short passing tones between neighbors fold into the next note, as do short blips
   an octave off.
4. **Lyrics:** `alignLyrics` splits each word into syllables with the lexicon and spreads them
   over the notes sung during that word. Extra notes become `_`; a word with no note of its own
   splits the previous note; each note gets `confidence`.

## Next steps after the voice test (agreed direction)

### Real-voice test completed 2026-09-29

Jonathan supplied `SEND FOLDER/novox-test.mp3` (17.58 s). Converted input is
`reference/novox-test.wav`. Reproduce with `tools/.venv/bin/python tools/transcribe_take.py
reference/novox-test.wav --out out/novox-test`. Outputs and findings are in
`out/novox-test/REPORT.md`, including original/Novox A/B audio, note plots, JSON scores,
and MIDI with lyrics for the existing plugin Import button. MIDI round-trip checks pass;
plugin GUI playback and transcription accuracy against a human annotation are unverified.

Default: 24 melody events / 25 after alignment, 10 under 200 ms. Experimental
`splitSemitones=1.5`: 18 / 19, 5 under 200 ms. The longest default held note already stays
intact at 2.72 s, but other held passages fragment. The experiment reduces those fragments
and keeps repeated same-pitch syllables in this take; it is **not** a new default, since it
can swallow deliberate semitone changes. Both variants have a guessed half-note split for
the second “had”; lyric timing needs work too. Next prioritize stable pitch-change evidence
and syllable-aware onset decisions, then test real semitone steps, vibrato and repeated words.
Engine defaults unchanged, 1210 checks pass. New evaluation tool is uncommitted; no commit made.

Second pass (user authorized tuning): `MelodyOptions.stabilizePitch` and the corresponding
Python/C JSON option enable experimental 80 ms settled-pitch evidence for small changes,
with level evidence for short voicing dropouts. It remains opt-in because some synthetic
voice results regress with stronger protection. Defaults reproduce the old six-voice results.
Lyric shared-note splits now use word timestamps, and words in rests warn instead of
manufacturing earlier notes. `out/novox-test/pass2/REPORT.md` has details: baseline 25 aligned
notes / 10 under 200 ms; stabilized 22 / 7. Listen to `stabilized-AB.wav` and import
`stabilized.mid`. 1220 checks pass, including wide 6 Hz vibrato, brief tracker glitches,
a 120 ms semitone note, repeated same-pitch notes and timestamp splits. Still uncommitted.

1. **whisper.cpp in the CLI and plugin**, so audio-in needs no Python. The model downloads on first
   use (with consent) to `~/Library/Application Support/Novox`.
2. **Review screen in the plugin**, like mockup C (`mockups/c-audio-drop.html`): drop a take, see
   the pitch curve and notes with low-confidence words highlighted, fix them, accept, and it becomes
   the song. The plugin UI is `plugin/ui/*` (HTML/JS in a JUCE WebView). The native functions are
   in `plugin/Source/PluginEditor.cpp`; the processor is in `PluginProcessor.cpp`.
3. **Publishing**, which Jonathan has to do: GitHub repo (the CI/release workflows are already in
   `.github/`), PyPI and npm accounts (the name `novox` was free on both), Apple Developer ID for
   signing/notarizing the plugin, and hosting for the web demo.
4. Later: live-instrument mode (keys sing the next syllable), a per-phoneme accent editor, more
   voices and languages, and using Novox as Hugh's own voice (see `../hugh-harmony-creator`).

## Environment notes (things that bit us)

- **Build:** `cmake -S . -B build && cmake --build build -j 8`, then `ctest --test-dir build`
  (1210 checks). Plugin: `-DNOVOX_BUILD_PLUGIN=ON` in `build-plugin/`, target `NovoxTheRobot_All`.
  Plugin tests: `build-plugin/plugin/NovoxPluginTests_artefacts/Release/NovoxPluginTests`. AU check:
  `auval -v aumu Nvox Sngd`.
- **Python:** `tools/.venv` (uv-managed 3.12, with numpy, scipy, librosa, mlx-whisper, jiwer,
  matplotlib). An **old x86 Python 3.9 sits first on PATH**, so use `--python-preference only-managed`
  with uv, and `-DPython3_EXECUTABLE=/opt/homebrew/bin/python3` for CMake.
- **WebAssembly:** Homebrew Emscripten; needs `EMSDK_PYTHON=/opt/homebrew/bin/python3`, and its config
  was edited to point at the bundled LLVM/binaryen. Build with `scripts/build-wasm.sh`; the web demo
  copies the engine via `web/build.sh` and serves with `python3 -m http.server 8777` in `web/`.
- **Harmony Assistant** (reference singer) runs under Rosetta. Renders come from the MyrScript
  `~/Documents/Myriad Documents/Settings/Scripts/NVS Render Batch.mys`, which Jonathan triggers
  from Harmony's script menu. MyrScript is **Lua 4**: no `#`, no closures, no `pcall`; use globals,
  `getn`, while-loops. Working calls: `Load(xml)`, `FrontScore()`, `score.FirstStaff`,
  `staff.LoadVirtualSingerVoice(prv)`, `score.MusicExport(wav, 15, 44100, 16)`. Harmony renders are
  in `out/ha/`; reference MP3s are in `reference/myriad/`.
- **Safety hooks** block `rm -rf` and some redirect patterns. Delete individual files, or use
  `cmake --build ... --clean-first` instead.
- There's no screen-capture permission and computer use is off, so Jonathan checks GUI behavior
  (plugin window, Logic) himself.
- The old "New Virtual Singer" plugin bundles may still be in `~/Library/Audio/Plug-Ins/`. Jonathan
  was asked to delete them.
- The project folder is still named `new virtual singer`. Renaming it is fine between sessions.

## Native audio import delivered (September 30)

Standalone/AU/VST3 now share WAV/MP3 import and drop conversion via `AudioImport`:
JUCE decode, stabilized native melody tracking and whisper.cpp English lyrics on a worker.
First-use consent downloads verified large-v3-turbo-q5_0 (574 MB) into Application Support/Novox.
No Python/ffmpeg runtime dependency. Review provides waveform/pitch, editable notes/lyrics,
split/merge/delete, original/Novox audition through processor output, accept and cancel.
Accept preserves current tempo, supports Undo and applies explicit per-note lyrics.
Native WAV test on Jonathan's take produces 22 aligned notes. Word timing is approximate;
DTW token completion times delimit word intervals, with final sung vowel extended to take end.
This remains a reviewable transcription, not a promise of perfect lyric/held-note accuracy.

Review UI harness verified split/merge and edited lyrics surviving acceptance. Native tests
cover malformed input, original audition/stop and edited preview regeneration. Logic timeline
region drops and actual host audition still need Jonathan's host test. CLI lyric recognition
still uses the earlier external workflow; whisper.cpp integration here is for app/plugin.
No commit made. See `docs/AUDIO-IMPORT-PLAN.md` and README for current workflow.

Final verification: all standalone/AU/VST3 targets built; engine suite passed (1220 checks),
plugin suite 15/15, native WAV and MP3 import suites 12/12 each, and Audio Unit validation passed.

## October 1 recording diagnosis and piano-roll review

`SEND FOLDER/novox test.mp3` (space, distinct from earlier hyphenated file) reproduces
15 aligned notes spanning MIDI 36..64 with severe octave jumps. Whisper recognizes all words,
including Can (0..0.60 s); first YIN note is 0.596 s, so alignLyrics drops Can for insufficient
overlap. Frequency analysis finds simultaneous 260/330/393 Hz components; accompaniment or
mixed-source ambiguity is suspected, not confirmed by isolated vocal reference. Current YIN
is monophonic, no vocal separation. Do not blindly transpose the result to claim correction.
Pitch accuracy remains unresolved; next evaluate vocal isolation or a singing melody model.

Review numeric table replaced with a piano roll: labeled keys, second grid, waveform and
pitch overlay, select/multi-select, drag move/pitch and right-edge resize, double-click add,
lyric edit, arrow semitone/Shift octave edits, split/merge/delete, original/Novox preview.
Full transcript remains visible including unassigned words. Native build passed; browser
fixture verified rendering and edits. No commit made.

## Direct import and plugin buffer clicks (October 1, latest)

Jonathan clarified that import must use the existing Mac-style song editor immediately.
The intermediate review UI was removed. `plugin/ui/audio-import.js` now handles conversion,
shows only first-use model consent, reports progress/errors in the existing LCD, and applies
completed notes/lyrics directly through app.js's existing checkpoint/Undo and song update.
Current tempo stays intact; normal editor tools handle correction. No-note errors preserve
song. Older review sections above are superseded by this flow.

`midi-keyboard-test-weird-click.wav` is 8 s, 44.1 kHz stereo (identical channels). High-frequency
transients during held notes concentrate at 512-sample boundaries. Existing processor recalculated
render position from rounded host PPQ each block, permitting skipped/repeated samples. Continuous
playback now follows an audio-thread sample cursor, checks contiguous host sample time when
available, tolerates small beat-report rounding (1/480 beat), and reanchors on real seeks/loops,
stop/start or changed render. Fake-host test alternates +/-10-sample PPQ error with exact sample
clock and matches the engine render sample-for-sample; real seek tests still pass.
This explains a plausible source of the captured clicks; actual updated Logic playback still
needs Jonathan's listening test. No physical clock mismatch established. No commit made.

Final checks for direct import/cursor fix: all app/AU/VST3 targets built, 17 plugin checks pass,
AU validation passes. Full editor browser fixture shows 15 imported notes immediately in the
original Mac-style piano roll; Cmd-Z restores previous 22-note song. Actual Logic listening
confirmation remains pending.

## Both Audio Unit identities now current (October 1)

Live lsof after Jonathan created a fresh project still showed AUHostingService loading
September 25 `New Virtual Singer.component` (aumu/Nvsg/Sngd), not Nvox. The names have
separate AU identities, so the earlier timing fix had not reached that loaded plugin.
Astra xhigh independently confirmed 28/30 strongest held-note transients at the same
512-sample block offset, no full-block dropouts, and legacy song-state compatibility.

Added `NewVirtualSingerCompat_AU`: same current processor/editor with original bundle ID,
Nvsg subtype and factory name. Installed it after preserving the old bundle at
`out/plugin-backups/686aca61/New Virtual Singer.component`. Keep both identities current:
NovoxTheRobot_All now depends on compatibility AU, both copy to user Components on build.
19 processor tests pass (44.1k/48k rounded PPQ, seek, legacy JUCE-string song state). Both
Nvsg and Nvox auval pass. Still requires fully quitting Logic to unload its old binary.
Current cursor's exact host sample continuity assumption remains an unproven fallback risk;
do not claim the clicking is physically a clock mismatch or fixed in actual Logic yet.

Actual installed-AU smoke added at `plugin/Tests/AuHostSmoke.cpp` / target NovoxAuHostSmoke.
Both installed identities passed AudioComponentInstanceNew + AudioUnitRender tests at44.1k/512:
131072 nonzero samples each, RMS0.171841, +/-10sample PPQ jitter maxdiff0 vs baseline,
real timeline seek maxdiff0, stopped silence, both identities' audio identical. This verifies
the wrapper/installed bundles, not the user's specific Logic callback behavior. Legacy and
smoke targets are macOS-only; no commits.

## Singing engine first iteration (October 1)

Jonathan confirmed the plugin clicking fix worked. Leave that playback path alone.
He requested Astra at extra-high to write a transcription spec and then implement.
Spec: docs/SINGING-TRANSCRIPTION-SPEC.md. Implementation is native `singing-v1`:
multiple measured YIN troughs, sequence path selection, symmetric analysis timing,
recording-end clipping, held-note segmentation, supported short octave/passing notes,
conservative initial scoops, and word conservation. C API/CLI/Python default remains
`legacy`; native app/plugin explicitly selects `singing-v1`.

Original evidence persists under song.transcription.{algorithm,transcriptWords,
unassignedWords}; original times remain seconds through edits and tempo changes.
The ordinary editor/Undo workflow remains; there is no intermediate review window.
Song's C++ JSON state preserves metadata so Logic reopening does not erase it.

Human-reference harness: tools/transcribe_benchmark.py, recording instructions and
manifest example: docs/SINGING-TESTS.md. Use tools/.venv/bin/python (native arm64),
not the default Rosetta Python against arm64 libnovox. Private recordings/annotations
belong in gitignored reference/. The harness compares both modes on identical audio
and fixed words, maximum-cardinality minimum-onset-error matching, strict events,
octave/other substitutions, fragmentation/repeated-note merges, boundary statistics,
word conservation and runtime. No Whisper variance in this benchmark.

1257 engine checks pass, including waveform harmonic changes, real160ms octave,
6Hz widevibrato, repeated valley,120ms scoop, quiet/silent audio, file edges and
16/44.1/48k timing; frame passing/octave/dropout; tiny Can overlap; invalid API input.
20 plugin checks pass incl metadata roundtrip. All standalone/AU/VST3 and legacyAU
built/installed. Native MP3 conversion passes15 checks, retains all10 recognized words
including Can. Browser fixture puts18 detected notes directly into the ordinary UI.
Actual existing take still has low-octave/bass tracking and pitch jumps; DO NOT claim
accuracy solved. Jonathan is preparing annotated recordings: next step measure each,
identify candidate/path/segmentation failures, and tune against held-out references.
No commit made; earlier approval boundary still applies.

## Measured iteration on life/sleep (October 1, afternoon)

See docs/SINGING-ITERATION-01.md for alignment, changes and limits. Frozen private
WAV/MIDI pairs+hashes and manifest in reference/novox-tests. Region offsets life1.5s
and sleep28*60/106s are established from screenshots plus MIDI metadata, not fitted
against detector output. New singing-v1 period preference0.30/unvoiced1.2 with
bounded40ms candidate-supported onset recovery.12 sleep events vs17 previous,14
life vs16 previous; exact60ms onset matches unchanged4/12 and6/15. Spectral checks
show some life semitone differences are present in audio vs intended MIDI; don't
force reference pitches into inference. Both references now tuning data, not held-out.
Engine/plugin/native/AU regressions pass; all app/plugin formats rebuilt/installed.
No commit. Next focus repeated notes and timing, preserving synthetic pitch gates.

## Syllable placement and key correction (October 1)

User approved further fixes after Astra review and key-setting idea. Added native
phrase-wide monotonic syllable alignment with soft word timings, using existing
notes only, fallback preserving hard cases/rests. Native sleep now re-/mem-/ber/me
instead of remem-/ber/me/hold. Melody benchmarks unchanged. Insufficient syllable
boundaries now emit a warning rather than silently squeezing without notice.

New ordinary editor key controls: Off default,12 roots,major/naturalminor,Gentle/Snap,
explicit Correct button; selected-or-all, changed notes selected,CmdZ Undo verified
in browser fixture. No automatic correction on import. Gentle preserves confident
chromatic notes, requires matched import evidence and nearest-scale distance<=.65st;
Snap explicitly snaps nearest, lower on ties. Continuous detectedPitch added to
transcription output. Original note evidence in transcription.originalNotes(beats),
originalNotesUnits; raw word times seconds. Song.keyCorrection preserves settings
and correction audit across C++ state/Logic. New module pitch-correction.js embedded
via CMake; Node tests in plugin/Tests/PitchCorrectionTests.mjs exercise allMIDI/key
combinations and gentle cases.1267engine checks,21plugin checks,native sleep and
installedAU smoke pass. Standalone/AU/VST3/compatAU rebuilt/installed. No commit.
Repeated-note/articulation remains unfinished; avoid globally lowering dip threshold
and reintroducing vibrato/tremolo fragmentation. Two references are tuning data.

## Life syllable-boundary iteration (October 1)

Recovered a third event for `anymore` at the measured 7.176s level valley,
without changing raw melody segmentation or pitches. In lyric alignment only,
a word with fewer events than dictionary syllables may split at a voiced local
level minimum with >1dB recovery on both sides within96ms, at least80ms from
both event edges, within its word interval and within1semitone of event pitch.
Flat vowels, unvoiced valleys and words with sufficient events do not trigger it.
This remains a heuristic supported by transcript demand, not a general syllable detector.

After fallback word/acoustic splits, rerun phrase assignment with soft onset/end
costs and a penalty for long rests inside a word group. Fixed life now reads
For/all/of/my/life/I/called/my/shot/hold/I/don't/know/a-/ny-/more (16 events).
Don't/know split remains approximate ASR6.18s, not claimed acoustic precision.
All three anymore pitches retain measured61/62/62; semitone disagreements
with intended MIDI remain. Sleep still12 events with re-/mem-/ber/me.

Fixed-word artifacts: out/novox-tests/life-articulation.json and
sleep-articulation.json; life-words.json freezes the earlier nativeASR words.
Raw melody benchmark unchanged6/15 life and4/12 sleep exact60ms pitch/onset
matches. Both fixtures are tuning data. Added flat/voiced valley/unvoiced valley,
duration conservation and long-rest word-alignment regressions;1276checks pass.

All standalone/AU/VST3/compatAU rebuilt and installed. Native life import:16checks pass and all13 ASR words assigned with full a-/ny-/more; plugin21checks, key controls, installedAU continuity smoke pass. No commit. Restart standalone and reimport life to test.

## Standalone/plugin parity and import lyrics/window size (October 1)

Jonathan requests plugin parity at every stage: treat every app feature/fix as
standalone + NovoxAU + VST3 + compatibilityAU delivery. Build NovoxTheRobot_All,
run processor regressions and installedAU smoke, and report any unavailable host
verification before calling a change ready. All formats share native import,
transcription and embedded editor sources; do not introduce a standalone-only lane.

Audio import populates song.lyrics from all transcriptWords (readable words), while
song.useNoteLyrics=true keeps the measured per-note tokens authoritative. Song
state preserves that flag and toScoreJson omits flowing lyrics in this mode, so
blank/humming notes stay hums and transcript words don't sing twice. Editing the
bottom text resets the flag and uses the existing lyric-flow behavior; Undo restores
both. Browser life fixture verifies readable lyrics, no leftovers warning, editing
and Undo. MIDI/MusicXML import resets the flag. Existing saved songs defaultfalse.

Toolbar Window size choices Normal1000x660/Large1280x800/Extra large1600x1000 call
native setSize, clamp to display userArea with margins and existing760x520 minimum.
JUCE bottom-right drag grip remains enabled; processor retains chosen dimensions
when editor closes/reopens during this instance's lifetime. Size is not serialized
in DAW state. Keep Aqua styling. No commit authorized.

Checks for this change:28plugin checks pass including native editor resize/reopen, imported-note lyric mode state roundtrip and normal-flow reset; installedAU timing/seek/stop smoke and key correction checks pass. Browser verifies imported text, editing and Undo. Clear resets useNoteLyrics so retained text can flow onto newly drawn notes. Final all-format build includes this UI tweak. No live Logic window resize interaction tested; native editor1280x800 and reopen retention tested.

## Robot icon, Mac Select All, timeline overview (October 1)

User's own robot reference generated a transparent icon concept; requestedwhite
version assets/icons/novox-robot-white.png used via JUCE ICON_BIG for Novox and
compatibilityAU bundle. Generated originals preserved under ~/.codex/generated_images.
No text/tile added. CMake automatically packages Icon.icns in standalone Resources.

Browser already had Cmd/CtrlA selection, but JUCE's Mac WebViewKeyEquivalentResponder
intercepts CmdA and invokes Cocoa selectAll before DOMkeydown. Own MacShortcuts.mm
installs an NSEvent local monitor scoped to this editor's NSWindow; CmdA dispatches
novox-select-all into its browser. DOM handler ignores active text/number/select
controls; native event continues so ordinary text selection works. Monitor removed
before editor destruction. No JUCE dependency source edits. Root CMake enables
both OBJC and OBJCXX onApple (OBJC is essential so whisper .m files aren'tcompiled
as C++). Sources addedto standalone/AU/VST3,compatAU and native testtarget.

Optional bottom Timeline disclosure expands72px overview: pitch marks, barlabels,
visibleviewport outline and playhead. Full extent includes notes+tail/currentview.
Click/drag centers editor; focused overview left/right arrows pan4beats without
nudging selectednotes. Defaultsclosed; size/display choices notDAWserialized.
Browser lifefixture:CmdA16notes,overviewexpanded,ArrowRightscroll0to262.5px,
notes/gapsvisiblyshown. Pending nativebuild/hostverification at timeofthisnote.

Final verification: allformats built/installed.28native processor/editor checks and both installedAU timing/seek/stop smoke pass. Isolated signed test app out/Novox UI Check.app (unique bundleID) verified via real Mac UI:CmdA selected16notes, Lyrics CmdAselectedfull56-character textwithoutselectingnotes, Timelineexpanded. AppIcon.icns packaged. No liveLogic editor shortcut interaction tested, but shared nativeeditor used. No commit.

## Standalone window chrome / Mac Options menu (October 1)

User requested removalofblueJUCEtitlebar,OptionsinMacsystemmenubar,andMac-style
close/minimize. Confirmed Large/Extra large pluginsizechoicesaresatisfactory;
keepthoseunchanged. StandaloneApp.cpp owns customJUCEapplication lifecycle,
adaptedfromJUCE8.0.15 StandaloneFilterApp (AGPLattribution),retains same user
settings,pluginholder,deviceinitialization,state save/restore,quitmodalcancellation.
SubclassofStandaloneFilterWindow enables native Mac titlebar, hides JUCEOptions
button by child TextButton lookup afterconstructorandresized. Native Options
MenuBarModel exposes same4actions via existinghandleMenuResult;unsetsMacmenubar
before window destruction. Standalone targetonly defines customapp macro and
adds source;AU/VST3/compatAU editor isunchanged. OtherplatformskeepexistingOptions
button. Native titlebar uses current macOS traffic-light controls, not fake Aqua
button art. Build/nativewindowverificationpending at timeofthisnote. No commit.

Verified real standalone Mac UI in isolated signed out/Novox Chrome Check.app:system Optionsmenu,all4items,Audio/MIDI Settingsdialog opens/closes,native close/minimize/zoom AXcontrols enabled,closebuttonworks. Existing16-note life project restoredwithoutchanges.28plugin checksandbothAUhostsmokepass. Finalstandalone rebuild changesmenuellipsis toASCII dots toavoidnativeencodingartifacts. No commit.

## Note editing hum preview (October 1)

User requested audible mmm onplacing/moving notes,current voice preferred.
Native previewNote bridge queues latest pitch on existing backgroundrenderworker,
rendering short{M}token with song.voice,20ms attack/release and reducedgain.
No fullsongrerender for preview-only requests (separate songRenderRequested flag).
Uses existing audition output/resampling with hoststopped. Playing ignores requests;
audio-thread transportstart cancels note audition so existing playback isn't replaced.
Audio-import review auditions retain their existingbehavior. No ASR/network.

UI45ms debounce ondrawstart,doubleclicknewnote,pitchchangesduringdrag,move release,
and arrowkeymoves. Resize-only/marquee/synthetic undo don't audition. Selected-group
moves preview only anchor. Same embedded UI/native engine acrossstandalone/allplugins.
Native regression checks stopped-host outputaudible,shorttailends,song unchanged,
onsetfade and transportstartcancel vs existing exact-playback checks. Buildpending.

Note-preview final allformats rebuilt/installed.32native checks pass,including audiblehum onstoppedhost,tailends,fadedonset,songunchanged,and startingplaybackmatchesoriginalengine despitequeuedhum. Both installedAUs exacttiming/seek/stop smoke pass. No liveLogic UI dragging/audio listening checked. No commit.

## Keyboard capture icon (October 1)

Replaced Capture text and LED with an inline SVG piano-keyboard icon. Kept the
existing arm/live behavior, accessible label, and tooltip. When enabled, a status
strip below the toolbar explains that playback must run while the user plays a
MIDI keyboard; it disappears when disabled. Browser checks verify both states.
All standalone/plugin formats rebuilt and installed; no commit authorized.

Jonathan also floated singing capture. This is not implemented. Suggested first
version: record a phrase, stop, convert with the current native audio-import engine,
and put notes/lyrics directly in the normal piano roll. This requires microphone
recording and plugin input routing; the current synth has only an output bus.
Keep standalone/plugin parity and avoid a placeholder microphone control.

Validation: 32 native checks pass twice after replacing a fixed 250ms preview wait with bounded polling of actual stopped-host output. Both installed AU smoke checks pass. Browser capture help shows/hides correctly. No singing capture implemented yet.


## Singing capture and native region drops (October 1)

Implemented the user's requested singing mode in the shared standalone/plugin editor.
The singing face/music-note icon starts input-only recording from the system default
microphone. First use requests microphone permission. The status strip shows a level
meter, elapsed seconds, stop instructions and Cancel. Stop encodes mono 16-bit WAV
and invokes the existing AudioImport pipeline; notes and readable lyrics appear in
the normal editor. Recording auto-stops/converts at 60 seconds. No microphone
monitoring; independent input device manager preserves the existing synth host buses.
Cancel discards the buffer. Keyboard capture is disabled while recording.

Added native MIDI/MusicXML file delivery alongside audio. Mac WKWebView drop handling
receives file URLs, modern NSFilePromiseReceiver promises and public.midi-audio bytes.
Promised/raw MIDI temporary files are removed after reading. Unsupported private Logic
payloads produce a visible export-and-drop explanation and log pasteboard type names.
One file/region at a time. This is protocol support, not proof that all Logic regions
provide those protocols; exact live Logic region dragging and trim fidelity remain
unverified. Do not claim opaque Logic regions are decoded. Runtime Objective-C handler
and WKWebView subclass names are unique per plugin target to permit both AUs in a host.

UI fixture checks: start shows meter/instructions; Cancel preserves song; stop uses
conversion and shows 16 fixture notes plus lyrics directly in the normal piano roll.
This used simulated microphone status and an existing real conversion result, not a
live hardware recording. Native tests check WAV encoding/decoding and invalid input;
Mac private-pasteboard tests cover file URLs, raw MIDI, cleanup and unsupported data.
All formats must be rebuilt and both installed AU smoke tests rerun before delivery.
No commit authorized.

Singing/drop final build: all standalone/AU/VST3 and compatibility AU targets rebuilt and installed. 39 native checks pass; git diff whitespace check passes. Hardware mic recording and exact Logic region payloads await user testing.
Both final installed AUs pass exact 44.1 kHz/512-sample playback, rounded PPQ, seek and stop smoke checks, without the earlier Objective-C class collision warning. No commit.


## Astra singer portraits (October 1)

User requested Astra-designed singer icons matching the robot app illustration.
Astra implemented 16 SVG painted-toy portraits in shared plugin/ui/app.js: uneven
black outlines, shaded solid paint, tiny scuffs, glossy eyes, cobalt/pink accents,
and the gray tilted Novox robot head with blue ears/dome and slotted grille.
Portrait-only CSS replaces blue circular backplate with white, removes drop shadows.
Voice labels, controls and animated .mouth ellipses preserved. Browser checked all
16 visible and selecting Robot updates chip, larger portrait and voice controls.
Preview assets/icons/singer-portraits-preview.svg/.png show the exact rendered art.
JavaScript syntax check passes. All standalone/plugin formats rebuilt for parity.
No commit authorized.


## Feature roadmap and phrase comparison first pass (October 1)

User authorized working through docs/FEATURE-TODO.md (all eight proposed features).
Started original comparison and selected phrase looping. Native audition supports an
explicit seconds range and loop; playSongReview uses current edited Rendered audio,
with renderer lead correction. Both route through plugin output with host stopped.
Invalid ranges rejected. Loop interpolation wraps to phrase start; tests cross in-block
and between-block boundaries. Editing preview hum does not replace a phrase loop.
UI refreshes looping robot after render completion; Stop and changing Loop end audition.

Shared editor adds Original/Robot/Stop, Loop checkbox, selected bounds, aligned waveform.
Import source audio/waveform stored in session map keyed by transcription.recordingId
so Undo selects the correct source. Original selection maps using import tempo; robot
uses latest render tempo. Does not put audio bytes in Song or note-edit bridge payloads.
Source persistence across editor close/reopen is intentionally still outstanding and
roadmap entries remain unchecked. Source audible comparison only after fresh import.
Exact live Logic loop workflow remains to test. No commit authorized.

Phrase comparison final validation: all formats rebuilt/installed; 43 native checks pass, including selected audio range and repeated loop boundaries, with invalid/empty ranges rejected. Both installed AU playback/rounded PPQ/seek/stop smoke checks pass. Browser real conversion fixture verifies source waveform, selected phrase display, Original/Robot range bridge calls and Stop. Native looping cancelled when host starts. No live listening in Logic performed. No commit.


## Plugin display name (October 1)

User requested Novox The Robot plugin name. Current Nvox AU/VST3 already used it;
legacy Nvsg compatibility AU still advertised New Virtual Singer. Set legacy
PLUGIN_NAME and DESCRIPTION to Novox The Robot while preserving PRODUCT_NAME,
bundle path, bundle ID and Nvsg identity so old Logic projects resolve correctly.
Both AU identities may appear with the same displayed name; retained intentionally
for existing projects. No cache deletion or host interruption. No commit.
Plugin name final: installed legacy AudioComponents name verified Song A Day: Novox The Robot, original subtype Nvsg retained. Both installed AU smoke tests pass. Broad auval inventory stopped because it loads unrelated third-party plugins; no cache reset performed. Restart Logic to refresh displayed name.


## Roadmap implementation: retained source, exports, song files, syllables (October 1)

Native processor stores source recordings keyed by thin transcription.recordingId;
storeSource/getSource bridge transfers audio once per import/reopen, never on note edits.
DAW state now wraps song/source/current tempo in novoxState:1 JSON inside unchanged
JUCE writeString envelope. Legacy plain-song state remains readable. Saving includes
only currently referenced source (session Undo cache older sources not all embedded).
53 expected native checks cover actual temporary .novox disk roundtrip, source bytes,
legacy state, exported MIDI tempo/timing and readable 24-bit WAV, plus existing loops.

File actions Open/Save/Save As/Export WAV/MIDI use native async file chooser; cancellation
is silent, malformed song files rejected, note changes flushed before file actions.
Drag WAV/MIDI uses JUCE native external file drag; destination may reference audio so
exports persist under Application Support/Novox/Exports/UUID rather than deleted temp.
Exact drag initiation and receiving behavior in live Logic still unverified. WAV removes
renderer lead to align timeline zero, clipping any consonant before zero; not claimed
lossless preroll. MIDI 960 PPQ includes tempo and note lyric meta events.

Syllable buttons: Split one selected note into equal-duration syllables entered with
hyphens; Join adjacent notes of same pitch (retains word spaces, joins hyphenated tokens);
lyric arrows swap with neighbor. Flowing lyrics pinned first so later assignments stay.
All Undo capable. Browser fixture checks split16->18 notes, join->16, remember token,
two Undos restore For/original16, lyric swap and Undo, source restored on editor boot.
Native file dialogs / live Logic still await real UI verification. No commit authorized.

Roadmap delivery validated: all formats rebuilt/installed; 53 native checks pass, including disk state/source roundtrip, MIDI timing and WAV decoding; both installed AU exact playback/rounded PPQ/seek/stop smoke checks pass. UI fixtures verify reopened source availability, silent save cancellation, syllable split/join/swap and Undo. Native file dialogs and drag into live Logic remain unverified. Old saved states lacking source audio cannot recover it without reimport. Next record-at-playhead/replacement, then expression and multiple parts. No commit.


## Desktop crash fix — Oct 1
User crash aborts in WebKit native popup creation / NSResponder setNextResponder.
Reproduced in isolated signed app copy; fresh crash additionally shows WKWebView
KVO corner-radius observation. MacRegionDrop previously changed a live WKWebView
class with object_setClass, breaking AppKit/WebKit-managed identity. Removed that
isa swap: install four drag overrides on the existing JUCE per-binary subclass,
walking past KVO subclasses; preserve and delegate original IMPs without touching
Apple base classes. Native drop handler association/lifetime remains unchanged.

Rebuilt NovoxTheRobot_All and tests; 53 checks / zero failures, both installed AU
exact playback / rounded PPQ / seek / stop checks pass. Fresh isolated signed copy
now exposes full WebKit accessibility tree and successfully opens/closes Window
size, Key, Scale, Correction strength and Snap native menus. Native zoom/restore
and clean quit also succeed. User running standalone left untouched; restart it
to load rebuilt executable. Live Logic region reception still unverified. No commit.

## Imports land at the region's timeline position (October 2)

User asked that imported MIDI/audio keep the dropped region's exact placement.
Logic's exports already carry it: MIDI regions keep absolute ticks (meta events at
the region start), and audio exports carry a Broadcast WAV bext TimeReference
(life.wav 01:00:01.5 = beat 3 at 120; sleep.wav 01:00:14.264 = beat 28 because that
project runs 120 bpm for 24 beats, then 106 — matching sleep.mid's two tempo events).

MIDI/MusicXML: new engine import option `"units":"beats"` (ImportOptions::beats)
returns positions straight from ticks/divisions, so tempo maps cannot shift notes;
the plugin always requests it. Default C/Python/JS import output is unchanged.
Audio: AudioImport returns timeReference (seconds) + originator from bext. The editor
places notes at that position when the host reports an edit origin (VST3), or for
Logic/GarageBand files assuming bar 1 at a whole hour (Logic default 01:00:00:00).
Seconds convert to beats through the host's own seconds/ppq pair at the playhead
(processor status now carries seconds/editOrigin), which absorbs earlier tempo
changes; a tempo change between the playhead and the region still skews placement
(sleep with playhead at bar 1 lands 28.53 instead of 28). Other WAVs (field
recorders, mic takes, MP3s) still start at beat 0. Source waveform and Original
phrase audition honor source.startBeat. 1283 engine checks, 62 plugin checks
(incl. bext round trip, status seconds/origin, tempo-change MIDI at beat 28), real
life/sleep WAV stamps read natively, both installed AU smoke checks pass. Live Logic
region drags still need Jonathan's test. No commit.

## Click on "for-e" in Forever Punk (October 2)

Jonathan heard a loud click between "for" and "e" (G4→F4, alto, 80 bpm) in
SongADay2026_working/10_October/2/2 Forever Punk.logicx (song state read-only from
ProjectData's jucePluginState). Cause: the notes left a 0.003-beat gap, the planner
emitted a 2.5 ms silence, and the synth clamps transitions to neighbor length, so the
loud vowel faded out/in in ~1 ms with instant formant snaps (sample steps 12× normal).
Fix in planner: kShortestRest = 40 ms. Syllable gaps under it are sung through, with
pitch legato using the same threshold (was 10 ms). Emitter holes under it, such as
a rest mostly used by an onset stop that leaves a 7 ms sliver before "punk", extend the
previous sound rather than becoming silence. Rests of 40 ms or more stay silent.
The real song had 6 such slivers; now 0, and for→e measures 2.2× instead of 11.6×.
A/B: out/forever-punk-click/. New regression test fails on old planner; 1288 engine
checks, 62 plugin checks, both installed AU smoke checks pass. All formats rebuilt and
installed; reload the plugin in Logic. No commit.

## singing-v2 ships in the plugin, with Detailed notes (October 16)

Jonathan's complaint about sung-vocal import: singing-v1 split words into notes in odd places and
invented octave jumps mid-word. Every plugin import (standalone, AU, VST3, compat AU Nvsg) now runs
`singing-v2` (design and tuning history: `docs/SINGING-V2-DESIGN.md`). Its default, "natural", gives
one note per sung syllable plus real melodic steps; scoops, short approaches, grace notes, vibrato and
droops stay part of their note, and the octave is decided per note.

**Detailed notes.** Jonathan asked for it as an option. Engine option `"detail": "natural" | "detailed"`
(C API, `novox transcribe --detail`, Python `transcribe(detail=...)`; `kDetailedRules` in
`engine/src/singing_segment.cpp`). Detailed keeps every natural note and splits each one further where
a held approach, grace note, quick step or settled droop holds its own pitch. Natural notes are
byte-identical to before. In the editor it's a "Detailed notes" checkbox under Import…, read when a
recording is opened (file, dropped file or region, singing take), remembered in the web view's
localStorage, not in the song.

**Plugin fixes.** whisper.cpp v1.8.3 asserted in its DTW median filter on a decoding window under
160 ms (CSD en023b, en012a), which would kill the DAW; `plugin/patches/whisper-dtw-short-window.patch`
is applied by FetchContent (upstream still asserts through v1.9.5). Words that whisper stacks at the
end of a file are clamped inside it (`AudioImport::wordsFromPieces`), which fixes the engine's
"invalid timestamped word" (en011b, en047a/b). The LCD shows the warning count and the most serious
warning, with the full list in its tooltip.

**Whisper repetition threshold reverted.** The plugin briefly used `entropy_thold` 1.5 to keep sung
"la la la" lines. On held-out full songs it let long loops through (CSD en020a's lyrics became
"1 liter, 2 liter…" ×6), and it gained nothing elsewhere, so `AudioImport.cpp` keeps whisper's 2.4.
The lab's default `words.json` is again exactly what the plugin sends (checked on stem-when-i-die).

**Evaluation corpora.** Four, each with a dev split (tuned on) and a held-out test split:
Jonathan's own takes (15/6, `reference/corpus/manifest.json`, lab `out/import-lab`), vocadito (29
amateur singers, every pitch step annotated), GTSinger (3 trained singers, 5 techniques) and CSD (one
trained singer, whole songs). The datasets are local-only (`ASSETS.md`); setup in
`tools/datasets/README.md`. Score dev with
`tools/.venv/bin/python -I tools/import_lab.py corpus [MANIFEST --lab LAB] --options '{"algorithm":"singing-v2"}' --tag NAME`
(add `"detail":"detailed"`). The test splits were scored once today; use them only to confirm.

**Held-out results** (COnP F, shipped words; v1 / natural / detailed): Jonathan 0.626 / 0.726 / 0.742,
vocadito 0.670 / 0.617 / 0.654 (human 0.713), GTSinger 0.652 / 0.702 / 0.740, CSD 0.718 / 0.879 /
0.791. On Jonathan's test takes, octave jumps inside words went 37 → 1. GTSinger and CSD generalise;
Jonathan's margin over v1 shrank from +0.30 on dev to +0.10; vocadito stays below v1. Full tables
are in the design doc's Evaluation section.

**Open issues.**
- Natural or Detailed as default for Jonathan's own takes: dev favoured natural (0.818 vs 0.786),
  the 6 test takes favoured detailed (0.742 vs 0.726). Jonathan's ears should decide.
- Natural swallows real steps in long melismas (dry-falsetto-born-knowing, 17–19 s).
- Under accompaniment the v2 tracker can follow the backing (acc-nostalgia: whole notes an octave or
  a fifth off).
- Whisper still loops on long inputs at 2.4 (a 5-minute concat of dev takes repeated one line for
  55 s). Worth a dev-only test of `wp.n_max_text_ctx = 0` (no conditioning on earlier text) or a
  repeated-phrase guard.
- The LCD cuts off the generic "N sung passage(s) matched no transcribed word…" warning at 800 px,
  and long songs show dozens of per-word warnings.
- Lab caches: CSD en011b, en012a, en023b and en047a/b need `import_lab.py cache` again (they failed
  before today's fixes).
- Stale `.orig` files from an earlier patch sit untracked in `docs/`, `engine/src/` and `tests/`;
  delete them before committing.

**Checks.** 3262 engine checks and 95 plugin checks pass, plus the UI node tests; all four formats
rebuilt and installed; the AU smoke test and `auval` pass for Nvox and Nvsg. Not checked here: live
Logic, VST3 in a host, the standalone GUI, a real microphone take, and whether the Detailed setting
survives reopening the editor inside a DAW. `.gitignore` now ignores `.env` files (oss-check). No commit.

## Number words, hyphen check and the held-note baseline (October 16)

Jonathan asked for numbers sung as words ("one hun-dred", not "100"), hyphens on every multi-syllable
word, and, next, automatic "_" (held notes).

**Numbers.** `syllabify` (engine/src/syllables.cpp, rules in the "Numbers and symbols read out" comment)
now reads every digit or symbol token whisper writes, and each number word is a sung word of its own:
cardinals to 999,999,999 without "and" ("105" one | hun-dred | five, "1,000" one | thou-sand), years
1100-2099 pair by pair except round thousands and 2001-2009 ("1999" nine-teen | nine-ty-nine, "2000" two |
thou-sand, "2005" two | thou-sand | five, "1900" nine-teen | hun-dred), ordinals ("21st" twen-ty-first),
decades ("1990s", "'90s" nine-ties), decimals ("0.3" ze-ro | point | three), times ("3:30" three |
thir-ty, "3:00" o'clock), $ £ € % # & ("$5.50" five | dol-lars | fif-ty | cents), longer digit strings and
leading-zero codes digit by digit, letters glued to a number ("49er," for-ty-ni-ner, was "er"; "3pm"
three | p-m; "4ever" four-ev-er). The letter W alone now shows its name "dou-ble-u" (the notes were "W-",
"-" and an empty, lyric-less note), and "hundreds" splits hun-dreds (a plural keeps its stem's compound
split; the only change across all 125k dictionary words). `SungSyllable::startsPart` replaces the
segmenter's hyphenPartStarts. Pieces are always letters; the plugin needs a rebuild to pick this up.

**Lab.** `import_lab.py hyphens TAG [MANIFEST]` scans stored note lyrics for digits and words split over
notes without hyphens: 0 problems on all four dev corpora (natural and detailed); the old runs had 4
dangling-hyphen W's in CSD en001a/b. metrics.json gains `heldNotes` (P/R/F of predicted vs reference "_"
by best overlap, plus where each reference "_" went: found / merged / asNew / asHum / missed, and
missedOnset = a reference new syllable under a predicted "_") and `heldWhere`; the corpus tables get an
hF column and every corpus/compare prints a `HELD` line. Dev scores are unchanged (tags s4-held-natural,
s4-held-detailed vs d-v2g, f-gd-dev); only CSD lyrics moved (en007b now "for- ty- ni- ner" as sung; the W's,
lyricW -0.001 because CSD's own pieces for W are "W-", "", "").

**Held-note baseline** (dev, headline reference with lyrics; vocadito has none):

| corpus | mode | P | R | F | ref "_" | pred "_" | merged | asNew | missedOnset |
|---|---|---|---|---|---|---|---|---|---|
| Jonathan | natural | 0.588 | 0.435 | 0.500 | 23 | 17 | 9 | 3 (+1 hum) | 4 |
| Jonathan | detailed | 0.278 | 0.696 | 0.397 | 23 | 54 | 3 | 3 (+1 hum) | 25 |
| GTSinger | natural | 0.575 | 0.397 | 0.470 | 282 | 193 | 124 | 45 | 66 |
| GTSinger | detailed | 0.460 | 0.688 | 0.551 | 282 | 420 | 43 | 44 | 179 |
| CSD | natural | 0.268 | 0.533 | 0.357 | 90 | 138 | 26 | 16 | 99 |
| CSD | detailed | 0.093 | 0.667 | 0.164 | 90 | 503 | 15 | 15 | 430 |

Natural misses held notes mostly by keeping a 1-2 semitone step on a held vowel inside its note
("merged": Jonathan fat 32.6 s, games 16.2 s, for 19.3 s, die 25.8 s; GTSinger 124 of 282). Lyric drift
("asNew") is a syllable placed one pitch step late (darling: "_" on "ling" at 7.41 s and "ling" on the
held step at 7.87 s in sleep; belong 4.26 s; harmless 24.0 s). Most predicted "_" on a new syllable are
the reverse: the syllable starts early, at the old pitch, and its "_" carries the new pitch (CSD natural
57 of 99, CSD detailed 323 of 430, GTSinger detailed 122 of 179); in detailed mode these are the split-off
approaches. Checks: 3478 engine checks. No commit.

## Lyric text and numbers (2026-10-16)

After an audio import or singing take the Lyrics box now shows the imported notes' own syllables,
not whisper's raw words, as text that flows back onto the same notes: `To for-ty for this beat`
(from "40"), `the day's _ too short`, `sun- _ set`, `mm` on lyric-less notes, a line per phrase (a
rest of a beat or more, or a capitalized word or sentence mark after three words). Whisper's words stay
in `transcription.transcriptWords`. While `useNoteLyrics` is on the text follows the notes (split,
join, move syllable, per-note edits) once the plan for those notes arrives; typing in the box turns it
off and the text flows as before, now landing every unchanged syllable on its old note.

- `plugin/ui/lyric-text.js` (new, embedded in the plugin): `labelNotes` (the old `relabel` body;
  "mm"/"{M}" sung as a plain hum now show as hums), `lyricsFromNotes` (also used by MIDI import, where
  a missing lyric still means "_") and `pinnedLyric`. Pinning a flow-split word used to write "-er"
  style labels as lyrics, which changed its pronunciation; it now writes "-" / "".
- Engine lyric flow (`flowLyrics`, planner.cpp): a word containing `~` stays on one note (`wanna~`;
  the mark is dropped, a lone `~` is a note with no lyric), and a token after `x- _` is still the tail
  of the word (it was split again by its own vowels). Needed for 72 one-note words like wanna, fire,
  our, Dina (0.6%) and 15 held tails (trea- _ sures). The editor adds `~` only where the plan shows a
  one-note word with two or more vowels.
- Checks: `node plugin/Tests/LyricTextTests.mjs` (pure) and `node plugin/Tests/LyricRoundTrip.mjs
  [TAG ...] [--score out/native-audio-review.json] [--bpm N]` (engine CLI + lab outputs): 504 dev imports
  (s4-held-natural/detailed, also d-v2g and f-gd-dev), 20,567 notes, every note keeps its label and both
  plans sing identical syllables, at 60, 120 and 180 bpm; plus synthetic and pinned-typed-lyrics cases.
  Without the `~` marks 29 of the 252 natural imports fail. Engine 3489 checks, plugin 95 checks
  (`--audio` en021a 711, a Jonathan take 266), AU smoke and `auval` Nvox/Nvsg pass; all formats rebuilt.
- Limits: a MIDI import's text still flows at once with no plan, so a one-note multi-vowel word there
  can split; a note lyric with a space ("I love" after Join) comes back as "Ilove" (sung the same).
  Not tried in live Logic. No commit.

## Region drags from Logic land at the region's bar (2026-10-17)

Jonathan, in Logic 12.4: a vocal region starting at bar 2 imported as if it started at bar 1. A
region dragged onto a plugin is not a WAV export: Logic writes a temporary AIFF named after the
region, with no bext. Its position is in the MARK chunk as marker names ("Tempo: 123.0", "Start",
"End", "Timestamp: <samples since 00:00:00:00 at the file's rate>"), plus a COMT comment
"Creator: Logic Pro 12.4" and Logic-private LGWV/ResU chunks (read from three drops Logic made
for another plugin; the Stem Splitter WAVs in a project's Audio Files have no bext either).

- `AudioImport::timelineStamp` (also used by the import) keeps the bext path first, then reads
  the AIFF/AIFC MARK and COMT chunks from the bytes, bounds-checked (JUCE's reader stops at the
  first unknown AIFC chunk after SSND, and Logic writes MARK last). `Timestamp: N` gives
  timeReference = N / file rate; the originator is the COMT creator, else "Logic Pro" (only Logic
  is known to write that marker). A WAV cue label of the same form also counts (originator ISFT).
- `timelineBeats` moved to `plugin/ui/audio-import.js` (pure, tested); app.js is otherwise
  unchanged, and the source waveform and Original audition use the same startBeat.
- Drop paths already pass the original bytes: native file promises and URLs ->
  `filesDropped` -> `loadFileAsData` -> pending audio; the web view's File path sends base64 of
  `arrayBuffer()`.
- `NovoxPluginTests --audio FILE --melody` runs a Melody only import and prints the stamp. The
  real drops: "2 Drums mixed merged.5" 158819159 -> 01:00:01.3415 -> beat 2.75 at 123 bpm;
  "2 Drums mixed_1.1" 159512927 -> 01:00:17.0732 -> beat 35; "2-Step Jump Beat 02" 161083317 ->
  01:00:52.6829 -> beat 108 (bar 28; no sung notes in that loop). Two land exactly on a beat,
  which supports Logic's bar 1 at 01:00:00:00.
- Checks: 123 plugin checks (Logic-style AIFF import 3601.5 s / "Logic Pro 12.4", AIFC, no
  creator, no or non-numeric markers, overlong name, oversized and truncated MARK, WAV cue label),
  UI node tests (Logic drag stamp at 120 bpm -> beat 3), all formats rebuilt and installed, AU
  smoke and `auval` Nvox/Nvsg pass.
- Not verified live: reload the plugin in Logic and drag the region again. Placement in a host
  still assumes the AU's playhead seconds count from bar 1; if notes still land at bar 1, check
  the status seconds Logic reports. No commit.

## Editor redesign: "Stage" layout (2026-10-17)

Jonathan picked design C ("Stage") from the three mockups, with three tweaks: smaller default lyric
type (like B's sheet), bigger singer portraits, and a hover label that says who each singer is.
It replaces the old toolbar + voice panel layout in `plugin/ui` (index.html, style.css, app.js; the
mock in backend.js records `fileAction`/`setEditorSize` and its demo lyrics have a line break).
Native bridge calls are unchanged; PluginEditor.cpp is unchanged (1000x660 start, 760x520 minimum
still fit).

- Layout: top bar (Open/Save/Save As…, Import… + Detailed notes, Export menu, WAV/MIDI drag tiles,
  LCD with the standalone tempo, keyboard and singing capture icons, Play); a band with the current
  singer (face, name, description, Tweak popover) and a strip of all 16 portraits; the roll; a tall
  ruled lyric sheet on the right with a small legend; one bottom bar (hint/note info, Key, Edit,
  Compare, Snap, zoom, View with timeline overview and window size). Below 960 px wide the drag
  tiles move into the Export menu and the portrait names hide; at 900 px and below the gutters,
  singer card and lyric column shrink.
- Lyric type is `--ly-size`/`--ly-line`: 13/22 px at Normal and narrow, 14/24 at Large, 15/26 at
  Extra large (B's 15 px Palatino renders about as wide as 13 px Lucida Grande; C had 14/24, 15/26,
  16/28).
- Portraits: 44 px with 9.5 px names at Normal (36 px, no names before), 54 px at Large, 64 px at
  Extra large, 36-46 px without names below 960 px. The band is 81 px at Normal (73 before); the
  roll is 694x477 at 1000x660. The band's height doesn't change with the chosen singer.
- Each portrait has `title` "Name · description" plus an in-page Aqua help tag with the same text,
  shown on hover and keyboard focus and nudged to stay inside the window (`placeTip`); aria-labels
  stay. The strip is one radio group: Tab lands on the chosen singer, arrows/Home/End choose
  (handled before the roll's arrow-key nudge and Space-to-play).
- Bottom-bar hint shortened so it fits at Normal; the empty-song hint says "keyboard icon" instead
  of "Capture". README's editor description updated.
- Checks: a mock-backend browser suite (72 checks: play/stop, draw/select/move, lyrics flow and
  the mirrored note-lyric text after split and after a MIDI import, singer click and keyboard,
  hover tag/title/aria, Tweak + Reset, Key, Snap/zoom/Fit, Edit split/join/move/clear, Compare,
  timeline, window sizes, all file actions and drag tiles, Import… and Detailed notes, both capture
  icons, ⌘A and ⌘Z, no overflow, roll size, name spacing) passes at 1000x660, 1280x800, 1600x1000,
  960x660, 900x600 and 760x520 (run in real time; Chrome's virtual-time mode can deliver the
  syllable dialog's close event twice). UI node tests, 123 plugin checks, AU smoke and `auval`
  Nvox/Nvsg pass; all formats rebuilt and installed. Screenshots: `out/design/final-*.png`.
- Not tried in a live host yet: the WKWebView in Logic should look the same, but the native title
  tooltip may also appear about a second after the in-page tag. No commit.

## Region-by-region import (2026-10-17)

Jonathan's vocal track is split into many regions on purpose. He wanted to drag one region in,
then the next, without the second removing the first. Until now every audio import (file, Logic
region drag, singing take) replaced the whole song.

- **Add to song**, a checkbox above **Detailed notes** beside **Import…** (the two now sit one above
  the other, one line each). It's on by default and remembered in the web view's localStorage
  (`novox.addToSong`, guarded like `novox.detailedNotes`), not in the song. Off: imports replace the
  song exactly as before.
- **Merge rules** (`mergeImport` in the new `plugin/ui/song-merge.js`, pure, embedded via
  NovoxUiData). An import's span is [start, start + its recording's length in beats], where the
  length is the later of the file's duration and its last note's end, at the current tempo. Notes
  that start inside the span (start <= s < end) are removed. A note that starts before the span and
  runs into it is shortened to end at the span start, or removed if less than `MIN_KEPT_BEATS` (1/8
  beat) would be left. Notes after the span stay, and the import's notes join. A word left open
  just before the span (`hel-`, or a pinned middle note `-`) is closed (`hel`, `""`) whenever the
  import's notes now follow it (its next note was removed, or the import sits in a gap before it),
  so it can't glue onto the import's first word. Lyrics flowing from typed text
  (`useNoteLyrics` false) are pinned first: each note gets `pinnedLyric` of its label from a plan of
  exactly these notes (the editor pushes and waits for one); a note without a label keeps its own
  lyric. Afterwards `useNoteLyrics` is true and the text is `lyricsFromNotes` of all notes, which
  the plan later refines with `~` marks.
- **Placement** (`importPosition` in audio-import.js): the recording's timeline stamp when it has
  one (`timelinePosition`, which now returns null for "no stamp" instead of 0); without one the
  playhead (`status.beats`: the host's, or the standalone's, which stays where Stop left it) with
  Add to song on, beat 0 with it off. The transport is snapshotted when the recording arrives (the
  file drop; for a singing take, when singing started) and handed through `audioImport.open` to
  `acceptRecording`, because converting takes seconds and a running transport moves on.
- **Starter song**: with Add to song on, an import into the song a new instance starts with
  (`Song::defaultSong`, mirrored as `STARTER_SONG`; a node test parses Song.cpp so they can't drift)
  still replaces it, otherwise every fresh instance would keep "hel-lo, I'm a new" before region 1.
  Any change to its notes, lyric text or note lyrics makes the song the user's; the voice may
  differ. The mock's demo song is its starter (`B.starterSong`).
- **Records**: `song.transcriptions` (oldest first) replaces `song.transcription`. Each record is the
  engine's `transcription` plus `recordingId`, `name`, `bpm`, `startBeat`, `beats`, `originalNotes`
  (beats). Beat values are rounded to 1e-6 so JUCE's 15-decimal JSON returns them unchanged (an
  unrounded `originalNotes` value could make pullSong see a different song and add an Undo step).
  An earlier record the new import overlaps is dropped when no song note starts in its live part
  any more (all of it inside the new span, or a sliver of a take the retake didn't quite cover). Old songs: `normSong`
  migrates `transcription` to a list of one and keeps a non-enumerable `transcription` accessor
  (latest record; setting it sets a list of one) for code that reads it; only the list is saved.
  `Song` (C++) reads `transcription` the same way, writes only `transcriptions`, and has
  `recordingIds()`. Key correction uses every record's `originalNotes`, latest first.
- **Sources**: the processor still keeps every stored source for the session (Undo/Redo), and
  loading a state (`.novox` Open, host reload) adds its sources instead of clearing the others, so
  an editor Undo back to the song from before the load can still play and save its recordings. The DAW
  state and `.novox` files now save `"sources": [{"id", "source"}]` for every recording the song's
  records name, each once; recordings Undo took back aren't saved. States with the old
  `sourceId`/`source` and bare-song states still load. The load limit for states and `.novox` files
  went from 100 MB to 512 MB (`NovoxProcessor::maxStateBytes`), so a song built from many recordings
  can always be reopened.
- **Compare > Original**: `originalSegments` gives pieces `{id, from, to, at}` for the phrase. Each
  import plays its live part: its span minus the spans of later imports (`liveRanges`), since those
  replaced its notes there; the phrase starts with the first piece. New native function
  `playSourceReview(pieces, loop)` -> `NovoxProcessor::auditionSources` decodes only the needed
  ranges from the stored sources (no audio crosses the bridge any more), mixes at 16 kHz (overlapping
  pieces add, scaled down only if they'd clip; at most 10 minutes) and plays like the other
  auditions. `playAudioReview` stays for compatibility. Robot is unchanged.
- **Display**: the source waveform draws each import's live part at its position with a blue tick
  at its start; the timeline overview has a grey bar under the notes for each. After an added import
  its notes are selected and scrolled into view (zooming out only if they don't fit); the LCD says
  `ADDED ✓ · 6 notes at 6.1.1 · name` (position before the name so the standalone's short LCD line
  still shows it), and the tooltip says how many earlier notes it replaced.
- **Mock backend**: audio imports now convert (`?model=0` shows the model dialog first). The words
  and pitches depend on the file name ("take B" differs), and `@barN` in the name stamps it like a
  Logic region at bar N. Also mocked: `storeSource`/`getSource` and recorders for
  `playSourceReview`, `playSongReview` and `stopAudioReview`. Stop now leaves the playhead where it is
  (Play restarts from the top), like the standalone.
- **NovoxPluginTests --regions FILE... [--melody]** converts real files natively, places each at its
  stamp as the editor does with Logic's playhead at bar 1, round-trips the state and plays Original
  across them. It writes the conversions (without audio) to `out/native-regions.json`. On
  `reference/novox-tests/life.wav` + `sleep.wav` (real Logic bext stamps), life lands at beat 3 and
  sleep at 28.53. The state is 704 KB for 16 s of audio, and Original plays each at its place (rms
  0.081 / 0.068) with silence between. The editor's `importPosition`/`mergeImport` on those two
  conversions gives "For all of my life I / called my shot / I don't know an-y-more / When you go to
  sleep re-mem-ber me dar- _ ling"; re-importing life swapped only its span.
- **Checks**:
  - `node plugin/Tests/SongMergeTests.mjs` (new): placement, spans, shortening and removal, retakes,
    pinning and word closing, records, purity/Undo snapshot and JUCE number round trip, old songs,
    Original pieces, starter song. The other UI node tests pass, and AudioImportUiTests covers the
    ADDED summary and `timelinePosition`.
  - 166 plugin checks. New: three-source state and `.novox` round trip (unreferenced source left
    out, each source once), old single-source state, transcriptions round trip and legacy
    migration, Original over two sources in a row and mixed, refused pieces, and every module an
    embedded script imports is embedded.
  - A mock-backend browser run (headless Chrome over CDP, 50 checks, real time), at 1000x660:
    region at bar 2 replaces the starter; bar 6 joins (both present, lyrics column a line per
    region); take B of region 1 swaps only that span and drops region 1's record; Compare Original
    whole song (2 pieces) and selection (1), Robot; Undo x3 steps back one import at a time (notes,
    lyrics, records, waveform) and Redo x3; an unstamped file after Play/Stop lands at the playhead;
    Add to song off replaces everything at beat 0, and Undo; typed lyrics are pinned then mirrored;
    Detailed flag reaches the bridge; MIDI still replaces (records cleared, Undo restores them);
    Clear empties everything; `?model=0` dialog -> Melody only; no page errors. The top bar fits
    with no overflow at 1000x660, 1280x800, 1600x1000, 960x660, 900x600 and 760x520.
  - All formats rebuilt and installed; NovoxAuHostSmoke and `auval` pass for Nvox and Nvsg.
- **Not verified**: live Logic (drag two regions of a real vocal track, then a retake), VST3 in a
  host, the standalone GUI by hand, and the new `playSourceReview` bridge call in a real WKWebView.
  The mock and the C++ tests each cover their side. MIDI/MusicXML imports still always replace the
  song; merging MIDI regions the same way would be a small follow-up (the span would be the notes'
  extent). No commit.
- **Review fixes (same day)**: (1) an import placed in a gap inside an open word ("hel-" ... "lo")
  glued its first word onto "hel-"; the word is now closed. (2) A retake trimmed slightly shorter
  than the take it replaces kept the old record for the leftover sliver: its whole recording stayed
  in the project and Original played the old take's tail after the new one. (3) A file or take
  without a stamp landed where the playhead had moved by the end of the conversion, not where it
  was at the drop or when singing started. (4) `setStateInformation` cleared every stored source,
  so after opening a `.novox` file, an Undo back to the previous song lost its recordings (Original
  "missing", and the next save dropped them). Tests: SongMergeTests (gap word, trimmed retake,
  untouched records), AudioImportUiTests (the drop-time transport reaches acceptScore), and a
  plugin check that a state load keeps the previous song's sources for Undo.

## Compressed vocals: inhale whistles read as notes 3 octaves up (2026-10-17)

Jonathan heard huge multi-octave jumps on a region import and sent two bounces of the same take
(`SEND FOLDER/bigjumpvocals.wav` through its vocal chain, `bigjumpvocals-nofx.wav` dry; lab cases
`bigjump-fx`, `bigjump-nofx`, `bigjump-fx-fixed`). Only the effected bounce had them: notes at
A#6-B6 (93-94) before "go", "know" and "blow", one taking the word "know" from its vowel.
- **Cause**: his inhales whistle (a lone ~1.75-1.85 kHz tone in breath noise). Dry they sit 50-65 dB
  under the singing, below the silence floor. The chain's compression lifts them +49 dB (vowels +5
  dB), the tracker voiced them as A5 (the tone's sub-multiple) and the note stage moved them to A6.
- **Fix** (`dropTonalBreaths`, singing_pitch.cpp; constants under "Tonal breaths"): between the two
  decoding passes, first-pass voiced runs that are mostly tone-only frames (nothing under 1 kHz
  within 20 dB of the top partial) with their tone 24+ semitones over the take's median reading
  become silence, unless they total more than 4 s or 5% of voiced frames (a whistled passage).
  No voiced frame of vocadito, CSD, GTSinger or the Novox corpus is tone-only (~1M frames
  scanned), and every dev score is identical (tag `breath3` vs `s4-held-natural`). A periodicity
  guard for lip whistling was tried and dropped: one inhale whistle reached 0.98.
- **Result**: the real plugin import of the effected bounce now tops out at 68 (was 94), 129 notes
  (was 138); the dry bounce is unchanged. Tests: an inhale whistle between two phrases leaves no
  voiced frames (fails without the fix), a 1 s whistled passage stays, a soprano's C6 stays.
  3501 engine checks pass; all formats rebuilt and installed.
- **The dragged region** (`~/Desktop/1 #01 (Vocals)_bip.aif`, Logic's file for a region dragged to
  the Finder; lab `bigjump-drag`, `bigjump-drag-final`): the region is a Bounce in Place ("_bip"),
  so the vocal chain's compression is printed in the audio itself: inhales -13 to -18 dB as in the
  effected bounce (dry -64 to -69), spectrum the effected bounce's +2.3 dB in every band. The
  channel strip's reverb/delay is not in it (mono; -98 dB in a gap where the effected bounce has
  -51), so a drag still carries no channel-strip effects; it carries whatever the region's file
  has. The fix as first built missed it in three ways, now fixed:
  - `assignNotePitches` gives a syllable the path voices nowhere the centre of its frames' best
    candidates; dropped breath frames still had theirs ("know" 93). Dropped frames now lose their
    candidates, and that fallback leaves a syllable unpitched (a neighbour's pitch) when its centre
    is more than 19 semitones from the take's median (`kUnvoicedReach`; all four corpora have 16
    such syllables over both splits, the one past 19 was GTSinger "I" at 78 for 42).
  - Breath noise under 1 kHz hides part of a whistle from the tone-only test, so a run with a far
    tone-only tone also drops its frames peaking within 1.5 semitones of that tone
    (`kBreathSameTone`); "row" had been placed on such a breath, and is back on its vowel.
  Real plugin import of the dragged file: before, 6 notes at 70-94 (4 at A6-B6 holding the words
  "know", "prove", "glow", "Where"); after, one lyricless 0.11 s note at 70 (23.8 s, the onset of an
  inhale read from 1.4 kHz noise). Dev scores on all four corpora unchanged (`breath5`); 3501
  engine checks and the plugin binary's 543 checks pass; all formats rebuilt and installed.
- **Open**: inhales without a clean tone still give an occasional short lyricless note, and in the
  effected bounce reverb/delay tails extend notes. No commit.

## Space plays and stops the host from inside the plugin (2026-10-17)

Jonathan asked for Space to start/stop the DAW while the plugin window has focus. No plugin API
controls a host's transport (JUCE's `AudioPlayHead::transportPlay` only works for iOS Inter-App
Audio), and the editor's WKWebView takes every key press. So in host mode the page's Space (not
typing, not a key repeat) calls the new `hostSpace` native function, and `forwardNovoxHostSpace`
(plugin/Source/MacShortcuts.mm) sends a fresh Space key-down/up to the host view above the editor,
made first responder for the call and then restored. This is how JUCE's AU wrapper passes keys to
Live. The standalone app keeps Space for its own Play/Stop. A focused character portrait no longer
takes Space (Enter still picks one), so Space plays after clicking a portrait.
- **Checks**: mock browser run with `?host=1`: Space on a portrait and on the page calls
  `hostSpace`, typing in the lyrics keeps the space, key repeats are ignored. Without `host`: Space is
  `setTransport` and Enter picks a portrait. 171 plugin checks, the UI node tests and the AU smoke
  test pass; all formats rebuilt and installed.
- **Not verified**: Logic itself. Logic runs Novox in AUHostingService (its temp folder is under
  `T/AUHostingService/`), so the "host view" is the service's container, and it is untested whether
  an unhandled key there reaches Logic. If it does not, the next things to try are sending the key
  up the responder chain without changing first responder, or giving up first responder (so
  Logic's window takes keys) whenever the page is not typing. No commit.

## Faster lyric edits, ruler playhead, key detection and grid snapping on import (2026-10-18)

**Lyric edits felt like "change, wait".** Every edit pushes the song; the render thread then rendered
the whole song (0.6-1.2 s for a 2-minute take, measured through the Python binding) while holding
SharedEngine's one mutex, and the plan the piano roll's labels need (3-5 ms) waited for that lock on
the message thread. `SharedEngine::render` now holds the lock only while it creates the stream (a
stream owns its plan and voice; the engine has no other mutable state, checked) and renders in
16k-sample blocks, giving up when a newer song is waiting (`songRenderRequested_`), so the next
keystroke's render starts at once. Labels now follow an edit by the 150 ms push debounce plus a
few ms. Not measured inside a DAW.

**Playhead.** Clicking or dragging the ruler places the playhead on the grid (standalone): Play starts
there and Stop returns there (`setPlayhead` → `NovoxProcessor::setPreviewCursor`; the audio thread
applies `previewSeek_`, which also replaces the old message-thread write of `previewSamples_`).
Inside a host the ruler shows a hint: no plugin API moves a host's playhead. Options discussed with
Jonathan: ARA (ARAPlaybackController can locate/start/stop in Logic, but means becoming an ARA
effect on the vocal track), MIDI Machine Control through a virtual MIDI port (Logic: Listen to
MMC Input), or Novox-only audition from the clicked point. None built.

**Key detection and Snap to grid** (plugin/ui/note-shaping.js, NoteShapingTests.mjs). Every audio
import detects the song's key (the major-scale set holding the most sung time, Krumhansl-Kessler
profiles for ties and major vs relative minor) and sets Key to it unless a key was chosen there by
hand (`keyCorrection.auto`). Snap to grid (top bar, on by default) puts the import's notes on the
editor's Snap grid (1/16 by default): starts and ends on lines, consecutive syllables on
consecutive lines, slivers closed. "Snap imports to this key" (Key menu, off by default) also moves
out-of-key notes. Both are an Undo step after the import's, so one Undo returns the notes as sung;
Key › Correct now finds a note's detected pitch by overlap (`evidenceFor`), so it still works on
snapped notes.
- **Why key snapping is off by default**: on the four dev corpora, snapping every import to its
  detected key lowered pitch accuracy everywhere except CSD (children's songs): notes with the right
  pitch, Jonathan 549 → 485 of 642, vocadito 815 → 686 of 1126, GTSinger 2022 → 1793 of 2623, CSD
  5457 → 5440 of 5797 (Gentle: 541, 783, 1971, 5502). The references themselves have 11% (Jonathan),
  16% (GTSinger), 24% (vocadito) and 2% (CSD) of their notes outside their own best key, and
  snapping moves every one. Detection names the reference's own scale for 12 of 15 Jonathan dev
  takes, 38/46 CSD, 97/153 GTSinger, 14/23 vocadito.
- **Checks**: 172 plugin checks, all UI node tests (NoteShapingTests new), AU smoke; mock browser
  run: ruler click places the playhead at 5.75 (1/16), Play starts there, Stop returns; host mode
  only shows the hint; imports report "on the 1/16 grid" / "Key A major"; Undo once gives the
  notes as sung, twice the song before; top bar fits at 760x520 (Snap to key moved into the Key
  menu for that). All formats rebuilt and installed. No commit.

## The DAW's transport from the plugin over MMC (2026-10-18)

Jonathan chose MIDI Machine Control for moving Logic's playhead and starting/stopping it, and
accepted the per-project Logic setting it needs.
- **Native** (`plugin/Source/HostMmc.*`): one virtual MIDI port per process ("Novox The Robot", or
  "… 2" when another process holds the name), opened by each plugin instance in a DAW
  (`NovoxProcessor::inDaw`).
  - **Messages:** Play 02 and Stop 01 to device 7F. A locate is an MTC Full Frame (which Logic's
    guide documents) followed by MMC Locate 44 (which Logic follows without subframes), both in whole
    frames rounded to the nearest. Bar 1 is the host's edit origin, else Logic's and GarageBand's
    01:00:00:00.
  - **Frame rate:** an AU's reported plain 24 fps is treated as unknown. JUCE's AU wrapper reports
    that when the host leaves the timestamp's SMPTE field empty.
  - **Bridge:** `hostTransport`, `hostLocate(seconds, fps)` → `{sent, frame, fps}`, and
    `setMmcState`. Status adds `daw`, `blocks` (processBlock calls), `frameRate`, `hostName` and `mmc`.
  - **Saved state:** the learned MMC state (works, offset, rate) is saved with the DAW project
    (`writeState(…, true)`), not in .novox files.
- **Page** (app.js, "the DAW's transport over MMC"):
  - **Ruler clicks:** a click in a DAW locates there. Clicks are coalesced. Evidence counts only from
    real moves while the DAW calls the plugin; Logic stops calling plugins on tracks it isn't playing.
  - **A missed landing:** it teaches the project's frame rate (when the sent frame at a standard rate
    explains it) or else the bar-1 offset, then the locate goes again (twice at most).
  - **Space:** uses MMC once it's known to work. Play and Stop are checked and fall back to the key
    press only on positive evidence. The hints name Logic's setting.
- **Review** (workflow `mmc-review`, 18 agents): every verified finding is applied:
  - per-project scope;
  - Full Frame;
  - AU frame rate;
  - nearest-frame rounding and frame-based tolerance;
  - false confirmations;
  - locate timeouts and checks that never expired;
  - rapid clicks;
  - playing misses;
  - Stop verification;
  - double toggles;
  - the port's lifetime and unique ID;
  - a loopback test that no longer sends MMC.
- **Facts the review established from Logic 12.4** (sources in the workflow output):
  - the input ID is 127 (All), so all-call works;
  - Logic is not sandboxed, and AUHostingServiceXPC has no app sandbox, so it can create the port;
  - Play and Deferred Play behave the same;
  - the setting lives in SongSettingsSync.nib, i.e. per project.
- **Checks:** 189 plugin checks, including byte tests (25 fps, 30, 29.97 drop frame, Full Frame), a
  CoreMIDI loopback on a private test port, and per-project state that stays out of .novox files.
  Also UI node tests, the AU smoke test, `auval` and 3501 engine checks. The mock DAW
  (`?host=1&mmc=1`, with `mmcBias`, `mmcRate`, `mmcLatency` and `frozen`) checks landing and saving;
  learning 25 fps and a 2 s bar-1 offset, then landing within a frame; coalesced quick clicks;
  frozen blocks (one "Did it move?" hint, Space stays a key press); and a DAW that ignores MMC (hint).
- **Untested in Logic:**
  - whether Logic enables a port created after launch (if not: Logic › Settings › MIDI › Inputs);
  - whether locates with other rate bits change the project rate when "Auto detect MTC format" is on;
  - whether a second Stop sends the playhead to the start.

  No commit.

## Old audio playing over Logic: reviews and render staleness (2026-10-18)

Jonathan heard clicks and, on "gate" (a long held G#2 in the piano roll), a pitch stepping down
(`SEND FOLDER/adventure-test.mp3` and `.mid`).

**Analysis.**
- **The engine renders the MIDI correctly.** "gate" holds G#2 in every voice. A render in 16k
  blocks is bit-identical to a single pass, so today's chunked rendering is not the cause.
- **The MP3 matches a render of the current song only in pieces.** Matching stretches sit at offsets
  that wander by 0.15-0.35 s at note boundaries, which is the same syllables at other note timings.
  The robot content starts about 12.3 s before its place on Logic's timeline.
- **"gate" holds a clean F2, then a D2.** That is periods of 11.25 and 13.3 ms; the audio is not a
  stuck buffer (consecutive chunks correlate only 0.7-0.9). The likeliest source is a song version
  before the edit, with the sung fall still in it.

**Cause in the code.** Compare > Robot (`auditionSong`) plays a copy of the render taken at the
click. `processBlock` ended note previews and loops when the transport ran, but not a one-shot
review. So a review started before Logic's playback kept playing instead of the song, out of step,
until its end, and edits never reached it. Separately, today's render cancellation could, under a
continuous stream of song changes, keep every render from finishing.

**Fixes.**
- **Reviews end when the transport starts** (`transportStarted_`, audio thread).
- **A Robot review ends when the song changes** (`Audition::song`, in `setSongJson`; a looping one
  restarts on the new render as before). The page drops its review mode when the transport starts.
- **Render cancellation is bounded.** A render is cancelled for a newer song only while the
  transport is stopped and the last finished render is under 1 s old (`kStaleRenderMs`).

**Checks.** `renderedVersion()` was added for tests. New plugin checks:
- a review stops when the transport starts, and playback is then sample-identical to the song;
- an edit silences a review;
- edits every 100 ms on a 600-note song still yield a fresh render.

Each check fails with its fix disabled and passes with it. In total there are 195 plugin checks,
plus the UI node tests, the AU smoke test and `auval`; all formats are rebuilt and installed.

**Not settled.** Jonathan had not clicked Compare, so the review bug was not this case. After the
rebuild he could not reproduce it either. The remaining suspect is today's render cancellation
(stale audio under a stream of song changes), now bounded. That doesn't fully explain the robot
running about 12 s ahead of Logic's bars. If it returns, get a bounce of the Novox track alone,
what was edited just before, and whether the LCD said RENDERING. No commit.
- **Out-of-date indicator** (same day, Jonathan asked for it to catch the problem if it returns).
  - **What the plugin reports:** status carries `renderedVersion` (the song version of the audio
    being played) and `renders` {started, cancelled, secondsSinceDone}. It also reports `reviewing`,
    which is true while a Compare review sounds.
  - **Display:**
    - after 3 s behind the edits, the LCD shows "OUT OF DATE · you're hearing an earlier edit · N s"
      (red). Its tooltip has the versions, render counts, transport state and any review.
    - the READY tooltip keeps a session tally ("fell behind N×, longest, last at").
    - a Compare review audible while the DAW plays shows "REVIEW ♪".
  - **Checks:** the mock has `?stale=1`; the status versions and counters have plugin checks. There are
    198 plugin checks; the UI tests, AU smoke test and `auval` pass, and all formats are installed.

## Release prep: 1.0 beta installer and songaday.studio/plugins (2026-10-19)

Jonathan wants a public release, with the app and plugins downloadable from songaday.studio/plugins
by people who never look at GitHub. Pro Tools is dropped. His decisions:
- Apple Silicon only.
- "1.0 beta" (project VERSION 1.0.0, `NOVOX_VERSION_LABEL`).
- He will join the Apple Developer Program; until then builds are unsigned.

**Installer.** `scripts/package-mac.sh` (and `packaging/macos/`; its README covers signing):
- **Build:** it builds `build-release/` with `NOVOX_INSTALL_AFTER_BUILD=OFF`. That's a new option
  (default ON for development) that also drops the compatibility AU from releases.
- **Targets:** `CMAKE_OSX_DEPLOYMENT_TARGET=13.3`; whisper's ggml-blas calls `cblas_sgemm`, which
  is new in 13.3. Before this, builds required the build Mac's own macOS (15.5).
- **Chip tuning:** `GGML_NATIVE=OFF` and `GGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16`. GGML_NATIVE
  was ON, tuning whisper to this Mac's chip.
- **Contents:** one productbuild installer, `out/release/Novox-The-Robot-macOS.pkg` (19 MB), with
  a SHA256SUMS. It installs the app in /Applications and the AU/VST3 in /Library/Audio/Plug-Ins. It
  is arm64-only, requires macOS 13.3, and has welcome, license and finish pages.
- **Install behaviour:** the bundles are not relocatable. The AU postinstall restarts
  AudioComponentRegistrar.
- **Signing:** ad hoc, unless `NOVOX_SIGN_APP` / `NOVOX_SIGN_INSTALLER` / `NOVOX_NOTARY_PROFILE`
  are set. The app gets the hardened runtime's audio-input entitlement.
- **Dropbox gotcha:** files in Dropbox carry com.dropbox.attrs plus com.apple.provenance, which
  can't be removed. pkgbuild turned those into stray "._" AppleDouble entries (32 of them) and printed
  "write: Permission denied". The script stages in a temp folder and rebuilds each component's
  Payload (cpio with COPYFILE_DISABLE) and Bom (root:wheel) itself, then fails if any "._" remains.
- **Verified:** payloads are clean; signatures pass; every binary is arm64 with minos 13.3; nothing
  links outside macOS; Info.plist version 1.0.0; the microphone text and entitlement are present.

**In the editor.** The View menu shows "Novox The Robot 1.0 beta · free and open source ·
songaday.studio/plugins" (status `version`).

**Download page.** In the `songaday-studios` repo, not committed:
- **The page:** `plugins/index.html` is a static classic-Aqua page (pinstripes, window chrome, gel
  buttons, Lucida Grande), with images in `private-assets/site/plugins/`.
- **Build and records:** `build-production.mjs` copies the page into `dist/plugins`; `ASSETS.md`
  has a new row. Checked at 1280 and 390 px.
- **Links:** the Download button links to
  `github.com/songadaymann/novox-the-robot/releases/latest/download/Novox-The-Robot-macOS.pkg`.
  That repo does not exist yet.
- **Unsigned note:** an "Open Anyway" note (`data-unsigned`) stays until notarization.

**Docs.** README has a Download section. The license page bundles the MIT and CMU notices.
HOSTS.md marks Pro Tools as not released. `build-release/` is gitignored.

**Not done (needs Jonathan):**
- commit, create the public repo, a GitHub release with the pkg, and deploy the site;
- the full AGPLv3 text (download from gnu.org; plugin/LICENSE.md is only a pointer);
- where the robot art came from (ASSETS.md says "To confirm");
- signing.

Changes made this morning (10:17-11:50) by someone else are in the release build:
- melody clipboard, `plugin/ui/melody-clipboard.js`;
- synth.cpp/hpp;
- `NoteLyricsTests.mjs`, which fails here because it runs the x86 system `python3` against the
  arm64 dylib.

They are not in this handoff.

**Checks.** 275 plugin checks, 52709 engine checks, the UI node tests except NoteLyricsTests, the AU
smoke test and oss-check pass (0 blockers, warning: no remote). The commit set is 107 files, none
over 2 MB, with no /Users paths and no secret patterns.
