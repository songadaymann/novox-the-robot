#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <mutex>
#include <vector>
#include <juce_cryptography/juce_cryptography.h>

struct whisper_full_params;

// Independent offline worker: never uses the processor's engine or audio thread.
class AudioImport : private juce::Thread {
 public:
  AudioImport();
  ~AudioImport() override;
  // detailedNotes asks the engine to also split scoops, approach and grace notes and droops
  // into their own notes ("detail":"detailed"); the default sings one note per syllable.
  juce::var start(juce::MemoryBlock data, juce::String name, bool downloadModel, bool melodyOnly,
                  bool detailedNotes = false);
  void cancel();
  juce::var preview(const juce::String& score);
  juce::var status() const;
  static juce::File modelFile();
  // The audio files an import decodes here, by extension (without the dot): JUCE's readers for
  // WAV, MP3, AIFF, FLAC and Ogg, and on macOS Core Audio's for m4a (AAC/ALAC), which Windows lacks.
  static const juce::StringArray& extensions();
  // Why lyrics recognition cannot run on this computer, or "" when it can. Windows builds use
  // ggml's AVX2 baseline (plugin/CMakeLists.txt: AVX2, FMA, F16C, BMI2), which some older or
  // low-end CPUs lack.
  static juce::String recognitionUnavailable();
  // whisper.cpp's CPU threads: 4 on macOS (Metal does the heavy work there) and Linux; on Windows,
  // where it all runs on the CPU, one per physical core but one, at most 8.
  static int recognitionThreads();
  // The novox_transcribe options of an import (JSON).
  static juce::String transcribeOptions(bool detailedNotes);
  // One recognized whisper piece: its text as decoded (a leading space starts a word), the time
  // the model finished it (seconds, DTW) and its probability.
  struct Piece { juce::String text; double time; float probability; };
  // The timestamped words novox_transcribe takes, from the pieces in order: each word starts at
  // the previous word's end and ends at its last piece; every word lies within [0, seconds] with
  // start <= end, and the last ends at `seconds` (its measured end kept as "asrEnd").
  static juce::Array<juce::var> wordsFromPieces(const std::vector<Piece>& pieces, double seconds);
  // One bounded retry for a sparse transcript over several confident, unclaimed sung notes.
  // Word indices include context on either side; a negative firstWord means no retry.
  struct LyricRetryWindow {
    int firstWord = -1, lastWord = -1, unclaimedNotes = 0;
    double start = 0, end = 0, unclaimedSeconds = 0;
  };
  static LyricRetryWindow lyricRetryWindow(const juce::Array<juce::var>& words,
                                           const juce::var& score, double seconds);
  // Local pieces use crop-relative times. Returns no words if the alternative changes lexical
  // context, crowds timestamps or adds more vocables than the acoustic passage can support.
  static juce::Array<juce::var> mergeLyricRetry(const juce::Array<juce::var>& words,
      const std::vector<Piece>& localPieces, const LyricRetryWindow& window, double seconds);
  static bool lyricRetryImprovesCoverage(const juce::var& original, const juce::var& alternative,
                                         const LyricRetryWindow& window);
  // Where a DAW placed a recording on its timeline: seconds since 00:00:00:00 (<= 0 when the file
  // carries no stamp) and the application that wrote it. Read from a Broadcast WAV bext
  // TimeReference, or from the "Timestamp: <samples>" marker of a region Logic hands to a plugin.
  struct TimelineStamp { double seconds = -1; juce::String originator; };
  static TimelineStamp timelineStamp(const juce::MemoryBlock& data);
  // Development hook for recognition experiments (NovoxPluginTests --whisper-variant): edits the
  // decoding settings on the worker thread just before whisper_full. The plugin never sets it.
  std::function<void(whisper_full_params&)> adjustRecognition;
  bool retrySparseLyrics = true;  // development A/B hook; the plugin leaves this enabled
  std::function<void(const juce::var&)> inspectLyricRetry;  // development evidence, never set by the plugin
 private:
  void run() override;
  void publish(const juce::String& state, const juce::String& message, juce::var result = {});
  mutable std::mutex mutex_;
  juce::String state_ = "idle", message_;
  juce::var result_;
  juce::MemoryBlock data_;
  juce::String name_, previewScore_;
  juce::var previousResult_;
  bool download_ = false, melodyOnly_ = false, detailed_ = false;
};
