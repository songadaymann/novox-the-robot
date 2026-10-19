# Recording tests

Put private recordings and annotations in `reference/` (already excluded from Git).
A useful first set includes a held note with vibrato, a short scale, a real octave
leap, repeated notes, and a phrase with several words on the same pitch. Dry solo
vocals help distinguish detector errors from accompaniment; tag accompanied takes
separately. Include some recordings we do not tune against.

Create `reference/manifest.json` using this format. The example below is a format
illustration, not an annotation of Jonathan's existing recording:

```json
{
  "version": 1,
  "cases": [{
    "id": "held-note-example",
    "audio": "held-note.wav",
    "tags": ["solo", "held-note", "vibrato"],
    "notes": [{"start": 0.5, "dur": 1.8, "pitch": 60, "lyric": "love"}],
    "words": [{"word": "love", "start": 0.45, "end": 2.3, "probability": 1}],
    "ignore": [],
    "comment": "One intended C4, including its vibrato"
  }]
}
```

Pitch uses MIDI numbering: C4 is 60. Times are seconds from the beginning of the
file, including any silence. Annotate intended sung notes, not individual vibrato
cycles. Words are optional and remain fixed across both engines. `ignore` accepts
`{"start": 0, "end": 0.2}` intervals; notes overlapping them are excluded from
both sets. Keep ignores narrow and document why they are needed.

Run `tools/.venv/bin/python tools/transcribe_benchmark.py reference/manifest.json` after building
the engine (use a native Apple Silicon Python with this arm64 build). It decodes WAV/MP3 through ffmpeg, compares both algorithms, and writes
`out/benchmark.json`, including editable score outputs. It does not run Whisper.

The report measures exact pitch/onset matches, duration-aware strict events,
missed/extra notes, time-associated octave substitutions, other pitch substitutions,
fragmentation, median/p90 boundary errors and runtime. No timing matches yield
null timing statistics. Words-retained counts describe metadata conservation;
they do not measure whether pronunciation or lyric placement is correct.

Import evidence is saved in the song's `transcription` object with times in
original audio seconds. It describes the original detection, even after manual
note edits or a tempo change; it is not an annotation of the edited song.
