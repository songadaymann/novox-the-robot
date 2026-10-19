#include "articulation.hpp"

#include <algorithm>
#include <cmath>

namespace novox {

namespace {

constexpr double kPi = 3.14159265358979323846;  // not M_PI: MSVC defines that only on request
constexpr int kFft = 512;          // 32 ms analysis window
constexpr int kMelBands = 26;
constexpr int kCepstra = 12;       // c1..c12
constexpr int kChangeSpan = 6;     // frames each side for the spectral change (48 ms)
constexpr float kLevelRange = 90;  // dB under loud singing where band levels are floored
// Level references (levelReferences).
constexpr long kLoudHalf = 188;        // frames (1.5 s) each side: a phrase or two
constexpr size_t kLoudMinVoiced = 25;  // voiced frames (200 ms) that define a level
constexpr float kLoudRange = 30;       // dB: no section counts as quieter than this under the take
constexpr float kNearbyRange = 12;     // dB: nor, for what is negligible next to it, its surroundings
constexpr long kPauseFrames = 31;      // >= 248 ms without voiced sound ends a phrase (as the segmenter's)
// Weak frames (weakFrames): CMND and harmonic evidence as in singing_pitch.cpp.
constexpr float kWeakCmnd = 0.25f, kWeakHarmonic = -0.5f, kWeakBelowLoud = 15;  // dB
constexpr size_t kWeakRun = 5, kWeakApart = 6;  // frames

float percentileOf(std::vector<float> v, double p) {
  if (v.empty()) return 0;
  const size_t k = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
  return v[k];
}

std::vector<float> smooth3(const std::vector<float>& v) {
  std::vector<float> out(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    const size_t a = i ? i - 1 : 0, b = std::min(v.size() - 1, i + 1);
    out[i] = (v[a] + v[i] + v[b]) / 3;
  }
  return out;
}

double hzToMel(double hz) { return 2595 * std::log10(1 + hz / 700); }
double melToHz(double mel) { return 700 * (std::pow(10, mel / 2595) - 1); }

// Vibrato (and trills): normalised autocorrelation of the detrended pitch
// contour at 4-10 Hz periods over +-250 ms, where the contour is voiced and
// moves by at least ~0.2 st. Spectral change and level dips at the vibrato
// rate are not syllable onsets.
std::vector<float> vibratoStrength(const PitchAnalysis& pitch, size_t n) {
  std::vector<float> out(n, 0.0f);
  std::vector<float> c(n, 0.0f);
  std::vector<char> v(n, 0);
  for (size_t k = 0; k < n; ++k) {
    c[k] = pitch.frames[k].midi;
    v[k] = c[k] > 0;
  }
  // Fold octave flips onto the running register so they do not look like modulation.
  float ref = 0;
  for (size_t k = 0; k < n; ++k) {
    if (!v[k]) continue;
    if (ref > 0) c[k] -= 12 * std::round((c[k] - ref) / 12);
    ref = ref > 0 ? 0.9f * ref + 0.1f * c[k] : c[k];
  }
  const long half = 31, trend = 11, minLag = 12, maxLag = 31;  // frames
  std::vector<float> d(n, 0.0f);  // contour minus its ~180 ms moving average
  for (size_t k = 0; k < n; ++k) {
    if (!v[k]) continue;
    double sum = 0;
    int count = 0;
    for (long j = static_cast<long>(k) - trend; j <= static_cast<long>(k) + trend; ++j)
      if (j >= 0 && j < static_cast<long>(n) && v[static_cast<size_t>(j)]) sum += c[static_cast<size_t>(j)], ++count;
    d[k] = static_cast<float>(c[k] - sum / count);
  }
  for (size_t k = 0; k < n; ++k) {
    if (!v[k]) continue;
    const long a = std::max(0L, static_cast<long>(k) - half), b = std::min(static_cast<long>(n) - 1, static_cast<long>(k) + half);
    int voiced = 0;
    double energy = 0;
    for (long j = a; j <= b; ++j)
      if (v[static_cast<size_t>(j)]) ++voiced, energy += d[static_cast<size_t>(j)] * d[static_cast<size_t>(j)];
    if (voiced < half || energy / voiced < 0.01) continue;  // RMS under 0.1 st: steady
    float best = 0;
    for (long lag = minLag; lag <= maxLag; ++lag) {
      double num = 0, e1 = 0, e2 = 0;
      for (long j = a; j + lag <= b; ++j) {
        const size_t x = static_cast<size_t>(j), y = static_cast<size_t>(j + lag);
        if (!v[x] || !v[y]) continue;
        num += d[x] * d[y], e1 += d[x] * d[x], e2 += d[y] * d[y];
      }
      if (e1 > 0 && e2 > 0) best = std::max(best, static_cast<float>(num / std::sqrt(e1 * e2)));
    }
    out[k] = std::clamp(best, 0.0f, 1.0f);
  }
  return out;
}

// Level references: loud singing is measured around each frame rather than
// over the take, so a quiet verse is judged against itself and not against the
// loudest chorus. `loud` is a high percentile of the voiced frames of the
// frame's phrase (voiced sound without a pause of kPauseFrames) within
// +-kLoudHalf, or of all of them there when the phrase has too little voiced
// sound to define a level (an isolated creak); between phrases the nearest
// phrase's; never more than kLoudRange under the take's. `nearbyLoud` is the
// level over every voiced frame within +-kLoudHalf, never more than
// kNearbyRange under the take's, so sound far under the take's singing in a
// long rest (bleed, a creak) stays negligible.
void levelReferences(ArticulationFeatures& f, const PitchAnalysis& pitch) {
  const long n = static_cast<long>(f.frames);
  auto at = [](long k) { return static_cast<size_t>(k); };
  std::vector<char> sound(at(n));
  for (long k = 0; k < n; ++k) sound[at(k)] = pitch.frames[at(k)].midi > 0 && f.total[at(k)] > f.floorDb + 6;
  std::vector<int> phrase(at(n), -1);  // from the first to the last sound of each phrase
  int count = -1;
  for (long k = 0, last = -1; k < n; ++k) {
    if (!sound[at(k)]) continue;
    if (last < 0 || k - last > kPauseFrames) ++count;
    else std::fill(phrase.begin() + last + 1, phrase.begin() + k, count);
    phrase[at(k)] = count;
    last = k;
  }
  f.loud.assign(at(n), f.loudDb);
  f.nearbyLoud.assign(at(n), f.loudDb);
  std::vector<float> all, own;
  for (long k = 0; k < n; ++k) {
    all.clear();
    own.clear();
    for (long j = std::max(0L, k - kLoudHalf); j <= std::min(n - 1, k + kLoudHalf); ++j) {
      if (!sound[at(j)]) continue;
      all.push_back(f.total[at(j)]);
      if (phrase[at(j)] == phrase[at(k)]) own.push_back(f.total[at(j)]);
    }
    const float nearby = all.size() >= kLoudMinVoiced ? percentileOf(all, 0.95) : f.loudDb;
    f.nearbyLoud[at(k)] = std::max(nearby, f.loudDb - kNearbyRange);
    f.loud[at(k)] = std::max(phrase[at(k)] >= 0 && own.size() >= kLoudMinVoiced ? percentileOf(own, 0.95) : nearby, f.loudDb - kLoudRange);
  }
  // Between phrases (an onset just before its voicing, a short syllable the
  // path left unvoiced) the nearest phrase within kLoudHalf is the reference.
  for (long k = 0, previous = -1; k < n; ++k) {
    if (phrase[at(k)] >= 0) {
      previous = k;
      continue;
    }
    long next = k + 1;
    while (next < n && next - k <= kLoudHalf && phrase[at(next)] < 0) ++next;
    const bool hasNext = next < n && next - k <= kLoudHalf;
    const bool hasPrevious = previous >= 0 && k - previous <= kLoudHalf;
    if (hasNext && (!hasPrevious || next - k <= k - previous)) f.loud[at(k)] = f.loud[at(next)];
    else if (hasPrevious) f.loud[at(k)] = f.loud[at(previous)];
  }
}

// Sung frames the pitch path leaves unvoiced: loud for their phrase, vowel-like,
// and periodic in at least one band with partials that agree (a rough or pressed
// short syllable can fail full-band voicing while its low band repeats cleanly;
// bleed from an instrument under the voice rarely has a consistent series).
// Runs under kWeakRun frames are ignored, and so are runs next to voiced frames:
// those are voicing dropouts of a sound the segmenter already sees.
std::vector<char> weakFrames(const ArticulationFeatures& f, const PitchAnalysis& pitch) {
  const size_t n = f.frames;
  std::vector<char> weak(n, 0);
  for (size_t k = 0; k < n; ++k) {
    if (pitch.frames[k].midi > 0 || f.total[k] < f.loud[k] - kWeakBelowLoud || f.mid[k] < f.high[k] + 6) continue;
    for (const auto& c : pitch.candidates[k]) weak[k] = weak[k] || (c.cmnd <= kWeakCmnd && c.harmonic >= kWeakHarmonic);
  }
  for (size_t k = 0; k < n;) {
    size_t e = k;
    while (e < n && weak[e]) ++e;
    bool apart = true;
    for (size_t j = k >= kWeakApart ? k - kWeakApart : 0; j < std::min(n, e + kWeakApart); ++j) apart = apart && !f.voiced[j];
    if (e - k < kWeakRun || !apart) std::fill(weak.begin() + static_cast<long>(k), weak.begin() + static_cast<long>(e), 0);
    k = e + 1;
  }
  return weak;
}

}  // namespace

ArticulationFeatures analyzeArticulation(const std::vector<float>& x16k, const PitchAnalysis& pitch) {
  ArticulationFeatures f;
  const size_t n = std::min(analysisFrameCount(x16k.size()), pitch.frames.size());
  f.frames = n;
  if (n == 0) return f;
  const Spectra s = computeSpectra(x16k, kFft);
  std::vector<float> low(n), mid(n), high(n), total(n);
  for (size_t k = 0; k < n; ++k) {
    low[k] = s.bandDb(k, 80, 400);
    mid[k] = s.bandDb(k, 640, 2800);
    high[k] = s.bandDb(k, 3500, 7800);
    total[k] = s.bandDb(k, 80, 7800);
  }
  // Levels are floored kLevelRange under the take's singing rather than at a
  // fixed dBFS, so the same take at another gain gives the same features (and
  // digital silence no dips deeper than any real pause).
  std::vector<float> sung;
  for (size_t k = 0; k < n; ++k)
    if (pitch.frames[k].midi > 0) sung.push_back(total[k]);
  const float levelFloor = (sung.empty() ? percentileOf(total, 0.98) : percentileOf(sung, 0.95)) - kLevelRange;
  for (auto* band : {&low, &mid, &high, &total})
    for (float& v : *band) v = std::max(v, levelFloor);
  f.low = smooth3(low);
  f.mid = smooth3(mid);
  f.high = smooth3(high);
  f.total = smooth3(total);

  std::vector<float> voicedLevels;
  for (size_t k = 0; k < n; ++k)
    if (pitch.frames[k].midi > 0) voicedLevels.push_back(f.total[k]);
  f.loudDb = voicedLevels.empty() ? percentileOf(f.total, 0.98) : percentileOf(voicedLevels, 0.95);
  // A take without any pause has no background to measure: keep the floor well below the singing.
  f.floorDb = std::min(percentileOf(f.total, 0.05), f.loudDb - 40);
  levelReferences(f, pitch);
  f.voiced.assign(n, 0);
  for (size_t k = 0; k < n; ++k)
    f.voiced[k] = pitch.frames[k].midi > 0 && f.total[k] > std::max(f.floorDb + 6, f.loud[k] - 50);

  // MFCCs (26 mel bands, c1..c12) z-normalised over active frames, then the
  // distance between the mean cepstra of the 48 ms after and before each frame.
  std::vector<std::vector<std::pair<int, float>>> mel(kMelBands);
  const double lo = hzToMel(80), hi = hzToMel(7800);
  for (int m = 0; m < kMelBands; ++m) {
    const double a = melToHz(lo + (hi - lo) * m / (kMelBands + 1));
    const double c = melToHz(lo + (hi - lo) * (m + 1) / (kMelBands + 1));
    const double b = melToHz(lo + (hi - lo) * (m + 2) / (kMelBands + 1));
    for (int bin = static_cast<int>(a / s.binHz); bin <= static_cast<int>(b / s.binHz) + 1 && bin < s.bins; ++bin) {
      const double hz = bin * s.binHz;
      const double w = hz < c ? (hz - a) / (c - a) : (b - hz) / (b - c);
      if (w > 0) mel[static_cast<size_t>(m)].push_back({bin, static_cast<float>(w)});
    }
  }
  // The level floor as power per bin of the unnormalised spectrum (see Spectra::bandDb).
  const double melFloor = std::pow(10.0, levelFloor / 10.0) * kFft * kFft / 4.0;
  std::vector<float> cep(n * kCepstra);
  std::vector<double> logMel(kMelBands);
  for (size_t k = 0; k < n; ++k) {
    const float* m = s.frame(k);
    for (int b = 0; b < kMelBands; ++b) {
      double e = 0;
      for (const auto& [bin, w] : mel[static_cast<size_t>(b)]) e += w * static_cast<double>(m[bin]) * m[bin];
      logMel[static_cast<size_t>(b)] = 10 * std::log10(e + melFloor);
    }
    for (int j = 1; j <= kCepstra; ++j) {
      double c = 0;
      for (int b = 0; b < kMelBands; ++b) c += logMel[static_cast<size_t>(b)] * std::cos(kPi * j * (b + 0.5) / kMelBands);
      cep[k * kCepstra + static_cast<size_t>(j - 1)] = static_cast<float>(c);
    }
  }
  std::vector<float> activeDb(n);
  for (size_t k = 0; k < n; ++k) activeDb[k] = std::max(f.floorDb + 10, f.loud[k] - 40);
  for (int j = 0; j < kCepstra; ++j) {
    double sum = 0, sq = 0;
    size_t count = 0;
    for (size_t k = 0; k < n; ++k) {
      if (f.total[k] < activeDb[k]) continue;
      const double v = cep[k * kCepstra + static_cast<size_t>(j)];
      sum += v;
      sq += v * v;
      ++count;
    }
    const double mean = count ? sum / static_cast<double>(count) : 0;
    const double sd = count > 1 ? std::sqrt(std::max(1e-9, sq / static_cast<double>(count) - mean * mean)) : 1;
    for (size_t k = 0; k < n; ++k) {
      float& v = cep[k * kCepstra + static_cast<size_t>(j)];
      v = static_cast<float>((v - mean) / sd);
    }
  }
  std::vector<double> prefix((n + 1) * kCepstra, 0.0);
  for (size_t k = 0; k < n; ++k)
    for (int j = 0; j < kCepstra; ++j)
      prefix[(k + 1) * kCepstra + static_cast<size_t>(j)] =
          prefix[k * kCepstra + static_cast<size_t>(j)] + cep[k * kCepstra + static_cast<size_t>(j)];
  f.change.assign(n, 0.0f);
  for (size_t k = kChangeSpan; k + kChangeSpan <= n; ++k) {
    double d = 0;
    for (int j = 0; j < kCepstra; ++j) {
      const size_t jj = static_cast<size_t>(j);
      const double after = (prefix[(k + kChangeSpan) * kCepstra + jj] - prefix[k * kCepstra + jj]) / kChangeSpan;
      const double before = (prefix[k * kCepstra + jj] - prefix[(k - kChangeSpan) * kCepstra + jj]) / kChangeSpan;
      d += (after - before) * (after - before);
    }
    f.change[k] = static_cast<float>(std::sqrt(d / kCepstra));
  }
  f.vibrato = vibratoStrength(pitch, n);
  f.weak = weakFrames(f, pitch);
  return f;
}

namespace {

// Logistic evidence models over clipped, scaled measurements, fitted on the
// dev split of reference/corpus (vowel onsets of audited annotations against
// candidates more than 48 ms from any onset). Order: gap, frication, dipMid,
// dipHigh, change, rise, pitchStep, level, bias.
constexpr int kTerms = 8;
// Inside vibrato (periodicity above the gate), onsets without a voicing break
// or a deep vowel-band dip are scaled down (Kroher & Gomez: vibrato-rate level
// changes are not onsets). Mostly matters without lyrics.
constexpr float kVibratoGate = 0.7f, kVibratoScale = 0.25f;
struct Term { float lo, hi; };
constexpr Term kClip[kTerms] = {{0, 0.12f}, {0, 30}, {0, 25}, {0, 30}, {0, 2}, {0, 30}, {0, 3}, {-40, 0}};
constexpr float kAny[kTerms + 1] = {-1.390f, 2.078f, 1.133f, 0.687f, 7.176f, 6.702f, 1.472f, 5.572f, -4.978f};
constexpr float kKind[4][kTerms + 1] = {
    {-0.859f, 2.829f, 1.986f, 0.165f, 6.682f, 3.679f, 0.790f, 5.443f, -5.791f},   // unvoiced onset
    {-2.073f, 1.974f, 1.933f, 0.690f, 6.157f, 5.139f, 1.458f, 5.859f, -5.925f},   // voiced obstruent
    {-2.799f, 0.932f, 0.831f, 0.872f, 7.587f, 5.991f, 1.930f, 6.168f, -5.939f},   // sonorant
    {1.096f, -1.163f, 0.259f, -1.042f, 3.693f, 5.040f, 1.446f, 3.368f, -5.391f},  // vowel-initial
};

float logistic(const float* w, const float* x) {
  double z = w[kTerms];
  for (int i = 0; i < kTerms; ++i) z += w[i] * x[i];
  return static_cast<float>(1 / (1 + std::exp(-z)));
}

float foldOctave(float d) { return d - 12 * std::round(d / 12); }

float medianVoiced(const PitchAnalysis& pitch, long from, long to) {
  std::vector<float> v;
  for (long k = std::max(0L, from); k <= to && k < static_cast<long>(pitch.frames.size()); ++k)
    if (pitch.frames[static_cast<size_t>(k)].midi > 0) v.push_back(pitch.frames[static_cast<size_t>(k)].midi);
  if (v.size() < 4) return 0;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

float slopeAt(const ArticulationFeatures& f, size_t k) {
  return k >= 1 && k + 1 < f.frames ? f.mid[k + 1] - f.mid[k - 1] : 0;
}

}  // namespace

size_t vowelOnsetBefore(const ArticulationFeatures& f, size_t frame) {
  // The pitch tracker often voices 50-200 ms after the vowel is audible. Walk
  // back while the vowel band stays near the vowel's level, then take the
  // steepest vowel-band rise there (where annotators put the onset).
  if (frame >= f.frames) return frame;
  std::vector<float> v;
  for (size_t k = frame; k < std::min(f.frames, frame + 10); ++k) v.push_back(f.mid[k]);
  std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
  const float vowelMid = v[v.size() / 2];
  size_t from = frame;
  while (from > 1 && frame - from < 25 && f.mid[from - 1] >= vowelMid - 6) --from;
  from = from > 3 ? from - 3 : 1;
  size_t best = frame;
  float bestSlope = 4;  // dB over two frames; below this keep the voicing onset
  for (size_t k = from; k <= frame + 2 && k + 1 < f.frames; ++k) {
    const float s = slopeAt(f, k);
    if (s > bestSlope) bestSlope = s, best = k;
  }
  return best;
}

std::vector<BoundaryCandidate> findBoundaryCandidates(const ArticulationFeatures& f, const PitchAnalysis& pitch) {
  const size_t n = f.frames;
  std::vector<BoundaryCandidate> out;
  if (n < 8) return out;
  // Proposals: spectral-change peaks (any syllable change shows up there) and
  // voicing onsets moved to the vowel-band rise (which win where both fall).
  enum Proposal : char { kNoProposal = 0, kChangePeak = 1, kVoicingOnset = 2 };
  std::vector<char> kind(n, kNoProposal);
  for (size_t k = 2; k + 2 < n; ++k) {
    bool peak = true;
    for (size_t j = k - 2; j <= k + 2 && peak; ++j) peak = f.change[j] <= f.change[k];
    if (peak && f.change[k] > 0) kind[k] = kChangePeak;
  }
  for (size_t k = 1; k < n; ++k)
    if (f.voiced[k] && !f.voiced[k - 1]) kind[vowelOnsetBefore(f, k)] = kVoicingOnset;
  // Unvoiced run ending at frame e (e unvoiced, e + 1 voiced): its length in frames (at most 60).
  auto runEndingAt = [&](long e) {
    long g = 0;
    while (e - g >= 0 && !f.voiced[static_cast<size_t>(e - g)] && g < 60) ++g;
    return g;
  };
  std::vector<size_t> picks;
  auto score = [&](size_t k) { return (kind[k] == kVoicingOnset ? 3.0f : 0.0f) + slopeAt(f, k) / 6 + f.change[k]; };
  for (size_t k = 0; k < n; ++k) {
    if (!kind[k]) continue;
    if (!picks.empty() && k - picks.back() <= 2) {
      if (score(k) > score(picks.back())) picks.back() = k;
      continue;
    }
    picks.push_back(k);
  }
  for (size_t c : picks) {
    if (c < 2 || c + 10 >= n) continue;
    float after = -1e9f;
    for (size_t k = c; k < c + 10; ++k) after = std::max(after, f.total[k]);
    if (after < f.loud[c] - 35) continue;
    // A vowel must follow: voiced frames (a voicing onset's own vowel may start
    // ~200 ms before the tracker voices it), or vowel-band energy above the
    // frication band (short syllables the tracker leaves unvoiced).
    int voicedAfter = 0, vowelLike = 0;
    for (size_t k = c; k < std::min(n, c + (kind[c] == kVoicingOnset ? 30 : 15)); ++k) voicedAfter += f.voiced[k];
    for (size_t k = c; k < c + 8; ++k) vowelLike += f.mid[k] >= f.high[k] + 6 && f.total[k] >= f.loud[c] - 25;
    if (voicedAfter < 3 && vowelLike < 4) continue;

    BoundaryCandidate b;
    b.frame = b.soundEnd = c;
    b.voicingOnset = kind[c] == kVoicingOnset;
    for (long e = std::min(static_cast<long>(n) - 2, static_cast<long>(c) + 2); e >= static_cast<long>(c) - 4 && e >= 0; --e)
      if (!f.voiced[static_cast<size_t>(e)] && f.voiced[static_cast<size_t>(e) + 1]) {
        b.breakBefore = static_cast<float>(static_cast<double>(runEndingAt(e) * kAnalysisHop) / kAnalysisRate);
        break;
      }
    for (size_t k = c; k < c + 6; ++k) b.unvoicedAfter += f.voiced[k] ? 0.0f : 1.0f / 6;
    // The previous sound ends where the last voicing break (>= 24 ms) before the onset begins.
    for (long e = static_cast<long>(c) + 2; e >= static_cast<long>(c) - 25 && e >= 0; --e) {
      if (static_cast<size_t>(e) + 1 >= n || f.voiced[static_cast<size_t>(e)] || !f.voiced[static_cast<size_t>(e) + 1]) continue;
      const long g = runEndingAt(e);
      if (g >= 3) {
        b.soundEnd = static_cast<size_t>(std::min(e - g + 1, static_cast<long>(c)));
        break;
      }
    }
    // Voicing break at the onset (voicing may start up to ~100 ms after the vowel rise).
    long gapEnd = -1;
    for (long e = static_cast<long>(std::min(n - 2, c + 12)); e >= static_cast<long>(c) - 3 && e >= 0; --e)
      if (!f.voiced[static_cast<size_t>(e)] && f.voiced[static_cast<size_t>(e) + 1]) { gapEnd = e; break; }
    if (gapEnd >= 0) {
      const long g = runEndingAt(gapEnd);
      b.gap = static_cast<float>(static_cast<double>(g * kAnalysisHop) / kAnalysisRate);
      const long lo = gapEnd - g + 1;
      const long hi = lo <= static_cast<long>(c) ? std::min(gapEnd, static_cast<long>(c)) : gapEnd;
      std::vector<float> vowel;
      for (size_t k = c; k < c + 8; ++k) vowel.push_back(f.high[k] - f.mid[k]);
      std::nth_element(vowel.begin(), vowel.begin() + 4, vowel.end());
      float fric = -1e9f;
      for (long k = lo; k <= hi; ++k) fric = std::max(fric, f.high[static_cast<size_t>(k)] - f.mid[static_cast<size_t>(k)]);
      b.frication = fric - vowel[4];
    }
    auto dip = [&](const std::vector<float>& x) {
      size_t m = c >= 12 ? c - 12 : 0;
      for (size_t k = m; k <= c; ++k) if (x[k] < x[m]) m = k;
      float left = x[m], right = x[c];
      for (size_t k = m >= 20 ? m - 20 : 0; k <= m; ++k) left = std::max(left, x[k]);
      for (size_t k = c; k <= c + 10; ++k) right = std::max(right, x[k]);
      return std::min(left, right) - x[m];
    };
    b.dipMid = dip(f.mid);
    b.dipHigh = dip(f.high);
    for (size_t k = c - 2; k <= c + 2; ++k) {
      b.change = std::max(b.change, f.change[k]);
      b.rise = std::max(b.rise, slopeAt(f, k));
    }
    const float a = medianVoiced(pitch, static_cast<long>(c) + 2, static_cast<long>(c) + 12);
    const float z = medianVoiced(pitch, static_cast<long>(c) - 14, static_cast<long>(c) - 2);
    b.pitchStep = a > 0 && z > 0 ? std::fabs(foldOctave(a - z)) : 0;
    b.level = after - f.loud[c];

    const float raw[kTerms] = {b.gap, b.frication, b.dipMid, b.dipHigh, b.change, b.rise, b.pitchStep, b.level};
    float x[kTerms];
    for (int i = 0; i < kTerms; ++i) x[i] = (std::clamp(raw[i], kClip[i].lo, kClip[i].hi)) / (kClip[i].hi - kClip[i].lo);
    b.any = logistic(kAny, x);
    for (int k = 0; k < 4; ++k) b.evidence[k] = logistic(kKind[k], x);
    // Inside vibrato, spectral change and shallow level dips recur every cycle:
    // without a voicing break or a deep dip they are not onsets.
    if (f.vibrato[c] > kVibratoGate && b.gap < 0.024f && b.dipMid < 8) {
      b.any *= kVibratoScale;
      for (float& e : b.evidence) e *= kVibratoScale;
    }
    out.push_back(b);
  }
  return out;
}

}  // namespace novox
