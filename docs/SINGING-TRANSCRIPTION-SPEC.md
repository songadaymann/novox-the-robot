# Singing transcription engine

Author: Astra, extra-high reasoning. Design date: 2026-10-01.

Status: implementation specification. Accuracy on Jonathan's forthcoming annotated
recordings is not yet established. This document distinguishes the first native
implementation from later research; it is not a claim to match Melodyne.

## 1. Outcome and scope

Import a sung recording into the existing song editor with plausible pitches,
octaves, note boundaries and lyrics. Preserve the immediate editor workflow,
current song tempo and Undo. The first target is one audible, predominant voice,
especially an unaccompanied recording. Mixed instruments and multiple singers
remain a harder input category and must be measured separately.

Build our own musical interpretation pipeline around replaceable pitch evidence:

    audio -> candidate pitches -> coherent pitch path -> sung note events
          -> conservative lyric association -> ordinary editable score

The first implementation is native C++ and deterministic. It introduces no new
model download, Python runtime, external process or network service in the app or
plugin. Python remains acceptable for development measurements. YIN's difference
function is useful evidence; replacing the entire system does not require
discarding that evidence or training a neural network.

The first version must not infer an obligatory key, quantize event times to the
beat, assume the singer's register, globally transpose a recording, or invent
pitch solely to accommodate a recognized word. Recorded pitch and elapsed time
remain authoritative. A genuine short octave leap must be able to survive.

## 2. Current implementation and concrete failures

Relevant files are `engine/src/transcribe.{hpp,cpp}`, `engine/src/novox.cpp`,
`plugin/Source/AudioImport.cpp`, `plugin/ui/audio-import.js`,
`tests/engine_tests.cpp` and `tools/transcribe_eval.py`.

The current pitch tracker chooses one YIN minimum independently per 8 ms frame,
then moves pitches up or down an octave to match a 15-frame local median. The
segmenter has useful experimental held-note protection, but it also unconditionally
folds certain passing notes shorter than 150 ms and octave notes shorter than
200 ms. These rules can erase actual musical events as well as tracking errors.

Pitch frames are labelled at the center of a 512-sample integration window even
though the YIN comparison accesses an additional lag of up to 266 samples.
Boundary timing therefore needs an explicit sample-support convention and
calibration, not another unexplained global time offset.

The plugin currently computes pitch twice: once for its pitch curve, then again
through `novox_transcribe`. Both consumers should eventually use one analysis
result; at minimum they must use the same engine mode and parameters.

Lyric association can discard a word with only a few milliseconds of overlap.
For example, the documented “Can” interval ends at 0.60 s and the first detected
note begins at 0.596 s. A hard 20 ms endpoint rule drops it. Whisper's word
intervals are approximate and must not be treated as exact phonetic boundaries.

The prior six-voice synthetic round trips and current recordings are useful
regressions. Unannotated recordings cannot establish pitch accuracy. The fixed
plugin playback cursor and compatibility AU are outside this change's scope.

## 3. First implementation contract

Expose a named algorithm option, for example `"algorithm":"singing-v1"`, alongside
`"legacy"`. Keep the previous implementation callable for exact A/B measurement.
The native app/plugin explicitly selects `singing-v1` after the tests below pass.
Keep the public C function signature unchanged; the option travels through the
existing JSON argument. Preserve the legacy C API default so existing CLI/Python
callers and the synthetic benchmark retain an exact baseline; callers can select
the new engine explicitly. A later default change requires its own comparison.

Use separate internal stages, even if they initially share one source file:

1. Analysis of acoustic evidence, independent of words and musical key.
2. Selection of a time-consistent pitch/voicing path.
3. Conversion of that path and acoustic boundaries into note events.
4. Association of words with notes, with explicit unresolved words.

Suggested internal types are `PitchCandidate`, `CandidateFrame`, an extended
`PitchFrame`, and a transcription result holding frames, notes and word metadata.
They need not become public ABI types. Existing `PitchFrame` construction in tests
must continue to work through defaults for added fields.

