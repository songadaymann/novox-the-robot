#include "novox.h"

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

struct novox_engine {
  double sampleRate = 48000;
  novox::Lexicon lexicon;
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
const novox::Lexicon* prepare(novox_engine* e, const char* json, novox::Score& score, novox::Lexicon& local) {
  try {
    score = novox::parseScore(novox::json::parse(json ? json : ""));
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

novox_engine* novox_create(double sample_rate) {
  auto* e = new novox_engine;
  if (sample_rate > 0) e->sampleRate = sample_rate;
  e->lexicon.loadEmbedded();  // no-op in builds without an embedded dictionary
  return e;
}

void novox_destroy(novox_engine* e) { delete e; }

void novox_set_sample_rate(novox_engine* e, double sample_rate) {
  if (sample_rate > 0) e->sampleRate = sample_rate;
}

int novox_load_dictionary(novox_engine* e, const char* path) {
  int n = e->lexicon.loadCmudict(path ? path : "");
  if (n < 0) e->error = std::string("could not read dictionary: ") + (path ? path : "(null)");
  return n;
}

int novox_add_word(novox_engine* e, const char* word, const char* arpabet) {
  if (!word || !arpabet || !e->lexicon.addWord(word, arpabet)) {
    e->error = "bad pronunciation";
    return -1;
  }
  return 0;
}

int novox_render(novox_engine* e, const char* score_json, float** out_samples, size_t* out_count) {
  if (!out_samples || !out_count) return -1;
  *out_samples = nullptr;
  *out_count = 0;
  novox::Score score;
  novox::Lexicon local;
  const novox::Lexicon* lex = prepare(e, score_json, score, local);
  if (!lex) return -1;
  novox::Plan plan = novox::planScore(score, *lex);
  novox::RenderOptions opts;
  opts.sampleRate = e->sampleRate;
  std::vector<float> audio = novox::renderPlan(plan, score.voice, opts);
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

char* novox_plan(novox_engine* e, const char* score_json) {
  novox::Score score;
  novox::Lexicon local;
  const novox::Lexicon* lex = prepare(e, score_json, score, local);
  if (!lex) return nullptr;
  return dupString(novox::planToJson(novox::planScore(score, *lex)));
}

char* novox_import(novox_engine* e, const void* data, size_t size, const char* options_json) {
  if (!data || size == 0) {
    e->error = "no data to import";
    return nullptr;
  }
  try {
    novox::ImportOptions opts;
    std::string voice;
    if (options_json && *options_json) {
      novox::json::Value o = novox::json::parse(options_json);
      opts.track = static_cast<int>(o.num("track", -1));
      opts.lyrics = o.str("lyrics", "");
      voice = o.str("voice", "");
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<std::string> warnings;
    novox::Score score = novox::importAny(std::vector<uint8_t>(bytes, bytes + size), opts, warnings);
    if (!voice.empty()) score.voice.name = voice;
    std::string out = novox::scoreToJson(score);
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

char* novox_phonemize(novox_engine* e, const char* text) {
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

char* novox_voice_names(void) {
  std::string s;
  for (const auto& n : novox::voicePresetNames()) s += (s.empty() ? "" : ",") + n;
  return dupString(s);
}

struct novox_stream {
  novox::Stream stream;
};

novox_stream* novox_stream_create(novox_engine* e, const char* score_json) {
  novox::Score score;
  novox::Lexicon local;
  const novox::Lexicon* lex = prepare(e, score_json, score, local);
  if (!lex) return nullptr;
  novox::Plan plan = novox::planScore(score, *lex);
  return new novox_stream{novox::Stream(std::move(plan), score.voice, e->sampleRate)};
}

void novox_stream_destroy(novox_stream* s) { delete s; }
size_t novox_stream_length(const novox_stream* s) { return s->stream.length(); }
size_t novox_stream_position(const novox_stream* s) { return s->stream.position(); }
double novox_stream_lead(const novox_stream* s) { return s->stream.plan().lead; }
void novox_stream_seek(novox_stream* s, size_t sample) { s->stream.seek(sample); }
size_t novox_stream_process(novox_stream* s, float* out, size_t frames) { return s->stream.process(out, frames); }

const char* novox_last_error(novox_engine* e) { return e->error.c_str(); }

void novox_free(void* ptr) { std::free(ptr); }

}  // extern "C"
