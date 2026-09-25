#include "score.hpp"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace nvs {

bool parsePitch(const std::string& text, float& midi) {
  if (text.empty()) return false;
  static const int base[] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
  char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
  if (letter < 'A' || letter > 'G') {
    // Plain number?
    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && *end == '\0') {
      midi = static_cast<float>(v);
      return true;
    }
    return false;
  }
  int semis = base[letter - 'A'];
  size_t i = 1;
  while (i < text.size() && (text[i] == '#' || text[i] == 'b')) {
    semis += text[i] == '#' ? 1 : -1;
    ++i;
  }
  if (i >= text.size()) return false;
  char* end = nullptr;
  long octave = std::strtol(text.c_str() + i, &end, 10);
  if (*end != '\0') return false;
  midi = static_cast<float>((octave + 1) * 12 + semis);
  return true;
}

std::vector<std::string> tokenizeLyrics(const std::string& line) {
  std::vector<std::string> out;
  std::string word;
  std::istringstream in(line);
  while (in >> word) {
    // Keep {explicit phonemes} intact even if they contain hyphens.
    if (word.front() == '{') {
      std::string full = word;
      while (full.back() != '}' && in >> word) full += " " + word;
      out.push_back(full);
      continue;
    }
    size_t start = 0;
    while (start < word.size()) {
      size_t dash = word.find('-', start);
      if (dash == std::string::npos) {
        out.push_back(word.substr(start));
        break;
      }
      out.push_back(word.substr(start, dash - start + 1));  // keep the dash
      start = dash + 1;
    }
  }
  return out;
}

namespace {

double beatsToSeconds(double beats, double tempo) { return beats * 60.0 / tempo; }

}  // namespace

Score parseScore(const json::Value& root) {
  if (!root.isObject()) throw std::runtime_error("score must be a JSON object");
  Score score;
  score.tempo = root.num("tempo", 120.0);
  if (score.tempo <= 0) throw std::runtime_error("tempo must be positive");
  bool seconds = root.str("units", "beats") == "seconds";
  auto toSec = [&](double v) { return seconds ? v : beatsToSeconds(v, score.tempo); };

  // Voice
  const json::Value& voice = root["voice"];
  if (voice.isString()) {
    if (!voicePreset(voice.string, score.voice))
      throw std::runtime_error("unknown voice preset: " + voice.string);
  } else if (voice.isObject()) {
    applyVoiceJson(voice, score.voice);
  }

  // Dictionary overrides
  const json::Value& dict = root["dictionary"];
  if (dict.isObject()) {
    for (const auto& [word, pron] : dict.object) {
      if (pron.isString()) score.dictionary.emplace_back(word, pron.string);
    }
  }

  // Notes
  const json::Value& notes = root["notes"];
  if (!notes.isArray()) throw std::runtime_error("score needs a \"notes\" array");
  double cursor = 0.0;  // in score units
  int index = 0;
  for (const auto& n : notes.array) {
    ++index;
    std::string pitchText, lyric;
    double start = cursor, dur = 0;
    bool rest = false;
    float midi = 60;

    if (n.isString()) {
      std::istringstream in(n.string);
      std::string durText;
      in >> pitchText >> durText;
      std::getline(in, lyric);
      lyric.erase(0, lyric.find_first_not_of(' '));
      dur = std::strtod(durText.c_str(), nullptr);
    } else if (n.isObject()) {
      const json::Value& p = n["pitch"];
      if (p.isNumber()) {
        midi = static_cast<float>(p.number);
        pitchText = "#";
      } else {
        pitchText = p.isString() ? p.string : "r";
      }
      start = n.num("start", cursor);
      dur = n.num("dur", 0);
      lyric = n.str("lyric", "");
    } else {
      throw std::runtime_error("note " + std::to_string(index) + " must be a string or object");
    }

    if (pitchText == "r" || pitchText == "R" || pitchText == "rest") {
      rest = true;
    } else if (pitchText != "#" && !parsePitch(pitchText, midi)) {
      throw std::runtime_error("note " + std::to_string(index) + ": bad pitch '" + pitchText + "'");
    }
    if (dur <= 0) throw std::runtime_error("note " + std::to_string(index) + ": duration must be > 0");

    cursor = start + dur;
    if (rest) continue;
    Note note;
    note.start = toSec(start);
    note.dur = toSec(dur);
    note.pitch = midi;
    note.lyric = lyric;
    score.notes.push_back(note);
  }
  std::stable_sort(score.notes.begin(), score.notes.end(),
                   [](const Note& a, const Note& b) { return a.start < b.start; });

  score.lyrics = root.str("lyrics", "");
  return score;
}

}  // namespace nvs
