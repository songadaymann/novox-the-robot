# First measured singing iteration — October 1, 2026

The life/sleep WAV and MIDI pairs are now development references. A frozen private
copy is in gitignored reference/novox-tests, with source hashes. MIDI times are
shifted by the region origin, not by best-fitting detected notes: life uses 3 beats
at120bpm (1.5s); sleep uses28 beats at106bpm (15.8490566s). Screenshots establish
these region positions; MIDI's initial metadata events carry the same tick offsets.
Leading gaps inside each region remain intact.

## Accepted result

| Recording | MIDI notes | Previous singing-v1 notes | Updated notes | Pitch agreement over reference duration, before → after |
|---|---:|---:|---:|---:|
| life | 15 | 16 | 14 | 60.2% → 61.3% |
| sleep | 12 | 17 | 12 | 84.7% → 84.8% |

The octave dip inside life's C4 and sleep's two false low tails disappear.
These counts do not imply all notes match. Strict exact-pitch/onset-within60ms
matches remain6/15 and4/12 respectively. The legacy detector gets7/15 and6/12
on that metric while producing19 and20 events. This iteration improves octave
stability and unwanted segmentation; strict timing accuracy has not improved.

Some disagreements are performance/reference differences rather than an octave
failure: independent FFT harmonic peaks around life's0.55s support196Hz/G3 while
MIDI saysA3; at1.03s support236Hz/approximatelyBb3 while MIDI saysA3; at6.06s
support310.5Hz/approximatelyEb4 while MIDI saysE4. Do not force these toward the
MIDI without deciding whether the desired feature is faithful transcription or
correction toward an intended melody. No reference is passed to the detector.

## Accepted native change

Strengthen preference against redundant multiple-period troughs from0.08 to0.30.
Reduce the energetic unvoiced emission from1.8 to1.2 to reject weakly periodic
breaths/tails. Recover a weak onset only if a measured candidate agrees within
0.6semitone with a confident original-path neighbor within40ms. This is bounded
and cannot grow recursively through silence. Legacy remains unchanged.

Rejected:0.60 period preference fails harmonic-change regression; broader allowed
settled-pitch spread fails initial-scoop regression. Neither is included.

## Validation and next work

Full engine regressions pass, including new120ms realoctave, weakfundamental and
energetic noise tests.20 plugin checks,15 native import checks and actual installed
AU smoke pass. Standalone/AU/VST3/compatibilityAU rebuilt and installed. No commit.

Reports:out/novox-tests/baseline.json and final.json. Visual comparison:
out/novox-tests/iteration-comparison.png. Editable seconds scores:life-singing-v1.json
and sleep-singing-v1.json in the same folder. Benchmark:tools/.venv/bin/python
 tools/transcribe_benchmark.py reference/novox-tests/manifest.json.

Next: assess repeated same-pitch syllables and onset/offset placement against these
references; collect held-out examples before claiming general accuracy improvement.
Preserve genuine120ms events, widevibrato, weakfundamentals and realoctave leaps.

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
