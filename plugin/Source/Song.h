#pragma once

#include <juce_core/juce_core.h>

// The song a plugin instance sings: notes in beats (so it follows the host's
// tempo), lyric line, voice and pronunciation overrides. Stored in the DAW
// project as JSON; converted to an engine score (see engine/src/score.hpp) at
// render time by adding the host tempo.
struct SongNote {
  double start = 0;  // beats from the DAW's bar 1
  double dur = 1;    // beats
  int pitch = 60;    // MIDI note
  juce::String lyric;  // per-note lyric token; empty = flows from `lyrics`
};

struct Song {
  juce::Array<SongNote> notes;
  juce::String lyrics;
  juce::var voice = "std";         // preset name or {"preset": ..., overrides}
  juce::var dictionary;            // {"word": "ARPABET"}

  juce::String toJson() const;                     // plugin state
  static Song fromJson(const juce::String& json);  // tolerant of missing fields
  juce::String toScoreJson(double bpm) const;      // engine input

  void sortNotes();
  // Adds a captured note, trimming or replacing anything it overlaps (the
  // engine sings one note at a time).
  void addNote(const SongNote& n);

  static Song defaultSong();
};
