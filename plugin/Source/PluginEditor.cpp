#include "PluginEditor.h"

#include "BinaryData.h"

namespace {

juce::String mimeFor(const juce::String& file) {
  if (file.endsWith(".html")) return "text/html";
  if (file.endsWith(".js")) return "text/javascript";
  if (file.endsWith(".css")) return "text/css";
  if (file.endsWith(".svg")) return "image/svg+xml";
  if (file.endsWith(".png")) return "image/png";
  return "application/octet-stream";
}

using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;
using Args = juce::Array<juce::var>;

}  // namespace

std::optional<juce::WebBrowserComponent::Resource> NovoxEditor::getResource(const juce::String& url) {
  juce::String path = url.fromFirstOccurrenceOf("/", false, false);
  if (path.isEmpty() || path == "/") path = "index.html";
  path = path.upToFirstOccurrenceOf("?", false, false).fromLastOccurrenceOf("/", false, false);
  for (int i = 0; i < BinaryData::namedResourceListSize; ++i) {
    if (path == BinaryData::originalFilenames[i]) {
      int size = 0;
      const char* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size);
      std::vector<std::byte> bytes(static_cast<size_t>(size));
      std::memcpy(bytes.data(), data, static_cast<size_t>(size));
      return juce::WebBrowserComponent::Resource{std::move(bytes), mimeFor(path)};
    }
  }
  return std::nullopt;
}

juce::WebBrowserComponent::Options NovoxEditor::makeOptions() {
  auto& p = proc_;
  return juce::WebBrowserComponent::Options{}
      .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
      .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}.withUserDataFolder(
          juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("NovoxTheRobot")))
      .withNativeIntegrationEnabled()
      .withResourceProvider([](const juce::String& url) { return getResource(url); })
      .withNativeFunction("getSong", [&p](const Args&, Completion done) { done(p.getSongJson()); })
      .withNativeFunction("setSong",
                          [&p](const Args& a, Completion done) {
                            if (a.size() > 0) p.setSongJson(a[0].toString());
                            done(juce::var());
                          })
      .withNativeFunction("getPlan",
                          [&p](const Args&, Completion done) {
                            juce::String error;
                            done(p.planJson(error));
                          })
      .withNativeFunction("getVoices",
                          [&p](const Args&, Completion done) {
                            juce::Array<juce::var> names;
                            for (const auto& n : p.voiceNames()) names.add(n);
                            done(names);
                          })
      .withNativeFunction("setCapture",
                          [&p](const Args& a, Completion done) {
                            p.setCaptureArmed(a.size() > 0 && static_cast<bool>(a[0]));
                            done(juce::var());
                          })
      .withNativeFunction("setTransport",
                          [&p](const Args& a, Completion done) {
                            p.setPreviewPlaying(a.size() > 0 && static_cast<bool>(a[0]));
                            done(juce::var());
                          })
      .withNativeFunction("setTempo",
                          [&p](const Args& a, Completion done) {
                            if (a.size() > 0) p.setPreviewTempo(static_cast<double>(a[0]));
                            done(juce::var());
                          })
      .withNativeFunction("importFile", [&p](const Args& a, Completion done) {
        juce::MemoryBlock data;
        if (a.size() > 0) data.fromBase64Encoding(a[0].toString());
        juce::String error;
        juce::String score = p.importFile(data, a.size() > 1 ? a[1].toString() : juce::String(), error);
        done(score);
      });
}

NovoxEditor::NovoxEditor(NovoxProcessor& p) : juce::AudioProcessorEditor(p), proc_(p), browser_(makeOptions()) {
  addAndMakeVisible(browser_);
  browser_.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
  setResizable(true, true);
  setResizeLimits(760, 520, 2400, 1600);
  setSize(1000, 660);
  startTimerHz(30);
}

NovoxEditor::~NovoxEditor() { stopTimer(); }

void NovoxEditor::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xffe8e8e8)); }

void NovoxEditor::resized() { browser_.setBounds(getLocalBounds()); }

void NovoxEditor::timerCallback() {
  const auto s = proc_.getStatus();
  auto* o = new juce::DynamicObject();
  o->setProperty("beats", s.beats);
  o->setProperty("bpm", s.bpm);
  o->setProperty("playing", s.playing);
  o->setProperty("host", s.hostTransport);
  o->setProperty("capture", s.capture);
  o->setProperty("rendering", s.rendering);
  o->setProperty("error", s.error);
  o->setProperty("songVersion", s.songVersion);
  browser_.emitEventIfBrowserIsVisible("status", juce::var(o));
}
