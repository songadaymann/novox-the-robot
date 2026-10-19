#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>

// Owns input only: recording never monitors the mic through the speaker/DAW.
class MicrophoneCapture : private juce::AudioIODeviceCallback {
public:
  ~MicrophoneCapture() { cancel(); }
  juce::String start();
  juce::MemoryBlock stop();
  void cancel();
  juce::var status() const;
  static juce::MemoryBlock encode(const juce::AudioBuffer<float>&, int samples, double rate);
private:
  void closeInput();
  void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                       const juce::AudioIODeviceCallbackContext&) override;
  void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
  void audioDeviceStopped() override {}
  juce::AudioDeviceManager devices_;
  juce::AudioBuffer<float> recording_;
  double rate_ = 48000;
  std::atomic<int> position_{0};
  std::atomic<bool> active_{false}, full_{false};
  std::atomic<float> level_{0};
  juce::String error_;
};