Parameters with experimental values belong in one documented options/constants
section. Reject unknown algorithm names and non-finite, out-of-range options.
Output must always have finite pitches/times, positive durations, chronological
notes and valid JSON; a failed import must preserve the previous song.

## 4. Acoustic candidates

Retain the anti-aliased 16 kHz analysis path and an 8 ms hop initially. Search the
existing approximately 60–1100 Hz range; an optional documented range is useful
later, but the default must not encode an assumed male or female register.

For each frame:

- Compute normalized YIN differences and retain multiple local minima, rather
  than only the first acceptable minimum. Refine each retained lag parabolically,
  constrain the result to the supported frequency range and deduplicate nearby
  candidates. Keep a bounded candidate count, initially at most eight.
- Retain the first credible short-lag minimum as well as the deepest minima.
  Pure periodic audio also has minima at integer multiples of its true period;
  selecting only the deepest trough can create a persistent low octave.
- Store continuous MIDI pitch, periodicity quality, lag and a comparable emission
  cost for every candidate. Octave alternatives require measured support in the
  difference function. Do not simply clone every pitch at plus/minus 12 semitones.
- Include an explicit unvoiced candidate. RMS alone cannot distinguish a vowel
  from breath; periodicity alone cannot distinguish silence from a reliable note.
- Use a finite absolute energy floor in addition to a relative/noise-floor test.
  Silence and extremely quiet numerical residue must not become voiced merely
  because they are the loudest part of a file.

Resolve ambiguous multiple-period minima with a small, bounded preference for a
shorter plausible period, while allowing a sufficiently better-supported longer
period to win. Its strength is a parameter to test, not a blanket octave rule.
Periodicity quality must dominate that prior when a weak fundamental is supported
by its higher harmonics. A second-harmonic-heavy tone is a required waveform test.

Use a symmetric comparison support about each frame timestamp, or explicitly
account for the actual support of the selected lag. For a symmetric implementation,
compare pairs equidistant about the analysis center, with at most a half-sample
asymmetry for odd lags. Level measurements use the same center. Edge padding and
first/last frame rules must be defined and tested at recording start/end. Do not
extend a note beyond the actual decoded audio duration.

A harmonic-spectrum salience term could help later, particularly on weak
fundamentals. It is optional for the first delivery and should only be added when
a targeted waveform test demonstrates why it is needed. A spectral peak is not
automatically the sung fundamental.

## 5. Sequence decoding

Select the minimum-cost path through candidate frames with dynamic programming:

    pathCost[t,k] = acousticCost[t,k]
                   + min_j(pathCost[t-1,j] + transitionCost(j,k))

This is our own finite candidate sequence decoder, not a claim to implement the
full published pYIN algorithm. Retain backpointers so the chosen path uses future
as well as past evidence. Processing a complete offline recording is acceptable;
bounded independent phrases separated by clear silence are also acceptable.

Transition design:

- Nearby voiced pitches have a small smoothness cost; the cost for a large leap
  is capped. Sustained acoustic evidence must overcome it, including a real
  120–200 ms octave note.
- Entering/exiting unvoiced has a modest cost that discourages isolated toggles
  but permits actual consonants and silence. Unvoiced output remains unvoiced;
  continuity may inform segmentation without filling the raw curve with invented
  voiced samples.
- Reliable acoustic rearticulation may reduce the transition cost. Reset pitch
  continuity after a substantial silence so the next phrase can begin in a
  different register.
- Costs are normalized consistently for the fixed hop. If the hop changes later,
  transition and duration parameters must retain their meaning in seconds.

Remove local-median octave rewriting from this mode. The decoder chooses among
measured hypotheses; it never rewrites a chosen pitch to satisfy a neighbor.

Store acoustic reliability and an ambiguity measure separately when practical.
The top-two local candidate margin alone is not a calibrated probability that a
note is correct. A note can be periodic yet octave-ambiguous. Conservatively
lower reported confidence for weak or nearly tied evidence; do not label the
composite score as statistical accuracy.

### Numerical starting recipe

These are concrete initial values for implementation and regression tuning, not
parameters demonstrated to solve the current real recording:

- Candidate trough threshold: CMND <= 0.40; include the best trough when no
  threshold candidate exists, but let unvoiced win if it is poor. Deduplicate
  within 0.35 semitones and keep up to eight measured troughs. Apply the frequency
  limits after lag interpolation as well as during the integer search.
- Let `c` be the candidate's refined CMND and `tau0` the shortest retained trough
  with CMND <= 0.30. A starting voiced emission is
  `6 * clamp(c, 0, 1) + 0.08 * max(0, log2(tau / tau0))`. Omit the second term
  if no credible `tau0` exists. This is a weak period prior: a CMND improvement of
  about 0.014 overcomes a one-octave preference. Treat these minima as distinct
  measured hypotheses, including the fundamental when its second harmonic is
  louder. Do not add a stronger unconditional low-frequency or register bias.
- Start the unvoiced emission at 1.8 for energetic frames. For frames below the
  absolute/relative energy floor, make voiced candidates unavailable and give
  unvoiced cost zero. Candidate quality plus this emission initially puts the
  standalone voicing crossover around clarity 0.70. Use an absolute floor near
  -80 dBFS and a relative floor near 50 dB below a robust high-level percentile;
  the current `silenceDb` segmentation option remains separately documented.
- Start voiced transition cost at `min(0.30, 0.025 * abs(midiDelta))`, and entering
  or exiting unvoiced at 0.15. Reset the path across roughly 120 ms of clear
  silence. No special octave surcharge is needed initially.
- With these scales, a 120 ms true octave change has about 15 frames for its
  shorter-period evidence to overcome two capped transitions; a 48 ms ambiguous
  octave excursion has only six. This illustrates the intended tradeoff, not a
  proof for every waveform. The explicit short-octave regression decides whether
  the final values are acceptable.
- A reliable large jump may use approximately 32 ms confirmation in segmentation;
  a small center change starts at 64–80 ms, with a stable target spread of about
  0.5 semitones. Test the vibrato/short-semitone pair together when tuning these
  values. A minimum event duration of 80 ms matches the existing default.

An optional refinement is to identify approximately integer-multiple periods
whose CMND evidence is indistinguishable and mark them redundant for ambiguity
reporting. It must not remove the longer-period candidate: a true weak fundamental
can be at that period. Spectral harmonic evidence, if later introduced, must compare
the odd-harmonic support that distinguishes `f` from `2f`, not simply reward total
energy at multiples (which systematically favors subharmonics).

This decoder may confidently track an accompanying bass instrument in mixed
audio. Its confidence describes consistency with the evidence, not identification
of the singer. Neither the prior nor smoothness costs solve source separation.

## 6. From pitch path to intended notes

Note segmentation should identify stable centers and articulation, preserving
the detailed pitch path for diagnostics. It must not turn each vibrato crossing
into a note.

The first segmenter may build on the existing stabilized mode, with these changes:

1. Estimate a robust current pitch center from reliable frames. Require evidence
   of a distinct settled center before splitting for pitch alone: initially about
   60–80 ms for a small interval, shorter for a large well-supported change.
   Examine dispersion and same-direction persistence, not only distance from a
   trailing median. A 120 ms semitone step must remain distinguishable from a
   wide 6 Hz vibrato excursion.
2. Once a new center is confirmed, place the boundary back near the center-crossing
   of the transition, or at nearby acoustic rearticulation. Confirmation latency
   must not systematically delay the note's reported onset.
3. Bridge brief tracking failures inside a held note when energy remains present
   and the same pitch resumes. Preserve the unvoiced frames in the raw trace.
   A clear silence or energy valley followed by a renewed vowel can separate
   repeated notes at the same pitch.
4. Protect real rearticulation: a conservative dip/onset detector must retain
   “la la la” on one pitch. Require a valley with recovery and a minimum distance
   between accepted boundaries. Ordinary vibrato or modest tremolo alone should
   not create a succession of syllables.
