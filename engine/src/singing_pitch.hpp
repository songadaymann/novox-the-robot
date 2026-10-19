// singing-v2 pitch evidence: per-frame pitch candidates with periodicity and
// harmonic (spectral) support, a decoded pitch path, and note-level octave and
// pitch-centre decisions made from all frames of a note at once.
//
// Ownership: this module answers "what pitch is sung here?". It never decides
// where notes begin or end; singing_segment.* does that and asks this module
// for contours and note pitches.
#pragma once

#include <cstddef>
#include <vector>

#include "spectrum.hpp"
#include "transcribe.hpp"

namespace novox {

struct PitchCandidateV2 {
  float midi = 0;       // fractional MIDI pitch
  double lag = 0;       // period in 16 kHz samples
  bool lowBandLag = false;  // the low band supplied the period; refinement must use the same band
  float cmnd = 1;       // YIN cumulative-mean-normalised difference at this lag (0 = perfectly periodic):
                        // the better of the full band and the ~1.3 kHz low band
  float harmonic = 0;   // spectral support for this octave reading: 0 = consistent with the spectrum,
                        // down to -2.5 when the partials say f/2, 2f, f/3 or 3f (see singing_pitch.cpp)
  float fundamental = 0;  // level of the partial at f over the partial at 2f, dB (H1-H2 of this reading)
  float emission = 0;   // combined per-frame cost used by the path decoder (lower = better; periodicity,
                        // harmonic evidence, low-register floor and local-register prior); after
                        // decoding, the upper reading of an undone octave flip is raised to the
                        // lower reading's (see analyzeSingingPitch)
};

struct PitchAnalysis {
  // Decoded path on the shared analysis timeline (frame k at k * 8 ms). midi/f0 are 0 when unvoiced.
  // frames[k].confidence is the path's per-frame reliability (0..1): clarity, lowered when an
  // octave-related alternative is nearly as good or the frame is polyphonic. clarity = 1 - cmnd,
  // db = level of the 32 ms window (dBFS), endTime = frame time + 4 ms clipped to the recording.
  std::vector<PitchFrame> frames;
  // All candidates considered per frame (same indexing as frames), for note-level decisions.
  std::vector<std::vector<PitchCandidateV2>> candidates;
  // Index into candidates[k] of the decoded choice, or -1 when unvoiced.
  std::vector<int> chosen;
  double duration = 0;  // seconds of audio analysed
  float floorDb = -80;  // take-wide silence level (50 dB under the loud frames); a quieter section's
                        // floor follows its own loud frames, down to 30 dB under this
  // Fraction of voiced frames whose spectrum shows more than one strong harmonic
  // series (accompaniment bleed / polyphony); 0 for clean solo voice.
  float polyphony = 0;
  // Per frame: the spectrum shows more than one strong harmonic series (the
  // frames `polyphony` counts, plus quiet ones).
  std::vector<bool> polyphonic;
};

// Analyses 16 kHz mono audio (see resampleTo16k). Deterministic; memory linear
// in the recording length. A one-hop octave
// jump of the decoded path across which the lower octave's fundamental partial
// keeps its level (a weak-fundamental voice read as its 2nd harmonic) is undone:
// the frames on its upper side take the lower reading.
// refinePeriods=false retains the original YIN interpolation for diagnostic A/B tests.
// Refinement adjusts reliable selected periods of at most 40 samples (roughly 400 Hz and up)
// within their existing pitch neighbourhood;
// candidate scores, frame confidence, octave decoding and voicing retain the original evidence.
// Later note/contour interpretation uses the refined pitches.
PitchAnalysis analyzeSingingPitch(const std::vector<float>& x16k, bool refinePeriods = true);

// Per-frame MIDI contour for frames [first, last] (inclusive) with octave errors
// relabelled to a supported candidate (or dropped to 0 when none exists). When the
// span holds one register, its octave is decided from all frames' evidence (as in
// assignNotePitches); when it holds a real leap (two registers each >= 300 ms),
// each keeps its octave and only shorter excursions are relabelled. Unvoiced
// frames are 0. Used for melisma / scoop / vibrato analysis.
std::vector<float> consistentContour(const PitchAnalysis& analysis, size_t first, size_t last);

// The centre line of a contour (MIDI, 0 = unvoiced): inside each run of whole
// vibrato cycles (3.5-9 Hz turning points, the cycles robustCentre finds) every
// voiced frame becomes the mean over one cycle around it, so a held note sung
// with vibrato is flat and a step between notes still moves the line; frames
// outside vibrato and unvoiced frames are returned unchanged.
std::vector<float> vibratoCentreLine(const std::vector<float>& contour);

// A proposed note (frame range, inclusive) whose pitch this module decides.
struct NoteSpan {
  size_t first = 0, last = 0;  // analysis frames, inclusive
  long long word = -1;         // in: notes with the same word >= 0 are one sung word (its syllables and
                               // melisma notes, or one hum's melisma): they share a register; -1: none
  float center = 0;            // out: perceived pitch centre, fractional MIDI (0 if no voiced evidence):
                               // a mode-seeking mean (scoops, fall-offs and overshoots excluded), on the
                               // one-cycle average of the contour when it carries vibrato
  float spread = 0;            // out: P90-P10 of the contour residual after removing drift (semitones)
  float confidence = 0;        // out: 0..1 (frame confidence x voiced share x octave decisiveness)
  int octaveShift = 0;         // out: semitones moved from the raw decoded path (multiple of 12), for diagnostics
};

// Decides each note's octave and pitch centre from all of its frames, with a
// continuity prior between neighbouring notes in a phrase (an octave leap
// between adjacent notes needs clear evidence; one inside a note almost never happens).
// The notes of one word (NoteSpan::word) are decided together: the word sits in
// the register all its frames' evidence favours, and a leap of 9 semitones or
// more between two of its notes survives only when each side holds its octave
// firmly (its octave margin per frame of evidence, over at least 40 ms of
// evidence, is near the most a frame gives); a note of a word the path never voices takes the
// octave of the word's nearest voiced note.
// Frames read an octave under the modal frames of the note, or of the other notes
// of its word, with a far weaker fundamental (period doubling) carry no octave
// evidence and count an octave above it; in an accompanied take (polyphony >=
// 0.35) neither do polyphonic frames.
// Notes may be given in any order; notes within 0.3 s of each other form a phrase.
// A note the path never voices gets the centre of its frames' best periodic
// candidates (confidence <= 0.3), or 0 when there are none.
void assignNotePitches(const PitchAnalysis& analysis, std::vector<NoteSpan>& notes);

// The take's pitch reference: how far, in semitones (-0.5..0.5), the singer's
// semitone grid sits from A440, from the notes' pitch centres (fractional MIDI)
// and their weights (seconds held x pitch confidence; 0 = ignore). Notes are
// rounded to that grid (as Melodyne does with its detected pitch reference),
// so a singer tuned 40 cents flat keeps the notes they meant. `offset` is 0
// when the take is in tune (within 25 cents), shows no common offset (notes
// scattered around the grid), or has accompaniment (`polyphony`, see
// PitchAnalysis), whose A440 tuning the notes are heard against.
struct TuningEstimate {
  float offset = 0;     // semitones to round against (0 = A440)
  float measured = 0;   // semitones, the robust circular mean before the gates
  float strength = 0;   // Rayleigh statistic n_eff * R^2 of the plain circular mean
};
TuningEstimate estimateTuning(const std::vector<float>& centres, const std::vector<float>& weights,
                              float polyphony = 0);

}  // namespace novox
