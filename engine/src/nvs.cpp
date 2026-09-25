#include "nvs.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "json.hpp"
#include "lexicon.hpp"
#include "planner.hpp"
#include "score.hpp"
#include "synth.hpp"

struct nvs_engine {
  double sampleRate = 48000;
  nvs::Lexicon lexicon;
  std::string error;
};

namespace {

char* dupString(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

// Parses a score and applies its dictionary entries to a copy of the lexicon,
// so per-song pronunciations don't leak into the engine.
bool prepare(nvs_engine* e, const char* json, nvs::Score& score, nvs::Lexicon& lex) {
  try {
    score = nvs::parseScore(nvs::json::parse(json ? json : ""));
  } catch (const std::exception& ex) {
    e->error = ex.what();
    return false;
  }
  lex = e->lexicon;
  for (const auto& [word, pron] : score.dictionary) {
    if (!lex.addWord(word, pron)) {
      e->error = "bad dictionary entry for '" + word + "': " + pron;
      return false;
    }
  }
  return true;
}

}  // namespace

extern "C" {

nvs_engine* nvs_create(double sample_rate) {
  auto* e = new nvs_engine;
  if (sample_rate > 0) e->sampleRate = sample_rate;
  return e;
}

void nvs_destroy(nvs_engine* e) { delete e; }

int nvs_load_dictionary(nvs_engine* e, const char* path) {
  int n = e->lexicon.loadCmudict(path ? path : "");
  if (n < 0) e->error = std::string("could not read dictionary: ") + (path ? path : "(null)");
  return n;
}

int nvs_add_word(nvs_engine* e, const char* word, const char* arpabet) {
  if (!word || !arpabet || !e->lexicon.addWord(word, arpabet)) {
    e->error = "bad pronunciation";
    return -1;
  }
  return 0;
}

int nvs_render(nvs_engine* e, const char* score_json, float** out_samples, size_t* out_count) {
  if (!out_samples || !out_count) return -1;
  *out_samples = nullptr;
  *out_count = 0;
  nvs::Score score;
  nvs::Lexicon lex;
  if (!prepare(e, score_json, score, lex)) return -1;
  nvs::Plan plan = nvs::planScore(score, lex);
  nvs::RenderOptions opts;
  opts.sampleRate = e->sampleRate;
  std::vector<float> audio = nvs::renderPlan(plan, score.voice, opts);
  auto* buf = static_cast<float*>(std::malloc(audio.size() * sizeof(float)));
  if (!buf) {
    e->error = "out of memory";
    return -1;
  }
  std::memcpy(buf, audio.data(), audio.size() * sizeof(float));
  *out_samples = buf;
  *out_count = audio.size();
  return 0;
}

char* nvs_plan(nvs_engine* e, const char* score_json) {
  nvs::Score score;
  nvs::Lexicon lex;
  if (!prepare(e, score_json, score, lex)) return nullptr;
  return dupString(nvs::planToJson(nvs::planScore(score, lex)));
}

char* nvs_phonemize(nvs_engine* e, const char* text) {
  std::istringstream in(text ? text : "");
  std::string word, out;
  while (in >> word) {
    if (!out.empty()) out += " | ";
    std::string line;
    for (const auto& t : e->lexicon.lookup(word)) line += (line.empty() ? "" : " ") + t.symbol;
    out += line;
  }
  return dupString(out);
}

char* nvs_voice_names(void) {
  std::string s;
  for (const auto& n : nvs::voicePresetNames()) s += (s.empty() ? "" : ",") + n;
  return dupString(s);
}

const char* nvs_last_error(nvs_engine* e) { return e->error.c_str(); }

void nvs_free(void* ptr) { std::free(ptr); }

}  // extern "C"
