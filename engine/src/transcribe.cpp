#include "transcribe.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "phonemes.hpp"
#include "syllables.hpp"
#include "utf8.hpp"

namespace novox {

namespace {

constexpr double kRate = 16000;   // analysis rate
constexpr int kWindow = 512;      // YIN integration window (32 ms)
constexpr int kHop = 128;         // 8 ms
constexpr int kTauMin = 16000 / 1100;
constexpr int kTauMax = 16000 / 60;
constexpr double kPi = 3.14159265358979323846;

// Band-limited resampling to 16 kHz (windowed sinc, anti-aliased). Deliberately
// not spectrum.cpp's resampleTo16k: that one widens its filter above 48 kHz for
// singing-v2, and legacy / singing-v1 output must not change at any rate.
std::vector<float> resample16k(const float* x, size_t n, double sr) {
  if (std::fabs(sr - kRate) < 0.5) return std::vector<float>(x, x + n);
  const double ratio = kRate / sr;
  const size_t outN = static_cast<size_t>(std::floor(n * ratio));
  std::vector<float> y(outN);
  const double cutoff = std::min(1.0, ratio) * 0.45;  // cycles per input sample
  const int half = 24;
  for (size_t i = 0; i < outN; ++i) {
    const double t = i / ratio;
    const long c = static_cast<long>(std::floor(t));
    double acc = 0, wsum = 0;
    for (long k = c - half + 1; k <= c + half; ++k) {
      if (k < 0 || k >= static_cast<long>(n)) continue;
      const double d = t - static_cast<double>(k);
      const double s = d == 0 ? 2 * cutoff : std::sin(2 * kPi * cutoff * d) / (kPi * d);
      const double w = 0.5 + 0.5 * std::cos(kPi * d / half);  // Hann
      acc += x[k] * s * w;
      wsum += s * w;
    }
    y[i] = static_cast<float>(wsum != 0 ? acc / wsum : 0);
  }
  return y;
}

float median(std::vector<float> v) {
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

float percentile(std::vector<float> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(std::clamp(p, 0.0, 1.0) * (v.size() - 1))];
}

// Splits a word's spelling into `parts` pieces for display ("hello", 2 -> "hel", "lo"),
// preferring breaks just before a consonant that starts a new vowel group.
// Positions count characters, so an accented letter ("tête") is never cut.
std::vector<std::string> spellingParts(const std::string& word, size_t parts) {
  std::vector<size_t> at;  // byte offset of each character, then the word's length
  std::string base;        // each character's lowercase ASCII reading
  for (size_t i = 0; i < word.size();) {
    char c = 0;
    size_t len = spellingCharAt(word, i, &c);
    if (len == 0) {  // not from cleanWord: step over the whole character
      c = word[i];
      len = 1;
      utf8Decode(word, i, len);
    }
    at.push_back(i);
    base += c;
    i += len;
  }
  const size_t n = at.size();
  at.push_back(word.size());
  std::vector<std::string> out;
  if (parts <= 1 || n < parts) {
    out.push_back(word);
    for (size_t i = 1; i < parts; ++i) out.push_back("");
    return out;
  }
  auto isV = [](char c) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
  };
  // Candidate cut points: before a consonant that is followed by a vowel.
  std::vector<size_t> cands;
  for (size_t i = 1; i + 1 < n; ++i)
    if (!isV(base[i]) && isV(base[i + 1]) && i > 0) cands.push_back(i);
  std::vector<size_t> cuts;
  for (size_t p = 1; p < parts; ++p) {
    const size_t ideal = n * p / parts;
    size_t best = ideal;
    size_t bestDist = SIZE_MAX;
    for (size_t c : cands) {
      if (!cuts.empty() && c <= cuts.back()) continue;
      size_t d = c > ideal ? c - ideal : ideal - c;
      if (d < bestDist) bestDist = d, best = c;
    }
    if (!cuts.empty() && best <= cuts.back()) best = cuts.back() + 1;
    cuts.push_back(std::min(best, n - (parts - p)));
  }
  size_t from = 0;
  for (size_t c : cuts) {
    out.push_back(word.substr(at[from], at[c] - at[from]));
    from = c;
  }
  out.push_back(word.substr(at[from]));
  return out;
}

// A word's letters and apostrophes as written: accented letters whole (bytes
// tested one at a time with std::isalpha kept or dropped parts of them,
// depending on the host's locale), curly apostrophes straightened.
std::string cleanWord(const std::string& w) {
  std::string out;
  for (size_t i = 0; i < w.size();) {
    char c = 0;
    size_t len = spellingCharAt(w, i, &c);
    if (len > 0) {
      if (c == '\'') out += '\'';
      else out.append(w, i, len);
    } else {
      len = 1;
      utf8Decode(w, i, len);
    }
    i += len;
  }
  while (!out.empty() && out.front() == '\'') out.erase(out.begin());
  return out;
}

// The spelling of a cleanWord the lexicon reads: accented letters as their
// base letters ("tête" -> "tete"); ASCII words are unchanged.
std::string lexiconSpelling(const std::string& clean) {
  std::string out;
  for (size_t i = 0; i < clean.size();) {
    char c = 0;
    const size_t len = spellingCharAt(clean, i, &c);
    if (len == 0) break;  // cleanWord holds nothing else
    out += len == 1 ? clean[i] : c;
    i += len;
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pitch tracking (YIN)

std::vector<PitchFrame> trackPitch(const float* samples, size_t count, double sampleRate) {
  std::vector<float> x = resample16k(samples, count, sampleRate);
  std::vector<PitchFrame> frames;
  if (x.size() < static_cast<size_t>(kWindow + kTauMax + 1)) return frames;
  std::vector<double> d(kTauMax + 1), cmnd(kTauMax + 1);
  for (size_t start = 0; start + kWindow + kTauMax < x.size(); start += kHop) {
    const float* f = x.data() + start;
    PitchFrame pf;
    pf.time = (start + kWindow / 2.0) / kRate;
    double energy = 0;
    for (int j = 0; j < kWindow; ++j) energy += static_cast<double>(f[j]) * f[j];
    pf.db = static_cast<float>(10 * std::log10(energy / kWindow + 1e-12));

    d[0] = 0;
    for (int tau = 1; tau <= kTauMax; ++tau) {
      double s = 0;
      for (int j = 0; j < kWindow; ++j) {
        const double diff = f[j] - f[j + tau];
        s += diff * diff;
      }
      d[static_cast<size_t>(tau)] = s;
    }
    double running = 0;
    cmnd[0] = 1;
    for (int tau = 1; tau <= kTauMax; ++tau) {
      running += d[static_cast<size_t>(tau)];
      cmnd[static_cast<size_t>(tau)] = running > 0 ? d[static_cast<size_t>(tau)] * tau / running : 1;
    }
    int best = -1;
    for (int tau = kTauMin; tau <= kTauMax; ++tau) {
      if (cmnd[static_cast<size_t>(tau)] < 0.15) {
        while (tau + 1 <= kTauMax && cmnd[static_cast<size_t>(tau + 1)] < cmnd[static_cast<size_t>(tau)]) ++tau;
        best = tau;
        break;
      }
    }
    if (best < 0) {  // no dip below threshold: take the global minimum
      best = kTauMin;
      for (int tau = kTauMin; tau <= kTauMax; ++tau)
        if (cmnd[static_cast<size_t>(tau)] < cmnd[static_cast<size_t>(best)]) best = tau;
    }
    double tau = best;
    if (best > kTauMin && best < kTauMax) {  // parabolic refinement
      const double a = cmnd[static_cast<size_t>(best - 1)], b = cmnd[static_cast<size_t>(best)],
                   c = cmnd[static_cast<size_t>(best + 1)];
      const double denom = a - 2 * b + c;
      if (std::fabs(denom) > 1e-12) tau = best + 0.5 * (a - c) / denom;
    }
    pf.clarity = static_cast<float>(std::clamp(1.0 - cmnd[static_cast<size_t>(best)], 0.0, 1.0));
    pf.f0 = static_cast<float>(kRate / tau);
    frames.push_back(pf);
  }

  // Voicing decision.
  float loudest = -120;
  for (const auto& f : frames) loudest = std::max(loudest, f.db);
  for (auto& f : frames) {
    const bool voiced = f.clarity > 0.7f && f.db > loudest - 50 && f.f0 >= 60 && f.f0 <= 1100;
    if (!voiced) {
      f.f0 = 0;
      continue;
    }
    f.midi = static_cast<float>(69 + 12 * std::log2(f.f0 / 440.0));
  }

  // Fix octave jumps against a local median.
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].midi == 0) continue;
    std::vector<float> win;
    for (size_t j = i >= 7 ? i - 7 : 0; j < std::min(frames.size(), i + 8); ++j)
      if (frames[j].midi > 0) win.push_back(frames[j].midi);
    const float m = median(win);
    float& v = frames[i].midi;
    if (std::fabs(v - 12 - m) < 1.5f) v -= 12;
    else if (std::fabs(v + 12 - m) < 1.5f) v += 12;
    frames[i].f0 = static_cast<float>(440.0 * std::pow(2.0, (v - 69) / 12.0));
  }
  return frames;
}

