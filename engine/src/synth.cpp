#include "synth.hpp"

#include <algorithm>
#include <cmath>

namespace nvs {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 32;          // control-rate block, samples
constexpr int kOversample = 4;      // glottal source oversampling
constexpr float kNasalPoleHz = 270.0f;

// Level balance between the sources, tuned by ear and by RMS measurement
// (see tools/levels.py).
constexpr float kVoiceGain = 1.0f;
constexpr float kAspGain = 0.10f;
constexpr float kFricGain = 0.30f;
constexpr float kBypassGain = 0.12f;

float smoothstep(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return static_cast<float>(x * x * (3 - 2 * x));
}

class Rng {
 public:
  explicit Rng(uint32_t seed) : s_(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  float uniform() { return static_cast<float>(next() >> 8) / 16777216.0f; }  // [0,1)
  float bipolar() { return uniform() * 2.0f - 1.0f; }

 private:
  uint32_t s_;
};

// Smooth random wander in [-1, 1], changing at roughly `rate` Hz.
class Wander {
 public:
  explicit Wander(uint32_t seed) : rng_(seed) {
    a_ = rng_.bipolar();
    b_ = rng_.bipolar();
  }
  float step(double dt, double rate) {
    pos_ += dt * rate;
    while (pos_ >= 1.0) {
      pos_ -= 1.0;
      a_ = b_;
      b_ = rng_.bipolar();
    }
    return a_ + (b_ - a_) * smoothstep(pos_);
  }

