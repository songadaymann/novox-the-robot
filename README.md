# Novox The Robot

A modern take on Myriad's **Virtual Singer** (from Harmony Assistant): a formant-synthesis
singing voice that sings any lyrics on any melody. Deliberately synthetic, deliberately
old-school, but easy to use.

Goals:

- **One engine, many front ends.** A small C++ core with a plain C API that can sit behind a
  DAW plugin (AU/VST3 for Logic and friends), Python, and JavaScript (WebAssembly).
- **Fast lyric + melody entry.** Type a lyric line and it flows onto the notes by syllable. Later:
  MIDI keyboard capture, and dropping in a sung audio take to extract melody + words.
- **Many voices.** Presets modeled on the kinds of voices the original shipped with (std, robot,
  male/female/kid, soprano/alto/tenor/bass, gregorian, choirs, crooner…), all built from the
  same knobs.

## Quick start

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
(`--dict` adds extra entries). MIDI import reads lyric meta events and tempo changes; MusicXML
import handles ties, melisma extenders, hyphenated syllables and multiple voices (it sings the
first voice of the first part; choose another part with `--track`).

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
  previous vowel across the next note, `{HH EH L OW}` spells phonemes directly, and a note with
  no lyric hums "mm" (like the original).
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
```

How it works in a DAW: add it as an instrument. The plugin keeps its own song (notes in beats,
lyrics, voice) and saves it with the project. It renders the whole song in the background whenever
you edit it or the tempo changes, and plays the right part for wherever the playhead is, so looping
and scrubbing are sample-accurate and consonants land before the beat without added latency.
Arm **Capture** and play a MIDI keyboard (or play a MIDI region into it) while the transport runs to
record notes into the song. The standalone app has its own play/stop and tempo.

The plugin is AGPLv3 (JUCE's open-source license); the engine it links stays MIT.

## Layout

| Path | What |
|---|---|
| `engine/src/` | The engine: lexicon (CMU dict + spelling fallback), phoneme targets, planner (score → timeline), Klatt-style formant synth, voices |
| `engine/include/novox.h` | C API used by every front end |
| `engine/cli/` | `novox` command-line tool |
| `bindings/python/` | ctypes binding |
| `tools/intelligibility.py` | Sings test sentences, transcribes with Whisper (local, free), reports word error rate |
| `tools/spectrogram.py` | Spectrogram PNG of a render |
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
Dictionary in `data/` keeps its own BSD-style license. The planned DAW plugin (built on JUCE)
will be AGPLv3.