// A frame carries several measured periods. Unlike the legacy octave rewrite,
// the sequence decoder can only choose pitches supported by the audio itself.
std::vector<PitchFrame> trackSingingPitch(const float* samples, size_t count, double sampleRate) {
  if (!samples || count == 0 || !std::isfinite(sampleRate) || sampleRate <= 0)
    throw std::invalid_argument("invalid audio for singing transcription");
  for (size_t i = 0; i < count; ++i)
    if (!std::isfinite(samples[i])) throw std::invalid_argument("audio contains non-finite samples");
  const std::vector<float> x = resample16k(samples, count, sampleRate);
  if (x.empty()) return {};
  struct Candidate {
    float midi = 0, cmnd = 1;
    double lag = 0, emission = 0;
  };
  struct Evidence {
    PitchFrame frame;
    std::vector<Candidate> candidates;
    std::vector<int> previous;
  };
  std::vector<Evidence> evidence;
  evidence.reserve((x.size() + kHop - 1) / kHop);
  std::vector<double> difference(kTauMax + 2), cmnd(kTauMax + 2);
  // Padding is used only at the recording edges. Both members of each lag pair
  // are centered about the stated timestamp (within half a sample for odd lags).
  constexpr int support = kWindow / 2 + (kTauMax + 2) / 2;
  std::vector<float> local(static_cast<size_t>(2 * support + 1));
  for (size_t center = 0; center < x.size(); center += kHop) {
    Evidence e;
    e.frame.time = center / kRate;
    e.frame.endTime = std::min((center + kHop / 2.0) / kRate, count / sampleRate);
    for (int j = -support; j <= support; ++j) {
      const auto at = static_cast<long long>(center) + j;
      local[static_cast<size_t>(j + support)] = at >= 0 && at < static_cast<long long>(x.size())
          ? x[static_cast<size_t>(at)] : 0;
    }
    double energy = 0;
    for (int j = -kWindow / 2; j < kWindow / 2; ++j) {
      const double value = local[static_cast<size_t>(j + support)];
      energy += value * value;
    }
    e.frame.db = static_cast<float>(10 * std::log10(energy / kWindow + 1e-12));
    double running = 0;
    cmnd[0] = 1;
    for (int tau = 1; tau <= kTauMax + 1; ++tau) {
      const int left = support - kWindow / 2 - tau / 2;
      const int right = support - kWindow / 2 + (tau + 1) / 2;
      double d = 0;
      for (int j = 0; j < kWindow; ++j) {
        const double delta = local[static_cast<size_t>(left + j)] - local[static_cast<size_t>(right + j)];
        d += delta * delta;
      }
      difference[static_cast<size_t>(tau)] = d;
      running += d;
      cmnd[static_cast<size_t>(tau)] = running > 1e-20 ? d * tau / running : 1;
    }
    auto candidateAt = [&](int tau) {
      const double a = cmnd[static_cast<size_t>(tau - 1)], b = cmnd[static_cast<size_t>(tau)],
                   c = cmnd[static_cast<size_t>(tau + 1)];
      const double denominator = a - 2 * b + c;
      const double offset = denominator > 1e-12 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0;
      Candidate candidate;
      candidate.lag = tau + offset;
      candidate.cmnd = static_cast<float>(std::clamp(b - 0.25 * (a - c) * offset, 0.0, 1.0));
      candidate.midi = static_cast<float>(69 + 12 * std::log2((kRate / candidate.lag) / 440.0));
      return candidate;
    };
    int best = kTauMin;
    std::vector<Candidate> minima;
    for (int tau = kTauMin; tau <= kTauMax; ++tau) {
      if (cmnd[static_cast<size_t>(tau)] < cmnd[static_cast<size_t>(best)]) best = tau;
      if (cmnd[static_cast<size_t>(tau)] > cmnd[static_cast<size_t>(tau - 1)] ||
          cmnd[static_cast<size_t>(tau)] > cmnd[static_cast<size_t>(tau + 1)]) continue;
      auto candidate = candidateAt(tau);
      const double hz = kRate / candidate.lag;
      if (candidate.cmnd <= 0.40f && hz >= 60 && hz <= 1100) minima.push_back(candidate);
    }
    if (minima.empty()) {
      auto candidate = candidateAt(best);
      if (kRate / candidate.lag >= 60 && kRate / candidate.lag <= 1100) minima.push_back(candidate);
    }
    double shortestCredible = 0;
    for (const auto& candidate : minima)
      if (candidate.cmnd <= 0.30f && (shortestCredible == 0 || candidate.lag < shortestCredible))
        shortestCredible = candidate.lag;
    // Keep the shortest plausible trough as well as the deepest ones: pure
    // periodic signals have excellent minima at several integer periods.
    if (!minima.empty()) {
      e.candidates.push_back(minima.front());
      std::stable_sort(minima.begin(), minima.end(), [](const Candidate& a, const Candidate& b) {
        return a.cmnd < b.cmnd;
      });
      for (const auto& candidate : minima) {
        if (e.candidates.size() >= 8) break;
        bool duplicate = false;
        for (const auto& kept : e.candidates)
          if (std::fabs(candidate.midi - kept.midi) < 0.35f) duplicate = true;
        if (!duplicate) e.candidates.push_back(candidate);
      }
    }
    for (auto& candidate : e.candidates)
      candidate.emission = 6 * candidate.cmnd + (shortestCredible > 0
          ? 0.30 * std::max(0.0, std::log2(candidate.lag / shortestCredible)) : 0);
    evidence.push_back(std::move(e));
  }
  std::vector<float> levels;
  levels.reserve(evidence.size());
  for (const auto& e : evidence) levels.push_back(e.frame.db);
  const float floorDb = std::max(-80.0f, percentile(levels, 0.95) - 50.0f);
  constexpr double infinity = std::numeric_limits<double>::infinity();
  std::vector<double> previousCost;
  for (size_t t = 0; t < evidence.size(); ++t) {
    auto& e = evidence[t];
    const bool energetic = e.frame.db > floorDb;
    const size_t unvoiced = e.candidates.size();
    std::vector<double> cost(unvoiced + 1, infinity);
    e.previous.assign(unvoiced + 1, -1);
    for (size_t k = 0; k <= unvoiced; ++k) {
      if (k != unvoiced && !energetic) continue;
      const double emission = k == unvoiced ? (energetic ? 1.2 : 0) : e.candidates[k].emission;
      if (t == 0) { cost[k] = emission; continue; }
      const auto& before = evidence[t - 1];
      const size_t priorUnvoiced = before.candidates.size();
      for (size_t j = 0; j < previousCost.size(); ++j) {
        double transition = 0;
        if ((j == priorUnvoiced) != (k == unvoiced)) transition = 0.15;
        else if (j != priorUnvoiced && k != unvoiced)
          transition = std::min(0.30, 0.025 * std::fabs(before.candidates[j].midi - e.candidates[k].midi));
        const double total = previousCost[j] + transition + emission;
        if (total < cost[k]) { cost[k] = total; e.previous[k] = static_cast<int>(j); }
      }
    }
    // Recenter to keep long recordings numerically well behaved.
    const double minimum = *std::min_element(cost.begin(), cost.end());
    for (auto& value : cost) value -= minimum;
    previousCost = std::move(cost);
  }
  int state = static_cast<int>(std::min_element(previousCost.begin(), previousCost.end()) - previousCost.begin());
  std::vector<PitchFrame> frames(evidence.size());
  for (size_t t = evidence.size(); t-- > 0;) {
    const auto& e = evidence[t];
    auto& frame = frames[t];
    frame = e.frame;
    if (state >= 0 && static_cast<size_t>(state) < e.candidates.size()) {
      const auto& selected = e.candidates[static_cast<size_t>(state)];
      frame.midi = selected.midi;
      frame.f0 = static_cast<float>(kRate / selected.lag);
      frame.clarity = 1 - selected.cmnd;
      double margin = 0.6;
      for (size_t k = 0; k < e.candidates.size(); ++k) {
        if (k == static_cast<size_t>(state)) continue;
        const auto& alternative = e.candidates[k];
        const double ratio = alternative.lag / selected.lag;
        // Longer integer periods with identical evidence are the ordinary
        // redundant minima of a periodic wave, not independent vocal pitches.
        if (ratio > 1.8 && std::fabs(ratio - std::round(ratio)) < 0.06 &&
            std::fabs(alternative.cmnd - selected.cmnd) < 0.015f) continue;
        margin = std::min(margin, alternative.emission - selected.emission);
      }
      frame.confidence = frame.clarity * static_cast<float>(0.75 + 0.25 * std::clamp(margin / 0.6, 0.0, 1.0));
    } else {
      frame.confidence = 0;
    }
    state = e.previous[static_cast<size_t>(state)];
  }
  // A breathy onset may have weaker periodicity than its vowel. Recover only
  // measured candidates consistent with a nearby confidently voiced frame;
  // the bounded original path prevents this from propagating through a rest.
  const auto decoded = frames;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (decoded[i].midi > 0 || evidence[i].frame.db <= floorDb) continue;
    float anchor = 0;
    for (size_t distance = 1; distance <= 5 && anchor == 0; ++distance) {
      for (int direction : {-1, 1}) {
        const auto j = static_cast<long long>(i) + direction * static_cast<long long>(distance);
        if (j < 0 || j >= static_cast<long long>(frames.size())) continue;
        if (decoded[static_cast<size_t>(j)].midi > 0 && decoded[static_cast<size_t>(j)].clarity >= 0.85f)
          anchor = decoded[static_cast<size_t>(j)].midi;
      }
    }
    if (anchor == 0) continue;
    const Candidate* best = nullptr;
    for (const auto& candidate : evidence[i].candidates)
      if (candidate.cmnd <= 0.30f && std::fabs(candidate.midi - anchor) <= 0.6f &&
          (!best || candidate.cmnd < best->cmnd)) best = &candidate;
    if (!best) continue;
    frames[i].midi = best->midi;
    frames[i].f0 = static_cast<float>(kRate / best->lag);
    frames[i].clarity = 1 - best->cmnd;
    frames[i].confidence = 0.75f * frames[i].clarity;
  }
  return frames;
}

