# Design notes and roadmap

## What the original was

Virtual Singer (Myriad, a Harmony Assistant add-on) used **formant synthesis**: no recordings,
just a buzzing glottal source shaped by 3–6 resonances per phoneme. Its sibling **RealSinger**
used recorded voice fragments instead. The robotic charm comes from formant synthesis, so
that's what this engine does.

The voice presets that ship in `Harmony Assistant.app/Contents/VirtualSinger/Voices/*.prv` are
plain text and show what a "voice" was made of: octave/semitone shift, attack pitch scoop,
timing looseness, random wander on F0/F1/F2/volume (amount + speed), pitch drift into notes,
envelope timings per phoneme class, vibrato (delay, ramp, rate, depth), a "singing formant",
per-formant scale factors F1–F6, bass/treble tilt, clipping, glottal open ratio, and choir
size/dephasing. Our `Voice` struct covers the same ground with our own values. Built-in presets
there: Std voice, Karaoke Voice, Male, Female, Kid, Sopranino, Soprano, Alto, Tenor, Tenor slow,
Bass, Gregorian, VirtualRobert, four choirs, High male, Quick vibrato, and six "Modern" named
singers.

## Engine architecture

```
score JSON ──► parse ──► flow lyrics onto notes ──► syllables (CMU dict / rules / user dict)
                                                      │
                          planner: consonants before the beat, vowels on it,
                          diphthong glides at the end, silences, transitions
                                                      │
                                                   Plan  (segments of acoustic targets + pitch notes)
                                                      │
               synth: KLGLOTT88 glottal pulse + breath ─► nasal zero/pole ─► F1–F5 + fixed F6–F8
                      noise ─► 3 band-pass peaks + bypass (fricatives, bursts)
                      pitch: scoop, portamento, delayed vibrato, wander, choir detune
                                                      │
                                                  mono float audio
```

The synth only ever sees a `Plan`, so a real-time front end can build plans incrementally.

## The "render → Whisper → fix" loop

It's free and fully automatable: Whisper runs locally (`mlx-whisper` on Apple Silicon), and
`tools/intelligibility.py` renders sentences, transcribes them, and scores word error rate.

We use it as a **development harness**, not at run time:

- The engine itself is deterministic rules (dictionary + phoneme targets + timing), so it's
  predictable and instant, which a plugin needs.
- Whisper tells us which phonemes are weak, and catches regressions when we change something.
- Later, it can drive automatic tuning: search phoneme parameters to minimize error across a
  test set, then bake the result into the phoneme table.

First results (Harvard sentences + lyric lines, sung on a melody, 0.33 s per syllable):

| voice | WER round 1 | after first tuning pass |
|---|---|---|
| std | 13.3% | 12.2% |
| robot | 13.3% | 11.2% |
| female | 21.4% | 18.4% |
| tenor | — | 16.3% |
| kid | — | 19.4% |
| choir | — | 31.6% (ensemble timing smears consonants) |

Known weak spots: word-final stops, some vowel pairs (IY/IH, EH/EY), clusters like "gl",
higher voices (fewer harmonics to show the formants).

## Matching the original (round 2)

Reference: Myriad's official demo MP3s (`reference/myriad/`, not in git). `tools/voice_compare.py copy`
extracts a reference melody with pYIN, sings it with our engine, and compares measurements.
Against the a cappella tenor ("Salve Regina"):

| measure | original | ours before | ours now |
|---|---|---|---|
| vibrato rate | 5.8 Hz | (started too late to measure) | 5.7 Hz |
| vibrato depth | 0.32 st | — | 0.30 st |
| pitch wander | 3.2 cents | 3.2 | 3.7 |
| spectral tilt | −9.6 dB/oct | −5.3 | −11.1 |
| 2–3 kHz band | −21 dB | −12 | −20 |
| 3–4 kHz band | −26 dB | −16 | −29 |

What changed: a 4.5 kHz band limit (the original's output stops at ~4.3 kHz; the new
`bandLimitHz` knob, 0 = full band, `hifi` preset), no default singer's-formant boost, a darker
source, breathier default, vibrato starting at 0.12 s instead of 0.35 s, shorter stop closures
with a stronger voice bar (more legato). Intelligibility held: std 12%, tenor 14%, kid 17%.

Still different: the original has audible breath noise between harmonics, snappier pitch
changes between notes, and even more connected consonants. Next step is exact A/B with
Harmony Assistant itself (needs Rosetta) singing our test phrases.

## Head-to-head with Harmony Assistant itself (round 3)

