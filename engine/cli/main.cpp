// novox: command-line front end for the Novox The Robot engine.
//
//   novox render score.json -o out.wav [--voice robot] [--rate 48000]
//   novox plan score.json                 print the phoneme timeline as JSON
//   novox say "some words" -o out.wav [--pitch C4] [--voice female]
//   novox phonemes "some words"
//   novox voices

#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "novox.h"

namespace {

bool writeWav(const std::string& path, const float* data, size_t count, int rate) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  uint32_t bytes = static_cast<uint32_t>(count * 2);
  f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
  f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(static_cast<uint32_t>(rate));
  u32(static_cast<uint32_t>(rate * 2)); u16(2); u16(16);
  f.write("data", 4); u32(bytes);
  for (size_t i = 0; i < count; ++i) {
    float s = data[i] < -1 ? -1 : data[i] > 1 ? 1 : data[i];
    auto v = static_cast<int16_t>(s * 32767.0f);
    f.write(reinterpret_cast<const char*>(&v), 2);
  }
  return static_cast<bool>(f);
}

// Minimal WAV reader (PCM 16/24/32-bit or float 32), mixed down to mono.
bool readWav(const std::string& path, std::vector<float>& out, double& rate, std::string& error) {
  std::string d;
  {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "could not read " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    d = ss.str();
  }
  auto u16 = [&](size_t p) { return static_cast<uint16_t>(static_cast<uint8_t>(d[p]) | static_cast<uint8_t>(d[p + 1]) << 8); };
  auto u32 = [&](size_t p) { return static_cast<uint32_t>(u16(p)) | static_cast<uint32_t>(u16(p + 2)) << 16; };
  if (d.size() < 12 || d.compare(0, 4, "RIFF") || d.compare(8, 4, "WAVE")) { error = "not a WAV file"; return false; }
  int format = 0, channels = 0, bits = 0;
  size_t p = 12, dataPos = 0, dataLen = 0;
  while (p + 8 <= d.size()) {
    const std::string id = d.substr(p, 4);
    const size_t len = u32(p + 4);
    if (id == "fmt ") {
      format = u16(p + 8); channels = u16(p + 10); rate = u32(p + 12); bits = u16(p + 22);
      if (format == 0xFFFE && len >= 26) format = u16(p + 32);  // WAVE_FORMAT_EXTENSIBLE
    } else if (id == "data") {
      dataPos = p + 8; dataLen = std::min(len, d.size() - dataPos);
    }
    p += 8 + len + (len & 1);
  }
  if (!dataPos || channels < 1) { error = "WAV has no audio data"; return false; }
  const int bytes = bits / 8;
  const size_t frames = dataLen / static_cast<size_t>(bytes * channels);
  out.assign(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (int c = 0; c < channels; ++c) {
      const size_t q = dataPos + (i * channels + c) * bytes;
      double v = 0;
      if (format == 3 && bits == 32) { float fv; std::memcpy(&fv, d.data() + q, 4); v = fv; }
      else if (bits == 16) v = static_cast<int16_t>(u16(q)) / 32768.0;
      else if (bits == 24) v = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint8_t>(d[q])) << 8 |
                                                     static_cast<uint32_t>(static_cast<uint8_t>(d[q + 1])) << 16 |
                                                     static_cast<uint32_t>(static_cast<uint8_t>(d[q + 2])) << 24)) / 2147483648.0;
      else if (bits == 32) v = static_cast<int32_t>(u32(q)) / 2147483648.0;
      else { error = "unsupported WAV sample format"; return false; }
      sum += v;
    }
    out[i] = static_cast<float>(sum / channels);
  }
  return true;
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string jsonEscape(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (c == '"' || c == '\\') r += '\\';
    r += c;
  }
  return r;
}

