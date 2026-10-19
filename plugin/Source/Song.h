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
  bool useNoteLyrics = false; // Imported text is displayed; note tokens control singing.
  juce::var voice = "std";         // preset name or {"preset": ..., overrides}
  juce::var keyCorrection;         // optional editor key settings and correction audit
  // One record per audio import, oldest first (plugin/ui/song-merge.js): its recordingId (the
  // source recording the processor keeps), startBeat and beats on the timeline, and the engine's
  // evidence (transcriptWords in seconds of the recording, originalNotes in beats). Songs saved
  // before 2026-10-17 had one import in "transcription"; fromJson reads it as a list of one.
  juce::var transcriptions;
  juce::var dictionary;            // {"word": "ARPABET"}

  juce::String toJson() const;                     // plugin state
  static Song fromJson(const juce::String& json);  // tolerant of missing fields
  juce::String toScoreJson(double bpm) const;      // engine input
  juce::StringArray recordingIds() const;          // the source recordings the imports refer to, once each

  void sortNotes();
  // Adds a captured note, trimming or replacing anything it overlaps (the
  // engine sings one note at a time).
  void addNote(const SongNote& n);

  static Song defaultSong();
};
