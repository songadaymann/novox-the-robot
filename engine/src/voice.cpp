#include "voice.hpp"

namespace novox {

namespace {

struct Preset {
  const char* name;
  void (*apply)(Voice&);
};

const Preset kPresets[] = {
    {"std", [](Voice&) {}},

    // The classic: no humanizing at all, pressed buzzy source, no vibrato.
    {"robot", [](Voice& v) {
       v.openQuotient = 0.4f; v.breathiness = 0.0f; v.brightness = 0.35f;
       v.scoop = 0.0f; v.portamento = 0.02f;
       v.vibratoDepth = 0.0f; v.jitterPitch = 0.0f; v.jitterFormant = 0.0f;
       v.singerFormantDb = 0.0f;
     }},

    {"male", [](Voice& v) { v.formantScale = 1.0f; }},

    {"female", [](Voice& v) {
       // Per-formant scaling and open ratio as in Harmony's Female preset.
       v.formantScale = 1.0f; v.openQuotient = 0.6f; v.vibratoRate = 5.8f;
       const float c[5] = {1.04f, 1.07f, 1.09f, 1.11f, 1.16f};
       for (int i = 0; i < 5; ++i) v.formantCoeff[i] = c[i];
     }},

    {"kid", [](Voice& v) {
       // Per-formant scaling and a pressed source (open ratio 40), as in Harmony's Kid.
       v.formantScale = 1.0f; v.openQuotient = 0.4f;
       const float c[5] = {1.25f, 1.23f, 1.19f, 1.15f, 1.18f};
       for (int i = 0; i < 5; ++i) v.formantCoeff[i] = c[i];
       v.vibratoDepth = 0.12f; v.scoop = -0.2f; v.jitterPitch = 10.0f;
     }},

    {"soprano", [](Voice& v) {
       v.formantScale = 1.2f; v.openQuotient = 0.62f; v.breathiness = 0.03f;
       v.vibratoDepth = 0.45f; v.vibratoRate = 5.9f; v.vibratoDelay = 0.25f;
       v.singerFormantDb = 3.0f; v.singerFormantHz = 3200.0f;
     }},

    {"alto", [](Voice& v) {
       v.formantScale = 1.12f; v.openQuotient = 0.62f; v.vibratoDepth = 0.35f;
       v.singerFormantDb = 1.0f;
     }},

    {"tenor", [](Voice& v) {
       v.formantScale = 1.04f; v.openQuotient = 0.55f; v.vibratoDepth = 0.33f;
       v.singerFormantDb = 2.0f; v.singerFormantHz = 2850.0f;
     }},

    {"bass", [](Voice& v) {
       v.formantScale = 0.9f; v.openQuotient = 0.55f; v.vibratoDepth = 0.25f;
       v.vibratoRate = 5.0f; v.brightness = -0.3f; v.singerFormantHz = 2500.0f;
     }},

    {"gregorian", [](Voice& v) {
       v.formantScale = 0.97f; v.vibratoDepth = 0.0f; v.scoop = -0.15f;
       v.portamento = 0.12f; v.brightness = -0.2f; v.choirSize = 6;
       v.choirDetune = 8.0f;
     }},

    {"choir", [](Voice& v) {
       v.choirSize = 8; v.choirDetune = 14.0f; v.choirSpread = 0.03f;
       v.vibratoDepth = 0.25f;
     }},

    {"female-choir", [](Voice& v) {
       v.formantScale = 1.17f; v.openQuotient = 0.68f; v.breathiness = 0.07f;
       v.choirSize = 8; v.choirDetune = 14.0f; v.choirSpread = 0.03f;
     }},

    {"karaoke", [](Voice& v) {
       v.vibratoDepth = 0.18f; v.scoop = -0.6f; v.scoopTime = 0.14f;
       v.brightness = 0.2f; v.jitterPitch = 9.0f;
     }},

    {"quick-vibrato", [](Voice& v) {
       v.vibratoRate = 7.5f; v.vibratoDepth = 0.4f; v.vibratoDelay = 0.12f;
       v.vibratoRamp = 0.2f;
     }},

    // Full-bandwidth, brighter modern take on the same engine.
    {"hifi", [](Voice& v) {
       v.bandLimitHz = 0.0f; v.brightness = 0.0f; v.bandwidthScale = 1.0f; v.eqBands = 0;
       v.singerFormantDb = 3.0f; v.singerFormantHz = 2900.0f; v.breathiness = 0.05f;
     }},

    {"crooner", [](Voice& v) {
       v.formantScale = 0.95f; v.openQuotient = 0.72f; v.breathiness = 0.12f;
       v.brightness = -0.35f; v.scoop = -0.8f; v.scoopTime = 0.16f;
       v.portamento = 0.12f; v.vibratoDepth = 0.35f; v.vibratoRate = 5.2f;
       v.vibratoDelay = 0.45f;
     }},
};

}  // namespace

std::vector<std::string> voicePresetNames() {
  std::vector<std::string> names;
  for (const auto& p : kPresets) names.emplace_back(p.name);
  return names;
}

bool voicePreset(const std::string& name, Voice& out) {
  for (const auto& p : kPresets) {
    if (name == p.name) {
      out = Voice{};
      out.name = name;
      p.apply(out);
      return true;
    }
  }
  return false;
}

void applyVoiceJson(const json::Value& o, Voice& v) {
  if (!o.isObject()) return;
  if (o.has("preset")) voicePreset(o.str("preset", "std"), v);
  v.name = o.str("name", v.name);
  v.octave = static_cast<int>(o.num("octave", v.octave));
  auto f = [&](const char* key, float& field) {
    field = static_cast<float>(o.num(key, field));
  };
  f("transpose", v.transpose);
  f("formantScale", v.formantScale);
  f("bandwidthScale", v.bandwidthScale);
  f("openQuotient", v.openQuotient);
  f("breathiness", v.breathiness);
  f("brightness", v.brightness);
  f("grit", v.grit);
  f("bandLimitHz", v.bandLimitHz);
  f("scoop", v.scoop);
  f("scoopTime", v.scoopTime);
  f("portamento", v.portamento);
  f("vibratoRate", v.vibratoRate);
  f("vibratoDepth", v.vibratoDepth);
  f("vibratoDelay", v.vibratoDelay);
  f("vibratoRamp", v.vibratoRamp);
  f("jitterPitch", v.jitterPitch);
  f("jitterFormant", v.jitterFormant);
  f("jitterSpeed", v.jitterSpeed);
  f("consonantLevel", v.consonantLevel);
  f("consonantSpeed", v.consonantSpeed);
  f("singerFormantDb", v.singerFormantDb);
  f("singerFormantHz", v.singerFormantHz);
  f("choirDetune", v.choirDetune);
  f("choirSpread", v.choirSpread);
  f("gain", v.gain);
  v.choirSize = static_cast<int>(o.num("choirSize", v.choirSize));
  const json::Value& eq = o["eq"];  // [[hz, db], [hz, db, q], ...]
  if (eq.isArray()) {
    v.eqBands = 0;
    for (const auto& band : eq.array) {
      if (v.eqBands >= 4 || !band.isArray() || band.array.size() < 2) continue;
      v.eq[v.eqBands][0] = static_cast<float>(band.array[0].number);
      v.eq[v.eqBands][1] = static_cast<float>(band.array[1].number);
      v.eq[v.eqBands][2] = band.array.size() > 2 ? static_cast<float>(band.array[2].number) : 1.2f;
      ++v.eqBands;
    }
  }
  const json::Value& coeffs = o["formantCoeff"];
  if (coeffs.isArray()) {
    for (size_t i = 0; i < coeffs.array.size() && i < 5; ++i) {
      if (coeffs.array[i].isNumber()) v.formantCoeff[i] = static_cast<float>(coeffs.array[i].number);
    }
  }
}

}  // namespace novox
