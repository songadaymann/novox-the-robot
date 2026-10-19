// Audio in: turn a sung recording into notes, and line up transcribed words
// with those notes.
//
//   melody:  YIN pitch tracking (at 16 kHz) -> voiced regions -> split at pitch
//            changes and at new-syllable onsets -> one note per sung event
//   lyrics:  words with timestamps (from Whisper or any ASR) are split into
//            syllables with the lexicon and spread over the notes sung during
//            each word; extra notes hold the vowel, words squeezed onto one
//            note keep their syllables together.
#pragma once

#include <string>
#include <vector>

#include "lexicon.hpp"
#include "score.hpp"

namespace novox {

struct PitchFrame {
  double time = 0;    // seconds (frame center)
  float f0 = 0;       // Hz, 0 = unvoiced
  float midi = 0;     // fractional MIDI note, 0 = unvoiced
  float clarity = 0;  // 0..1, how periodic the frame is
  float db = -120;    // frame level, dBFS
  float confidence = -1; // evidence reliability; negative means use clarity
  double endTime = 0;    // optional right hop boundary, clipped to recording end
};

struct MelodyOptions {
  float minNoteSeconds = 0.08f;   // shorter events merge into neighbors
  float splitSemitones = 0.7f;    // pitch change that starts a new note
  float onsetDb = 5.0f;           // level jump (after a dip) that starts a new note
  float silenceDb = -45.0f;       // below this (relative to the loudest frame) is silence
  bool roundPitches = true;       // snap note pitches to semitones
  bool stabilizePitch = false;    // experimental held-note protection for natural singing
  std::string algorithm = "legacy"; // explicit "singing-v1" / "singing-v2" opt-in; preserve old callers
  // singing-v2 only: how finely pitch divides a syllable. "natural": one note per
  // sung syllable plus real melodic steps (scoops, approaches, grace notes and
  // droops belong to their note); "detailed": those that hold a pitch are notes too.
  std::string detail = "natural";
};

struct TranscribedNote {
  Note note;             // start/dur in seconds, pitch, lyric (filled by alignLyrics)
  float detectedPitch = -1; // continuous median before semitone rounding
  float pitchSpread = 0; // semitones of variation inside the note (vibrato, glides)
  float confidence = 1;  // 0..1: pitch clarity x word confidence
};

// One ASR sub-word piece of a Word (e.g. whisper BPE tokens " re", "member").
struct WordToken {
  std::string text;
  double time = 0;            // seconds; when the recogniser finished this piece (DTW), approximate
  float probability = 1;
};

struct Word {
  std::string text;
  double start = 0, end = 0;  // seconds
  float probability = 1;      // ASR confidence
  std::vector<WordToken> tokens;  // optional, in order; singing-v2 uses them as soft timing anchors
};

std::vector<PitchFrame> trackPitch(const float* samples, size_t count, double sampleRate);
// Offline, measured multi-candidate pitch path; no assumed key or vocal register.
std::vector<PitchFrame> trackSingingPitch(const float* samples, size_t count, double sampleRate);

std::vector<TranscribedNote> segmentNotes(const std::vector<PitchFrame>& frames, const MelodyOptions& opts = {});

// Assigns lyric tokens to notes (in place). Returns warnings (e.g. words that
// fell between notes).
std::vector<std::string> alignLyrics(std::vector<TranscribedNote>& notes, const std::vector<Word>& words,
                                     const Lexicon& lexicon, bool preserveWords = false,
                                     std::vector<size_t>* unassignedWordIndices = nullptr,
                                     const std::vector<PitchFrame>* articulationFrames = nullptr);

// Seconds-based score JSON with per-note "confidence" and "pitchSpread".
// `debugFrames` (development only) adds "frames": [[time, midi, db, clarity, confidence], ...].
// `tuningSemitones` and `detail` (singing-v2) are reported under "transcription"
// as "tuningCents" and "detail".
std::string transcriptionToJson(const std::vector<TranscribedNote>& notes, const std::vector<std::string>& warnings,
                                const std::string& algorithm = "", const std::vector<Word>* transcriptWords = nullptr,
                                const std::vector<size_t>* unassignedWordIndices = nullptr,
                                const std::vector<PitchFrame>* debugFrames = nullptr,
                                const float* tuningSemitones = nullptr, const std::string* detail = nullptr);

}  // namespace novox
