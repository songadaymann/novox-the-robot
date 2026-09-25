// Klatt-style formant synthesizer driven by a Plan.
//
// Signal flow per voice:
//   glottal pulse (KLGLOTT88 flow derivative, 4x oversampled) + breath noise
//     -> nasal zero/pole -> F1..F5 cascade resonators          (vowels, sonorants)
//   white noise -> three band-pass peaks + flat bypass         (fricatives, bursts)
//   sum -> singer's-formant peak -> tilt -> grit -> DC block
#pragma once

#include <cstdint>
#include <vector>

#include "planner.hpp"
#include "voice.hpp"

namespace nvs {

struct RenderOptions {
  double sampleRate = 48000;
  bool normalize = true;      // peak-normalize the result to -1 dBFS
  uint32_t seed = 1;
};

// Renders the whole plan (including choir members) to mono float samples.
std::vector<float> renderPlan(const Plan& plan, const Voice& voice, const RenderOptions& opts);

}  // namespace nvs
