# singing-v2: syllable-first vocal import

Status: shipped in the plugin, 2026-10-16 (uncommitted). Every plugin import
(standalone, AU, VST3 and the compatibility AU) runs `"algorithm": "singing-v2"`
with `"detail": "natural"`, or `"detailed"` when the editor's Detailed notes box
is ticked. This document describes the code; the held-out results are under
Evaluation. Supersedes the segmentation and octave parts of
`SINGING-TRANSCRIPTION-SPEC.md`; `legacy` and `singing-v1` stay callable,
unchanged, for A/B (the C API default is still `legacy`).

## Why

Jonathan's complaint about importing sung vocals: words get split into notes in
unnatural places, and notes jump an octave in the middle of a word. Measured on
21 real takes (16 of them new clips of Jonathan's own voice, see
`reference/corpus/manifest.json`), singing-v1 produced 123 within-syllable
splits, 112 false octave errors and 83 lyric misplacements. Root causes:

1. **Octaves.** This voice often has period-doubled (rough, pressed) phonation:
   the waveform nearly repeats at 2T, so YIN's dip at twice the period is
   deeper than at the true period, while listeners hear 1/T. The low-band
   spectrum shows strong harmonics of f0 and weak half-harmonics. v1's
   path decoder caps any pitch jump at 0.30, so it switches octave for whole
   syllables whenever 2T is slightly more periodic. Sometimes the true period
   is not a candidate at all (its trough is above the 0.40 cap). The note-level
   octave fold is disabled for singing. Octave-up errors (weak fundamental,
   strong 2nd harmonic) occur too.
2. **Splits.** v1 finds notes from pitch first and assigns lyrics afterwards,
   so every vibrato swing that holds 80 ms, scoop, post-consonant dip, level
   dip of 5 dB or octave blip cuts a note, and the lyric aligner (which can't
   merge notes) spreads a word over the fragments.
3. **Merges and lyric drift.** Legato syllable changes (nasals, liquids, vowel
   changes) show up in band energy and spectral change, not the overall level
   v1 uses, so syllables merge. The lyric DP must give each word at least its
   dictionary syllable count of existing notes; one error shifts every later
   syllable. ASR word starts are just the previous word's end.

## Principles (shared with Melodyne, ROSVOT, SOME, Tony)

- A syllable onset is a note boundary, even without a pitch change.
- Inside a syllable, pitch adds a note only for a real melisma: two settled
  plateaus. Vibrato, scoops, overshoot, portamento and fall-offs belong to one
  note (drift and modulation around a pitch centre).
- Octave is decided per note from all its frames, plus continuity with its
  neighbours. Frame-level octave flips inside a note are tracker errors
  (a real 12-semitone glide takes ~120 ms or more in voice). The notes of one
  word share a register: the word's octave is decided from all its notes'
  frames, and a leap of 9 or more semitones inside a word (none in
  Jonathan's dev references; GTSinger 4 of 711 and CSD 22 of 1081 adjacent
  same-word pairs, up to the octave of "Some-where") survives when both sides
  hold their octave firmly.
- Recorded pitch and time stay authoritative: no key, no beat quantization,
  no assumed register; a real short octave leap must survive.

## Conventions

- Note `start` is the syllable's **vowel onset** (Novox's planner places onset
  consonants before the note start). `end` is the next note's start when
  legato, otherwise the end of audible sound before a rest.
- Lyric tokens are as today: `"re-"`, `"mem-"`, `"ber"`, `"_"` for a melisma
  continuation, `""` for a hum (voiced audio no word claims).
- Times in seconds on the shared analysis timeline: frame k is centred at
  k × 8 ms of the 16 kHz signal (`engine/src/spectrum.hpp`).

## Architecture and ownership

```
audio -> resampleTo16k, removeDc (spectrum.cpp)
      -> analyzeSingingPitch (singing_pitch.cpp, track A)       candidates + path + evidence
      -> segmentSinging     (singing_segment.cpp, track B)       phrases -> syllables -> notes
            uses analyzeArticulation / findBoundaryCandidates (articulation.cpp, track B)
            uses syllabify (syllables.cpp, track C)
            uses consistentContour / assignNotePitches (track A)
      -> transcriptionToJson (unchanged format)
```

