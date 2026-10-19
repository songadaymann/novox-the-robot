// singing-v2 tests for engine/src/singing_segment.* and articulation.*
//
// The most realistic synthetic singer at hand is Novox itself: each test
// renders a score with known notes and lyrics, gives the segmenter exact word
// timings (ASR word ends at the end of each word's last note), and checks
// that the notes come back one per sung syllable.
#include "v2_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "articulation.hpp"
#include "json.hpp"
#include "lexicon.hpp"
#include "novox.h"
#include "planner.hpp"
#include "score.hpp"
#include "singing_segment.hpp"
#include "spectrum.hpp"
#include "synth.hpp"
#include "transcribe.hpp"
#include "utf8.hpp"

namespace {

constexpr double kPiTest = 3.14159265358979323846;  // kPiTest is not standard C++ (MSVC lacks it)

const novox::Lexicon& lexicon() {
  static novox::Lexicon lex = [] {
    novox::Lexicon l;
    l.loadEmbedded();
    return l;
  }();
  return lex;
}

struct Take {
  std::vector<float> x16k;
  double lead = 0;  // seconds of pre-roll before score time 0
};

// Renders a score (JSON) the way a singer's take would arrive: 44.1 kHz, then
// the analysis resampling.
Take render(const std::string& scoreJson) {
  const novox::Score score = novox::parseScore(novox::json::parse(scoreJson));
  novox::Lexicon lex = lexicon();
  for (const auto& [w, p] : score.dictionary) lex.addWord(w, p);
  const novox::Plan plan = novox::planScore(score, lex);
  novox::RenderOptions o;
  o.sampleRate = 44100;
  o.normalize = true;
  const std::vector<float> x = novox::renderPlan(plan, score.voice, o);
  Take t;
  t.x16k = novox::resampleTo16k(x.data(), x.size(), o.sampleRate);
  t.lead = plan.lead;
  return t;
}

// A word whose recognised end is `end` seconds of score time.
novox::Word word(const std::string& text, double end, const Take& t) {
  novox::Word w;
  w.text = text;
  w.end = end + t.lead;
  w.start = 0;  // ASR word starts are the previous word's end: never used
  w.probability = 1;
  return w;
}

novox::SegmentationResult segment(const Take& t, std::vector<novox::Word> words, const std::string& detail = "natural") {
  // Word starts tile the recording, as the plugin passes them.
  for (size_t i = 1; i < words.size(); ++i) words[i].start = words[i - 1].end;
  novox::MelodyOptions opts;
  opts.algorithm = "singing-v2";
  opts.detail = detail;
  const auto pitch = novox::analyzeSingingPitch(t.x16k);
  return novox::segmentSinging(t.x16k, pitch, words, lexicon(), opts);
}

std::string lyricsOf(const novox::SegmentationResult& r) {
  std::string s;
  for (const auto& n : r.notes) s += (s.empty() ? "" : " ") + (n.note.lyric.empty() ? std::string("~") : n.note.lyric);
  return s;
}

// The voice: plain, no vibrato or scoop unless a test asks for it.
std::string voice(const std::string& extra = "") {
  return R"("voice": {"preset": "std", "vibratoDepth": 0, "scoop": 0, "jitterPitch": 0)" + extra + "}";
}

// Shapes Novox does not sing (an overshoot, a nasal murmur at its own pitch,
// period doubling), rendered directly at 16 kHz from 0.1 s to `end`: harmonics
// of `midi(t)` up to 3.5 kHz with 1/k amplitudes, those above 450 Hz scaled by
// `upper(t)` (a nasal keeps its lowest partials and loses the vowel band), all
// scaled by `level(t)` (0 in a rest) and by 1 + doubling(t) cos(pi phase)
// (alternate glottal cycles louder and softer: a partial at f/2 about 7 dB under
// f at 0.9), 20 ms fades, 0.3 s of silence after.
Take additive(const std::function<double(double)>& midi, const std::function<double(double)>& upper, double end,
              const std::function<double(double)>& level = [](double) { return 1.0; },
              const std::function<double(double)>& doubling = [](double) { return 0.0; },
              int maxHarmonics = 0) {
  Take t;
  t.x16k.assign(static_cast<size_t>((end + 0.3) * novox::kAnalysisRate), 0.0f);
  double phase = 0;
  for (size_t i = 0; i < t.x16k.size(); ++i) {
    const double time = i / novox::kAnalysisRate;
    if (time < 0.1 || time >= end) continue;
    const double f0 = 440.0 * std::pow(2.0, (midi(time) - 69) / 12);
    phase += f0 / novox::kAnalysisRate;
    const double fade = std::min({1.0, (time - 0.1) / 0.02, (end - time) / 0.02});
    double v = 0;
    for (int k = 1; k * f0 < 3500 && (maxHarmonics == 0 || k <= maxHarmonics); ++k)
      v += (k * f0 < 450 ? 1 : upper(time)) * std::sin(2 * kPiTest * k * phase) / k;
    t.x16k[i] = static_cast<float>(0.2 * fade * level(time) * (1 + doubling(time) * std::cos(kPiTest * phase)) * v);
  }
  return t;
}

// Adds an unvoiced fricative (/s/: noise at 3.5-7.5 kHz) to the take over
// [from, to) seconds, so the vowel after it has a clear syllable onset.
void addFricative(Take& t, double from, double to, double amplitude = 0.05) {
  uint32_t seed = 777;
  auto uniform = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
  std::vector<double> frequency, phase;
  for (int i = 0; i < 60; ++i) frequency.push_back(3500 + 4000 * uniform()), phase.push_back(2 * kPiTest * uniform());
  for (size_t i = static_cast<size_t>(from * novox::kAnalysisRate); i < t.x16k.size() && i < to * novox::kAnalysisRate; ++i) {
    const double time = i / novox::kAnalysisRate;
    double v = 0;
    for (size_t j = 0; j < frequency.size(); ++j) v += std::sin(2 * kPiTest * frequency[j] * time + phase[j]);
    t.x16k[i] += static_cast<float>(amplitude * std::min({1.0, (time - from) / 0.01, (to - time) / 0.01}) * v / 6);
  }
}

double smooth(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return x * x * (3 - 2 * x);
}

bool sameNotes(const novox::SegmentationResult& a, const novox::SegmentationResult& b, double onset) {
  if (a.notes.size() != b.notes.size()) return false;
  for (size_t i = 0; i < a.notes.size(); ++i)
    if (std::fabs(a.notes[i].note.start - b.notes[i].note.start) > onset || a.notes[i].note.pitch != b.notes[i].note.pitch ||
        a.notes[i].note.lyric != b.notes[i].note.lyric)
      return false;
  return true;
}

// A rough syllable (see tests/singing_pitch_tests.cpp): partials of `hz` below
// 1.1 kHz plus aperiodic energy at 1.7-2.6 kHz, so the pitch path leaves it unvoiced.
std::vector<float> roughSyllable(double hz, double seconds, double amplitude) {
  std::vector<float> x(static_cast<size_t>(seconds * 16000));
  uint32_t seed = 4242;
  auto uniform = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
  std::vector<double> frequency, phase;
  for (int i = 0; i < 80; ++i) frequency.push_back(1700 + 900 * uniform()), phase.push_back(2 * kPiTest * uniform());
  const double harmonics[5] = {1.0, 0.7, 0.5, 0.35, 0.25};
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = i / 16000.0;
    double sum = 0;
    for (int k = 0; k < 5; ++k) sum += harmonics[k] * std::sin(2 * kPiTest * hz * (k + 1) * t);
    for (int j = 0; j < 80; ++j) sum += 0.12 * std::sin(2 * kPiTest * frequency[static_cast<size_t>(j)] * t + phase[static_cast<size_t>(j)]);
    x[i] = static_cast<float>(amplitude * std::min({1.0, t / 0.01, (seconds - t) / 0.01}) * sum);
  }
  return x;
}

}  // namespace

