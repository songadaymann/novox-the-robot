// singing-v2 pitch analysis (docs/SINGING-V2-DESIGN.md, track A).
//
//   candidates  symmetric YIN on the full band and on a ~1.3 kHz low band. Rough,
//               pressed phonation repeats almost exactly at twice the period above
//               ~1.3 kHz, so the full-band dip at the true period T can be shallow,
//               split or missing while the low band stays periodic at T. Local
//               minima of both, plus the sub-multiples L/2, L/3, L/4 of every strong
//               minimum, so the true period is always a candidate.
//   evidence    per candidate, the linear amplitude of its partials against the
//               positions between them (half and third harmonics, Sun's
//               subharmonic-to-harmonic ratio) in a 64 ms spectrum weighted to the
//               resolved low band (to 1-1.8 kHz, or the lower pitch's first 5.5-10
//               harmonics when its fundamental is present, for high voices). It
//               ranks octave-related candidates (period
//               doubling, weak fundamentals, strong 3rd harmonics) and never decides
//               voicing, which stays with full-band periodicity as in singing-v1.
//   path        Viterbi with a speed limit (vibrato and glides cheap, a one-frame
//               octave about 6) and pitch memory across short unvoiced gaps, so
//               voiced -> unvoiced -> voiced cannot flip octave for free; a second
//               pass adds a soft prior toward the singer's local register.
//   flips       one-hop octave jumps of the path across which the lower octave's
//               fundamental persists are undone (a weak-fundamental voice read as
//               its 2nd harmonic).
//   notes       octave hypotheses scored over all frames of a note, a continuity DP
//               across neighbouring notes in which the notes of one word share a
//               register, and a mode-seeking pitch centre. Frames that cannot tell
//               the octave do not vote: subharmonic readings under the modal frames
//               of the note or of the rest of its word and, in accompanied takes,
//               frames with a second harmonic series.
//
// Constants were calibrated on the dev split of tools/import_lab.py (verified
// note-per-syllable references) and checked against tests/singing_pitch_tests.cpp.
#include "singing_pitch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace novox {

namespace {

constexpr double kRate = kAnalysisRate;
// Tuning reference (estimateTuning). Dev takes of both corpora: amateur and
// natural singing scatters +-25-50 cents from note to note, so most takes show
// no common offset at all (Rayleigh statistic < 3).
constexpr int kTuningIterations = 3;      // mode-seeking passes
constexpr double kTuningWindow = 0.35;    // st: notes further from the offset (Tukey weight 0) do not move it
// A wrong reference renames every note near a grid line, so the offset must be
// clearly there: Rayleigh n_eff R^2 >= 4.6 is p < 0.01 for phases with no
// preferred offset (dev: 4 of 38 takes reach 3 more than 15 cents off A440,
// one of them accompanied; 1 reaches 4.6).
constexpr float kTuningMinStrength = 4.6f;
// Short phrases fake an offset: a dozen notes sung 25-60 cents off on both
// sides fold into one cluster near +-50 cents (dev: all 3 takes the reference
// fired on had 10-21 effective notes, and all got worse), so it needs this many.
constexpr double kTuningMinNotes = 25;
// Near a quarter tone either grid is as good a guess, and A440 is the one the
// rest of the session uses: beyond this the take keeps A440.
constexpr double kTuningAmbiguous = 0.40;
// Within 25 cents of A440 a take keeps the A440 grid: in a DAW the voice is
// usually sung against instruments tuned there, and moving every note's name
// for a smaller offset trades known pitches for a guess.
constexpr double kTuningDeadZone = 0.25;
// With accompaniment audible (a guitar bleeds in at 0.30, solo voice <= 0.21)
// the notes are heard against the band, tuned to A440 almost always; the
// voice's own offset there is intonation (dev: the one accompanied take that
// shows one, -25 cents, has its notes named by key at A440).
constexpr float kTuningMaxPolyphony = 0.25f;
constexpr int kWindow = 512;                 // YIN integration window, 32 ms
// Up to C7: whistling and soprano / whistle-register notes above ~C6 would
// otherwise only have their sub-multiples as candidates (heard 12 or 19
// semitones low).
constexpr double kMinHz = 60, kMaxHz = 2100;
constexpr int kTauMin = 7;                   // floor(16000 / 2100)
constexpr int kTauMax = 267;                 // ceil(16000 / 60)
constexpr float kCandidateCmnd = 0.5f;       // a dip above this is not a period
constexpr float kStrongCmnd = 0.3f;          // dips whose sub-multiples are added
constexpr float kSubmultipleCmnd = 0.7f;     // a derived sub-multiple worse than this is no period (noise ~1)
constexpr size_t kMaxCandidates = 12;
constexpr int kFft = 1024;                   // harmonic evidence: 64 ms, 15.6 Hz bins
// Silence: frames 50 dB under the loud frames around them, or below 16-bit
// resolution (an absolute floor low enough that any audible take is analysed
// the same at any gain), are not decoded.
constexpr float kSilenceDb = -110;    // dBFS
constexpr size_t kQuietHalf = 312;    // frames (2.5 s) each side
constexpr float kQuietRange = 30;     // dB: a quiet section's floor stays within this of the take's
// Tonal breaths (dropTonalBreaths). An inhale through a narrowed nose or mouth
// can whistle: a lone 1.5-2 kHz tone in noise, periodic enough to decode as
// voice (often as its sub-multiple). A dry take keeps it 50-65 dB under the
// singing, below the silence floor, but a vocal chain's compression lifts it to
// singing level (a bounced take: inhales +49 dB, vowels +5 dB), where it became
// a note 2-3 octaves over the melody, often taking the next word's lyric.
// Voice under 1 kHz always has a partial there (H1, H2 or F1): of ~1M voiced
// frames (vocadito, CSD, GTSinger and the Novox takes) none has its strongest
// bin under 1 kHz 20 dB under its strongest to 2.5 kHz; the whistles 18-51 dB
// (median 33). A tone that high must also stand far over the singer to go: a
// real note over 1 kHz is a high voice's top, under two octaves over its median
// (the whistles stand 33-36 semitones over that take's median). And inhales
// are brief: that take's add up to 1.1 s, 1.4% of its voiced frames. More than
// this of lone high tones is a whistled passage (clean whistles cannot be told
// by periodicity: one inhale's whistle reached 0.98), which keeps them all.
constexpr float kToneOnlyGap = 20;          // dB: nothing under 1 kHz within this of the top partial
constexpr float kBreathOverMedian = 24;     // semitones over the take's median voiced reading
constexpr size_t kBreathMinVoice = 125;     // other voiced frames (1 s) needed for that median
constexpr float kBreathSameTone = 1.5f;     // semitones: a run's frames peaking this near its whistle's tone
constexpr size_t kBreathMaxFrames = 500;    // 4 s of tonal breaths in all ...
constexpr double kBreathMaxShare = 0.05;    // ... and 5% of the voiced frames
constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();

float midiOf(double hz) { return static_cast<float>(69 + 12 * std::log2(hz / 440.0)); }

// ---------------------------------------------------------------------------
// Periodicity

// Low band: windowed-sinc low-pass, flat to ~1.0 kHz and > 60 dB down from ~1.6
// kHz (161-tap Blackman), so period-doubled upper partials stay out. Kept at 16
// kHz: CMND dips of the upper low-band partials are too narrow for an 8 kHz grid.
std::vector<float> lowBand(const std::vector<float>& x) {
  constexpr int half = 80;
  constexpr double cutoff = 1300 / kRate;  // cycles per sample (-6 dB point)
  constexpr int length = 2 * half + 1, padded = 168;  // taps padded to a multiple of 8
  std::vector<float> taps(padded, 0.0f);
  double sum = 0;
  for (int k = -half; k <= half; ++k) {
    const double s = k == 0 ? 2 * cutoff : std::sin(2 * kPi * cutoff * k) / (kPi * k);
    const double w = 0.42 + 0.5 * std::cos(kPi * k / half) + 0.08 * std::cos(2 * kPi * k / half);  // Blackman
    taps[static_cast<size_t>(k + half)] = static_cast<float>(s * w);
    sum += s * w;
  }
  for (int k = 0; k < length; ++k) taps[static_cast<size_t>(k)] = static_cast<float>(taps[static_cast<size_t>(k)] / sum);
  // Zero-padded copy so every output is the same branch-free dot product.
  std::vector<float> in(x.size() + 2 * padded, 0.0f);
  std::copy(x.begin(), x.end(), in.begin() + half);
  std::vector<float> y(x.size());
  for (size_t i = 0; i < y.size(); ++i) {
    const float* at = in.data() + i;
    float partial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int k = 0; k < padded; k += 8)
      for (int u = 0; u < 8; ++u) partial[u] += taps[static_cast<size_t>(k + u)] * at[k + u];
    y[i] = ((partial[0] + partial[1]) + (partial[2] + partial[3])) + ((partial[4] + partial[5]) + (partial[6] + partial[7]));
  }
  return y;
}

// Symmetric YIN cumulative-mean-normalised difference for lags 0..maxLag+1. Both
// windows of each lag pair are centred on `center` (within half a sample), so the
// period describes the audio around the frame time; zero padding at the edges.
// `window` must be a multiple of 8.
void symmetricCmnd(const std::vector<float>& x, long long center, int window, int maxLag,
                   std::vector<float>& local, std::vector<float>& cmnd) {
  const int support = window / 2 + (maxLag + 2) / 2 + 1;
  local.resize(static_cast<size_t>(2 * support + 1));
  for (int j = -support; j <= support; ++j) {
    const long long at = center + j;
    local[static_cast<size_t>(j + support)] = at >= 0 && at < static_cast<long long>(x.size()) ? x[static_cast<size_t>(at)] : 0.0f;
  }
  cmnd.assign(static_cast<size_t>(maxLag + 2), 1.0f);
  double running = 0;
  for (int tau = 1; tau <= maxLag + 1; ++tau) {
    const float* left = local.data() + (support - window / 2 - tau / 2);
    const float* right = left + tau;
    // Eight independent partial sums let the compiler vectorise the reduction.
    float partial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int j = 0; j < window; j += 8)
      for (int u = 0; u < 8; ++u) {
        const float delta = left[j + u] - right[j + u];
        partial[u] += delta * delta;
      }
    const double d = (static_cast<double>(partial[0]) + partial[1] + partial[2] + partial[3]) +
                     (static_cast<double>(partial[4]) + partial[5] + partial[6] + partial[7]);
    running += d;
    cmnd[static_cast<size_t>(tau)] = running > 1e-20 ? static_cast<float>(d * tau / running) : 1.0f;
  }
}

double frameDb(const std::vector<float>& x, long long center) {
  double energy = 0;
  for (int j = -kWindow / 2; j < kWindow / 2; ++j) {
    const long long at = center + j;
    if (at >= 0 && at < static_cast<long long>(x.size())) energy += static_cast<double>(x[static_cast<size_t>(at)]) * x[static_cast<size_t>(at)];
  }
  return 10 * std::log10(energy / kWindow + 1e-12);
}

// ---------------------------------------------------------------------------
// Harmonic evidence

// One frame's spectrum below 3 kHz and its noise floor.
struct FrameSpectrum {
  std::vector<float> mag;  // linear magnitude per bin
  std::vector<float> db;   // the same in dB
  float floor = -120;      // dB, 20th percentile of the 50 Hz - 3 kHz bins
  float floorMag = 0;
  float top = -200;        // strongest partial 60 Hz - 2.5 kHz, dB
  double binHz = 0;

  // Largest bin within +-tol Hz of hz; the top bin for a position above the
  // spectrum (the 3rd-harmonic test of a high candidate asks for (k - 1/3) f up
  // to 3.3 kHz: weighted 0 there, but it must not read past the bins).
  int peakBin(double hz, double tol) const {
    const int hi = std::min(static_cast<int>(mag.size()) - 1, static_cast<int>(std::ceil((hz + tol) / binHz)));
    const int lo = std::min(hi, std::max(1, static_cast<int>(std::floor((hz - tol) / binHz))));
    int best = lo;
    for (int i = lo; i <= hi; ++i)
      if (mag[static_cast<size_t>(i)] > mag[static_cast<size_t>(best)]) best = i;
    return best;
  }
  // Partial amplitude near hz above the noise floor (linear, >= 0).
  float amplitude(double hz, double tol) const { return std::max(0.0f, mag[static_cast<size_t>(peakBin(hz, tol))] - floorMag); }
  // Peak level near hz in dB above the floor (0..60).
  float prominence(double hz, double tol) const { return std::clamp(db[static_cast<size_t>(peakBin(hz, tol))] - floor, 0.0f, 60.0f); }
};

// One frame's spectrum (as computeSpectra, one frame at a time so a long take
// never holds all of them) and its floor and strongest partial.
class FrameSpectrumAnalyzer {
 public:
  explicit FrameSpectrumAnalyzer(int size) : spectrum_(size), magnitude_(static_cast<size_t>(size / 2 + 1)) {}
  void analyze(const std::vector<float>& x, long long center, FrameSpectrum& s) {
    spectrum_.analyze(x, center, magnitude_.data());
    s.binHz = kRate / spectrum_.size();
    const int bins = static_cast<int>(3000 / s.binHz) + 2;
    s.mag.assign(magnitude_.begin(), magnitude_.begin() + bins);
    s.db.resize(static_cast<size_t>(bins));
    for (int i = 0; i < bins; ++i) s.db[static_cast<size_t>(i)] = static_cast<float>(20 * std::log10(s.mag[static_cast<size_t>(i)] + 1e-9));
    const int first = static_cast<int>(std::ceil(50 / s.binHz));
    sorted_.assign(s.db.begin() + first, s.db.end());
    std::nth_element(sorted_.begin(), sorted_.begin() + static_cast<long>(sorted_.size() / 5), sorted_.end());
    s.floor = sorted_[sorted_.size() / 5];
    s.floorMag = std::pow(10.0f, s.floor / 20);
    s.top = -200;
    for (int i = static_cast<int>(60 / s.binHz); i <= static_cast<int>(2500 / s.binHz); ++i) s.top = std::max(s.top, s.db[static_cast<size_t>(i)]);
  }

