#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <atomic>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <vector>

#include "Song.h"
#include "HostMmc.h"
#include "novox.h"

// Shares one engine (and its ~126k-word dictionary) across every plugin instance.
class SharedEngine {
 public:
  static std::shared_ptr<SharedEngine> get();
  ~SharedEngine();

  // Offline render at a fixed level (no normalization). Thread-safe. The engine is locked only
  // while the song is planned (milliseconds); the audio then renders on its own, so plan() never
  // waits for a song's render (a whole song takes about a second). Returns false without an error
  // when `cancelled` turns true between blocks.
  bool render(const juce::String& scoreJson, double sampleRate, std::vector<float>& out, double& lead,
              juce::String& error, const std::function<bool()>& cancelled = {});
  juce::String plan(const juce::String& scoreJson, juce::String& error);
  juce::String lyricTokens(const juce::String& text, const juce::String& dictionaryJson, juce::String& error);
  juce::String importFile(const juce::MemoryBlock& data, const juce::String& optionsJson, juce::String& error);
  juce::StringArray voices();

 private:
  SharedEngine();
  std::mutex mutex_;
  novox_engine* engine_ = nullptr;
};

class NovoxProcessor : public juce::AudioProcessor, private juce::Timer {
 public:
  NovoxProcessor();
  ~NovoxProcessor() override;