// ---------------------------------------------------------------------------
// Note segmentation

std::vector<TranscribedNote> segmentNotes(const std::vector<PitchFrame>& frames, const MelodyOptions& opts) {
  std::vector<TranscribedNote> notes;
  if (frames.empty()) return notes;
  const bool singing = opts.algorithm == "singing-v1";
  const bool stabilize = opts.stabilizePitch || singing;
  if (opts.algorithm != "legacy" && !singing) throw std::invalid_argument("unknown transcription algorithm");
  const double hop = frames.size() > 1 ? frames[1].time - frames[0].time : kHop / kRate;
  float loudest = -120;
  for (const auto& f : frames) loudest = std::max(loudest, f.db);
  auto voiced = [&](size_t i) { return frames[i].midi > 0 && frames[i].db > loudest + opts.silenceDb; };

  // Smoothed level for onset detection.
  std::vector<float> level(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    double s = 0;
    int c = 0;
    for (size_t j = i >= 2 ? i - 2 : 0; j < std::min(frames.size(), i + 3); ++j, ++c) s += frames[j].db;
    level[i] = static_cast<float>(s / c);
  }

  std::vector<std::vector<size_t>> groups;
  std::vector<size_t> cur;
  size_t gap = 0;
  const size_t maxGap = static_cast<size_t>((singing ? 0.08 : 0.04) / hop);  // bridge tracker dropouts
  auto flush = [&] {
    if (!cur.empty()) groups.push_back(cur);
    cur.clear();
  };
  for (size_t i = 0; i < frames.size(); ++i) {
    if (!voiced(i)) {
      if (!cur.empty() && ++gap > maxGap) {
        flush();
        gap = 0;
      }
      continue;
    }
    if (!cur.empty()) {
      // Compare note pitch centers; small changes need longer evidence than
      // large melodic steps, and the evidence must stay on the same side.
      std::vector<float> recent;
      for (size_t k = cur.size() > 20 ? cur.size() - 20 : 0; k < cur.size(); ++k) recent.push_back(frames[cur[k]].midi);
      const float ref = median(recent);
      bool jump = false;
      float target = ref;
      if (cur.size() >= 3 && std::fabs(frames[i].midi - ref) > opts.splitSemitones) {
        const bool largeStep = !stabilize || std::fabs(frames[i].midi - ref) > 2.0f;
        const size_t settle = largeStep ? 4 : static_cast<size_t>(std::ceil(0.08 / hop));
        const float direction = frames[i].midi > ref ? 1.0f : -1.0f;
        std::vector<float> ahead;
        size_t hold = 0;
        for (size_t k = i; k < std::min(frames.size(), i + settle); ++k) {
          if (!voiced(k)) continue;
          ahead.push_back(frames[k].midi);
          if ((stabilize ? (frames[k].midi - ref) * direction :
              std::fabs(frames[k].midi - ref)) > opts.splitSemitones) ++hold;
        }
        jump = largeStep ? hold >= 3 :
            hold == settle && percentile(ahead, 0.9) - percentile(ahead, 0.1) < 0.5f;
      }
      // Tracking failures alone need not be consonants. Keep a short dropout
      // inside a hold unless there is also an acoustic dip.
      const bool onset = gap >= 2 && (!stabilize ||
          frames[cur.back()].db - level[i - gap / 2] > opts.onsetDb);
      if (jump) {
        // Put the boundary where the glide crosses halfway between the two pitches,
        // not where it finally arrives.
        std::vector<float> boundaryPitch;
        for (size_t k = i; k < std::min(frames.size(), i + 5); ++k)
          if (voiced(k)) boundaryPitch.push_back(frames[k].midi);
        target = median(boundaryPitch);
        const float mid = (ref + target) / 2;
        size_t cut = cur.size();
        while (cut > 3 && ((target > ref) ? frames[cur[cut - 1]].midi > mid : frames[cur[cut - 1]].midi < mid)) --cut;
        std::vector<size_t> tail(cur.begin() + static_cast<long>(cut), cur.end());
        cur.resize(cut);
        flush();
        cur = tail;
      } else if (onset) {
        flush();
      }
    }
    gap = 0;
    cur.push_back(i);
  }
  flush();

  // Level-dip split for repeated same-pitch syllables inside a group.
  std::vector<std::vector<size_t>> split;
  for (auto& g : groups) {
    size_t from = 0;
    for (size_t k = 4; k + 4 < g.size(); ++k) {
      const float here = level[g[k]];
      float left = -1e9f, right = -1e9f;
      for (size_t j = (k >= 12 ? k - 12 : 0); j < k; ++j) left = std::max(left, level[g[j]]);
      for (size_t j = k + 1; j < std::min(g.size(), k + 13); ++j) right = std::max(right, level[g[j]]);
      bool isMin = here <= level[g[k - 1]] && here <= level[g[k + 1]];
      if (isMin && left - here > opts.onsetDb && right - here > opts.onsetDb && k - from >= 6) {
        split.emplace_back(g.begin() + static_cast<long>(from), g.begin() + static_cast<long>(k));
        from = k;
      }
    }
    split.emplace_back(g.begin() + static_cast<long>(from), g.end());
  }

  for (const auto& g : split) {
    if (g.empty()) continue;
    TranscribedNote tn;
    tn.note.start = frames[g.front()].time - hop / 2;
    if (singing) tn.note.start = std::max(0.0, tn.note.start);
    const double end = singing && frames[g.back()].endTime > frames[g.back()].time
        ? frames[g.back()].endTime : frames[g.back()].time + hop / 2;
    tn.note.dur = end - tn.note.start;
    // Pitch from the steady middle of the note (skip scoops and glides).
    std::vector<float> mids;
    const size_t a = g.size() / 5, b = g.size() - g.size() / 5;
    for (size_t k = a; k < std::max(b, a + 1) && k < g.size(); ++k) mids.push_back(frames[g[k]].midi);
    const float pitch = median(mids);
    tn.detectedPitch = pitch;
    tn.note.pitch = opts.roundPitches ? std::round(pitch) : pitch;
    tn.pitchSpread = percentile(mids, 0.9) - percentile(mids, 0.1);
    float clarity = 0;
    for (size_t k : g) clarity += singing && frames[k].confidence >= 0 ? frames[k].confidence : frames[k].clarity;
    tn.confidence = clarity / static_cast<float>(g.size());
    notes.push_back(tn);
  }

  // A phrase can begin with a continuously rising/falling scoop before there
  // is any settled center. Merge only that monotonic lead-in into a following
  // sustained center; a short stable first note remains a musical event.
  if (singing) {
    for (size_t i = 0; i + 1 < notes.size(); ++i) {
      auto& lead = notes[i];
      auto& target = notes[i + 1];
      const bool phraseStart = i == 0 || lead.note.start - (notes[i - 1].note.start + notes[i - 1].note.dur) > 0.08;
      if (!phraseStart || lead.note.dur > 0.18 || lead.pitchSpread < 0.75f || target.note.dur < 0.16 ||
          target.pitchSpread > 0.75f || target.note.start - (lead.note.start + lead.note.dur) > 0.03) continue;
      std::vector<float> contour;
      for (const auto& frame : frames)
        if (frame.midi > 0 && frame.time >= lead.note.start && frame.time < lead.note.start + lead.note.dur)
          contour.push_back(frame.midi);
      if (contour.size() < 5) continue;
      const size_t third = std::max<size_t>(1, contour.size() / 3);
      const float from = median(std::vector<float>(contour.begin(), contour.begin() + static_cast<long>(third)));
      const float to = median(std::vector<float>(contour.end() - static_cast<long>(third), contour.end()));
      const float direction = to > from ? 1 : -1;
      if (std::fabs(to - from) < 1.0f || (target.note.pitch - to) * direction < 0) continue;
      size_t consistent = 0;
      for (size_t k = 1; k < contour.size(); ++k)
        if ((contour[k] - contour[k - 1]) * direction >= -0.05f) ++consistent;
      if (consistent < (contour.size() - 1) * 0.85) continue;
      target.note.dur = target.note.start + target.note.dur - lead.note.start;
      target.note.start = lead.note.start;
      lead.note.dur = 0;
    }
    notes.erase(std::remove_if(notes.begin(), notes.end(), [](const TranscribedNote& n) { return n.note.dur <= 0; }),
                notes.end());
  }

  // Fold short passing tones (glides between two notes) into the next note.
  for (size_t i = 1; i + 1 < notes.size(); ++i) {
    auto& n = notes[i];
    const auto& prev = notes[i - 1];
    auto& next = notes[i + 1];
    const bool touching = n.note.start - (prev.note.start + prev.note.dur) < 0.07 &&
                          next.note.start - (n.note.start + n.note.dur) < 0.07;
    const float lo = std::min(prev.note.pitch, next.note.pitch), hi = std::max(prev.note.pitch, next.note.pitch);
    if (touching && n.note.dur < 0.15 && n.note.pitch > lo && n.note.pitch < hi &&
        (!singing || (n.pitchSpread > 0.75f && n.confidence < 0.65f))) {
      next.note.dur = next.note.start + next.note.dur - n.note.start;
      next.note.start = n.note.start;
      n.note.dur = 0;  // dropped below
    }
  }
  notes.erase(std::remove_if(notes.begin(), notes.end(), [](const TranscribedNote& n) { return n.note.dur <= 0; }),
              notes.end());

  // Octave glitches: a short blip exactly an octave from a touching neighbor is
  // the tracker locking onto a strong harmonic. Fold it into that neighbor.
  auto octaveOf = [](float a, float b) { return std::fabs(std::fabs(a - b) - 12.0f) < 0.6f; };
  for (size_t i = 0; i < notes.size(); ++i) {
    auto& n = notes[i];
    if (singing || n.note.dur >= 0.2) continue;
    if (i + 1 < notes.size() && notes[i + 1].note.start - (n.note.start + n.note.dur) < 0.03 &&
        octaveOf(n.note.pitch, notes[i + 1].note.pitch)) {
      auto& next = notes[i + 1];
      next.note.dur = next.note.start + next.note.dur - n.note.start;
      next.note.start = n.note.start;
      n.note.dur = 0;
    } else if (i > 0 && notes[i - 1].note.dur > 0 &&
               n.note.start - (notes[i - 1].note.start + notes[i - 1].note.dur) < 0.03 &&
               octaveOf(n.note.pitch, notes[i - 1].note.pitch)) {
      notes[i - 1].note.dur = n.note.start + n.note.dur - notes[i - 1].note.start;
      n.note.dur = 0;
    }
  }
  notes.erase(std::remove_if(notes.begin(), notes.end(), [](const TranscribedNote& n) { return n.note.dur <= 0; }),
              notes.end());

  // Merge or drop events shorter than the minimum.
  std::vector<TranscribedNote> out;
  for (auto& n : notes) {
    if (n.note.dur >= opts.minNoteSeconds) {
      out.push_back(n);
      continue;
    }
    if (!out.empty() && n.note.start - (out.back().note.start + out.back().note.dur) < 0.03) {
      out.back().note.dur = n.note.start + n.note.dur - out.back().note.start;  // extend previous
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lyric alignment

std::vector<std::string> alignLyrics(std::vector<TranscribedNote>& notes, const std::vector<Word>& words,
                                     const Lexicon& lexicon, bool preserveWords,
                                     std::vector<size_t>* unassignedWordIndices,
                                     const std::vector<PitchFrame>* articulationFrames) {
  std::vector<std::string> warnings;
  std::vector<std::string> boundaryWarnings;
  const size_t originalCount = notes.size();
  if (unassignedWordIndices) unassignedWordIndices->clear();
  for (auto& n : notes) n.note.lyric.clear();
  // Prefer a phrase-wide monotonic assignment when there are enough existing
  // events for the pronounced syllables. Word timestamps are soft evidence;
  // never create events or move their pitches/times to satisfy the transcript.
  if (preserveWords && !words.empty() && !notes.empty() && words.size() * notes.size() <= 250000) {
    const size_t W = words.size(), N = notes.size();
    std::vector<size_t> syllables(W);
    size_t total = 0;
    bool valid = true;
    for (size_t w = 0; w < W; ++w) {
      if (cleanWord(words[w].text).empty() || words[w].end <= words[w].start ||
          (w && words[w].start < words[w-1].start)) { valid = false; break; }
      for (const auto& phoneme : lexicon.lookup(lexiconSpelling(cleanWord(words[w].text))))
        syllables[w] += isVowelSymbol(phoneme.symbol) ? 1 : 0;
      syllables[w] = std::max<size_t>(1, syllables[w]);
      total += syllables[w];
    }
    if (valid && total <= N) {
      const double inf = std::numeric_limits<double>::infinity();
      const size_t stride = N + 1;
      std::vector<double> costs((W+1)*stride, inf);
      std::vector<int> previous((W+1)*stride, -1);
      costs[0] = 0;
      for (size_t w = 0; w <= W; ++w) {
        for (size_t n = 0; n <= N; ++n) {
          const double cost = costs[w*stride+n];
          if (!std::isfinite(cost)) continue;
          // Unclaimed leading/rest notes remain hums rather than stealing words.
          if (n < N && cost + 1.5 < costs[w*stride+n+1]) {
            costs[w*stride+n+1] = cost + 1.5;
            previous[w*stride+n+1] = -2;
          }
          if (w == W) continue;
          for (size_t length = syllables[w]; length <= std::min(N-n, syllables[w]+12); ++length) {
            const auto& first = notes[n].note;
            const auto& last = notes[n+length-1].note;
            const double slack = 0.45;
            if (first.start > words[w].end + slack || last.start + last.dur < words[w].start - slack) continue;
            if (first.start < words[w].start - slack || last.start > words[w].end + slack) continue;
            double timing = 0;
            for (size_t j = n; j < n+length; ++j) {
              const auto& note = notes[j].note;
              const double outside = std::max({0.0, words[w].start-(note.start+note.dur), note.start-words[w].end});
              timing += 4 * outside / slack;
              // A long rest inside a word group is stronger evidence of a new
              // word than the ASR onset, which can lead the sung vowel.
              if (j > n) {
                const auto& prior = notes[j-1].note;
                timing += 2 * std::max(0.0, note.start-(prior.start+prior.dur)-.15);
              }
            }
            // Weak boundary cost tolerates ASR lag, while count and ordering
            // prevent a three-syllable word from losing its last event to 'me'.
            timing += 0.5 * std::min(1.0, std::fabs(first.start-words[w].start)/slack);
            timing += 1.5 * std::min(1.0, std::fabs(last.start+last.dur-words[w].end)/slack);
            timing += 0.25 * (length-syllables[w]);
            const size_t dest = (w+1)*stride+n+length;
            if (cost+timing < costs[dest]) {costs[dest]=cost+timing;previous[dest]=static_cast<int>(length);}
          }
        }
      }
      if (std::isfinite(costs[W*stride+N])) {
        std::vector<std::pair<size_t,size_t>> groups(W);
        size_t w=W,n=N;
        while (w || n) {
          const int length = previous[w*stride+n];
          if (length == -2) {--n;continue;}
          if (length < 1 || w == 0) break;
          groups[w-1] = {n-static_cast<size_t>(length),n};
          n -= static_cast<size_t>(length);--w;
        }
        if (w == 0) {
          for (size_t index=0;index<W;++index) {
            const auto parts = spellingParts(cleanWord(words[index].text),syllables[index]);
            const auto [begin,end] = groups[index];
            for (size_t j=begin;j<end;++j) {
              const size_t part=j-begin;
              notes[j].note.lyric = part<parts.size() ? parts[part]+(part+1<parts.size()?"-":"") : "_";
              notes[j].confidence=std::min(notes[j].confidence,words[index].probability);
            }
            if (notes[end-1].note.start >= words[index].end || notes[begin].note.start+notes[begin].note.dur <= words[index].start)
              warnings.push_back("'"+cleanWord(words[index].text)+"' aligned by syllables beyond its approximate word timestamp");
          }
          return warnings;
        }
      }
    }
  }
  size_t ni = 0;
  long lastLyric = -1;
  for (size_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
    const auto& w = words[wordIndex];
    const auto unassigned = [&] {
      if (unassignedWordIndices) unassignedWordIndices->push_back(wordIndex);
    };
    const std::string text = cleanWord(w.text);
    if (text.empty()) {
      if (preserveWords && w.text.find_first_not_of(" \t\r\n") != std::string::npos) {
        unassigned();
        warnings.push_back("'" + w.text + "' has no supported lyric text; retained for editing");
      }
      continue;
    }
    if (preserveWords && (!std::isfinite(w.start) || !std::isfinite(w.end) || w.start < 0 || w.end <= w.start)) {
      unassigned();
      warnings.push_back("'" + text + "' has an invalid timestamp; retained for editing");
      continue;
    }
    size_t k = 0;
    for (const auto& t : lexicon.lookup(lexiconSpelling(text))) k += isVowelSymbol(t.symbol) ? 1 : 0;
    k = std::max<size_t>(1, k);

    // Notes sung during this word (Whisper word times are approximate).
    std::vector<size_t> group;
    while (ni < notes.size() && notes[ni].note.start < w.end - (preserveWords ? 0.0 : 0.02)) {
      const double noteEnd = notes[ni].note.start + notes[ni].note.dur;
      const bool beforeWord = preserveWords ? noteEnd <= w.start : noteEnd < w.start + 0.03;
      if (beforeWord && !group.empty()) break;
      if (beforeWord) {
        // A note before this word started: hold the previous vowel (a melisma
        // chain), or hum if it stands apart.
        if (lastLyric >= 0 && ni > 0 && !notes[ni - 1].note.lyric.empty() &&
            notes[ni].note.start - (notes[ni - 1].note.start + notes[ni - 1].note.dur) < 0.3)
          notes[ni].note.lyric = "_";
        ++ni;
        continue;
      }
      group.push_back(ni++);
    }
    if (group.empty()) {
      if (lastLyric >= 0) {
        // Split only when the timestamp actually falls inside the previous
        // event. A word in a rest must not manufacture a note in earlier audio.
        const size_t at = static_cast<size_t>(lastLyric);
        TranscribedNote second = notes[at];
        const double end = second.note.start + second.note.dur;
        if (w.start <= second.note.start + 0.04 || w.start >= end - 0.04) {
          unassigned();
          warnings.push_back("'" + text + "' has no overlapping note; review its timestamp");
          continue;
        }
        notes[at].note.dur = w.start - second.note.start;
        second.note.start = w.start;
        second.note.dur = end - w.start;
        second.note.lyric = text;  // all syllables on one note
        second.confidence = std::min(second.confidence, w.probability) * 0.8f;
        notes.insert(notes.begin() + static_cast<long>(at) + 1, second);
        ++ni;
        lastLyric = static_cast<long>(at) + 1;
        warnings.push_back("'" + text + "' shares a held note; split at its word timestamp (review)");
        boundaryWarnings.push_back(warnings.back());
      } else {
        unassigned();
        warnings.push_back("'" + text + "' was heard before any note; " +
                           (preserveWords ? "retained for editing" : "skipped"));
      }
      continue;
    }
    // Recover only boundaries demanded by a multi-syllable word. A shallow
    // level valley is insufficient for melody-only segmentation, but can support
    // an articulation here. Never split flat audio, rests, or near event edges.
    while (preserveWords && articulationFrames && k > group.size()) {
      double bestDepth = 1.0, boundary = 0;
      size_t bestNote = notes.size();
      const auto& frames = *articulationFrames;
      for (const size_t index : group) {
        const auto& note = notes[index].note;
        for (size_t f = 1; f + 1 < frames.size(); ++f) {
          const auto& frame = frames[f];
          if (frame.time < std::max(note.start + .08, w.start) ||
              frame.time > std::min(note.start + note.dur - .08, w.end) ||
              frame.f0 <= 0 || std::fabs(frame.midi - note.pitch) > 1 ||
              frame.db > frames[f-1].db || frame.db >= frames[f+1].db) continue;
          float left = frame.db, right = frame.db;
          for (size_t j=f; j>0 && frame.time-frames[j-1].time <= .096; --j)
            left = std::max(left, frames[j-1].db);
          for (size_t j=f+1; j<frames.size() && frames[j].time-frame.time <= .096; ++j)
            right = std::max(right, frames[j].db);
          const double depth = std::min(left,right)-frame.db;
          if (depth > bestDepth) {bestDepth=depth;boundary=frame.time;bestNote=index;}
        }
      }
      if (bestNote == notes.size()) break;
      auto second = notes[bestNote];
      const double end = second.note.start + second.note.dur;
      notes[bestNote].note.dur = boundary - second.note.start;
      second.note.start = boundary; second.note.dur = end-boundary;
      second.confidence *= .8f;
      notes.insert(notes.begin()+static_cast<long>(bestNote)+1, second);
      for (auto& index : group) if (index > bestNote) ++index;
      group.insert(std::find(group.begin(),group.end(),bestNote)+1,bestNote+1);
      ++ni;
      warnings.push_back("'"+text+"' recovered a syllable at an acoustic valley (review)");
      boundaryWarnings.push_back(warnings.back());
    }
    // Spread syllables over the notes: one each, extra notes hold the vowel,
    // too few notes squeeze the remaining syllables onto the last one.
    if (preserveWords && k > group.size())
      warnings.push_back("'"+text+"' has "+std::to_string(k)+" syllables but only "+std::to_string(group.size())+" note boundaries; review the lyric split");
    const size_t used = std::min(k, group.size());
    auto parts = spellingParts(text, used);
    for (size_t i = 0; i < group.size(); ++i) {
      auto& n = notes[group[i]];
      if (i < used) {
        n.note.lyric = parts[i] + (i + 1 < used ? "-" : "");
      } else {
        n.note.lyric = "_";
      }
      n.confidence = std::min(n.confidence, w.probability);
      if (preserveWords) {
        const double overlap = std::min(n.note.start + n.note.dur, w.end) - std::max(n.note.start, w.start);
        if (overlap < 0.02) {
          n.confidence *= 0.75f;
          warnings.push_back("'" + text + "' has only a small note overlap; review its timestamp");
        }
      }
    }
    lastLyric = static_cast<long>(group.back());
    for (size_t i = group.size(); i-- > 0;)
      if (notes[group[i]].note.lyric != "_") {
        lastLyric = static_cast<long>(group[i]);
        break;
      }
  }
  // Notes after the last word.
  for (; ni < notes.size(); ++ni) {
    if (lastLyric >= 0 && ni > 0 && !notes[ni - 1].note.lyric.empty() &&
        notes[ni].note.start - (notes[ni - 1].note.start + notes[ni - 1].note.dur) < 0.3)
      notes[ni].note.lyric = "_";
  }
  if (articulationFrames && notes.size() > originalCount) {
    // Revisit the phrase after recovering events: early approximate word windows
    // must not steal notes needed by later words. No further acoustic splits.
    auto aligned = alignLyrics(notes, words, lexicon, preserveWords, unassignedWordIndices);
    warnings = std::move(boundaryWarnings);
    warnings.insert(warnings.end(), aligned.begin(), aligned.end());
  }
  return warnings;
}

std::string transcriptionToJson(const std::vector<TranscribedNote>& notes, const std::vector<std::string>& warnings,
                                const std::string& algorithm, const std::vector<Word>* transcriptWords,
                                const std::vector<size_t>* unassignedWordIndices,
                                const std::vector<PitchFrame>* debugFrames, const float* tuningSemitones,
                                const std::string* detail) {
  // Lyrics and warnings echo the caller's words: invalid UTF-8 in them is
  // replaced (U+FFFD) so the result always parses.
  auto esc = [](const std::string& s) {
    std::string r;
    for (unsigned char c : validUtf8(s)) {
      if (c < 0x20) {
        char encoded[7];
        std::snprintf(encoded, sizeof encoded, "\\u%04x", static_cast<unsigned>(c));
        r += encoded;
        continue;
      }
      if (c == '"' || c == '\\') r += '\\';
      r += static_cast<char>(c);
    }
    return r;
  };
  std::ostringstream o;
  char buf[160];
  o << "{\n  \"units\": \"seconds\",\n  \"tempo\": 120,\n";
  if (!algorithm.empty() || transcriptWords || tuningSemitones || detail)
    o << "  \"transcription\": {\"algorithm\": \"" << esc(algorithm.empty() ? "legacy" : algorithm) << "\"";
  // The pitch reference note pitches were rounded against, in cents from A440
  // (detectedPitch stays absolute).
  if (tuningSemitones) o << ", \"tuningCents\": " << static_cast<int>(std::lround(*tuningSemitones * 100));
  if (detail) o << ", \"detail\": \"" << esc(*detail) << "\"";  // the note detail the notes were segmented with
  if (transcriptWords) {
    o << ", \"transcriptWords\": [";
    for (size_t i = 0; i < transcriptWords->size(); ++i) {
      const auto& w = (*transcriptWords)[i];
      // Invalid supplied times remain explicitly unassigned; keep valid JSON.
      const auto finiteOrZero = [](double value) { return std::isfinite(value) ? value : 0.0; };
      // Supplied times are only checked to be finite, and "%.4f" of one can run to
      // 315 characters: a 160-byte buffer cut e.g. "end": 1e300 into invalid JSON.
      char fields[768];
      std::snprintf(fields, sizeof fields, ", \"start\": %.4f, \"end\": %.4f, \"probability\": %.4f}",
                    finiteOrZero(w.start), finiteOrZero(w.end), finiteOrZero(w.probability));
      o << (i ? ", " : "") << "{\"word\": \"" << esc(w.text) << "\"" << fields;
    }
    o << "], \"unassignedWords\": [";
    if (unassignedWordIndices) {
      bool first = true;
      for (size_t index : *unassignedWordIndices) {
        if (index >= transcriptWords->size()) continue;
        o << (first ? "" : ", ") << "{\"wordIndex\": " << index << ", \"word\": \""
          << esc((*transcriptWords)[index].text) << "\", \"reason\": \"no_reliable_note\"}";
        first = false;
      }
    }
    o << "]";
  }
  if (!algorithm.empty() || transcriptWords || tuningSemitones || detail) o << "},\n";
  if (!warnings.empty()) {
    o << "  \"transcribeWarnings\": [";
    for (size_t i = 0; i < warnings.size(); ++i) o << (i ? ", " : "") << "\"" << esc(warnings[i]) << "\"";
    o << "],\n";
  }
  if (debugFrames) {
    // The analysis frames the notes were segmented from (unvoiced: midi 0).
    o << "  \"frames\": [";
    for (size_t i = 0; i < debugFrames->size(); ++i) {
      const auto& f = (*debugFrames)[i];
      std::snprintf(buf, sizeof buf, "%s[%.4f, %.3f, %.2f, %.3f, %.3f]", i ? ", " : "", f.time, f.midi, f.db,
                    f.clarity, f.confidence);
      o << buf;
    }
    o << "],\n";
  }
  o << "  \"notes\": [\n";
  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& n = notes[i];
    std::snprintf(buf, sizeof buf, "    {\"pitch\": %.2f, \"start\": %.4f, \"dur\": %.4f, \"confidence\": %.2f, \"pitchSpread\": %.2f",
                  n.note.pitch, n.note.start, n.note.dur, n.confidence, n.pitchSpread);
    o << buf;
    if (n.detectedPitch >= 0) o << ", \"detectedPitch\": " << n.detectedPitch;
    if (!n.note.lyric.empty()) o << ", \"lyric\": \"" << esc(n.note.lyric) << "\"";
    o << "}" << (i + 1 < notes.size() ? "," : "") << "\n";
  }
  o << "  ]\n}\n";
  return o.str();
}

}  // namespace novox
