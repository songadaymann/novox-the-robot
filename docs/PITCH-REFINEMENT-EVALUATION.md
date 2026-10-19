# Novox pitch refinement: comparison with the current version

2026-10-18. Implemented and retained a targeted refinement for high pitches in `singing-v2`. It materially improves fine pitch on known synthetic signals. Real-recording results are mostly unchanged, with a small CSD note-scoring regression; this is not evidence of a broad transcription or octave-detection improvement.

The code is independently authored standard signal processing. No Melodyne source, decompiled routines, proprietary tables, or binary assets were added to Novox. The useful design lesson was to separate the initial pitch/octave decision from a subsequent waveform check. This is not a claim to reproduce Melodyne's algorithm or quality.

## What changed

`engine/src/singing_pitch.cpp` now refines reliable selected periods of at most 40 samples at 16 kHz (about 400 Hz and above). It directly compares fractional-delay waveform pairs, using normalized absolute mismatch and 12-tap windowed-sinc interpolation. The bounded search stays within ±35 cents of the selected pitch. It retains the old estimate at audio edges, abrupt transitions, ambiguous mixtures, weak periodicity, boundary minima, or an insufficiently good waveform fit. The refinement uses whichever signal band supplied the original period.

The existing decoder still chooses the octave and voiced/unvoiced state. Its candidate scores and frame confidence are computed before refinement and remain unchanged. Both the selected candidate and output frame receive the refined period, so downstream note processing stays internally consistent. Later note segmentation/rounding can change; the comparison below measures this explicitly. Lower selected pitches retain their original estimate.

The internal C++ call `analyzeSingingPitch(audio, false)` retains the prior interpolation for regression comparisons; the default enables the refinement. The plugin's existing `singing-v2` path picks it up when rebuilt. No plugin installation or distribution was performed.

Changed files: `engine/src/singing_pitch.cpp`, `engine/src/singing_pitch.hpp`, `tests/singing_pitch_tests.cpp`. Added a reusable synthetic comparison tool, `tools/pitch_synthetic_eval.py`, and this report. Unrelated local changes were preserved.

## Fair comparison

The baseline is an immutable snapshot of the actual current checkout, including its uncommitted and untracked pitch code. It is not Git HEAD. Both native libraries were built in Release with the same configuration. Corpus runs use `algorithm: singing-v2`, `detail: natural`, identical cached 16 kHz audio and timestamped words, and the same scoring code. Native libraries run in separate processes. Audio, labels, split files, harness and libraries were hashed; input hashes were checked before and after every corpus batch.

Development comprised 252 recordings. After choosing the conservative upper-register variant, its source and library hashes were frozen before evaluating 138 held-out recordings. No parameters changed after those results. These are the project's existing regression splits; they are not claimed to be globally pristine research holdouts.

The 390 recordings comprise 21 Jonathan clips, 40 vocadito clips, 234 sung GTSinger clips, and 95 cached CSD clips. CSD excludes four development cases (`csd-en011b`, `csd-en023b`, `csd-en047a`, `csd-en047b`) and one test case (`csd-en012a`) whose cached input/words were unavailable at the start. The subset is identical for both versions. No speech-only GTSinger cases were included.

Jonathan's references include existing silver/verified annotations; vocadito supplies human note and continuous-F0 annotations. GTSinger/CSD use the repository's adapted references. Report these corpora separately rather than treating all labels as equally independent ground truth.

## Controlled pitch accuracy

The predeclared 80-case suite covers fractional pitches, multiple phases and timbres, weak fundamentals, noise, amplitude modulation, vibrato, glides, pitch steps, short notes, silence and unpitched noise. Ground truth is the generated instantaneous frequency. Frame MIDI output is quantized to 0.1 cent, so very small errors are limited by reporting precision. One semitone is 100 cents.

| Metric | Current | Refined |
|---|---:|---:|
| Steady-tone mean absolute error, all detected voiced frames | 19.42 cents | 17.03 cents |
| Steady-tone fine error, frames already within 50 cents | 2.93 cents | 0.49 cents |
| Steady-tone fine error, 95th percentile | 17.8 cents | 1.9 cents |
| Changing-pitch mean absolute error | 3.82 cents | 0.96 cents |
| Changing-pitch error, 95th percentile | 8.28 cents | 3.20 cents |
| Steady-tone octave-error frames | 101 | 101 |
| Missed voiced interior frames | 289 | 289 |

The fine-error reduction is about 83%; it does not include pre-existing octave failures or missed voiced frames. Those remain, and are counted separately rather than disappearing from the report. The all-frame error above makes that limitation visible. Voiced masks, confidence, clarity and synthetic note counts were identical. No tested step crossing/settling delay worsened.

A separate 46-case probe around the 400 Hz cutoff found a small hard-gate discontinuity: the correction can toggle by up to 0.9 cents between frames. Its aggregate errors improved and voicing/note counts stayed unchanged. This is a measured limitation, not a claim of a perfectly seamless cutoff.

## Recorded-vocal note results

COnP F1 uses one-to-one note matching with onset within 100 ms and pitch within 50 cents. Higher is better. Exact matched-note counts are shown because rounded scores can hide small changes.

