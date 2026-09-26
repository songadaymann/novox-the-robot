// Engine tests. Plain asserts, no framework: build with CMake and run `ctest`
// (or build/novox_tests directly).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "import.hpp"
#include "json.hpp"
#include "lexicon.hpp"
#include "novox.h"
#include "planner.hpp"
#include "score.hpp"
#include "synth.hpp"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    ++checks;                                                                           \
    if (!(cond)) {                                                                      \
      ++failures;                                                                       \
      std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond << "\n";      \
    }                                                                                   \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

const std::string kDemo = readFile(std::string(NOVOX_SOURCE_DIR) + "/scores/demo.json");

novox::Lexicon embeddedLexicon() {
  novox::Lexicon lex;
  lex.loadEmbedded();
  return lex;
}

novox::Plan planOf(const std::string& json) {
  novox::Score score = novox::parseScore(novox::json::parse(json));
  novox::Lexicon lex = embeddedLexicon();
  for (const auto& [w, p] : score.dictionary) lex.addWord(w, p);
  return novox::planScore(score, lex);
}

// ---------------------------------------------------------------------------

void testJson() {
  auto v = novox::json::parse("{\"a\": [1, 2, 3,], // comment\n \"b\": \"x\\ny\", \"c\": true}");
  CHECK(v["a"].array.size() == 3);
  CHECK(v["b"].string == "x\ny");
  CHECK(v["c"].boolean);
  bool threw = false;
  try {
    novox::json::parse("{\"a\": }");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

void testPitchAndLyrics() {
  float m = 0;
  CHECK(novox::parsePitch("C4", m) && m == 60);
  CHECK(novox::parsePitch("F#3", m) && m == 54);
  CHECK(novox::parsePitch("Bb5", m) && m == 82);
  CHECK(novox::parsePitch("61.5", m) && m == 61.5f);
  CHECK(!novox::parsePitch("H2", m));
  auto t = novox::tokenizeLyrics("hap-py birth-day _ {HH EH L OW}");
  CHECK(t.size() == 6);
  CHECK(t[0] == "hap-" && t[1] == "py" && t[4] == "_" && t[5] == "{HH EH L OW}");
}

void testLexicon() {
  novox::Lexicon lex = embeddedLexicon();
  CHECK(lex.size() > 100000);
  bool known = false;
  auto canoe = lex.lookup("Canoe", &known);
  CHECK(known);
  int vowels = 0;
  for (const auto& p : canoe) vowels += novox::isVowelSymbol(p.symbol);
  CHECK(vowels == 2);
  CHECK(lex.addWord("hugh", "HH Y UW1"));
  auto hugh = lex.lookup("Hugh");
  CHECK(hugh.size() == 3 && hugh[2].symbol == "UW");
  auto guessed = lex.lookup("zorbulator", &known);
  CHECK(!known && !guessed.empty());
}

void testLyricFlow() {
  // "canoe" flows over two notes; "_" holds; a missing lyric hums.
  novox::Plan p = planOf(R"({"tempo": 120, "lyrics": "the canoe _", "notes": ["C4 1", "D4 1", "E4 1", "F4 1", "G4 1"]})");
  CHECK(p.syllables.size() == 4);
  CHECK(p.syllables[0].text == "the");
  CHECK(p.syllables[1].text == "canoe" && p.syllables[1].continues);
  CHECK(p.syllables[2].text.empty() && !p.syllables[2].continues);
  CHECK_NEAR(p.syllables[2].end, 2.0, 1e-6);  // "noe" held over the "_" note (ends at beat 4)
  CHECK(p.syllables[3].phonemes.find("M") != std::string::npos);  // hum
}

void testPlanInvariants() {
  novox::Plan p = planOf(kDemo);
  CHECK(!p.segments.empty());
  CHECK(p.warnings.empty());
  for (size_t i = 0; i < p.segments.size(); ++i) {
    const auto& s = p.segments[i];
    CHECK(s.end > s.start);
    if (i > 0) CHECK_NEAR(p.segments[i - 1].end, s.start, 1e-9);
  }
  CHECK_NEAR(p.segments.front().start, 0.0, 1e-9);
  CHECK(p.length >= p.segments.back().end - 1e-9);
  // Vowels start exactly on their notes.
  for (const auto& syl : p.syllables) {
    bool found = false;
    for (const auto& s : p.segments) {
      if (s.kind == novox::SegKind::Vowel && std::fabs(s.start - (syl.start + p.lead)) < 1e-6) found = true;
    }
    CHECK(found);
  }
}

std::vector<float> offline(const std::string& json, bool normalize) {
  novox::Score score = novox::parseScore(novox::json::parse(json));
  novox::Lexicon lex = embeddedLexicon();
  for (const auto& [w, p] : score.dictionary) lex.addWord(w, p);
  novox::RenderOptions o;
  o.sampleRate = 44100;
  o.normalize = normalize;
  return novox::renderPlan(novox::planScore(score, lex), score.voice, o);
}

void testDeterminismAndStreaming() {
  auto a = offline(kDemo, false);
  auto b = offline(kDemo, false);
  CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);

  // Streaming in awkward chunk sizes gives bit-identical audio.
  novox::Score score = novox::parseScore(novox::json::parse(kDemo));
  novox::Lexicon lex = embeddedLexicon();
  for (const auto& [w, p] : score.dictionary) lex.addWord(w, p);
  novox::Stream stream(novox::planScore(score, lex), score.voice, 44100);
  CHECK(stream.length() == a.size());
  std::vector<float> s(stream.length() + 1000);
  size_t pos = 0;
  const size_t sizes[] = {37, 511, 1, 64, 4096, 13};
  for (size_t k = 0; pos < s.size(); ++k) {
    size_t n = std::min(sizes[k % 6], s.size() - pos);
    stream.process(s.data() + pos, n);
    pos += n;
  }
  bool same = true;
  for (size_t i = 0; i < a.size(); ++i) same = same && a[i] == s[i];
  CHECK(same);
  for (size_t i = a.size(); i < s.size(); ++i) CHECK(s[i] == 0.0f);

  // Output level: loud enough to use, not clipping.
  float peak = 0;
  for (float x : a) peak = std::max(peak, std::fabs(x));
  float dbfs = 20 * std::log10(peak);
  std::cout << "  demo peak " << dbfs << " dBFS\n";
  CHECK(dbfs > -12.0f && dbfs < -0.5f);

  // Seeking lands where asked and keeps singing.
  stream.seek(44100 * 5);
  CHECK(stream.position() == 44100 * 5);
  std::vector<float> chunk(4410);
  CHECK(stream.process(chunk.data(), chunk.size()) == chunk.size());
  float energy = 0;
  for (float x : chunk) energy += x * x;
  CHECK(energy > 0);

  // Choir voices stream identically too.
  std::string choir = kDemo;
  choir.insert(choir.rfind('}'), ", \"voice\": \"choir\"");
  auto c1 = offline(choir, false);
  CHECK(!c1.empty());
}

std::vector<uint8_t> tinyMidi() {
  // Format 0, 480 ticks/quarter, 120 bpm: C4 "hel-" (1 beat), E4 "lo" (1 beat), a
  // simultaneous lower chord note that should be dropped.
  std::vector<uint8_t> trk = {
      0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,        // tempo 500000
      0x00, 0xFF, 0x05, 0x04, 'h', 'e', 'l', '-',      // lyric
      0x00, 0x90, 60, 100,                              // C4 on
      0x00, 0x90, 48, 100,                              // C3 on (chord)
      0x83, 0x60, 0x80, 60, 0,                          // +480: C4 off
      0x00, 0x80, 48, 0,                                // C3 off
      0x00, 0xFF, 0x05, 0x02, 'l', 'o',                 // lyric
      0x00, 0x90, 64, 100,                              // E4 on
      0x83, 0x60, 0x90, 64, 0,                          // +480: E4 off (velocity 0)
      0x00, 0xFF, 0x2F, 0x00};
  std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
  uint32_t n = static_cast<uint32_t>(trk.size());
  f.push_back(static_cast<uint8_t>(n >> 24));
  f.push_back(static_cast<uint8_t>(n >> 16));
  f.push_back(static_cast<uint8_t>(n >> 8));
  f.push_back(static_cast<uint8_t>(n));
  f.insert(f.end(), trk.begin(), trk.end());
  return f;
}

void testMidiImport() {
  std::vector<std::string> warnings;
  novox::Score s = novox::importAny(tinyMidi(), {}, warnings);
  CHECK(s.notes.size() == 2);
  CHECK_NEAR(s.tempo, 120.0, 1e-6);
  CHECK(s.notes[0].pitch == 60 && s.notes[1].pitch == 64);
  CHECK_NEAR(s.notes[0].start, 0.0, 1e-9);
  CHECK_NEAR(s.notes[1].start, 0.5, 1e-9);
  CHECK_NEAR(s.notes[1].dur, 0.5, 1e-9);
  CHECK(s.notes[0].lyric == "hel-" && s.notes[1].lyric == "lo");
  CHECK(warnings.size() == 1);  // chord note dropped
  // Round-trips through JSON into the renderer.
  novox::Plan p = planOf(novox::scoreToJson(s));
  CHECK(p.syllables.size() == 2 && p.syllables[0].text == "hel" && p.syllables[0].continues);
}

void testMusicXmlImport() {
  const char* xml = R"(<?xml version="1.0"?>
<!DOCTYPE score-partwise PUBLIC "-//Recordare//DTD MusicXML 3.1 Partwise//EN" "x">
<score-partwise><part-list><score-part id="P1"/></part-list><part id="P1"><measure number="1">
<attributes><divisions>2</divisions></attributes>
<direction><sound tempo="60"/></direction>
<note><pitch><step>C</step><octave>4</octave></pitch><duration>2</duration><voice>1</voice>
  <lyric><syllabic>begin</syllabic><text>sing</text></lyric></note>
<note><pitch><step>D</step><octave>4</octave></pitch><duration>2</duration><voice>1</voice><tie type="start"/>
  <lyric><syllabic>end</syllabic><text>ing</text><extend/></lyric></note>
<note><pitch><step>D</step><octave>4</octave></pitch><duration>2</duration><voice>1</voice><tie type="stop"/></note>
<note><pitch><step>F</step><alter>1</alter><octave>4</octave></pitch><duration>2</duration><voice>1</voice></note>
<backup><duration>8</duration></backup>
<note><pitch><step>C</step><octave>3</octave></pitch><duration>8</duration><voice>2</voice></note>
</measure></part></score-partwise>)";
  std::vector<std::string> warnings;
  std::vector<uint8_t> bytes(xml, xml + std::strlen(xml));
  novox::Score s = novox::importAny(bytes, {}, warnings);
  CHECK(s.notes.size() == 3);  // tie merged, voice 2 skipped
  CHECK_NEAR(s.notes[1].dur, 2.0, 1e-9);
  CHECK(s.notes[0].lyric == "sing-" && s.notes[1].lyric == "ing" && s.notes[2].lyric == "_");
  CHECK(s.notes[2].pitch == 66);
  CHECK_NEAR(s.tempo, 60.0, 1e-9);
}