 private:
  WindowedSpectrum spectrum_;
  std::vector<float> magnitude_, sorted_;
};

// Weights of spectral positions in the octave evidence between a pitch f and
// the lower pitch f/m it might be a harmonic of. Pitch is heard from resolved
// low harmonics, and period-doubled voices show half-harmonics close to 0 dB
// above ~1.3 kHz: full weight to 1 kHz, none from 1.8 kHz, the 5.5th and 10th
// harmonics of a 185 Hz lower pitch, about where the band was calibrated (a
// period-doubled mid voice). A higher lower pitch would keep only its first
// few harmonics there (a G4-B4 voice: H1 and H2 under 1 kHz), and a weak H1
// under a strong H2 alone reads as the octave up, so the band follows the
// lower pitch (its first 5.5 harmonics in full, none from its 10th) when that
// pitch's own fundamental is present. A fundamental 25 dB or more under the
// partial at f is absent (a sung H1-H2 stays within about +-20 dB): such a lower
// pitch rests on half-partials alone, which period doubling supplies high in
// the spectrum (a dev period-doubled 383 Hz note: f/2 33 dB under f, its
// half-partials 8-12 dB above both neighbours at 2.1-2.5 kHz), and it keeps
// the fixed band.
struct EvidenceBand {
  double full = 1000, zero = 1800;  // Hz: full weight up to `full`, none from `zero`
};
constexpr double kBandFullHarmonics = 5.5, kBandZeroHarmonics = 10;
constexpr double kFundamentalPresentDb = 25;
constexpr double kSpectrumTopHz = 2900;  // positions (with their search tolerance) inside FrameSpectrum's 3 kHz

// Level (dB) of the strongest bin within 2.5% of hz.
double partialLevel(const FrameSpectrum& s, double hz) {
  return s.db[static_cast<size_t>(s.peakBin(hz, std::max(0.5 * s.binHz, 0.025 * hz)))];
}

EvidenceBand evidenceBand(const FrameSpectrum& s, double f, int m) {
  EvidenceBand band;
  const double lower = f / m;
  if (m != 2 || f >= kSpectrumTopHz || partialLevel(s, lower) < partialLevel(s, f) - kFundamentalPresentDb) return band;
  band.full = std::min(kSpectrumTopHz, std::max(band.full, kBandFullHarmonics * lower));
  band.zero = std::min(kSpectrumTopHz, std::max(band.zero, kBandZeroHarmonics * lower));
  return band;
}

double bandWeight(double hz, const EvidenceBand& band) {
  if (hz < 60 || hz >= band.zero) return 0;
  if (hz <= band.full) return 1;
  return 1 - (hz - band.full) / (band.zero - band.full);
}

// Subharmonic-to-harmonic contrast of pitch f in dB (Sun's SHR, inverted): the
// weighted linear amplitude of the partials k*f over that of the positions between
// them that a pitch f/m would add ((k - 1/2) f for m = 2; (k - 1/3) f and (k - 2/3) f
// for m = 3). Linear sums follow the dominant partials, as perception does. Large
// means those positions are empty (pitch f, f/m would be a subharmonic); near 0 dB
// means f is itself a harmonic of f/m. `reliability` is 0..1 by the partials counted.
double subharmonicContrast(const FrameSpectrum& s, double f, int m, double* reliability) {
  const double tolerance = std::max(0.5 * s.binHz, std::min(0.025 * f, f / 6));
  double harmonics = 0, between = 0, weight = 0;
  const EvidenceBand band = evidenceBand(s, f, m);
  for (int k = 1; (k - 0.5) * f < 2500; ++k) {
    const double hz = k * f;
    const double w = bandWeight(hz, band);
    if (w > 0) {
      harmonics += w * s.amplitude(hz, std::min(tolerance * k, f / 6));
      weight += w;
    }
    if (m == 2) {
      const double half = (k - 0.5) * f;
      between += bandWeight(half, band) * s.amplitude(half, std::min(tolerance * k, f / 6));
    } else {
      for (double o : {1.0 / 3, 2.0 / 3}) {
        const double third = (k - o) * f;
        between += 0.5 * bandWeight(third, band) * s.amplitude(third, std::min(tolerance * k, f / 8));
      }
    }
  }
  if (reliability) *reliability = std::min(1.0, weight / 2.0);
  const double floor = 0.05 * s.floorMag * std::max(1.0, weight);
  return 20 * std::log10((harmonics + floor) / (between + floor));
}

// Contrast at which f and f/2 are equally likely. On the dev references the share
// of octave pairs whose upper reading is right crosses 50% at 10-11 dB and the
// log-odds grow ~0.3 per dB around it; frames are correlated, so the cost uses less.
constexpr double kContrastTheta = 10;
constexpr double kEvidencePerDb = 0.12;
constexpr double kEvidenceCap = 2.5;
constexpr double kMissingDb = 25;  // first three harmonics this far under the strongest partial = not a voice

// Share (0..1, low-band weighted) of the half positions (k - 1/2) f, k >= 2, whose
// partial stands above one of its neighbours k f and (k - 1) f. Subharmonics of
// period doubling are modulation products that sit under both neighbours; the
// odd harmonics of a real pitch f/2 follow the vowel's formants and often rise
// above an even neighbour (a weak fundamental with a dominant 2nd harmonic).
double interleave(const FrameSpectrum& s, double f) {
  double above = 0, total = 0;
  const double tolerance = std::max(0.5 * s.binHz, std::min(0.025 * f, f / 6));
  auto level = [&](double k) { return s.prominence(k * f, std::min(tolerance * k, f / 6)); };
  const EvidenceBand band = evidenceBand(s, f, 2);
  const double limit = std::min(band.zero, 2500.0);  // half positions, as subharmonicContrast's
  for (int k = 2; (k - 0.5) * f < limit && k * f < kSpectrumTopHz; ++k) {  // the upper neighbour inside the spectrum
    const double w = bandWeight((k - 0.5) * f, band);
    if (w <= 0) continue;
    total += w;
    if (level(k - 0.5) > std::min(level(k), level(k - 1)) + 3) above += w;
  }
  return total > 0 ? above / total : 0;
}

// Octave-evidence cost of hearing pitch f (0 = consistent with the spectrum): f
// is too high when it is a harmonic of f/2 or f/3, too low when 2f or 3f has
// nothing between its partials (unless f's odd harmonics behave like real ones).
double harmonicPenalty(const FrameSpectrum& s, double f) {
  double r1 = 0, r2 = 0, r3 = 0, r4 = 0;
  const double tooHigh2 = subharmonicContrast(s, f, 2, &r1);
  const double tooLow2 = subharmonicContrast(s, 2 * f, 2, &r2);
  const double tooHigh3 = subharmonicContrast(s, f, 3, &r3);
  const double tooLow3 = subharmonicContrast(s, 3 * f, 3, &r4);
  const double odd = std::clamp((interleave(s, 2 * f) - 0.25) / 0.5, 0.0, 1.0);
  double penalty = r1 * std::max(0.0, kContrastTheta - tooHigh2) + (1 - odd) * r2 * std::max(0.0, tooLow2 - kContrastTheta) +
                   0.7 * (r3 * std::max(0.0, kContrastTheta - tooHigh3) + r4 * std::max(0.0, tooLow3 - kContrastTheta));
  // A voice has partials among its first three harmonics. A lag that only lines
  // up with a strong higher partial (e.g. 2.5 periods when H2 dominates) does not.
  float lowest = -200;
  for (int k = 1; k <= 3 && k * f < 2500; ++k)
    lowest = std::max(lowest, s.db[static_cast<size_t>(s.peakBin(k * f, std::max(0.5 * s.binHz, 0.025 * k * f)))]);
  penalty += std::max(0.0, std::min(static_cast<double>(s.top - std::max(lowest, s.floor)), 60.0) - kMissingDb);
  return std::min(kEvidenceCap, kEvidencePerDb * penalty);
}