5. Do not unconditionally delete a short note because it is an octave from its
   neighbor or lies between neighboring pitches. Use insufficient duration,
   ambiguous pitch evidence or an unresolved glide as evidence for merging.
   A stable 120 ms passing note or octave leap has positive preservation tests.
6. Estimate the final pitch using a confidence-weighted median or comparably
   robust center of the sustained portion. Exclude initial scoops and transition
   frames when a stable portion exists. Round only the final note pitch when
   `roundPitches` is enabled; preserve fractional pitch otherwise.

Retain pitch spread as an editing diagnostic. Large vibrato is not inherently an
incorrect note; confidence should primarily describe missing/ambiguous evidence.
Keep note start/end refinement independent of any host tempo or grid.

The first version need not solve overlapping singers, every expressive scoop,
all deliberate tremolo/repetition ambiguity, or unpitched speech. These cases
must remain visible as limitations rather than receive fabricated certainty.

## 7. Lyrics and preservation of words

Keep pitch inference independent of the recognizer. Whisper remains responsible
for recognizing text, while its word times are soft association evidence.

Use temporal overlap first, including small positive overlaps that the current
hard endpoint rule rejects. An optional small, bounded tolerance may associate a
word just outside a note, but such an association is marked uncertain. Avoid
unbounded nearest-note assignment across rests.

Retain current syllable/melisma handling where valid. A word boundary may split a
held pitch when it falls inside that event with enough duration on each side;
this is a lyric-informed split and should lower boundary confidence or generate
a review warning. Do not split at an arbitrary midpoint merely to fit word count.
Word timings must never transpose a note or move an event into absent audio.

Every nonempty recognized word must survive the operation, either associated with
a note or in structured unresolved-word metadata. Preserve the raw recognized
word array and original order, including repeated words. Suggested additive score
metadata is:

```json
{
  "transcription": { "algorithm": "singing-v1" },
  "transcriptWords": [
    { "word": "Can", "start": 0.0, "end": 0.6, "probability": 0.9 }
  ],
  "unassignedWords": [
    { "wordIndex": 0, "reason": "no_reliable_note" }
  ]
}
```

Equivalent clearly documented names are acceptable. Structured preservation is
required; a discarded word appearing only inside an ephemeral warning is not
enough. If the standard editor cannot place an unresolved word automatically,
keep it in the score/import metadata and show a concise import notice listing
the unresolved text. Do not bring back the intermediate review screen. Ensure
save/load or the existing score update path does not immediately erase metadata.

## 8. Annotation and benchmark contract

Add an evaluator mode for human annotations without removing the existing
synthetic round-trip mode. Input is a small versioned JSON manifest, for example:

```json
{
  "version": 1,
  "cases": [
    {
      "id": "held-you-belong",
      "audio": "held-you-belong.wav",
      "tags": ["solo", "held-note", "vibrato"],
      "notes": [
        { "start": 0.45, "dur": 0.65, "pitch": 60, "lyric": "you" },
        { "start": 1.10, "dur": 0.40, "pitch": 62, "lyric": "be-" },
        { "start": 1.50, "dur": 1.80, "pitch": 64, "lyric": "long" }
      ],
      "words": [
        { "word": "you", "start": 0.40, "end": 1.05 },
        { "word": "belong", "start": 1.05, "end": 3.30 }
      ],
      "ignore": [],
      "comment": "Illustrative values only; replace with a human annotation."
    }
  ]
}
```

Paths are relative to the manifest. Times are seconds from decoded audio sample
zero. Pitch is MIDI number, with C4 = 60. `dur` is positive. Notes represent the
intended musical events; do not label each vibrato wiggle or scoop as a new note.
Use an interval in `ignore`, such as `{"start":2.0,"end":2.3}`, only where the
annotator cannot establish the intended event. Validate and report the excluded
duration. Melody notes and word intervals are separate: one word may span several
notes, and several words may occupy one sustained pitch. Words are optional.

The example is a schema example, not ground truth for any existing recording.
Raw recordings and personal annotations may remain under gitignored `reference/`;
commit only generic examples and generated-signal fixtures unless Jonathan
explicitly wants his material tracked.

For each case and aggregate, report:

- Note precision, recall and F1 with exact semitone pitch and onset within 60 ms.
  Use maximum-cardinality one-to-one matching, with onset error as a tie-break;
  one estimate cannot satisfy multiple reference notes. Report tolerance settings.
- A separate strict event score that also requires end time within the larger of
  80 ms or 20% of reference duration. This distinguishes a correctly started held
  note from a fragment. Do not silently replace the headline criterion.
- Median and 90th-percentile absolute onset/offset error on matched notes. Report
  “not available” when there are no matches, not a misleading zero error.
- Octave substitutions, other pitch substitutions, missed notes and extra notes,
  using a separate time-associated diagnostic matching. State that these
  diagnostics differ from pitch-correct matching used for F1.
- Held-note fragmentation and repeated-note misses for tagged cases. A lower
  output count alone is not evidence of improvement.
- Word preservation: recognized input word count, assigned count and unresolved
  count. Test 100% conservation independently from word recognition accuracy.
- Runtime, audio duration and engine/options identity. Save both machines-readable
  JSON and a concise text summary, including per-case failures.

Compare `legacy` and `singing-v1` on the same decoded samples. Run melody-only
evaluation separately from alignment with fixed supplied word timestamps so
Whisper variability does not conceal a melody regression. Optional full-native
Whisper evaluation is a third measurement, not a replacement for either.

The old synthetic evaluator's precision/recall stays useful, but its greedy
matching, onset-only truths and mean-zero-on-no-match behavior are insufficient
for the annotated benchmark. Do not call note-label accuracy “frame-level F0
accuracy”: true frame-level pitch metrics require separate continuous F0 truth.

## 9. Regression and integration acceptance

Extend the existing C++ test executable with generated waveform tests as well as
direct pitch-frame tests. The waveform tests must exercise candidate extraction
and decoding; frame-only tests cannot detect octave mistakes in the detector.

Required first-delivery cases:

| Test | Expected behavior |
| --- | --- |
| Silence, very quiet input and deterministic noise | No stable false melody; finite output |
| Fundamental plus stronger second harmonic | Correct fundamental, not a doubled octave |
| Brief timbre change that favors an octave | Continuous held event when the true period remains supported |
| Real octave up/down for 120–200 ms | Short actual event retained |
| Sustained wide 6 Hz vibrato | One intended note |
| 120 ms semitone step and passing note | Deliberate short note retained |
| Initial scoop into a held center | Stable target pitch; sensible single event |
| Brief unvoiced glitch at steady energy | No artificial extra syllable |
| Repeated same pitch with acoustic valleys | Separate repeated notes |
| New phrase after silence in a new register | No forced return to old octave |
| Tone beginning at sample zero or ending at EOF | No fixed unexplained onset loss; end bounded by recording |
| 16, 44.1 and 48 kHz input | Same intended pitches; onset agreement within one analysis hop plus resampling tolerance |
| “Can” with only a small positive first-note overlap | Text retained; uncertain association or explicit unresolved entry |
| Word in a genuine rest, repeated words, shared-note words | No fabricated pitch event; no lost text; chronological output |
| Invalid algorithm/options and malformed words | Clear error or documented sanitation; valid existing song preserved |

Use tolerances appropriate to the fixture; for abrupt generated note starts,
initial target is <= 30 ms timing error, except explicitly ambiguous transitions.
Do not weaken a musical preservation test just because an early parameter set
fails it. Tune costs or retain the input as a known failing case with clear status.

Run the full engine suite, native plugin tests and an actual WAV/MP3 import through
`AudioImport`. Build standalone, AU, VST3 and the compatibility AU so both installed
plugin identities receive the same engine. Preserve existing processor and
installed-AU playback smoke tests; no import change should regress the resolved
clicking problem. Check direct editor import and Undo through the existing UI
harness. No additional import screen or model consent is introduced.

