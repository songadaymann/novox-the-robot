// singing-v2 articulation features: where syllables begin, from band levels,
// voicing and spectral change (owned by the segmentation track, see
// docs/SINGING-V2-DESIGN.md).
//
// Legato syllable changes rarely move the overall level much: an /m/ or /n/
// keeps the low band and drops the vowel band (640-2800 Hz) by 5-20 dB, an
// /s/ or /t/ breaks voicing and adds or removes high-band energy, and a vowel
// change only shows as a change of spectral shape. So each candidate boundary
// carries separate evidence for each kind of onset consonant, and the segmenter
// asks for the kind the lyrics expect there.
#pragma once

#include <cstddef>
#include <vector>

#include "singing_pitch.hpp"

namespace novox {

// Per-frame features on the shared analysis timeline (frame k at k * 8 ms).
struct ArticulationFeatures {
  size_t frames = 0;
  std::vector<float> low, mid, high, total;  // band levels in dB (80-400, 640-2800, 3500-7800, 80-7800 Hz), lightly smoothed
  std::vector<float> change;                 // spectral (MFCC) change across +-48 ms, in z units of the take
  std::vector<char> voiced;                  // the pitch path is voiced and the frame is above the noise floor
  std::vector<char> weak;                    // loud, vowel-like, periodic frames the path leaves unvoiced (runs >= 40 ms)
  std::vector<float> vibrato;                // 0..1: the pitch oscillates at 4-10 Hz around here (periodicity of the detrended contour)
  float loudDb = -120;                       // level of loud singing over the take (high percentile of `total` over voiced frames)
  std::vector<float> loud;                   // per frame: level of loud singing in its phrase, nearby (the reference of
                                             // level decisions, so quiet sections keep their notes; see articulation.cpp)
  std::vector<float> nearbyLoud;             // per frame: level of loud singing nearby, any phrase (what sound is negligible next to)
  float floorDb = -120;                      // background level (low percentile of `total`)
};

ArticulationFeatures analyzeArticulation(const std::vector<float>& x16k, const PitchAnalysis& pitch);

// The consonant class that separates two sung vowels, strongest first.
enum class BoundaryKind { Unvoiced = 0, Voiced = 1, Sonorant = 2, Vowel = 3 };

// A place where a new syllable's vowel may begin, with the probability that a
// syllable of each onset kind starts here (0..1, calibrated on reference
// annotations of real takes).
struct BoundaryCandidate {
  size_t frame = 0;      // vowel onset (note start)
  size_t soundEnd = 0;   // where the previous sound ends: start of the voicing break before `frame`, else `frame`
  // Measurements the evidence is computed from.
  bool voicingOnset = false;  // proposed by a voicing onset (else by a spectral-change peak)
  float gap = 0;         // seconds of the voicing break nearest the onset (voicing may start up to ~100 ms late)
  float breakBefore = 0; // seconds of voicing break ending right at the onset
  float unvoicedAfter = 0;  // share of unvoiced frames in the 48 ms after the onset
  float frication = 0;   // dB the (high - vowel band) balance in the break exceeds the following vowel's
  float dipMid = 0;      // dB depth of the vowel-band dip before `frame`
  float dipHigh = 0;     // dB depth of the high-band dip before `frame`
  float change = 0;      // spectral change peak near `frame`
  float rise = 0;        // dB the vowel band rises over 16 ms at the onset
  float pitchStep = 0;   // settled pitch change across `frame`, semitones (octave-folded)
  float level = 0;       // dB of the following vowel relative to loud singing
  float evidence[4] = {0, 0, 0, 0};  // P(syllable onset of each BoundaryKind)
  float any = 0;         // P(any syllable onset)
};

// Candidate vowel onsets of the whole take, in time order: spectral-change
// peaks and voicing onsets (moved back to where the vowel band rises when the
// pitch tracker starts late), with their evidence.
std::vector<BoundaryCandidate> findBoundaryCandidates(const ArticulationFeatures& f, const PitchAnalysis& pitch);

// Where the vowel of a sound starting (voicing) at `frame` begins: the steepest
// vowel-band rise within the preceding 120 ms, or `frame`.
size_t vowelOnsetBefore(const ArticulationFeatures& f, size_t frame);

}  // namespace novox
