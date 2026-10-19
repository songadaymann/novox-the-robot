// Engine tests. Plain asserts, no framework: build with CMake and run `ctest`
// (or build/novox_tests directly).

#include <clocale>
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
#include "transcribe.hpp"
#include "utf8.hpp"
#include "v2_check.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;  // M_PI is not standard C++ (MSVC lacks it)

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
  // \u escapes: characters beyond the BMP (emoji) arrive as surrogate pairs,
  // as Python's json.dumps writes them; a lone half becomes U+FFFD.
  CHECK(novox::json::parse("\"caf\\u00e9\"").string == "caf\xC3\xA9");
  CHECK(novox::json::parse("\"\\ud83c\\udfb5\"").string == "\xF0\x9F\x8E\xB5");
  CHECK(novox::json::parse("\"\\ud83c x\"").string == "\xEF\xBF\xBD x");
  CHECK(novox::json::parse("\"\\udfb5\"").string == "\xEF\xBF\xBD");
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

  // "~" keeps a two-vowel word on one note and leaves the mark off its text; a
  // lone "~" is a note with no lyric (the editor writes these for imported notes).
  novox::Plan one = planOf(R"({"tempo": 120, "lyrics": "~ wanna~ go", "notes": ["C4 1", "D4 1", "E4 1"]})");
  CHECK(one.warnings.empty());
  CHECK(one.syllables.size() == 3);
  CHECK(one.syllables[0].text.empty() && one.syllables[0].phonemes == " | M | ");
  CHECK(one.syllables[1].text == "wanna" && !one.syllables[1].continues);
  CHECK(one.syllables[2].text == "go");

  // A held note inside a hyphenated word does not end the word: "member" is the
  // tail of re-member on one note, not split again by its own two vowels.
  novox::Plan held = planOf(R"({"tempo": 120, "lyrics": "re- _ member", "notes": ["C4 1", "D4 1", "E4 1", "F4 1"]})");
  CHECK(held.warnings.empty());
  CHECK(held.syllables.size() == 3);
  CHECK(held.syllables[0].text == "re" && held.syllables[0].continues);
  CHECK_NEAR(held.syllables[0].end, 1.0, 1e-6);  // "re" held over the "_" note
  CHECK(held.syllables[1].text == "member" && !held.syllables[1].continues);
  CHECK(held.syllables[2].text.empty());  // the fourth note hums
}

void testLyricTokenApi() {
  novox_engine* e = novox_create(48000);
  auto tokens = [&](const char* text, const char* dictionary = "{}") {
    char* result = novox_lyric_tokens(e, text, dictionary);
    CHECK(result != nullptr);
    std::vector<std::string> out;
    if (result) {
      const auto parsed = novox::json::parse(result);
      CHECK(parsed["tokens"].isArray());
      for (const auto& t : parsed["tokens"].array) { CHECK(t.isString()); out.push_back(t.string); }
      novox_free(result);
    }
    return out;
  };
  CHECK(tokens("remember") == std::vector<std::string>({"remember-", "-", ""}));
  CHECK(tokens("re-mem-ber") == std::vector<std::string>({"re-", "mem-", "ber"}));
  CHECK(tokens("re- _ member") == std::vector<std::string>({"re-", "_", "member"}));
  CHECK(tokens("wanna~ ~ + {R IH M EH M B ER}") == std::vector<std::string>({"wanna", "", "+", "{R IH M EH M B ER}"}));
  CHECK(tokens("cat", R"({"cat":"K AE1 T AH0 IY0"})") == std::vector<std::string>({"cat-", "-", ""}));
  CHECK(tokens("cat") == std::vector<std::string>({"cat"})); // song dictionaries never leak between instances
  CHECK(tokens("").empty());
  CHECK(tokens(nullptr).empty());
  CHECK(tokens("\"hey\" \\") == std::vector<std::string>({"\"hey\"", "\\"}));
  for (const char* dictionary : {"[1]", "{", R"({"cat":42})", R"({"cat":"BADPHONEME"})"}) {
    CHECK(novox_lyric_tokens(e, "cat", dictionary) == nullptr);
    CHECK(!std::string(novox_last_error(e)).empty());
  }
  CHECK(tokens("cat").size() == 1);
  CHECK(std::string(novox_last_error(e)).empty());
  novox_destroy(e);

  // Moving flowing tokens into explicit per-note lyrics must be acoustically
  // identical, including automatic syllables, custom splits and one-note ties.
  for (const char* line : {"I remember the canoe _", "re- _ member", "wanna~ ~ + {R IH M EH M B ER}", "hel-lo world"}) {
    novox::Score flow;
    flow.lyrics = line;
    for (size_t i = 0; i < 12; ++i) flow.notes.push_back({i * .5, .5, 60, ""});
    const auto expanded = novox::lyricFlowTokens(line, embeddedLexicon());
    auto pinned = flow;
    pinned.lyrics.clear();
    for (size_t i = 0; i < expanded.size(); ++i) pinned.notes[i].lyric = expanded[i];
    CHECK(novox::planToJson(novox::planScore(flow, embeddedLexicon())) ==
          novox::planToJson(novox::planScore(pinned, embeddedLexicon())));
  }
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

// A near-touching note boundary (2 ms) must not chop the voice with a sliver of
// silence (it clicked on "for-e" in "forever"); a real rest still goes silent.
void testShortGapsSingThrough() {
  auto silences = [](const novox::Plan& p) {
    int n = 0;
    for (size_t k = 1; k + 1 < p.segments.size(); ++k) n += p.segments[k].kind == novox::SegKind::Silence;
    return n;
  };
  const std::string forever = R"({"units": "seconds", "lyrics": "for-e-ver punk", "notes": [
    {"pitch": 67, "start": 0, "dur": 0.558}, {"pitch": 65, "start": 0.5603, "dur": 0.37},
    {"pitch": 63, "start": 0.94, "dur": 0.488}, {"pitch": 63, "start": 1.5, "dur": 0.8}]})";
  novox::Plan tight = planOf(forever);
  CHECK(silences(tight) == 0);
  CHECK(tight.notes.size() == 4 && tight.notes[1].legato);
  novox::Plan rest = planOf(R"({"units": "seconds", "lyrics": "for e", "notes": [
    {"pitch": 67, "start": 0, "dur": 0.5}, {"pitch": 65, "start": 0.6, "dur": 0.5}]})");
  CHECK(silences(rest) > 0);
  CHECK(!rest.notes[1].legato);
  // The rendered vowel-to-vowel join has no sample step far above the vowel's own.
  std::vector<float> x = offline(forever, false);
  const size_t join = static_cast<size_t>((0.559 + tight.lead) * 44100);
  float inside = 0, near = 0;
  for (size_t i = join - 4410; i < join - 441; ++i) inside = std::max(inside, std::fabs(x[i + 1] - x[i]));
  for (size_t i = join - 441; i < join + 441; ++i) near = std::max(near, std::fabs(x[i + 1] - x[i]));
  CHECK(near < inside * 3);
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

