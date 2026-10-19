#include "novox.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "import.hpp"
#include "json.hpp"
#include "lexicon.hpp"
#include "planner.hpp"
#include "score.hpp"
#include "singing_pitch.hpp"
#include "singing_segment.hpp"
#include "spectrum.hpp"
#include "synth.hpp"
#include "transcribe.hpp"

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
      opts.beats = o.str("units", "seconds") == "beats";
      voice = o.str("voice", "");
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<std::string> warnings;
    novox::Score score = novox::importAny(std::vector<uint8_t>(bytes, bytes + size), opts, warnings);
    if (!voice.empty()) score.voice.name = voice;
    std::string out = novox::scoreToJson(score, opts.beats);
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

char* novox_transcribe(novox_engine* e, const float* samples, size_t count, double sample_rate,
                       const char* words_json, const char* options_json) {
  if (!samples || count == 0 || !std::isfinite(sample_rate) || sample_rate < 8000 || sample_rate > 384000) {
    e->error = "no audio to transcribe";
    return nullptr;
  }
  try {
    novox::MelodyOptions opts;
    bool debugFrames = false;  // development: include the analysis frames in the result
    if (options_json && *options_json) {
      novox::json::Value o = novox::json::parse(options_json);
      if (o.type != novox::json::Value::Type::Object) throw std::runtime_error("transcription options must be an object");
      if (o.has("algorithm") && o["algorithm"].type != novox::json::Value::Type::String) throw std::runtime_error("algorithm must be a string");
      if (o.has("detail") && o["detail"].type != novox::json::Value::Type::String) throw std::runtime_error("detail must be a string");
      for (const char* key : {"minNoteSeconds", "splitSemitones", "onsetDb", "silenceDb"})
        if (o.has(key) && !o[key].isNumber()) throw std::runtime_error("numeric transcription option required");
      for (const char* key : {"roundPitches", "stabilizePitch", "debugFrames"})
        if (o.has(key) && o[key].type != novox::json::Value::Type::Bool) throw std::runtime_error("boolean transcription option required");
      opts.algorithm = o.str("algorithm", "legacy");
      if (opts.algorithm != "legacy" && opts.algorithm != "singing-v1" && opts.algorithm != "singing-v2")
        throw std::runtime_error("unknown transcription algorithm");
      opts.detail = o.str("detail", "natural");
      if (opts.detail != "natural" && opts.detail != "detailed") throw std::runtime_error("unknown transcription detail");
      opts.minNoteSeconds = static_cast<float>(o.num("minNoteSeconds", opts.minNoteSeconds));
      opts.splitSemitones = static_cast<float>(o.num("splitSemitones", opts.splitSemitones));
      opts.onsetDb = static_cast<float>(o.num("onsetDb", opts.onsetDb));
      opts.silenceDb = static_cast<float>(o.num("silenceDb", opts.silenceDb));
      const novox::json::Value& r = o["roundPitches"];
      if (r.type == novox::json::Value::Type::Bool) opts.roundPitches = r.boolean;
      const auto& stable = o["stabilizePitch"];
      if (stable.type == novox::json::Value::Type::Bool) opts.stabilizePitch = stable.boolean;
      debugFrames = o.has("debugFrames") && o["debugFrames"].boolean;
    }
    if (!std::isfinite(opts.minNoteSeconds) || opts.minNoteSeconds < 0.016f || opts.minNoteSeconds > 2 ||
        !std::isfinite(opts.splitSemitones) || opts.splitSemitones <= 0 || opts.splitSemitones > 12 ||
        !std::isfinite(opts.onsetDb) || opts.onsetDb < 0 || opts.onsetDb > 60 ||
        !std::isfinite(opts.silenceDb) || opts.silenceDb >= 0 || opts.silenceDb < -120)
      throw std::runtime_error("invalid transcription options");
    for (size_t i = 0; i < count; ++i) if (!std::isfinite(samples[i])) throw std::runtime_error("non-finite audio sample");
    const bool v2 = opts.algorithm == "singing-v2";
    std::vector<novox::PitchFrame> frames;
    std::vector<novox::TranscribedNote> notes;
    if (!v2) {
      frames = opts.algorithm == "singing-v1" ? novox::trackSingingPitch(samples, count, sample_rate)
                                              : novox::trackPitch(samples, count, sample_rate);
      notes = novox::segmentNotes(frames, opts);
    }
    std::vector<std::string> warnings;
    std::vector<novox::Word> words;
    std::vector<size_t> unassigned;
    float tuning = 0;
    if (words_json && *words_json) {
      novox::json::Value arr = novox::json::parse(words_json);
      if (arr.type != novox::json::Value::Type::Array) throw std::runtime_error("words must be an array");
      for (const auto& w : arr.array) {
        novox::Word word;
        word.text = w.str("word", w.str("text", ""));
        word.start = w.num("start", 0);
        word.end = w.num("end", word.start);
        word.probability = static_cast<float>(w.num("probability", 1));
        if (w.type != novox::json::Value::Type::Object || word.text.empty() || !w["start"].isNumber() || !w["end"].isNumber() ||
            !std::isfinite(word.start) || !std::isfinite(word.end) || word.start < 0 || word.end < word.start ||
            !std::isfinite(word.probability) || word.probability < 0 || word.probability > 1)
          throw std::runtime_error("invalid timestamped word");
        // Optional ASR sub-word pieces: [{"text": " re", "time": 1.23, "probability": 0.9}, ...]
        if (w.has("tokens")) {
          const auto& tokens = w["tokens"];
          if (tokens.type != novox::json::Value::Type::Array) throw std::runtime_error("word tokens must be an array");
          for (const auto& t : tokens.array) {
            novox::WordToken token;
            token.text = t.str("text", "");
            token.time = t.num("time", -1);
            token.probability = static_cast<float>(t.num("probability", 1));
            if (t.type != novox::json::Value::Type::Object || !t["time"].isNumber() || !std::isfinite(token.time) ||
                token.time < 0 || !std::isfinite(token.probability) || token.probability < 0 || token.probability > 1)
              throw std::runtime_error("invalid word token");
            word.tokens.push_back(token);
          }
        }
        words.push_back(word);
      }
      if (!v2)
        warnings = novox::alignLyrics(notes, words, e->lexicon, opts.algorithm == "singing-v1", &unassigned,
                                      opts.algorithm == "singing-v1" ? &frames : nullptr);
    }
    if (v2) {
      auto x16k = novox::resampleTo16k(samples, count, sample_rate);
      novox::removeDc(x16k);
      const auto pitch = novox::analyzeSingingPitch(x16k);
      auto segmented = novox::segmentSinging(x16k, pitch, words, e->lexicon, opts);
      notes = std::move(segmented.notes);
      warnings = std::move(segmented.warnings);
      unassigned = std::move(segmented.unassignedWords);
      tuning = segmented.tuning;
      if (debugFrames) frames = pitch.frames;  // reported only; the notes are already final
    }
    return dupString(novox::transcriptionToJson(notes, warnings, opts.algorithm, &words, &unassigned,
                                                debugFrames ? &frames : nullptr, v2 ? &tuning : nullptr,
                                                v2 ? &opts.detail : nullptr));
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

char* novox_lyric_tokens(novox_engine* e, const char* text, const char* dictionary_json) {
  if (!e) return nullptr;
  try {
    novox::Lexicon local;
    const novox::Lexicon* lex = &e->lexicon;
    if (dictionary_json && *dictionary_json) {
      const auto dict = novox::json::parse(dictionary_json);
      if (!dict.isObject()) throw std::runtime_error("dictionary must be a JSON object");
      if (!dict.object.empty()) { local = e->lexicon; lex = &local; }
      for (const auto& [word, pron] : dict.object) {
        if (!pron.isString() || !local.addWord(word, pron.string))
          throw std::runtime_error("bad dictionary entry for '" + word + "'");
      }
    }
    const auto tokens = novox::lyricFlowTokens(text ? text : "", *lex);
    std::string result = "{\"tokens\":[";
    const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (i) result += ',';
      result += '\"';
      for (unsigned char c : tokens[i]) {
        if (c == '\"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
      }
      result += '\"';
    }
    result += "]}";
    e->error.clear();
    return dupString(result);
  } catch (const std::exception& ex) {
    e->error = ex.what();
    return nullptr;
  }
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
