# New Virtual Singer

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

build/nvs say "hello my name is hugh" -o out/hello.wav --pitch D3 --voice robot
build/nvs render scores/demo.json -o out/demo.wav --voice female
build/nvs plan scores/demo.json      # the syllable + phoneme timeline, as JSON
build/nvs voices

# MIDI and MusicXML work directly (lyrics from the file, or supplied):
build/nvs render melody.mid --lyrics "hap-py birth-day to you" -o out/song.wav
build/nvs render song.musicxml --voice female -o out/song.wav
build/nvs import melody.mid --lyrics "..." -o scores/melody.json   # editable JSON score

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
import nvs
singer = nvs.Singer()
audio = singer.render({"tempo": 100, "lyrics": "good mor-ning", "notes": ["C4 1", "E4 1", "G4 2"]})
singer.save_wav("out/morning.wav", audio)

score = singer.import_file("melody.mid", lyrics="hel-lo world")   # MIDI / MusicXML -> score dict
stream = singer.stream(score)                                    # real-time style rendering
chunk = stream.process(512)                                      # next 512 samples
```

## Real-time streaming (for plugins)

`nvs_stream_create` / `nvs_stream_process` render a score incrementally: any chunk size, any
start position (`nvs_stream_seek`), no allocation in `process`, and output identical to an offline
render. Output is at a fixed level (peaks around −5 dBFS) with a soft clip. Onset consonants start
before the beat, so audio begins `nvs_stream_lead()` seconds before score time 0.

## Layout

| Path | What |
|---|---|
| `engine/src/` | The engine: lexicon (CMU dict + spelling fallback), phoneme targets, planner (score → timeline), Klatt-style formant synth, voices |
| `engine/include/nvs.h` | C API used by every front end |
| `engine/cli/` | `nvs` command-line tool |
| `bindings/python/` | ctypes binding |
| `tools/intelligibility.py` | Sings test sentences, transcribes with Whisper (local, free), reports word error rate |
| `tools/spectrogram.py` | Spectrogram PNG of a render |
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