| Corpus | Split | Clips | COnP F1: current → refined | Matched notes | Octave errors |
|---|---|---:|---:|---:|---:|
| Jonathan | dev | 15 | 0.8180 → 0.8180 | 519 → 519 | 14 → 14 |
| Jonathan | test | 6 | 0.7262 → 0.7262 | 236 → 236 | 3 → 3 |
| vocadito | dev | 23 | 0.6948 → 0.6957 | 709 → 710 | 5 → 5 |
| vocadito | test | 17 | 0.6169 → 0.6169 | 615 → 615 | 0 → 0 |
| GTSinger | dev | 168 | 0.6627 → 0.6627 | 1746 → 1746 | 1 → 1 |
| GTSinger | test | 66 | 0.7015 → 0.7015 | 731 → 731 | 4 → 4 |
| CSD | dev | 46 | 0.8925 → 0.8910 | 5186 → 5177 | 0 → 0 |
| CSD | test | 49 | 0.8792 → 0.8789 | 6890 → 6889 | 3 → 2 |

The CSD development set loses nine matched notes, and its held-out set loses one. The latter also changes output note count from 7,810 to 7,814 and fragmentation from 252 to 257, while octave errors fall from three to two. These mixed changes must not be described as universally better note transcription. Inspection of development changes found half-semitone rounding crossings and some segmentation changes; a more precise contour can cross an existing discrete threshold.

## Recorded-vocal continuous pitch

On vocadito, original reference timestamps are aligned to the nearest 8 ms engine frame, with a maximum 4 ms distance and earlier-frame tie breaking. No output-dependent time shift, pitch interpolation across silence, tuning adjustment, or confidence-based frame selection is used. All reference-voiced frames count in raw pitch accuracy; missing or unvoiced predictions count as misses.

| Metric | Dev current → refined (23 clips) | Held-out current → refined (17 clips) |
|---|---:|---:|
| Raw pitch accuracy, within 50 cents | 98.3240% → 98.3240% | 98.2392% → 98.2392% |
| Jointly voiced median absolute error | 2.17310 → 2.17282 cents | 2.40041 → 2.40041 cents |
| Jointly voiced 90th percentile error | 11.55829 → 11.58007 cents | 12.21542 → 12.21542 cents |

Held-out mean absolute error moves from 6.063289 to 6.063394 cents; voiced recall remains 98.7127%, false-voicing rate remains 16.6571%, and overall accuracy remains 93.4098%. Coverage is 100% of reference-voiced timestamps and over 99.99% of all timestamps. The evaluator passed alignment/denominator checks and an independent review.

This corpus contains little material above the refinement cutoff: only three held-out engine-frame pitches changed. It validates compatibility on these voices, but does not establish improved accuracy on real high-register singing. More annotated upper-register recordings would be needed for that claim.

Across all 1,406,506 engine frames in the 390 recordings, the voiced masks, levels, clarity and confidence were identical between versions.

## Runtime and regression tests

Four fixed development clips (102.9 seconds total) were timed with cached input, one warmup per library/clip and three sequential alternating pairs. Aggregate medians were 0.580 seconds per audio minute for the current version and 0.583 for the refinement: approximately unchanged in this small sample. It excludes decoding and speech recognition. These clips are mostly below the cutoff and the machine was not an isolated benchmark host; this is not a worst-case upper-register cost guarantee.

The original engine suite passed (3,501 assertions). The final suite, including focused accuracy, gain, vibrato, short-note, noise, silence/edge, lower-register equality and decoder-invariant checks, passed 52,528 assertions with zero failures. These are assertions inside the existing suite, not 52,528 independent audio test cases. Source review found no blocking bounds or candidate/frame consistency issue.

## Reproduce

Capture the current version before applying the patch, because this checkout already contained substantial uncommitted work. Build baseline and candidate in separate directories with the same options. For example, in each checkout:

```sh
cmake -S . -B build-pitch -DCMAKE_BUILD_TYPE=Release -DNOVOX_BUILD_PLUGIN=OFF -DNOVOX_BUILD_TESTS=ON
cmake --build build-pitch -j 6
ctest --test-dir build-pitch --output-on-failure
```

Then run the supplied tool from the repository root (NumPy required; the existing tools environment contains it):

```sh
tools/.venv/bin/python tools/pitch_synthetic_eval.py \
  --baseline-lib /path/to/baseline/build-pitch/libnovox.dylib \
  --candidate-lib build-pitch/libnovox.dylib \
  --out out/pitch-synthetic-results.json
```

For recorded corpora, use `tools/import_lab.py corpus` with explicit manifest, lab, splits, library, `--options '{"algorithm":"singing-v2","detail":"natural"}'`, `--words words.json`, and fresh tags. Run development first, freeze the candidate, then test. The accompanying evidence package includes the exact batch runner and independent continuous-F0 evaluator; corpus audio and reference caches remain local and are not redistributed.

Frozen candidate library SHA-256: `a210ede9e5c15daca5a36ef1221ba2af2b2f1c4759931c9cdf0ae553d9b7f564`.

The patch is relative to the saved current checkout, not the last commit. It contains only the new source/test changes and the independently authored tool/report. No binaries, corpus audio, or Melodyne research artifacts are included.
