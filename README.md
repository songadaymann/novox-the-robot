# Novox The Robot

A modern take on Myriad's **Virtual Singer** (from Harmony Assistant): a formant-synthesis
singing voice that sings any lyrics on any melody. Deliberately synthetic, deliberately
old-school, but easy to use.

Goals:

- **One engine, many front ends.** A small C++ core with a plain C API that can sit behind a
  DAW plugin (AU/VST3 for Logic and friends), Python, and JavaScript (WebAssembly).
- **Fast lyric + melody entry.** Type a lyric line and it flows onto the notes by syllable. Play
  a MIDI keyboard into it, or sing into it (or drop in a sung take) to get the melody and words.
- **Many voices.** Presets modeled on the kinds of voices the original shipped with (std, robot,
  male/female/kid, soprano/alto/tenor/bass, gregorian, choirs, crooner…), all built from the
  same knobs.

## Download

**Mac (Apple Silicon, macOS 13.3 or later): [songaday.studio/plugins](https://songaday.studio/plugins).**
One installer adds the standalone app and the Audio Unit and VST3 instrument plugins (Logic Pro,
GarageBand, Ableton Live, Reaper and other hosts). Pro Tools (AAX) is not supported. To build the
installer yourself, see [`packaging/macos/README.md`](packaging/macos/README.md).

## Quick start (engine and CLI)

```bash
cmake -S . -B build && cmake --build build -j 8

build/novox say "hello my name is hugh" -o out/hello.wav --pitch D3 --voice robot
build/novox render scores/demo.json -o out/demo.wav --voice female
build/novox plan scores/demo.json      # the syllable + phoneme timeline, as JSON
build/novox voices

# MIDI and MusicXML work directly (lyrics from the file, or supplied):
build/novox render melody.mid --lyrics "hap-py birth-day to you" -o out/song.wav
build/novox render song.musicxml --voice female -o out/song.wav
build/novox import melody.mid --lyrics "..." -o scores/melody.json   # editable JSON score

ctest --test-dir build   # engine tests
```

The CMU dictionary is compiled into the engine, so the binary and library need no data files
(`--dict` adds extra entries). MIDI import reads lyric meta events (hyphenated, MuseScore's
trailing-space words, or karaoke-style), tempo changes and time signatures. MusicXML import
(uncompressed `.musicxml`/`.xml`, as MuseScore and Dorico export it; unzip a `.mxl` first) sings
the first part with lyrics, in the voice that carries them, one syllable per note: ties, melisma
extenders, hyphenated syllables and elisions (`a‿in` becomes `a~in`), chords (top note),
transposing parts (concert pitch), and tempo from `<sound tempo>` or metronome marks. Repeats,
endings, D.C., D.S., To Coda and Fine are played out as MuseScore plays them, with verse N on pass
N. Choose with `--track`
(part), `--part-voice`, `--verse`, or `--no-unfold` (as written). A file's own lyrics win over
`--lyrics`, which fills in only for a file without any. Time 0 is the start of the first measure,
a pickup included; the parts, tempo map, time signatures and pickup length are in the result's
`"import"` object.

Scores are JSON (`scores/demo.json`, format documented in `engine/src/score.hpp`):

```json
{
  "tempo": 92,
  "voice": "robot",
  "dictionary": { "hugh": "HH Y UW1" },
  "lyrics": "hel-lo, my name is Hugh, I sing a song for you _ _",
  "notes": ["E4 0.5", "G4 1", "r 0.5", "G4 0.5", "A4 0.5", "G4 0.5", "E4 1.5"]
}
```

- Words flow onto notes by syllable automatically; `hel-lo` forces a split, `_` holds the
  previous vowel across the next note (`sun- _ set` holds inside a word), `wanna~` keeps a word
  on one note however many vowels it has, `{HH EH L OW}` spells phonemes directly, and a note with
  no lyric hums "mm" (like the original; `mm` in the text hums too, and a lone `~` leaves a note
  without a lyric).
- `voice` can be a preset name or an object: `{"preset": "female", "vibratoDepth": 0.5, "grit": 0.3}`.
  All knobs are in `engine/src/voice.hpp`.

Python:

```python
import sys; sys.path.insert(0, "bindings/python")
import novox
singer = novox.Singer()
audio = singer.render({"tempo": 100, "lyrics": "good mor-ning", "notes": ["C4 1", "E4 1", "G4 2"]})
singer.save_wav("out/morning.wav", audio)

score = singer.import_file("melody.mid", lyrics="hel-lo world")   # MIDI / MusicXML -> score dict
stream = singer.stream(score)                                    # real-time style rendering
chunk = stream.process(512)                                      # next 512 samples
```

## Real-time streaming (for plugins)

`novox_stream_create` / `novox_stream_process` render a score incrementally: any chunk size, any
start position (`novox_stream_seek`), no allocation in `process`, and output identical to an offline
render. Output is at a fixed level with a soft clip, and each voice is loudness-matched to the
std voice (averaged over vowels and pitches), so presets sit within a few dB of each other. Onset consonants start
before the beat, so audio begins `novox_stream_lead()` seconds before score time 0.

## Plugin (AU / VST3) and standalone app

```bash
cmake -S . -B build-plugin -DCMAKE_BUILD_TYPE=Release -DNOVOX_BUILD_PLUGIN=ON   # downloads JUCE 8
cmake --build build-plugin -j --target NovoxTheRobot_All
# installs to ~/Library/Audio/Plug-Ins/{Components,VST3}; the app is in
# build-plugin/plugin/NovoxTheRobot_artefacts/Release/Standalone/
build-plugin/plugin/NovoxPluginTests_artefacts/Release/NovoxPluginTests   # headless transport tests
# macOS: test both installed AUs through real host callbacks:
cmake --build build-plugin --target NovoxAuHostSmoke
build-plugin/plugin/NovoxAuHostSmoke
```

On macOS, each newly built plugin is saved with its source and build settings in
`out/plugin-versions/` before installation. The previous installed plugin is also
backed up before replacement. Archives have unique UTC dates and are never pruned
by the build. Python 3.9+ and Git are required. See [plugin backups and recovery](docs/PLUGIN-BACKUPS.md)
for archive contents, a custom backup location, and restoring a saved version.

**Windows** (64-bit) builds the VST3 and the standalone app with Visual Studio 2022 or newer (the
"Desktop development with C++" workload), Git and Python 3. Visual Studio ignores
`CMAKE_BUILD_TYPE`, so always build with `--config Release`; a Debug build makes lyrics recognition
very slow.

```powershell
cmake -S . -B build-plugin -A x64 -DNOVOX_BUILD_PLUGIN=ON   # downloads JUCE, whisper.cpp and the WebView2 SDK
cmake --build build-plugin --config Release --parallel --target NovoxTheRobot_VST3 NovoxTheRobot_Standalone NovoxPluginTests
.\build-plugin\plugin\NovoxPluginTests_artefacts\Release\NovoxPluginTests.exe
```

- The VST3 is `build-plugin\plugin\NovoxTheRobot_artefacts\Release\VST3\Novox The Robot.vst3` and the
  app is `...\Release\Standalone\Novox The Robot.exe`.
- `NovoxPluginTests.exe --webview-smoke` opens the editor in WebView2 for a few seconds and checks
  that its page loads and talks to the plugin.
- Nothing is installed. Copy the `.vst3` folder into `C:\Program Files\Common Files\VST3`, or
  configure with `-DNOVOX_INSTALL_AFTER_BUILD=ON` from an administrator prompt and the build copies
  it there.
- The engine alone builds the same way: `cmake -S . -B build -A x64`, then
  `cmake --build build --config Release`. The CLI is `build\Release\novox.exe`; run the tests with
  `ctest --test-dir build -C Release`.
- `-DJUCE_WEBVIEW2_PACKAGE_LOCATION=<folder>` uses a copy of Microsoft's WebView2 SDK (the
  `Microsoft.Web.WebView2` NuGet package) you already have instead of downloading it. Windows builds
  link its WebView2 loader, which has a BSD-style license.
