// A "voice" is a set of knobs applied on top of the phoneme targets:
// vocal-tract size, glottal source character, pitch behavior, humanization
// and ensemble. Presets are just named Voice values.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace nvs {

struct Voice {
  std::string name = "std";

  // Pitch placement
  int octave = 0;                 // shift sung pitch by octaves
  float transpose = 0.0f;         // semitones

  // Vocal tract
  float formantScale = 1.0f;      // 1 = adult male, ~1.17 adult female, ~1.3 child
  float formantCoeff[5] = {1, 1, 1, 1, 1};  // per-formant fine tuning
  float bandwidthScale = 1.5f;    // >1 = duller, breathier resonances

  // Glottal source
  float openQuotient = 0.72f;      // 0.35 = pressed/buzzy, 0.8 = soft/breathy
  float breathiness = 0.01f;      // aspiration mixed into voiced sound
  float brightness = -0.3f;        // -1 dark .. +1 bright (spectral tilt)
  float grit = 0.0f;              // 0..1 saturation / clipping
  float bandLimitHz = 4800.0f;    // steep low-pass on the voiced sound (not consonants); 0 = off

  // Pitch behavior
  float scoop = -0.35f;           // semitones the attack starts from (negative = below)
  float scoopTime = 0.20f;        // seconds to reach pitch
  float portamento = 0.20f;       // legato glide time, seconds
  float vibratoRate = 5.8f;       // Hz
  float vibratoDepth = 0.35f;     // semitones (peak)
  float vibratoDelay = 0.0f;      // seconds into a note before vibrato starts
  float vibratoRamp = 0.25f;      // seconds to reach full depth

  // Humanization
  float jitterPitch = 6.0f;       // cents of slow random pitch wander
  float jitterFormant = 0.004f;   // fractional random formant wander
  float jitterSpeed = 4.0f;       // Hz

  // Articulation
  float consonantLevel = 1.0f;    // loudness of consonant noise
  float consonantSpeed = 1.0f;    // >1 = snappier (shorter) consonants
  float singerFormantDb = 0.0f;   // peak (+) or dip (-) near 2.6-3 kHz; Harmony's std voice has a dip
  float singerFormantHz = 2600.0f;
  // Tone EQ applied to the whole voice: up to 4 peaking bands {Hz, dB, Q}.
  int eqBands = 0;
  float eq[4][3] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};

  // Ensemble
  int choirSize = 1;
  float choirDetune = 12.0f;      // cents (random per member)
  float choirSpread = 0.025f;     // seconds of timing looseness

  float gain = 1.0f;
};

// Built-in presets, e.g. "std", "robot", "female", "kid", "bass", "choir".
std::vector<std::string> voicePresetNames();
bool voicePreset(const std::string& name, Voice& out);

// Applies any fields present in `obj` over `v`. `obj` may name a base preset
// with "preset".
void applyVoiceJson(const json::Value& obj, Voice& v);

}  // namespace nvs
