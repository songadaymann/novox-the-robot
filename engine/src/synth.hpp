// Klatt-style formant synthesizer driven by a Plan.
//
// Signal flow per voice:
//   glottal pulse (KLGLOTT88 flow derivative, 4x oversampled) + breath noise
//     -> nasal zero/pole -> F1..F5 cascade resonators          (vowels, sonorants)
//   white noise -> three band-pass peaks + flat bypass         (fricatives, bursts)
//   sum -> singer's-formant peak -> tilt -> grit -> DC block
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "planner.hpp"
#include "voice.hpp"

namespace novox {

struct RenderOptions {
  double sampleRate = 48000;
  bool normalize = true;      // peak-normalize the result to -1 dBFS
  uint32_t seed = 1;
};

// Renders the whole plan (including choir members) to mono float samples.
std::vector<float> renderPlan(const Plan& plan, const Voice& voice, const RenderOptions& opts);

// Incremental renderer for real-time use (plugins): owns its plan, renders any
// chunk size from any position, never allocates in process(). Output is at a
// fixed gain (no normalization) with a soft clip above -2 dBFS. Long silent
// rests retain their duration but bypass synthesis once filter tails are
// inaudible; source phase/noise resumes deterministically before the next onset.
class Stream {
 public:
  Stream(Plan plan, Voice voice, double sampleRate, uint32_t seed = 1);
  ~Stream();
  Stream(Stream&&) noexcept;
  Stream& operator=(Stream&&) noexcept;

  size_t length() const;      // total samples including release tail
  size_t position() const;
  double sampleRate() const;
  const Plan& plan() const;

  void seek(size_t sample);   // not real-time safe (rebuilds voices)
  // Writes `frames` samples (zeros past the end). Returns how many had content.
  size_t process(float* out, size_t frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace novox