- The installer is described in [`packaging/windows/README.md`](packaging/windows/README.md).

How it works in a DAW: add it as an instrument. The plugin keeps its own song (notes in beats,
lyrics, voice) and saves it with the project. It renders the whole song in the background whenever
you edit it or the tempo changes, and plays the right part for wherever the playhead is, so looping
and scrubbing are sample-accurate and consonants land before the beat without added latency.
Arm the **keyboard icon** and play a MIDI keyboard (or play a MIDI region into it) while the transport runs to
record notes into the song. The standalone app has its own play/stop and tempo.

**The editor.** The top bar has the song file (**Open**, **Save**, **Save As…**), **Import…** with
the **Add to song** and **Detailed notes** checkboxes, the **Export** menu and the WAV/MIDI drag tiles, the LCD (click its
tempo to type a new one in the standalone app), the keyboard and singing icons, and **Play**. Below
it are the current singer, with **Tweak** for formant, vibrato, glide, breath, grit and choir, and a strip
of every singer: click one, or Tab to the strip and use the arrow keys. Hover a portrait (or focus
it) to see its name and description. The piano roll fills the middle, with the lyric sheet to its
right. The bottom bar shows what's under the pointer and holds **Key** (pitch correction), **Edit**
(split syllables, join notes, move a lyric, clear), **Compare**, **Snap**, zoom and **View**
(timeline overview, window size). In windows narrower than the Normal size the drag tiles move
into the Export menu.