void testSingingSegmentV2(int& checks, int& failures) {
  // Repeated same-pitch syllables: every syllable onset is a note boundary.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "la la la", "notes": [
      {"pitch": 57, "start": 0, "dur": 0.45}, {"pitch": 57, "start": 0.45, "dur": 0.45},
      {"pitch": 57, "start": 0.9, "dur": 0.6}]})");
    const auto r = segment(t, {word("la", 0.45, t), word("la", 0.9, t), word("la", 1.5, t)});
    V2_CHECK(r.notes.size() == 3);
    V2_CHECK(lyricsOf(r) == "la la la");
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[1].note.start, 0.45 + t.lead, 0.08);
      V2_CHECK_NEAR(r.notes[2].note.start, 0.9 + t.lead, 0.08);
      for (const auto& n : r.notes) V2_CHECK_NEAR(n.note.pitch, 57, 0.01f);
    }
    V2_CHECK(r.unassignedWords.empty());
  }
  // A three-syllable word sung legato across liquid and nasal onsets.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "lem-o-nade", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.4}, {"pitch": 57, "start": 0.4, "dur": 0.35},
      {"pitch": 59, "start": 0.75, "dur": 0.6}]})");
    const auto r = segment(t, {word("lemonade", 1.35, t)});
    V2_CHECK(r.notes.size() == 3);
    if (r.notes.size() == 3) {
      V2_CHECK(r.notes[0].note.lyric.back() == '-' && r.notes[1].note.lyric.back() == '-');
      V2_CHECK(r.notes[2].note.lyric.back() != '-' && r.notes[2].note.lyric != "_");
      std::string joined;
      for (const auto& n : r.notes) joined += n.note.lyric.substr(0, n.note.lyric.find('-'));
      V2_CHECK(joined == "lemonade");
      V2_CHECK_NEAR(r.notes[1].note.start, 0.4 + t.lead, 0.1);
      V2_CHECK_NEAR(r.notes[2].note.start, 0.75 + t.lead, 0.1);
    }
  }
  // A brief, deep level dip already separates two held vowels at the same
  // pitch. Preserving attacks must not duplicate this existing boundary or
  // assign the transcript's one word twice (the earlier vowel may be a hum).
  {
    const Take t = additive([](double) { return 60.0; }, [](double) { return 1.0; }, 1.65,
                            [](double s) { return s >= 0.76 && s < 0.84 ? 0.02 : 1.0; });
    for (const char* detail : {"natural", "detailed"}) {
      const auto r = segment(t, {word("oh", 1.65, t)}, detail);
      V2_CHECK(r.notes.size() == 2);
      V2_CHECK(std::count_if(r.notes.begin(), r.notes.end(), [](const auto& n) { return n.note.lyric == "oh"; }) == 1);
      if (r.notes.size() == 2) {
        V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
        V2_CHECK_NEAR(r.notes[1].note.pitch, 60, 0.01f);
        V2_CHECK_NEAR(r.notes[0].note.start, 0.1, 0.06);
        V2_CHECK_NEAR(r.notes[1].note.start, 0.84, 0.06);
        V2_CHECK(r.notes[0].note.dur > 0.5 && r.notes[1].note.dur > 0.5);
      }
      V2_CHECK(r.unassignedWords.empty());
    }
  }
  // A smooth breath-sized amplitude dip can be one acoustic re-attack even
  // when the lyric aligner keeps both held vowels in the same syllable. The
  // second note sustains the word; it does not need a different pitch.
  {
    const Take t = additive([](double) { return 60.0; }, [](double) { return 1.0; }, 1.65, [](double s) {
      const double distance = (s - 0.8) / 0.03;
      return 1.0 - 0.85 * std::exp(-distance * distance);
    });
    for (const char* detail : {"natural", "detailed"}) {
      const auto r = segment(t, {word("oh", 1.65, t)}, detail);
      V2_CHECK(r.notes.size() == 2);
      V2_CHECK(lyricsOf(r) == "oh _");
      if (r.notes.size() == 2) {
        V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
        V2_CHECK_NEAR(r.notes[1].note.pitch, 60, 0.01f);
        V2_CHECK_NEAR(r.notes[1].note.start, 0.83, 0.06);
        V2_CHECK(r.notes[0].note.dur > 0.5 && r.notes[1].note.dur > 0.5);
      }
      V2_CHECK(r.unassignedWords.empty());
    }
  }
  // A held vowel with wide (+-1 st, 5.5 Hz) vibrato stays one note.
  {
    const Take t = render(R"({"units": "seconds", )" + voice(R"(, "vibratoDepth": 1.0, "vibratoRate": 5.5, "vibratoRamp": 0.1)") +
                          R"(, "lyrics": "ah", "notes": [{"pitch": 57, "start": 0, "dur": 1.6}]})");
    const auto r = segment(t, {word("ah", 1.6, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
  }
  // ... also without lyrics, and with a three-syllable word on one pitch.
  {
    const Take t = render(R"({"units": "seconds", )" + voice(R"(, "vibratoDepth": 1.0, "vibratoRate": 5.5, "vibratoRamp": 0.1)") +
                          R"(, "lyrics": "ah", "notes": [{"pitch": 57, "start": 0, "dur": 1.6}]})");
    const auto r = segment(t, {});
    V2_CHECK(r.notes.size() == 1);
  }
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "lem-o-nade", "notes": [
      {"pitch": 57, "start": 0, "dur": 0.4}, {"pitch": 57, "start": 0.4, "dur": 0.35},
      {"pitch": 57, "start": 0.75, "dur": 0.6}]})");
    const auto r = segment(t, {word("lemonade", 1.35, t)});
    V2_CHECK(r.notes.size() == 3);
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[1].note.start, 0.4 + t.lead, 0.1);
      V2_CHECK_NEAR(r.notes[2].note.start, 0.75 + t.lead, 0.1);
    }
  }
  // A take with no pause at all (the background level cannot be measured).
  {
    std::vector<float> x(32000);
    double phase = 0;
    for (size_t i = 0; i < x.size(); ++i) {
      phase += 2 * kPiTest * 220 * std::pow(2.0, 0.8 * std::sin(2 * kPiTest * 5.5 * i / 16000.0) / 12) / 16000.0;
      double v = 0;
      for (int k = 1; k < 8; ++k) v += 0.3 / k * std::sin(k * phase);
      x[i] = static_cast<float>(v);
    }
    novox::MelodyOptions opts;
    opts.algorithm = "singing-v2";
    const auto r = novox::segmentSinging(x, novox::analyzeSingingPitch(x), {}, lexicon(), opts);
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
  }
  // A 3-semitone scoop into the note stays one note at the target pitch.
  {
    const Take t = render(R"({"units": "seconds", )" + voice(R"(, "scoop": -3, "scoopTime": 0.12)") +
                          R"(, "lyrics": "no", "notes": [{"pitch": 60, "start": 0, "dur": 0.9}]})");
    const auto r = segment(t, {word("no", 0.9, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
  }
  // A melisma: two settled pitches on one syllable -> two notes, "_" second.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 57, "start": 0, "dur": 0.4, "lyric": "oh"}, {"pitch": 60, "start": 0.4, "dur": 0.5, "lyric": "_"}]})");
    const auto r = segment(t, {word("oh", 0.9, t)});
    V2_CHECK(r.notes.size() == 2);
    if (r.notes.size() == 2) {
      V2_CHECK(r.notes[0].note.lyric == "oh" && r.notes[1].note.lyric == "_");
      V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 60, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.start, 0.4 + t.lead, 0.1);
    }
  }
  // A turn (57-59-57, every note held): three notes. The middle note is not a
  // glide between its neighbours just because they share a pitch.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 57, "start": 0, "dur": 0.35, "lyric": "ah"}, {"pitch": 59, "start": 0.35, "dur": 0.35, "lyric": "_"},
      {"pitch": 57, "start": 0.7, "dur": 0.45, "lyric": "_"}]})");
    const auto r = segment(t, {word("ah", 1.15, t)});
    V2_CHECK(lyricsOf(r) == "ah _ _");
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 59, 0.01f);
      V2_CHECK_NEAR(r.notes[2].note.pitch, 57, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.start, 0.35 + t.lead, 0.1);
      V2_CHECK_NEAR(r.notes[2].note.start, 0.7 + t.lead, 0.1);
    }
  }
  // A melisma whose first note is scooped into, quickly or slowly (the scoop
  // takes most of the note): still two notes, the first at the pitch it
  // settles on. A slow scoop into a single note stays one note.
  for (const char* scoop : {R"(, "scoop": -2, "scoopTime": 0.15)", R"(, "scoop": -2.5, "scoopTime": 0.3, "portamento": 0.05)"}) {
    const Take t = render(R"({"units": "seconds", )" + voice(scoop) + R"(, "notes": [
      {"pitch": 57, "start": 0, "dur": 0.45, "lyric": "oh"}, {"pitch": 58.6, "start": 0.45, "dur": 0.45, "lyric": "_"}]})");
    const auto r = segment(t, {word("oh", 0.9, t)});
    V2_CHECK(lyricsOf(r) == "oh _");
    if (r.notes.size() == 2) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 59, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.start, 0.45 + t.lead, 0.1);
    }
  }
  {
    const Take t = render(R"({"units": "seconds", )" + voice(R"(, "scoop": -3, "scoopTime": 0.3)") +
                          R"(, "lyrics": "no", "notes": [{"pitch": 60, "start": 0, "dur": 0.9}]})");
    const auto r = segment(t, {word("no", 0.9, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
  }
  // Melodic steps inside syllables (Melodyne-like notes): every settled pitch
  // is a note. A run of six notes on one vowel (200 ms each, whole and half
  // steps, as a melisma is sung) gives six notes ...
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 60, "start": 0, "dur": 0.2, "lyric": "ah"}, {"pitch": 62, "start": 0.2, "dur": 0.2, "lyric": "_"},
      {"pitch": 64, "start": 0.4, "dur": 0.2, "lyric": "_"}, {"pitch": 65, "start": 0.6, "dur": 0.2, "lyric": "_"},
      {"pitch": 67, "start": 0.8, "dur": 0.2, "lyric": "_"}, {"pitch": 64, "start": 1.0, "dur": 0.4, "lyric": "_"}]})");
    const auto r = segment(t, {word("ah", 1.4, t)});
    V2_CHECK(lyricsOf(r) == "ah _ _ _ _ _");
    const float pitches[6] = {60, 62, 64, 65, 67, 64};
    if (r.notes.size() == 6)
      for (size_t k = 0; k < 6; ++k) {
        V2_CHECK_NEAR(r.notes[k].note.pitch, pitches[k], 0.01f);
        if (k > 0) V2_CHECK_NEAR(r.notes[k].note.start, 0.2 * static_cast<double>(k) + t.lead, 0.1);
      }
  }
  // ... and two syllables each slurred over two notes give four ("la _ la _").
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 57, "start": 0, "dur": 0.3, "lyric": "la"}, {"pitch": 59, "start": 0.3, "dur": 0.3, "lyric": "_"},
      {"pitch": 60, "start": 0.6, "dur": 0.3, "lyric": "la"}, {"pitch": 57, "start": 0.9, "dur": 0.45, "lyric": "_"}]})");
    const auto r = segment(t, {word("la", 0.6, t), word("la", 1.35, t)});
    V2_CHECK(lyricsOf(r) == "la _ la _");
    const float pitches[4] = {57, 59, 60, 57};
    if (r.notes.size() == 4)
      for (size_t k = 0; k < 4; ++k) {
        V2_CHECK_NEAR(r.notes[k].note.pitch, pitches[k], 0.01f);
        if (k > 0) V2_CHECK_NEAR(r.notes[k].note.start, 0.3 * static_cast<double>(k) + t.lead, 0.1);
      }
  }
  // A held note with +-1 st, 5.5 Hz vibrato after another syllable stays one
  // note, in detailed mode too: its swings cancel on the vibrato's centre line,
  // so they are no steps, and its start (the leap still rising into the first
  // swing, where the centre line sits flat ~1.3 st low for ~100 ms) never holds
  // on the contour, so it is no approach note.
  {
    const Take t = render(R"({"units": "seconds", )" + voice(R"(, "vibratoDepth": 1.0, "vibratoRate": 5.5, "vibratoRamp": 0.05)") +
                          R"(, "lyrics": "la la", "notes": [{"pitch": 55, "start": 0, "dur": 0.4}, {"pitch": 59, "start": 0.4, "dur": 1.4}]})");
    for (const char* detail : {"natural", "detailed"}) {
      const auto r = segment(t, {word("la", 0.4, t), word("la", 1.8, t)}, detail);
      V2_CHECK(lyricsOf(r) == "la la");
      if (r.notes.size() == 2) V2_CHECK_NEAR(r.notes[1].note.pitch, 59, 0.01f);
    }
  }
  // A melisma sung with vibrato: a semitone step under +-0.5 or +-1 st
  // vibrato is still a step (plateaus are measured on the vibrato's centre
  // line, where its swings cancel).
  for (const char* depth : {"0.5", "1.0"}) {
    const Take t = render(R"({"units": "seconds", )" + voice(std::string(R"(, "vibratoRate": 5.5, "vibratoRamp": 0.05, "vibratoDepth": )") + depth) + R"(, "notes": [
      {"pitch": 59, "start": 0, "dur": 0.7, "lyric": "ah"}, {"pitch": 60, "start": 0.7, "dur": 0.7, "lyric": "_"}]})");
    const auto r = segment(t, {word("ah", 1.4, t)});
    V2_CHECK(lyricsOf(r) == "ah _");
    if (r.notes.size() == 2) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 59, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 60, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.start, 0.7 + t.lead, 0.1);
    }
  }
  // A 150 ms scoop (a steady 3-semitone rise, no plateau) into a held note is
  // one note at the held pitch.
  {
    const Take t = additive([](double s) { return s < 0.25 ? 57 + 3 * (s - 0.1) / 0.15 : 60.0; }, [](double) { return 1.0; }, 0.9);
    const auto r = segment(t, {word("oh", 0.9, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
  }
  // "la la" with a step inside the second word: the step is a melisma note,
  // also when the recogniser ends the first word late, inside the second
  // word's first note (a word end bounds where the next syllable may start;
  // it never moves a pitch plateau into the neighbouring note).
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 62, "start": 0, "dur": 0.4, "lyric": "la"}, {"pitch": 60, "start": 0.4, "dur": 0.35, "lyric": "la"},
      {"pitch": 57, "start": 0.75, "dur": 0.5, "lyric": "_"}]})");
    for (double firstEnd : {0.4, 0.55}) {
      const auto r = segment(t, {word("la", firstEnd, t), word("la", 1.25, t)});
      V2_CHECK(lyricsOf(r) == "la la _");
      const float pitches[3] = {62, 60, 57};
      const double starts[3] = {0, 0.4, 0.75};
      if (r.notes.size() == 3)
        for (size_t k = 0; k < 3; ++k) {
          V2_CHECK_NEAR(r.notes[k].note.pitch, pitches[k], 0.01f);
          if (k > 0) V2_CHECK_NEAR(r.notes[k].note.start, starts[k] + t.lead, 0.1);
        }
    }
  }
  // A short fall at the end of a held note is a fall-off, not a second note.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 60, "start": 0, "dur": 0.6, "lyric": "oh"}, {"pitch": 57, "start": 0.6, "dur": 0.12, "lyric": "_"}]})");
    const auto r = segment(t, {word("oh", 0.72, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
  }
  // A quiet low sound after the voice stops (an instrument under a released
  // note: 100 ms unvoiced, then 200 ms of 50 at -12 dB with a weak vowel band)
  // is no fall-off: the note ends at the gap and keeps its pitch.
  {
    const Take t = additive([](double s) { return s < 0.7 ? 57.0 : 50.0; }, [](double s) { return s < 0.7 ? 1.0 : 0.3; }, 1.0,
                            [](double s) { return s < 0.7 ? 1.0 : s < 0.8 ? 0.0 : 0.25; });
    const auto r = segment(t, {word("oh", 0.7, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
      V2_CHECK(r.notes[0].note.start + r.notes[0].note.dur < 0.75);
    }
  }
  // A scoop that overshoots (57 -> 60.8 -> 60 within 200 ms) is one note at 60.
  {
    const Take t = additive([](double s) { return s < 0.18 ? 57 + 3.8 * smooth((s - 0.1) / 0.08) : 60.8 - 0.8 * smooth((s - 0.18) / 0.12); },
                            [](double) { return 1.0; }, 0.9);
    const auto r = segment(t, {word("oh", 0.9, t)});
    V2_CHECK(r.notes.size() == 1);
    if (r.notes.size() == 1) V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
  }
  // Shapes inside one syllable, sung after a phrase (so the take's spectral
  // variety is that of singing) and an /s/: the notes of the syllable "oh"
  // (its lyric and pitch per note), sung from 0.1 s to `end` at `level`.
  auto syllableNotes = [](const std::function<double(double)>& midi, double end = 0.9,
                          const std::function<double(double)>& level = [](double) { return 1.0; },
                          const std::string& detail = "natural") {
    const Take sung = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "say to me", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.5}, {"pitch": 59, "start": 0.5, "dur": 0.5}, {"pitch": 62, "start": 1.0, "dur": 0.7}]})");
    Take shape = additive(midi, [](double) { return 1.0; }, end, level);
    addFricative(shape, 0.03, 0.1);
    Take t = sung;
    const double at = static_cast<double>(t.x16k.size()) / novox::kAnalysisRate;
    t.x16k.insert(t.x16k.end(), shape.x16k.begin(), shape.x16k.end());
    novox::Word oh;
    oh.text = "oh";
    oh.end = at + end;
    const auto r = segment(t, {word("say", 0.5, sung), word("to", 1.0, sung), word("me", 1.7, sung), oh}, detail);
    std::vector<std::pair<std::string, float>> out;
    for (const auto& n : r.notes)
      if (n.note.start >= at) out.push_back({n.note.lyric, n.note.pitch});
    return out;
  };
  // A held approach a whole tone below the note (the trained CSD singer's
  // ~180 ms reach for it) is the singer finding the note: one note at the target ...
  {
    const auto notes = syllableNotes([](double s) { return s < 0.24 ? 57.0 : 57.0 + 2.0 * smooth((s - 0.24) / 0.07); });
    V2_CHECK(notes.size() == 1);
    if (notes.size() == 1) V2_CHECK_NEAR(notes[0].second, 59, 0.01f);
  }
  {  // ... while a 140 ms note a major third under the next, as Novox sings it, is
     // a note of its own (the first of a melisma).
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 57, "start": 0, "dur": 0.14, "lyric": "oh"}, {"pitch": 61, "start": 0.14, "dur": 0.6, "lyric": "_"}]})");
    const auto r = segment(t, {word("oh", 0.74, t)});
    V2_CHECK(lyricsOf(r) == "oh _");
    if (r.notes.size() == 2) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 61, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.start, 0.14 + t.lead, 0.08);
    }
  }
  // ... but a flat start too brief to be a plateau (40 ms at 57, then a rise
  // to 60), a held start within a semitone of the note (140 ms at 59, then 60)
  // and a dip before the rise (64 -> 63.5 -> 64 over 100 ms, then 65.2)
  // approach a single note.
  {
    const std::function<double(double)> approaches[3] = {
        [](double s) { return s < 0.14 ? 57.0 : 57 + 3 * smooth((s - 0.14) / 0.12); },
        [](double s) { return s < 0.24 ? 59.0 : 59 + 1.0 * smooth((s - 0.24) / 0.07); },
        [](double s) { return s < 0.2 ? 64 - 0.5 * std::sin(kPiTest * (s - 0.1) / 0.1) : 64 + 1.2 * smooth((s - 0.2) / 0.1); }};
    const float targets[3] = {60, 60, 65};
    for (int k = 0; k < 3; ++k) {
      const auto notes = syllableNotes(approaches[k]);
      V2_CHECK(notes.size() == 1);
      if (notes.size() == 1) {
        V2_CHECK(notes[0].first == "oh");
        V2_CHECK_NEAR(notes[0].second, targets[k], 0.01f);
      }
    }
  }
  // A short first note that holds its pitch above the next one ("so oh": the
  // "oh" after the /s/ holds 62 for 150 ms, then 60) is a note: a scoop comes
  // from below and never holds.
  {
    Take t = additive([](double s) { return s < 0.5 ? 57.0 : s < 0.72 ? 62.0 : 62 - 2 * smooth((s - 0.72) / 0.06); }, [](double) { return 1.0; }, 1.3,
                      [](double s) { return s >= 0.5 && s < 0.57 ? 0.0 : 1.0; });
    addFricative(t, 0.5, 0.57);
    const auto r = segment(t, {word("so", 0.5, t), word("oh", 1.3, t)});
    V2_CHECK(lyricsOf(r) == "so oh _");
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[1].note.pitch, 62, 0.01f);
      V2_CHECK_NEAR(r.notes[2].note.pitch, 60, 0.01f);
      V2_CHECK_NEAR(r.notes[2].note.start, 0.75, 0.06);
    }
  }
  // A short last note that rises and holds (150 ms at 62 after 60) is a note;
  // a droop that settles is still a release (see the 120 ms fall above).
  {
    const auto notes = syllableNotes([](double s) { return s < 0.65 ? 60.0 : 60 + 2 * smooth((s - 0.65) / 0.06); }, 0.85);
    V2_CHECK(notes.size() == 2);
    if (notes.size() == 2) {
      V2_CHECK_NEAR(notes[0].second, 60, 0.01f);
      V2_CHECK_NEAR(notes[1].second, 62, 0.01f);
    }
  }
  // A second plateau sung softly (a decrescendo: 280 ms at 65, then 420 ms at
  // 63 held 14 dB down, "la" in a real take) is a note ...
  {
    const auto notes = syllableNotes([](double s) { return s < 0.38 ? 65.0 : 65 - 2 * smooth((s - 0.38) / 0.06); }, 0.8,
                                     [](double s) { return s < 0.38 ? 1.0 : 0.2; });
    V2_CHECK(notes.size() == 2);
    if (notes.size() == 2) {
      V2_CHECK(notes[0].first == "oh" && notes[1].first == "_");
      V2_CHECK_NEAR(notes[0].second, 65, 0.01f);
      V2_CHECK_NEAR(notes[1].second, 63, 0.01f);
    }
  }
  // ... but a release that fades as it drops (500 ms at 60, then 57 fading
  // from -12 to -30 dB) is the note's end.
  {
    const auto notes = syllableNotes([](double s) { return s < 0.6 ? 60.0 : 60 - 3 * smooth((s - 0.6) / 0.06); }, 0.9,
                                     [](double s) { return s < 0.6 ? 1.0 : 0.25 * std::pow(0.125, (s - 0.6) / 0.3); });
    V2_CHECK(notes.size() == 1);
    if (notes.size() == 1) V2_CHECK_NEAR(notes[0].second, 60, 0.01f);
  }
  // Detailed mode (MelodyOptions::detail): what natural mode keeps inside one
  // note becomes notes of its own once it holds a pitch: a held approach from a
  // whole tone or a semitone below, a grace note held ~70 ms (from above at the
  // start, or a neighbour inside the note), a droop that settles at the end
  // (also sung quietly, 14 dB down). Natural mode sings each as one note.
  {
    auto one = [](double) { return 1.0; };
    auto droop = [](double s) { return s < 0.74 ? 60.0 : 60.0 - 2.0 * smooth((s - 0.74) / 0.05); };
    struct Shape {
      std::function<double(double)> midi, level;
      std::vector<float> detailed;  // pitches in detailed mode; natural: the held pitch alone
      float natural;
    };
    const Shape shapes[] = {
        {[](double s) { return s < 0.24 ? 57.0 : 57.0 + 2.0 * smooth((s - 0.24) / 0.07); }, one, {57, 59}, 59},
        {[](double s) { return s < 0.24 ? 58.0 : 58.0 + 1.0 * smooth((s - 0.24) / 0.05); }, one, {58, 59}, 59},
        {[](double s) { return s < 0.17 ? 62.0 : 62.0 - 2.0 * smooth((s - 0.17) / 0.02); }, one, {62, 60}, 60},
        {[](double s) { return 60 + 2 * (smooth((s - 0.4) / 0.015) - smooth((s - 0.47) / 0.015)); }, one, {60, 62, 60}, 60},
        {droop, one, {60, 58}, 60},
        {droop, [](double s) { return s < 0.74 ? 1.0 : 0.2; }, {60, 58}, 60},
    };
    for (const Shape& shape : shapes) {
      const auto natural = syllableNotes(shape.midi, 0.9, shape.level, "natural");
      V2_CHECK(natural.size() == 1);
      if (natural.size() == 1) V2_CHECK_NEAR(natural[0].second, shape.natural, 0.01f);
      const auto detailed = syllableNotes(shape.midi, 0.9, shape.level, "detailed");
      V2_CHECK(detailed.size() == shape.detailed.size());
      if (detailed.size() == shape.detailed.size())
        for (size_t k = 0; k < detailed.size(); ++k) {
          V2_CHECK(detailed[k].first == (k == 0 ? "oh" : "_"));
          V2_CHECK_NEAR(detailed[k].second, shape.detailed[k], 0.01f);
        }
    }
    // ... while vibrato (+-1 st at 5.5 Hz, +-0.7 st at 6 Hz), tremolo (a 6 Hz
    // level swing) and a scoop without a plateau (a steady 3 st rise over
    // 150 ms) stay one note in both modes.
    const std::pair<std::function<double(double)>, std::function<double(double)>> single[] = {
        {[](double s) { return 60 + 1.0 * std::sin(2 * kPiTest * 5.5 * (s - 0.1)); }, one},
        {[](double s) { return 60 + 0.7 * std::sin(2 * kPiTest * 6 * (s - 0.1)); }, one},
        {[](double) { return 60.0; }, [](double s) { return 0.6 + 0.4 * std::sin(2 * kPiTest * 6 * (s - 0.1)); }},
        {[](double s) { return s < 0.25 ? 57 + 3 * (s - 0.1) / 0.15 : 60.0; }, one},
    };
    for (const auto& [midi, level] : single)
      for (const char* detail : {"natural", "detailed"}) {
        const auto notes = syllableNotes(midi, 0.9, level, detail);
        V2_CHECK(notes.size() == 1);
        if (notes.size() == 1) V2_CHECK_NEAR(notes[0].second, 60, 0.01f);
      }
  }
  // A turn whose middle note is short but held (300 ms at 57, 150 ms at 59,
  // 300 ms at 57) is three notes; a vibrato swing never holds a pitch.
  {
    const auto notes = syllableNotes([](double s) { return 57 + 2 * (smooth((s - 0.4) / 0.04) - smooth((s - 0.55) / 0.04)); }, 0.85);
    V2_CHECK(notes.size() == 3);
    if (notes.size() == 3) {
      V2_CHECK_NEAR(notes[0].second, 57, 0.01f);
      V2_CHECK_NEAR(notes[1].second, 59, 0.01f);
      V2_CHECK_NEAR(notes[2].second, 57, 0.01f);
    }
  }
  // A nasal coda that drops in pitch ("mon-key": the /n/ keeps the low band and
  // loses the vowel band, 250 ms at 57 after the vowel at 60) is the end of
  // the syllable's note, not a melisma.
  {
    // A sung phrase first, so the take's spectral variety is that of singing.
    const Take sung = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "say to me", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.5}, {"pitch": 59, "start": 0.5, "dur": 0.5}, {"pitch": 62, "start": 1.0, "dur": 0.7}]})");
    const Take coda = additive([](double s) { return 60 - 3 * smooth((s - 0.5) / 0.08); },
                               [](double s) { return s < 0.8 ? 1 - 0.85 * smooth((s - 0.54) / 0.05) : 1.0; }, 1.3,
                               [](double s) { return s < 0.8 || s >= 0.88 ? 1.0 : 0.0; });
    Take t = sung;
    const double at = static_cast<double>(t.x16k.size()) / novox::kAnalysisRate;
    t.x16k.insert(t.x16k.end(), coda.x16k.begin(), coda.x16k.end());
    std::vector<novox::Word> words = {word("say", 0.5, sung), word("to", 1.0, sung), word("me", 1.7, sung)};
    novox::Word monkey;
    monkey.text = "monkey";
    monkey.end = at + 1.3;
    words.push_back(monkey);
    const auto r = segment(t, words);
    V2_CHECK(lyricsOf(r) == "say to me mon- key");
    if (r.notes.size() == 5) {
      V2_CHECK_NEAR(r.notes[3].note.pitch, 60, 0.01f);
      V2_CHECK_NEAR(r.notes[4].note.start, at + 0.88, 0.06);
      V2_CHECK_NEAR(r.notes[4].note.pitch, 57, 0.01f);
    }
  }
  // Octaves are decided per word. "baby" with its second syllable sung
  // period-doubled (the pitch path reads it an octave low, 45): it is one word
  // at 57, not a leap inside the word.
  {
    const Take t = additive([](double) { return 57.0; }, [](double) { return 1.0; }, 0.85,
                            [](double s) { return s > 0.47 && s < 0.55 ? 0.05 : 1.0; },
                            [](double s) { return s >= 0.53 ? 0.9 : 0.0; });
    const auto r = segment(t, {word("baby", 0.85, t)});
    V2_CHECK(lyricsOf(r) == "ba- by");
    for (const auto& n : r.notes) V2_CHECK_NEAR(n.note.pitch, 57, 0.01f);
  }
  // A real octave leap inside a word (an octave up on "where") and between
  // words (an octave down to "now") keeps every octave.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "some-where now", "notes": [
      {"pitch": 60, "start": 0, "dur": 0.45}, {"pitch": 72, "start": 0.45, "dur": 0.55},
      {"pitch": 60, "start": 1.0, "dur": 0.6}]})");
    const auto r = segment(t, {word("somewhere", 1.0, t), word("now", 1.6, t)});
    V2_CHECK(lyricsOf(r) == "some- where now");
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 72, 0.01f);
      V2_CHECK_NEAR(r.notes[2].note.pitch, 60, 0.01f);
    }
  }
  // An octave leap within a single uninterrupted vowel is also two notes.
  // Both registers have long plateaus, with either an immediate change or an
  // 80 ms glide. A word/syllable boundary cannot rescue an octave-folded fit.
  // Keep six partials throughout the glides so none abruptly enter or leave
  // the 3.5 kHz cutoff; the instantaneous leaps retain the richer spectrum.
  for (double interval : {12.0, -12.0}) {
    for (double transition : {0.0, 0.08}) {
      const Take t = additive([interval, transition](double s) {
        const double amount = transition == 0.0 ? (s >= 0.8 ? 1.0 : 0.0)
                                                : std::clamp((s - (0.8 - transition / 2)) / transition, 0.0, 1.0);
        return 60.0 + interval * amount;
      }, [](double) { return 1.0; }, 1.65, [](double) { return 1.0; }, [](double) { return 0.0; }, transition > 0 ? 6 : 0);
      const auto r = segment(t, {word("oh", 1.65, t)});
      V2_CHECK(r.notes.size() == 2);
      V2_CHECK(lyricsOf(r) == "oh _");
      if (r.notes.size() == 2) {
        V2_CHECK_NEAR(r.notes[0].note.pitch, 60, 0.01f);
        V2_CHECK_NEAR(r.notes[1].note.pitch, 60 + interval, 0.01f);
        V2_CHECK_NEAR(r.notes[0].note.start, 0.1, 0.06);
        V2_CHECK_NEAR(r.notes[1].note.start, 0.8, 0.1);
        V2_CHECK(r.notes[0].note.dur > 0.5 && r.notes[1].note.dur > 0.5);
      }
      V2_CHECK(r.unassignedWords.empty());
    }
  }
  // A legato note runs to the next vowel onset, so it holds the next syllable's
  // onset consonant; its pitch comes from its own frames. A short "la" at 57
  // (80 or 100 ms), then "ma", whose /m/ (110 ms, longer than the vowel of "la")
  // the path voices at the next note's 62: "la" is still 57.
  for (double vowel : {0.08, 0.1}) {
    const double m = 0.1 + vowel, v = m + 0.11;
    const Take t = additive([m](double s) { return s < m ? 57.0 : 62.0; }, [m, v](double s) { return s >= m && s < v ? 0.08 : 1.0; }, 0.9,
                            [m, v](double s) { return s >= m && s < v ? 0.3 : 1.0; });
    const auto r = segment(t, {word("la", m, t), word("ma", 0.9, t)});
    V2_CHECK(lyricsOf(r) == "la ma");
    if (r.notes.size() == 2) {
      V2_CHECK_NEAR(r.notes[0].note.pitch, 57, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 62, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.start, v, 0.03);
    }
  }
  // Two words; the recogniser's end for the first is 250 ms early. The DTW
  // end only bounds where the next word may start, so they still align.
  const std::string twoWords = R"({"units": "seconds", )" + voice() + R"(, "lyrics": "sun set", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.7}, {"pitch": 59, "start": 0.85, "dur": 0.7}]})";
  {
    const Take t = render(twoWords);
    const auto r = segment(t, {word("sun", 0.45, t), word("set", 1.55, t)});
    V2_CHECK(lyricsOf(r) == "sun set");
    if (r.notes.size() == 2) V2_CHECK_NEAR(r.notes[1].note.start, 0.85 + t.lead, 0.06);
  }
  // A word's last syllable starts before the recogniser has finished the
  // word. "you in" has no clear onset (vowel to vowel on one pitch); without
  // that bound "in" slides to the end of its note, and on real takes each
  // following word then lands a syllable late.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "you in the mor-ning", "notes": [
      {"pitch": 60, "start": 0, "dur": 0.25}, {"pitch": 60, "start": 0.25, "dur": 0.45}, {"pitch": 59, "start": 0.75, "dur": 0.25},
      {"pitch": 60, "start": 1.0, "dur": 0.35}, {"pitch": 63, "start": 1.35, "dur": 0.5}]})");
    const auto r = segment(t, {word("you", 0.2, t), word("in", 0.5, t), word("the", 0.95, t), word("morning", 1.85, t)});
    V2_CHECK(lyricsOf(r) == "you in the mor- ning");
    if (r.notes.size() == 5) {
      V2_CHECK_NEAR(r.notes[1].note.start, 0.25 + t.lead, 0.08);
      V2_CHECK_NEAR(r.notes[2].note.start, 0.75 + t.lead, 0.08);
    }
  }
  // One word end is still one anchor: when a held first syllable makes the
  // recogniser finish "little" well before its second syllable, that syllable
  // keeps its clear onset.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "lit-tle lamb", "notes": [
      {"pitch": 53, "start": 0, "dur": 1.3}, {"pitch": 56, "start": 1.4, "dur": 1.0}, {"pitch": 55, "start": 2.5, "dur": 1.4}]})");
    for (double end : {0.6, 1.0}) {
      const auto r = segment(t, {word("little", end, t), word("lamb", 3.9, t)});
      V2_CHECK(lyricsOf(r) == "lit- tle lamb");
      if (r.notes.size() == 3) V2_CHECK_NEAR(r.notes[1].note.start, 1.4 + t.lead, 0.08);
    }
  }
  // Voiced audio no word claims (a vocable the recogniser dropped) becomes a
  // lyric-less note; the words keep their own notes.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "sun rise oh", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.7}, {"pitch": 59, "start": 0.85, "dur": 0.7},
      {"pitch": 62, "start": 2.2, "dur": 0.8}]})");
    const auto r = segment(t, {word("sun", 0.7, t), word("rise", 1.55, t)});
    V2_CHECK(lyricsOf(r) == "sun rise ~");
    V2_CHECK(!r.warnings.empty());
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[2].note.start, 2.2 + t.lead, 0.1);
      V2_CHECK_NEAR(r.notes[2].note.pitch, 62, 0.01f);
    }
  }
  // Unclaimed vocables are segmented like sung syllables: each re-attack on
  // one pitch is its own note (these /l/ onsets have p ~0.67, more likely than
  // not, but below what the DP asks of a hum after a hum), and a melisma inside
  // one sustains it ("_").
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [
      {"pitch": 55, "start": 0, "dur": 0.6, "lyric": "sun"}, {"pitch": 59, "start": 1.1, "dur": 0.4, "lyric": "lo"},
      {"pitch": 59, "start": 1.5, "dur": 0.4, "lyric": "lo"}, {"pitch": 59, "start": 1.9, "dur": 0.4, "lyric": "lo"},
      {"pitch": 57, "start": 2.8, "dur": 0.4, "lyric": "oh"}, {"pitch": 60, "start": 3.2, "dur": 0.5, "lyric": "_"}]})");
    const auto r = segment(t, {word("sun", 0.6, t)});
    V2_CHECK(lyricsOf(r) == "sun ~ ~ ~ ~ _");
    if (r.notes.size() == 6) {
      V2_CHECK_NEAR(r.notes[2].note.start, 1.5 + t.lead, 0.08);
      V2_CHECK_NEAR(r.notes[3].note.start, 1.9 + t.lead, 0.08);
      V2_CHECK_NEAR(r.notes[5].note.start, 3.2 + t.lead, 0.1);
      V2_CHECK_NEAR(r.notes[5].note.pitch, 60, 0.01f);
    }
    V2_CHECK(r.warnings.size() == 1);  // two unclaimed passages, one warning
  }
  // A transcript that cannot be the lyrics (a recogniser's one-word guess for a
  // whole phrase of syllables): the take is segmented as if it had no words,
  // with a warning, and the word is reported unassigned. A transcript with a
  // word per syllable is kept.
  {
    std::string notes;
    for (int i = 0; i < 16; ++i)
      notes += std::string(i ? ", " : "") + R"({"pitch": )" + std::to_string(i % 4 == 3 ? 59 : 55 + i % 4) +
               R"(, "start": )" + std::to_string(0.4 * i) + R"(, "dur": 0.4, "lyric": "la"})";
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [)" + notes + "]}");
    const auto melodyOnly = segment(t, {});
    const auto r = segment(t, {word("Huang", 6.4, t)});
    V2_CHECK(sameNotes(r, melodyOnly, 0.0) && r.notes.size() >= 12);
    V2_CHECK(r.unassignedWords == std::vector<size_t>{0});
    V2_CHECK(r.warnings.size() == 1 && r.warnings[0].find("cannot be the lyrics") != std::string::npos);
    std::vector<novox::Word> every;
    for (int i = 0; i < 16; ++i) every.push_back(word("la", 0.4 * (i + 1), t));
    const auto lyric = segment(t, every);
    V2_CHECK(lyric.unassignedWords.empty() && lyric.notes.size() == 16);
    for (const auto& w : lyric.warnings) V2_CHECK(w.find("cannot be the lyrics") == std::string::npos);
    // A transcript of the first notes only is judged on the span it covers: it
    // keeps its lyrics there, and the rest is lyric-less notes.
    every.resize(3);
    const auto first = segment(t, every);
    V2_CHECK(first.unassignedWords.empty() && lyricsOf(first).rfind("la la la ~", 0) == 0);
    for (const auto& w : first.warnings) V2_CHECK(w.find("cannot be the lyrics") == std::string::npos);
    // A transcript that heard the first and the last notes but skipped the
    // middle: word times tile the take, so the first word after the gap
    // stretches over it and the take-wide count is as thin as the one-word
    // guess, but most words time their own syllables. They keep their lyrics.
    std::vector<novox::Word> ends;
    for (int i : {0, 1, 13, 14, 15}) ends.push_back(word("la", 0.4 * (i + 1), t));
    const auto skipped = segment(t, ends);
    V2_CHECK(skipped.unassignedWords.empty());
    V2_CHECK(lyricsOf(skipped).rfind("la la ~", 0) == 0);
    for (const auto& w : skipped.warnings) V2_CHECK(w.find("cannot be the lyrics") == std::string::npos);
  }
  // Accented words from a recogniser (French, Spanish, German): their letters
  // are letters, kept whole in the lyric pieces, which stay valid UTF-8.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "la tet a nyo", "notes": [
      {"pitch": 57, "start": 0, "dur": 0.45}, {"pitch": 59, "start": 0.45, "dur": 0.45},
      {"pitch": 60, "start": 0.9, "dur": 0.45}, {"pitch": 62, "start": 1.35, "dur": 0.6}]})");
    const auto r = segment(t, {word("l\xC3\xA0", 0.45, t), word("t\xC3\xAAte", 0.9, t), word("a\xC3\xB1o", 1.95, t)});
    V2_CHECK(lyricsOf(r) == "l\xC3\xA0 t\xC3\xAAte a\xC3\xB1- o");
    for (const auto& n : r.notes) V2_CHECK(novox::validUtf8(n.note.lyric) == n.note.lyric);
  }
  // A singer tuned 35 cents flat of A440 through a whole song keeps the notes
  // they meant: pitches round against the take's own reference (detectedPitch
  // stays absolute); in tune, nothing changes.
  {
    const int phrase[] = {60, 62, 64, 65, 67, 65, 64, 62, 60, 64, 67, 72};
    constexpr int kNotes = 30;  // a short phrase is not trusted with an offset
    int melody[kNotes];
    for (int i = 0; i < kNotes; ++i) melody[i] = phrase[i % 12];
    for (const double offset : {-0.35, 0.0}) {
      std::string notes;
      for (int i = 0; i < kNotes; ++i)
        notes += std::string(i ? ", " : "") + R"({"pitch": )" + std::to_string(melody[i] + offset) + R"(, "start": )" +
                 std::to_string(0.45 * i) + R"(, "dur": 0.45, "lyric": "la"})";
      const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "notes": [)" + notes + "]}");
      std::vector<novox::Word> words;
      for (int i = 0; i < kNotes; ++i) words.push_back(word("la", 0.45 * (i + 1), t));
      const auto r = segment(t, words);
      V2_CHECK_NEAR(r.tuning, offset, 0.06);
      V2_CHECK(r.notes.size() == kNotes);
      if (r.notes.size() != kNotes) continue;
      for (int i = 0; i < kNotes; ++i) {
        V2_CHECK(r.notes[static_cast<size_t>(i)].note.pitch == melody[i]);
        V2_CHECK_NEAR(r.notes[static_cast<size_t>(i)].detectedPitch, melody[i] + offset, 0.15);
      }
      // The reference is reported in the result (cents from A440).
      novox_engine* e = novox_create(48000);
      std::string wordsJson = "[";
      for (size_t i = 0; i < words.size(); ++i)
        wordsJson += (i ? ", " : "") + std::string(R"({"word": "la", "start": )") + std::to_string(i ? words[i - 1].end : 0.0) +
                     R"(, "end": )" + std::to_string(words[i].end) + "}";
      wordsJson += "]";
      char* out = novox_transcribe(e, t.x16k.data(), t.x16k.size(), 16000, wordsJson.c_str(), R"({"algorithm": "singing-v2"})");
      V2_CHECK(out != nullptr);
      if (out) {
        const auto v = novox::json::parse(out);
        V2_CHECK(std::fabs(v["transcription"].num("tuningCents", 99) - offset * 100) <= 6);
        novox_free(out);
      }
      out = novox_transcribe(e, t.x16k.data(), t.x16k.size(), 16000, wordsJson.c_str(), R"({"algorithm": "singing-v1"})");
      V2_CHECK(out != nullptr && std::string(out).find("tuningCents") == std::string::npos);  // singing-v1 output unchanged
      novox_free(out);
      novox_destroy(e);
    }
  }
  // Unclaimed vocables before a word the recogniser stretched over them: the
  // word goes where its end anchor is, the vocables become hums.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "la la sun", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.5}, {"pitch": 57, "start": 0.6, "dur": 0.5},
      {"pitch": 60, "start": 1.4, "dur": 0.7}]})");
    const auto r = segment(t, {word("sun", 2.1, t)});
    V2_CHECK(lyricsOf(r) == "~ ~ sun");
    if (r.notes.size() == 3) V2_CHECK_NEAR(r.notes[2].note.start, 1.4 + t.lead, 0.08);
  }
  // A word sung with fewer syllables than the dictionary has ("fire" sung as
  // one syllable, F AY R) keeps all of its letters on the note it has.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "dictionary": {"fire": "F AY1 R"}, "notes": [
      {"pitch": 55, "start": 0, "dur": 0.5, "lyric": "my"}, {"pitch": 58, "start": 0.5, "dur": 0.8, "lyric": "fire"}]})");
    const auto r = segment(t, {word("my", 0.5, t), word("fire", 1.3, t)});
    V2_CHECK(lyricsOf(r) == "my fire");
    V2_CHECK(r.unassignedWords.empty());
  }
  // Digits are sung as number words ("twenty-seven" is one word, as lyric sheets write it).
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "twen-ty twen-ty sev-en", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.3}, {"pitch": 55, "start": 0.3, "dur": 0.3}, {"pitch": 57, "start": 0.6, "dur": 0.3},
      {"pitch": 57, "start": 0.9, "dur": 0.3}, {"pitch": 59, "start": 1.2, "dur": 0.3}, {"pitch": 59, "start": 1.5, "dur": 0.5}]})");
    const auto r = segment(t, {word("2027,", 2.0, t)});
    V2_CHECK(r.notes.size() == 6);
    V2_CHECK(lyricsOf(r) == "twen- ty twen- ty- sev- en");
    V2_CHECK(r.unassignedWords.empty());
  }
  // The notes show the words sung for a number, hyphenated like any word:
  // "100" is one | hun-dred, "49ers" one word for-ty-ni-ners.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "one hun-dred for-ty ni-ners", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.4}, {"pitch": 57, "start": 0.4, "dur": 0.3}, {"pitch": 57, "start": 0.7, "dur": 0.3},
      {"pitch": 59, "start": 1.0, "dur": 0.3}, {"pitch": 60, "start": 1.3, "dur": 0.3}, {"pitch": 62, "start": 1.6, "dur": 0.3},
      {"pitch": 60, "start": 1.9, "dur": 0.6}]})");
    const auto r = segment(t, {word("100", 1.0, t), word("49ers.", 2.5, t)});
    V2_CHECK(lyricsOf(r) == "one hun- dred for- ty- ni- ners");
    V2_CHECK(r.unassignedWords.empty());
  }
  // A vocable the dictionary lists as an abbreviation first ("ba" = B.A.) is
  // one sung syllable per note, as recognisers write it.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "dictionary": {"ba": "B AA1"}, "lyrics": "ba ba ba", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.45}, {"pitch": 57, "start": 0.55, "dur": 0.45}, {"pitch": 59, "start": 1.1, "dur": 0.6}]})");
    const auto r = segment(t, {word("Ba,", 0.45, t), word("ba,", 1.0, t), word("ba.", 1.7, t)});
    V2_CHECK(lyricsOf(r) == "Ba ba ba");
    V2_CHECK(r.unassignedWords.empty());
  }
  // A DC offset in the recording changes nothing, not even the analysed levels
  // that the silence gates use (novox_transcribe removes it).
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "say to me", "notes": [
      {"pitch": 55, "start": 0.3, "dur": 0.5}, {"pitch": 59, "start": 0.8, "dur": 0.5},
      {"pitch": 62, "start": 1.3, "dur": 0.7}]})");
    std::vector<float> offset = t.x16k;
    for (auto& v : offset) v += 0.05f;
    const std::string words = "[{\"word\": \"say\", \"start\": 0, \"end\": " + std::to_string(0.8 + t.lead) +
                              "}, {\"word\": \"to\", \"start\": 0, \"end\": " + std::to_string(1.3 + t.lead) +
                              "}, {\"word\": \"me\", \"start\": 0, \"end\": " + std::to_string(2.0 + t.lead) + "}]";
    novox_engine* engine = novox_create(44100);
    const char* options = "{\"algorithm\":\"singing-v2\",\"debugFrames\":true}";
    char* clean = novox_transcribe(engine, t.x16k.data(), t.x16k.size(), 16000, words.c_str(), options);
    char* shifted = novox_transcribe(engine, offset.data(), offset.size(), 16000, words.c_str(), options);
    V2_CHECK(clean && shifted);
    if (clean && shifted) {
      const auto fa = novox::json::parse(clean)["frames"], fb = novox::json::parse(shifted)["frames"];
      V2_CHECK(!fa.array.empty() && fa.array.size() == fb.array.size());
      double worst = 0;
      for (size_t i = 0; i < std::min(fa.array.size(), fb.array.size()); ++i)
        worst = std::max(worst, std::fabs(fa.array[i].array[2].number - fb.array[i].array[2].number));
      V2_CHECK(worst < 1.0);  // dB
      const auto a = novox::json::parse(clean)["notes"], b = novox::json::parse(shifted)["notes"];
      V2_CHECK(a.array.size() == 3 && b.array.size() == a.array.size());
      for (size_t i = 0; i < std::min(a.array.size(), b.array.size()); ++i) {
        V2_CHECK_NEAR(a.array[i].num("start", -1), b.array[i].num("start", -2), 0.009);
        V2_CHECK(a.array[i].num("pitch", -1) == b.array[i].num("pitch", -2));
        V2_CHECK(a.array[i].str("lyric", "") == b.array[i].str("lyric", "?"));
      }
    }
    novox_free(clean);
    novox_free(shifted);
    novox_destroy(engine);
  }
  // Without words the notes come from acoustics alone.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "say to me", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.5}, {"pitch": 59, "start": 0.5, "dur": 0.5},
      {"pitch": 62, "start": 1.0, "dur": 0.7}]})");
    const auto r = segment(t, {});
    V2_CHECK(r.notes.size() == 3);
    if (r.notes.size() == 3) {
      V2_CHECK(lyricsOf(r) == "~ ~ ~");
      V2_CHECK_NEAR(r.notes[0].note.pitch, 55, 0.01f);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 59, 0.01f);
      V2_CHECK_NEAR(r.notes[2].note.pitch, 62, 0.01f);
    }
    V2_CHECK(r.unassignedWords.empty());
  }
  // A loud unvoiced burst in a rest (a breath into the mic, a bump) belongs to
  // neither phrase: both phrases still get their notes.
  {
    const Take sung = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "say to me", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.5}, {"pitch": 59, "start": 0.5, "dur": 0.5},
      {"pitch": 62, "start": 1.0, "dur": 0.7}]})");
    Take t = sung;
    const size_t gap = t.x16k.size();
    t.x16k.resize(gap + 16000, 0.0f);
    uint32_t seed = 7;
    for (size_t i = gap + 5600; i < gap + 10400; ++i) {
      seed = seed * 1664525u + 1013904223u;
      t.x16k[i] = 4.0f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    t.x16k.insert(t.x16k.end(), sung.x16k.begin(), sung.x16k.end());
    const auto r = segment(t, {});
    size_t before = 0, after = 0;
    for (const auto& n : r.notes) (n.note.start < static_cast<double>(gap) / novox::kAnalysisRate ? before : after)++;
    V2_CHECK(before == 3 && after == 3);
  }
  // Junk and empty word text never take a note; a word with no sung audio
  // near it is left unassigned with a warning.
  {
    const Take t = render(twoWords);
    std::vector<novox::Word> words = {word("\xE2\x99\xAA", 0.1, t), word("sun", 0.7, t), word("", 0.75, t),
                                      word("set", 1.55, t), word("again", 6.0, t)};
    const auto r = segment(t, words);
    V2_CHECK(lyricsOf(r) == "sun set");
    V2_CHECK(r.unassignedWords.size() == 3);
    V2_CHECK(r.warnings.size() >= 3);
  }
  // A quiet breath after the last note is not a note.
  {
    Take t = render(twoWords);
    uint32_t seed = 1;
    const size_t from = t.x16k.size();
    t.x16k.resize(from + 8000, 0.0f);
    for (size_t i = from + 2000; i < from + 6000; ++i) {
      seed = seed * 1664525u + 1013904223u;
      t.x16k[i] = 0.004f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    const auto r = segment(t, {word("sun", 0.7, t), word("set", 1.55, t)});
    V2_CHECK(lyricsOf(r) == "sun set");
  }
  // Silence, near-silence and very short input give no notes and no errors.
  {
    const std::vector<float> zeros(16000, 0.0f);
    const auto pitch = novox::analyzeSingingPitch(zeros);
    novox::MelodyOptions opts;
    opts.algorithm = "singing-v2";
    novox::Word w;
    w.text = "hello";
    w.end = 0.5;
    auto r = novox::segmentSinging(zeros, pitch, {w}, lexicon(), opts);
    V2_CHECK(r.notes.empty());
    V2_CHECK(r.unassignedWords.size() == 1);
    std::vector<float> tiny(16000);
    for (size_t i = 0; i < tiny.size(); ++i) tiny[i] = 1e-6f * std::sin(0.05f * static_cast<float>(i));
    r = novox::segmentSinging(tiny, novox::analyzeSingingPitch(tiny), {}, lexicon(), opts);
    V2_CHECK(r.notes.empty());
    for (size_t n : {size_t(0), size_t(1), size_t(100), size_t(700)}) {
      std::vector<float> x(n);
      for (size_t i = 0; i < n; ++i) x[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));
      r = novox::segmentSinging(x, novox::analyzeSingingPitch(x), {w}, lexicon(), opts);
      V2_CHECK(r.notes.size() <= 1);
    }
  }
  // Level: the same take at -20, -40 and -60 dB gives the same notes (every
  // level is measured against the take's own singing).
  {
    const Take t = render(twoWords);
    const auto full = segment(t, {word("sun", 0.7, t), word("set", 1.55, t)});
    V2_CHECK(lyricsOf(full) == "sun set");
    for (float gain : {0.1f, 0.01f, 0.001f}) {
      Take quiet = t;
      for (float& s : quiet.x16k) s *= gain;
      V2_CHECK(sameNotes(segment(quiet, {word("sun", 0.7, t), word("set", 1.55, t)}), full, 1e-9));
    }
  }
  // A quiet section (a verse 20 dB under the chorus before it) is segmented as
  // if it stood alone: its onsets are judged against its own phrase's level.
  {
    const Take loud = render(twoWords);
    const Take lemonade = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "lem-o-nade", "notes": [
      {"pitch": 57, "start": 0, "dur": 0.4}, {"pitch": 57, "start": 0.4, "dur": 0.35},
      {"pitch": 57, "start": 0.75, "dur": 0.6}]})");
    auto take = [&](float gain) {
      Take t = loud;
      t.x16k.resize(t.x16k.size() + 16000, 0.0f);
      const double offset = t.x16k.size() / 16000.0 + lemonade.lead - t.lead;  // score time of the second phrase
      for (float s : lemonade.x16k) t.x16k.push_back(gain * s);
      return segment(t, {word("sun", 0.7, t), word("set", 1.55, t), word("lemonade", offset + 1.35, t)});
    };
    const auto even = take(1.0f), quiet = take(0.1f);
    V2_CHECK(lyricsOf(even) == "sun set lem- o- nade");
    V2_CHECK(sameNotes(quiet, even, 0.03));
  }
  // ... but a faint pitched sound alone in a long rest (bleed, 40 dB under the
  // singing) does not become a phrase of its own level with a note.
  {
    Take t = render(twoWords);
    const size_t from = t.x16k.size() + 32000;
    t.x16k.resize(from + 56000, 0.0f);
    for (size_t i = 0; i < 8000; ++i) {
      const double s = i / 16000.0, fade = std::min({1.0, s / 0.02, (0.5 - s) / 0.02});
      t.x16k[from + i] = static_cast<float>(0.005 * fade * (std::sin(2 * kPiTest * 196 * s) + 0.5 * std::sin(2 * kPiTest * 392 * s)));
    }
    const auto r = segment(t, {word("sun", 0.7, t), word("set", 1.55, t)});
    V2_CHECK(lyricsOf(r) == "sun set");
  }
  // A loud syllable the pitch path leaves unvoiced (rough phonation) is still
  // sung: it can take its lyric, with the pitch of its low band.
  {
    const Take la = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "la", "notes": [{"pitch": 55, "start": 0, "dur": 0.5}]})");
    Take t = la;
    t.x16k.resize(t.x16k.size() + 5600, 0.0f);  // 350 ms rest
    const double hey = t.x16k.size() / 16000.0;
    const auto syllable = roughSyllable(220, 0.2, 0.15);
    t.x16k.insert(t.x16k.end(), syllable.begin(), syllable.end());
    t.x16k.resize(t.x16k.size() + 4800, 0.0f);
    const double second = t.x16k.size() / 16000.0;
    t.x16k.insert(t.x16k.end(), la.x16k.begin(), la.x16k.end());
    const auto r = segment(t, {word("la", 0.5, t), word("hey", hey + 0.2 - t.lead, t), word("la", second + 0.5, t)});
    V2_CHECK(lyricsOf(r) == "la hey la");
    if (r.notes.size() == 3) {
      V2_CHECK_NEAR(r.notes[1].note.start, hey, 0.03);
      V2_CHECK(r.notes[1].note.dur > 0.15);
      V2_CHECK_NEAR(r.notes[1].note.pitch, 57, 0.01f);
      V2_CHECK(r.notes[1].confidence <= 0.3f);
    }
  }
  // The C API path (as the plugin calls it): word tokens are parsed and used,
  // debugFrames only adds the analysis frames, malformed tokens are rejected.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "re-mem-ber", "notes": [
      {"pitch": 55, "start": 0, "dur": 0.3}, {"pitch": 57, "start": 0.3, "dur": 0.3},
      {"pitch": 59, "start": 0.6, "dur": 0.6}]})");
    const std::string words = R"([{"word": "remember", "start": 0, "end": )" + std::to_string(1.2 + t.lead) +
                              R"(, "probability": 0.9, "tokens": [{"text": " re", "time": )" +
                              std::to_string(0.25 + t.lead) + R"(}, {"text": "member", "time": )" +
                              std::to_string(1.2 + t.lead) + "}]}]";
    novox_engine* e = novox_create(16000);
    char* plain = novox_transcribe(e, t.x16k.data(), t.x16k.size(), novox::kAnalysisRate, words.c_str(),
                                   R"({"algorithm": "singing-v2"})");
    char* debug = novox_transcribe(e, t.x16k.data(), t.x16k.size(), novox::kAnalysisRate, words.c_str(),
                                   R"({"algorithm": "singing-v2", "debugFrames": true})");
    V2_CHECK(plain && debug);
    if (plain && debug) {
      const auto a = novox::json::parse(plain), b = novox::json::parse(debug);
      V2_CHECK(a["transcription"].str("algorithm", "") == "singing-v2");
      V2_CHECK(a["transcription"].str("detail", "") == "natural");  // the default
      V2_CHECK(a["notes"].array.size() == 3 && b["notes"].array.size() == 3);
      V2_CHECK(!a.has("frames") && b["frames"].array.size() == novox::analysisFrameCount(t.x16k.size()));
      std::string lyrics;
      for (const auto& n : a["notes"].array) lyrics += n.str("lyric", "~") + " ";
      V2_CHECK(lyrics == "re- mem- ber ");
    }
    novox_free(plain);
    novox_free(debug);
    // The note detail: "natural" (the default) or "detailed", reported with the
    // result; anything else is rejected, whatever the algorithm.
    for (const char* bad : {R"({"algorithm": "singing-v2", "detail": "Detailed"})", R"({"algorithm": "singing-v2", "detail": "fine"})",
                            R"({"algorithm": "singing-v2", "detail": 1})", R"({"algorithm": "singing-v2", "detail": null})",
                            R"({"algorithm": "legacy", "detail": "coarse"})"}) {
      V2_CHECK(novox_transcribe(e, t.x16k.data(), t.x16k.size(), novox::kAnalysisRate, words.c_str(), bad) == nullptr);
      V2_CHECK(std::string(novox_last_error(e)).find("detail") != std::string::npos);
    }
    for (const char* detail : {"natural", "detailed"}) {
      const std::string options = std::string(R"({"algorithm": "singing-v2", "detail": ")") + detail + "\"}";
      char* out = novox_transcribe(e, t.x16k.data(), t.x16k.size(), novox::kAnalysisRate, words.c_str(), options.c_str());
      V2_CHECK(out != nullptr);
      if (out) V2_CHECK(novox::json::parse(out)["transcription"].str("detail", "") == detail);
      novox_free(out);
    }
    {
      char* legacy = novox_transcribe(e, t.x16k.data(), t.x16k.size(), novox::kAnalysisRate, words.c_str(),
                                      R"({"algorithm": "legacy", "detail": "detailed"})");
      V2_CHECK(legacy != nullptr);  // validated, ignored, not reported: legacy has no note detail
      if (legacy) V2_CHECK(!novox::json::parse(legacy)["transcription"].has("detail"));
      novox_free(legacy);
    }
    const std::string badToken = R"([{"word": "re", "start": 0, "end": 1, "tokens": [{"text": "re", "time": "x"}]}])";
    V2_CHECK(novox_transcribe(e, t.x16k.data(), t.x16k.size(), novox::kAnalysisRate, badToken.c_str(),
                              R"({"algorithm": "singing-v2"})") == nullptr);
    novox_destroy(e);
  }
  // Articulation: candidate onsets are in time order with evidence in 0..1.
  {
    const Take t = render(twoWords);
    const auto pitch = novox::analyzeSingingPitch(t.x16k);
    const auto f = novox::analyzeArticulation(t.x16k, pitch);
    const auto cands = novox::findBoundaryCandidates(f, pitch);
    V2_CHECK(f.frames == pitch.frames.size());
    V2_CHECK(!cands.empty());
    bool ordered = true, bounded = true, found = false;
    for (size_t i = 0; i < cands.size(); ++i) {
      ordered &= i == 0 || cands[i].frame > cands[i - 1].frame;
      bounded &= cands[i].any >= 0 && cands[i].any <= 1 && cands[i].soundEnd <= cands[i].frame;
      found |= std::fabs(novox::analysisFrameTime(cands[i].frame) - (0.85 + t.lead)) < 0.06 && cands[i].any > 0.5;
    }
    V2_CHECK(ordered && bounded);
    V2_CHECK(found);  // the vowel onset of "set"
  }
  // Words without usable timing anchors (ends at 0 here): the DP band then holds
  // every word at every position, and runs of left-out words are carried along
  // in one pass instead of walked from every boundary (quadratic in the words).
  // Every word is either sung or reported.
  {
    const Take t = render(R"({"units": "seconds", )" + voice() + R"(, "lyrics": "la la la", "notes": [
      {"pitch": 57, "start": 0, "dur": 0.45}, {"pitch": 57, "start": 0.45, "dur": 0.45},
      {"pitch": 57, "start": 0.9, "dur": 0.6}]})");
    std::vector<novox::Word> words(600);
    for (auto& w : words) w.text = "la";
    novox::MelodyOptions opts;
    opts.algorithm = "singing-v2";
    const auto r = novox::segmentSinging(t.x16k, novox::analyzeSingingPitch(t.x16k), words, lexicon(), opts);
    size_t sung = 0;
    for (const auto& n : r.notes) sung += n.note.lyric == "la" ? 1 : 0;
    V2_CHECK(sung >= 3 && sung == r.notes.size());
    V2_CHECK(sung + r.unassignedWords.size() == words.size());
  }
  // Audio too short for one analysis frame still accounts for every word.
  {
    novox::Word w;
    w.text = "hello";
    w.end = 0.5;
    novox::MelodyOptions opts;
    opts.algorithm = "singing-v2";
    const std::vector<float> none;
    const auto r = novox::segmentSinging(none, novox::analyzeSingingPitch(none), {w}, lexicon(), opts);
    V2_CHECK(r.notes.empty() && r.unassignedWords.size() == 1);
    novox_engine* e = novox_create(48000);
    const float two[2] = {0.1f, -0.1f};  // 44.1 kHz: nothing left at 16 kHz
    char* out = novox_transcribe(e, two, 2, 44100, R"([{"word": "hello", "start": 0, "end": 0.5}])", R"({"algorithm": "singing-v2"})");
    V2_CHECK(out != nullptr);
    if (out) {
      try {
        V2_CHECK(novox::json::parse(out)["transcription"]["unassignedWords"].array.size() == 1);
      } catch (const std::exception&) {
        V2_CHECK(false);
      }
      novox_free(out);
    }
    // Any finite word time stays valid JSON (a 1e300 end once overflowed the formatting buffer).
    std::vector<float> x(16000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.3 * std::sin(2 * kPiTest * 220 * i / 16000.0));
    for (const char* algorithm : {"legacy", "singing-v1", "singing-v2"}) {
      const std::string options = std::string(R"({"algorithm": ")") + algorithm + "\"}";
      out = novox_transcribe(e, x.data(), x.size(), 16000, R"([{"word": "huge", "start": 0, "end": 1e300}])", options.c_str());
      V2_CHECK(out != nullptr);
      if (!out) continue;
      try {
        const auto v = novox::json::parse(out);
        V2_CHECK_NEAR(v["transcription"]["transcriptWords"].array.at(0).num("end", 0) / 1e300, 1.0, 1e-9);
      } catch (const std::exception&) {
        V2_CHECK(false);
      }
      novox_free(out);
    }
    novox_destroy(e);
  }
}