`hyphenate` (track C) splits a spelling into a given number of lyric-sheet
pieces with the same cut rules `syllabify` uses for its `text`; the engine
does not call it yet.

Interfaces are in the headers; each file says what it owns. Tests per track:
`tests/singing_pitch_tests.cpp`, `tests/singing_segment_tests.cpp`,
`tests/syllables_tests.cpp` (registered in `tests/engine_tests.cpp`).

### Track A: pitch (`singing_pitch.*`)

- Input: any rate resampled to 16 kHz (the windowed-sinc filter widens above
  48 kHz so content above 8 kHz cannot fold in), then a 20 Hz one-pole
  high-pass removes DC, which would otherwise raise every frame's level.
- Candidates from 60 Hz to 2.1 kHz (C7: whistling and the whistle register):
  symmetric YIN CMND minima (cap 0.5, so a true period with CMND
  up to ~0.5 is kept) of the better of the full band and a ~1.3 kHz low band
  (period-doubled upper partials stay out of the low band), plus sub-multiple
  lags τ/2, τ/3, τ/4 of every candidate with CMND ≤ 0.3, refined to the best
  nearby lag even when it is no local minimum (kept only if that lag is still
  fairly periodic, CMND ≤ 0.7), so the true period is always considered; at
  most 12 per frame, with the shortest credible period and the shortest of the
  deepest dips kept first (at high pitch the true period's dip is a hair
  shallower than its multiples').
- Harmonic evidence per candidate from a 1024-point spectrum: the partials k·f
  against the positions a pitch f/2 or f/3 would add ((k−½)·f; (k−⅓)·f and
  (k−⅔)·f), linear amplitudes weighted fully to 1 kHz and not at all from
  1.8 kHz, where pitch is heard from resolved harmonics (the 5.5th-10th
  harmonics of a 185 Hz lower pitch, where this was calibrated). For an
  octave pair whose lower pitch f/2 is higher, the band follows it (its
  first 5.5 harmonics in full, none from its 10th, inside the 2.9 kHz
  analysed): a G4-B4 voice has only H1 and H2 under 1 kHz, and a weak H1
  under a strong H2 alone reads as the octave up (R4 audit "jimothy": 11 of
  35 notes). Only when f/2's own fundamental is present (within 25 dB of the
  partial at f): a period-doubled note's half-partials stand up to 8-12 dB
  above both neighbours at 2-2.5 kHz while its f/2 partial is ~33 dB down,
  and such a lower pitch keeps the fixed band. Empty in-between
  positions favour f over f/2 (period doubling); f is too low when 3f, or 2f,
  explains the spectrum (for 2f unless f's odd harmonics rise above an even
  neighbour, as a weak fundamental's do). A reading whose first three
  harmonics are all 25 dB under the strongest partial is penalised. Evidence
  only ranks octave-related candidates; voicing stays with full-band
  periodicity. No unconditional shorter-lag bias.
- Path decoding with a speed limit: cost grows with the interval per frame
  (about 6 for a one-frame octave), cheap small moves, pitch memory across
  short unvoiced gaps (200 ms) so voiced→unvoiced→voiced cannot flip octave
  for free; across real consonant gaps between syllables, non-octave jumps
  stay cheap. Frames 50 dB under the loud frames within ±2.5 s (and below
  −110 dBFS) are silence; a quiet section's floor follows its own level, down
  to 30 dB under the take's. Readings below E2 need strong support (chord
  subharmonics land there). Tonal breaths leave the first pass's voicing: an
  inhale can whistle (a lone 1.5-2 kHz tone in noise) that a compressed take
  lifts to singing level, where it read as a note 2-3 octaves over the melody.
  A voiced run that is mostly tone-only frames (strongest bin under 1 kHz at
  least 20 dB under the strongest to 2.5 kHz, which no voiced frame of the four
  corpora reaches) with its tone 24+ semitones over the take's median reading
  becomes silence with no candidates, as do frames of a run with such a tone
  that peak within 1.5 semitones of it (breath noise under 1 kHz hides the
  rest of the whistle), unless such frames add up to more than 4 s or 5% of
  the voiced frames (a whistled passage). A syllable the path voices nowhere,
  pitched from its frames' candidates, takes a neighbour's pitch instead when
  that centre is more than 19 semitones from the take's median. A second pass adds a soft prior toward the singer's local register
  (the ±5 s median of frames whose octave was not contested in the first
  pass). Voiced runs then extend up to 5 frames through breathy onsets and
  releases along a continuous candidate chain.