**Formant** shifts the voice's resonance: lower values sound deeper, higher values
sound brighter, while the notes keep their pitch. It is the control previously
labelled **Size**, so saved songs keep their existing settings. Double-click the
slider to return to the singer's preset value.

Click the **singing icon** to record the system microphone, then click again to stop and
convert the phrase into notes and lyrics. A meter shows the input level; **Cancel** discards
the recording. Recording is limited to 60 seconds and does not monitor the microphone.
Microphone access must be allowed for the standalone app, or for the DAW when using a plugin.

MIDI files also support drag and drop. Native Mac region drops accept file URLs, promised
files and standard MIDI bytes. Logic's private region payloads are not decoded; when a
region supplies no usable file, the editor asks you to export it and drop the exported file.

Drop a WAV or MP3 into the app/plugin window, or choose it with **Import**. Conversion
runs locally in the background. On first use, **Download & convert** downloads a 574 MB
Whisper Turbo model to `~/Library/Application Support/Novox` (Windows: `%LOCALAPPDATA%\Novox`);
the recording is not uploaded.
**Melody only** skips the model and lyric recognition. No Python or ffmpeg installation is needed.
Files are limited to 64 MB and five minutes, with mono or stereo audio.

**What you get.** The importer (`singing-v2`) writes one note per sung syllable, plus a new note
where the melody really steps inside a syllable. Scoops, short approach notes, grace notes,
vibrato and droops at a syllable's end stay part of their note, and each note's octave is decided
from the whole note, so words aren't split in odd places and don't jump an octave mid-word. Each
note gets its syllable of the recognized lyrics (`_` on the later notes of a melisma). Numbers and
symbols are sung as words: "100" is sung "one hun-dred", "1999" "nine-teen nine-ty-nine", "$5"
"five dol-lars".

**The Lyrics box** shows the same syllables as text you can edit, one line per phrase, for example
`I love you like the day is long and the day's _ too short to write this song` or
`To for-ty for this beat` (sung from "40").

Multi-syllable words are hyphenated the way the notes split them (`shif-ting`), `_` stands for
each held note (`sun- _ set` inside a word), `mm` for a note with no words, and a word sung on one
note that would otherwise split over two gets a `~` (`wanna~`). Until you type in the box the notes
keep their own syllables and the text follows them (splitting, joining or moving a syllable updates
it). Once you edit it, the text flows back onto the notes and every syllable you didn't change stays
on its note. Whisper's raw words are kept in the song's transcription (`transcriptWords`).

**Detailed notes** (the lower checkbox beside **Import…**) also gives held approach notes, grace notes,
quick pitch steps and settled droops their own notes, the way a note-by-note transcription marks
every pitch it hears. Natural notes are all kept; Detailed only splits them further. It applies to
the next file, dropped region or singing take. It's a setting of this computer, not of the song.
Leave it off for one note per syllable.

Known limits:
- **Solo voice works best.** Under accompaniment or a second voice, the pitch tracker can follow
  the backing and put whole notes in the wrong octave or a fifth off. The LCD then shows
  "accompaniment heard: check pitches".
- **Lyrics are recognized as English.** Other languages give junk or English-sounding words. The
  melody still imports; retype the lyrics.
- **Long recordings can loop.** Whisper sometimes repeats one lyric line over the next part of a
  long take or full song. Repeated words that match no sung audio show up as warnings, but some
  land on real notes, so check the lyrics.
- **Long melismas** (a run on one vowel) can lose real steps with Detailed off; try it on.
- Short notes and repeated words still need correcting by hand.

Conversion places the detected notes and per-note lyrics directly in the existing song piano
roll. Use the usual draw, drag, resize, lyric, split/merge and playback tools to correct them.
The current tempo is preserved. There is no intermediate review or Accept step.

