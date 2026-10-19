#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace novox {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

std::vector<float> resampleTo16k(const float* x, size_t n, double sr) {
  if (std::fabs(sr - kAnalysisRate) < 0.5) return std::vector<float>(x, x + n);
  const double ratio = kAnalysisRate / sr;
  const size_t outN = static_cast<size_t>(std::floor(static_cast<double>(n) * ratio));
  std::vector<float> y(outN);
  const double cutoff = std::min(1.0, ratio) * 0.45;  // cycles per input sample
  // Taps each side: 24 up to 48 kHz, then as many as 8 output samples span. The
  // cutoff narrows with the decimation, so a fixed width would leave little
  // stopband (at 192 kHz, 9-10 kHz folded into the vowel and frication bands at
  // only -11..-15 dB, against -47..-57 dB at 48 kHz).
  const int half = std::max(24, static_cast<int>(std::ceil(8 / ratio)));
  for (size_t i = 0; i < outN; ++i) {
    const double t = static_cast<double>(i) / ratio;
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

void removeDc(std::vector<float>& x) {
  if (x.empty()) return;
  const double pole = std::exp(-2 * kPi * 20 / kAnalysisRate);
  // Starting from the first sample, a constant offset leaves no step behind.
  double previous = x[0], y = 0;
  for (auto& v : x) {
    y = pole * y + (v - previous);
    previous = v;
    v = static_cast<float>(y);
  }
}

RealFFT::RealFFT(int size) : n_(size) {
  if (size < 2 || (size & (size - 1))) throw std::invalid_argument("FFT size must be a power of two");
  re_.resize(static_cast<size_t>(n_));
  im_.resize(static_cast<size_t>(n_));
  cos_.resize(static_cast<size_t>(n_ / 2));
  sin_.resize(static_cast<size_t>(n_ / 2));
  for (int k = 0; k < n_ / 2; ++k) {
    cos_[static_cast<size_t>(k)] = std::cos(2 * kPi * k / n_);
    sin_[static_cast<size_t>(k)] = -std::sin(2 * kPi * k / n_);
  }
  bitrev_.resize(static_cast<size_t>(n_));
  int bits = 0;
  while ((1 << bits) < n_) ++bits;
  for (int i = 0; i < n_; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
    bitrev_[static_cast<size_t>(i)] = r;
  }
}

void RealFFT::magnitude(const float* in, float* out) {
  for (int i = 0; i < n_; ++i) {
    re_[static_cast<size_t>(bitrev_[static_cast<size_t>(i)])] = in[i];
    im_[static_cast<size_t>(bitrev_[static_cast<size_t>(i)])] = 0;
  }
  for (int len = 2; len <= n_; len <<= 1) {
    const int half = len / 2, step = n_ / len;
    for (int i = 0; i < n_; i += len) {
      for (int j = 0; j < half; ++j) {
        const double wr = cos_[static_cast<size_t>(j * step)], wi = sin_[static_cast<size_t>(j * step)];
        const size_t a = static_cast<size_t>(i + j), b = static_cast<size_t>(i + j + half);
        const double tr = re_[b] * wr - im_[b] * wi, ti = re_[b] * wi + im_[b] * wr;
        re_[b] = re_[a] - tr; im_[b] = im_[a] - ti;
        re_[a] += tr; im_[a] += ti;
      }
    }
  }
  for (int k = 0; k <= n_ / 2; ++k)
    out[k] = static_cast<float>(std::hypot(re_[static_cast<size_t>(k)], im_[static_cast<size_t>(k)]));
}

float Spectra::bandDb(size_t f, double loHz, double hiHz) const {
  if (f >= frames) return -120;
  const int lo = std::max(0, static_cast<int>(std::ceil(loHz / binHz)));
  const int hi = std::min(bins, static_cast<int>(std::ceil(hiHz / binHz)));
  if (hi <= lo) return -120;
  const float* m = frame(f);
  double power = 0;
  for (int i = lo; i < hi; ++i) power += static_cast<double>(m[i]) * m[i];
  // Fixed scaling so levels are comparable between FFT sizes; only differences matter.
  const double norm = 4.0 / (static_cast<double>(fftSize) * fftSize);
  return static_cast<float>(10 * std::log10(power * norm / (hi - lo) + 1e-20));
}

WindowedSpectrum::WindowedSpectrum(int size)
    : fft_(size), window_(static_cast<size_t>(size)), buffer_(static_cast<size_t>(size)) {
  for (int i = 0; i < size; ++i)
    window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / size));
}

void WindowedSpectrum::analyze(const std::vector<float>& x, long long center, float* magnitude) {
  const int n = fft_.size();
  const long long start = center - n / 2;
  for (int i = 0; i < n; ++i) {
    const long long at = start + i;
    const float v = at >= 0 && at < static_cast<long long>(x.size()) ? x[static_cast<size_t>(at)] : 0.0f;
    buffer_[static_cast<size_t>(i)] = v * window_[static_cast<size_t>(i)];
  }
  fft_.magnitude(buffer_.data(), magnitude);
}

Spectra computeSpectra(const std::vector<float>& x, int fftSize) {
  Spectra s;
  s.fftSize = fftSize;
  s.bins = fftSize / 2 + 1;
  s.binHz = kAnalysisRate / fftSize;
  s.frames = analysisFrameCount(x.size());
  s.mag.assign(s.frames * static_cast<size_t>(s.bins), 0.0f);
  WindowedSpectrum spectrum(fftSize);
  for (size_t f = 0; f < s.frames; ++f)
    spectrum.analyze(x, static_cast<long long>(f) * kAnalysisHop, s.mag.data() + f * static_cast<size_t>(s.bins));
  return s;
}

}  // namespace novox
