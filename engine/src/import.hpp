// Importers: Standard MIDI Files and MusicXML -> Score.
//
// Both produce a seconds-based, monophonic Score. Lyrics come from the file when
// present (MIDI lyric meta events; MusicXML <lyric> with syllabic/extend), and
// can be supplied separately as a line to flow onto the notes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "score.hpp"

namespace novox {

struct ImportOptions {
  int track = -1;              // MIDI track / MusicXML part index; -1 = first one with notes
  std::string lyrics;          // lyric line to flow onto notes without lyrics
  bool beats = false;          // keep the file's beat positions (ignores its tempo map) instead of seconds
};

// Throws std::runtime_error on malformed input.
Score importMidi(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings);
Score importMusicXml(const std::string& xml, const ImportOptions& opts, std::vector<std::string>& warnings);

// Detects the format from the bytes ("MThd" = MIDI, '<' = MusicXML).
Score importAny(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings);

// Serializes a score as JSON in the format parseScore reads (seconds-based,
// or beat-based when the notes were imported with ImportOptions::beats).
std::string scoreToJson(const Score& score, bool beats = false);

}  // namespace novox
