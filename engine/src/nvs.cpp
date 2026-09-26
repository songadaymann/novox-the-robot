#include "nvs.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "import.hpp"
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

// Parses a score. Returns the lexicon to use: the engine's own, or (only when
// the score adds pronunciations) a per-song copy held in `local`.
const nvs::Lexicon* prepare(nvs_engine* e, const char* json, nvs::Score& score, nvs::Lexicon& local) {
  try {
    score = nvs::parseScore(nvs::json::parse(json ? json : ""));
  } catch (const std::exception& ex) {
    e->error = ex.what();
    return nullptr;
  }
  if (score.dictionary.empty()) return &e->lexicon;
  local = e->lexicon;
  for (const auto& [word, pron] : score.dictionary) {
    if (!local.addWord(word, pron)) {
      e->error = "bad dictionary entry for '" + word + "': " + pron;
      return nullptr;
    }
  }
  return &local;
}

}  // namespace

extern "C" {

nvs_engine* nvs_create(double sample_rate) {
  auto* e = new nvs_engine;
  if (sample_rate > 0) e->sampleRate = sample_rate;
  e->lexicon.loadEmbedded();  // no-op in builds without an embedded dictionary
  return e;
}

void nvs_destroy(nvs_engine* e) { delete e; }

void nvs_set_sample_rate(nvs_engine* e, double sample_rate) {
  if (sample_rate > 0) e->sampleRate = sample_rate;
}

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
  nvs::Lexicon local;
  const nvs::Lexicon* lex = prepare(e, score_json, score, local);
  if (!lex) return -1;
  nvs::Plan plan = nvs::planScore(score, *lex);
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
  nvs::Lexicon local;
  const nvs::Lexicon* lex = prepare(e, score_json, score, local);
  if (!lex) return nullptr;
  return dupString(nvs::planToJson(nvs::planScore(score, *lex)));
}

char* nvs_import(nvs_engine* e, const void* data, size_t size, const char* options_json) {
  if (!data || size == 0) {
    e->error = "no data to import";
    return nullptr;
  }
  try {
    nvs::ImportOptions opts;
    std::string voice;
    if (options_json && *options_json) {
      nvs::json::Value o = nvs::json::parse(options_json);
      opts.track = static_cast<int>(o.num("track", -1));
      opts.lyrics = o.str("lyrics", "");
      voice = o.str("voice", "");
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<std::string> warnings;
    nvs::Score score = nvs::importAny(std::vector<uint8_t>(bytes, bytes + size), opts, warnings);
    if (!voice.empty()) score.voice.name = voice;
    std::string out = nvs::scoreToJson(score);
    if (!warnings.empty()) {
      std::string w = "  \"importWarnings\": [";
      for (size_t i = 0; i < warnings.size(); ++i) {
        std::string esc;
        for (char c : warnings[i]) {
          if (c == '"' || c == '\\') esc += '\\';
          esc += c;
        }
        w += (i ? ", \"" : "\"") + esc + "\"";
      }
      w += "],\n";
      out.insert(out.find('\n') + 1, w);
    }
    return dupString(out);
  } catch (const std::exception& ex) {
    e->error = ex.what();
    return nullptr;
  }
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

struct nvs_stream {
  nvs::Stream stream;
};

nvs_stream* nvs_stream_create(nvs_engine* e, const char* score_json) {
  nvs::Score score;
  nvs::Lexicon local;
  const nvs::Lexicon* lex = prepare(e, score_json, score, local);
  if (!lex) return nullptr;
  nvs::Plan plan = nvs::planScore(score, *lex);
  return new nvs_stream{nvs::Stream(std::move(plan), score.voice, e->sampleRate)};
}

void nvs_stream_destroy(nvs_stream* s) { delete s; }
size_t nvs_stream_length(const nvs_stream* s) { return s->stream.length(); }
size_t nvs_stream_position(const nvs_stream* s) { return s->stream.position(); }
double nvs_stream_lead(const nvs_stream* s) { return s->stream.plan().lead; }
void nvs_stream_seek(nvs_stream* s, size_t sample) { s->stream.seek(sample); }
size_t nvs_stream_process(nvs_stream* s, float* out, size_t frames) { return s->stream.process(out, frames); }

const char* nvs_last_error(nvs_engine* e) { return e->error.c_str(); }

void nvs_free(void* ptr) { std::free(ptr); }

}  // extern "C"
