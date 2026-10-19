// singing-v2 tests for engine/src/singing_pitch.*
//
// Synthetic voices with known pitch: harmonic tones (glottal-like spectra), period
// doubling (alternating cycles), weak fundamentals, real octave leaps, vibrato,
// chords, silence and recording edges.
#include "v2_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

#include "singing_pitch.hpp"
#include "spectrum.hpp"

namespace {

constexpr double kPiTest = 3.14159265358979323846;

struct Voice {
  std::function<double(double)> midi;     // pitch at time t (seconds)
  std::vector<double> harmonics = {1.0, 0.7, 0.5, 0.35, 0.25, 0.18, 0.12, 0.08, 0.05, 0.03};
  double amplitude = 0.2;
  double start = 0.1, end = 1.1;          // sounding span (seconds)
  double doubling = 0;                    // alternate-cycle amplitude change (0..1): period doubling
  double doublingFrom = 0, doublingTo = 1e9;
  bool smoothDoubling = false;            // alternate as a cosine at f0/2 instead (weak f/2 partial)
  double highDoubling = 0;                // half-partials above 1.5 kHz, relative to their neighbours
};

// Additive synthesis at `rate`, 20 ms fades. Period doubling multiplies by
// 1 +- depth on alternate glottal cycles (a square wave at f0/2), which adds
// partials at the half-harmonics while the waveform still repeats every two
// periods. With `smoothDoubling` the gain follows a cosine at f0/2 instead: the
// partial at f0/2 stays 6-10 dB under f0 (the dev takes' doubled phonation),
// while a square wave puts it near f0's level. `highDoubling` instead adds
// half-partials above 1.5 kHz only (pressed phonation: the resolved low band
// stays clean).
std::vector<float> sing(const Voice& v, double rate, double total) {
  std::vector<float> out(static_cast<size_t>(total * rate), 0.0f);
  double phase = 0;  // in cycles
  for (size_t i = 0; i < out.size(); ++i) {
    const double t = i / rate;
    if (t < v.start || t >= v.end) continue;
    const double f0 = 440.0 * std::pow(2.0, (v.midi(t) - 69) / 12);
    phase += f0 / rate;
    const double fade = std::min({1.0, (t - v.start) / 0.02, (v.end - t) / 0.02});
    double sum = 0;
    for (size_t k = 0; k < v.harmonics.size(); ++k) {
      sum += v.harmonics[k] * std::sin(2 * kPiTest * (k + 1) * phase);
      if (v.highDoubling > 0 && (k + 1.5) * f0 >= 1500 && k + 1 < v.harmonics.size())
        sum += v.highDoubling * v.harmonics[k + 1] * std::sin(2 * kPiTest * (k + 1.5) * phase);
    }
    const double alternate = v.smoothDoubling ? std::cos(kPiTest * phase) : static_cast<long long>(std::floor(phase)) % 2 == 0 ? 1.0 : -1.0;
    const bool doubled = t >= v.doublingFrom && t < v.doublingTo;
    const double gain = doubled ? 1 + v.doubling * alternate : 1;
    out[i] = static_cast<float>(v.amplitude * fade * gain * sum);
  }
  return out;
}

float medianVoiced(const novox::PitchAnalysis& a, double from, double to) {
  std::vector<float> v;
  for (const auto& f : a.frames)
    if (f.time >= from && f.time <= to && f.midi > 0) v.push_back(f.midi);
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

// Share of voiced frames in [from, to] within `tolerance` semitones of `midi`.
double shareNear(const novox::PitchAnalysis& a, double from, double to, double midi, double tolerance = 1.0) {
  int voiced = 0, near = 0;
  for (const auto& f : a.frames)
    if (f.time >= from && f.time <= to && f.midi > 0) {
      ++voiced;
      if (std::fabs(f.midi - midi) <= tolerance) ++near;
    }
  return voiced ? static_cast<double>(near) / voiced : 0;
}

novox::NoteSpan span(double from, double to) {
  novox::NoteSpan s;
  s.first = static_cast<size_t>(std::lround(from / novox::kAnalysisHopSeconds));
  s.last = static_cast<size_t>(std::lround(to / novox::kAnalysisHopSeconds));
  return s;
}

float notePitch(const novox::PitchAnalysis& a, double from, double to) {
  std::vector<novox::NoteSpan> notes{span(from, to)};
  novox::assignNotePitches(a, notes);
  return notes[0].center;
}

int voicedFrames(const novox::PitchAnalysis& a, double from, double to) {
  int voiced = 0;
  for (const auto& f : a.frames) voiced += f.time >= from && f.time < to && f.midi > 0;
  return voiced;
}

// A harmonic tone added to `x` (16 kHz) whose level falls `decayDb` over its
// length: a held note fading out, so its soft end tests the silence floor.
void fadingTone(std::vector<float>& x, double start, double seconds, double hz, double amplitude, double decayDb) {
  for (size_t i = static_cast<size_t>(start * 16000); i < static_cast<size_t>((start + seconds) * 16000) && i < x.size(); ++i) {
    const double t = i / 16000.0 - start;
    const double level = amplitude * std::pow(10, -decayDb * t / seconds / 20) * std::min({1.0, t / 0.02, (seconds - t) / 0.02});
    double sum = 0;
    for (int k = 1; k <= 6; ++k) sum += std::pow(0.7, k - 1) * std::sin(2 * kPiTest * hz * k * t);
    x[i] += static_cast<float>(level * sum);
  }
}

// A rough syllable: clean partials of `hz` below 1.1 kHz plus aperiodic energy
// at 1.7-2.6 kHz (80 partials at unrelated frequencies). Full-band periodicity
// fails, so the path leaves it unvoiced, while the low band repeats cleanly.
std::vector<float> roughSyllable(double hz, double seconds, double amplitude) {
  std::vector<float> x(static_cast<size_t>(seconds * 16000));
  uint32_t seed = 4242;
  auto uniform = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
  std::vector<double> frequency, phase;
  for (int i = 0; i < 80; ++i) frequency.push_back(1700 + 900 * uniform()), phase.push_back(2 * kPiTest * uniform());
  const double harmonics[5] = {1.0, 0.7, 0.5, 0.35, 0.25};
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = i / 16000.0;
    double sum = 0;
    for (int k = 0; k < 5; ++k) sum += harmonics[k] * std::sin(2 * kPiTest * hz * (k + 1) * t);
    for (int j = 0; j < 80; ++j) sum += 0.12 * std::sin(2 * kPiTest * frequency[static_cast<size_t>(j)] * t + phase[static_cast<size_t>(j)]);
    x[i] = static_cast<float>(amplitude * std::min({1.0, t / 0.01, (seconds - t) / 0.01}) * sum);
  }
  return x;
}

// A harmonic tone at 16 kHz sounding from `start` to `end` (20 ms fades):
// harmonic k (1-based, below 3.5 kHz) of midi(t) at linear amplitude level(t, k).
std::vector<float> tone(const std::function<double(double)>& midi, const std::function<double(double, int)>& level, double start,
                        double end, double total) {
  std::vector<float> x(static_cast<size_t>(total * 16000), 0.0f);
  double phase = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = i / 16000.0;
    if (t < start || t >= end) continue;
    const double f0 = 440.0 * std::pow(2.0, (midi(t) - 69) / 12);
    phase += f0 / 16000.0;
    double sum = 0;
    for (int k = 1; k * f0 < 3500; ++k) sum += level(t, k) * std::sin(2 * kPiTest * k * phase);
    x[i] = static_cast<float>(0.2 * std::min({1.0, (t - start) / 0.02, (end - t) / 0.02}) * sum);
  }
  return x;
}

// dB over H2 of the R4 audit singer's bright G4-B4 voice before and after its
// vowel moves its formants off the odd harmonics (H1 stays ~19 dB under H2).
constexpr double kBrightOdd[12] = {-18, 0, -12.6, -8.8, -19.6, -26.5, -14, -36.5, -40, -40, -40, -40};
constexpr double kBrightEven[12] = {-19.5, 0, -23, -4.5, -30.7, -27.9, -31.6, -19.3, -40, -40, -40, -40};

}  // namespace

