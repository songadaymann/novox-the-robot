// Importers: Standard MIDI Files and MusicXML -> Score.
//
// Both produce a seconds-based, monophonic Score. Lyrics come from the file when
// present (MIDI lyric meta events; MusicXML <lyric> with syllabic/extend/elision),
// one syllable per note; a lyric line supplied separately is used only when the
// sung part has no lyrics of its own. MusicXML repeats, endings, D.C./D.S., coda
// and fine are played out in order, with verse N sung on pass N.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "score.hpp"

namespace novox {

class Lexicon;

struct ImportOptions {
  int track = -1;              // MIDI track / MusicXML part index; -1 = the first with lyrics, else the first with notes
  std::string lyrics;          // lyric line to flow onto the notes when the sung part has no lyrics
  bool beats = false;          // keep the file's beat positions (ignores its tempo map) instead of seconds
  std::string partVoice;       // MusicXML <voice> to sing; "" = the first with lyrics (unless another has
                               // more than twice as many), else the first
  int verse = 0;               // lyric verse (1-based); 0 = verse N on pass N of a repeat (verse 1 when not unfolding)
  bool unfold = true;          // MusicXML: play repeats, endings, D.C./D.S./coda/fine in order (false: as written)
  // Tells words from non-words where a MIDI file's verses can't be told apart by position (a tick
  // with syllables for only some verses); without one the lowest verse takes them.
  const Lexicon* lexicon = nullptr;
};

// What an import found, so a UI can offer the choices (part, voice, verse) and
// draw the score's bars and tempo. Beats are quarter notes from time 0, which
// is the start of the first measure (a pickup's first beat, as in notation apps).
struct ImportInfo {
  struct Voice {
    std::string id;
    int notes = 0, lyrics = 0;   // sung notes (a chord counts once) and those with a lyric
  };
  struct Part {
    int index = 0;
    std::string id, name;      // MusicXML part id and part-name; a MIDI track's name
    int notes = 0, lyrics = 0;
    std::vector<int> verses;   // lyric verse numbers present
    std::vector<Voice> voices; // MusicXML voices (empty for MIDI)
  };
  struct Tempo {
    double beat = 0, seconds = 0, bpm = 120;
  };
  struct Meter {
    double beat = 0, seconds = 0;
    int beats = 4, beatType = 4;
  };
  std::string format;          // "midi" or "musicxml"
  std::vector<Part> parts;
  int part = -1;               // index of the part / track sung
  std::string partVoice;       // the MusicXML voice sung
  int verse = 0;               // the verse sung throughout, or 0 when it follows the repeat passes
  bool fileLyrics = false;     // the notes' lyrics are the file's own (ImportOptions::lyrics unused)
  bool repeats = false;        // the score has repeats, endings or jumps
  bool unfolded = false;       // ... and they were played out
  double pickupBeats = 0;      // length of an implicit (pickup) first measure, in beats
  std::vector<Tempo> tempos;   // the tempo map, unrounded
  std::vector<Meter> meters;   // time signatures where they start
};

// Throws std::runtime_error on malformed input. `info`, when given, receives
// what the file holds and what was chosen.
Score importMidi(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings,
                 ImportInfo* info = nullptr);
Score importMusicXml(const std::string& xml, const ImportOptions& opts, std::vector<std::string>& warnings,
                     ImportInfo* info = nullptr);

// Detects the format from the bytes ("MThd" = MIDI, '<' = MusicXML).
Score importAny(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings,
                ImportInfo* info = nullptr);

// Serializes a score as JSON in the format parseScore reads (seconds-based,
// or beat-based when the notes were imported with ImportOptions::beats).
std::string scoreToJson(const Score& score, bool beats = false);

// The ImportInfo as a JSON object (the "import" member of novox_import's result).
std::string importInfoToJson(const ImportInfo& info);

// `s` as the inside of a JSON string: quotes, backslashes and control
// characters escaped, invalid UTF-8 replaced by U+FFFD.
std::string jsonEscape(const std::string& s);

}  // namespace novox