 private:
  Rng rng_;
  double pos_ = 0;
  float a_ = 0, b_ = 0;
};

// Klatt two-pole resonator, unity gain at DC.
struct Resonator {
  double a = 1, b = 0, c = 0, y1 = 0, y2 = 0;
  void set(double f, double bw, double sr) {
    double r = std::exp(-kPi * bw / sr);
    c = -r * r;
    b = 2 * r * std::cos(2 * kPi * f / sr);
    a = 1 - b - c;
  }
  double tick(double x) {
    double y = a * x + b * y1 + c * y2;
    y2 = y1;
    y1 = y;
    return y;
  }
};

// Klatt antiresonator (zero pair), unity gain at DC.
struct AntiResonator {
  double a = 1, b = 0, c = 0, x1 = 0, x2 = 0;
  void set(double f, double bw, double sr) {
    double r = std::exp(-kPi * bw / sr);
    double cc = -r * r;
    double bb = 2 * r * std::cos(2 * kPi * f / sr);
    double aa = 1 - bb - cc;
    a = 1 / aa;
    b = -bb / aa;
    c = -cc / aa;
  }
  double tick(double x) {
    double y = a * x + b * x1 + c * x2;
    x2 = x1;
    x1 = x;
    return y;
  }
};

// RBJ biquad; used for band-pass noise peaks (0 dB peak) and peaking EQ.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void bandpass(double f, double bw, double sr) {
    double w = 2 * kPi * f / sr;
    double q = std::max(0.3, f / std::max(1.0, bw));
    double alpha = std::sin(w) / (2 * q);
    double a0 = 1 + alpha;
    b0 = alpha / a0; b1 = 0; b2 = -alpha / a0;
    a1 = -2 * std::cos(w) / a0; a2 = (1 - alpha) / a0;
  }
  void lowpass(double f, double q, double sr) {
    double w = 2 * kPi * f / sr;
    double alpha = std::sin(w) / (2 * q);
    double cw = std::cos(w);
    double a0 = 1 + alpha;
    b0 = (1 - cw) / 2 / a0; b1 = (1 - cw) / a0; b2 = b0;
    a1 = -2 * cw / a0; a2 = (1 - alpha) / a0;
  }
  void peaking(double f, double q, double dB, double sr) {
    double A = std::pow(10.0, dB / 40.0);
    double w = 2 * kPi * f / sr;
    double alpha = std::sin(w) / (2 * q);
    double a0 = 1 + alpha / A;
    b0 = (1 + alpha * A) / a0; b1 = -2 * std::cos(w) / a0; b2 = (1 - alpha * A) / a0;
    a1 = -2 * std::cos(w) / a0; a2 = (1 - alpha / A) / a0;
  }
  double tick(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

class VoiceRenderer {
 public:
  VoiceRenderer(const Plan& plan, const Voice& v, double sr, uint32_t seed, float detuneCents, double delay)
      : plan_(plan), v_(v), sr_(sr), rng_(seed), pitchWander_(seed * 7 + 1),
        f1Wander_(seed * 13 + 2), f2Wander_(seed * 17 + 3), detune_(detuneCents), delay_(delay) {
    singer_.peaking(v.singerFormantHz * std::sqrt(v.formantScale), 1.4, v.singerFormantDb, sr);
    for (int i = 0; i < v.eqBands; ++i) eq_[i].peaking(v.eq[i][0], std::max(0.2f, v.eq[i][2]), v.eq[i][1], sr);
    // Brightness -> one-pole tilt coefficient on the glottal source.
    tiltCoef_ = std::clamp(0.5 - 0.4 * v.brightness, 0.0, 0.85);
    // 8th-order Butterworth low-pass on the voiced sound only. Harmony's voices
    // stop sharply at ~4.4 kHz while their fricatives go higher.
    if (v.bandLimitHz > 0 && v.bandLimitHz < sr * 0.45) {
      static const double kQ[4] = {0.5098, 0.6013, 0.9000, 2.5629};
      for (int i = 0; i < 4; ++i) bandLimit_[i].lowpass(v.bandLimitHz, kQ[i], sr);
      useBandLimit_ = true;
    }
    // Fricative noise rolls off above ~9 kHz, so S doesn't become a hiss.
    fricLp_.lowpass(std::min(9000.0, sr * 0.45), 0.707, sr);
  }

  void render(std::vector<float>& out) {
    const size_t total = out.size();
    const double dt = kBlock / sr_;
    for (size_t block = 0; block < total; block += kBlock) {
      double t = (static_cast<double>(block) + kBlock / 2.0) / sr_ - delay_;
      Params p = paramsAt(t);
      double f0 = f0At(t, dt);
      configure(p, f0, dt);
      size_t end = std::min(total, block + kBlock);
      for (size_t i = block; i < end; ++i) out[i] += static_cast<float>(sample(p, f0));
    }
  }

 private:
  const Plan& plan_;
  const Voice& v_;
  double sr_;
  Rng rng_;
  Wander pitchWander_, f1Wander_, f2Wander_;
  float detune_;
  double delay_;

  size_t seg_ = 0;
  size_t note_ = 0;

  // Source state
  double phase_ = 0;
  double tiltCoef_ = 0.35, tiltState_ = 0;
  double periodJitter_ = 1.0;

  // Filters
  Resonator formant_[5];
  Resonator highFormant_[3];
  Resonator nasalPole_;
  AntiResonator nasalZero_;
  Biquad noiseBand_[3];
  Biquad singer_;
  Biquad bandLimit_[4];
  Biquad fricLp_;
  Biquad eq_[4];
  bool useBandLimit_ = false;
  double dcX_ = 0, dcY_ = 0;
  double noiseLp_ = 0;

  // Per-block amplitudes (smoothed per sample to avoid zipper noise)
  double av_ = 0, ah_ = 0, af_ = 0, ab_ = 0, na_[3] = {0, 0, 0};
  double avT_ = 0, ahT_ = 0, afT_ = 0, abT_ = 0, naT_[3] = {0, 0, 0};

  Params paramsAt(double t) {
    const auto& segs = plan_.segments;
    if (segs.empty()) return Params{};
    while (seg_ + 1 < segs.size() && t >= segs[seg_].end) ++seg_;
    const Segment& cur = segs[seg_];
    Params p = cur.p;
    if (t < cur.start || t > cur.end) return p;
    const Segment* prev = seg_ > 0 ? &segs[seg_ - 1] : nullptr;
    const Segment* next = seg_ + 1 < segs.size() ? &segs[seg_ + 1] : nullptr;
    double curLen = cur.end - cur.start;
    for (int j = 0; j < NUM_PARAMS; ++j) {
      bool amp = isAmplitudeParam(j);
      if (prev) {
        double w = amp ? cur.tAmp : cur.tFreq;
        w = std::min({w, curLen, prev->end - prev->start});
        if (w > 0 && t < cur.start + w / 2) {
          float a = smoothstep((t - (cur.start - w / 2)) / w);
          p[j] = prev->p[j] + (cur.p[j] - prev->p[j]) * a;
          continue;
        }
      }
      if (next) {
        double w = amp ? next->tAmp : next->tFreq;
        w = std::min({w, curLen, next->end - next->start});
        if (w > 0 && t > cur.end - w / 2) {
          float a = smoothstep((t - (cur.end - w / 2)) / w);
          p[j] = cur.p[j] + (next->p[j] - cur.p[j]) * a;
        }
      }
    }
    return p;
  }

  double f0At(double t, double dt) {
    const auto& notes = plan_.notes;
    if (notes.empty()) return 220.0;
    while (note_ + 1 < notes.size() && t >= notes[note_ + 1].start) ++note_;
    const PitchNote* n = &notes[note_];
    // In a rest, move early to the next note so its onset consonants are on pitch.
    if (t > n->end && note_ + 1 < notes.size() && t > notes[note_ + 1].start - 0.2) n = &notes[note_ + 1];
    if (t < n->start && note_ == 0) n = &notes[0];

    double midi = n->midi;
    double age = t - n->start;
    size_t idx = static_cast<size_t>(n - notes.data());

    // Legato glide from the previous note.
    const double porta = std::max(0.005f, v_.portamento);
    if (n->legato && idx > 0 && age < porta * 0.7) {
      double from = notes[idx - 1].midi;
      midi = from + (n->midi - from) * smoothstep((age + porta * 0.3) / porta);
    } else if (idx + 1 < notes.size() && notes[idx + 1].legato && t > notes[idx + 1].start - porta * 0.3) {
      double to = notes[idx + 1].midi;
      double a = t - (notes[idx + 1].start - porta * 0.3);
      midi = n->midi + (to - n->midi) * smoothstep(a / porta);
    }

    // Scoop into notes that start a phrase.
    if (!n->legato && v_.scoop != 0.0f) {
      double st = std::max(0.005f, v_.scoopTime);
      midi += v_.scoop * (1.0 - smoothstep(age / st));
    }

    // Vibrato, delayed and ramped in.
    if (v_.vibratoDepth > 0 && age > v_.vibratoDelay) {
      double depth = v_.vibratoDepth * smoothstep((age - v_.vibratoDelay) / std::max(0.01f, v_.vibratoRamp));
      midi += depth * std::sin(2 * kPi * v_.vibratoRate * (age - v_.vibratoDelay));
    }

    // Slow random wander + choir detune.
    midi += (v_.jitterPitch * pitchWander_.step(dt, v_.jitterSpeed) + detune_) / 100.0;
    midi += v_.octave * 12 + v_.transpose;
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
  }

  void configure(const Params& p, double f0, double dt) {
    const double nyq = sr_ * 0.45;
    const double scale = v_.formantScale;
    const double jf1 = 1.0 + v_.jitterFormant * f1Wander_.step(dt, v_.jitterSpeed * 1.3);
    const double jf2 = 1.0 + v_.jitterFormant * f2Wander_.step(dt, v_.jitterSpeed * 1.1);

    double f[5] = {p[F1] * scale * v_.formantCoeff[0] * jf1, p[F2] * scale * v_.formantCoeff[1] * jf2,
                   p[F3] * scale * v_.formantCoeff[2], p[F4] * scale * v_.formantCoeff[3],
                   p[F5] * scale * v_.formantCoeff[4]};
    double bw[5] = {p[B1], p[B2], p[B3], p[B4], p[B5]};

    // Sopranos tune F1 above the fundamental; without this, high notes lose
    // their vowel and get quiet.
    if (p[AV] > 0.3f && f[0] < f0 * 1.1) f[0] = f0 * 1.1;
    if (f[1] < f[0] + 250) f[1] = f[0] + 250;
    for (int i = 0; i < 5; ++i) {
      f[i] = std::min(f[i], nyq);
      bw[i] = std::max(20.0, bw[i] * v_.bandwidthScale);
      formant_[i].set(f[i], bw[i], sr_);
    }
    // Fixed upper formants: without them a 5-formant cascade has no energy
    // above ~4 kHz at modern sample rates and sounds muffled.
    static const double kHigh[3][2] = {{4950, 400}, {5950, 500}, {7000, 700}};
    for (int i = 0; i < 3; ++i) highFormant_[i].set(std::min(kHigh[i][0] * scale, nyq), kHigh[i][1], sr_);
    nasalPole_.set(kNasalPoleHz * scale, 100, sr_);
    nasalZero_.set(std::min<double>(p[FNZ] * scale, nyq), 100, sr_);

    const double noiseScale = std::sqrt(scale);
    for (int i = 0; i < 3; ++i) {
      double nf = std::min(p[N1F + 3 * i] * noiseScale, nyq * 0.95);
      noiseBand_[i].bandpass(nf, p[N1B + 3 * i], sr_);
      naT_[i] = p[N1A + 3 * i];
    }
    avT_ = p[AV];
    ahT_ = p[AH];
    afT_ = p[AF] * v_.consonantLevel;
    abT_ = p[AB] * v_.consonantLevel;
  }

  double sample(const Params&, double f0) {
    // Smooth amplitudes over ~2 ms.
    const double k = 1.0 - std::exp(-1.0 / (0.002 * sr_));
    av_ += (avT_ - av_) * k;
    ah_ += (ahT_ - ah_) * k;
    af_ += (afT_ - af_) * k;
    ab_ += (abT_ - ab_) * k;
    for (int i = 0; i < 3; ++i) na_[i] += (naT_[i] - na_[i]) * k;

    // Glottal flow derivative (KLGLOTT88), oversampled and box-averaged.
    const double oq = std::clamp<double>(v_.openQuotient, 0.2, 0.95);
    double src = 0;
    bool open = false;
    const double inc = f0 * periodJitter_ / (sr_ * kOversample);
    for (int s = 0; s < kOversample; ++s) {
      phase_ += inc;
      if (phase_ >= 1.0) {
        phase_ -= 1.0;
        // Tiny per-period irregularity keeps even the robot from sounding like a test tone.
        periodJitter_ = 1.0 + 0.002 * rng_.bipolar();
      }
      if (phase_ < oq) {
        double x = phase_ / oq;
        src += 2 * x - 3 * x * x;
        open = true;
      }
    }
    src /= kOversample;
    // Normalize so a lower open quotient (sharper closure) doesn't just get louder.
    src *= oq;
    tiltState_ = (1 - tiltCoef_) * src + tiltCoef_ * tiltState_;
    double voiced = tiltState_ * av_ * kVoiceGain;

    double noise = rng_.bipolar();
    double breath = v_.breathiness * av_ * (open ? 1.0 : 0.4);
    double asp = noise * (ah_ + breath) * kAspGain;

    double x = nasalZero_.tick(voiced + asp);
    x = nasalPole_.tick(x);
    for (auto& r : formant_) x = r.tick(x);
    if (useBandLimit_)
      for (auto& b : bandLimit_) x = b.tick(x);
    for (auto& r : highFormant_) x = r.tick(x);

    double n2 = rng_.bipolar();
    double fric = 0;
    for (int i = 0; i < 3; ++i) fric += na_[i] * noiseBand_[i].tick(n2);
    // Bypass noise gets a gentle high-pass so F/TH aren't rumbly.
    noiseLp_ += (n2 - noiseLp_) * 0.2;
    double bypass = (n2 - noiseLp_) * ab_ * kBypassGain;
    double y = x + fricLp_.tick(fric * af_ * kFricGain + bypass);

    y = singer_.tick(y);
    for (int i = 0; i < v_.eqBands; ++i) y = eq_[i].tick(y);
    if (v_.grit > 0) {
      double drive = 1.0 + v_.grit * 12.0;
      y = std::tanh(y * drive * 4.0) / (4.0 * std::sqrt(drive));
    }
    // DC blocker.
    double out = y - dcX_ + 0.995 * dcY_;
    dcX_ = y;
    dcY_ = out;
    return out * v_.gain;
  }
};

}  // namespace

std::vector<float> renderPlan(const Plan& plan, const Voice& voice, const RenderOptions& opts) {
  const int members = std::max(1, voice.choirSize);
  double extra = members > 1 ? voice.choirSpread : 0.0;
  size_t total = static_cast<size_t>((plan.length + extra) * opts.sampleRate);
  std::vector<float> out(total, 0.0f);

  Rng memberRng(opts.seed * 7919u + 17u);
  for (int m = 0; m < members; ++m) {
    float detune = m == 0 ? 0.0f : voice.choirDetune * memberRng.bipolar();
    double delay = m == 0 ? 0.0 : voice.choirSpread * memberRng.uniform();
    VoiceRenderer r(plan, voice, opts.sampleRate, opts.seed + static_cast<uint32_t>(m) * 101u, detune, delay);
    r.render(out);
  }
  float norm = 1.0f / std::sqrt(static_cast<float>(members));
  for (auto& s : out) s *= norm;

  if (opts.normalize) {
    float peak = 0;
    for (float s : out) peak = std::max(peak, std::fabs(s));
    if (peak > 1e-9f) {
      float g = 0.891f / peak;  // -1 dBFS
      for (auto& s : out) s *= g;
    }
  }
  return out;
}

}  // namespace nvs