Harmony runs under Rosetta and is driven by a MyrScript installed at
`~/Documents/Myriad Documents/Settings/Scripts/NVS Render Batch.mys` (one menu click renders a
batch). MyrScript is **Lua 4**: no `#`, no closures over locals, no `pcall`, no `table.`/`string.`
libraries; use globals, `getn`, while-loops. Working calls: `Application.IsVirtualSingerActive=true`,
`Load(xmlPath)`, `FrontScore()`, `score.FirstStaff`, `staff.LoadVirtualSingerVoice(prvPath)`,
`score.MusicExport(wavPath, 15 --[[WAV]], 44100, 16)`. `score.Close` doesn't exist.
`tools/to_musicxml.py` turns our scores into MusicXML with the same syllables our planner uses.

Findings from rendering `scores/demo.json` in both:
- The 4.3 kHz ceiling heard in Myriad's web demos is MP3 encoding, not the synth: real output
  has harmonics to ~6.5 kHz. Band limit is now off by default (`lofi` preset keeps it).
- Harmony's std voice has weaker F2/F3 peaks and a smoother spectrum: new defaults are a darker,
  softer source (brightness −1, open quotient 0.72), 1.6× bandwidths, a −4 dB dip at 2.6 kHz.
  Bands 0–3 kHz now within 1.5 dB.
- Pitch glides are slow (~0.2 s scoops and portamento, sometimes longer on big leaps); vibrato
  starts immediately (5.9–6.0 Hz, ~0.3 st). Defaults updated.
- Whisper understood ours better on the demo ("…every single day, all year" vs Harmony's
  "…every single day on you"). Intelligibility after matching: std 14%, female 14%, kid 17%.

## Round 4: per-vowel formants measured from Harmony

Lesson: long-term average spectrum matching is misleading (it's dominated by the low harmonics).
Compare **harmonic by harmonic on the same vowel at the same pitch** (`tools/deep_compare.py`,
and the matched-frame sweep) and **measure formants per vowel** from an h_d test
(`scores/vowels.json` → `tools/vowel_formants.py`).

Findings:
- Harmony's F3 sits at ~2690 Hz and F4 at ~3700 Hz for nearly every vowel (only F1/F2 vary;
  ER lowers F3 to ~1460). Bandwidths are narrow (20–50 Hz), so upper harmonics ring clearly.
- Its F2 values match Klatt-style American English (UW ~1325, OW ~1220, AO ~1045, AE ~1550).
- Male = Std. Female scales formants ~1.10×, Kid ~1.20× (not 1.17 / 1.32).
- Vowel table rebuilt from these; F4/F5 defaults 3700/4300 Hz. Matched-frame harmonic
  difference fell from 12.3 dB to 7.7 dB with no remaining bias. Whisper: std 11%, tenor 13%.

## Roadmap

**Engine**
- [x] Formant synth, planner, lexicon, voices, C API, CLI, Python binding
- [ ] Tuning pass driven by the Whisper harness (per-phoneme targets, consonant timing, high voices)
- [ ] Coarticulation: nasalized vowels next to nasals, vowel-dependent consonant loci
- [ ] Dynamics: note velocity → loudness/brightness, phrase-end decay, crescendo on long notes
- [x] Streaming API (`nvs_stream_*`): chunked, seekable, allocation-free, bit-identical to offline;
      per-voice loudness matching
- [ ] Live-MIDI mode: `note_on(pitch, velocity, syllable)` with a small lookahead (reported as latency)
- [x] WebAssembly build + ES module wrapper + TypeScript types + `npx` CLI (`bindings/js`, `scripts/build-wasm.sh`)
- [x] CMU dictionary compiled into the engine
- [x] MIDI (lyric events) and MusicXML import; MusicXML export (`tools/to_musicxml.py`)
- [x] Engine tests (`ctest`), Python wheel (`pip install .` → `nvs` command), CI + release workflows
- [ ] Publish (needs the final name, a GitHub repo, PyPI + npm accounts)
- [ ] More languages (the original sang French, Italian, Latin…)

**Plugin (JUCE, AU + VST3 + CLAP)**
- [ ] Instrument plugin that receives MIDI and sings; lyrics stored in plugin state and matched
      to notes by order within the region
- [ ] UI as an HTML/JS web view (JUCE 8 supports this), so the mockups here can become the real UI
- [ ] Voice gallery + voice designer

**Input modes**
- [x] Type lyrics → flow onto notes by syllable
- [ ] MIDI keyboard capture
- [ ] Audio in: monophonic pitch tracking (pYIN / CREPE / basic-pitch) → notes; Whisper with word
      timestamps → lyrics; align words to notes; review + fix screen (mockup C)

## Open questions

- Which mockup direction becomes the main screen (A piano roll vs. B lyric sheet)? They could be
  two views of the same data.
- Lyric notation: `-` split, `_` hold, `{…}` phonemes. Keep, or match another convention?
- How faithful to the original's sound vs. "cleaner" modern formant synthesis?