void testSparseRendering() {
  novox::RenderOptions opts;
  opts.sampleRate = 48000;
  opts.normalize = false;
  auto renderWithoutSkipping = [&](novox::Plan plan, const novox::Voice& voice) {
    // The synthesizer otherwise uses segment parameters, not their kind. A
    // zero-amplitude closure exercises the original sample-by-sample path.
    for (auto& segment : plan.segments)
      if (segment.kind == novox::SegKind::Silence) segment.kind = novox::SegKind::Closure;
    return novox::renderPlan(plan, voice, opts);
  };
  auto chunked = [&](const novox::Plan& plan, const novox::Voice& voice, const std::vector<float>& whole) {
    novox::Stream stream(plan, voice, opts.sampleRate);
    std::vector<float> out(stream.length());
    const size_t sizes[] = {1, 37, 511, 32768, 19, 64};
    size_t pos = 0;
    for (size_t i = 0; pos < out.size(); ++i) {
      const size_t count = std::min(sizes[i % 6], out.size() - pos);
      stream.process(out.data() + pos, count);
      pos += count;
    }
    CHECK(out.size() == whole.size());
    CHECK(std::memcmp(out.data(), whole.data(), out.size() * sizeof(float)) == 0);
  };
  auto energy = [&](const std::vector<float>& samples, double from, double to) {
    double sum = 0;
    for (size_t i = static_cast<size_t>(from * opts.sampleRate);
         i < std::min(samples.size(), static_cast<size_t>(to * opts.sampleRate)); ++i)
      sum += static_cast<double>(samples[i]) * samples[i];
    return sum;
  };
  // Tight phrases and short rests retain the exact old waveform, including the
  // random source, choir spread, consonants and the normal 300 ms release.
  auto dense = novox::parseScore(novox::json::parse(R"({"units":"seconds","voice":"choir",
    "lyrics":"strange fly sing", "notes":[{"pitch":60,"start":0,"dur":0.5},
    {"pitch":64,"start":0.6,"dur":0.5},{"pitch":67,"start":1.2,"dur":0.5}]})"));
  auto densePlan = novox::planScore(dense, embeddedLexicon());
  auto denseAudio = novox::renderPlan(densePlan, dense.voice, opts);
  auto denseReference = renderWithoutSkipping(densePlan, dense.voice);
  CHECK(denseAudio == denseReference);

  // A long leading rest is still present, and the consonants begin at their
  // planned time rather than being skipped up to the first pitched note.
  auto leading = dense;
  for (auto& note : leading.notes) note.start += 3.0;
  auto leadingPlan = novox::planScore(leading, embeddedLexicon());
  auto leadingAudio = novox::renderPlan(leadingPlan, leading.voice, opts);
  CHECK(leadingAudio.size() == static_cast<size_t>((leadingPlan.length + leading.voice.choirSpread) * opts.sampleRate));
  CHECK(energy(leadingAudio, 0, 2.5) == 0);
  const double onset = leadingPlan.segments.front().end;
  CHECK(energy(leadingAudio, onset, 3.0 + leadingPlan.lead) > 1e-8);
  CHECK(energy(leadingAudio, 3.0 + leadingPlan.lead, 3.4 + leadingPlan.lead) > 1e-4);
  // Skipping resets the source phase in the silent span. A different waveform
  // from the forced sample path proves that the optimization actually ran;
  // output/chunk comparisons alone also pass when every rest is synthesized.
  CHECK(leadingAudio != renderWithoutSkipping(leadingPlan, leading.voice));
  chunked(leadingPlan, leading.voice, leadingAudio);

  // Both vowel/sonorant and fricative onsets must qualify. silenceLike keeps
  // relative N1A/N2A/N3A gains near fricatives; AF=0 is what silences them.
  for (const char* lyric : {"easy", "hello", "fly", "zoo"}) {
    auto onsetScore = dense;
    onsetScore.voice.choirSize = 1;
    onsetScore.notes.resize(1);
    onsetScore.notes[0].start = 2.0;
    onsetScore.lyrics = lyric;
    auto onsetPlan = novox::planScore(onsetScore, embeddedLexicon());
    auto actual = novox::renderPlan(onsetPlan, onsetScore.voice, opts);
    CHECK(actual != renderWithoutSkipping(onsetPlan, onsetScore.voice));
    CHECK(energy(actual, 0, 1.5) == 0);
    CHECK(energy(actual, 2.0 + onsetPlan.lead, 2.4 + onsetPlan.lead) > 1e-4);
  }

  // The first phrase and its release are preserved through a long interior
  // rest. A very low, resonant EQ band deliberately rings beyond 300 ms: the
  // state threshold must prevent a fixed-duration tail cutoff.
  for (bool ringing : {false, true}) {
    auto interior = dense;
    interior.notes.resize(2);
    interior.notes[1].start = 3.0;
    interior.lyrics = "strange fly";
    if (ringing) {
      interior.voice.choirSize = 1;
      interior.voice.eqBands = 1;
      interior.voice.eq[0][0] = 45;
      interior.voice.eq[0][1] = 18;
      interior.voice.eq[0][2] = 12;
    }
    auto plan = novox::planScore(interior, embeddedLexicon());
    auto actual = novox::renderPlan(plan, interior.voice, opts);
    auto reference = renderWithoutSkipping(plan, interior.voice);
    CHECK(actual.size() == reference.size());
    double delta = 0;
    for (size_t i = 0; i < static_cast<size_t>(2.5 * opts.sampleRate); ++i)
      delta = std::max(delta, std::fabs(static_cast<double>(actual[i]) - reference[i]));
    CHECK(delta <= 1e-8);
    if (!ringing) CHECK(actual != reference);
    if (ringing) CHECK(energy(actual, 1.0, 1.4) > 1e-12);
    CHECK(energy(actual, 3.0 + plan.lead, 3.4 + plan.lead) > 1e-4);
    chunked(plan, interior.voice, actual);
  }
  // Seeking into a skipped rest remains silent and can resume across the
  // boundary, even when the first chunk ends partway through that rest.
  novox::Stream sought(leadingPlan, leading.voice, opts.sampleRate);
  sought.seek(static_cast<size_t>(opts.sampleRate));
  std::vector<float> rest(777);
  sought.process(rest.data(), rest.size());
  CHECK(energy(rest, 0, 1) == 0);
  sought.seek(static_cast<size_t>(2.5 * opts.sampleRate));
  std::vector<float> attack(static_cast<size_t>(opts.sampleRate));
  sought.process(attack.data(), attack.size());
  CHECK(energy(attack, 0.5, 0.9) > 1e-4);
  // Non-48 kHz hosts must make the same skip/resume decisions regardless of
  // buffer size. In particular the warm-up endpoints need not be whole samples.
  for (double rate : {44100.0, 96000.0}) {
    opts.sampleRate = rate;
    auto actual = novox::renderPlan(leadingPlan, leading.voice, opts);
    CHECK(energy(actual, 0, 2.5) == 0);
    CHECK(energy(actual, 3.0 + leadingPlan.lead, 3.4 + leadingPlan.lead) > 1e-4);
    chunked(leadingPlan, leading.voice, actual);
  }
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

// Logic-style region export: notes keep their absolute song position, and the
// tempo changes from 120 to 106 bpm at beat 24, before the region at beat 28.
std::vector<uint8_t> tempoChangeMidi() {
  std::vector<uint8_t> trk = {
      0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,        // tick 0: 120 bpm
      0xDA, 0x00, 0xFF, 0x51, 0x03, 0x08, 0xA3, 0x16,  // tick 11520 (beat 24): 106 bpm
      0x87, 0x40, 0x90, 60, 100,                        // +960: beat 26 on
      0x83, 0x60, 0x80, 60, 0,                          // +480: off at beat 27
      0x83, 0x60, 0x90, 62, 100,                        // +480: beat 28 on
      0x81, 0x70, 0x80, 62, 0,                          // +240: off at beat 28.5
      0x00, 0xFF, 0x2F, 0x00};
  std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
  uint32_t n = static_cast<uint32_t>(trk.size());
  for (int shift = 24; shift >= 0; shift -= 8) f.push_back(static_cast<uint8_t>(n >> shift));
  f.insert(f.end(), trk.begin(), trk.end());
  return f;
}

void testMidiImportBeats() {
  std::vector<std::string> warnings;
  novox::ImportOptions beats;
  beats.beats = true;
  novox::Score b = novox::importAny(tempoChangeMidi(), beats, warnings);
  CHECK(b.notes.size() == 2);
  CHECK_NEAR(b.notes[0].start, 26.0, 1e-9);
  CHECK_NEAR(b.notes[1].start, 28.0, 1e-9);
  CHECK_NEAR(b.notes[1].dur, 0.5, 1e-9);
  novox::Score sec = novox::importAny(tempoChangeMidi(), {}, warnings);
  CHECK_NEAR(sec.notes[1].start, 24 * 0.5 + 4 * 60.0 / 106, 1e-4);  // seconds follow the tempo map
  novox_engine* e = novox_create(22050);
  auto midi = tempoChangeMidi();
  char* json = novox_import(e, midi.data(), midi.size(), "{\"units\": \"beats\"}");
  CHECK(json && std::string(json).find("\"units\": \"beats\"") != std::string::npos);
  CHECK(json && std::string(json).find("\"start\": 28.000000") != std::string::npos);
  novox_free(json);
  novox_destroy(e);
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

void testTranscribeMelody() {
  // A sine-like "voice" (with harmonics) singing C4 D4 E4 E4 (repeat after a short gap) G4.
  const double sr = 44100;
  struct N { double start, dur; float midi; };
  const N melody[] = {{0.2, 0.4, 60}, {0.6, 0.4, 62}, {1.0, 0.35, 64}, {1.4, 0.35, 64}, {1.8, 0.6, 67}};
  std::vector<float> x(static_cast<size_t>(2.8 * sr), 0.0f);
  for (const auto& n : melody) {
    const double f = 440.0 * std::pow(2.0, (n.midi - 69) / 12.0);
    for (size_t i = static_cast<size_t>(n.start * sr); i < static_cast<size_t>((n.start + n.dur - 0.03) * sr); ++i) {
      const double t = i / sr;
      x[i] = static_cast<float>(0.3 * std::sin(2 * kPi * f * t) + 0.15 * std::sin(4 * kPi * f * t) +
                                0.08 * std::sin(6 * kPi * f * t));
    }
  }
  auto frames = novox::trackPitch(x.data(), x.size(), sr);
  auto notes = novox::segmentNotes(frames);
  CHECK(notes.size() == 5);
  for (size_t i = 0; i < std::min<size_t>(5, notes.size()); ++i) {
    CHECK(notes[i].note.pitch == melody[i].midi);
    CHECK_NEAR(notes[i].note.start, melody[i].start, 0.03);
  }
}

void testTranscribeHeldNotes() {
  auto framesFor = [](auto pitch) {
    std::vector<novox::PitchFrame> frames;
    for (int i = 0; i < 250; ++i) {
      novox::PitchFrame f;
      f.time = 0.016 + i * 0.008;
      f.midi = pitch(i);
      f.db = -12;
      f.clarity = 0.99f;
      frames.push_back(f);
    }
    return frames;
  };
  auto held = framesFor([](int i) { return 60.0f + 0.9f * std::sin(2 * kPi * 6 * i * .008); });
  novox::MelodyOptions stable;
  stable.stabilizePitch = true;
  CHECK(novox::segmentNotes(held, stable).size() == 1);
  // Brief tracker excursions and dropouts are not new sung events.
  for (int i = 100; i < 105; ++i) held[i].midi = 61;
  for (int i = 160; i < 163; ++i) held[i].midi = 0;
  CHECK(novox::segmentNotes(held, stable).size() == 1);
  auto steps = framesFor([](int i) { return i < 100 ? 60.0f : i < 115 ? 61.0f : 60.0f; });
  auto notes = novox::segmentNotes(steps, stable);
  CHECK(notes.size() == 3); // deliberate 120 ms semitone note survives
  if (notes.size() == 3) CHECK(notes[1].note.pitch == 61);
  auto repeats = framesFor([](int) { return 60.0f; });
  for (int i = 120; i < 128; ++i) repeats[i].midi = 0, repeats[i].db = -90;
  CHECK(novox::segmentNotes(repeats, stable).size() == 2);
}

void testSingingV1() {
  novox::MelodyOptions options;
  options.algorithm = "singing-v1";
  auto tone = [](double sr, const std::vector<int>& pitches, double dur, double fundamental = .3, bool vibrato = false) {
    std::vector<float> audio(static_cast<size_t>((pitches.size()*dur+.3)*sr));
    double phase = 0;
    for (size_t i=0; i<audio.size(); ++i) {
      double t=i/sr-.1;
      if (t<0 || t>=pitches.size()*dur) continue;
      const auto index=std::min(pitches.size()-1,static_cast<size_t>(t/dur));
      double midi=pitches[index]+(vibrato ? .9*std::sin(2*kPi*6*t):0);
      phase+=2*kPi*440*std::pow(2.,(midi-69)/12)/sr;
      audio[i]=static_cast<float>(fundamental*std::sin(phase)+.45*std::sin(2*phase)+.1*std::sin(3*phase));
    }
    return audio;
  };
  for (double sr : {16000.,44100.,48000.}) {
    auto audio=tone(sr,{60,72,60},.16);
    auto frames=novox::trackSingingPitch(audio.data(),audio.size(),sr);
    auto notes=novox::segmentNotes(frames,options);
    CHECK(notes.size()==3);
    if(notes.size()==3) {
      CHECK(notes[0].note.pitch==60 && notes[1].note.pitch==72 && notes[2].note.pitch==60);
      CHECK_NEAR(notes[1].note.start,.26,.032);
    }
    audio=tone(sr,{60},1.,.15,true);
    notes=novox::segmentNotes(novox::trackSingingPitch(audio.data(),audio.size(),sr),options);
    CHECK(notes.size()==1);
    if(!notes.empty()) CHECK(notes[0].note.pitch==60);
  }
  // Increasing the period preference must still preserve a short real leap.
  auto shortLeap = tone(16000, {60,72,60}, .12);
  auto shortNotes = novox::segmentNotes(novox::trackSingingPitch(shortLeap.data(),shortLeap.size(),16000), options);
  CHECK(shortNotes.size()==3);
  if(shortNotes.size()==3) CHECK(shortNotes[1].note.pitch==72);
  auto weakFundamental = tone(16000, {60}, .8, .05);
  auto weakNotes = novox::segmentNotes(novox::trackSingingPitch(weakFundamental.data(),weakFundamental.size(),16000), options);
  CHECK(weakNotes.size()==1);
  if(!weakNotes.empty()) CHECK(weakNotes[0].note.pitch==60);
  // An energetic non-periodic breath is not a low sung note.
  std::vector<float> breath(16000);
  uint32_t noiseState = 12345;
  for(auto& sample : breath) {noiseState = noiseState*1664525u+1013904223u; sample = .2f*(static_cast<float>(noiseState >> 8)/8388608.f-1.f);}
  CHECK(novox::segmentNotes(novox::trackSingingPitch(breath.data(),breath.size(),16000),options).empty());
  // Acoustic octave ambiguity from a changing harmonic balance is not a leap.
  auto harmonic=tone(16000,{60},1.);
  for(size_t i=8000;i<8768;++i) {
    const double phase=2*kPi*261.625565*(i-1600)/16000.;
    harmonic[i]=static_cast<float>(.005*std::sin(phase)+.45*std::sin(2*phase)+.005*std::sin(3*phase));
  }
  auto harmonicNotes=novox::segmentNotes(novox::trackSingingPitch(harmonic.data(),harmonic.size(),16000),options);
  CHECK(harmonicNotes.size()==1);
  if(!harmonicNotes.empty()) CHECK(harmonicNotes[0].note.pitch==60);
  auto scoop=tone(16000,{60},1.);
  double scoopPhase=0;
  for(size_t i=1600;i<17600;++i) {
    double t=(i-1600)/16000.;double midi=56+4*std::min(1.,t/.12);
    scoopPhase+=2*kPi*440*std::pow(2.,(midi-69)/12)/16000.;
    scoop[i]=static_cast<float>(.3*std::sin(scoopPhase)+.45*std::sin(2*scoopPhase)+.1*std::sin(3*scoopPhase));
  }
  auto scoopNotes=novox::segmentNotes(novox::trackSingingPitch(scoop.data(),scoop.size(),16000),options);
  CHECK(scoopNotes.size()==1);
  if(!scoopNotes.empty()) CHECK(scoopNotes[0].note.pitch==60);
  // Supported events at both recording edges remain inside the decoded file.
  auto edges=tone(16000,{60},.6);
  edges.erase(edges.begin(),edges.begin()+1600); edges.resize(9600);
  auto edgeNotes=novox::segmentNotes(novox::trackSingingPitch(edges.data(),edges.size(),16000),options);
  CHECK(edgeNotes.size()==1);
  if(!edgeNotes.empty()) { CHECK(edgeNotes[0].note.start>=0 && edgeNotes[0].note.start<.032);
    CHECK(edgeNotes[0].note.start+edgeNotes[0].note.dur<=.600001); }
  // A new syllable with a clear acoustic valley remains a repeated note.
  auto repeat=tone(16000,{60},1.);
  for(size_t i=7200;i<8320;++i) repeat[i]=0;
  CHECK(novox::segmentNotes(novox::trackSingingPitch(repeat.data(),repeat.size(),16000),options).size()==2);
  std::vector<float> silence(16000);
  CHECK(novox::segmentNotes(novox::trackSingingPitch(silence.data(),silence.size(),16000),options).empty());
  for(size_t i=0;i<silence.size();++i) silence[i]=1e-6f*std::sin(i*1.231);
  CHECK(novox::segmentNotes(novox::trackSingingPitch(silence.data(),silence.size(),16000),options).empty());
  std::vector<novox::PitchFrame> frames(150);
  for(size_t i=0;i<frames.size();++i) {frames[i].time=i*.008;frames[i].midi=i>=60&&i<75?61:60;frames[i].clarity=.99;frames[i].db=-12;}
  auto notes=novox::segmentNotes(frames,options);
  CHECK(notes.size()==3);
  for(size_t i=60;i<75;++i) frames[i].midi=72;
  notes=novox::segmentNotes(frames,options);
  CHECK(notes.size()==3);
  if(notes.size()==3) CHECK(notes[1].note.pitch==72);
  for(size_t i=0;i<frames.size();++i) frames[i].midi=60;
  for(size_t i=60;i<66;++i) frames[i].midi=0;
  CHECK(novox::segmentNotes(frames,options).size()==1); // tracker dropout, unchanged acoustic energy
  for(size_t i=0;i<frames.size();++i) frames[i].midi=i<10?57+3.f*i/10:60;
  notes=novox::segmentNotes(frames,options);
  CHECK(notes.size()==1);
  if(!notes.empty()) CHECK(notes[0].note.pitch==60);
  std::vector<novox::TranscribedNote> held(1);
  held[0].note.start=.596;held[0].note.dur=1.;held[0].note.pitch=60;
  std::vector<novox::Word> words={{"Can",0,.6,1},{"you",.6,.82,1},{"rest",2,3,1},{"rest",4,5,1}};
  std::vector<size_t> unresolved;
  novox::alignLyrics(held,words,embeddedLexicon(),true,&unresolved);
  CHECK(!held.empty() && held[0].note.lyric.find("Can")!=std::string::npos);
  CHECK(unresolved.size()>=2);
  const auto metadata=novox::json::parse(novox::transcriptionToJson(held,{},"singing-v1",&words,&unresolved));
  CHECK(metadata["transcription"]["transcriptWords"].array.size()==words.size());
  auto* engine=novox_create(16000);
  CHECK(novox_transcribe(engine,silence.data(),silence.size(),16000,nullptr,"{\"algorithm\":\"typo\"}")==nullptr);
  CHECK(novox_transcribe(engine,silence.data(),silence.size(),16000,"{}","{\"algorithm\":\"singing-v1\"}")==nullptr);
  CHECK(novox_transcribe(engine,silence.data(),silence.size(),16000,"[{\"word\":\"bad\",\"start\":2,\"end\":1}]",nullptr)==nullptr);
  CHECK(novox_transcribe(engine,silence.data(),silence.size(),16000,nullptr,"{\"debugFrames\":1}")==nullptr);
  // debugFrames exposes the analysis frames without changing the notes.
  auto leap=tone(16000,{60,72},.3);
  char* plain=novox_transcribe(engine,leap.data(),leap.size(),16000,nullptr,"{\"algorithm\":\"singing-v1\"}");
  char* debug=novox_transcribe(engine,leap.data(),leap.size(),16000,nullptr,"{\"algorithm\":\"singing-v1\",\"debugFrames\":true}");
  CHECK(plain && debug);
  if(plain && debug) {
    const auto p=novox::json::parse(plain), d=novox::json::parse(debug);
    CHECK(!p.has("frames") && d["frames"].array.size()==(leap.size()+127)/128);
    CHECK(d["notes"].array.size()==p["notes"].array.size() && p["notes"].array.size()==2);
    for(size_t i=0;i<p["notes"].array.size();++i)
      CHECK(p["notes"].array[i].num("start",-1)==d["notes"].array[i].num("start",-2) &&
            p["notes"].array[i].num("pitch",-1)==d["notes"].array[i].num("pitch",-2));
    bool shape=true, high=false;
    for(const auto& f : d["frames"].array) {
      shape=shape && f.array.size()==5;
      if(f.array.size()==5 && f.array[0].number>.55 && f.array[0].number<.65) high=high || std::fabs(f.array[1].number-72)<.5;
    }
    CHECK(shape && high);
  }
  novox_free(plain);novox_free(debug);
  novox_destroy(engine);
}

void testAlignLyrics() {
  // Four notes, "hello" over the first two, "world" on the third, the fourth held.
  std::vector<novox::TranscribedNote> notes(4);
  const double starts[] = {0.0, 0.4, 0.8, 1.2};
  for (size_t i = 0; i < 4; ++i) {
    notes[i].note.start = starts[i];
    notes[i].note.dur = 0.38;
    notes[i].note.pitch = 60;
  }
  std::vector<novox::Word> words = {{" Hello,", 0.02, 0.75, 0.9f}, {" world", 0.82, 1.1, 0.4f}};
  novox::Lexicon lex = embeddedLexicon();
  auto warnings = novox::alignLyrics(notes, words, lex);
  CHECK(warnings.empty());
  CHECK(notes[0].note.lyric == "Hel-" && notes[1].note.lyric == "lo");
  CHECK(notes[2].note.lyric == "world" && notes[3].note.lyric == "_");
  CHECK_NEAR(notes[2].confidence, 0.4f, 1e-6);
  // The aligned notes plan into the same syllables.
  novox::Score score;
  for (const auto& n : notes) score.notes.push_back(n.note);
  novox::Plan p = novox::planScore(score, lex);
  CHECK(p.syllables.size() == 3 && p.syllables[0].continues && p.syllables[2].text == "world");
  std::vector<novox::TranscribedNote> hold(1);
  hold[0].note.start = 1; hold[0].note.dur = 2; hold[0].note.pitch = 60;
  warnings = novox::alignLyrics(hold, {{"my", 1, 1.4, 1}, {"name", 1.4, 2.8, 1}}, lex);
  CHECK(hold.size() == 2);
  CHECK_NEAR(hold[1].note.start, 1.4, 1e-6);
  CHECK_NEAR(hold[1].note.dur, 1.6, 1e-6);
  auto before = hold;
  warnings = novox::alignLyrics(hold, {{"my", 1, 1.4, 1}, {"name", 1.4, 2.8, 1}, {"hello", 4, 5, 1}}, lex);
  CHECK(hold.size() == before.size());
  CHECK(!warnings.empty());
  // Accented words keep whole UTF-8 characters in their pieces (legacy and
  // singing-v1), whatever the host's C locale: bytes tested with std::isalpha in
  // a UTF-8 locale once cut "t\xC3\xB4t" to the invalid "t\xC3t".
  {
    const std::string saved = std::setlocale(LC_CTYPE, nullptr);
    for (const char* locale : {"C", "en_US.UTF-8"}) {
      if (!std::setlocale(LC_CTYPE, locale)) continue;
      for (bool preserve : {false, true}) {
        std::vector<novox::TranscribedNote> sung(5);
        for (size_t i = 0; i < 5; ++i) sung[i].note.start = 0.4 * static_cast<double>(i), sung[i].note.dur = 0.38, sung[i].note.pitch = 60;
        const std::vector<novox::Word> accented = {{"t\xC3\xB4t", 0, 0.38, 1}, {"coraz\xC3\xB3n", 0.4, 1.58, 1},
                                                   {"\xC3\xBC" "ber\xF0\x9F\x8E\xB5", 1.6, 2.0, 1}};
        const auto warned = novox::alignLyrics(sung, accented, lex, preserve);
        std::string letters;
        for (const auto& n : sung) {
          CHECK(novox::validUtf8(n.note.lyric) == n.note.lyric);
          if (n.note.lyric != "_" && !n.note.lyric.empty())
            letters += n.note.lyric.back() == '-' ? n.note.lyric.substr(0, n.note.lyric.size() - 1) : n.note.lyric;
        }
        CHECK(letters == "t\xC3\xB4tcoraz\xC3\xB3n\xC3\xBC" "ber");
        for (const auto& w : warned) CHECK(novox::validUtf8(w) == w);
      }
    }
    std::setlocale(LC_CTYPE, saved.c_str());
    // Results echo the caller's words: invalid UTF-8 there comes back repaired.
    const std::vector<novox::Word> broken = {{"caf\xC3", 0, 1, 1}};
    const std::vector<size_t> unassigned = {0};
    const std::string json = novox::transcriptionToJson({}, {"'caf\xC3' left out"}, "singing-v1", &broken, &unassigned);
    CHECK(novox::validUtf8(json) == json && json.find("caf\xEF\xBF\xBD") != std::string::npos);
  }
}

void testSyllableAlignment() {
  // ASR puts the word endpoint just before its last musical event.
  std::vector<novox::TranscribedNote> notes(4);
  const double starts[] = {0,.3,.6,1};
  for(size_t i=0;i<4;++i) {notes[i].note.start=starts[i];notes[i].note.dur=.25;notes[i].note.pitch=60;}
  std::vector<size_t> unresolved;
  novox::alignLyrics(notes,{{"remember",0,.596,1},{"me",.596,1.3,1}},embeddedLexicon(),true,&unresolved);
  CHECK(notes.size()==4 && unresolved.empty());
  CHECK(notes[0].note.lyric=="re-" && notes[1].note.lyric=="mem-" && notes[2].note.lyric=="ber" && notes[3].note.lyric=="me");
  CHECK_NEAR(notes[2].note.start,.6,1e-9);
  // Count alone must never pull a word across a distant rest.
  novox::alignLyrics(notes,{{"remember",0,.9,1},{"missing",4,5,1}},embeddedLexicon(),true,&unresolved);
  CHECK(notes.size()==4 && !unresolved.empty());
  CHECK(notes[3].note.lyric!="missing");
  // Lexical demand supports a shallow acoustic boundary, but count alone
  // cannot divide a flat held vowel. Times/pitches remain audio-derived.
  std::vector<novox::TranscribedNote> held(1);
  held[0].note.start=0; held[0].note.dur=1; held[0].note.pitch=60;
  std::vector<novox::PitchFrame> frames(126);
  for(size_t i=0;i<frames.size();++i) {
    frames[i].time=i*.008;frames[i].f0=261.6f;frames[i].midi=60;frames[i].db=-20;
  }
  auto flat=held;
  novox::alignLyrics(flat,{{"hello",0,1,1}},embeddedLexicon(),true,&unresolved,&frames);
  CHECK(flat.size()==1);
  frames[65].db=-21.5f;
  novox::alignLyrics(held,{{"hello",0,1,1}},embeddedLexicon(),true,&unresolved,&frames);
  CHECK(held.size()==2 && unresolved.empty());
  CHECK(held[0].note.lyric=="hel-" && held[1].note.lyric=="lo");
  CHECK_NEAR(held[1].note.start,.52,1e-9);
  CHECK_NEAR(held[0].note.dur+held[1].note.dur,1,1e-9);
  CHECK(held[0].note.pitch==60 && held[1].note.pitch==60);
  frames[65].f0=0;
  flat.resize(1);flat[0].note.start=0;flat[0].note.dur=1;
  novox::alignLyrics(flat,{{"hello",0,1,1}},embeddedLexicon(),true,&unresolved,&frames);
  CHECK(flat.size()==1); // No articulation split on an unvoiced frame.
  // A word timestamp can include the preceding vowel and a long rest.
  // Keep a short adjoining tail with its word rather than shifting all lyrics.
  std::vector<novox::TranscribedNote> phrase(5);
  const double phraseStarts[]={.664,.928,1.992,2.2,2.4};
  const double phraseDurations[]={.264,.104,.208,.2,.6};
  for(size_t i=0;i<phrase.size();++i) {
    phrase[i].note.start=phraseStarts[i];phrase[i].note.dur=phraseDurations[i];phrase[i].note.pitch=60;
  }
  novox::alignLyrics(phrase,{{"shot",0,.9,1},{"I",.9,2.12,1},{"don't",2.12,2.4,1},{"know",2.4,2.74,1}},embeddedLexicon(),true,&unresolved);
  CHECK(phrase[0].note.lyric=="shot" && phrase[1].note.lyric=="_");
  CHECK(phrase[2].note.lyric=="I" && phrase[3].note.lyric=="don't" && phrase[4].note.lyric=="know");


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
      {"manual lyric token API", testLyricTokenApi},
      {"plan invariants", testPlanInvariants},
      {"short gaps sing through", testShortGapsSingThrough},
      {"determinism and streaming", testDeterminismAndStreaming},
      {"sparse rendering", testSparseRendering},
      {"MIDI import", testMidiImport},
      {"MIDI import in beats", testMidiImportBeats},
      {"MusicXML import", testMusicXmlImport},
      {"transcribe melody", testTranscribeMelody},
      {"transcribe held notes", testTranscribeHeldNotes},
      {"singing-v1", testSingingV1},
      {"align lyrics", testAlignLyrics},
      {"syllable alignment", testSyllableAlignment},
      {"C API", testCApi},
      {"singing-v2 pitch", [] { testSingingPitchV2(checks, failures); }},
      {"singing-v2 syllables", [] { testSyllablesV2(checks, failures); }},
      {"singing-v2 segmentation", [] { testSingingSegmentV2(checks, failures); }},
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
