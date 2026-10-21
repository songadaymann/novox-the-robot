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
  // Development hook (NovoxPluginTests): editors made while it is set act as on a Windows PC
  // without the WebView2 runtime. The plugin never sets it.
  static inline bool testWithoutWebView = false;

 private:
  void timerCallback() override;
  juce::WebBrowserComponent::Options makeOptions();
  static std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);
  void emit(const juce::Identifier& event, const juce::var& value);
  void showWebViewMissing();

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
  // Shown instead of the page when Windows has no WebView2 runtime (JUCE would fall back to
  // Internet Explorer, which cannot run it).
  juce::Label webViewMissing_;
  juce::HyperlinkButton webViewDownload_;
  // The page; none without WebView2. Last, so it goes first: its native functions use the rest.
  std::unique_ptr<juce::WebBrowserComponent> browser_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NovoxEditor)
};