void testCApi() {
  novox_engine* e = novox_create(22050);
  float* buf = nullptr;
  size_t n = 0;
  CHECK(novox_render(e, "{\"notes\": [\"X9 1\"]}", &buf, &n) != 0);
  CHECK(std::string(novox_last_error(e)).find("bad pitch") != std::string::npos);
  CHECK(novox_render(e, "{\"tempo\": 120, \"lyrics\": \"la\", \"notes\": [\"A3 1\"]}", &buf, &n) == 0);
  CHECK(n > 22050 / 2);
  novox_free(buf);
  novox_stream* s = novox_stream_create(e, "{\"tempo\": 120, \"lyrics\": \"la la\", \"notes\": [\"A3 1\", \"C4 1\"]}");
  CHECK(s != nullptr);
  CHECK(novox_stream_lead(s) > 0);
  std::vector<float> out(512);
  size_t total = 0;
  while (size_t got = novox_stream_process(s, out.data(), out.size())) total += got;
  CHECK(total == novox_stream_length(s));
  novox_stream_destroy(s);
  auto midi = tinyMidi();
  char* json = novox_import(e, midi.data(), midi.size(), "{\"voice\": \"robot\"}");
  CHECK(json && std::string(json).find("\"voice\": \"robot\"") != std::string::npos);
  CHECK(json && std::string(json).find("importWarnings") != std::string::npos);
  novox_free(json);
  novox_destroy(e);
}

}  // namespace

int main() {
  const std::pair<const char*, std::function<void()>> tests[] = {
      {"json", testJson},
      {"pitch and lyric tokens", testPitchAndLyrics},
      {"lexicon", testLexicon},
      {"lyric flow", testLyricFlow},
      {"plan invariants", testPlanInvariants},
      {"determinism and streaming", testDeterminismAndStreaming},
      {"MIDI import", testMidiImport},
      {"MusicXML import", testMusicXmlImport},
      {"C API", testCApi},
  };
  for (const auto& [name, fn] : tests) {
    int before = failures;
    try {
      fn();
    } catch (const std::exception& ex) {
      ++failures;
      std::cerr << "  EXCEPTION: " << ex.what() << "\n";
    }
    std::cout << (failures == before ? "ok   " : "FAIL ") << name << "\n";
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
