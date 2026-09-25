// Phoneme inventory (ARPAbet, as used by the CMU Pronouncing Dictionary) and
// the acoustic targets the formant synthesizer steers toward for each one.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace nvs {

// Indices into a Params vector. Everything the synthesizer interpolates
// between phoneme targets lives here so transitions are one loop.
enum P : int {
  // Cascade (vocal tract) formant frequencies and bandwidths, Hz.
  F1, F2, F3, F4, F5,
  B1, B2, B3, B4, B5,
  FNZ,  // nasal antiresonance; equals the fixed nasal pole (270 Hz) when not nasal
  // Source amplitudes, 0..1.
  AV,   // voicing
  AH,   // aspiration (breath noise through the vocal tract)
  AF,   // frication (noise through the parallel peak bank)
  AB,   // flat (bypass) frication, for F/TH/P-like noise
  // Parallel frication peaks: frequency, bandwidth, relative gain.
  N1F, N1B, N1A,
  N2F, N2B, N2A,
  N3F, N3B, N3A,
  NUM_PARAMS
};

using Params = std::array<float, NUM_PARAMS>;

// Whether a parameter should use the (fast) amplitude transition time or the
// (slower) articulator transition time when moving between targets.
bool isAmplitudeParam(int index);

enum class PhonClass { Vowel, Stop, Fricative, Affricate, Nasal, Liquid, Glide, Aspirate };

struct Phoneme {
  std::string symbol;   // ARPAbet without stress digit, e.g. "AA"
  PhonClass cls;
  bool voiced;
  Params target;        // steady-state target (for stops: the closure)
  bool diphthong = false;
  Params offglide{};    // second target for diphthongs
  Params burst{};       // stops/affricates: release burst spectrum
  float duration = 0.08f;  // natural duration for consonants, seconds
};

const Phoneme* findPhoneme(const std::string& symbol);
const std::vector<Phoneme>& allPhonemes();

// Silence that keeps the given formant frequencies, so fades into and out of
// silence don't sweep the formants.
Params silenceLike(const Params& p);

}  // namespace nvs
