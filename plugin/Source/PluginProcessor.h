#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "Song.h"
#include "nvs.h"

// Shares one engine (and its ~126k-word dictionary) across every plugin instance.
class SharedEngine {
 public:
  static std::shared_ptr<SharedEngine> get();
  ~SharedEngine();

  // Offline render at a fixed level (no normalization). Thread-safe.
  bool render(const juce::String& scoreJson, double sampleRate, std::vector<float>& out, double& lead,
              juce::String& error);
  juce::String plan(const juce::String& scoreJson, juce::String& error);
  juce::String importFile(const juce::MemoryBlock& data, const juce::String& optionsJson, juce::String& error);
  juce::StringArray voices();

 private:
  SharedEngine();
  std::mutex mutex_;
  nvs_engine* engine_ = nullptr;
};

class NvsProcessor : public juce::AudioProcessor, private juce::Timer {
 public:
  NvsProcessor();
  ~NvsProcessor() override;

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

  // --- Message-thread API for the editor ------------------------------------
  juce::String getSongJson() const;
  void setSongJson(const juce::String& json);   // replaces the song, re-renders
  juce::String planJson(juce::String& error);   // syllables for the piano roll
  juce::String importFile(const juce::MemoryBlock& data, const juce::String& lyrics, juce::String& error);
  juce::StringArray voiceNames() { return engine_->voices(); }

  void setCaptureArmed(bool armed) { captureArmed_ = armed; }
  bool isCaptureArmed() const { return captureArmed_; }
  // Standalone / no-host transport.
  void setPreviewPlaying(bool playing);
  void setPreviewTempo(double bpm);

  struct Status {
    double beats = 0;         // playhead position in beats
    double bpm = 120;
    bool playing = false;
    bool hostTransport = false;  // false in the standalone app (internal transport)
    bool capture = false;
    bool rendering = false;
    juce::String error;       // last render error, if any
    int songVersion = 0;      // bumps whenever the song changes (e.g. captured notes)
  };
  Status getStatus() const;

 private:
  struct Rendered {
    std::vector<float> audio;
    double sampleRate = 0, bpm = 0, lead = 0;
    int version = 0;
  };

  void timerCallback() override;
  void requestRender();
  void renderWorker();

  std::shared_ptr<SharedEngine> engine_;

  // Song (message thread owns; state save may read from another thread).
  mutable std::mutex songMutex_;
  Song song_;
  std::atomic<int> songVersion_{0};

  // Rendered audio handed to the audio thread.
  juce::SpinLock renderedLock_;
  std::shared_ptr<const Rendered> rendered_;
  std::vector<std::shared_ptr<const Rendered>> retired_;  // freed on the message thread

  // Background renderer.
  std::unique_ptr<juce::Thread> worker_;
  juce::WaitableEvent renderRequested_;
  std::atomic<bool> rendering_{false};
  std::mutex errorMutex_;
  juce::String renderError_;

  // Transport as seen by the audio thread.
  std::atomic<double> sampleRate_{48000};
  std::atomic<double> hostBpm_{120};
  std::atomic<double> playheadBeats_{0};
  std::atomic<bool> playing_{false};
  std::atomic<bool> hostTransport_{false};
  std::atomic<bool> previewPlaying_{false};
  std::atomic<double> previewBpm_{120};
  double previewSamples_ = 0;  // audio thread

  // MIDI capture: audio thread -> message thread.
  struct Captured {
    int pitch;
    double start, dur;
  };
  std::atomic<bool> captureArmed_{false};
  std::array<double, 128> heldSince_{};  // audio thread; <0 = not held
  juce::AbstractFifo captureFifo_{512};
  std::array<Captured, 512> captureBuf_{};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NvsProcessor)
};