- Octave flips: the decoder still takes a one-hop octave jump when the
  spectrum turns ambiguous for long enough (a weak-fundamental voice whose
  odd harmonics fade as its vowel moves reads as its own 2nd harmonic for as
  long as they stay faded). Whether the voice moved is told by its lower
  octave's fundamental partial: a real jump up leaves it behind, a flip keeps
  it at the level it had (the lower reading's H1−H2 within 8 dB on both
  sides, held over 80 ms of the upper side, so a release decaying under a new
  note — R4 audit "the end": −24 to −43 dB within 80 ms — does not pass). The
  upper side then takes the lower reading up to the next jump, a voicing gap
  over 200 ms or the fundamental fading, and its upper reading loses its
  spectral advantage so the notes built on it carry no evidence for it. The
  lower side must be a reading the spectrum supports (harmonic penalty ≤ 0.5;
  period-doubled readings jumping up to their true pitch are penalised
  0.6–1.4), and accompanied takes (polyphony ≥ 0.35) are left alone. On the
  four dev corpora it changes no frame; on the R4 audit "jimothy" it fixes 8
  of the 11 octave-high notes (two of them, the held "Mur-phy", together with
  the band), the word-level decision the other 3.
- `consistentContour`: relabel short (<300 ms) runs 12/19/24 semitones off the
  span's dominant register to a supported candidate; otherwise drop them. A
  span with two registers each held ≥ 300 ms is a real leap: both keep their
  octave.
- `assignNotePitches`: per note, score octave hypotheses (path −12/0/+12)
  over all voiced frames by the candidates' per-frame cost (CMND and harmonic
  evidence). Frames that cannot tell the octave do not vote: readings an
  octave under the note's own modal frames whose fundamental is ≥ 8 dB weaker
  than theirs (smooth period doubling, or a bass an octave under the voice)
  count an octave up instead, and in an accompanied take (polyphony ≥ 0.35)
  frames with a second harmonic series are muted. A syllable sung
  period-doubled throughout has no modal frame of its own, so a note's frames
  are also judged against the modal frames of the other notes of its word
  (`NoteSpan::word`) an octave above the reading (±3 semitones). Moving off
  the path's octave needs a margin, scaled by the share of frames that carry
  evidence. A small DP across notes ≤ 0.3 s apart (a word's notes at any
  distance) adds a continuity prior: a ≥10-semitone move between adjacent
  notes needs clear evidence, and a ≥9-semitone move between two notes of one
  word also costs up to the evidence of 80 ms of frames that rule the other
  octave out, scaled by how undecided the less decided note is (1 − its octave
  margin per frame of evidence, over at least 40 ms, as a share of the most a
  frame gives). A word takes the register its frames favour together; a leap
  whose notes both hold their octave survives however short ("dog-gie" with a
  40 ms "gie"; a flat cost dropped it and GTSinger's "in-to"). A
  note whose chosen octave no candidate supports takes the path's register
  moved to it. Centre: a
  mode-seeded mean shift (±0.6 st) that ignores scoops, fall-offs and
  overshoots, taken on the one-cycle average of the contour when it carries
  vibrato (so a slow approach sung with vibrato does not pull the centre),
  falling back to the weighted median when the mode holds under 30% of the
  weight. A note the path never voices (a rough syllable) gets the centre of
  its frames' best periodic candidates, with confidence ≤ 0.3; inside a word
  it takes the octave of the word's nearest voiced note (it has no octave
  evidence of its own).
