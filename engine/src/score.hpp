// Score: notes with pitches, times and lyric syllables.
//
// JSON format (times in beats unless "units": "seconds"):
// {
//   "tempo": 96,
//   "voice": "robot" | { "preset": "female", "vibratoDepth": 0.5 },
//   "dictionary": { "hugh": "HH Y UW1" },
//   "lyrics": "hap-py birth-day",          // optional: flowed onto notes without lyrics
//   "notes": [
//     { "pitch": "C4", "dur": 1, "lyric": "hel-" },   // start defaults to end of previous
//     "D4 1 lo",                                       // compact: pitch dur [lyric]
//     "r 0.5",                                         // rest
//     { "pitch": 64, "start": 3, "dur": 2, "lyric": "_" }  // "_" holds the previous vowel
//   ]
// }
//
// Lyric tokens: "hel-" means the word continues on the next note; "_" (or "+")
// is a melisma; "{HH EH L OW}" gives explicit phonemes; empty = hum.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "voice.hpp"

namespace novox {

struct Note {
  double start = 0;   // seconds
  double dur = 0;     // seconds
  float pitch = 60;   // MIDI note number (fractional allowed)
  std::string lyric;
};

struct Score {
  double tempo = 120;
  std::vector<Note> notes;  // sorted by start, rests removed
  Voice voice;
  std::vector<std::pair<std::string, std::string>> dictionary;
  // Lyric line to flow onto notes that have no lyric. Unhyphenated words take
  // as many notes as they have syllables ("canoe" -> two notes) unless marked
  // with "~" ("wanna~" -> one note); a lone "~" leaves a note without a lyric.
  std::string lyrics;
};

// Throws std::runtime_error with a readable message on bad input.
Score parseScore(const json::Value& root);

// "C4" -> 60, "F#3" -> 54, "Bb5" -> 82. Returns false if not a note name.
bool parsePitch(const std::string& text, float& midi);

// Splits a lyric line into note tokens: words by spaces, syllables by hyphens
// ("hap-py" -> "hap-", "py"). "_" stays its own token.
std::vector<std::string> tokenizeLyrics(const std::string& line);

}  // namespace novox