// Finds data/cmudict.dict next to the executable's project, or via NOVOX_DICT.
std::string findDictionary(const char* argv0, const std::string& flag) {
  if (!flag.empty()) return flag;
  if (const char* env = std::getenv("NOVOX_DICT")) return env;
  std::string exe = argv0;
  size_t slash = exe.find_last_of('/');
  std::string dir = slash == std::string::npos ? "." : exe.substr(0, slash);
  for (const char* rel : {"/../data/cmudict.dict", "/../../data/cmudict.dict", "/data/cmudict.dict",
                          "/../../../data/cmudict.dict"}) {
    std::string p = dir + rel;
    if (std::ifstream(p)) return p;
  }
  return "data/cmudict.dict";
}

void usage() {
  std::cerr << "usage:\n"
               "  novox render <score.json|song.mid|song.musicxml> -o <out.wav> [--voice NAME] [--rate HZ]\n"
               "             [--lyrics \"hel-lo world\"] [--track N]\n"
               "  novox import <song.mid|song.musicxml> [-o score.json] [--lyrics ...] [--track N]\n"
               "  novox plan <score.json|song.mid|song.musicxml>\n"
               "  novox transcribe <take.wav> [-o score.json] [--words words.json] [--algorithm legacy|singing-v1|singing-v2]\n"
               "             [--detail natural|detailed]  (singing-v2: also split held approaches, grace notes, droops)\n"
               "  novox say \"words to sing\" -o <out.wav> [--pitch C4] [--beats 1] [--tempo 100] [--voice NAME]\n"
               "  novox phonemes \"words\"\n"
               "  novox voices\n"
               "options: --dict PATH  extra cmudict-format dictionary (also $NOVOX_DICT)\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  std::string cmd = argv[1];
  std::string input, out = "out.wav", voice, dict, pitch = "C4", lyrics, wordsPath, algorithm = "legacy", detail = "natural";
  double rate = 48000, beats = 1, tempo = 100;
  int track = -1;
  bool outGiven = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "-o" || a == "--out") {
      out = next();
      outGiven = true;
    } else if (a == "--lyrics") lyrics = next();
    else if (a == "--track") track = std::atoi(next().c_str());
    else if (a == "--algorithm") algorithm = next();
    else if (a == "--detail") detail = next();
    else if (a == "--words") wordsPath = next();
    else if (a == "--voice") voice = next();
    else if (a == "--dict") dict = next();
    else if (a == "--rate") rate = std::atof(next().c_str());
    else if (a == "--pitch") pitch = next();
    else if (a == "--beats") beats = std::atof(next().c_str());
    else if (a == "--tempo") tempo = std::atof(next().c_str());
    else input = a;
  }

  if (cmd == "voices") {
    char* names = novox_voice_names();
    std::cout << names << "\n";
    novox_free(names);
    return 0;
  }

  novox_engine* e = novox_create(rate);
  // The dictionary is built in; --dict / $NOVOX_DICT add or override entries.
  if (!dict.empty() || std::getenv("NOVOX_DICT")) {
    if (novox_load_dictionary(e, findDictionary(argv[0], dict).c_str()) < 0) {
      std::cerr << "warning: " << novox_last_error(e) << "\n";
    }
  }

  int status = 0;
  if (cmd == "transcribe") {
    // Sung WAV -> score JSON (melody; lyrics too with --words from a speech recognizer).
    std::vector<float> audio;
    double wavRate = 0;
    std::string err;
    if (!readWav(input, audio, wavRate, err)) {
      std::cerr << "error: " << err << "\n";
      novox_destroy(e);
      return 1;
    }
    std::string words = wordsPath.empty() ? "" : readFile(wordsPath);
    if (algorithm != "legacy" && algorithm != "singing-v1" && algorithm != "singing-v2") { std::cerr << "Unknown transcription algorithm\n"; novox_destroy(e); return 1; }
    if (detail != "natural" && detail != "detailed") { std::cerr << "Unknown transcription detail\n"; novox_destroy(e); return 1; }
    const std::string options = "{\"algorithm\":\"" + algorithm + "\",\"detail\":\"" + detail + "\"}";
    char* json = novox_transcribe(e, audio.data(), audio.size(), wavRate, words.empty() ? nullptr : words.c_str(), options.c_str());
    if (!json) {
      std::cerr << "error: " << novox_last_error(e) << "\n";
      status = 1;
    } else if (outGiven) {
      std::ofstream(out) << json;
      std::cerr << "wrote " << out << "\n";
      novox_free(json);
    } else {
      std::cout << json;
      novox_free(json);
    }
  } else if (cmd == "phonemes") {
    char* p = novox_phonemize(e, input.c_str());
    std::cout << p << "\n";
    novox_free(p);
  } else if (cmd == "render" || cmd == "plan" || cmd == "say" || cmd == "import") {
    std::string score;
    if (cmd == "say") {
      // One note per syllable on a single pitch: a quick pronunciation check.
      char* phon = novox_phonemize(e, input.c_str());
      std::istringstream syms(phon);
      novox_free(phon);
      static const char* kVowels[] = {"AA", "AE", "AH", "AO", "AW", "AX", "AY", "EH",
                                      "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"};
      int syllables = 0;
      std::string sym;
      while (syms >> sym)
        for (const char* v : kVowels) syllables += sym == v;
      std::ostringstream s;
      s << "{\"tempo\": " << tempo << ", \"lyrics\": \"" << jsonEscape(input) << "\", \"notes\": [";
      for (int k = 0; k < std::max(1, syllables); ++k)
        s << (k ? ", " : "") << "\"" << pitch << " " << beats << "\"";
      s << "]";
      if (!voice.empty()) s << ", \"voice\": \"" << voice << "\"";
      s << "}";
      score = s.str();
    } else {
      score = readFile(input);
      if (score.empty()) {
        std::cerr << "could not read " << input << "\n";
        return 1;
      }
      // MIDI and MusicXML go through the importer; JSON scores are used as-is.
      size_t first = score.find_first_not_of(" \t\r\n");
      bool isJson = first != std::string::npos && score[first] == '{';
      if (!isJson || cmd == "import") {
        std::string opts = "{\"track\": " + std::to_string(track) + ", \"lyrics\": \"" + jsonEscape(lyrics) + "\"}";
        char* imported = novox_import(e, score.data(), score.size(), opts.c_str());
        if (!imported) {
          std::cerr << "error: " << novox_last_error(e) << "\n";
          return 1;
        }
        score = imported;
        novox_free(imported);
        size_t w = score.find("\"importWarnings\"");
        if (w != std::string::npos) std::cerr << "note: " << score.substr(w, score.find('\n', w) - w) << "\n";
      } else if (!lyrics.empty()) {
        size_t brace = score.rfind('}');
        if (brace != std::string::npos) score.insert(brace, ", \"lyrics\": \"" + jsonEscape(lyrics) + "\"");
      }
      if (!voice.empty()) {
        // Command-line voice wins: splice it in as the last key.
        size_t brace = score.rfind('}');
        if (brace != std::string::npos) score.insert(brace, ", \"voice\": \"" + voice + "\"");
      }
    }

    if (cmd == "import") {
      if (outGiven) {
        std::ofstream(out) << score;
        std::cerr << "wrote " << out << "\n";
      } else {
        std::cout << score;
      }
    } else if (cmd == "plan") {
      char* plan = novox_plan(e, score.c_str());
      if (!plan) {
        std::cerr << "error: " << novox_last_error(e) << "\n";
        status = 1;
      } else {
        std::cout << plan;
        novox_free(plan);
      }
    } else {
      float* samples = nullptr;
      size_t n = 0;
      if (novox_render(e, score.c_str(), &samples, &n) != 0) {
        std::cerr << "error: " << novox_last_error(e) << "\n";
        status = 1;
      } else {
        if (!writeWav(out, samples, n, static_cast<int>(rate))) {
          std::cerr << "could not write " << out << "\n";
          status = 1;
        } else {
          std::cerr << "wrote " << out << " (" << static_cast<double>(n) / rate << " s)\n";
        }
        novox_free(samples);
      }
    }
  } else {
    usage();
    status = 1;
  }
  novox_destroy(e);
  return status;
}
