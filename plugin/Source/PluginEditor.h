#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"
#include "AudioImport.h"
#include "MicrophoneCapture.h"

// The plugin window: an HTML/JS interface (plugin/ui, compiled in) hosted in a
// WebBrowserComponent. The page calls native functions (getSong, setSong,
// getPlan, ...) and receives a "status" event ~30 times a second.
class NovoxEditor : public juce::AudioProcessorEditor, private juce::Timer, public juce::FileDragAndDropTarget {
 public:
  explicit NovoxEditor(NovoxProcessor&);
  ~NovoxEditor() override;
  void resized() override;
  void paint(juce::Graphics&) override;
  bool isInterestedInFileDrag(const juce::StringArray&) override;
  void filesDropped(const juce::StringArray&, int, int) override;

 private:
  void timerCallback() override;
  juce::WebBrowserComponent::Options makeOptions();
  static std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

  void* regionDrop_ = nullptr;
  void* selectAllMonitor_ = nullptr;
  MicrophoneCapture microphone_;
  juce::String microphoneError_;
  bool microphonePending_ = false;
  AudioImport audioImport_;
  std::unique_ptr<juce::FileChooser> fileChooser_;
  juce::File songFile_;
  juce::MemoryBlock pendingAudio_;
  juce::String pendingName_;
  NovoxProcessor& proc_;
  juce::WebBrowserComponent browser_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NovoxEditor)
};