**Region by region.** With **Add to song** on (the upper checkbox beside **Import…**; it starts
on), each recording you import joins the song instead of replacing it, so a vocal track split
into many regions can come in one region at a time. Drag in a region and it lands at its bar;
drag in the next and the first stays. An import replaces only the notes within its own length:
notes that start inside it are removed, a note that runs into it from before is shortened to end
where it begins (or removed if less than a 32nd note would be left), and the notes after it stay. So
dragging in another take of the same region swaps just that section (an earlier recording with no
notes left outside the new one, such as a take trimmed a little longer, is let go). A file without
a timeline position goes to where the playhead was when you dropped it, and a singing take to where
it was when you started singing: the DAW's playhead, or in the standalone app wherever playback was
stopped. If the lyrics were typed into the Lyrics box, every existing note first
keeps the syllable it was singing; from then on the Lyrics box shows the lyrics of all the notes,
a line per phrase. Each import is one Undo step (notes, lyrics and recordings), and **Clear**
removes everything. With **Add to song** off, an import replaces the whole song (at beat 0 when
the file has no position). The starter song a new instance sings is always replaced, and MIDI
and MusicXML files always replace the song. Like Detailed notes, it's a setting of this computer.

Use **Original**, **Robot**, and **Stop** in the **Compare** menu to compare the selected notes
(or the whole song); enable **Loop** to repeat the phrase. **Original** plays each imported
recording under the phrase at its own place (where a later import replaced part of an earlier
one, you hear the later one there). The source waveform under the roll shows every import at its
position and follows piano-roll scrolling and zoom; a blue tick marks where each one starts. Every
import's original audio is retained with the project or a saved `.novox` song (16 kHz mono, about
2.5 MB a minute; a recording that a later import covered completely is left out). **Open**, **Save**, and **Save As…** manage song
files. **Export WAV…/MIDI…** saves the current performance; holding and dragging the **WAV** or
**MIDI** tile starts a native file drag into your DAW. WAV export starts at timeline zero; any preroll consonant
before zero is trimmed. Dragged files remain in Application Support/Novox/Exports (Windows:
`%LOCALAPPDATA%\Novox\Exports`). What differs on Windows is listed in [docs/HOSTS.md](docs/HOSTS.md#windows).
The [feature roadmap](docs/FEATURE-TODO.md) tracks remaining work.

Select one note and choose **Edit > Split syllables…** to enter `re-mem-ber`; resize the resulting
notes to adjust timing. **Join notes** merges selected notes of the same pitch. **Move lyric**
(← Earlier, Later →) swaps a lyric with its adjacent note without shifting later words. Progress appears
in the app's LCD. After an import it shows the number of warnings and the most serious one; hover
the LCD to see them all.

Double-click a note to enter a word or phrase. Words with multiple syllables, such as
`remember` or `re-mem-ber`, fill that note and the following notes using the singer's
pronunciation dictionary. The Lyrics pane updates at the corresponding place in the song.
The notes keep their timing and pitch; if there are too few following notes, the edit
stays open so you can shorten it or add notes first. Undo restores the entire lyric edit.

Select notes and use **Edit > Copy melody** / **Paste melody** (⌘C / ⌘V on Mac,
Ctrl+C / Ctrl+V elsewhere) to copy between Novox instances, including the standalone app.
Pasted notes keep their original beat positions and lyrics. If they overlap existing notes,
Novox asks before replacing the overlapping portions; Cancel leaves the song unchanged,
and one Undo restores a confirmed paste. Gaps between copied notes leave existing notes
alone. Each instance keeps its own singer and tempo, so the same beats may take a different
number of seconds. Copy and paste in a text field still work as ordinary text editing.

In Logic, reload the updated Audio Unit (quit and reopen Logic if needed).
Both **Novox The Robot** and the original **New Virtual Singer** AU identity use the current
code. The compatibility AU preserves existing Logic projects and is built by the same target. Finder file drops and the
Import picker share the same conversion path. An audio region dragged from Logic's tracks area
arrives as an AIFF that records where the region sits, and its notes land at that bar (as do WAV
exports with a Broadcast WAV time stamp). Bar 1 is taken to be at Logic's default start time,
01:00:00:00. Files without a stamp go to the playhead with **Add to song** on, or start at beat 0
with it off.

**Other hosts:** [docs/HOSTS.md](docs/HOSTS.md) has the optional Pro Tools build
(`-DNOVOX_AAX_SDK=bundled` adds an AAX target using the AAX SDK inside JUCE; Pro Tools also needs
PACE signing), the Ableton Live test checklist, notes on Logic, and what MuseScore and Dorico send.

The plugin is AGPLv3 (JUCE's open-source license); the engine it links stays MIT.

### Using Novox with MuseScore and Dorico

1. **Export MusicXML** from the notation app: in MuseScore Studio, File › Export › MusicXML; in
   Dorico, File › Export › MusicXML. Compressed (`.mxl`) and uncompressed (`.musicxml`, `.xml`)
   both work. Dorico writes one file per flow.
2. **Import it into Novox:** drop the file on the window or choose it with **Import…**, in the
   standalone app or in the plugin on a vocal staff. Both apps load the **VST3** (neither hosts AU):
   in MuseScore choose it in the Mixer's instrument slot of the staff; in Dorico load it as a VST
   instrument in Play mode and assign the vocal staff to it. Novox sings the first part with lyrics, in the voice that carries
   them, one syllable per note, so a one-syllable word such as "fire" stays on its note. A file
   with several parts, voices or verses opens a bar under the top bar to sing another (each choice
   imports the file again). A file without lyrics sings the Lyrics box.
   Problems with the file and import warnings show on the LCD; hover it to read them all.

What to expect:
- **Lyrics come from the file.** No notation app passes lyrics to plugins, so after editing words
  in the score, export and import again.
- **MuseScore** gives plugins no beat position or tempo, and runs them only while it plays. Novox
  follows its playback by time (seconds from the start of the score, at the song's tempo; View
  shows "clock: samples"). Start and stop playback in MuseScore: Novox's Play and ruler say so,
  and note previews and Compare are silent while MuseScore is stopped. Edit there, or in the
  standalone app.
- **Dorico** sends a beat position, which Novox follows as it does a DAW's. Steinberg staff say
  it counts that position at the flow's first tempo, and disagree about when it sends a tempo
  (unverified; see docs/HOSTS.md).
- **Tempo changes:** a Novox song has one tempo. A score whose tempo changes is placed by time at
  its first tempo, so it sounds as written and lines up with the notation app's playback clock; the
  LCD warns that the grid follows the first tempo, and the bars drawn after a change no longer match
  the score's. In a DAW that sends its own tempo, an imported file keeps its written beats instead,
  as before. Ritardando, accelerando and fermatas are not in MusicXML (MuseScore writes them as
  text), so Novox runs ahead of MuseScore's playback after them.
- **Repeats and verses:** repeats, endings, D.C., D.S., To Coda and Fine are played out as
  MuseScore plays them, with verse N on the Nth time through; choose a single verse in the import
  bar. A pickup starts at beat 0, as in the notation app; the grid still draws 4/4 bars from there
  (the LCD says so for a score in another meter or with a pickup).
  A MIDI export puts every verse at once; where one verse holds a note another sings, Novox matches
  syllables to verses by their words and says so, so prefer MusicXML for songs with verses.

## Layout

| Path | What |
|---|---|
| `engine/src/` | The engine: lexicon (CMU dict + spelling fallback), phoneme targets, planner (score → timeline), Klatt-style formant synth, voices |
| `engine/include/novox.h` | C API used by every front end |
| `engine/cli/` | `novox` command-line tool |
| `bindings/python/` | ctypes binding |
| `tools/intelligibility.py` | Sings test sentences, transcribes with Whisper (local, free), reports word error rate |
| `tools/spectrogram.py` | Spectrogram PNG of a render |
| `tools/import_lab.py`, `tools/datasets/` | Sung-vocal import evaluation: replays the plugin's exact import and scores it on Jonathan's takes and three public datasets, which you download yourself (see `tools/datasets/README.md`, `docs/SINGING-V2-DESIGN.md`) |
| `plugin/` | JUCE plugin + standalone app (AGPLv3): processor, WebView editor, `ui/` (HTML/JS), tests |
| `bindings/js/`, `web/` | WebAssembly npm package; Aqua browser demo |
| `mockups/` | Four plugin UI directions (open `mockups/index.html`) |
| `scores/` | Example scores |
| `data/cmudict.dict` | CMU Pronouncing Dictionary (BSD-style license, see `data/cmudict.LICENSE`) |
| `docs/PLAN.md` | Design notes and roadmap |

## Testing intelligibility

```bash
# one-time: uv venv tools/.venv --python 3.12 --python-preference only-managed
#           uv pip install --python tools/.venv/bin/python numpy scipy mlx-whisper jiwer matplotlib
tools/.venv/bin/python tools/intelligibility.py --voices std,robot,female -v
```

## License

The engine, CLI, bindings and tools are MIT licensed (see `LICENSE`). The CMU Pronouncing
Dictionary in `data/` keeps its own BSD-style license. The DAW plugin in `plugin/` (built on JUCE)
is AGPLv3 (see `plugin/LICENSE.md`; full text in `plugin/LICENSE-AGPL-3.0.txt`). Art and other media are listed in `ASSETS.md`.
