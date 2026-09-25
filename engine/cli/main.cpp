// nvs: command-line front end for the New Virtual Singer engine.
//
//   nvs render score.json -o out.wav [--voice robot] [--rate 48000]
//   nvs plan score.json                 print the phoneme timeline as JSON
//   nvs say "some words" -o out.wav [--pitch C4] [--voice female]
//   nvs phonemes "some words"
//   nvs voices

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nvs.h"

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

std::string readFile(const std::string& path) {
  std::ifstream f(path);
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

// Finds data/cmudict.dict next to the executable's project, or via NVS_DICT.
std::string findDictionary(const char* argv0, const std::string& flag) {
  if (!flag.empty()) return flag;
  if (const char* env = std::getenv("NVS_DICT")) return env;
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
               "  nvs render <score.json> -o <out.wav> [--voice NAME] [--rate HZ]\n"
               "  nvs plan <score.json>\n"
               "  nvs say \"words to sing\" -o <out.wav> [--pitch C4] [--beats 1] [--tempo 100] [--voice NAME]\n"
               "  nvs phonemes \"words\"\n"
               "  nvs voices\n"
               "options: --dict PATH (default data/cmudict.dict or $NVS_DICT)\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  std::string cmd = argv[1];
  std::string input, out = "out.wav", voice, dict, pitch = "C4";
  double rate = 48000, beats = 1, tempo = 100;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "-o" || a == "--out") out = next();
    else if (a == "--voice") voice = next();
    else if (a == "--dict") dict = next();
    else if (a == "--rate") rate = std::atof(next().c_str());
    else if (a == "--pitch") pitch = next();
    else if (a == "--beats") beats = std::atof(next().c_str());
    else if (a == "--tempo") tempo = std::atof(next().c_str());
    else input = a;
  }

  if (cmd == "voices") {
    char* names = nvs_voice_names();
    std::cout << names << "\n";
    nvs_free(names);
    return 0;
  }

  nvs_engine* e = nvs_create(rate);
  if (nvs_load_dictionary(e, findDictionary(argv[0], dict).c_str()) < 0) {
    std::cerr << "warning: " << nvs_last_error(e) << " (spelling rules only)\n";
  }

  int status = 0;
  if (cmd == "phonemes") {
    char* p = nvs_phonemize(e, input.c_str());
    std::cout << p << "\n";
    nvs_free(p);
  } else if (cmd == "render" || cmd == "plan" || cmd == "say") {
    std::string score;
    if (cmd == "say") {
      // One note per syllable on a single pitch: a quick pronunciation check.
      char* phon = nvs_phonemize(e, input.c_str());
      std::istringstream syms(phon);
      nvs_free(phon);
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
      if (!voice.empty()) {
        // Command-line voice wins: splice it in as the last key.
        size_t brace = score.rfind('}');
        if (brace != std::string::npos) score.insert(brace, ", \"voice\": \"" + voice + "\"");
      }
    }

    if (cmd == "plan") {
      char* plan = nvs_plan(e, score.c_str());
      if (!plan) {
        std::cerr << "error: " << nvs_last_error(e) << "\n";
        status = 1;
      } else {
        std::cout << plan;
        nvs_free(plan);
      }
    } else {
      float* samples = nullptr;
      size_t n = 0;
      if (nvs_render(e, score.c_str(), &samples, &n) != 0) {
        std::cerr << "error: " << nvs_last_error(e) << "\n";
        status = 1;
      } else {
        if (!writeWav(out, samples, n, static_cast<int>(rate))) {
          std::cerr << "could not write " << out << "\n";
          status = 1;
        } else {
          std::cerr << "wrote " << out << " (" << static_cast<double>(n) / rate << " s)\n";
        }
        nvs_free(samples);
      }
    }
  } else {
    usage();
    status = 1;
  }
  nvs_destroy(e);
  return status;
}