// ---------------------------------------------------------------------------
// Polyphony: does a strong spectral peak lie outside the best single-voice
// harmonic series? A sung tone has a (nearly) complete series below 1.2 kHz, so a
// chord's common subharmonic (a triad is harmonics 4:5:6 of a pitch two octaves
// down, with gaps) is not accepted as one voice. Half-partials of period doubling
// are not a second voice.
bool polyphonicFrame(const FrameSpectrum& s, const std::vector<PitchCandidateV2>& candidates) {
  struct Peak { double hz; float db; };
  std::vector<Peak> peaks;
  const int lo = static_cast<int>(std::ceil(70 / s.binHz)), hi = std::min(static_cast<int>(2000 / s.binHz), static_cast<int>(s.db.size()) - 2);
  float top = -200;
  for (int i = lo; i <= hi; ++i) top = std::max(top, s.db[static_cast<size_t>(i)]);
  if (top < s.floor + 25) return false;
  for (int i = lo; i <= hi; ++i) {
    const float v = s.db[static_cast<size_t>(i)];
    if (v < s.db[static_cast<size_t>(i - 1)] || v < s.db[static_cast<size_t>(i + 1)]) continue;
    if (v > top - 30 && v > s.floor + 15) peaks.push_back({i * s.binHz, v});
  }
  if (peaks.size() < 3) return false;
  auto level = [&](double hz) {  // dB relative to the strongest peak
    const int b = static_cast<int>(std::lround(hz / s.binHz)), t = std::max(1, static_cast<int>(std::lround(0.03 * hz / s.binHz)));
    float best = -200;
    for (int i = std::max(1, b - t); i <= std::min(static_cast<int>(s.db.size()) - 1, b + t); ++i) best = std::max(best, s.db[static_cast<size_t>(i)]);
    return best;
  };
  auto harmonicOf = [&](double hz, double f) {
    const double k = std::round(hz / f);
    return k >= 1 && std::fabs(hz - k * f) <= std::max(s.binHz, 0.03 * hz);
  };
  std::vector<double> hypotheses;
  for (const auto& c : candidates)
    if (c.cmnd <= kCandidateCmnd) hypotheses.push_back(kRate / c.lag);
  for (const auto& p : peaks)
    for (int k = 1; k <= 3; ++k) hypotheses.push_back(p.hz / k);
  double bestShare = -1, bestF = 0;
  for (double f : hypotheses) {
    if (f < 70) continue;
    const int count = std::max(1, std::min(8, static_cast<int>(1200 / f)));
    int missing = 0;
    for (int k = 1; k <= count; ++k)
      if (level(k * f) < std::max(s.floor + 10, top - 40)) ++missing;
    const bool fundamental = level(f) >= std::max(s.floor + 10, top - 40) || (count > 1 && level(2 * f) >= std::max(s.floor + 10, top - 40));
    if (!fundamental || missing > std::max(1, count / 4)) continue;
    double explained = 0, total = 0;
    for (const auto& p : peaks) {
      const double power = std::pow(10.0, (p.db - top) / 10);
      total += power;
      if (harmonicOf(p.hz, f)) explained += power;
    }
    if (explained / total > bestShare) bestShare = explained / total, bestF = f;
  }
  if (bestF <= 0) return false;
  for (const auto& p : peaks) {
    if (p.db < top - 15 || harmonicOf(p.hz, bestF)) continue;
    // Period doubling fills consecutive half positions; a chord's third is isolated.
    const double r = p.hz / bestF;
    const bool half = std::fabs(r - (std::floor(r) + 0.5)) < 0.06;
    if (half && std::max(r > 1 ? level((r - 1) * bestF) : -200.0f, level((r + 1) * bestF)) > p.db - 15) continue;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Path decoding

bool octaveRelated(double d) {
  for (double k : {12.0, 19.0, 24.0})
    if (std::fabs(d - k) < 1.0) return true;
  return false;
}

// Moving `d` semitones in one 8 ms hop of continuous voicing. Vibrato (~0.3
// st/hop) and portamento stay cheap; a real 12-semitone glide takes >= ~120 ms in
// voice, so a one-hop octave (a tracker error signature) costs about 6.
double glideCost(double d) {
  double c = d <= 1 ? 0.25 * d : d <= 4 ? 0.25 + 0.6 * (d - 1) : 2.05 + 0.25 * (d - 4);
  if (octaveRelated(d)) c += 2.0;
  return c;
}

// Re-entering voicing `gap` hops after the last voiced pitch. Consonants carry
// real interval leaps of any size, so ordinary jumps cost little; an
// octave-related re-entry soon after the old pitch is still an error signature.
constexpr int kMemoryHops = 25;  // 200 ms
double reentryCost(double d, int gap) {
  double c = std::min(2.0, 0.25 * std::max(0.0, d - 1 - 0.4 * gap));
  if (octaveRelated(d)) c += gap <= 6 ? 3.0 : std::max(0.5, 3.0 - 2.5 * (gap - 6) / (kMemoryHops - 6.0));
  return c;
}

constexpr double kToggle = 0.15;
constexpr double kUnvoiced = 1.2;
// Voicing compares 6 x the best CMND with kUnvoiced, as singing-v1. Between the
// candidates of one frame CMND differences count less: at octave pairs they say
// little once the spectral contrast is known (the deeper dip is often the
// subharmonic of a period-doubled voice).
constexpr double kPeriodicityWeight = 6;
constexpr double kRelativePeriodicityWeight = 2;

struct UnvoicedState {
  float memory = 0;  // last voiced MIDI pitch, 0 = none
  int gap = 0;       // hops since that pitch
};

// Below E2 a reading must be strongly supported: this is where the common
// subharmonic of a chord or of bleed lands, rarely a sung note.
double lowRegisterPenalty(double midi) { return std::min(3.0, 0.5 * std::max(0.0, 40 - midi)); }

// Viterbi over each frame's candidates plus unvoiced states that remember the last
// voiced pitch. Returns the chosen candidate per frame (-1 = unvoiced).
std::vector<int> decodePath(const PitchAnalysis& a, const std::vector<double>& voicingCost, const std::vector<float>& silence) {
  const size_t frames = a.frames.size();
  std::vector<std::vector<int>> back(frames);
  std::vector<std::vector<UnvoicedState>> unvoicedStates(frames);
  std::vector<double> previousCost;
  for (size_t t = 0; t < frames; ++t) {
    const auto& cs = a.candidates[t];
    const bool energetic = a.frames[t].db > silence[t];
    double bestEmission = kInf;
    for (const auto& c : cs) bestEmission = std::min(bestEmission, static_cast<double>(c.emission));
    // Voicing is decided by full-band periodicity alone: the unvoiced state is
    // compared with the best candidate as if harmonic evidence did not exist.
    const double unvoicedEmission = !energetic || cs.empty() ? 0 : kUnvoiced + (bestEmission - voicingCost[t]);
    std::vector<double> cost;
    std::vector<int> from;
    std::vector<UnvoicedState> unvoiced;
    if (t == 0) {
      for (const auto& c : cs) { cost.push_back(energetic ? c.emission : kInf); from.push_back(-1); }
      unvoiced.push_back({});
      cost.push_back(unvoicedEmission);
      from.push_back(-1);
    } else {
      const auto& before = a.candidates[t - 1];
      const auto& beforeUnvoiced = unvoicedStates[t - 1];
      const size_t nv = before.size();
      for (const auto& c : cs) {
        double best = kInf;
        int arg = -1;
        if (energetic) {
          for (size_t j = 0; j < nv; ++j) {
            const double total = previousCost[j] + glideCost(std::fabs(c.midi - before[j].midi));
            if (total < best) { best = total; arg = static_cast<int>(j); }
          }
          for (size_t u = 0; u < beforeUnvoiced.size(); ++u) {
            const auto& state = beforeUnvoiced[u];
            const double move = state.memory > 0 ? reentryCost(std::fabs(c.midi - state.memory), state.gap + 1) : 0;
            const double total = previousCost[nv + u] + kToggle + move;
            if (total < best) { best = total; arg = static_cast<int>(nv + u); }
          }
        }
        cost.push_back(best + c.emission);
        from.push_back(arg);
      }
      // Unvoiced: from each voiced state (remembering its pitch) or carried on.
      struct Entry { UnvoicedState state; double cost; int from; };
      std::vector<Entry> entries;
      for (size_t j = 0; j < nv; ++j)
        if (previousCost[j] < kInf) entries.push_back({{before[j].midi, 1}, previousCost[j] + kToggle, static_cast<int>(j)});
      for (size_t u = 0; u < beforeUnvoiced.size(); ++u) {
        UnvoicedState s = beforeUnvoiced[u];
        if (s.memory > 0 && ++s.gap > kMemoryHops) s = {};
        entries.push_back({s, previousCost[nv + u], static_cast<int>(nv + u)});
      }
      std::stable_sort(entries.begin(), entries.end(), [](const Entry& p, const Entry& q) { return p.cost < q.cost; });
      // Keep the cheapest state per remembered pitch (at most 8) plus the free state.
      for (const auto& e : entries) {
        bool merged = false;
        for (const auto& k : unvoiced)
          if ((k.memory == 0) == (e.state.memory == 0) && std::fabs(k.memory - e.state.memory) < 0.5f) merged = true;
        if (merged || (e.state.memory > 0 && unvoiced.size() >= 8)) continue;
        unvoiced.push_back(e.state);
        cost.push_back(e.cost + unvoicedEmission);
        from.push_back(e.from);
      }
    }
    // Recenter to keep long recordings numerically well behaved.
    double minimum = kInf;
    for (double v : cost) minimum = std::min(minimum, v);
    if (minimum < kInf)
      for (auto& v : cost) v -= minimum;
    back[t] = std::move(from);
    unvoicedStates[t] = std::move(unvoiced);
    previousCost = std::move(cost);
  }
  std::vector<int> chosen(frames, -1);
  if (frames == 0) return chosen;
  int state = static_cast<int>(std::min_element(previousCost.begin(), previousCost.end()) - previousCost.begin());
  for (size_t t = frames; t-- > 0;) {
    if (static_cast<size_t>(state) < a.candidates[t].size()) chosen[t] = state;
    state = back[t][static_cast<size_t>(state)];
    if (state < 0) break;  // t == 0
  }
  return chosen;
}

// The singer's local register: level-weighted median of the decoded pitch within
// +-5 s, counting only anchor frames whose octave is not contested (every
// octave-related alternative is clearly worse), so a long octave error cannot
// vote for itself. Taken from the take itself, so no range is assumed; low and
// high sections of one take get their own register. 0 = unknown.
std::vector<float> localRegister(const PitchAnalysis& a, const std::vector<float>& silence) {
  constexpr int lowest = 30, bins = 70;
  const size_t frames = a.frames.size();
  const size_t half = static_cast<size_t>(5.0 / kAnalysisHopSeconds);
  std::vector<double> weight(frames, 0);
  std::vector<int> bin(frames, -1);
  std::vector<double> global(bins, 0);
  double globalTotal = 0;
  for (size_t t = 0; t < frames; ++t) {
    if (a.chosen[t] < 0) continue;
    const auto& cs = a.candidates[t];
    const auto& c = cs[static_cast<size_t>(a.chosen[t])];
    double margin = kInf;
    for (const auto& o : cs)
      if (octaveRelated(std::fabs(o.midi - c.midi))) margin = std::min(margin, static_cast<double>(o.emission - c.emission));
    if (margin < 1.0) continue;
    bin[t] = std::clamp(static_cast<int>(std::lround(c.midi)) - lowest, 0, bins - 1);
    weight[t] = std::clamp((a.frames[t].db - silence[t]) / 30.0, 0.1, 1.0) * std::max(0.05, 1.0 - c.cmnd);
    global[static_cast<size_t>(bin[t])] += weight[t];
    globalTotal += weight[t];
  }
  auto median = [&](const std::vector<double>& histogram, double total) {
    double run = 0;
    for (int b = 0; b < bins; ++b) {
      run += histogram[static_cast<size_t>(b)];
      if (run >= 0.5 * total) return static_cast<float>(lowest + b);
    }
    return 0.0f;
  };
  const float fallback = globalTotal > 0 ? median(global, globalTotal) : 0.0f;
  std::vector<double> histogram(bins, 0);
  double total = 0;
  size_t count = 0, lo = 0, hi = 0;  // window [lo, hi)
  std::vector<float> out(frames, fallback);
  for (size_t t = 0; t < frames; ++t) {
    const size_t wantLo = t > half ? t - half : 0, wantHi = std::min(frames, t + half + 1);
    for (; hi < wantHi; ++hi)
      if (bin[hi] >= 0) histogram[static_cast<size_t>(bin[hi])] += weight[hi], total += weight[hi], ++count;
    for (; lo < wantLo; ++lo)
      if (bin[lo] >= 0) histogram[static_cast<size_t>(bin[lo])] -= weight[lo], total -= weight[lo], --count;
    if (static_cast<double>(count) * kAnalysisHopSeconds >= 0.5 && total > 0) out[t] = median(histogram, total);
  }
  return out;
}

// Tonal breaths (kToneOnlyGap) among the first path's voiced frames: in a voiced
// run whose tone-only frames' median tone stands kBreathOverMedian over the
// median reading of the take's other voiced frames, those frames go, and so does
// every frame whose strongest partial is that same tone (kBreathSameTone): the
// whistle where breath noise under 1 kHz hides it from kToneOnlyGap. A run that
// is mostly tone-only frames goes whole (its weaker frames are the same inhale). Unless they add up to more than an inhale's share (kBreathMaxFrames,
// kBreathMaxShare), they become silence without candidates, so neither the
// second pass, the register, a pitch for an unvoiced syllable
// (assignNotePitches) nor a weak sung frame (articulation) comes from them.
void dropTonalBreaths(PitchAnalysis& a, const std::vector<char>& toneOnly, const std::vector<float>& toneMidi,
                      std::vector<float>& silence) {
  const size_t frames = a.frames.size();
  std::vector<float> voice;
  size_t voiced = 0;
  for (size_t t = 0; t < frames; ++t) {
    if (a.chosen[t] < 0) continue;
    ++voiced;
    if (!toneOnly[t]) voice.push_back(a.candidates[t][static_cast<size_t>(a.chosen[t])].midi);
  }
  if (voice.size() < kBreathMinVoice) return;
  std::nth_element(voice.begin(), voice.begin() + static_cast<long>(voice.size() / 2), voice.end());
  const float limit = voice[voice.size() / 2] + kBreathOverMedian;
  std::vector<size_t> breath;
  for (size_t t = 0; t < frames;) {
    if (a.chosen[t] < 0) {
      ++t;
      continue;
    }
    size_t end = t;
    std::vector<float> tones;
    for (; end < frames && a.chosen[end] >= 0; ++end)
      if (toneOnly[end]) tones.push_back(toneMidi[end]);
    if (!tones.empty()) {
      std::nth_element(tones.begin(), tones.begin() + static_cast<long>(tones.size() / 2), tones.end());
      const float tone = tones[tones.size() / 2];
      const bool whole = 2 * tones.size() >= end - t;
      if (tone >= limit)
        for (size_t i = t; i < end; ++i)
          if (whole || (toneOnly[i] && toneMidi[i] >= limit) || std::fabs(toneMidi[i] - tone) <= kBreathSameTone) breath.push_back(i);
    }
    t = end;
  }
  if (breath.size() > kBreathMaxFrames || static_cast<double>(breath.size()) > kBreathMaxShare * static_cast<double>(voiced)) return;
  for (size_t i : breath) {
    a.chosen[i] = -1;
    a.candidates[i].clear();
    silence[i] = std::numeric_limits<float>::infinity();
  }
}

// Readings more than 9 semitones from the local register cost a little per frame
// (0.15 per semitone beyond): enough to break near-ties between octave readings
// (weak fundamental vs period doubling look alike in the spectrum), not to block
// a clearly supported leap.
double registerPenalty(double midi, float reg) {
  if (reg <= 0) return 0;
  return std::min(2.0, 0.15 * std::max(0.0, std::fabs(midi - reg) - 9));
}

// PitchAnalysis::polyphony from which a take has accompaniment (the import's
// warning threshold: dev chords under the voice 0.64-0.66, guitar bleed 0.30, solo <= 0.21).
constexpr float kAccompanied = 0.35f;

// Octave flips of the decoded path. Inside continuous voicing a one-hop
// octave jump is a tracker signature (a sung 12-semitone move takes >= ~120
// ms), but the decoder takes one when the spectrum turns ambiguous for long
// enough: a voice with a weak fundamental (H1 ~20 dB under H2, a high voice
// with F1 on H2) whose odd harmonics fade reads as its own 2nd harmonic for
// as long as they stay faded. Whether the voice moved is told by its lower
// octave's fundamental partial: a real jump up leaves it behind (R4 audit "the
// end": a release 24 dB under the new fundamental at the jump, 43 dB 80 ms
// later), a flip keeps it at the level it had (the lower reading's H1-H2 within
// kFlipFadeDb on both sides of the jump, over kFlipHoldFrames of the upper side
// so a decaying release cannot pass). The
// upper side's frames then take the lower reading, up to the next jump, a
// voicing gap the decoder's memory does not bridge, or the fundamental
// fading; and their upper reading loses its spectral advantage, so the notes
// built on them carry no evidence for it. The lower side must be a reading the
// spectrum supports (harmonic >= -kFlipSupport): a period-doubled reading
// (penalised ~0.6-1.4 on the dev doubled phrases) jumping up to the true
// pitch while its subharmonic lasts is a correction, not a flip. Accompanied
// takes are left alone (a bass under the voice is a persisting lower partial).
constexpr float kFlipJump = 1.0f;          // semitones around 12 that make a one-hop jump an octave flip
constexpr float kFlipContinuity = 1.0f;    // semitones per frame along one reading on either side
constexpr size_t kFlipSideFrames = 5;      // frames (40 ms) that give the lower side's fundamental level
constexpr size_t kFlipHoldFrames = 10;     // frames (80 ms) over which the upper side must hold it
constexpr float kFlipFadeDb = 8;           // as kSubharmonicDb: a partial this much weaker is another component
constexpr float kFlipSupport = 0.5f;       // largest harmonic penalty of a supported lower reading
constexpr float kFlipTrack = 2.0f;         // semitones the upper reading may move per frame and stay one flip

void undoOctaveFlips(PitchAnalysis& a, const std::vector<char>& polyphonic, const std::vector<float>& silence) {
  const size_t frames = a.frames.size();
  size_t loud = 0, loudPolyphonic = 0;
  for (size_t t = 0; t < frames; ++t)
    if (a.chosen[t] >= 0 && a.frames[t].db > silence[t] + 20) ++loud, loudPolyphonic += polyphonic[t] ? 1 : 0;
  if (static_cast<double>(loudPolyphonic) >= kAccompanied * static_cast<double>(loud)) return;
  auto chosen = [&](size_t t) -> const PitchCandidateV2& { return a.candidates[t][static_cast<size_t>(a.chosen[t])]; };
  auto lowerOf = [&](size_t t) {  // the candidate an octave under the chosen reading, or -1
    int best = -1;
    const auto& cs = a.candidates[t];
    const float target = chosen(t).midi - 12;
    for (size_t k = 0; k < cs.size(); ++k)
      if (std::fabs(cs[k].midi - target) <= 0.75f &&
          (best < 0 || std::fabs(cs[k].midi - target) < std::fabs(cs[static_cast<size_t>(best)].midi - target)))
        best = static_cast<int>(k);
    return best;
  };
  auto median = [](std::vector<float> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    return v[v.size() / 2];
  };
  // The lower octave's fundamental level (dB over its 2nd harmonic) on one side
  // of a jump, from `from` stepping by `step` along one continuous reading: the
  // reading itself on the lower side, its octave-lower candidate on the upper.
  struct Side { float level = 0, harmonic = 0; size_t count = 0; };
  auto side = [&](long long from, int step, bool lower, size_t want) {
    Side out;
    std::vector<float> level, harmonic;
    float last = chosen(static_cast<size_t>(from)).midi;
    for (long long k = from; k >= 0 && k < static_cast<long long>(frames) && level.size() < want; k += step) {
      const size_t i = static_cast<size_t>(k);
      if (a.chosen[i] < 0 || std::fabs(chosen(i).midi - last) > kFlipContinuity) break;
      last = chosen(i).midi;
      if (lower) {
        level.push_back(chosen(i).fundamental);
        harmonic.push_back(chosen(i).harmonic);
        continue;
      }
      const int low = lowerOf(i);
      if (low < 0) break;
      level.push_back(a.candidates[i][static_cast<size_t>(low)].fundamental);
    }
    out.count = level.size();
    if (out.count > 0) out.level = median(level);
    if (!harmonic.empty()) out.harmonic = median(harmonic);
    return out;
  };
  std::vector<char> relabelled(frames, 0);
  for (size_t t = 1; t < frames; ++t) {
    if (a.chosen[t] < 0 || a.chosen[t - 1] < 0 || relabelled[t] || relabelled[t - 1]) continue;
    const float jump = chosen(t).midi - chosen(t - 1).midi;
    if (std::fabs(std::fabs(jump) - 12) >= kFlipJump) continue;
    const bool up = jump > 0;
    const long long lowFrom = static_cast<long long>(up ? t - 1 : t), upFrom = static_cast<long long>(up ? t : t - 1);
    const int upStep = up ? 1 : -1;
    const Side low = side(lowFrom, -upStep, true, kFlipSideFrames);
    const Side high = side(upFrom, upStep, false, kFlipHoldFrames);
    if (low.count < 3 || high.count < kFlipSideFrames || low.harmonic < -kFlipSupport || high.level < low.level - kFlipFadeDb) continue;
    float reading = chosen(static_cast<size_t>(lowFrom)).midi;  // the lower reading being continued
    std::vector<float> recent;
    int gap = 0;
    for (long long k = upFrom; k >= 0 && k < static_cast<long long>(frames); k += upStep) {
      const size_t i = static_cast<size_t>(k);
      if (a.chosen[i] < 0) {
        if (++gap > kMemoryHops) break;
        continue;
      }
      if (std::fabs(chosen(i).midi - (reading + 12)) > kFlipTrack) break;
      const int lowIndex = lowerOf(i);
      if (lowIndex < 0) break;
      const auto& lower = a.candidates[i][static_cast<size_t>(lowIndex)];
      recent.push_back(lower.fundamental);
      if (recent.size() > kFlipSideFrames) recent.erase(recent.begin());
      if (median(recent) < low.level - kFlipFadeDb) break;
      auto& upper = a.candidates[i][static_cast<size_t>(a.chosen[i])];
      upper.emission = std::max(upper.emission, lower.emission);
      a.chosen[i] = lowIndex;
      relabelled[i] = 1;
      reading = lower.midi;
      gap = 0;
    }
  }
}

}  // namespace

// Fractional-delay waveform matching after octave/voicing decoding. Unlike a
// parabola through three integer-lag CMND values, this evaluates the waveform at
// fractional lags directly. Symmetric sample pairs keep the estimate centred
// on the frame, and a short period-scaled window retains vibrato and glides.
// This is an independently written local refinement, not another pitch decoder.
namespace {
double refinePeriod(const std::vector<float>& signal, long long center, double seed) {
  constexpr double radius = 0.35;  // semitones: cannot change an octave decision
  const double ratio = std::exp2(radius / 12);
  const double lo = seed / ratio, hi = seed * ratio;
  const int count = std::clamp(static_cast<int>(std::lround(6 * seed)), 192, 512);
  // Static storage keeps this a constant expression inside the lambdas on MSVC.
  static constexpr int halfTaps = 6;
  const long long margin = count / 2 + static_cast<long long>(std::ceil(hi / 2)) + halfTaps + 1;
  if (center < margin || center + margin >= static_cast<long long>(signal.size())) return seed;
  double mean = 0;
  for (int i = -count / 2; i < count - count / 2; ++i) mean += signal[static_cast<size_t>(center + i)];
  mean /= count;
  auto mismatch = [&](double lag) {
    // Windowed-sinc fractional delays preserve high partials; low-order polynomial
    // interpolation can bias the period of bright upper-register voices. The
    // fractional positions are constant throughout each window, so reuse taps.
    std::array<double, 2 * halfTaps> leftTaps{}, rightTaps{};
    const double leftPosition = center - count / 2 - lag / 2;
    const double rightPosition = center - count / 2 + lag / 2;
    const auto leftIndex = static_cast<size_t>(leftPosition);
    const auto rightIndex = static_cast<size_t>(rightPosition);
    auto taps = [&](double fraction, auto& coefficients) {
      double sum = 0;
      for (int j = 0; j < 2 * halfTaps; ++j) {
        const double distance = j - halfTaps + 1 - fraction;
        const double sinc = std::fabs(distance) < 1e-12 ? 1.0 : std::sin(kPi * distance) / (kPi * distance);
        coefficients[static_cast<size_t>(j)] = sinc * (0.5 + 0.5 * std::cos(kPi * distance / halfTaps));
        sum += coefficients[static_cast<size_t>(j)];
      }
      for (auto& coefficient : coefficients) coefficient /= sum;
    };
    taps(leftPosition - leftIndex, leftTaps);
    taps(rightPosition - rightIndex, rightTaps);
    double difference = 0, level = 0;
    for (int i = 0; i < count; ++i) {
      double left = 0, right = 0;
      const float* lp = signal.data() + leftIndex + i - halfTaps + 1;
      const float* rp = signal.data() + rightIndex + i - halfTaps + 1;
      for (int j = 0; j < 2 * halfTaps; ++j) {
        left += lp[j] * leftTaps[static_cast<size_t>(j)];
        right += rp[j] * rightTaps[static_cast<size_t>(j)];
      }
      difference += std::fabs(left - right);
      level += std::fabs(left - mean) + std::fabs(right - mean);
    }
    return level > 1e-12 ? difference / level : 1.0;
  };
  const double original = mismatch(seed);
  constexpr int intervals = 8;
  int best = 0;
  double bestScore = 1;
  for (int j = 0; j <= intervals; ++j) {
    const double score = mismatch(lo + (hi - lo) * j / intervals);
    if (score < bestScore) bestScore = score, best = j;
  }
  // A minimum outside the search neighbourhood is not evidence for a small correction.
  if (best == 0 || best == intervals) return seed;
  double left = lo + (hi - lo) * (best - 1) / intervals;
  double right = lo + (hi - lo) * (best + 1) / intervals;
  constexpr double golden = 0.6180339887498949;
  double p = right - golden * (right - left), q = left + golden * (right - left);
  double fp = mismatch(p), fq = mismatch(q);
  for (int iteration = 0; iteration < 16; ++iteration) {
    if (fp < fq) {
      right = q; q = p; fq = fp;
      p = right - golden * (right - left); fp = mismatch(p);
    } else {
      left = p; p = q; fp = fq;
      q = left + golden * (right - left); fq = mismatch(q);
    }
  }
  const double refined = fp < fq ? p : q;
  bestScore = std::min(fp, fq);
  // A large residual can be noise or a changing waveform; retain the decoder's
  // estimate instead of trusting a small improvement in that ambiguous fit.
  return bestScore < 0.10 && bestScore < original * 0.98 ? refined : seed;
}
}  // namespace

PitchAnalysis analyzeSingingPitch(const std::vector<float>& x, bool refinePeriods) {
  PitchAnalysis a;
  a.duration = static_cast<double>(x.size()) / kRate;
  if (x.empty()) return a;
  const size_t frames = analysisFrameCount(x.size());
  const std::vector<float> low = lowBand(x);
  a.frames.resize(frames);
  a.candidates.resize(frames);
  a.chosen.assign(frames, -1);
  std::vector<float> fullCmnd, lowCmnd, local, combined(static_cast<size_t>(kTauMax + 2));
  std::vector<double> voicingCost(frames, kInf);  // best full-band periodicity cost per frame
  std::vector<char> polyphonicFlag(frames, 0);
  std::vector<char> toneOnly(frames, 0);    // a lone partial over 1 kHz (kToneOnlyGap)
  std::vector<float> toneMidi(frames, 0);   // the strongest partial to 2.5 kHz
  FrameSpectrum spectrum;
  FrameSpectrumAnalyzer analyzer(kFft);

  for (size_t t = 0; t < frames; ++t) {
    const long long center = static_cast<long long>(t) * kAnalysisHop;
    PitchFrame& pf = a.frames[t];
    pf.time = analysisFrameTime(t);
    pf.endTime = std::min((static_cast<double>(center) + kAnalysisHop / 2.0) / kRate, a.duration);
    pf.db = static_cast<float>(frameDb(x, center));

    symmetricCmnd(x, center, kWindow, kTauMax, local, fullCmnd);
    symmetricCmnd(low, center, kWindow, kTauMax, local, lowCmnd);
    // Combined periodicity: the better of the full band and the low band.
    for (int tau = 1; tau <= kTauMax + 1; ++tau)
      combined[static_cast<size_t>(tau)] = std::min(fullCmnd[static_cast<size_t>(tau)], lowCmnd[static_cast<size_t>(tau)]);
    combined[0] = 1;
    auto refine = [&](const std::vector<float>& f, int tau, double& lag, float& value) {
      const double a0 = f[static_cast<size_t>(tau - 1)], b0 = f[static_cast<size_t>(tau)], c0 = f[static_cast<size_t>(tau + 1)];
      const double denominator = a0 - 2 * b0 + c0;
      const double offset = denominator > 1e-12 ? std::clamp(0.5 * (a0 - c0) / denominator, -0.5, 0.5) : 0;
      lag = tau + offset;
      value = static_cast<float>(std::clamp(b0 - 0.25 * (a0 - c0) * offset, 0.0, 1.0));
    };
    // Full-band CMND near a lag (its local minimum within one sample).
    auto fullNear = [&](double lag) {
      const int c = static_cast<int>(std::lround(lag));
      float best = 1;
      for (int tau = std::max(1, c - 1); tau <= std::min(kTauMax + 1, c + 1); ++tau) best = std::min(best, fullCmnd[static_cast<size_t>(tau)]);
      return best;
    };
    // Lag and periodicity of the dip at lag tau, each band refined at its own
    // nearby minimum. The full band gives the lag unless its dip is clearly worse
    // (split or missing under period doubling); then the low band does.
    auto estimate = [&](int tau, double& lag, float& value, bool& fromLowBand) {
      auto bandMinimum = [&](const std::vector<float>& f, double& bandLag, float& bandValue) {
        int at = tau;
        for (int d : {-1, 1})
          if (tau + d >= 2 && tau + d <= kTauMax && f[static_cast<size_t>(tau + d)] < f[static_cast<size_t>(at)]) at = tau + d;
        refine(f, at, bandLag, bandValue);
      };
      double fullLag, lowLag;
      float fullValue, lowValue;
      bandMinimum(fullCmnd, fullLag, fullValue);
      bandMinimum(lowCmnd, lowLag, lowValue);
      fromLowBand = fullValue > lowValue + 0.05f;
      lag = fromLowBand ? lowLag : fullLag;
      value = std::min(fullValue, lowValue);
    };
    std::vector<PitchCandidateV2> found;
    auto addCandidate = [&](double lag, float value, bool fromLowBand) {
      const double hz = kRate / lag;
      if (hz < kMinHz || hz > kMaxHz) return;
      PitchCandidateV2 c;
      c.lag = lag;
      c.lowBandLag = fromLowBand;
      c.midi = midiOf(hz);
      c.cmnd = value;
      found.push_back(c);
    };
    int globalBest = kTauMin;
    for (int tau = kTauMin; tau <= kTauMax; ++tau) {
      if (combined[static_cast<size_t>(tau)] < combined[static_cast<size_t>(globalBest)]) globalBest = tau;
      if (combined[static_cast<size_t>(tau)] > combined[static_cast<size_t>(tau - 1)] ||
          combined[static_cast<size_t>(tau)] > combined[static_cast<size_t>(tau + 1)]) continue;
      double lag;
      float value;
      bool fromLowBand;
      estimate(tau, lag, value, fromLowBand);
      if (value <= kCandidateCmnd) addCandidate(lag, value, fromLowBand);
    }
    if (found.empty()) {
      double lag;
      float value;
      bool fromLowBand;
      estimate(globalBest, lag, value, fromLowBand);
      addCandidate(lag, value, fromLowBand);
    }
    // Sub-multiples of strong periods, refined to the best nearby lag even when
    // it is no local minimum: the true period of a period-doubled voice often is not.
    // It is still fairly periodic there; a lag that is not (a low bass's half
    // period) would only be an arbitrary pitch within +-4% of the octave.
    const size_t seeds = found.size();
    for (size_t i = 0; i < seeds; ++i) {
      if (found[i].cmnd > kStrongCmnd) continue;
      for (int m : {2, 3, 4}) {
        const double target = found[i].lag / m;
        const int lo = std::max(kTauMin, static_cast<int>(std::floor(target * 0.96)));
        const int hi = std::min(kTauMax, static_cast<int>(std::ceil(target * 1.04)));
        if (lo > hi) continue;
        int best = lo;
        for (int tau = lo; tau <= hi; ++tau)
          if (combined[static_cast<size_t>(tau)] < combined[static_cast<size_t>(best)]) best = tau;
        double lag;
        float value;
        bool fromLowBand;
        estimate(best, lag, value, fromLowBand);
        if (value <= kSubmultipleCmnd) addCandidate(lag, value, fromLowBand);
      }
    }
    std::stable_sort(found.begin(), found.end(), [](const PitchCandidateV2& p, const PitchCandidateV2& q) { return p.cmnd < q.cmnd; });
    // A periodic wave has equally deep dips at every multiple of its period; keep
    // the shortest credible one ahead of them so the cap never drops the true period.
    auto shortest = found.end();
    for (auto it = found.begin(); it != found.end(); ++it)
      if (it->cmnd <= kStrongCmnd && (shortest == found.end() || it->lag < shortest->lag)) shortest = it;
    if (shortest != found.end()) std::rotate(found.begin(), shortest, shortest + 1);
    // So is the shortest of the deepest dips: a shallow spurious dip at a shorter
    // lag can take the place above, and at high pitch the true period's dip is a
    // hair shallower than its many multiples' (it falls between two samples).
    if (!found.empty()) {
      float deepest = 1;
      for (const auto& c : found) deepest = std::min(deepest, c.cmnd);
      auto deep = found.end();
      for (auto it = found.begin(); it != found.end(); ++it)
        if (it->cmnd <= deepest + 0.05f && (deep == found.end() || it->lag < deep->lag)) deep = it;
      if (deep != found.begin()) std::rotate(found.begin() + 1, deep, deep + 1);
    }
    auto& kept = a.candidates[t];
    for (const auto& c : found) {
      if (kept.size() >= kMaxCandidates) break;
      bool duplicate = false;
      for (const auto& k : kept)
        if (std::fabs(c.midi - k.midi) < 0.35f) duplicate = true;
      if (!duplicate) kept.push_back(c);
    }

    analyzer.analyze(x, center, spectrum);
    {
      float under1k = -200, top = -200;
      int topBin = 0;
      for (int i = static_cast<int>(60 / spectrum.binHz); i <= static_cast<int>(2500 / spectrum.binHz); ++i) {
        const float d = spectrum.db[static_cast<size_t>(i)];
        if (d > top) top = d, topBin = i;
        if (i * spectrum.binHz < 1000) under1k = std::max(under1k, d);
      }
      toneOnly[t] = top - under1k >= kToneOnlyGap;
      toneMidi[t] = midiOf(topBin * spectrum.binHz);
    }

    double bestPeriodicity = kInf;
    float bestCmnd = 1;
    for (const auto& c : kept) bestCmnd = std::min(bestCmnd, c.cmnd);
    for (auto& c : kept) {
      const double f = kRate / c.lag;
      c.harmonic = static_cast<float>(-harmonicPenalty(spectrum, f));
      auto partialDb = [&](double hz) { return spectrum.db[static_cast<size_t>(spectrum.peakBin(hz, std::max(0.5 * spectrum.binHz, 0.025 * hz)))]; };
      c.fundamental = partialDb(f) - partialDb(2 * f);
      c.emission = static_cast<float>(kPeriodicityWeight * bestCmnd + kRelativePeriodicityWeight * (c.cmnd - bestCmnd) -
                                       c.harmonic + lowRegisterPenalty(c.midi));
      bestPeriodicity = std::min(bestPeriodicity, kPeriodicityWeight * fullNear(c.lag));
    }
    voicingCost[t] = bestPeriodicity;
    polyphonicFlag[t] = polyphonicFrame(spectrum, kept);
  }

  // Silence floor as in singing-v1, 50 dB under the loud frames, but of the
  // frames within kQuietHalf (updated every 8 frames), so a quiet section keeps
  // its soft frames; never more than kQuietRange under the take's floor.
  std::vector<float> silence(frames);
  {
    auto loud = [](std::vector<float>::const_iterator from, std::vector<float>::const_iterator to) {
      std::vector<float> v(from, to);
      const auto at = static_cast<long>(0.95 * static_cast<double>(v.size() - 1));
      std::nth_element(v.begin(), v.begin() + at, v.end());
      return v[static_cast<size_t>(at)];
    };
    std::vector<float> levels;
    levels.reserve(frames);
    for (const auto& f : a.frames) levels.push_back(f.db);
    a.floorDb = std::max(kSilenceDb, loud(levels.begin(), levels.end()) - 50.0f);
    for (size_t t = 0; t < frames; t += 8) {
      const size_t lo = t > kQuietHalf ? t - kQuietHalf : 0, hi = std::min(frames, t + kQuietHalf + 1);
      const float local = loud(levels.begin() + static_cast<long>(lo), levels.begin() + static_cast<long>(hi)) - 50.0f;
      std::fill(silence.begin() + static_cast<long>(t), silence.begin() + static_cast<long>(std::min(frames, t + 8)),
                std::max({kSilenceDb, local, a.floorDb - kQuietRange}));
    }
  }

  // Two passes: the first path gives the singer's local register, which then
  // breaks ties between octave readings the spectrum cannot separate.
  a.chosen = decodePath(a, voicingCost, silence);
  dropTonalBreaths(a, toneOnly, toneMidi, silence);
  const std::vector<float> reg = localRegister(a, silence);
  for (size_t t = 0; t < frames; ++t)
    for (auto& c : a.candidates[t]) c.emission += static_cast<float>(registerPenalty(c.midi, reg[t]));
  a.chosen = decodePath(a, voicingCost, silence);

  // A breathy onset or fading release may be less periodic than its vowel.
  // Extend voiced runs through neighbouring energetic frames along a continuous
  // chain of candidates (each within 0.6 st of the last), at most 5 hops.
  const auto decoded = a.chosen;
  auto extend = [&](size_t edge, int direction) {
    float anchor = a.candidates[edge][static_cast<size_t>(decoded[edge])].midi;
    for (int step = 1; step <= 5; ++step) {
      const long long i = static_cast<long long>(edge) + direction * step;
      if (i < 0 || i >= static_cast<long long>(frames) || decoded[static_cast<size_t>(i)] >= 0) return;
      if (a.frames[static_cast<size_t>(i)].db <= silence[static_cast<size_t>(i)]) return;
      int best = -1;
      const auto& cs = a.candidates[static_cast<size_t>(i)];
      for (size_t k = 0; k < cs.size(); ++k)
        if (cs[k].cmnd <= 0.35f && std::fabs(cs[k].midi - anchor) <= 0.6f && (best < 0 || cs[k].cmnd < cs[static_cast<size_t>(best)].cmnd))
          best = static_cast<int>(k);
      if (best < 0) return;
      a.chosen[static_cast<size_t>(i)] = best;
      anchor = cs[static_cast<size_t>(best)].midi;
    }
  };
  for (size_t t = 0; t < frames; ++t) {
    if (decoded[t] < 0 || 1 - a.candidates[t][static_cast<size_t>(decoded[t])].cmnd < 0.8f) continue;
    if (t > 0 && decoded[t - 1] < 0) extend(t, -1);
    if (t + 1 < frames && decoded[t + 1] < 0) extend(t, 1);
  }

  undoOctaveFlips(a, polyphonicFlag, silence);

  // Frames, confidence and polyphony.
  size_t voiced = 0, polyphonic = 0;
  for (size_t t = 0; t < frames; ++t) {
    auto& pf = a.frames[t];
    const int k = a.chosen[t];
    if (k < 0) {
      float best = 1;
      for (const auto& c : a.candidates[t]) best = std::min(best, c.cmnd);
      pf.midi = 0;
      pf.f0 = 0;
      pf.clarity = 1 - best;
      pf.confidence = 0;
      continue;
    }
    const auto& cs = a.candidates[t];
    const auto& c = cs[static_cast<size_t>(k)];
    pf.midi = c.midi;
    pf.f0 = static_cast<float>(kRate / c.lag);
    pf.clarity = 1 - c.cmnd;
    // Octave decisiveness: how much worse the best octave-related alternative is.
    double margin = 1.5;
    for (size_t j = 0; j < cs.size(); ++j)
      if (static_cast<int>(j) != k && octaveRelated(std::fabs(cs[j].midi - c.midi)))
        margin = std::min(margin, static_cast<double>(cs[j].emission - c.emission));
    pf.confidence = pf.clarity * static_cast<float>(0.6 + 0.4 * std::clamp(margin / 1.5, 0.0, 1.0));
    if (pf.db > silence[t] + 20) {
      ++voiced;
      if (polyphonicFlag[t]) {
        ++polyphonic;
        pf.confidence *= 0.6f;
      }
    }
  }
  a.polyphony = voiced ? static_cast<float>(polyphonic) / static_cast<float>(voiced) : 0;
  a.polyphonic.assign(polyphonicFlag.begin(), polyphonicFlag.end());
  // Preserve the decoder's confidence as well as its octave and voicing decisions.
  if (refinePeriods) {
    // Use original decoded pitches for all gates, so the result is independent
    // of traversal direction. Skip ambiguous mixtures and abrupt transitions.
    std::vector<float> decodedMidi(frames, 0);
    for (size_t t = 0; t < frames; ++t)
      if (a.chosen[t] >= 0) decodedMidi[t] = a.candidates[t][static_cast<size_t>(a.chosen[t])].midi;
    for (size_t t = 1; t + 1 < frames; ++t) {
      if (a.chosen[t] < 0 || polyphonicFlag[t] || decodedMidi[t - 1] == 0 || decodedMidi[t + 1] == 0 ||
          std::fabs(decodedMidi[t] - decodedMidi[t - 1]) > 0.75f ||
          std::fabs(decodedMidi[t] - decodedMidi[t + 1]) > 0.75f) continue;
      auto& c = a.candidates[t][static_cast<size_t>(a.chosen[t])];
      // Long periods already have fine sample resolution. Limit this extra fit
      // to periods of at most 40 samples (400 Hz at our analysis rate), where
      // interpolation bias is material, without perturbing the lower register.
      if (c.cmnd > 0.25f || c.lag > 40) continue;
      const double lag = refinePeriod(c.lowBandLag ? low : x, static_cast<long long>(t) * kAnalysisHop, c.lag);
      const double hz = kRate / lag;
      if (hz >= kMinHz && hz <= kMaxHz) {
        c.lag = lag;
        c.midi = midiOf(hz);
        a.frames[t].midi = c.midi;
        a.frames[t].f0 = static_cast<float>(hz);
      }
    }
  }

  return a;
}

// ---------------------------------------------------------------------------
// Contours and note pitches

namespace {

// Weighted mode of rounded pitches (1-st bins smoothed over +-1 bin).
float dominantPitch(const std::vector<float>& values, const std::vector<float>& weights) {
  if (values.empty()) return 0;
  float lo = values[0], hi = values[0];
  for (float v : values) lo = std::min(lo, v), hi = std::max(hi, v);
  const int base = static_cast<int>(std::floor(lo)) - 1;
  std::vector<double> hist(static_cast<size_t>(std::ceil(hi) - static_cast<float>(base) + 3), 0);
  for (size_t i = 0; i < values.size(); ++i) {
    const int b = static_cast<int>(std::lround(values[i])) - base;
    hist[static_cast<size_t>(b)] += weights[i];
    if (b > 0) hist[static_cast<size_t>(b - 1)] += 0.5 * weights[i];
    if (b + 1 < static_cast<int>(hist.size())) hist[static_cast<size_t>(b + 1)] += 0.5 * weights[i];
  }
  const size_t peak = static_cast<size_t>(std::max_element(hist.begin(), hist.end()) - hist.begin());
  // Refine to the weighted mean of the values in the winning bin's neighbourhood.
  double sum = 0, weight = 0;
  for (size_t i = 0; i < values.size(); ++i)
    if (std::fabs(values[i] - (static_cast<double>(peak) + base)) <= 1.0) sum += weights[i] * values[i], weight += weights[i];
  return static_cast<float>(weight > 0 ? sum / weight : static_cast<double>(peak) + base);
}

// Level weight of a frame within its note: quiet onsets/releases count less.
float levelWeight(float db, float peakDb) { return std::clamp((db - (peakDb - 30)) / 20.0f, 0.1f, 1.0f); }

// Index of the candidate nearest `target` within `tolerance` semitones, or -1.
int candidateNear(const std::vector<PitchCandidateV2>& cs, float target, float tolerance) {
  int best = -1;
  for (size_t k = 0; k < cs.size(); ++k)
    if (std::fabs(cs[k].midi - target) <= tolerance &&
        (best < 0 || std::fabs(cs[k].midi - target) < std::fabs(cs[static_cast<size_t>(best)].midi - target)))
      best = static_cast<int>(k);
  return best;
}

float weightedPercentile(std::vector<std::pair<float, float>> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double total = 0;
  for (const auto& e : v) total += e.second;
  double run = 0;
  for (const auto& e : v) {
    run += e.second;
    if (run >= p * total) return e.first;
  }
  return v.back().first;
}

// Whole vibrato cycles of a contour: turning points found with 0.3 st
// hysteresis (frame jitter is ignored), consecutive ones 56-143 ms apart (a 3.5-9
// Hz oscillation). Each run of at least 4 such turns gives the index range from
// its first to its last turning point of the same kind and the mean cycle length
// in frames, in order; vibratoCycles is the longest run (empty when there is none).
struct VibratoCycles { size_t first = 0, last = 0; double period = 0; size_t turns = 0; };
std::vector<VibratoCycles> vibratoRuns(const std::vector<float>& v) {
  std::vector<std::pair<size_t, int>> turns;  // index, +1 peak / -1 trough
  int direction = 0;
  size_t extreme = 0;
  for (size_t i = 1; i < v.size(); ++i) {
    if (direction >= 0 && v[i] >= v[extreme]) { extreme = i; if (direction == 0 && v[i] - v[0] >= 0.3f) direction = 1; continue; }
    if (direction <= 0 && v[i] <= v[extreme]) { extreme = i; if (direction == 0 && v[0] - v[i] >= 0.3f) direction = -1; continue; }
    if (direction > 0 && v[extreme] - v[i] >= 0.3f) { turns.push_back({extreme, 1}); direction = -1; extreme = i; }
    else if (direction < 0 && v[i] - v[extreme] >= 0.3f) { turns.push_back({extreme, -1}); direction = 1; extreme = i; }
  }
  std::vector<VibratoCycles> runs;
  for (size_t a = 0; a < turns.size();) {
    size_t b = a + 1;
    while (b < turns.size()) {
      const double gap = static_cast<double>(turns[b].first - turns[b - 1].first) * kAnalysisHopSeconds;
      if (gap < 0.056 || gap > 0.143) break;
      ++b;
    }
    if (b - a >= 4) {
      size_t last = b - 1;
      if (turns[last].second != turns[a].second) --last;
      VibratoCycles c;
      c.first = turns[a].first;
      c.last = turns[last].first;
      c.period = 2.0 * static_cast<double>(c.last - c.first) / static_cast<double>(last - a);
      c.turns = b - a;
      runs.push_back(c);
    }
    a = b;
  }
  return runs;
}
VibratoCycles vibratoCycles(const std::vector<float>& v) {
  VibratoCycles best;  // the earliest of the runs with the most turns
  for (const auto& c : vibratoRuns(v))
    if (c.turns > best.turns) best = c;
  return best;
}

// Perceived centre of a note contour: a mode-seeking mean that ignores scoops,
// fall-offs and overshoots (they move, the held pitch dwells). With vibrato it
// is taken on the vibrato's centre line, which is flat at the mean over whole
// cycles while the note is held.
float robustCentre(const std::vector<float>& v, const std::vector<float>& w, float& spread) {
  const size_t n = v.size();
  spread = 0;
  if (n == 0) return 0;
  // Slow drift: Gaussian (sigma 44 ms) smoothing; the residual is modulation.
  std::vector<float> drift(n);
  const double sigma = 0.0435 / kAnalysisHopSeconds;
  for (size_t i = 0; i < n; ++i) {
    double s = 0, ws = 0;
    for (long long j = std::max(0LL, static_cast<long long>(i) - 15); j <= std::min(static_cast<long long>(n) - 1, static_cast<long long>(i) + 15); ++j) {
      const double g = std::exp(-0.5 * std::pow(static_cast<double>(j - static_cast<long long>(i)) / sigma, 2));
      s += g * v[static_cast<size_t>(j)];
      ws += g;
    }
    drift[i] = static_cast<float>(s / ws);
  }
  std::vector<std::pair<float, float>> residual;
  for (size_t i = 0; i < n; ++i) residual.push_back({v[i] - drift[i], w[i]});
  spread = weightedPercentile(residual, 0.9) - weightedPercentile(residual, 0.1);

  // Vibrato: each frame averaged over one cycle. The mean over whole cycles
  // would also count a slow approach sung with vibrato (61 -> 62 while
  // oscillating); on the centre line the approach moves and the held part dwells.
  std::vector<float> line;
  const VibratoCycles cycles = vibratoCycles(v);
  if (cycles.last > cycles.first) {
    const size_t half = static_cast<size_t>(std::lround(cycles.period / 2));
    std::vector<double> sum(n + 1, 0);
    for (size_t i = 0; i < n; ++i) sum[i + 1] = sum[i] + v[i];
    for (size_t i = 0; i < n; ++i) {
      const size_t a = i >= half ? i - half : 0, b = std::min(n, i + half + 1);
      line.push_back(static_cast<float>((sum[b] - sum[a]) / static_cast<double>(b - a)));
    }
  }
  const std::vector<float>& x = line.empty() ? v : line;
  // Mode seed from a kernel density (sigma 0.35 st), then mean shift within +-0.6 st.
  size_t seed = 0;
  double bestDensity = -1;
  const size_t stride = std::max<size_t>(1, n / 200);
  for (size_t i = 0; i < n; i += stride) {
    double d = 0;
    for (size_t j = 0; j < n; j += stride) d += w[j] * std::exp(-0.5 * std::pow((x[j] - x[i]) / 0.35, 2));
    if (d > bestDensity) bestDensity = d, seed = i;
  }
  double centre = x[seed], total = 0;
  for (float weight : w) total += weight;
  for (int iteration = 0; iteration < 4; ++iteration) {
    double s = 0, ws = 0;
    for (size_t i = 0; i < n; ++i)
      if (std::fabs(x[i] - centre) <= 0.6) s += w[i] * x[i], ws += w[i];
    if (ws < 0.3 * total) {
      std::vector<std::pair<float, float>> all;
      for (size_t i = 0; i < n; ++i) all.push_back({x[i], w[i]});
      return weightedPercentile(all, 0.5);
    }
    centre = s / ws;
  }
  return static_cast<float>(centre);
}

// Octave-related offset (+-12, 19, 24) of `midi` from `reference`, or 0 when it
// is within 8.5 semitones or at an unrelated interval.
constexpr float kRegisterTolerance = 3.0f;  // semitones around an octave-related interval
float octaveOffset(float midi, float reference) {
  const float rel = midi - reference;
  if (std::fabs(rel) <= 8.5f) return 0.0f;
  for (float o : {12.0f, -12.0f, 19.0f, -19.0f, 24.0f, -24.0f})
    if (std::fabs(rel - o) < kRegisterTolerance) return o;
  return 0.0f;
}

constexpr int kShifts[] = {-12, 0, 12};

// Octave hypotheses for frames [first, last]: the decoded path, folded onto its
// dominant register, then moved -12 / 0 / +12. Each hypothesis is scored by the
// candidates' evidence over all voiced frames, so a whole note decides its octave.
struct Hypothesis {
  std::vector<float> contour, weight;  // per voiced frame with a supporting candidate
  double cost = 0;                     // evidence regret summed over frames
  float estimate = 0;                  // weighted median pitch (continuity prior)
};
struct SpanEvidence {
  std::vector<size_t> frames;  // voiced frames
  std::vector<bool> subharmonic;  // per voiced frame: an octave under the span's modal frames (see subharmonicFrames)
  float dominant = 0;          // the path's register
  double evidence = 0;         // level weight of the frames that carry octave evidence
  std::array<Hypothesis, 3> hypotheses;
  int best() const {
    int h = 1;
    for (int k = 0; k < 3; ++k)
      if (hypotheses[static_cast<size_t>(k)].cost < hypotheses[static_cast<size_t>(h)].cost) h = k;
    return h;
  }
};

// Frames of a span read an octave under the span's modal frames, whose own
// fundamental is far weaker than the voice's there, are subharmonic readings, not
// the sung pitch: period doubling with smoothly alternating pulses adds a weak
// f/2 partial, and a bass entering an octave under a held note does the same to
// the mixture, while a voice keeps its spectral tilt through a syllable (dev:
// modal H1-H2 -0.3..+5 dB, +18 in a soft release; the doubled frames' f/2
// partial 6-12 dB under f). An
// f/2 partial about as strong as f (fry, sharply alternating pulses) is a real low
// pitch to the ear and stays one. Modal = fully supported by the spectrum,
// uncontested by every octave alternative and sung (at least half level weight:
// a decaying tail is near-sinusoidal and says nothing about the voice's tilt),
// for at least 40 ms. A modal frame is never subharmonic itself.
//
// A syllable can be sung period-doubled throughout, with no modal frame of its
// own; the voice keeps its tilt from syllable to syllable of one word, so its
// frames are also judged against the modal frames of the word's other notes
// that lie an octave above the reading (within kRegisterTolerance: neighbouring
// syllables are a few semitones apart).
constexpr size_t kModalFrames = 5;
constexpr float kSubharmonicDb = 8;

// The decoded reading of voiced frame i is fully supported by the spectrum and
// uncontested by every octave alternative.
bool modalReading(const PitchAnalysis& a, size_t i) {
  const int k = a.chosen[i];
  if (k < 0) return false;
  const auto& cs = a.candidates[i];
  const auto& c = cs[static_cast<size_t>(k)];
  if (c.harmonic < -0.1f) return false;
  float margin = std::numeric_limits<float>::infinity();
  for (const auto& alt : cs)
    if (octaveRelated(std::fabs(alt.midi - c.midi))) margin = std::min(margin, alt.emission - c.emission);
  return margin >= 1.0f;
}

// Pitch and fundamental level (dB over the 2nd harmonic) of a sung modal frame.
struct ModalFrame { float midi, fundamental; };

// The modal frames of the other notes of a note's word: its word's pool, less
// the note's own [skipBegin, skipEnd). One pool per word, so a word of N notes
// costs memory linear in N (a long melisma is one word).
struct WordModal {
  const std::vector<ModalFrame>* pool = nullptr;
  size_t skipBegin = 0, skipEnd = 0;
};

std::vector<bool> subharmonicFrames(const PitchAnalysis& a, const std::vector<size_t>& frames, const std::vector<float>& values,
                                    const std::vector<float>& weights, float dominant, const WordModal& word) {
  std::vector<bool> out(frames.size(), false), isModal(frames.size(), false);
  std::array<std::vector<float>, 3> modal;  // fundamental (dB) of modal frames per register (kShifts)
  std::vector<int> reg(frames.size(), -1);
  for (size_t q = 0; q < frames.size(); ++q) {
    const float o = octaveOffset(values[q], dominant);
    for (int r = 0; r < 3; ++r)
      if (o == static_cast<float>(kShifts[r])) reg[q] = r;
    if (reg[q] < 0) continue;
    isModal[q] = modalReading(a, frames[q]);
    if (isModal[q] && weights[q] >= 0.5f) modal[static_cast<size_t>(reg[q])].push_back(a.candidates[frames[q]][static_cast<size_t>(a.chosen[frames[q]])].fundamental);
  }
  for (int r = 1; r < 3; ++r) {
    auto& m = modal[static_cast<size_t>(r)];
    if (m.size() < kModalFrames) continue;
    std::nth_element(m.begin(), m.begin() + static_cast<long>(m.size() / 2), m.end());
    const float voice = m[m.size() / 2];
    for (size_t q = 0; q < frames.size(); ++q) {
      const int k = a.chosen[frames[q]];
      if (reg[q] == r - 1 && !isModal[q] && k >= 0 && a.candidates[frames[q]][static_cast<size_t>(k)].fundamental < voice - kSubharmonicDb)
        out[q] = true;
    }
  }
  if (!word.pool) return out;
  std::vector<float> level;
  for (size_t q = 0; q < frames.size(); ++q) {
    const int k = a.chosen[frames[q]];
    if (out[q] || k < 0 || modalReading(a, frames[q])) continue;
    level.clear();
    for (size_t j = 0; j < word.pool->size(); ++j) {
      const ModalFrame& m = (*word.pool)[j];
      if ((j < word.skipBegin || j >= word.skipEnd) && std::fabs(m.midi - (values[q] + 12)) <= kRegisterTolerance)
        level.push_back(m.fundamental);
    }
    if (level.size() < kModalFrames) continue;
    std::nth_element(level.begin(), level.begin() + static_cast<long>(level.size() / 2), level.end());
    out[q] = a.candidates[frames[q]][static_cast<size_t>(k)].fundamental < level[level.size() / 2] - kSubharmonicDb;
  }
  return out;
}

// The sung modal frames of [first, last] (see subharmonicFrames).
std::vector<ModalFrame> modalFrames(const PitchAnalysis& a, size_t first, size_t last) {
  std::vector<ModalFrame> out;
  if (a.frames.empty() || first >= a.frames.size()) return out;
  last = std::min(last, a.frames.size() - 1);
  float peakDb = -120;
  for (size_t i = first; i <= last; ++i)
    if (a.frames[i].midi > 0) peakDb = std::max(peakDb, a.frames[i].db);
  for (size_t i = first; i <= last; ++i)
    if (a.frames[i].midi > 0 && levelWeight(a.frames[i].db, peakDb) >= 0.5f && modalReading(a, i))
      out.push_back({a.frames[i].midi, a.candidates[i][static_cast<size_t>(a.chosen[i])].fundamental});
  return out;
}

constexpr double kMissing = 3.0;  // octave regret of a frame with no candidate at the hypothesis (the most a frame gives)

// Two adjacent notes of one word this far apart are a leap, not a melodic step:
// in Jonathan's dev references no two adjacent notes of a word are 9 or more
// semitones apart (0 of 163), while a note read an octave off lands 12 +- 3
// from its neighbour. Other singers do leap that far inside a word, rarely
// (GTSinger dev 4 of 711 pairs, "riv-er" 69 -> 60; CSD dev 22 of 1081, up to
// the octave of "Some-where" 57 -> 69), so the leap is priced, not barred.
constexpr float kWordLeap = 9;
// What such a leap costs at most: the octave evidence of 80 ms of singing whose
// every frame rules the other octave out (10 frames x kMissing). It is scaled
// by how undecided the less decided of the two notes is (1 - decisiveness, see
// assignNotePitches): a note whose own frames hold their octave keeps it however
// short ("dog-gie" 55 -> 64 with a 40 ms "gie"), one whose frames fit both
// octaves follows its word.
constexpr double kWordLeapCost = 10 * kMissing;
// Decisiveness is a note's octave margin per frame of evidence, over at least
// 40 ms of evidence: one or two clear frames are not strong independent evidence.
constexpr double kDecisiveFrames = 5;
// A syllable the path voices nowhere takes the centre of its frames' best
// candidates, unless that lies more than an octave and a fifth from the take's
// median voiced pitch: then it has no pitch of its own (the import borrows a
// neighbour's). Every corpus has 16 such syllables (all splits); the one past
// this was 36 semitones off (GTSinger "I", 78 for 42), and an inhale's
// whistle in a compressed take gave a syllable 94 (A#6) over a 58 median.
constexpr float kUnvoicedReach = 19;

// `word`: the modal frames of the other notes of the span's word (see subharmonicFrames).
SpanEvidence spanEvidence(const PitchAnalysis& a, size_t first, size_t last, const WordModal& word = {}) {
  SpanEvidence e;
  if (a.frames.empty() || first >= a.frames.size()) return e;
  last = std::min(last, a.frames.size() - 1);
  float peakDb = -120;
  for (size_t i = first; i <= last; ++i)
    if (a.frames[i].midi > 0) e.frames.push_back(i), peakDb = std::max(peakDb, a.frames[i].db);
  if (e.frames.empty()) return e;
  std::vector<float> values, weights;
  for (size_t i : e.frames) {
    values.push_back(a.frames[i].midi);
    weights.push_back(levelWeight(a.frames[i].db, peakDb));
  }
  e.dominant = dominantPitch(values, weights);
  e.subharmonic = subharmonicFrames(a, e.frames, values, weights, e.dominant, word);
  // In an accompanied take, a frame whose spectrum holds a second harmonic series
  // cannot tell the voice's octave either: a bass an octave under the voice makes
  // the mixture periodic at its own period and supplies exactly the partials that
  // separate f from f/2. Such notes take their octave from single-series frames
  // and from their neighbours.
  std::vector<bool> silent(e.frames.size());
  double total = 0, evidence = 0;
  for (size_t q = 0; q < e.frames.size(); ++q) {
    const size_t i = e.frames[q];
    silent[q] = e.subharmonic[q] || (a.polyphony >= kAccompanied && i < a.polyphonic.size() && a.polyphonic[i]);
    total += weights[q];
    if (!silent[q]) evidence += weights[q];
  }
  for (size_t h = 0; h < 3; ++h) {
    auto& hyp = e.hypotheses[h];
    std::vector<std::pair<float, float>> forMedian;
    for (size_t q = 0; q < e.frames.size(); ++q) {
      const auto& cs = a.candidates[e.frames[q]];
      const float target = values[q] - octaveOffset(values[q], e.dominant) + static_cast<float>(kShifts[h]);
      float bestEmission = std::numeric_limits<float>::infinity();
      for (const auto& c : cs) bestEmission = std::min(bestEmission, c.emission);
      const int c = candidateNear(cs, target, 0.75f);
      if (e.subharmonic[q]) {
        // Under the octave above, its frequency is twice the reading's.
        if (std::fabs(values[q] + 12 - target) < 0.75f) {
          const float cmnd = cs[static_cast<size_t>(a.chosen[e.frames[q]])].cmnd;
          hyp.contour.push_back(values[q] + 12);
          hyp.weight.push_back(weights[q] * std::max(0.05f, 1 - cmnd));
          forMedian.push_back({values[q] + 12, weights[q]});
        }
        continue;
      }
      if (!silent[q])
        hyp.cost += weights[q] * (c >= 0 ? std::min(kMissing, static_cast<double>(cs[static_cast<size_t>(c)].emission - bestEmission)) : kMissing);
      if (c >= 0) {
        hyp.contour.push_back(cs[static_cast<size_t>(c)].midi);
        hyp.weight.push_back(weights[q] * std::max(0.05f, 1 - cs[static_cast<size_t>(c)].cmnd));
        forMedian.push_back({cs[static_cast<size_t>(c)].midi, weights[q]});
      }
    }
    hyp.estimate = forMedian.empty() ? e.dominant + static_cast<float>(kShifts[h]) : weightedPercentile(forMedian, 0.5);
  }
  // A lower component makes a mixture repeat at the lower period, so its
  // better periodicity cannot identify the singer. The converse is useful:
  // when the upper octave is both more periodic and better supported by the
  // spectrum, the lower reading is not explained merely by a strong bass.
  // Keep this evidence local to that octave pair; it says nothing about a
  // third hypothesis. Use the acoustic terms, before the frame register prior,
  // and require the same 40 ms support as a note's octave decisiveness.
  if (a.polyphony >= kAccompanied) {
    bool pairEvidence = false;
    for (size_t h = 0; h + 1 < 3; ++h) {
      double pairCost = 0, pairSupport = 0;
      for (size_t q = 0; q < e.frames.size(); ++q) {
        const size_t i = e.frames[q];
        if (e.subharmonic[q] || i >= a.polyphonic.size() || !a.polyphonic[i]) continue;
        const auto& cs = a.candidates[i];
        const float target = values[q] - octaveOffset(values[q], e.dominant) + static_cast<float>(kShifts[h]);
        const int lower = candidateNear(cs, target, 0.75f), upper = candidateNear(cs, target + 12, 0.75f);
        if (lower < 0 || upper < 0) continue;
        const auto& lo = cs[static_cast<size_t>(lower)];
        const auto& hi = cs[static_cast<size_t>(upper)];
        if (hi.cmnd > kStrongCmnd || hi.harmonic < -0.1f || hi.cmnd >= lo.cmnd || hi.harmonic <= lo.harmonic) continue;
        const double regret = kRelativePeriodicityWeight * (lo.cmnd - hi.cmnd) + hi.harmonic - lo.harmonic;
        pairCost += weights[q] * std::min(kMissing, regret);
        pairSupport += weights[q];
      }
      if (pairSupport >= kDecisiveFrames) {
        e.hypotheses[h].cost += pairCost;
        pairEvidence = true;
      }
    }
    // If a pair supplies evidence, an octave without sustained acoustic
    // support must not win an equal-cost tie over its supported member. One
    // weak candidate is insufficient; use the same support as above. Keep
    // other octaves available for stronger phrase-continuity evidence, with
    // the small margin used below for departing from the decoded path.
    if (pairEvidence) {
      for (size_t h = 0; h < 3; ++h) {
        double support = 0;
        for (size_t q = 0; q < e.frames.size(); ++q) {
          const auto& cs = a.candidates[e.frames[q]];
          const float target = values[q] - octaveOffset(values[q], e.dominant) + static_cast<float>(kShifts[h]);
          const int c = candidateNear(cs, target, 0.75f);
          if (c >= 0 && cs[static_cast<size_t>(c)].cmnd <= kStrongCmnd && cs[static_cast<size_t>(c)].harmonic >= -0.1f)
            support += weights[q];
        }
        if (support < kDecisiveFrames) e.hypotheses[h].cost += 0.1;
      }
    }
  }
  // Moving off the decoded path's octave needs evidence beyond a small margin,
  // scaled by the share of frames that carry octave evidence (the path's octave
  // came from the others too); with none, ties stay with the path.
  e.evidence = evidence;
  const double margin = std::max(0.1, evidence / total);
  e.hypotheses[0].cost += margin;
  e.hypotheses[2].cost += margin;
  return e;
}

}  // namespace

std::vector<float> vibratoCentreLine(const std::vector<float>& contour) {
  std::vector<float> out = contour;
  std::vector<float> v;
  std::vector<size_t> at;  // frame of each voiced value (unvoiced gaps are skipped, as by a held note's tracker dropouts)
  for (size_t i = 0; i < contour.size(); ++i)
    if (contour[i] > 0) v.push_back(contour[i]), at.push_back(i);
  for (const auto& c : vibratoRuns(v)) {
    // One cycle around each frame from half a cycle before the run's first
    // turning point to half a cycle after its last: the swings on both sides
    // of the mean cancel, so a held note is flat and a step still moves.
    const size_t half = static_cast<size_t>(std::lround(c.period / 2));
    const size_t a = c.first >= half ? c.first - half : 0, b = std::min(v.size(), c.last + half + 1);
    for (size_t i = a; i < b; ++i) {
      const size_t lo = i >= half ? i - half : 0, hi = std::min(v.size(), i + half + 1);
      double sum = 0;
      for (size_t j = lo; j < hi; ++j) sum += v[j];
      out[at[i]] = static_cast<float>(sum / static_cast<double>(hi - lo));
    }
  }
  return out;
}

std::vector<float> consistentContour(const PitchAnalysis& a, size_t first, size_t last) {
  std::vector<float> out;
  if (a.frames.empty() || first >= a.frames.size()) return out;
  last = std::min(last, a.frames.size() - 1);
  for (size_t i = first; i <= last; ++i) out.push_back(a.frames[i].midi);
  const SpanEvidence e = spanEvidence(a, first, last);
  if (e.frames.empty()) return out;
  // Registers: runs of voiced frames (unvoiced gaps ignored) at one octave offset
  // from the path's dominant pitch, with short runs between two runs of the same
  // register merged into them. A register held 300 ms or more is real; anything
  // shorter that leaves and returns is a tracker excursion (a real 12-semitone
  // glide alone takes ~120 ms or more, see glideCost).
  struct Run { size_t begin, end; float offset; };  // indices into out, [begin, end)
  std::vector<Run> runs;
  std::vector<float> offset(out.size(), 0.0f);
  for (size_t k = 0; k < out.size(); ++k) {
    if (out[k] <= 0) continue;
    offset[k] = octaveOffset(out[k], e.dominant);
    if (!runs.empty() && runs.back().offset == offset[k]) runs.back().end = k + 1;
    else runs.push_back({k, k + 1, offset[k]});
  }
  auto isLong = [](const Run& r) { return static_cast<double>(r.end - r.begin) * kAnalysisHopSeconds >= 0.3; };
  for (size_t q = 1; q + 1 < runs.size();) {
    if (!isLong(runs[q]) && runs[q - 1].offset == runs[q + 1].offset) {
      runs[q - 1].end = runs[q + 1].end;
      runs.erase(runs.begin() + static_cast<long>(q), runs.begin() + static_cast<long>(q) + 2);
      q = 1;
    } else {
      ++q;
    }
  }
  const Run* held = nullptr;  // the first long run
  bool oneRegister = true;
  for (const auto& r : runs) {
    if (!isLong(r)) continue;
    if (!held) held = &r;
    else if (r.offset != held->offset) oneRegister = false;
  }
  // The register each frame should be in, as an offset from the dominant pitch.
  std::vector<float> target(out.size(), 0.0f);
  if (oneRegister) {
    // One held register: its octave is decided from the evidence of all frames
    // (as for a note); every frame off it is relabelled.
    std::fill(target.begin(), target.end(), static_cast<float>(kShifts[e.best()]));
  } else {
    // A real leap inside the span: each register keeps its octave; short runs
    // between registers take the nearest long one's (the earlier on a tie).
    for (const auto& r : runs) {
      const Run* nearest = &r;
      if (!isLong(r)) {
        size_t distance = SIZE_MAX;
        for (const auto& l : runs) {
          if (!isLong(l)) continue;
          const size_t d = l.end <= r.begin ? r.begin - l.end : l.begin - r.end;
          if (d < distance) distance = d, nearest = &l;
        }
      }
      std::fill(target.begin() + static_cast<long>(r.begin), target.begin() + static_cast<long>(r.end), nearest->offset);
    }
  }
  std::vector<bool> subharmonic(out.size(), false);
  for (size_t q = 0; q < e.frames.size(); ++q) subharmonic[e.frames[q] - first] = e.subharmonic[q];
  for (size_t k = 0; k < out.size(); ++k) {
    if (out[k] <= 0 || offset[k] == target[k]) continue;
    if (subharmonic[k] && target[k] == offset[k] + 12) {
      out[k] += 12;  // twice the subharmonic reading
      continue;
    }
    const auto& cs = a.candidates[first + k];
    const int c = candidateNear(cs, out[k] - offset[k] + target[k], 0.75f);
    out[k] = c >= 0 && cs[static_cast<size_t>(c)].cmnd <= kCandidateCmnd ? cs[static_cast<size_t>(c)].midi : 0.0f;
  }
  return out;
}

void assignNotePitches(const PitchAnalysis& a, std::vector<NoteSpan>& notes) {
  // The notes of each word (NoteSpan::word), in input order.
  std::vector<std::vector<size_t>> words;
  std::vector<long long> wordOf(notes.size(), -1);
  {
    std::vector<std::pair<long long, size_t>> byWord;
    for (size_t n = 0; n < notes.size(); ++n)
      if (notes[n].word >= 0) byWord.push_back({notes[n].word, n});
    std::sort(byWord.begin(), byWord.end());
    for (size_t k = 0; k < byWord.size(); ++k) {
      if (k == 0 || byWord[k].first != byWord[k - 1].first) words.emplace_back();
      words.back().push_back(byWord[k].second);
      wordOf[byWord[k].second] = static_cast<long long>(words.size()) - 1;
    }
  }
  auto sameWord = [&](size_t p, size_t n) { return wordOf[p] >= 0 && wordOf[p] == wordOf[n]; };
  // Each note's frames are judged against the modal frames of the rest of its
  // word too: the word's notes' modal frames in one pool, each note skipping its own.
  std::vector<std::vector<ModalFrame>> pools(words.size());
  std::vector<WordModal> rest(notes.size());
  for (size_t k = 0; k < words.size(); ++k) {
    if (words[k].size() < 2) continue;
    for (size_t n : words[k]) {
      const std::vector<ModalFrame> own = modalFrames(a, notes[n].first, notes[n].last);
      rest[n] = {&pools[k], pools[k].size(), pools[k].size() + own.size()};
      pools[k].insert(pools[k].end(), own.begin(), own.end());
    }
  }
  std::vector<SpanEvidence> evidence(notes.size());
  std::vector<bool> hasEvidence(notes.size(), false);
  for (size_t n = 0; n < notes.size(); ++n) {
    auto& note = notes[n];
    note.center = 0, note.spread = 0, note.confidence = 0, note.octaveShift = 0;
    evidence[n] = spanEvidence(a, note.first, note.last, rest[n]);
    hasEvidence[n] = !evidence[n].frames.empty();
  }
  auto hyps = [&](size_t n) -> const std::array<Hypothesis, 3>& { return evidence[n].hypotheses; };

  // Continuity across notes of a phrase: an octave-sized move between adjacent
  // notes (gap <= 0.3 s) costs extra, so it survives only with clear evidence.
  // Inside one word a leap of kWordLeap or more also costs up to the evidence of
  // a clearly decided syllable (kWordLeapCost), scaled by how undecided the less
  // decided note is: the word takes the register its frames favour together
  // unless each side of the leap holds its own octave firmly. A note's hold is
  // its octave margin (second-best hypothesis over the best) per frame of
  // evidence, as a share of the most a frame gives (kMissing).
  std::vector<double> hold(notes.size(), 0.0);
  for (size_t n = 0; n < notes.size(); ++n) {
    if (!hasEvidence[n] || evidence[n].evidence <= 0) continue;
    std::array<double, 3> c{};
    for (size_t h = 0; h < 3; ++h) c[h] = hyps(n)[h].cost;
    std::sort(c.begin(), c.end());
    hold[n] = std::clamp((c[1] - c[0]) / (kMissing * std::max(evidence[n].evidence, kDecisiveFrames)), 0.0, 1.0);
  }
  auto leapCost = [&](float from, float to, bool inWord, size_t p, size_t n) {
    const double d = std::fabs(to - from);
    const double word = inWord && d >= kWordLeap ? kWordLeapCost * (1 - std::min(hold[p], hold[n])) : 0.0;
    return (d < 10 ? 0.0 : 3.0 + 0.5 * (d - 10)) + word;
  };
  std::vector<size_t> order;
  for (size_t n = 0; n < notes.size(); ++n)
    if (hasEvidence[n]) order.push_back(n);
  std::stable_sort(order.begin(), order.end(), [&](size_t p, size_t q) { return notes[p].first < notes[q].first; });
  const size_t maxGap = static_cast<size_t>(0.3 / kAnalysisHopSeconds);
  std::vector<std::array<double, 3>> total(notes.size());
  std::vector<std::array<int, 3>> from(notes.size());
  std::vector<long long> chain(notes.size(), -1);  // previous note of the same phrase
  for (size_t q = 0; q < order.size(); ++q) {
    const size_t n = order[q];
    for (size_t h = 0; h < 3; ++h) total[n][h] = hyps(n)[h].cost, from[n][h] = -1;
    if (q == 0) continue;
    const size_t p = order[q - 1];
    const bool inWord = sameWord(p, n);  // a word's notes are decided together across any gap
    if (!inWord && notes[n].first > notes[p].last + maxGap) continue;
    chain[n] = static_cast<long long>(p);
    for (size_t h = 0; h < 3; ++h) {
      double best = kInf;
      for (size_t g = 0; g < 3; ++g) {
        const double v = total[p][g] + leapCost(hyps(p)[g].estimate, hyps(n)[h].estimate, inWord, p, n);
        if (v < best) best = v, from[n][h] = static_cast<int>(g);
      }
      total[n][h] += best;
    }
  }
  std::vector<int> pick(notes.size(), -1);
  for (size_t q = order.size(); q-- > 0;) {
    long long n = static_cast<long long>(order[q]);
    if (pick[static_cast<size_t>(n)] >= 0) continue;  // decided from a later note of its phrase
    const auto& t = total[static_cast<size_t>(n)];
    int h = static_cast<int>(std::min_element(t.begin(), t.end()) - t.begin());
    while (n >= 0) {
      pick[static_cast<size_t>(n)] = h;
      h = from[static_cast<size_t>(n)][static_cast<size_t>(h)];
      n = h >= 0 ? chain[static_cast<size_t>(n)] : -1;
    }
  }

  float takeMedian = -1;  // median voiced pitch, computed when a note needs it (kUnvoicedReach)
  for (size_t n = 0; n < notes.size(); ++n) {
    if (!hasEvidence[n]) {
      // The path voices no frame of this note (a short syllable whose full-band
      // periodicity is too rough for the voicing decision): the centre of the
      // best candidate of each periodic, audible frame, with low confidence.
      auto& note = notes[n];
      if (note.first >= a.frames.size()) continue;
      const size_t last = std::min(note.last, a.frames.size() - 1);
      float peakDb = -200;
      for (size_t i = note.first; i <= last; ++i) peakDb = std::max(peakDb, a.frames[i].db);
      const float quietest = std::max(kSilenceDb, a.floorDb - kQuietRange);  // the lowest silence floor of any section
      std::vector<float> values, weights;
      double clarity = 0;
      for (size_t i = note.first; i <= last; ++i) {
        const PitchCandidateV2* best = nullptr;
        for (const auto& c : a.candidates[i])
          if (!best || c.emission < best->emission) best = &c;
        if (!best || best->cmnd > kStrongCmnd || a.frames[i].db < peakDb - 30 || a.frames[i].db <= quietest) continue;
        values.push_back(best->midi);
        weights.push_back(levelWeight(a.frames[i].db, peakDb) * (1 - best->cmnd));
        clarity += 1 - best->cmnd;
      }
      if (values.size() < 3) continue;  // 24 ms
      float spread = 0;
      const float centre = robustCentre(values, weights, spread);
      if (takeMedian < 0) {
        std::vector<float> voiced;
        for (const auto& f : a.frames)
          if (f.midi > 0) voiced.push_back(f.midi);
        takeMedian = 0;
        if (!voiced.empty()) {
          std::nth_element(voiced.begin(), voiced.begin() + static_cast<long>(voiced.size() / 2), voiced.end());
          takeMedian = voiced[voiced.size() / 2];
        }
      }
      if (takeMedian > 0 && std::fabs(centre - takeMedian) > kUnvoicedReach) continue;
      note.center = centre;
      note.spread = spread;
      note.confidence = static_cast<float>(0.3 * clarity / static_cast<double>(values.size()));
      continue;
    }
    auto& note = notes[n];
    const auto& hyp = hyps(n)[static_cast<size_t>(pick[n])];
    if (hyp.contour.empty()) {
      // No candidate supports the chosen octave anywhere (it was decided by its
      // neighbours): the path's register moved to that octave.
      note.center = evidence[n].dominant + static_cast<float>(kShifts[pick[n]]);
      note.octaveShift = kShifts[pick[n]];
      note.confidence = 0.2f;
      continue;
    }
    note.center = robustCentre(hyp.contour, hyp.weight, note.spread);
    // Diagnostics: shift relative to the raw path's median.
    std::vector<std::pair<float, float>> raw;
    float clarity = 0;
    size_t voiced = 0;
    const size_t last = std::min(note.last, a.frames.size() - 1);
    for (size_t i = note.first; i <= last; ++i)
      if (a.frames[i].midi > 0) raw.push_back({a.frames[i].midi, 1.0f}), clarity += a.frames[i].confidence, ++voiced;
    note.octaveShift = static_cast<int>(std::lround((note.center - weightedPercentile(raw, 0.5)) / 12.0)) * 12;
    double second = kInf;
    for (int h = 0; h < 3; ++h)
      if (h != pick[n]) second = std::min(second, hyps(n)[static_cast<size_t>(h)].cost);
    const double decisive = std::clamp((second - hyp.cost) / (2.0 + 0.05 * static_cast<double>(voiced)), 0.0, 1.0);
    const float voicedShare = static_cast<float>(voiced) / static_cast<float>(last - note.first + 1);
    note.confidence = (clarity / static_cast<float>(std::max<size_t>(1, voiced))) * (0.5f + 0.5f * voicedShare) *
                      static_cast<float>(0.7 + 0.3 * decisive);
  }

  // A note the path never voices has no octave evidence of its own (its best
  // candidates are rough frames' readings): inside a word it takes the octave of
  // the word's nearest voiced note when its reading is that note's register
  // doubled, halved or tripled (octaveOffset: 12, 19 or 24 st, as the frames'
  // own octave alternatives); another interval is a sung one and stays.
  for (const auto& w : words)
    for (size_t n : w) {
      if (hasEvidence[n] || notes[n].center <= 0) continue;
      const NoteSpan* nearest = nullptr;
      size_t distance = SIZE_MAX;
      for (size_t m : w) {
        if (!hasEvidence[m] || notes[m].center <= 0) continue;
        const size_t d = notes[m].first > notes[n].last ? notes[m].first - notes[n].last : notes[n].first > notes[m].last ? notes[n].first - notes[m].last : 0;
        if (d < distance) distance = d, nearest = &notes[m];
      }
      if (!nearest || std::fabs(nearest->center - notes[n].center) < kWordLeap) continue;
      const int shift = -static_cast<int>(octaveOffset(notes[n].center, nearest->center));
      if (shift == 0) continue;
      notes[n].center += static_cast<float>(shift);
      notes[n].octaveShift += shift;
    }
}

TuningEstimate estimateTuning(const std::vector<float>& centres, const std::vector<float>& weights, float polyphony) {
  // Fractional pitch lives on a circle (59.6 and 60.6 sit equally far from
  // the grid), so the offset is a circular mean of the centres' phases.
  constexpr double kTwoPi = 6.283185307179586;
  TuningEstimate t;
  const auto mean = [&](const std::vector<double>& w, double& resultant) {
    double c = 0, s = 0, sum = 0;
    for (size_t i = 0; i < centres.size(); ++i) {
      c += w[i] * std::cos(kTwoPi * centres[i]);
      s += w[i] * std::sin(kTwoPi * centres[i]);
      sum += w[i];
    }
    resultant = sum > 0 ? std::hypot(c, s) / sum : 0;
    return sum > 0 ? std::atan2(s, c) / kTwoPi : 0.0;
  };
  std::vector<double> w(centres.size(), 0);
  double sum = 0, sumSq = 0;
  for (size_t i = 0; i < centres.size() && i < weights.size(); ++i) {
    if (!(centres[i] > 0) || !(weights[i] > 0) || !std::isfinite(centres[i]) || !std::isfinite(weights[i])) continue;
    w[i] = weights[i];
    sum += w[i];
    sumSq += w[i] * w[i];
  }
  if (sum <= 0) return t;
  double resultant = 0;
  double offset = mean(w, resultant);
  t.strength = static_cast<float>(sum * sum / sumSq * resultant * resultant);
  // Mode-seeking refinement: notes far from the current offset (a few sung out
  // of tune, glides the note centre could not settle) stop pulling on it.
  for (int iteration = 0; iteration < kTuningIterations; ++iteration) {
    std::vector<double> robust(w.size(), 0);
    for (size_t i = 0; i < w.size(); ++i) {
      double r = centres[i] - offset;
      r -= std::round(r);
      const double u = r / kTuningWindow;
      robust[i] = std::fabs(u) < 1 ? w[i] * (1 - u * u) * (1 - u * u) : 0;
    }
    double unused = 0;
    offset = mean(robust, unused);
  }
  t.measured = static_cast<float>(offset);
  if (t.strength >= kTuningMinStrength && sum * sum / sumSq >= kTuningMinNotes && std::fabs(offset) >= kTuningDeadZone &&
      std::fabs(offset) <= kTuningAmbiguous && polyphony < kTuningMaxPolyphony)
    t.offset = t.measured;
  return t;
}

}  // namespace novox
