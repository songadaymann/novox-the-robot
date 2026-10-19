// Turns a Score into a Plan: a timeline of acoustic target segments (one or
// more per phoneme) plus the notes that drive pitch. The synthesizer only
// ever sees a Plan, so the same synth can later be fed from live MIDI.
#pragma once

#include <string>
#include <vector>

#include "lexicon.hpp"
#include "phonemes.hpp"
#include "score.hpp"

namespace novox {

enum class SegKind { Silence, Vowel, Offglide, Closure, Burst, Aspiration, Consonant };

struct Segment {
  SegKind kind = SegKind::Silence;
  double start = 0, end = 0;   // seconds
  Params p{};
  float tFreq = 0.04f;         // transition time into this segment for formants
  float tAmp = 0.012f;         // ... and for amplitudes
  std::string label;           // phoneme symbol, or "_" for silence
  bool takeNextFormants = false;  // HH / aspiration: use the following vowel's formants
};

struct PitchNote {
  double start = 0, end = 0;
  float midi = 60;
  bool legato = false;         // starts where the previous note ended
};

struct SyllableInfo {          // for debugging and UIs
  double start = 0, end = 0;
  std::string text;
  std::string phonemes;
  bool continues = false;      // word continues on the next syllable
};

struct Plan {
  std::vector<Segment> segments;
  std::vector<PitchNote> notes;
  std::vector<SyllableInfo> syllables;
  std::vector<std::string> warnings;
  double lead = 0;             // seconds of pre-roll added before score time 0
  double length = 0;           // total seconds to render
};

Plan planScore(const Score& score, const Lexicon& lexicon);

// One lyric token per note, using the planner's dictionary and exact flow rules.
// Empty tokens finish an automatically split word (or represent a lone "~").
std::vector<std::string> lyricFlowTokens(const std::string& line, const Lexicon& lexicon);

// True when consonants p[from, to) may begin an English syllable: any single
// consonant, or a cluster from the planner's onset list ("S T R", "P L", ...).
// The planner and the sung-syllable inventory both split words by maximal onset.
bool legalOnset(const Pronunciation& p, size_t from, size_t to);

// JSON dump of a plan (segments, syllables, warnings) for inspection.
std::string planToJson(const Plan& plan);

}  // namespace novox