  // AudioProcessor
  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override {}
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }
  const juce::String getName() const override { return JucePlugin_Name; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 0.5; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;
  // A .novox file the editor opens: like a state the host loads, but an edit the host is told of.
  void openSongFile(const void* data, int sizeInBytes);
  // The state as JSON; `withProject` adds what belongs to the DAW project rather than the song (the
  // MMC state), left out of a .novox file.
  void writeState(juce::MemoryBlock& destData, bool withProject);

  // The host is MuseScore (by its path): it runs instruments only while it plays, and before 4.7
  // sends no playing flag, so the plugin follows its sample clock while its blocks carry on from one
  // another (status.clock "samples"). Set when the plugin is made; tests set it.
  static bool hostIsMuseScore();
  void setNotationHost(bool museScore);

  // A plugin in a DAW (not the standalone app or a test host), and its MMC port to the DAW's
  // transport (HostMmc; null when none could be opened).
  bool inDaw() const { return wrapperType != wrapperType_Standalone && wrapperType != wrapperType_Undefined; }
  std::shared_ptr<HostMmc> mmc() const { return mmc_; }
  // What the page learned about this project's MMC (it is a per-project Logic setting): that the DAW
  // follows it, the seconds to add to a locate, and the DAW's frame rate (24, 25, 29.97, 30; 0 =
  // not known). Saved with the DAW project.
  struct MmcState { bool works = false; double offset = 0; double rate = 0; };
  void setMmcState(const MmcState& s);
  MmcState mmcState() const;

  juce::Point<int> editorSize {1000, 660}; // Retain size when this instance reopens.
  // The largest DAW state or .novox file read back: a song built from many imported recordings
  // keeps each one (about 2.5 MB a minute).
  static constexpr int maxStateBytes = 512 * 1024 * 1024;

  // --- Message-thread API for the editor ------------------------------------
  juce::String getSongJson() const;
  void setSongJson(const juce::String& json);   // replaces the song, re-renders audible changes
  juce::String planJson(juce::String& error);   // syllables for the piano roll
  // Read-only preview for a pending editor transaction; does not render or change this instance.
  juce::String planSongJson(const juce::String& songJson, double bpm, juce::String& error);
  juce::String lyricTokens(const juce::String& text, const juce::String& dictionaryJson, juce::String& error) {
    return engine_->lyricTokens(text, dictionaryJson, error);
  }
  // A MIDI, MusicXML or compressed MusicXML (.mxl) file as the engine's score JSON in beats.
  // `lyrics` is sung only by a file without its own; `choice` is the editor's {track, partVoice,
  // verse} (plugin/ui/score-import.js). Empty with `error` set when the file can't be read.
  juce::String importFile(const juce::MemoryBlock& data, const juce::String& lyrics, juce::String& error,
                          const juce::var& choice = {});
  // The score inside a compressed MusicXML file: the rootfile its META-INF/container.xml names,
  // else the first .musicxml or .xml outside META-INF. Empty with `error` set otherwise.
  static juce::MemoryBlock musicXmlFromMxl(const juce::MemoryBlock& zip, juce::String& error);
  bool auditionAudio(const juce::MemoryBlock&, juce::String& error, double start = 0, double end = -1, bool loop = false);
  bool auditionSong(double startBeat, double endBeat, bool loop, juce::String& error);
  // The newest song version represented by the audio (-1 before the first render).
  // Display lyrics and import metadata can change without changing that audio.
  int renderedVersion();
  // A Compare review is still sounding.
  bool reviewing();
  // Compare > Original: pieces [{id, from, to, at}] of stored source recordings (seconds [from, to)
  // of each, played `at` seconds into the phrase), mixed where they overlap.
  bool auditionSources(const juce::var& pieces, bool loop, juce::String& error);
  void stopAudition();
  // Source recordings of audio imports, by recordingId: {original (16 kHz WAV data URL), waveform,
  // seconds, bpm, startBeat}. The DAW state and .novox files keep every one the song refers to.
  bool storeSource(const juce::String&, const juce::var&);
  juce::var getSource(const juce::String&) const;
  juce::MemoryBlock exportMidi(juce::String& error) const;
  juce::MemoryBlock exportWave(juce::String& error) const;
  void previewNote(int pitch);
  juce::StringArray voiceNames() { return engine_->voices(); }

  void setCaptureArmed(bool armed) { captureArmed_ = armed; }
  bool isCaptureArmed() const { return captureArmed_; }
  // Standalone / no-host transport. Play starts at the cursor (the ruler's last click, else the
  // top) and Stop returns there; setPreviewCursor also moves a running playhead.
  void setPreviewPlaying(bool playing);
  void setPreviewCursor(double beats);
  void setPreviewTempo(double bpm);

  struct Status {
    double beats = 0;         // playhead position in beats
    double seconds = NAN;     // host timeline seconds at `beats` (NaN if the host doesn't say)
    double editOrigin = NAN;  // host timeline start, seconds from 00:00:00:00 (NaN if unknown)
    double bpm = 120;
    bool playing = false;
    bool hostTransport = false;  // false in the standalone app (internal transport)
    bool hostTempo = false;      // the host gives its tempo with its beat position (a DAW; maybe not Dorico)
    // What the playhead follows: "beats", the host's beat position (a DAW); "samples", the host's
    // sample clock at the song's tempo while a VST3 host that gives no beat position plays (and
    // MuseScore 4 throughout, which calls the plugin only then); "own", Novox's own transport (the
    // standalone app, or a host that has not called it yet or doesn't play).
    const char* clock = "own";
    bool capture = false;
    bool rendering = false;
    juce::String error;       // last render error, if any
    int songVersion = 0;      // bumps whenever the song changes (e.g. captured notes)
    // The audio being played: the song version it was rendered from (-1: none yet), renders started
    // and given up for a newer song, and seconds since one last finished (for the page to tell
    // when the audio heard is behind the edits, and why). reviewing: a Compare review is audible.
    int renderedVersion = -1;
    int rendersStarted = 0, rendersCancelled = 0;
    double secondsSinceRender = -1;
    bool reviewing = false;
    // The host's timecode rate (0 = not reported) and processBlock calls so far: a host that stops
    // calling a plugin (Logic, for a track it is not playing) leaves `playing` and `beats` stale.
    bool daw = false;         // a plugin in a DAW, whether or not the DAW has called it yet
    MmcState mmc;
    int frameBase = 0;
    bool frameDrop = false, framePullDown = false;
    uint32_t blocks = 0;
  };
  Status getStatus() const;

 private:
  struct Rendered {
    std::vector<float> audio;
    double sampleRate = 0, bpm = 0, lead = 0;
    int version = 0, audioRevision = 0;
  };

  void timerCallback() override;
  void requestRender();
  void renderWorker();
  // Offline (isNonRealtime) only: blocks until the render is of the current song at this sample
  // rate and, when tempoToo (the first block after the transport starts), at the tempo the worker
  // renders; or until this song's render fails, or kOfflineRenderWaitMs passes.
  void waitForRender(double sampleRate, bool tempoToo);
  bool loadState(const void* data, int sizeInBytes);  // false: not a Novox state
  void storeMmc(const MmcState& s);                   // no edit (a loaded state)
  static constexpr double kOfflineRenderWaitMs = 120000;

  // song: a copy of the song's render (Compare > Robot), stale once the song changes.
  struct Audition { std::vector<float> samples; double rate = 0, position = 0; bool notePreview = false; double begin = 0, end = 0; bool loop = false; bool song = false; };
  juce::SpinLock auditionLock_;
  std::unique_ptr<Audition> audition_;
  std::shared_ptr<SharedEngine> engine_;

  // Song (message thread owns; state save may read from another thread).
  mutable std::mutex songMutex_;
  Song song_;
  std::map<juce::String,juce::var> sources_;
  std::atomic<int> songVersion_{0};
  // Unlike songVersion_, only changes when the score can sound different. A metadata edit
  // must neither cancel a render in progress nor make its completed audio appear stale.
  std::atomic<int> audioRevision_{0};
  // Edits to what the project saves (song, tempo, MMC), not loads; the timer tells the host.
  std::atomic<int> stateEdits_{0};
  // Message thread: edits told to the host, the last count seen, when it last changed and when the
  // first edit not yet told was seen (-1: none).
  int stateEditsNotified_ = 0, stateEditsSeen_ = 0;
  double lastEditAt_ = 0, firstEditAt_ = -1;
  static constexpr double kStateQuietMs = 1000, kStateMaxDelayMs = 5000;

  // Rendered audio handed to the audio thread.
  juce::SpinLock renderedLock_;
  std::shared_ptr<const Rendered> rendered_;
  std::vector<std::shared_ptr<const Rendered>> retired_;  // freed on the message thread
  std::mutex retiredMutex_;

  // Background renderer.
  std::unique_ptr<juce::Thread> worker_;
  juce::WaitableEvent renderRequested_;
  std::atomic<bool> songRenderRequested_{false};
  std::atomic<int> notePreviewPitch_{-1};
  std::atomic<bool> rendering_{false};
  std::mutex errorMutex_;
  juce::String renderError_;
  int failedRevision_ = -1;  // the audioRevision renderError_ is for (-1: the last render worked)

  // Transport as seen by the audio thread.
  std::atomic<double> sampleRate_{48000};
  std::atomic<double> hostBpm_{120};
  std::atomic<int> hostFrameRate_{0};  // base | drop << 8 | pull-down << 9 (0 = unknown)
  std::shared_ptr<HostMmc> mmc_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);  // for work posted to the message thread
  std::atomic<bool> mmcWorks_{false};
  std::atomic<double> mmcOffset_{0}, mmcRate_{0};
  std::atomic<uint32_t> blocks_{0};
  bool wasPlaying_ = false, transportStarted_ = false;  // audio thread
  std::atomic<double> lastRenderDone_{-1e9};  // Time::getMillisecondCounterHiRes of the last finished render
  std::atomic<int> rendersStarted_{0}, rendersCancelled_{0};
  std::atomic<double> playheadBeats_{0};
  std::atomic<double> playheadSeconds_{NAN};
  std::atomic<double> editOrigin_{NAN};
  std::atomic<bool> playing_{false};
  std::atomic<bool> hostTransport_{false};
  std::atomic<bool> hostTempo_{false};
  std::atomic<int> clock_{0};  // Status::clock: 0 own, 1 beats, 2 samples
  std::atomic<bool> museScore_{false};
  // Audio thread: the host has given a beat position (a DAW: its sample clock is never followed),
  // it has said it plays (its playing flag is trusted), and its sample clock: the last position,
  // the samples played since it last changed, and where the next block carries on (-1: unknown).
  bool hostGavePpq_ = false, hostFlagsPlaying_ = false;
  long long sampleClockLast_ = -1, sampleClockSame_ = 0, sampleClockNext_ = -1;
  // Message thread: processBlock calls seen, and when they last changed.
  static constexpr double kHostGoneMs = 200;
  uint32_t blocksSeen_ = 0;
  double blocksSeenAt_ = 0;
  std::atomic<bool> previewPlaying_{false};
  std::atomic<double> previewBpm_{120};
  double previewSamples_ = 0;  // audio thread
  std::atomic<double> previewCursor_{0};  // beats
  std::atomic<double> previewSeek_{-1};   // beats for the audio thread to move to (-1 = none)

  // Audio-thread cursor: host beat reports may be rounded between buffers.
  const Rendered* cursorRender_ = nullptr; // identity only; never dereferenced
  long long nextRenderSample_ = 0, nextHostSample_ = 0;
  bool cursorValid_ = false, cursorHasHostSamples_ = false;

  // MIDI capture: audio thread -> message thread.
  struct Captured {
    int pitch;
    double start, dur;
  };
  std::atomic<bool> captureArmed_{false};
  std::array<double, 128> heldSince_{};  // audio thread; <0 = not held
  juce::AbstractFifo captureFifo_{512};
  std::array<Captured, 512> captureBuf_{};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NovoxProcessor)
};