Do not gate the first native implementation on annotations Jonathan has not yet
provided. Before those arrive, accept architectural integration and deterministic
regression behavior, and label musical quality as awaiting real-voice evaluation.
Once annotations exist, compare every case, inspect worsened cases and report
changes in octave errors, fragmentation, F1 and timing explicitly. Human listening
and correction remain part of acceptance; synthetic success is not enough to
declare natural-voice accuracy solved.

## 10. Implementation order and future work

Deliver the following together as the first iteration:

1. Named version/legacy selection and candidate/decoder implementation.
2. Evidence-based segmentation without unconditional octave/passing-note deletion.
3. Word conservation metadata and conservative association fixes.
4. Annotated benchmark input, example and output reporting.
5. Native import selection, tests and fresh plugin/application builds.

The minimum complete first delivery does not require a new neural pitch detector,
vocal separator, tuning UI, continuous score automation or new piano-roll design.
Avoid an architecture that requires those to work before improvements can be
measured. If harmonic ambiguity remains, preserve the failing clip and measure
which stage caused it before adding more heuristics.

Later experiments may compare a learned monophonic detector, a vocal-specific
pitch detector and a separation-plus-detection pipeline behind the same candidate
or pitch-frame interface. Model redistribution rights, model size, native runtime
compatibility, Mac performance and accuracy on the held-out clips are adoption
criteria. Names such as CREPE or RMVPE describe candidates to investigate, not
promised integrations or assumptions about licensing. Fresh model training and
competitive claims against commercial products are separate projects.

Keep a few clips as a held-out set once enough examples exist. Tune on the rest,
then report held-out results so this engine improves beyond whichever recording
was most recently heard.

## October 1 measured iteration

The initial recipe above remains the historical starting point. On the aligned
life/sleep references, the accepted native recipe uses a 0.30 shorter-period
prior and unvoiced emission 1.2. The 0.60 period prior failed the harmonic-balance
regression and was rejected. Allowing wider settled-pitch spread failed the scoop
regression and was rejected. Neither rejected variant is shipped.

A weak frame can recover only a measured candidate within 0.6 semitone of a
confident decoded neighbor (clarity at least 0.85) within five hops / 40 ms. Its
CMND must be at most 0.30 and its energy above the voicing floor. Recovery uses
the original path as anchors, so it cannot propagate through a rest. Its confidence
is reduced. This retains breathy onsets without treating unrelated low tails as
notes. The decoder does not receive MIDI references during inference.

These two examples are now tuning examples, not held-out accuracy evidence.
Some MIDI/audio disagreements in life have independently measured harmonic
peaks supporting a different semitone; do not silently retune the audio to the
reference. Report event accuracy and duration-weighted pitch agreement separately.

## October 1 syllable alignment and optional key correction

For singing-v1, lyric alignment first attempts a bounded monotonic phrase-wide
assignment to existing events, using pronounced vowel counts and soft ASR timing.
It requires enough existing events, chronological words and feasible temporal
association (450ms slack); gaps/noise events can remain unclaimed. It never
creates/moves notes for that assignment. If infeasible, existing conservative
word association remains, with warnings for insufficient syllable boundaries.
The fixed sleep recording now aligns re-/mem-/ber/me; its melody is unchanged.

The ordinary editor has explicit Key/Scale/Gentle-or-Snap/Correct controls. Key
starts Off and import remains as sung. Correct affects selected notes or all notes
if none are selected, selects changes, and uses the existing Undo checkpoint.
Settings and correction audit survive Song JSON/Logic project state. Gentle uses
import evidence matched by original beat timing and unchanged pitch; it only
moves an uncertain out-of-scale note within0.65semitone of a scale tone. Confident
low-spread chromatic notes, tied directions and notes lacking evidence stay put.
Snap explicitly chooses the nearest scale tone, with the lower tone breaking an
exact tie. Both are bounded to MIDI0..127. Major and natural minor are supported.

Continuous detectedPitch is retained alongside confidence/spread; import evidence
is saved under transcription.originalNotes with originalNotesUnits="beats".
Original transcriptWords still use audio seconds. Correction does not feed key or
MIDI answers into the pitch detector, and is not counted in the raw benchmark.
Repeated-note splitting remains conservative pending stronger acoustic evidence.