void testSingingPitchV2(int& checks, int& failures) {
  using novox::analyzeSingingPitch;

  // BEGIN fractional-period refinement regression tests.
  // Independent synthetic ground truth: measure pitch accuracy as well as
  // preserving the decoder's decisions, without relying on search internals.
  {
    const auto sameDecisions = [&](const novox::PitchAnalysis& before, const novox::PitchAnalysis& after) {
      V2_CHECK(before.frames.size() == after.frames.size());
      V2_CHECK(before.chosen == after.chosen);
      V2_CHECK(before.polyphonic == after.polyphonic && before.polyphony == after.polyphony);
      V2_CHECK(before.candidates.size() == after.candidates.size());
      if (before.frames.size() != after.frames.size() || before.candidates.size() != after.candidates.size()) return;
      for (size_t i = 0; i < before.frames.size(); ++i) {
        const auto& a = before.frames[i];
        const auto& b = after.frames[i];
        V2_CHECK(a.time == b.time && a.endTime == b.endTime && a.db == b.db);
        V2_CHECK(a.clarity == b.clarity && a.confidence == b.confidence);
        V2_CHECK((a.midi > 0) == (b.midi > 0));
        V2_CHECK(std::isfinite(b.midi) && std::isfinite(b.f0));
        V2_CHECK(b.confidence >= 0 && b.confidence <= 1);
        V2_CHECK(std::fabs(b.midi - a.midi) < 0.5f);  // fine pitch; never a new note or octave
        V2_CHECK(before.candidates[i].size() == after.candidates[i].size());
        if (before.candidates[i].size() != after.candidates[i].size()) continue;
        for (size_t j = 0; j < before.candidates[i].size(); ++j) {
          const auto& ca = before.candidates[i][j];
          const auto& cb = after.candidates[i][j];
          V2_CHECK(ca.cmnd == cb.cmnd && ca.emission == cb.emission);
          V2_CHECK(ca.harmonic == cb.harmonic && ca.fundamental == cb.fundamental);
        }
        if (after.chosen[i] >= 0) {
          const auto& c = after.candidates[i][static_cast<size_t>(after.chosen[i])];
          V2_CHECK(std::isfinite(c.lag) && c.lag > 0);
          V2_CHECK(c.midi == b.midi);
          V2_CHECK_NEAR(b.f0, 16000.0 / c.lag, 0.001);
          V2_CHECK_NEAR(b.midi, 69 + 12 * std::log2(b.f0 / 440.0), 0.0001);
        }
      }
    };
    const auto meanError = [&](const novox::PitchAnalysis& a, const std::function<double(double)>& expected,
                               double from, double to) {
      double sum = 0;
      int voiced = 0, total = 0;
      for (const auto& f : a.frames) {
        if (f.time < from || f.time > to) continue;
        ++total;
        if (f.midi <= 0) continue;
        sum += std::fabs(f.midi - expected(f.time));
        ++voiced;
      }
      V2_CHECK(total > 0 && voiced >= 0.98 * total);  // dropping hard frames is not an accuracy gain
      return voiced ? sum / voiced : 100.0;
    };

    // Fractional pitches span short periods and several phases of integer-lag
    // interpolation. Three partials stay below Nyquist throughout this grid.
    double coarseError = 0, refinedError = 0;
    for (double pitch : {80.19, 84.37, 86.27, 90.41, 94.19}) {
      Voice v;
      v.midi = [pitch](double) { return pitch; };
      v.harmonics = {1.0, 0.4, 0.2};
      const auto x = sing(v, 16000, 1.2);
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      coarseError += meanError(coarse, v.midi, 0.2, 1.0);
      refinedError += meanError(refined, v.midi, 0.2, 1.0);
    }
    V2_CHECK(refinedError < coarseError * 0.65);
    V2_CHECK(refinedError / 5 < 0.03);  // mean error below three cents

    // The lower register retains the original estimator exactly; the optional
    // refinement is reserved for short periods in the upper register.
    for (double pitch : {48.31, 64.27}) {
      Voice v;
      v.midi = [pitch](double) { return pitch; };
      const auto x = sing(v, 16000, 1.2);
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      for (size_t i = 0; i < coarse.frames.size(); ++i) {
        if (coarse.frames[i].time < 0.2 || coarse.frames[i].time > 1.0) continue;
        V2_CHECK(coarse.frames[i].f0 > 0 && coarse.frames[i].f0 < 400);
        V2_CHECK(coarse.frames[i].midi == refined.frames[i].midi);
        V2_CHECK(coarse.frames[i].f0 == refined.frames[i].f0);
        const auto choice = static_cast<size_t>(coarse.chosen[i]);
        if (coarse.chosen[i] >= 0)
          V2_CHECK(coarse.candidates[i][choice].lag == refined.candidates[i][choice].lag);
      }
    }

    // Scaling a clean waveform changes its level, not the estimated period.
    {
      Voice v;
      v.midi = [](double) { return 72.31; };
      v.harmonics = {1.0, 0.5, 0.25, 0.125};
      auto x = sing(v, 16000, 1.2);
      const auto loud = analyzeSingingPitch(x, true);
      for (float& sample : x) sample *= 0.05f;
      const auto quiet = analyzeSingingPitch(x, true);
      V2_CHECK(meanError(quiet, v.midi, 0.2, 1.0) < 0.03);
      double difference = 0;
      int count = 0;
      for (size_t i = 0; i < loud.frames.size(); ++i) {
        if (loud.frames[i].time < 0.2 || loud.frames[i].time > 1.0) continue;
        V2_CHECK(loud.frames[i].midi > 0 && quiet.frames[i].midi > 0);
        difference += std::fabs(loud.frames[i].midi - quiet.frames[i].midi);
        ++count;
      }
      V2_CHECK(count > 0 && difference / count < 0.005);  // less than half a cent on average
    }

    // A smaller pointwise error must not come from flattening vibrato.
    {
      Voice v;
      v.end = 1.7;
      v.harmonics = {1.0, 0.5, 0.25, 0.125};
      v.midi = [](double t) { return 72.31 + 0.7 * std::sin(2 * kPiTest * 5.5 * t); };
      const auto x = sing(v, 16000, 1.8);
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      const double before = meanError(coarse, v.midi, 0.2, 1.6);
      const double after = meanError(refined, v.midi, 0.2, 1.6);
      V2_CHECK(after < 0.04 && after <= before + 0.005);
      double projection = 0, energy = 0;
      for (const auto& f : refined.frames) {
        if (f.time < 0.2 || f.time > 1.6 || f.midi <= 0) continue;
        const double expected = v.midi(f.time) - 72.31;
        projection += (f.midi - 72.31) * expected;
        energy += expected * expected;
      }
      V2_CHECK(energy > 0 && projection / energy > 0.9 && projection / energy < 1.1);
    }

    // A 120 ms octave note, with boundaries between hops, survives in either
    // direction. Refinement must not move its observed crossing by a hop.
    for (int direction : {-1, 1}) {
      const double held = direction > 0 ? 60.19 : 72.19, shortPitch = held + 12 * direction;
      Voice v;
      v.midi = [held, shortPitch](double t) { return t >= 0.503 && t < 0.623 ? shortPitch : held; };
      v.harmonics = {1.0, 0.5, 0.25, 0.125};
      const auto x = sing(v, 16000, 1.2);
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      V2_CHECK(shareNear(refined, 0.535, 0.59, shortPitch, 0.25) > 0.9);
      V2_CHECK(shareNear(refined, 0.68, 1.0, held, 0.25) > 0.95);
      const auto crossing = [&](const novox::PitchAnalysis& a) {
        for (const auto& f : a.frames)
          if (f.time >= 0.47 && f.time < 0.56 && f.midi > 0 && direction * (f.midi - (held + shortPitch) / 2) >= 0)
            return f.time;
        return -1.0;
      };
      const double before = crossing(coarse), after = crossing(refined);
      V2_CHECK(before > 0 && after > 0);
      V2_CHECK(std::fabs(after - before) <= novox::kAnalysisHopSeconds + 1e-9);
      V2_CHECK(std::fabs(after - 0.503) <= 0.04);
    }

    // Moderate deterministic noise: do not trade a good low-register estimate
    // for a noise-fitting local minimum. RMS defines 15 dB input SNR.
    {
      Voice v;
      v.midi = [](double) { return 48.31; };
      v.harmonics = {1.0, 0.65, 0.4, 0.28, 0.2, 0.13, 0.09, 0.06};
      auto x = sing(v, 16000, 1.2);
      double energy = 0;
      for (size_t i = 3200; i < 16000; ++i) energy += static_cast<double>(x[i]) * x[i];
      const double noiseScale = std::sqrt(energy / 12800) * std::pow(10.0, -15.0 / 20);
      uint32_t seed = 271828;
      for (float& sample : x) {
        double normal = -6;
        for (int j = 0; j < 12; ++j) {
          seed = seed * 1664525u + 1013904223u;
          normal += (seed >> 8) / 16777216.0;
        }
        sample += static_cast<float>(noiseScale * normal);
      }
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      const double before = meanError(coarse, v.midi, 0.2, 1.0);
      const double after = meanError(refined, v.midi, 0.2, 1.0);
      V2_CHECK(after < 0.06 && after <= before + 0.03);
    }

    // With no useful waveform or too few samples, refinement stays finite and
    // preserves the existing voicing decision; silence never acquires a pitch.
    for (size_t size : {size_t{0}, size_t{32}, size_t{16000}}) {
      const std::vector<float> x(size, 0.0f);
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      V2_CHECK(std::none_of(refined.frames.begin(), refined.frames.end(), [](const novox::PitchFrame& f) { return f.midi > 0; }));
    }
    {
      Voice v;
      v.start = 0;
      v.end = 1.2;
      v.midi = [](double) { return 64.27; };
      const auto x = sing(v, 16000, 1.2);
      const auto coarse = analyzeSingingPitch(x, false), refined = analyzeSingingPitch(x, true);
      sameDecisions(coarse, refined);
      V2_CHECK(coarse.frames.front().midi == refined.frames.front().midi);
      V2_CHECK(coarse.frames.back().midi == refined.frames.back().midi);
    }
  }
  // END fractional-period refinement regression tests.

  // Tuning reference from note centres: a common offset is found from the
  // notes' fractional pitch (as a circular mean, so 59.6 and 60.6 agree), a few
  // notes sung out of tune do not move it, and an in-tune take, a take whose
  // notes scatter all around the grid, or an accompanied one keeps A440.
  {
    uint32_t seed = 99;
    const auto noise = [&](double sd) {  // deterministic, roughly normal
      double sum = 0;
      for (int k = 0; k < 12; ++k) seed = seed * 1664525u + 1013904223u, sum += (seed >> 8) / 16777216.0;
      return (sum - 6) * sd;
    };
    const auto take = [&](double offset, double sd, int notes, std::vector<float>& centres, std::vector<float>& weights) {
      centres.clear();
      weights.clear();
      for (int i = 0; i < notes; ++i) {
        centres.push_back(static_cast<float>(55 + (i * 7) % 12 + offset + noise(sd)));
        weights.push_back(0.4f);
      }
    };
    std::vector<float> c, w;
    take(-0.35, 0.08, 30, c, w);
    novox::TuningEstimate t = novox::estimateTuning(c, w);
    V2_CHECK_NEAR(t.offset, -0.35, 0.05);
    V2_CHECK_NEAR(novox::estimateTuning(c, w, 0.3f).measured, -0.35, 0.05);
    V2_CHECK(novox::estimateTuning(c, w, 0.3f).offset == 0);  // accompaniment: the band's A440
    for (int i = 0; i < 4; ++i) c[static_cast<size_t>(i)] += 0.45f;  // four notes sung far off
    V2_CHECK_NEAR(novox::estimateTuning(c, w).offset, -0.35, 0.05);
    take(-0.35, 0.08, 12, c, w);  // a short phrase: too few notes to trust an offset
    t = novox::estimateTuning(c, w);
    V2_CHECK(t.offset == 0 && std::fabs(t.measured + 0.35) < 0.05);
    take(0.45, 0.08, 30, c, w);  // nearly a quarter tone sharp: either grid is a guess, so A440
    t = novox::estimateTuning(c, w);
    V2_CHECK(t.offset == 0 && std::fabs(t.measured - 0.45) < 0.05);
    take(0.08, 0.08, 30, c, w);  // in tune
    t = novox::estimateTuning(c, w);
    V2_CHECK(t.offset == 0 && std::fabs(t.measured - 0.08) < 0.05);
    take(-0.3, 0.45, 30, c, w);  // intonation all over the grid: no common offset
    t = novox::estimateTuning(c, w);
    V2_CHECK(t.offset == 0 && t.strength < 4.6f);
    take(-0.4, 0.08, 3, c, w);  // three notes are too few to tell
    V2_CHECK(novox::estimateTuning(c, w).offset == 0);
    V2_CHECK(novox::estimateTuning({}, {}).offset == 0);
    V2_CHECK(novox::estimateTuning({60.4f, 0.0f}, {0.0f, 1.0f}).offset == 0);  // no weight, or no pitch
  }

  // Plain voices at several registers, at 16, 44.1 and 48 kHz.
  for (double rate : {16000.0, 44100.0, 48000.0}) {
    for (double pitch : {45.0, 57.0, 64.0, 72.0}) {
      Voice v;
      v.midi = [pitch](double) { return pitch; };
      const auto x = sing(v, rate, 1.2);
      const auto a = analyzeSingingPitch(novox::resampleTo16k(x.data(), x.size(), rate));
      V2_CHECK_NEAR(medianVoiced(a, 0.2, 1.0), pitch, 0.15);
      V2_CHECK(shareNear(a, 0.15, 1.05, pitch, 0.5) > 0.97);
      V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), pitch, 0.1);
      V2_CHECK(a.polyphony < 0.1f);
    }
  }

  // Pure sine tones (no harmonics) keep their octave, high and low.
  for (double pitch : {43.0, 55.0, 69.0, 79.0}) {
    Voice v;
    v.midi = [pitch](double) { return pitch; };
    v.harmonics = {1.0};
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, pitch, 0.5) > 0.97);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), pitch, 0.1);
  }

  // Whistling and the soprano / whistle register above C6 (up to C7): the pitch,
  // not a sub-multiple 12 or 19 semitones down. (Frame precision is coarser at
  // these lags of 8-15 samples; the note centre is not.)
  for (double pitch : {80.0, 84.0, 86.0, 90.0, 95.0}) {
    for (double vibrato : {0.0, 0.15}) {
      Voice v;
      v.midi = [pitch, vibrato](double t) { return pitch + vibrato * std::sin(2 * kPiTest * 6 * t); };
      v.harmonics = {1.0};
      const auto x = sing(v, 16000, 1.2);
      const auto a = analyzeSingingPitch(x);
      V2_CHECK(shareNear(a, 0.15, 1.05, pitch, 1.0) > 0.95);
      V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), pitch, 0.15);
    }
  }
  {
    Voice v;  // a sung D6 with a few weak harmonics
    v.midi = [](double) { return 86.0; };
    v.harmonics = {1.0, 0.25, 0.08};
    const auto x = sing(v, 16000, 1.2);
    V2_CHECK_NEAR(notePitch(analyzeSingingPitch(x), 0.12, 1.08), 86, 0.15);
  }
  // A compressed take's inhale whistle: a lone 1.76 kHz tone over breath noise
  // (rising with frequency), as loud as the singing, between two phrases around
  // Bb3. It leaves no voiced frames (a bounced take read it as A5 or A6, 2-3
  // octaves over its melody), clean or not. A whistled passage there, longer
  // than any inhale, stays (checked clean: breath noise breaks its voicing).
  for (double whistle : {0.3, 1.0}) {
    for (double noise : {0.08, 0.0}) {
      const double from = 4.15, to = from + whistle;
      Voice first, second;
      first.midi = [](double) { return 58.0; };
      first.start = 0.1, first.end = 4.0;
      second.midi = [](double) { return 53.0; };
      second.start = to + 0.15, second.end = to + 4.0;
      auto x = sing(first, 16000, to + 4.2);
      const auto y = sing(second, 16000, to + 4.2);
      uint32_t seed = 7;
      double last = 0;
      for (size_t i = 0; i < x.size(); ++i) {
        x[i] += y[i];
        const double t = i / 16000.0;
        if (t < from || t >= to) continue;
        seed = seed * 1664525u + 1013904223u;
        const double white = (seed >> 8) / 8388608.0 - 1;
        const double fade = std::min({1.0, (t - from) / 0.02, (to - t) / 0.02});
        x[i] += static_cast<float>(fade * (0.2 * std::sin(2 * kPiTest * 1760 * t) + noise * (white - last)));
        last = white;
      }
      const auto a = analyzeSingingPitch(x);
      V2_CHECK(shareNear(a, 0.2, 3.9, 58, 0.5) > 0.95);
      V2_CHECK(shareNear(a, to + 0.25, to + 3.9, 53, 0.5) > 0.95);
      if (whistle < 0.5) V2_CHECK(voicedFrames(a, from, to) == 0);
      else if (noise == 0) V2_CHECK(voicedFrames(a, from + 0.05, to - 0.05) >= 0.9 * (whistle - 0.1) / novox::kAnalysisHopSeconds);
    }
  }
  {
    // A soprano's C6 after her C5 phrase: a lone partial over 1 kHz, but one
    // octave over her median, so it is sung, not breathed.
    Voice phrase, top;
    phrase.midi = [](double) { return 72.0; };
    phrase.start = 0.1, phrase.end = 1.4;
    top.midi = [](double) { return 84.0; };
    top.harmonics = {1.0, 0.25, 0.08};
    top.start = 1.6, top.end = 2.0;
    auto x = sing(phrase, 16000, 2.2);
    const auto y = sing(top, 16000, 2.2);
    for (size_t i = 0; i < x.size(); ++i) x[i] += y[i];
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 1.65, 1.95, 84, 1.0) > 0.9);
  }
  // A low bass C2 (65 Hz) with a weak fundamental: its half period is no
  // period at all, so it must not be read as a pitch about an octave up.
  {
    Voice v;
    v.midi = [](double t) { return 36 + 0.15 * std::sin(2 * kPiTest * 5.5 * t); };
    v.harmonics = {0.35, 1.0, 0.8, 0.6, 0.5, 0.4, 0.3, 0.25, 0.2, 0.15, 0.1, 0.08};
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, 36, 0.5) > 0.9);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 36, 0.15);
  }

  // A DC offset (or a DC step) is removed before analysis, so the levels the
  // silence gates use are those of the sound.
  {
    std::vector<float> dc(4000, 0.25f);
    novox::removeDc(dc);
    V2_CHECK(std::all_of(dc.begin(), dc.end(), [](float s) { return s == 0.0f; }));
    std::vector<float> step(16000, 0.0f);
    std::fill(step.begin() + 8000, step.end(), 0.1f);
    novox::removeDc(step);
    V2_CHECK(std::fabs(step.back()) < 1e-4f && std::fabs(step[8000]) <= 0.1f);
    Voice v;
    v.midi = [](double) { return 50.0; };
    v.start = 0.4, v.end = 1.0;
    auto x = sing(v, 16000, 1.4);
    auto offset = x;
    for (auto& s : offset) s += 0.05f;
    novox::removeDc(x);
    novox::removeDc(offset);
    const auto a = analyzeSingingPitch(x), b = analyzeSingingPitch(offset);
    bool same = a.frames.size() == b.frames.size() && std::fabs(a.floorDb - b.floorDb) < 0.5f;
    for (size_t i = 0; same && i < a.frames.size(); ++i)
      same = (a.frames[i].midi > 0) == (b.frames[i].midi > 0) && std::fabs(a.frames[i].db - b.frames[i].db) < 0.5f;
    V2_CHECK(same);
    std::vector<float> empty;
    novox::removeDc(empty);
    V2_CHECK(empty.empty());
  }

  // Period doubling: alternate glottal cycles 30% louder/softer. The waveform
  // repeats more exactly at 2T, but the half-harmonics sit ~16 dB under the
  // harmonics, so the heard pitch (and the decoded one) is 1/T.
  {
    Voice v;
    v.midi = [](double) { return 55.0; };
    v.doubling = 0.3;
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, 55, 0.5) > 0.95);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 55, 0.15);
  }
  // Pressed phonation: half-partials as strong as the partials above 1.5 kHz,
  // the resolved low band clean. Still heard (and decoded) at 1/T.
  {
    Voice v;
    v.midi = [](double) { return 60.0; };
    v.highDoubling = 1.0;
    v.harmonics = {1.0, 0.8, 0.6, 0.6, 0.5, 0.5, 0.4, 0.4, 0.3, 0.3, 0.2, 0.2};
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, 60, 0.5) > 0.95);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 60, 0.15);
    V2_CHECK(a.polyphony < 0.1f);  // half-partials of one voice are not a second voice
  }
  // A 60 ms burst of strong doubling inside a held note must not flip the note's
  // octave, nor most of the burst's frames.
  {
    Voice v;
    v.midi = [](double) { return 57.0; };
    v.doubling = 0.8;
    v.doublingFrom = 0.6;
    v.doublingTo = 0.66;
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, 57, 0.5) > 0.95);
    V2_CHECK(shareNear(a, 0.6, 0.66, 45, 1.0) < 0.2);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 57, 0.15);
    const auto contour = novox::consistentContour(a, span(0.12, 1.08).first, span(0.12, 1.08).last);
    int off = 0;
    for (float m : contour)
      if (m > 0 && std::fabs(m - 57) > 1) ++off;
    V2_CHECK(off == 0);
  }

  // Period doubling setting in after a modal onset, with smoothly alternating
  // pulses: the doubled frames read an octave low (their f/2 partial is ~9 dB
  // under f, the onset's fundamental ~3 dB over its 2nd harmonic), but they are
  // subharmonic readings of the same voice, so the note keeps the onset's octave
  // and its contour is folded back up.
  {
    Voice v;
    v.midi = [](double) { return 57.0; };
    v.doubling = 0.7;
    v.smoothDoubling = true;
    v.doublingFrom = 0.3;
    v.end = 1.0;
    const auto x = sing(v, 16000, 1.1);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.35, 0.95, 45, 1.0) > 0.9);
    V2_CHECK_NEAR(notePitch(a, 0.12, 0.98), 57, 0.15);
    const auto contour = novox::consistentContour(a, span(0.12, 0.98).first, span(0.12, 0.98).last);
    int voiced = 0, off = 0;
    for (float m : contour)
      if (m > 0) ++voiced, off += std::fabs(m - 57) > 1;
    V2_CHECK(voiced > 80 && off == 0);
  }
  // A real octave drop inside one span keeps the voice's spectrum on both sides:
  // nothing is subharmonic and the span's evidence decides as before.
  {
    Voice v;
    v.midi = [](double t) { return t < 0.35 ? 69.0 : 57.0; };
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 0.32, 69, 0.5) > 0.9);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 57, 0.15);
  }

  // Weak fundamental: H1 20 dB under H2 but odd harmonics present. The pitch is
  // f0, not the strong second harmonic.
  {
    Voice v;
    v.midi = [](double) { return 57.0; };
    v.harmonics = {0.05, 0.5, 0.3, 0.25, 0.15, 0.1, 0.06, 0.04};
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, 57, 0.5) > 0.95);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 57, 0.15);
  }

  // A high voice at G4-C5 with H1 22 dB under H2 whose only strong odd harmonic
  // is the one nearest 2 kHz (a formant there), the rest 24 dB down: with only
  // H1 and H2 under 1 kHz the fixed evidence band hears H2 as the pitch; the
  // band follows the lower pitch (its fundamental is present), so the odd
  // harmonic counts.
  for (double midi : {67.0, 69.0, 71.0, 72.0}) {
    const double f0 = 440.0 * std::pow(2.0, (midi - 69) / 12);
    int formant = 3;
    for (int k = 5; k <= 11; k += 2)
      if (std::fabs(k * f0 - 2000) < std::fabs(formant * f0 - 2000)) formant = k;
    auto level = [formant](double, int k) { return std::pow(10.0, (k == 1 ? -22 : k == 2 ? 0 : k == formant ? -6 : -24) / 20.0); };
    const auto x = tone([midi](double) { return midi; }, level, 0.1, 1.1, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, midi, 0.5) > 0.95);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), midi, 0.15);
  }
  // ...while a period-doubled note in the same register keeps its pitch: its
  // half-partials stand up to 8 dB above both neighbours above 1.8 kHz (a dev
  // doubled "there" at 383 Hz, levels measured on the take), but its f/2
  // partial is absent (33 dB under f), so the lower pitch rests on those
  // half-partials alone and keeps the fixed band.
  for (double midi : {66.6, 69.0, 71.0, 72.0}) {
    static const double db[15] = {-37, -4.1, -19, 0, -20.1, -16.8, -22, -16.1, -12.9, -11.8, -13.6, -20.1, -13.9, -23.2, -30};
    auto level = [](double, int k) { return std::pow(10.0, (k <= 15 ? db[k - 1] : -40) / 20); };
    const auto x = tone([midi](double) { return midi - 12; }, level, 0.1, 1.1, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, midi, 0.5) > 0.95);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), midi, 0.15);
    // ...and as firmly as before: the octave under stays clearly worse (a
    // band that let those half-partials vote would halve the margin).
    int voiced = 0, firm = 0;
    for (size_t i = 0; i < a.frames.size(); ++i) {
      if (a.frames[i].time < 0.3 || a.frames[i].time > 0.9 || a.chosen[i] < 0) continue;
      const auto& cs = a.candidates[i];
      const auto& c = cs[static_cast<size_t>(a.chosen[i])];
      float margin = 9;
      for (const auto& o : cs)
        if (std::fabs(o.midi - (c.midi - 12)) < 0.75f) margin = std::min(margin, o.emission - c.emission);
      ++voiced, firm += margin >= 1.0f;
    }
    if (midi >= 69) V2_CHECK(voiced > 60 && firm > 0.9 * voiced);  // lower pitch over 196 Hz: the band would grow
  }
  // Octave flips: after a clean note at 70 (the register), the audit singer's
  // bright note at 66 whose odd harmonics fade at 1.5 s (spectra measured on the
  // audit take) while its fundamental stays ~19 dB under H2. Once they fade the
  // spectrum alone fits 78, and the path takes it, but the voice did not move:
  // its fundamental partial continues at its level across the jump, so the
  // frames and the notes stay at 66, also when the note then glides to 63.
  for (bool glide : {false, true}) {
    auto midi = [glide](double t) { return t < 1.03 ? 70.0 : glide && t > 1.9 ? 66 - 3 * std::min(1.0, (t - 1.9) / 0.2) : 66.0; };
    auto level = [](double t, int k) {
      const size_t i = static_cast<size_t>(std::min(k, 12) - 1);
      if (t < 1.03) return std::pow(10.0, -3.0 * (k - 1) / 20);
      const double w = std::clamp((t - 1.5) / 0.05, 0.0, 1.0);
      return std::pow(10.0, ((1 - w) * kBrightOdd[i] + w * kBrightEven[i]) / 20);
    };
    auto x = tone(midi, level, 0.1, 1.0, 2.4);
    const auto bright = tone(midi, level, 1.06, 2.3, 2.4);
    for (size_t i = 0; i < x.size(); ++i) x[i] += bright[i];
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 1.1, 1.88, 66, 0.75) > 0.95);
    if (glide) V2_CHECK(shareNear(a, 2.12, 2.25, 63, 0.75) > 0.95);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.98), span(1.08, 1.5), span(1.5, 1.9), span(2.1, 2.28)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 70, 0.15);
    V2_CHECK_NEAR(notes[1].center, 66, 0.15);
    V2_CHECK_NEAR(notes[2].center, 66, 0.15);
    V2_CHECK_NEAR(notes[3].center, glide ? 63 : 66, 0.3);
  }
  // A real instant octave jump up is no flip: the old fundamental is a release
  // under the new note, falling 2.4 dB per frame (a dev "the end": -24 to -43 dB
  // under the new fundamental within 80 ms), so it does not hold its level
  // over 80 ms; both notes keep their octave, also a short upper note, and with a
  // weak lower fundamental (H1 20 dB under H2) or a full one.
  for (double lowerH1 : {1.0, 0.1}) {
    auto midi = [](double t) { return t < 0.6 ? 55.0 : 67.0; };
    auto level = [lowerH1](double t, int k) { return t < 0.6 ? (k == 1 ? lowerH1 : 1.0 / k) : 1.0 / k; };
    auto x = tone(midi, level, 0.1, 0.72, 0.9);
    const double release = 0.2 * std::pow(10.0, -22 / 20.0);  // relative to the new fundamental (0.2)
    double phase = 0;
    for (size_t i = static_cast<size_t>(0.6 * 16000); i < static_cast<size_t>(0.72 * 16000); ++i) {
      const double t = i / 16000.0 - 0.6;
      phase += 196.0 / 16000;
      x[i] += static_cast<float>(release * std::exp(-t / 0.029) * std::sin(2 * kPiTest * phase));
    }
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 0.58, 55, 0.5) > 0.95);
    V2_CHECK(shareNear(a, 0.61, 0.7, 67, 0.5) > 0.9);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.6), span(0.6, 0.71)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 55, 0.15);
    V2_CHECK_NEAR(notes[1].center, 67, 0.3);
  }

  // Real octave leaps survive: legato with a 140 ms glide, both directions.
  for (int direction : {1, -1}) {
    const double from = direction > 0 ? 48 : 60, to = from + 12 * direction;
    Voice v;
    v.end = 1.3;
    v.midi = [from, to](double t) { return t < 0.6 ? from : t < 0.74 ? from + (to - from) * (t - 0.6) / 0.14 : to; };
    const auto x = sing(v, 16000, 1.4);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 0.58, from, 0.5) > 0.95);
    V2_CHECK(shareNear(a, 0.76, 1.25, to, 0.5) > 0.95);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.6), span(0.67, 1.28)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, from, 0.15);
    V2_CHECK_NEAR(notes[1].center, to, 0.15);
  }
  // ...and across a 40 ms unvoiced consonant gap (an instant leap, as across a /t/).
  for (int direction : {1, -1}) {
    const double from = direction > 0 ? 55 : 67, to = from + 12 * direction;
    Voice v;
    v.end = 1.3;
    v.midi = [from, to](double t) { return t < 0.62 ? from : to; };
    auto x = sing(v, 16000, 1.4);
    uint32_t noise = 77;
    for (size_t i = static_cast<size_t>(0.60 * 16000); i < static_cast<size_t>(0.64 * 16000); ++i) {
      noise = noise * 1664525u + 1013904223u;
      x[i] = 0.01f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
    }
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 0.58, from, 0.5) > 0.95);
    V2_CHECK(shareNear(a, 0.68, 1.25, to, 0.5) > 0.95);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.6), span(0.64, 1.28)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, from, 0.15);
    V2_CHECK_NEAR(notes[1].center, to, 0.15);
  }
  // A short (120 ms) real octave note between two notes, no glide.
  {
    Voice v;
    v.end = 0.98;
    v.midi = [](double t) { return t >= 0.5 && t < 0.62 ? 72.0 : 60.0; };
    const auto x = sing(v, 16000, 1.1);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.53, 0.59, 72, 0.5) > 0.9);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.5), span(0.5, 0.62), span(0.62, 0.96)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[1].center, 72, 0.3);
  }

  // Vibrato: 5.5 Hz, +-1 st around A3; the centre is the mean, not an extreme.
  {
    Voice v;
    v.end = 1.6;
    v.midi = [](double t) { return 57 + std::sin(2 * kPiTest * 5.5 * t); };
    const auto x = sing(v, 16000, 1.7);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.55, 57, 1.2) > 0.97);
    std::vector<novox::NoteSpan> notes{span(0.12, 1.58)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 57, 0.15);
    V2_CHECK(notes[0].spread > 1.0f);
  }
  // Vibrato over a slow approach (60 -> 62 in 400 ms, then held): the centre is
  // the held pitch, not the mean over all cycles (about 61.7).
  {
    Voice v;
    v.midi = [](double t) { return 60 + 2 * std::min(1.0, (t - 0.1) / 0.4) + 0.5 * std::sin(2 * kPiTest * 5.5 * t); };
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 62, 0.15);
  }
  // A scoop into a held note: the centre is the held pitch.
  {
    Voice v;
    v.midi = [](double t) { return t < 0.25 ? 57 + 3 * (t - 0.1) / 0.15 : 60.0; };
    const auto x = sing(v, 16000, 1.2);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 60, 0.15);
  }

  // Silence and near-silence: nothing voiced.
  {
    std::vector<float> silence(16000, 0.0f);
    auto a = analyzeSingingPitch(silence);
    V2_CHECK(a.frames.size() == novox::analysisFrameCount(silence.size()));
    V2_CHECK(std::none_of(a.frames.begin(), a.frames.end(), [](const novox::PitchFrame& f) { return f.midi > 0; }));
    for (size_t i = 0; i < silence.size(); ++i) silence[i] = 1e-6f * static_cast<float>(std::sin(i * 1.231));
    a = analyzeSingingPitch(silence);
    V2_CHECK(std::none_of(a.frames.begin(), a.frames.end(), [](const novox::PitchFrame& f) { return f.midi > 0; }));
    std::vector<novox::NoteSpan> notes{span(0.1, 0.5)};
    novox::assignNotePitches(a, notes);
    V2_CHECK(notes[0].center == 0 && notes[0].confidence == 0);
    V2_CHECK(analyzeSingingPitch({}).frames.empty());
    const auto tiny = analyzeSingingPitch(std::vector<float>(50, 0.1f));  // shorter than one hop
    V2_CHECK(tiny.frames.size() == 1 && tiny.candidates.size() == 1 && tiny.chosen.size() == 1);
  }
  // Breath noise is not a pitch.
  {
    std::vector<float> breath(16000);
    uint32_t noise = 12345;
    for (auto& s : breath) {
      noise = noise * 1664525u + 1013904223u;
      s = 0.2f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
    }
    const auto a = analyzeSingingPitch(breath);
    int voiced = 0;
    for (const auto& f : a.frames) voiced += f.midi > 0;
    V2_CHECK(voiced < 5);
  }

  // A chord (C4 + E4 + G4) is polyphonic; its common subharmonic C2 is not sung.
  {
    std::vector<float> chord(static_cast<size_t>(1.2 * 16000));
    for (size_t i = 0; i < chord.size(); ++i) {
      const double t = i / 16000.0;
      double s = 0;
      for (double midi : {60.0, 64.0, 67.0})
        for (int k = 1; k <= 4; ++k) s += std::pow(0.6, k - 1) * std::sin(2 * kPiTest * k * 440 * std::pow(2.0, (midi - 69) / 12) * t);
      chord[i] = static_cast<float>(0.08 * s * std::min(1.0, std::min(t, 1.2 - t) / 0.02));
    }
    const auto a = analyzeSingingPitch(chord);
    V2_CHECK(a.polyphony > 0.5f);
    V2_CHECK(shareNear(a, 0.1, 1.1, 36, 1.0) < 0.5);
  }

  // Noise: a voice 20 dB above white noise keeps its pitch.
  {
    Voice v;
    v.midi = [](double) { return 57.0; };
    auto x = sing(v, 16000, 1.2);
    uint32_t noise = 99;
    for (auto& s : x) {
      noise = noise * 1664525u + 1013904223u;
      s += 0.013f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
    }
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.15, 1.05, 57, 0.5) > 0.9);
    V2_CHECK_NEAR(notePitch(a, 0.12, 1.08), 57, 0.15);
  }

  // Note-level decisions on a hand-built analysis: 40 frames whose decoded path
  // sits an octave low (48) while every frame also offers 60 with better evidence.
  {
    novox::PitchAnalysis a;
    a.frames.resize(40);
    a.candidates.resize(40);
    a.chosen.assign(40, 0);
    for (size_t i = 0; i < 40; ++i) {
      a.frames[i].time = novox::analysisFrameTime(i);
      a.frames[i].midi = 48;
      a.frames[i].db = -20;
      a.frames[i].confidence = 0.8f;
      novox::PitchCandidateV2 low, high;
      low.midi = 48, low.cmnd = 0.01f, low.emission = 1.5f;
      high.midi = 60, high.cmnd = 0.05f, high.emission = 0.3f;
      a.candidates[i] = {low, high};
    }
    std::vector<novox::NoteSpan> notes(1);
    notes[0].first = 0, notes[0].last = 39;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK(notes[0].octaveShift == 12);
    // consistentContour follows the same evidence for the whole span...
    auto contour = novox::consistentContour(a, 0, 39);
    V2_CHECK(contour.size() == 40 && std::all_of(contour.begin(), contour.end(), [](float m) { return m == 60; }));
    // ...and with the evidence on the path's side, relabels only a short excursion.
    for (size_t i = 0; i < 40; ++i) {
      a.frames[i].midi = 60;
      a.chosen[i] = 1;
      a.candidates[i][0].emission = 1.5f;
    }
    for (size_t i = 10; i < 16; ++i) a.frames[i].midi = 48, a.chosen[i] = 0;  // 48 ms at the octave below
    contour = novox::consistentContour(a, 0, 39);
    V2_CHECK(std::all_of(contour.begin(), contour.end(), [](float m) { return m == 60; }));
    notes[0] = {};
    notes[0].first = 0, notes[0].last = 39;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK(notes[0].octaveShift == 0);
    // A real leap inside the span (400 ms at 60, then 400 ms at 72, each with its
    // own evidence) keeps both registers; a 48 ms dip to 48 inside the first and a
    // 40 ms jump to 84 inside the second are relabelled to their surroundings.
    novox::PitchAnalysis b;
    b.frames.resize(100);
    b.candidates.resize(100);
    b.chosen.resize(100);
    for (size_t i = 0; i < 100; ++i) {
      const bool first = i < 50;
      b.frames[i] = a.frames[0];
      b.frames[i].time = novox::analysisFrameTime(i);
      novox::PitchCandidateV2 c48, c60, c72, c84;
      c48.midi = 48, c48.emission = 1.5f;
      c60.midi = 60, c60.emission = first ? 0.2f : 1.5f;
      c72.midi = 72, c72.emission = first ? 1.5f : 0.2f;
      c84.midi = 84, c84.emission = 1.5f;
      c48.cmnd = c60.cmnd = c72.cmnd = c84.cmnd = 0.05f;
      b.candidates[i] = {c48, c60, c72, c84};
      b.chosen[i] = first ? 1 : 2;
      b.frames[i].midi = first ? 60 : 72;
    }
    for (size_t i = 20; i < 26; ++i) b.frames[i].midi = 48, b.chosen[i] = 0;
    for (size_t i = 80; i < 85; ++i) b.frames[i].midi = 84, b.chosen[i] = 3;
    contour = novox::consistentContour(b, 0, 99);
    V2_CHECK(contour.size() == 100);
    if (contour.size() == 100) {
      V2_CHECK(std::all_of(contour.begin(), contour.begin() + 50, [](float m) { return m == 60; }));
      V2_CHECK(std::all_of(contour.begin() + 50, contour.end(), [](float m) { return m == 72; }));
    }
  }

  // Subharmonic frames on a hand-built analysis: 40 loud frames read 57 with a
  // weak fundamental (f/2 partial 10 dB under f, the path's choice), then 10 frames
  // of modal singing at 69 whose fundamental is 30 dB over its 2nd harmonic.
  {
    auto build = [](float tailDb) {
      novox::PitchAnalysis a;
      a.frames.resize(50);
      a.candidates.resize(50);
      a.chosen.assign(50, 0);
      for (size_t i = 0; i < 50; ++i) {
        const bool tail = i >= 40;
        a.frames[i].time = novox::analysisFrameTime(i);
        a.frames[i].db = tail ? tailDb : -20;
        a.frames[i].confidence = 0.8f;
        novox::PitchCandidateV2 low, high;
        low.midi = 57, high.midi = 69;
        low.cmnd = tail ? 0.1f : 0.02f, high.cmnd = tail ? 0.05f : 0.6f;
        low.harmonic = tail ? -2.5f : -1.0f, high.harmonic = tail ? 0.0f : -2.0f;
        low.emission = tail ? 3.0f : 0.5f, high.emission = tail ? 0.1f : 2.5f;
        low.fundamental = tail ? -40.0f : -10.0f, high.fundamental = tail ? 30.0f : 3.0f;
        a.candidates[i] = tail ? std::vector<novox::PitchCandidateV2>{high, low} : std::vector<novox::PitchCandidateV2>{low, high};
        a.frames[i].midi = tail ? 69 : 57;
      }
      return a;
    };
    // Sung at the octave above: the 57 frames are its subharmonic readings.
    auto a = build(-20);
    std::vector<novox::NoteSpan> notes(1);
    notes[0].first = 0, notes[0].last = 49;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 69, 0.01);
    auto contour = novox::consistentContour(a, 0, 49);
    V2_CHECK(std::all_of(contour.begin(), contour.end(), [](float m) { return m == 69; }));
    // A quiet, near-sinusoidal tail (25 dB down) says nothing about the voice: 57 stays.
    a = build(-45);
    notes[0] = {};
    notes[0].first = 0, notes[0].last = 49;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 57, 0.01);
  }

  // The notes of one word share a register. A held syllable at 60 (40 frames
  // that rule 48 out), then a short one (12 frames) the path reads at 48 whose
  // frames fit 60 almost as well: alone it stays at 48 (its evidence beats the
  // prior on a leap between notes); as the word's second syllable it joins the
  // word at 60. When the short syllable's own frames rule 60 out (no candidate
  // there, 40 frames), the leap is real and survives inside the word too.
  {
    auto build = [](size_t secondFrames, bool contested) {
      novox::PitchAnalysis a;
      const size_t total = 40 + secondFrames;
      a.frames.resize(total);
      a.candidates.resize(total);
      a.chosen.assign(total, 0);
      for (size_t i = 0; i < total; ++i) {
        const bool second = i >= 40;
        a.frames[i].time = novox::analysisFrameTime(i);
        a.frames[i].db = -20;
        a.frames[i].confidence = 0.8f;
        a.frames[i].midi = second ? 48 : 60;
        novox::PitchCandidateV2 c60, c48;
        c60.midi = 60, c60.cmnd = 0.05f, c60.emission = second ? 1.2f : 0.2f;
        c48.midi = 48, c48.cmnd = 0.05f, c48.emission = second ? 0.5f : 1.5f;
        if (second && !contested) a.candidates[i] = {c48};
        else a.candidates[i] = second ? std::vector<novox::PitchCandidateV2>{c48, c60} : std::vector<novox::PitchCandidateV2>{c60, c48};
      }
      return a;
    };
    auto run = [](const novox::PitchAnalysis& a, long long firstWord, long long secondWord) {
      std::vector<novox::NoteSpan> notes(2);
      notes[0].first = 0, notes[0].last = 39, notes[0].word = firstWord;
      notes[1].first = 40, notes[1].last = a.frames.size() - 1, notes[1].word = secondWord;
      novox::assignNotePitches(a, notes);
      return notes;
    };
    auto a = build(12, true);
    auto notes = run(a, -1, -1);
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK_NEAR(notes[1].center, 48, 0.01);
    notes = run(a, 0, 1);  // two words: as before
    V2_CHECK_NEAR(notes[1].center, 48, 0.01);
    notes = run(a, 7, 7);  // one word
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK_NEAR(notes[1].center, 60, 0.01);
    V2_CHECK(notes[1].octaveShift == 12);
    // Given in any order, and across a gap longer than a phrase break (the
    // word's notes are decided together however far apart).
    std::vector<novox::NoteSpan> reversed(2);
    reversed[0].first = 40, reversed[0].last = 51, reversed[0].word = 3;
    reversed[1].first = 0, reversed[1].last = 39, reversed[1].word = 3;
    novox::assignNotePitches(a, reversed);
    V2_CHECK_NEAR(reversed[0].center, 60, 0.01);
    novox::PitchAnalysis apart = build(12, true);
    apart.frames.insert(apart.frames.begin() + 40, 50, novox::PitchFrame{});
    apart.candidates.insert(apart.candidates.begin() + 40, 50, std::vector<novox::PitchCandidateV2>{});
    apart.chosen.insert(apart.chosen.begin() + 40, 50, -1);
    for (size_t i = 0; i < apart.frames.size(); ++i) apart.frames[i].time = novox::analysisFrameTime(i);
    std::vector<novox::NoteSpan> far(2);
    far[0].first = 0, far[0].last = 39, far[0].word = 2;
    far[1].first = 90, far[1].last = 101, far[1].word = 2;
    novox::assignNotePitches(apart, far);
    V2_CHECK_NEAR(far[1].center, 60, 0.01);
    a = build(40, false);  // a real leap: both syllables rule the other octave out
    notes = run(a, 7, 7);
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK_NEAR(notes[1].center, 48, 0.01);
  }
  // A leap inside a word is priced by how firmly the less decided note holds
  // its own octave (its margin per frame of evidence, over at least 40 ms):
  // "dog-gie" 55 -> 64 with a 40 ms "gie" whose every frame rules 52 out keeps
  // the leap; with 16 ms of such frames "gie" follows its word to 52; and a
  // "gie" whose frames fit both octaves (regret 0.5 per frame) follows it too.
  {
    auto build = [](size_t secondFrames, bool contested) {
      novox::PitchAnalysis a;
      const size_t total = 40 + secondFrames;
      a.frames.resize(total);
      a.candidates.resize(total);
      a.chosen.assign(total, 0);
      for (size_t i = 0; i < total; ++i) {
        const bool second = i >= 40;
        a.frames[i].time = novox::analysisFrameTime(i);
        a.frames[i].db = -20;
        a.frames[i].confidence = 0.8f;
        a.frames[i].midi = second ? 64 : 55;
        novox::PitchCandidateV2 c, o;
        c.midi = second ? 64 : 55, c.cmnd = 0.05f, c.emission = 0.1f;
        o.midi = second ? 52 : 43, o.cmnd = 0.05f, o.emission = second ? 0.6f : 3.6f;  // "dog-" holds 55 firmly
        a.candidates[i] = second && !contested ? std::vector<novox::PitchCandidateV2>{c} : std::vector<novox::PitchCandidateV2>{c, o};
      }
      return a;
    };
    for (size_t frames : {5u, 2u})
      for (bool contested : {false, true}) {
        const auto a = build(frames, contested);
        std::vector<novox::NoteSpan> notes(2);
        notes[0].first = 0, notes[0].last = 39, notes[0].word = 1;
        notes[1].first = 40, notes[1].last = 39 + frames, notes[1].word = 1;
        novox::assignNotePitches(a, notes);
        V2_CHECK_NEAR(notes[0].center, 55, 0.01);
        V2_CHECK_NEAR(notes[1].center, frames == 5 && !contested ? 64 : 52, 0.01);
        notes[1].word = 2;  // two words: the leap between words keeps both octaves
        novox::assignNotePitches(a, notes);
        V2_CHECK_NEAR(notes[1].center, 64, 0.01);
      }
  }
  // A word whose second syllable is period-doubled throughout (smoothly
  // alternating pulses, f/2 partial ~7 dB under f, so the path reads it an
  // octave low and it has no modal frame of its own): its frames are judged
  // against the first syllable's modal frames and the word stays at 57. A real
  // octave leap inside a word (the same voice on both sides) keeps both octaves.
  {
    Voice v;
    v.midi = [](double) { return 57.0; };
    v.doubling = 0.9;
    v.smoothDoubling = true;
    v.doublingFrom = 0.53;
    v.end = 0.85;
    auto x = sing(v, 16000, 1.0);
    for (size_t i = static_cast<size_t>(0.47 * 16000); i < static_cast<size_t>(0.55 * 16000); ++i) x[i] *= 0.05f;  // a consonant
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(shareNear(a, 0.58, 0.82, 45, 1.0) > 0.9);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.46), span(0.56, 0.83)};
    notes[0].word = notes[1].word = 0;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 57, 0.15);
    V2_CHECK_NEAR(notes[1].center, 57, 0.15);
    for (int direction : {1, -1}) {
      Voice leap;
      leap.midi = [direction](double t) { return t < 0.5 ? 57.0 : 57.0 + 12 * direction; };
      leap.end = 0.85;
      auto y = sing(leap, 16000, 1.0);
      for (size_t i = static_cast<size_t>(0.47 * 16000); i < static_cast<size_t>(0.55 * 16000); ++i) y[i] *= 0.05f;
      const auto b = analyzeSingingPitch(y);
      std::vector<novox::NoteSpan> word{span(0.12, 0.46), span(0.56, 0.83)};
      word[0].word = word[1].word = 4;
      novox::assignNotePitches(b, word);
      V2_CHECK_NEAR(word[0].center, 57, 0.15);
      V2_CHECK_NEAR(word[1].center, 57 + 12 * direction, 0.15);
    }
  }
  // A syllable the path never voices (rough phonation) has no octave evidence of
  // its own: inside a word it takes the octave of the word's voiced note (its
  // low band reads 45 here, the word is sung at 57); alone it keeps its reading.
  {
    Voice v;
    v.midi = [](double) { return 57.0; };
    v.end = 0.5;
    auto x = sing(v, 16000, 1.0);
    const auto rough = roughSyllable(110, 0.25, 0.15);
    std::copy(rough.begin(), rough.end(), x.begin() + static_cast<long>(0.6 * 16000));
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(voicedFrames(a, 0.62, 0.83) == 0);
    std::vector<novox::NoteSpan> notes{span(0.12, 0.48), span(0.6, 0.85)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[1].center, 45, 0.2);
    notes[0].word = notes[1].word = 1;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 57, 0.15);
    V2_CHECK_NEAR(notes[1].center, 57, 0.2);
    V2_CHECK(notes[1].octaveShift == 12 && notes[1].confidence <= 0.3f);
  }
  // ...and only by an octave relation the frames' own alternatives use (12,
  // 19 or 24 st, octaveOffset): a reading 19 st over the word's voiced
  // syllable (its 3rd harmonic) joins it, while one 16 st over is a sung
  // interval and keeps its pitch.
  for (const float reading : {79.0f, 76.0f}) {
    novox::PitchAnalysis a;
    a.frames.resize(60);
    a.candidates.resize(60);
    a.chosen.assign(60, -1);
    for (size_t i = 0; i < 60; ++i) {
      a.frames[i].time = novox::analysisFrameTime(i);
      a.frames[i].db = -20;
      novox::PitchCandidateV2 c;
      c.cmnd = 0.1f, c.emission = 0.3f, c.midi = reading;
      if (i < 40) {
        novox::PitchCandidateV2 o;
        c.midi = 60, c.cmnd = 0.05f, c.emission = 0.2f;
        o.midi = 48, o.cmnd = 0.05f, o.emission = 1.5f;
        a.frames[i].midi = 60, a.frames[i].confidence = 0.8f, a.chosen[i] = 0;
        a.candidates[i] = {c, o};
      } else {
        a.candidates[i] = {c};  // never voiced: the note's pitch comes from its candidates
      }
    }
    std::vector<novox::NoteSpan> notes(2);
    notes[0].first = 0, notes[0].last = 39, notes[0].word = 3;
    notes[1].first = 40, notes[1].last = 59, notes[1].word = 3;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK_NEAR(notes[1].center, reading == 79.0f ? 60 : 76, 0.01);
  }

  // Accompanied take: frames holding a second harmonic series carry no octave
  // evidence, so a note whose frames are all polyphonic (path at 48, the bass
  // an octave under the voice, no candidate at 60) follows its neighbours at 60.
  // The same frames in a solo take keep their evidence.
  {
    novox::PitchAnalysis a;
    a.frames.resize(90);
    a.candidates.resize(90);
    a.chosen.assign(90, 0);
    a.polyphonic.assign(90, false);
    for (size_t i = 0; i < 90; ++i) {
      const bool middle = i >= 30 && i < 60;
      a.frames[i].time = novox::analysisFrameTime(i);
      a.frames[i].db = -20;
      a.frames[i].confidence = 0.8f;
      a.frames[i].midi = middle ? 48 : 60;
      novox::PitchCandidateV2 c60, c48;
      c60.midi = 60, c60.cmnd = 0.05f, c60.emission = 0.2f;
      c48.midi = 48, c48.cmnd = 0.02f, c48.emission = middle ? 0.2f : 3.0f;
      a.candidates[i] = middle ? std::vector<novox::PitchCandidateV2>{c48} : std::vector<novox::PitchCandidateV2>{c60, c48};
      a.polyphonic[i] = middle;
    }
    auto run = [&](float polyphony) {
      a.polyphony = polyphony;
      std::vector<novox::NoteSpan> notes(3);
      for (size_t n = 0; n < 3; ++n) notes[n].first = 30 * n, notes[n].last = 30 * n + 29;
      novox::assignNotePitches(a, notes);
      return notes;
    };
    auto notes = run(0.6f);
    V2_CHECK_NEAR(notes[0].center, 60, 0.01);
    V2_CHECK_NEAR(notes[1].center, 60, 0.01);
    V2_CHECK(notes[1].octaveShift == 12);
    V2_CHECK_NEAR(notes[2].center, 60, 0.01);
    notes = run(0.1f);
    V2_CHECK_NEAR(notes[1].center, 48, 0.01);
  }

  // A low voice under an upper chord keeps its register. The chord makes the
  // spectrum polyphonic, but its upper tones must not automatically become the
  // singer's melody, even when they overlap the voice's harmonics.
  for (double midi : {45.0, 55.0}) {
    for (double padLevel : {0.05, 0.1}) {
      Voice voice;
      voice.start = 0.1;
      voice.end = 1.9;
      voice.midi = [midi](double t) { return midi + 0.2 * std::sin(2 * kPiTest * 5.5 * t); };
      auto x = sing(voice, 16000, 2.0);
      for (double chordNote : {midi + 12, midi + 16, midi + 19}) {
        Voice pad;
        pad.start = voice.start;
        pad.end = voice.end;
        pad.midi = [chordNote](double) { return chordNote; };
        pad.amplitude = padLevel;
        pad.harmonics = {1.0, 0.5, 0.25, 0.125};
        const auto tone = sing(pad, 16000, 2.0);
        for (size_t i = 0; i < x.size(); ++i) x[i] += tone[i];
      }
      const auto a = analyzeSingingPitch(x);
      V2_CHECK(a.polyphony > 0.5f);
      V2_CHECK_NEAR(notePitch(a, 0.2, 1.8), midi, 0.2);
    }
  }

  // Mixture evidence is acoustic, not the register-biased frame cost: a
  // sustained upper candidate supported by both waveform and spectrum can
  // recover the singer despite the decoded path following the lower octave.
  // An isolated attack with the same reading is insufficient.
  for (bool sustained : {false, true}) {
    for (bool straySubharmonic : {false, true}) {
      novox::PitchAnalysis a;
      a.frames.resize(40);
      a.candidates.resize(40);
      a.chosen.assign(40, 0);
      a.polyphonic.assign(40, true);
      a.polyphony = 0.8f;
      for (size_t i = 0; i < 40; ++i) {
        a.frames[i].midi = 48;
        a.frames[i].db = -20;
        a.frames[i].confidence = 0.8f;
        novox::PitchCandidateV2 lower, upper;
        lower.midi = 48, lower.cmnd = 0.24f, lower.harmonic = -0.5f, lower.emission = 0.2f;
        upper.midi = 60, upper.cmnd = sustained || i < 2 ? 0.18f : 0.4f;
        upper.harmonic = 0, upper.emission = 1.6f;
        a.candidates[i] = {lower, upper};
        if (straySubharmonic && i == 0) {
          novox::PitchCandidateV2 stray;
          stray.midi = 36, stray.cmnd = 0.8f, stray.harmonic = -1, stray.emission = 3;
          a.candidates[i].push_back(stray);
        }
      }
      std::vector<novox::NoteSpan> notes(1);
      notes[0].first = 0, notes[0].last = 39;
      novox::assignNotePitches(a, notes);
      V2_CHECK_NEAR(notes[0].center, sustained ? 60 : 48, 0.01);
    }
  }

  // Rejecting a still lower subharmonic does not disprove the voice above it.
  // The middle mixture favours 48 over 36, while neighbouring clean notes
  // establish 60. Evidence about the 36/48 pair must not vote against 60.
  {
    novox::PitchAnalysis a;
    a.frames.resize(90);
    a.candidates.resize(90);
    a.chosen.assign(90, 0);
    a.polyphonic.assign(90, false);
    a.polyphony = 0.8f;
    for (size_t i = 0; i < 90; ++i) {
      const bool middle = i >= 30 && i < 60;
      a.frames[i].midi = middle ? 48 : 60;
      a.frames[i].db = -20;
      a.frames[i].confidence = 0.8f;
      novox::PitchCandidateV2 low, bass, voice;
      low.midi = 36, low.cmnd = 0.3f, low.harmonic = -1, low.emission = 2;
      bass.midi = 48, bass.cmnd = 0.1f, bass.harmonic = 0, bass.emission = middle ? 0.2f : 3;
      voice.midi = 60, voice.cmnd = middle ? 0.4f : 0.05f;
      voice.harmonic = middle ? -0.5f : 0, voice.emission = middle ? 1.5f : 0.2f;
      a.candidates[i] = middle ? std::vector<novox::PitchCandidateV2>{bass, low, voice}
                               : std::vector<novox::PitchCandidateV2>{voice, bass};
      a.polyphonic[i] = middle;
    }
    std::vector<novox::NoteSpan> notes(3);
    for (size_t n = 0; n < 3; ++n) notes[n].first = 30 * n, notes[n].last = 30 * n + 29;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[1].center, 60, 0.01);
  }

  // A real third register, supported by clean frames, remains available even
  // if the rest of the note contains a stronger upper instrument.
  {
    novox::PitchAnalysis a;
    a.frames.resize(40);
    a.candidates.resize(40);
    a.chosen.assign(40, 0);
    a.polyphonic.assign(40, true);
    a.polyphony = 0.7f;
    for (size_t i = 0; i < 40; ++i) {
      novox::PitchCandidateV2 voice, lower, upper;
      voice.midi = 36, voice.cmnd = 0.05f, voice.harmonic = 0, voice.emission = 0.2f;
      lower.midi = 48, lower.cmnd = 0.24f, lower.harmonic = -0.5f, lower.emission = i < 12 ? 3 : 0.2f;
      upper.midi = 60, upper.cmnd = 0.18f, upper.harmonic = 0, upper.emission = i < 12 ? 3 : 1.6f;
      a.frames[i].midi = i < 12 ? 36 : 48;
      a.frames[i].db = -20;
      a.frames[i].confidence = 0.8f;
      a.polyphonic[i] = i >= 12;
      a.candidates[i] = i < 12 ? std::vector<novox::PitchCandidateV2>{voice, lower, upper}
                               : std::vector<novox::PitchCandidateV2>{lower, upper};
    }
    std::vector<novox::NoteSpan> notes(1);
    notes[0].first = 0, notes[0].last = 39;
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 36, 0.01);
  }

  // Recording edges: a voice from the first to the last sample.
  {
    Voice v;
    v.midi = [](double) { return 62.0; };
    v.start = -1;
    v.end = 10;
    const auto x = sing(v, 16000, 0.6);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(a.frames.size() == novox::analysisFrameCount(x.size()));
    V2_CHECK(a.frames.front().midi > 0 && std::fabs(a.frames.front().midi - 62) < 0.5);
    V2_CHECK(a.frames.back().midi > 0 && std::fabs(a.frames.back().midi - 62) < 0.5);
    V2_CHECK(a.frames.back().endTime <= a.duration + 1e-9);
    V2_CHECK(a.candidates.size() == a.frames.size() && a.chosen.size() == a.frames.size());
    for (size_t i = 0; i < a.frames.size(); ++i)
      if (a.chosen[i] >= 0) V2_CHECK(a.candidates[i][static_cast<size_t>(a.chosen[i])].midi == a.frames[i].midi);
    // Out-of-range spans are handled.
    std::vector<novox::NoteSpan> notes{span(0.5, 5.0)};
    notes[0].first = a.frames.size() + 3;
    novox::assignNotePitches(a, notes);
    V2_CHECK(notes[0].center == 0);
    V2_CHECK(novox::consistentContour(a, a.frames.size() + 1, a.frames.size() + 5).empty());
  }

  // Level: a note fading out over 60 dB is decoded the same at -20 and -40 dB
  // gain (the silence floor follows the take, down to 16-bit resolution).
  {
    std::vector<float> x(32000, 0.0f);
    fadingTone(x, 0.2, 1.5, 220, 0.3, 60);
    const int full = voicedFrames(analyzeSingingPitch(x), 0, 2);
    V2_CHECK(full > 150);
    for (float gain : {0.1f, 0.01f}) {
      std::vector<float> quiet = x;
      for (float& s : quiet) s *= gain;
      V2_CHECK(std::abs(voicedFrames(analyzeSingingPitch(quiet), 0, 2) - full) <= 2);
    }
  }
  // ... and a section 30 dB quieter than the rest of the take keeps its soft end:
  // its floor follows its own loud frames, not the take's.
  {
    std::vector<float> x(7 * 16000, 0.0f);
    fadingTone(x, 0.2, 1.5, 220, 0.3, 60);
    fadingTone(x, 5.0, 1.5, 220, 0.3 * std::pow(10, -30 / 20.0), 60);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(voicedFrames(a, 4.5, 7) >= voicedFrames(a, 0, 2.5) - 10);
  }
  // A rough syllable the path leaves unvoiced still gets its pitch from the
  // low band's candidates, with low confidence (the segmenter may need it for
  // a lyric syllable).
  {
    std::vector<float> x(16000, 0.0f);
    const auto syllable = roughSyllable(220, 0.2, 0.15);
    std::copy(syllable.begin(), syllable.end(), x.begin() + 4800);
    const auto a = analyzeSingingPitch(x);
    V2_CHECK(voicedFrames(a, 0.3, 0.5) == 0);
    std::vector<novox::NoteSpan> notes{span(0.3, 0.5)};
    novox::assignNotePitches(a, notes);
    V2_CHECK_NEAR(notes[0].center, 57, 0.2);
    V2_CHECK(notes[0].confidence > 0 && notes[0].confidence <= 0.3f);
  }

  // Resampling keeps the band below ~6 kHz and removes what lies above the 8 kHz
  // analysis Nyquist at every input rate: sibilance at 9-12 kHz must not fold
  // into the vowel and frication bands (at 192 kHz it once came through at -11 dB).
  for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
    auto level = [rate](double hz) {  // dB of the 16 kHz output relative to the input tone
      std::vector<float> x(static_cast<size_t>(rate));
      for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2 * kPiTest * hz * i / rate));
      const auto y = novox::resampleTo16k(x.data(), x.size(), rate);
      double e = 0;
      for (size_t i = 1000; i + 1000 < y.size(); ++i) e += static_cast<double>(y[i]) * y[i];
      return 10 * std::log10(e / static_cast<double>(y.size() - 2000) / 0.125 + 1e-15);
    };
    V2_CHECK(std::fabs(level(1000)) < 0.2 && std::fabs(level(5000)) < 0.5);
    V2_CHECK(level(9000) < -40 && level(10000) < -40 && level(12000) < -40);
  }
}