- `vibratoCentreLine`: inside each run of whole vibrato cycles (the cycles
  `robustCentre` finds) every voiced frame of a contour becomes the mean over
  one cycle around it, so a held note sung with vibrato is flat and a step
  between notes still moves the line.
- `polyphony`: fraction of voiced frames with a strong peak outside the best
  single-voice harmonic series (accompaniment bleed, chords; period-doubling
  half-partials excluded); the segmenter warns from 0.35.
- `estimateTuning`: the take's pitch reference, as Melodyne detects one. A
  circular mean of the note centres' fractional pitch (weight: seconds held,
  capped at 1 s, x pitch confidence), refined by Tukey-weighted passes (notes
  more than 35 cents off the estimate stop pulling on it). Note pitches round
  against it (`detectedPitch` stays absolute; the result reports
  `"tuningCents"`). It applies only when the offset is clearly there (Rayleigh
  n_eff R^2 >= 4.6, p < 0.01, over at least 25 effective notes), between 25 and
  40 cents (nearer a quarter tone either grid is a guess, so A440 stays), and
  the take has no accompaniment (polyphony < 0.25: the band, almost always at A440, is the
  reference then). Dev: amateur and natural singing scatters +-25-50 cents
  note to note, so most takes stay at A440. With the 25-cent dead zone it
  fired on 3 of the 250 dev takes of the four corpora before the 25-note and
  40-cent limits (which now keep all three at A440), all short GTSinger
  phrases (n_eff 10-21) whose notes sit 25-60 cents off the score on both
  sides (interval compression, which folds near +-50 cents on the circle):
  -34, +29 and -49 cents, and on each the score's pitches and intervals get
  worse (pitch right 21 -> 15 of 27, 20 -> 19 of 29, 14 -> 9 of 19). On dev takes detuned by
  -40 cents the consistent singers' notes come back (voc-2 21 -> 34 of 45,
  voc-3 30 -> 42 of 49, demodex 35 -> 45 of 51). No drift tracking: dev takes
  are 8-35 s and show no slow drift, and windows of 15-30 s hold too few notes
  for the p < 0.01 test.

### Track B: segmentation (`singing_segment.*`)

