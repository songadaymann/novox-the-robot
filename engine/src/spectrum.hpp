// Shared spectral front end for singing transcription: the 16 kHz analysis
// signal, a real FFT, and short-time spectra on the pitch tracker's frame
// timeline (frame k is centred on sample k * kAnalysisHop).
#pragma once

#include <cstddef>
#include <vector>

namespace novox {

constexpr double kAnalysisRate = 16000;  // Hz
constexpr int kAnalysisHop = 128;        // samples, 8 ms
constexpr double kAnalysisHopSeconds = kAnalysisHop / kAnalysisRate;

// Band-limited (windowed-sinc) resampling of any input rate to 16 kHz.
std::vector<float> resampleTo16k(const float* x, size_t n, double sampleRate);

// Removes DC and subsonic rumble in place (one-pole high-pass, -3 dB at 20 Hz;
// sung pitch starts at 60 Hz). An offset would otherwise raise the level of
// every frame, including the silences the level gates are measured from.
void removeDc(std::vector<float>& x);

// Number of analysis frames for a 16 kHz signal of `samples` samples.
inline size_t analysisFrameCount(size_t samples) { return (samples + kAnalysisHop - 1) / kAnalysisHop; }
inline double analysisFrameTime(size_t frame) { return static_cast<double>(frame * kAnalysisHop) / kAnalysisRate; }

// Radix-2 FFT magnitudes of a power-of-two length real signal.
class RealFFT {
 public:
  explicit RealFFT(int size);
  int size() const { return n_; }
  // `in` holds size() samples; `magnitude` receives size()/2 + 1 linear magnitudes.
  void magnitude(const float* in, float* magnitude);

 private:
  int n_;
  std::vector<double> re_, im_, cos_, sin_;
  std::vector<int> bitrev_;
};

// Hann-windowed magnitude spectrum of the `size` samples centred on any sample
// of a signal (zero-padded at the recording edges), for analyses that go frame
// by frame instead of holding a whole take's spectra.
class WindowedSpectrum {
 public:
  explicit WindowedSpectrum(int size);
  int size() const { return fft_.size(); }
  // Writes size()/2 + 1 linear magnitudes for the window centred on x[center].
  void analyze(const std::vector<float>& x, long long center, float* magnitude);

 private:
  RealFFT fft_;
  std::vector<float> window_, buffer_;
};

// Hann-windowed magnitude spectra, one per analysis frame, centred on the frame
// time (zero-padded at the recording edges).
struct Spectra {
  int fftSize = 0;
  int bins = 0;           // fftSize / 2 + 1
  double binHz = 0;       // kAnalysisRate / fftSize
  size_t frames = 0;
  std::vector<float> mag; // frames * bins, linear magnitude

  const float* frame(size_t i) const { return mag.data() + i * static_cast<size_t>(bins); }
  // Mean power over [loHz, hiHz) in dB (10*log10 of mean squared magnitude), down to
  // -200 dB for digital silence: callers floor it relative to their signal's level.
  float bandDb(size_t frame, double loHz, double hiHz) const;
};

Spectra computeSpectra(const std::vector<float>& x16k, int fftSize);

}  // namespace novox