- Articulation features (`articulation.*`) on a 512-point spectrum: band
  levels L 80–400 Hz, M 640–2800 Hz (vowel band), H 3.5–7.8 kHz (frication)
  and total, floored 90 dB under the take's singing (not at a fixed dBFS, so
  any gain gives the same features); voicing from the pitch path; spectral
  change (MFCC distance across ±48 ms); vibrato strength (autocorrelation of
  the detrended pitch contour at 4–10 Hz). Level decisions (voicing gate,
  candidate checks, the onset model's level term, phrase building) use the
  loud level of the frame's own phrase within ±1.5 s, so a quiet verse is
  judged against itself; the drop rule uses the level of singing nearby in
  any phrase. Weak frames, loud vowel-like frames the path leaves unvoiced but
  whose low band is periodic with consistent partials (rough syllables), count
  as sung for phrases and note ends.
- Candidate vowel onsets at spectral-change peaks and voicing onsets (moved
  back to the vowel-band rise when the tracker voices late), each with
  measurements (voicing gap, frication, M and H dips, spectral change, M rise,
  settled pitch step, level) and logistic models fitted on the dev split: the
  probability of a syllable onset of each `BoundaryKind` (unvoiced, voiced
  obstruent, sonorant, vowel-initial) and of any onset. Inside vibrato, onsets
  without a voicing break or a deep M dip are scaled down, so vibrato-rate
  level and spectral changes don't split.
- Phrases at unvoiced or quiet spans ≥ ~250 ms.
- Segment lengths are compared in whole frames (88 ms and 160 ms are exact
  multiples of a frame; a difference of two absolute times would land on
  either side depending on position and compiler).
- Global monotonic DP placing the expected syllables (from `syllabify`) on
  candidate onsets: the onset probability (the expected consonant class can
  only raise it over the class-free model; on dev, class models alone were no
  better), a broad log-normal duration prior, soft anchors from word end times
  and token times (calibrated on the dev set; word starts are not anchors),
  phrase gaps must be segment boundaries. A word end bounds both sides: the
  next word starts no earlier, the word's sound ends near it, and its last
  syllable starts no later (+30 ms; without this a missed legato onset shifts
  every following word a syllable late); the two costs on one word end share
  a cap of about one clear onset, since whisper can end a word early when its
  first syllable is held. Up to four syllables of one word may
  share a note, cheapest for hiatus vowels, then optional syllables, then any
  other. ASR words can be skipped (cost by length and confidence); unclaimed
  voiced audio becomes lyric-less notes (a base cost plus a cost per second,
  cheaper when continuing a run of them), so dropped vocables ("oh oh oh")
  still appear; a hum under 150 ms is a release or, between lyrics, the next
  syllable's onset consonant, so it is no note unless it is a whole phrase.
  After the DP each hum is cut where a take without words would cut it
  (onset p > 0.27): unclaimed audio holds as many notes whether or not other
  phrases have lyrics (p > 0.5 left other singers' unclaimed "la"s and
  non-English phrases under-split). Quiet sound (tails, breaths) can be
  dropped without a note.
- A transcript that cannot be the lyrics: when the words promise fewer
  syllables than a quarter of the clear onsets (phrase starts plus candidates
  with p > 0.5; at least 12) in the span the words cover (so a transcript that
  stops after the first verse keeps its lyrics there), and at least half of
  its words are that thin over their own [start, end] (word times tile the
  take, so a recogniser that skips lines stretches the word after the gap
  over them; a few such words are unheard lines and the others keep their
  lyrics), the take is segmented
  as melody only, every word is reported unassigned, and a warning says so. Dev ratios: 0.59-1.7
  for real or mis-heard but syllable-faithful transcripts (whisper's English
  guesses at Portuguese or French still time the syllables well), 0.01 for a
  one-word guess at a 20 s Mandarin song (vocadito voc-36: 0.48 -> 0.60).
- Inside each segment: a least-squares piecewise-constant fit of the
  octave-consistent contour proposes plateaus (≥ 120 ms of voiced frames);
  frames 20 dB under the segment's peak and murmur frames (vowel band against
  low band 15 dB under the vowel's: a nasal coda, a low instrument) are left
  out. Each piece's plateau is its longest run within ±0.2 st on the
  contour's vibrato centre line; a piece holds a pitch when that run lasts
  ≥ 80 ms (scoops, fall-offs and glides keep moving; vibrato is flat on its
  centre line). A melisma split survives only between pitches ≥ 0.8 st apart,
  measured plateau to plateau when both pieces hold one (a piece mean counts
  half of each transition and shrinks short notes' steps), else between
  piece means, where more is needed under vibrato. Short (by sounding
  length) or gliding first pieces (scoops), short, gliding or fading last
  pieces (fall-offs), short inner pieces, inner glides between their
  neighbours and A-B-A excursions are merged back, except that a short first
  piece that holds a pitch ≥ 2.5 st below the next or ≥ 0.8 st above it, a
  short last piece that holds a pitch ≥ 1.25 st above the previous one, a
  quiet last piece that holds one for 200 ms without fading (a note sung
  softly), and an excursion that holds its own pitch (a neighbour note) are
  notes. Up to a whole tone below, a held start is the singer reaching for
  the note, and a settled droop at a syllable's end is a release: the audited
  references and the CSD score keep both in one note (the trained CSD singer
  holds ~2 st under the target for ~180 ms; at a 1.25 st threshold those
  doubled CSD's fragments, COnP F 0.895 -> 0.851), while vocadito's and
  GTSinger's annotators mark them (and grace notes under 80 ms, and gliding
  approaches) as notes of their own (2.5 st costs vocadito 0.014 and GTSinger
  0.007 COnP F). Novox sings the intended melody, so those conventions are not
  chased. A scoop or fall-off joining a
  settled plateau leaves its pitch and drift alone, while a turn (58-60-58
  with every piece held) stays three notes. A last piece after ≥ 64 ms
  without voicing is no fall-off but another sound: the note ends at the gap.
  With words, melisma notes are `"_"`, inside hums too (sustain the hum
  rather than start a new "mm").
- Note detail (`"detail"` option, `MelodyOptions::detail`): `"natural"`
  (default) is everything above. `"detailed"` writes what annotators who mark
  every pitch they hear write (vocadito, GTSinger): it keeps every natural
  note and divides each one again with finer rules (`kDetailedRules`): pieces
  from 64 ms, a pitch held 64 ms is a plateau, held approaches from a step
  (0.8 st) below or above, settled tails a rise or droop of 1.25 st, gliding
  approaches and fall-offs that hold a pitch first, short inner pieces that
  hold (grace and passing notes), and quiet tails held 64 ms without fading.
  Those holds must be on the contour itself, not only its vibrato centre line,
  which sits flat for ~100 ms where a leap rises into vibrato. Vibrato,
  tremolo and glides or scoops without a plateau stay one note. Natural notes
  are byte-identical to the natural-only engine on all four dev corpora (with
  and without words); every natural note start is kept in detailed mode. Dev
  COnP F, natural -> detailed (tuned on vocadito and GTSinger; entropy-1.5
  words, see Evaluation for the shipped words):

  | corpus | with words | melody only |
  |---|---|---|
  | vocadito | 0.696 -> 0.743 (merged 189 -> 156) | 0.628 -> 0.688 |
  | GTSinger | 0.663 -> 0.667 (merged 328 -> 237) | 0.599 -> 0.613 |
  | Jonathan | 0.824 -> 0.788 (fragmented 19 -> 46) | 0.733 -> 0.723 |
  | CSD | 0.894 -> 0.827 (fragmented 164 -> 467) | 0.806 -> 0.749 |

  Jonathan's and CSD's references follow the natural convention (a score's
  notes), so detailed mode is opt-in.
- Without words (melody only): every segment is a lyric-less note; a note
  starts at any candidate with onset probability above ~0.27 (lower than the
  ~0.9 needed to leave the lyrics), plus melisma splits.
- Pitches from `assignNotePitches`, each note tagged with its spoken word
  (or the hum whose melisma it is) so a word's octave is decided once. A
  legato note runs to the next vowel onset and so holds the next syllable's
  onset consonant, often already moving to the next pitch; when the next
  syllable starts with a consonant, the note's pitch comes from its frames
  before the vowel-band minimum in the 96 ms before that onset (the dip the
  onset model measures; never its first 40 ms), unless every frame before
  that minimum stays 10 dB under the frames after it: then the note's onset
  sits on a murmur before its own vowel (vocadito "wa-ter": a −47 dB murmur
  read at 43, its vowel at −20 dB) and keeps all its frames. Lyrics from the syllables;
  warnings for skipped words, unclaimed audio, polyphony.

### Track C: syllables (`syllables.*`)

- `syllabify`: lexicon phonemes split by maximal onset with English legal
  onsets (the planner has the onset list; "ng" never starts a syllable),
  stress, optional (unstressed reducible) syllables, and an onset class within
  the word (a flapped t counts as voiced). The segmenter does not use that
  class: it classifies each boundary itself from the previous syllable's coda
  (across word boundaries too) plus the onset, with `consonantClass` and no
  flap rule (using the flap-aware class instead changed nothing on dev).
  Tokens are read as sung: numbers as number words, short all-caps acronyms
  letter by letter, "-in'" as the -ing word; vocables are words, not the
  dictionary's abbreviations ("ba" is not B.A.), and a held vowel written out
  ("nooo", "aah", "heyyy") is one syllable. A run of more than 64 letters is no
  word (the dictionary's longest has 28) and is cut evenly without the
  quadratic letter alignment.
- `hyphenate`: natural spelling pieces matching the syllable count
  ("an-y-more", "hel-lo", "re-mem-ber", "beau-ti-ful", silent e, "-le",
  "-tion", doubled consonants split between syllables).
- UTF-8 (`utf8.hpp`): letters are ASCII and accented Latin letters (Latin-1,
  Extended-A, pinyin, Romanian and Vietnamese letters, with any combining
  accents after them), read as their base letter for pronunciation and kept
  whole as written ("tête", "corazón", "Straße"); other scripts and emoji
  separate words and are never cut. The same holds for singing-v1 and legacy
  pieces (`cleanWord`, `spellingParts`), which used to test bytes with the
  host-locale-dependent `std::isalpha`. Result JSON repairs any invalid UTF-8
  echoed from the caller's words (U+FFFD), and the JSON reader decodes
  surrogate-pair escapes. Pronunciation stays English letter-to-sound rules,
  so a French "été" counts one syllable; Chinese characters (one syllable each)
  were tried and dropped (vocadito voc-35 0.60 -> 0.42: the lyric path
  under-splits legato steps there), so CJK words are left out and their audio
  is segmented as melody only.

### Track D: plugin (`plugin/Source/AudioImport.cpp`)

- Whisper's per-token DTW times go to the engine as `tokens` on each word,
  beside `end` (`AudioImport::wordsFromPieces`). Words are clamped to the
  file: a run of tokens sharing one DTW time at the end of a file (a loop
  decoded in the last window) used to chain word starts past the end, and
  the engine rejected the last word (`invalid timestamped word`).
- Options come from `AudioImport::transcribeOptions(detailed)`. The editor's
  Detailed notes checkbox is read when a recording is opened (file, dropped
  file or region, singing take) and passed as `startAudioImport`'s fifth
  argument; all four formats share `PluginEditor.cpp` and `AudioImport.cpp`.
- whisper.cpp v1.8.3 is patched (`plugin/patches/`): its DTW median filter
  asserted, killing the host, on a window under 160 ms (CSD en023b, en012a).
- whisper's repetition threshold stays at its default (`entropy_thold`
  2.4). 1.5 kept "la la la" lines on Jonathan's dev takes but let long
  loops through on held-out full songs (Evaluation).
- The LCD shows the warning count and the most serious warning
  (melody-only fallback, then accompaniment); its tooltip lists them all.

## Evaluation

`tools/import_lab.py` reproduces the plugin path exactly (cached whisper.cpp
words, the exact 16 kHz input) and scores against references:
`gt.json` (Jonathan's intended MIDI for life/sleep) and `gt-verified.json`
(independently audited note-per-syllable annotations, vowel-onset convention).
`reference/corpus/splits.json` holds out six takes: **never tune on them**.
Headline metrics (dev aggregate): octave errors, fragmented and merged
reference notes, phantom and missed notes, COnP F (onset ±100 ms, pitch
±0.5 st), lyric word accuracy. Engine synthetic tests guard behaviour the
corpus can't show (real short octave leaps, wide vibrato, weak fundamentals,
period doubling, repeated same-pitch syllables, silence, file edges, a word
with one period-doubled syllable, real octave leaps inside and between words,
short decisive syllables keeping a leap inside a word, bright weak-fundamental
voices at G4-C5, period-doubled notes at E4-C5, octave flips of a held note,
instant real octave jumps over a decaying release, and detailed mode's extra
notes for approaches, grace notes and droops that natural mode sings as one,
with vibrato, tremolo and scoops one note in both modes).

### Results at ship (2026-10-16)

Four corpora, each split into dev (tuned on) and held-out test (scored once,
after tuning stopped). The datasets are local evaluation data, never
committed (`ASSETS.md`; download and adapter steps in `tools/datasets/README.md`).

| corpus | what | dev | test | split by |
|---|---|---|---|---|
| Jonathan | his own takes (dry, stems, accompanied, voice memos), audited references | 15 | 6 | take |
| vocadito | 29 amateur singers, 7 languages, every pitch step annotated | 23 | 17 | singer |
| GTSinger | 3 trained singers, 5 techniques plus control takes, score-based notes | 168 | 66 | singer (test EN-Alto-2) |
| CSD | one trained female singer, whole children's songs | 46 of 49 | 49 of 50 | song |

Run them (dev; add `--split test` only for a final check; the default
`words.json` holds the words the shipped plugin gives the engine):

```bash
O='{"algorithm":"singing-v2"}'        # add "detail":"detailed" for Detailed notes
tools/.venv/bin/python -I tools/import_lab.py corpus --options "$O" --tag mytag
for n in vocadito gtsinger csd; do
  tools/.venv/bin/python -I tools/import_lab.py corpus reference/datasets/$n/manifest.json \
    --lab out/import-lab-$n --options "$O" --tag mytag
done
```

COnP F (onset within 100 ms and pitch within 0.5 st), shipped settings
(whisper.cpp words at its default repetition threshold), lyric word accuracy
in brackets:

| corpus | split | singing-v1 | v2 natural | v2 detailed | human |
|---|---|---|---|---|---|
| Jonathan | dev | 0.516 (0.631) | **0.818** (0.908) | 0.786 (0.879) | |
| | test | 0.626 (0.691) | 0.726 (0.882) | **0.742** (0.855) | |
| vocadito | dev | **0.744** | 0.695 | 0.742 | 0.784 |
| | test | **0.670** | 0.617 | 0.654 | 0.713 |
| GTSinger | dev | 0.604 (0.646) | 0.663 (0.812) | **0.667** (0.781) | ~0.9 |
| | test | 0.652 (0.692) | 0.702 (0.846) | **0.740** (0.836) | |
| CSD | dev | 0.734 (0.692) | **0.893** (0.872) | 0.821 (0.822) | |
| | test | 0.718 (0.628) | **0.879** (0.843) | 0.791 (0.776) | |

("human" is vocadito's second annotator scored against the first; for
GTSinger it is the ~0.9 annotation ceiling estimated in
`tools/datasets/README.md`.) Tags in the labs: `d-*`/`t-*`
(v1, v2g natural) and `f-gd-dev`/`f-gd-test` (detailed).

What held and what didn't:

- **The complaint is fixed on held-out takes.** On Jonathan's 6 test takes,
  octave jumps inside a word go 37 -> 1, frame octave jumps 38 -> 0, splits
  beyond a word's syllables 30 -> 4 and notes per syllable 1.25 -> 1.06.
- **GTSinger and CSD generalise.** v2 beats v1 by about as much on test as
  on dev (GTSinger +0.050 vs +0.059, CSD +0.161 vs +0.159).
- **Natural's margin on Jonathan shrank:** +0.302 over v1 on dev, +0.100 on
  test, and Detailed beat Natural on the test takes (0.742 vs 0.726). Six
  takes is too few to call, but natural's absorb rules may be partly fitted
  to the dev takes. Two test takes don't beat v1: the falsetto take (natural
  swallows a five-step vocalise at 17-19 s into one note) and a voice memo
  (syllables drift onto short notes in breaths).
- **vocadito stays below v1** (dev -0.049, test -0.053), mostly merged steps
  its annotators mark separately. Detailed closes most of the gap (test
  -0.016, 92% of the human ceiling against v1's 94%).
- **Accompanied audio:** on acc-nostalgia the v2 tracker follows the backing
  and puts whole notes an octave or a fifth off (8 errors of 2 st or more,
  against 1 for v1).
- **Detailed** helps where references mark every pitch (vocadito, GTSinger)
  and costs CSD (-0.07 to -0.09), whose references are a score's notes, so it
  stays opt-in.

Whisper's repetition threshold (`entropy_thold`): 1.5 instead of the default
2.4 kept a sung "la" line (stem-when-i-die, x14) and raised Jonathan's dev
lyric word accuracy 0.908 -> 0.942. On test it changed one Jonathan take,
no vocadito or GTSinger take, and 11 of 49 CSD songs, mostly for the worse:
long phrase loops (en002b "come to the earth" x44, en048b x18) and en020a's
counting song read as "1 liter, 2 liter ..." x6 (lyric words 0.92 -> 0.23);
CSD test F 0.879 -> 0.873, lyric words 0.843 -> 0.829. The plugin keeps 2.4.
The tuning notes above quote dev scores with 1.5 (`words-entropy=1.5.json`):
natural Jonathan 0.824 (lyric 0.942), vocadito 0.696, GTSinger 0.663, CSD
0.894 (44 cases).
