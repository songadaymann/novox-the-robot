// Hosts NovoxProcessor with a fake DAW playhead and checks transport sync,
// tempo changes, MIDI capture and state save/restore.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#include "../Source/PluginProcessor.h"
#include "BinaryData.h"
#include "novox.h"
#include "whisper.h"
#include "../Source/AudioImport.h"
#include "../Source/MicrophoneCapture.h"
#include "../Source/HostMmc.h"
#include "../Source/Platform.h"
#include "../Source/PluginEditor.h"
#if JUCE_MAC
bool testNovoxRegionDrops();
#endif

namespace {

int failures = 0, checks = 0;
#define CHECK(c)                                                                            \
  do {                                                                                      \
    ++checks;                                                                               \
    if (!(c)) {                                                                             \
      ++failures;                                                                           \
      std::cerr << "  FAIL line " << __LINE__ << ": " #c "\n";                              \
    }                                                                                       \
  } while (0)

struct FakePlayHead : juce::AudioPlayHead {
  double ppq = 0, bpm = 120;
  bool playing = true;
  bool beats = true;  // false: a host that gives no beat position or tempo (MuseScore 4)
  juce::Optional<int64_t> sampleTime;
  juce::Optional<double> seconds, editOrigin;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo p;
    if (beats) {
      p.setPpqPosition(ppq);
      p.setBpm(bpm);
    }
    if (sampleTime) p.setTimeInSamples(*sampleTime);
    if (seconds) p.setTimeInSeconds(*seconds);
    if (editOrigin) p.setEditOriginTime(*editOrigin);
    p.setIsPlaying(playing);
    return p;
  }
};

void pump(double seconds) {
  // Let the render thread finish and the message loop run.
  auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000;
  while (juce::Time::getMillisecondCounterHiRes() < until)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

void waitForRender(NovoxProcessor& p) {
  pump(0.05);
  for (int i = 0; i < 400 && p.getStatus().rendering; ++i) pump(0.02);
  pump(0.05);
}

// Plays `seconds` of audio from beat `startBeat` in blocks of `block`.
std::vector<float> play(NovoxProcessor& p, FakePlayHead& ph, double startBeat, double seconds, int block = 512) {
  const double sr = 48000;
  std::vector<float> out;
  juce::AudioBuffer<float> buf(2, block);
  juce::MidiBuffer midi;
  double samples = 0;
  while (out.size() < static_cast<size_t>(seconds * sr)) {
    ph.ppq = startBeat + samples / sr * ph.bpm / 60.0;
    p.processBlock(buf, midi);
    for (int i = 0; i < block; ++i) out.push_back(buf.getSample(0, i));
    samples += block;
  }
  return out;
}

float rms(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  double s = 0;
  to = std::min(to, v.size());
  for (size_t i = from; i < to; ++i) s += v[i] * v[i];
  return static_cast<float>(std::sqrt(s / std::max<size_t>(1, to - from)));
}

// Recognition experiments (tools/import_lab.py asr): named changes to the plugin's whisper.cpp
// settings, combinable with commas ("beam5,prompt"). The plugin itself always runs the defaults.
bool applyWhisperVariant(whisper_full_params& p, const juce::String& name) {
  if (name == "greedy") return true;  // the plugin's own settings, timed
  if (name == "beam5") { p.strategy = WHISPER_SAMPLING_BEAM_SEARCH; p.beam_search.beam_size = 5; return true; }
  if (name == "prompt") { p.initial_prompt = "Oh, oh, oh. La, la, la. Ooh, yeah."; return true; }
  if (name == "nst") { p.suppress_nst = true; return true; }
  if (name == "nofallback") { p.temperature_inc = 0; return true; }  // keep the T = 0 decode even if repetitive
  if (name.startsWith("entropy=")) {  // repetition test: lower = only near-loops resample at T > 0 (plugin and whisper.cpp 2.4)
    p.entropy_thold = name.fromFirstOccurrenceOf("=", false, false).getFloatValue();
    return p.entropy_thold > 0;
  }
  if (name == "maxlen1") { p.max_len = 1; p.split_on_word = true; return true; }  // one word per segment
  if (name == "maxtok16") { p.max_tokens = 16; return true; }  // end segments early, re-seeking more often
  return false;
}

// A 44.1 kHz mono AIFF laid out like the temporary file Logic hands a plugin for a dragged region:
// a COMT creator comment (with Logic's leading space) first, then COMM, SSND, a private chunk and
// the MARK chunk last, each marker name a pascal string padded to an even size. aifc writes the
// AIFC form (FVER first, "NONE" compression), whose chunks JUCE stops reading after SSND.
juce::MemoryBlock logicAiff(const juce::AudioBuffer<float>& audio, const juce::StringArray& markers,
                            const juce::String& creator, bool aifc = false) {
  juce::MemoryOutputStream chunks;
  const auto chunk = [&chunks](const char* id, const juce::MemoryOutputStream& body) {
    chunks.write(id, 4); chunks.writeIntBigEndian(static_cast<int>(body.getDataSize()));
    chunks.write(body.getData(), body.getDataSize());
    if (body.getDataSize() % 2) chunks.writeByte(0);
  };
  if (aifc) { juce::MemoryOutputStream fver; fver.writeIntBigEndian(static_cast<int>(0xa2805140)); chunk("FVER", fver); }
  if (creator.isNotEmpty()) {
    const auto text = " Creator: " + creator;
    juce::MemoryOutputStream comt;
    comt.writeShortBigEndian(1); comt.writeIntBigEndian(0); comt.writeShortBigEndian(0);
    comt.writeShortBigEndian(static_cast<short>(text.length())); comt.write(text.toRawUTF8(), static_cast<size_t>(text.length()));
    chunk("COMT", comt);
  }
  const int frames = audio.getNumSamples();
  juce::MemoryOutputStream comm;
  comm.writeShortBigEndian(1); comm.writeIntBigEndian(frames); comm.writeShortBigEndian(16);
  const unsigned char rate44100[10] = {0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0};  // 80-bit extended
  comm.write(rate44100, sizeof(rate44100));
  if (aifc) { comm.write("NONE", 4); comm.writeByte(0); comm.writeByte(0); }  // empty name, padded
  chunk("COMM", comm);
  juce::MemoryOutputStream ssnd;
  ssnd.writeIntBigEndian(0); ssnd.writeIntBigEndian(0);
  for (int i = 0; i < frames; ++i)
    ssnd.writeShortBigEndian(static_cast<short>(juce::roundToInt(juce::jlimit(-1.0f, 1.0f, audio.getSample(0, i)) * 32767)));
  chunk("SSND", ssnd);
  juce::MemoryOutputStream lgwv; lgwv.writeIntBigEndian(0); chunk("LGWV", lgwv);
  if (!markers.isEmpty()) {
    juce::MemoryOutputStream mark;
    mark.writeShortBigEndian(static_cast<short>(markers.size()));
    for (int i = 0; i < markers.size(); ++i) {
      const auto& name = markers[i];
      mark.writeShortBigEndian(static_cast<short>(i + 1)); mark.writeIntBigEndian(name.startsWith("End") ? frames : 0);
      mark.writeByte(static_cast<char>(name.length())); mark.write(name.toRawUTF8(), static_cast<size_t>(name.length()));
      if (name.length() % 2 == 0) mark.writeByte(0);
    }
    chunk("MARK", mark);
  }
  juce::MemoryOutputStream file;
  file.write("FORM", 4); file.writeIntBigEndian(static_cast<int>(chunks.getDataSize()) + 4);
  file.write(aifc ? "AIFC" : "AIFF", 4); file.write(chunks.getData(), chunks.getDataSize());
  return file.getMemoryBlock();
}

// Byte offset of the first four characters of `id` at or after `from` in an audio file, or -1.
int chunkAt(const juce::MemoryBlock& data, const char* id, size_t from = 12) {
  for (size_t i = from; i + 4 <= data.getSize(); ++i)
    if (std::memcmp(static_cast<const char*>(data.getData()) + i, id, 4) == 0) return static_cast<int>(i);
  return -1;
}

void testRenderStateChanges() {
  NovoxProcessor processor;
  processor.setRateAndBufferSizeDetails(48000, 512);
  processor.prepareToPlay(48000, 512);
  Song song;
  song.useNoteLyrics = true;
  song.lyrics = "the pane before it is mirrored";
  song.notes.add({0, 1, 60, "la"});
  song.notes.add({1, 1, 64, "love"});
  processor.setSongJson(song.toJson());
  waitForRender(processor);
  CHECK(!processor.getStatus().rendering && processor.renderedVersion() == processor.getStatus().songVersion);
  juce::String error;
  const auto originalWave = processor.exportWave(error);
  CHECK(originalWave.getSize() > 100 && error.isEmpty());
  auto before = processor.getStatus();

  // Pinned note lyrics are the score. Updating the pane, paste provenance or pitch-review
  // metadata still publishes the saved state, without rerendering/cancelling its audio.
  song.lyrics = "la love";
  processor.setSongJson(song.toJson());
  CHECK(processor.getStatus().songVersion == before.songVersion + 1);
  CHECK(!processor.getStatus().rendering && processor.renderedVersion() == processor.getStatus().songVersion);
  CHECK(Song::fromJson(processor.getSongJson()).lyrics == "la love");
  CHECK(processor.auditionSong(0, 2, true, error));
  song.transcriptions = juce::JSON::parse(R"([{"kind":"melody-paste","startBeat":0,"beats":2}])");
  song.keyCorrection = juce::JSON::parse(R"({"root":0,"scale":"major","mode":"gentle"})");
  processor.setSongJson(song.toJson());
  pump(.08);
  CHECK(processor.getStatus().rendersStarted == before.rendersStarted);
  CHECK(processor.getStatus().rendersCancelled == before.rendersCancelled);
  CHECK(processor.getStatus().songVersion == before.songVersion + 2 && processor.renderedVersion() == processor.getStatus().songVersion);
  CHECK(processor.reviewing());
  CHECK(Song::fromJson(processor.getSongJson()).transcriptions[0]["kind"].toString() == "melody-paste");
  const auto mirroredWave = processor.exportWave(error);
  CHECK(mirroredWave == originalWave && error.isEmpty());
  processor.stopAudition();

  // A real note/voice/global-lyric change still invalidates and replaces the audio.
  song.notes.getReference(0).pitch = 67;
  processor.setSongJson(song.toJson());
  CHECK(processor.getStatus().rendering && processor.renderedVersion() != processor.getStatus().songVersion);
  waitForRender(processor);
  CHECK(processor.getStatus().rendersStarted > before.rendersStarted && processor.renderedVersion() == processor.getStatus().songVersion);
  CHECK(processor.exportWave(error) != originalWave);
  before = processor.getStatus();
  song.voice = "robot";
  processor.setSongJson(song.toJson());
  waitForRender(processor);
  CHECK(processor.getStatus().rendersStarted > before.rendersStarted && processor.renderedVersion() == processor.getStatus().songVersion);
  song.useNoteLyrics = false;
  song.lyrics = "sing me";
  processor.setSongJson(song.toJson());
  waitForRender(processor);
  before = processor.getStatus();
  song.lyrics = "love you";
  processor.setSongJson(song.toJson());
  waitForRender(processor);
  CHECK(processor.getStatus().rendersStarted > before.rendersStarted && processor.renderedVersion() == processor.getStatus().songVersion);

  // Metadata arriving after synthesis has started must not obsolete that render. This is
  // dense singing so the test still catches the race after silent-section acceleration.
  song.notes.clear(); song.useNoteLyrics = true; song.voice = "choir";
  for (int i = 0; i < 64; ++i) song.notes.add({static_cast<double>(i), 1, 55 + i % 12, "la"});
  before = processor.getStatus();
  const double denseRenderStarted = juce::Time::getMillisecondCounterHiRes();
  processor.setSongJson(song.toJson());
  for (int i = 0; i < 100 && processor.getStatus().rendersStarted == before.rendersStarted; ++i) pump(.005);
  CHECK(processor.getStatus().rendering && processor.getStatus().rendersStarted == before.rendersStarted + 1);
  for (int i = 0; i < 12; ++i) {
    song.lyrics = "display update " + juce::String(i);
    processor.setSongJson(song.toJson());
  }
  CHECK(processor.renderedVersion() != processor.getStatus().songVersion);
  // This deliberately dense eight-voice passage can take longer than the ordinary short-song
  // fixture on a busy host. Keep a bounded wait without weakening the convergence assertions.
  const double renderDeadline = juce::Time::getMillisecondCounterHiRes() + 30000;
  while (processor.getStatus().rendering && juce::Time::getMillisecondCounterHiRes() < renderDeadline) pump(.02);
  pump(.05);
  const auto denseStatus = processor.getStatus();
  std::cout << "Dense choir render: " << (juce::Time::getMillisecondCounterHiRes() - denseRenderStarted) / 1000.0
            << " s wall; " << denseStatus.rendersStarted - before.rendersStarted << " started, "
            << denseStatus.rendersCancelled - before.rendersCancelled << " cancelled; audio edit "
            << processor.renderedVersion() << "/" << denseStatus.songVersion
            << (denseStatus.rendering ? " (still rendering)" : " (complete)") << std::endl;
  CHECK(!processor.getStatus().rendering && processor.renderedVersion() == processor.getStatus().songVersion);
  CHECK(processor.getStatus().rendersStarted == before.rendersStarted + 1);
  CHECK(processor.getStatus().rendersCancelled == before.rendersCancelled);
  CHECK(Song::fromJson(processor.getSongJson()).lyrics == "display update 11");
  CHECK(processor.exportWave(error).getSize() > 100);
  CHECK(processor.auditionSong(1, 2, false, error));
  processor.stopAudition();

  // Tempo and sample-rate changes remain separate render requests even when the score
  // and its revision are unchanged. Run one block so the preview transport reports BPM.
  song.notes.clear(); song.voice = "std"; song.notes.add({0, 1, 60, "la"});
  processor.setSongJson(song.toJson()); waitForRender(processor);
  before = processor.getStatus();
  processor.setPreviewTempo(90);
  juce::AudioBuffer<float> buffer(2, 512); juce::MidiBuffer midi;
  processor.processBlock(buffer, midi);
  waitForRender(processor);
  CHECK(processor.getStatus().rendersStarted > before.rendersStarted && processor.getStatus().bpm == 90);
  CHECK(processor.renderedVersion() == processor.getStatus().songVersion);
  before = processor.getStatus();
  processor.setRateAndBufferSizeDetails(44100, 512); processor.prepareToPlay(44100, 512);
  waitForRender(processor);
  CHECK(processor.getStatus().rendersStarted > before.rendersStarted && processor.renderedVersion() == processor.getStatus().songVersion);
  const auto rateWave = processor.exportWave(error);
  juce::WavAudioFormat format;
  const std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::MemoryInputStream(rateWave, false), true));
  CHECK(reader && reader->sampleRate == 44100);
}

// The editor's page, or nullptr where it shows the WebView2-missing message instead.
juce::WebBrowserComponent* webBrowserOf(juce::Component& editor) {
  for (auto* child : editor.getChildren())
    if (auto* browser = dynamic_cast<juce::WebBrowserComponent*>(child)) return browser;
  return nullptr;
}

// --webview-smoke: the editor's page in its real web view (on Windows, WebView2; the editor checks
// in main only see that the runtime was found). The page loads from the resource provider, boots
// with its song and voices from native functions, and gets the editor's status. The editor is on
// screen for a few seconds, so this runs only when asked for (Windows CI).
void testWebView() {
  // What the page says once it has booted (app.js keeps its state in window.__novox).
  const juce::String script = R"js((() => {
    const app = window.__novox;
    if (!window.__JUCE__) return "waiting: no native bridge at " + location.href;
    if (!app || !app.S.loaded || !app.S.status?.platform) return "waiting: the page is booting";
    const S = app.S;
    return JSON.stringify({ platform: S.status.platform, audio: S.status.importTypes?.audio || [], voices: S.voices,
      accept: document.getElementById("file").accept, agent: navigator.userAgent });
  })())js";
  juce::String answer = "no answer";
  bool asking = false;
  NovoxProcessor processor;
  std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
  auto* browser = webBrowserOf(*editor);
  CHECK(browser != nullptr);
  if (browser) {
    editor->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
    editor->setVisible(true);
    const auto until = juce::Time::getMillisecondCounterHiRes() + 60000;
    while (!answer.startsWith("{") && juce::Time::getMillisecondCounterHiRes() < until) {
      if (!asking) {
        asking = true;
        browser->evaluateJavascript(script, [&answer, &asking](juce::WebBrowserComponent::EvaluationResult result) {
          asking = false;
          if (const auto* value = result.getResult()) answer = value->toString();
          else if (const auto* error = result.getError()) answer = "script error: " + error->message;
        });
      }
      pump(0.1);
    }
  }
  std::cout << "web view: " << answer << std::endl;
  const auto page = juce::JSON::parse(answer);
  const auto strings = [](const juce::var& list) {
    juce::StringArray out;
    if (const auto* items = list.getArray())
      for (const auto& item : *items) out.add(item.toString());
    return out;
  };
  CHECK(page["platform"].toString() == NovoxPlatform::name());
  CHECK(strings(page["audio"]) == AudioImport::extensions());
  CHECK(strings(page["voices"]).contains("robot"));
  const auto accept = page["accept"].toString();
  for (const auto& extension : AudioImport::extensions()) CHECK(accept.contains("." + extension));
  CHECK(accept.contains(".m4a") == AudioImport::extensions().contains("m4a"));
  editor.reset();
  pump(0.2);  // with answer and asking still here for a late reply
}

}  // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI init;
  if (argc >= 2 && juce::String(argv[1]) == "--render-state-smoke") {
    testRenderStateChanges();
    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures ? 1 : 0;
  }
  if (argc >= 2 && juce::String(argv[1]) == "--webview-smoke") {
    testWebView();
    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures ? 1 : 0;
  }
#if JUCE_MAC
  CHECK(testNovoxRegionDrops());
#endif
  // MMC for the DAW's transport (HostMmc): the bytes, a virtual port read back, the project state.
  {
    using Bytes = std::vector<uint8_t>;
    const HostMmc::FrameRate fps25{25}, fps30{30}, df{30, true, true};
    CHECK(HostMmc::commandMessage(0x02) == (Bytes{0xf0, 0x7f, 0x7f, 0x06, 0x02, 0xf7}));
    CHECK(HostMmc::commandMessage(0x01) == (Bytes{0xf0, 0x7f, 0x7f, 0x06, 0x01, 0xf7}));
    // Logic's bar 1 is 01:00:00:00; 2.5 s on at 25 fps is 62.5 frames, sent as 01:00:02:13 (rate bits
    // 01): whole frames, to the nearest, as Full Frame and as MMC Locate (subframes 0).
    CHECK(HostMmc::fullFrameMessage(HostMmc::hourFrames(1, fps25), 2.5, fps25) == (Bytes{0xf0, 0x7f, 0x7f, 0x01, 0x01, 0x21, 0, 2, 13, 0xf7}));
    CHECK(HostMmc::locateMessage(HostMmc::hourFrames(1, fps25), 2.5, fps25) ==
          (Bytes{0xf0, 0x7f, 0x7f, 0x06, 0x44, 0x06, 0x01, 0x21, 0, 2, 13, 0, 0xf7}));
    CHECK(HostMmc::locateMessage(0, 3723.5, fps30) == (Bytes{0xf0, 0x7f, 0x7f, 0x06, 0x44, 0x06, 0x01, 0x61, 2, 3, 15, 0, 0xf7}));
    // 29.97 drop-frame: an hour is 107892 frames; 61 s on, frame 1828.17 of the hour, is labelled
    // 01:01:01;00 (minute 1 starts at ;02; rate bits 10).
    CHECK(HostMmc::hourFrames(1, df) == 107892);
    CHECK(HostMmc::locateMessage(HostMmc::hourFrames(1, df), 61, df) ==
          (Bytes{0xf0, 0x7f, 0x7f, 0x06, 0x44, 0x06, 0x01, 0x41, 1, 1, 0, 0, 0xf7}));
    CHECK(HostMmc::locateMessage(0, NAN, fps30).empty() && HostMmc::fullFrameMessage(NAN, 0, fps30).empty());
    // One shared port per process (the DAW's), opened only by a plugin in a DAW.
    {
      auto mmc = HostMmc::open();
#if JUCE_MAC
      CHECK(mmc != nullptr && HostMmc::open() == mmc);
#endif
      NovoxProcessor notInDaw;
      CHECK(!notInDaw.inDaw() && notInDaw.mmc() == nullptr);
    }
#if JUCE_MAC
    // A port of its own, read back through CoreMIDI. Not MMC (a running DAW listening to every input
    // would act on it): a non-commercial SysEx.
    struct Collector : juce::MidiInputCallback {
      std::mutex m;
      std::vector<Bytes> got;
      void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) override {
        std::lock_guard<std::mutex> lock(m);
        got.emplace_back(msg.getRawData(), msg.getRawData() + msg.getRawDataSize());
      }
    } collector;
    const auto name = "Novox loopback test " + juce::String(juce::Random::getSystemRandom().nextInt(1 << 30));
    auto port = HostMmc::openNamed(name);
    CHECK(port != nullptr);
    std::unique_ptr<juce::MidiInput> input;
    for (int wait = 0; wait < 40 && port && !input; ++wait) {
      for (const auto& d : juce::MidiInput::getAvailableDevices())
        if (d.name == name) input = juce::MidiInput::openDevice(d.identifier, &collector);
      if (!input) juce::MessageManager::getInstance()->runDispatchLoopUntil(50);  // device-list notifications
    }
    CHECK(input != nullptr);
    const Bytes probe{0xf0, 0x7d, 0x4e, 0x6f, 0x76, 0xf7};
    if (port && input) {
      input->start();
      juce::Thread::sleep(100);
      CHECK(port->sendRaw(probe) && port->sendRaw(probe));
      for (int wait = 0; wait < 40; ++wait) {
        { std::lock_guard<std::mutex> lock(collector.m); if (collector.got.size() >= 2) break; }
        juce::Thread::sleep(50);
      }
      std::lock_guard<std::mutex> lock(collector.m);
      CHECK(collector.got.size() == 2 && collector.got[0] == probe && collector.got[1] == probe);
    }
    if (input) input->stop();
#endif
    // What the page learned about the project's MMC goes with the DAW project, not a .novox file.
    NovoxProcessor learned;
    learned.setMmcState({true, -1.5, 25});
    juce::MemoryBlock project, file;
    learned.getStateInformation(project);
    learned.writeState(file, false);
    NovoxProcessor reopened;
    reopened.setStateInformation(project.getData(), static_cast<int>(project.getSize()));
    const auto m = reopened.mmcState();
    CHECK(m.works && m.offset == -1.5 && m.rate == 25);
    CHECK(project.toString().contains("\"mmc\"") && !file.toString().contains("\"mmc\""));
    NovoxProcessor fromFile;
    fromFile.setMmcState({true, 2, 30});
    fromFile.setStateInformation(file.getData(), static_cast<int>(file.getSize()));
    CHECK(fromFile.mmcState().works && fromFile.mmcState().offset == 2);  // a song file keeps the project's
  }
  {
    NovoxProcessor phrase;phrase.prepareToPlay(48000,512);
    juce::AudioBuffer<float> recording(1,16000);recording.clear();
    for(int i=1600;i<2400;++i) recording.setSample(0,i,.2f);
    auto wav=MicrophoneCapture::encode(recording,16000,16000);juce::String error;
    CHECK(phrase.auditionAudio(wav,error,.1,.15,true));
    juce::AudioBuffer<float> block(2,512);juce::MidiBuffer midi;
    bool continuous=true;
    for(int n=0;n<20;++n) {phrase.processBlock(block,midi);for(int i=0;i<512;++i) if(std::fabs(block.getSample(0,i)-.2f)>.001f) continuous=false;}
    CHECK(continuous); // Repeatedly crosses both in-block and between-block loop boundaries.
    phrase.stopAudition();
    CHECK(!phrase.auditionAudio(wav,error,.3,.2,true));
    CHECK(!phrase.auditionAudio(wav,error,2,3,true));
  }
  {
    NovoxProcessor original;original.prepareToPlay(48000,512);
    auto song=Song::defaultSong();song.notes.clear();song.notes.add({1,1,64,"hello"});
    song.transcriptions=juce::JSON::parse(R"([{"recordingId":"source-test"}])");original.setSongJson(song.toJson());
    juce::AudioBuffer<float> silence(1,16000);silence.clear();auto wav=MicrophoneCapture::encode(silence,16000,16000);
    auto* source=new juce::DynamicObject();source->setProperty("original","data:audio/wav;base64,"+juce::Base64::toBase64(wav.getData(),wav.getSize()));
    source->setProperty("bpm",120);source->setProperty("seconds",1);source->setProperty("waveform",juce::var(juce::Array<juce::var>{0,0}));
    const juce::var sourceValue(source);
    CHECK(original.storeSource("source-test",sourceValue));
    CHECK(!original.getSongJson().contains("data:audio"));
    juce::MemoryBlock state;original.getStateInformation(state);
    auto songFile=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Novox-state-test-"+juce::Uuid().toString()+".novox");
    CHECK(songFile.replaceWithData(state.getData(),state.getSize()));juce::MemoryBlock fromDisk;CHECK(songFile.loadFileAsData(fromDisk));songFile.deleteFile();
    NovoxProcessor restored;restored.prepareToPlay(48000,512);restored.setStateInformation(fromDisk.getData(),static_cast<int>(fromDisk.getSize()));
    CHECK(restored.getSongJson()==original.getSongJson());
    CHECK(restored.getSource("source-test")["original"]==original.getSource("source-test")["original"]);
    CHECK(!restored.storeSource("",sourceValue));
    juce::String error;auto midiBytes=restored.exportMidi(error);juce::MemoryInputStream midiStream(midiBytes,false);juce::MidiFile midi;
    CHECK(midi.readFrom(midiStream) && midi.getTimeFormat()==960 && midi.getNumTracks()==1);
    bool found=false;for(int i=0;i<midi.getTrack(0)->getNumEvents();++i) {const auto& event=midi.getTrack(0)->getEventPointer(i)->message;if(event.isNoteOn() && event.getNoteNumber()==64 && event.getTimeStamp()==960) found=true;}
    CHECK(found);
    waitForRender(restored);auto waveBytes=restored.exportWave(error);juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::MemoryInputStream(waveBytes,false),true));
    CHECK(reader && reader->sampleRate==48000 && reader->numChannels==1 && reader->lengthInSamples>24000);
  }
  // Every module an embedded script imports ("./name.js") is embedded too (NovoxUiData in
  // plugin/CMakeLists.txt): a missing one leaves the editor blank in every format.
  {
    const auto embedded = [](const juce::String& file) {
      for (int i = 0; i < BinaryData::namedResourceListSize; ++i) if (file == BinaryData::originalFilenames[i]) return true;
      return false;
    };
    int modules = 0;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i) {
      if (!juce::String(BinaryData::originalFilenames[i]).endsWith(".js")) continue;
      int size = 0;
      const char* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size);
      const auto text = juce::String::fromUTF8(data, size);
      for (const auto* quote : {"\"./", "'./"})
        for (int at = text.indexOf(quote); at >= 0; at = text.indexOf(at + 1, quote)) {
          const auto name = text.substring(at + 3).upToFirstOccurrenceOf(juce::String::charToString(quote[0]), false, false);
          if (!name.endsWith(".js")) continue;
          ++modules;
          if (!embedded(name)) std::cerr << "  not embedded: " << name << "\n";
          CHECK(embedded(name));
        }
    }
    CHECK(modules >= 6 && embedded("song-merge.js"));
  }
  { auto song = Song::defaultSong();
    song.useNoteLyrics = true;
    CHECK(Song::fromJson(song.toJson()).useNoteLyrics);
    CHECK(!juce::JSON::parse(song.toScoreJson(120)).hasProperty("lyrics"));
    song.useNoteLyrics = false;
    CHECK(juce::JSON::parse(song.toScoreJson(120)).getProperty("lyrics", "").toString()==song.lyrics);
    song.keyCorrection = juce::JSON::parse(R"({"root":5,"scale":"major","mode":"gentle"})");
    CHECK(juce::JSON::toString(Song::fromJson(song.toJson()).keyCorrection) == juce::JSON::toString(song.keyCorrection));
    // One record per audio import, oldest first; the source recordings they refer to, once each.
    song.transcriptions = juce::JSON::parse(R"([{"recordingId":"take-a","algorithm":"singing-v1","transcriptWords":[{"word":"Can","start":0,"end":0.6}],"unassignedWords":[{"wordIndex":0,"reason":"no voiced overlap"}]},)"
                                            R"({"recordingId":"take-b","startBeat":8.25,"beats":12.5},{"recordingId":"take-a"},{"startBeat":2}])");
    CHECK(juce::JSON::toString(Song::fromJson(song.toJson()).transcriptions) == juce::JSON::toString(song.transcriptions));
    CHECK(song.recordingIds() == juce::StringArray({"take-a", "take-b"}));
    // A song saved before region-by-region import had its one import in "transcription".
    const auto legacy = Song::fromJson(R"({"lyrics":"","notes":[],"transcription":{"recordingId":"old","algorithm":"singing-v1"}})");
    CHECK(legacy.transcriptions.size() == 1 && legacy.transcriptions[0]["algorithm"].toString() == "singing-v1");
    CHECK(legacy.recordingIds() == juce::StringArray({"old"}));
    CHECK(legacy.toJson().contains("\"transcriptions\"") && !legacy.toJson().contains("\"transcription\""));
    CHECK(!Song::fromJson(R"({"notes":[]})").toJson().contains("transcription") && Song::fromJson(R"({"notes":[]})").recordingIds().isEmpty());
  }

  // Region-by-region import: a song built from several imported recordings. The DAW state and a
  // .novox file keep every source recording the song refers to, each once (not one an Undo took
  // back), and load them all; a state saved before then (one sourceId/source) still loads; and
  // Compare > Original plays pieces of two recordings in a row, or mixed where they overlap.
  {
    const auto sourceOf = [](float level, int samples) {
      juce::AudioBuffer<float> tone(1, samples);
      for (int i = 0; i < samples; ++i) tone.setSample(0, i, level);
      const auto wav = MicrophoneCapture::encode(tone, samples, 16000);
      auto* source = new juce::DynamicObject();
      source->setProperty("original", "data:audio/wav;base64," + juce::Base64::toBase64(wav.getData(), wav.getSize()));
      source->setProperty("bpm", 120); source->setProperty("seconds", samples / 16000.0); source->setProperty("startBeat", 0);
      source->setProperty("waveform", juce::var(juce::Array<juce::var>{level, level}));
      return juce::var(source);
    };
    const auto count = [](const juce::String& text, const juce::String& part) {
      int n = 0; for (int at = text.indexOf(part); at >= 0; at = text.indexOf(at + 1, part)) ++n; return n;
    };
    NovoxProcessor regions; regions.setRateAndBufferSizeDetails(48000, 480); regions.prepareToPlay(48000, 480);
    auto song = Song::defaultSong(); song.notes.clear(); song.useNoteLyrics = true;
    song.notes.add({4, 1, 60, "one"}); song.notes.add({12, 1, 62, "two"}); song.notes.add({20, 1, 64, "three"});
    song.transcriptions = juce::JSON::parse(R"([{"recordingId":"region-a","startBeat":4,"beats":2},)"
                                            R"({"recordingId":"region-b","startBeat":12,"beats":2},{"recordingId":"region-c","startBeat":20,"beats":2}])");
    regions.setSongJson(song.toJson());
    CHECK(regions.storeSource("region-a", sourceOf(.1f, 16000)) && regions.storeSource("region-b", sourceOf(.2f, 16000)) &&
          regions.storeSource("region-c", sourceOf(.3f, 16000)));
    CHECK(regions.storeSource("undone", sourceOf(.4f, 16000)));  // an import Undo took back
    juce::MemoryBlock state; regions.getStateInformation(state);
    const auto saved = juce::JSON::parse(state.toString());
    CHECK(saved["sources"].size() == 3 && !saved.hasProperty("sourceId") && !saved.hasProperty("source"));
    CHECK(count(state.toString(), "data:audio/wav;base64,") == 3);
    for (const bool fromFile : {false, true}) {
      juce::MemoryBlock data = state;
      if (fromFile) {
        auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Novox-regions-" + juce::Uuid().toString() + ".novox");
        CHECK(file.replaceWithData(state.getData(), state.getSize())); data.reset(); CHECK(file.loadFileAsData(data)); file.deleteFile();
      }
      NovoxProcessor restored; restored.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
      CHECK(restored.getSongJson() == regions.getSongJson());
      for (const auto* id : {"region-a", "region-b", "region-c"})
        CHECK(restored.getSource(id)["original"].toString().isNotEmpty() && restored.getSource(id)["original"] == regions.getSource(id)["original"]);
      CHECK(restored.getSource("undone").isVoid());
    }

    // Saved before 2026-10-17: the song's one import in "transcription", its source as sourceId/source.
    const auto oldSource = sourceOf(.25f, 8000);
    juce::MemoryBlock oldState;
    juce::MemoryOutputStream(oldState, false).writeString(
        R"({"novoxState":1,"tempo":100,"song":{"format":"novox-song","version":1,"lyrics":"hey","useNoteLyrics":true,"voice":"std",)"
        R"("transcription":{"recordingId":"old-take","algorithm":"singing-v2"},"notes":[{"pitch":60,"start":2,"dur":1,"lyric":"hey"}]},)"
        R"("sourceId":"old-take","source":)" + juce::JSON::toString(oldSource, true) + "}");
    NovoxProcessor old; old.setStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
    const auto loaded = Song::fromJson(old.getSongJson());
    CHECK(loaded.notes.size() == 1 && loaded.recordingIds() == juce::StringArray({"old-take"}));
    CHECK(loaded.transcriptions[0]["algorithm"].toString() == "singing-v2" && !old.getSongJson().contains("\"transcription\""));
    CHECK(old.getSource("old-take")["original"] == oldSource["original"]);
    juce::MemoryBlock resaved; old.getStateInformation(resaved);
    const auto again = juce::JSON::parse(resaved.toString());
    CHECK(again["sources"].size() == 1 && again["sources"][0]["id"].toString() == "old-take" && !again.hasProperty("sourceId"));

    // Compare > Original: region-a's last half second, then region-b's first (48 kHz output).
    const auto listen = [](NovoxProcessor& proc, int samples) {
      std::vector<float> out; juce::AudioBuffer<float> block(2, 480); juce::MidiBuffer midi;
      while (static_cast<int>(out.size()) < samples) { proc.processBlock(block, midi); for (int i = 0; i < 480; ++i) out.push_back(block.getSample(0, i)); }
      return out;
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < .002f; };
    juce::String error;
    CHECK(regions.auditionSources(juce::JSON::parse(R"([{"id":"region-a","from":0.5,"to":1,"at":0},{"id":"region-b","from":0,"to":0.5,"at":0.5}])"), false, error));
    auto heard = listen(regions, 52800);
    CHECK(near(heard[1200], .1f) && near(heard[12000], .1f) && near(heard[36000], .2f) && near(heard[47000], .2f) && heard[50000] == 0);
    // Overlapping pieces mix: region-a alone, then region-a with region-c, then region-c alone.
    CHECK(regions.auditionSources(juce::JSON::parse(R"([{"id":"region-a","from":0,"to":1,"at":0},{"id":"region-c","from":0,"to":1,"at":0.5}])"), false, error));
    heard = listen(regions, 76800);
    CHECK(near(heard[12000], .1f) && near(heard[36000], .4f) && near(heard[60000], .3f) && heard[74000] == 0);
    regions.stopAudition();
    CHECK(rms(listen(regions, 4800)) == 0.0f);
    // Nothing to play, an unknown recording, a backwards or overlong range: refused.
    CHECK(!regions.auditionSources(juce::var(juce::Array<juce::var>()), false, error));
    CHECK(!regions.auditionSources(juce::JSON::parse(R"([{"id":"nope","from":0,"to":1,"at":0}])"), false, error));
    CHECK(!regions.auditionSources(juce::JSON::parse(R"([{"id":"region-a","from":1,"to":0.5,"at":0}])"), false, error));
    CHECK(!regions.auditionSources(juce::JSON::parse(R"([{"id":"region-a","from":0,"to":1,"at":700}])"), false, error));

    // Opening another song (a .novox file, a host reload) while the editor is open: its Undo can
    // bring the previous song back, so that song's recordings stay available to play and to save.
    juce::MemoryBlock other; old.getStateInformation(other);
    regions.setStateInformation(other.getData(), static_cast<int>(other.getSize()));
    CHECK(Song::fromJson(regions.getSongJson()).recordingIds() == juce::StringArray({"old-take"}));
    CHECK(regions.getSource("old-take")["original"] == oldSource["original"]);
    regions.setSongJson(song.toJson());  // the editor's Undo pushes the regions song back
    juce::MemoryBlock undone; regions.getStateInformation(undone);
    const auto back = juce::JSON::parse(undone.toString());
    CHECK(back["sources"].size() == 3 && count(undone.toString(), "data:audio/wav;base64,") == 3);
    CHECK(regions.auditionSources(juce::JSON::parse(R"([{"id":"region-b","from":0,"to":0.5,"at":0}])"), false, error));
    regions.stopAudition();
  }
  {
    juce::AudioBuffer<float> input(1,16000);
    for(int i=0;i<16000;++i) input.setSample(0,i,.2f*std::sin(static_cast<float>(i)*.08f));
    auto data=MicrophoneCapture::encode(input,12000,16000);
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::MemoryInputStream(data,false),true));
    CHECK(reader && reader->lengthInSamples==12000 && reader->sampleRate==16000);
    juce::AudioBuffer<float> decoded(1,12000);
    CHECK(reader && reader->read(&decoded,0,12000,0,true,false));
    CHECK(std::fabs(decoded.getSample(0,1000)-input.getSample(0,1000))<.0001f);
    CHECK(MicrophoneCapture::encode(input,0,16000).isEmpty());
    CHECK(MicrophoneCapture::encode(input,16001,16000).isEmpty());
    CHECK(MicrophoneCapture::encode(input,12000,0).isEmpty());
  }
  // Sparse sung-lyrics retry: acoustic support, preserved context and rejected ASR loops.
  {
    using Piece = AudioImport::Piece;
    const auto original = AudioImport::wordsFromPieces({{" We", .5, .9f}, {" sing", 1, .9f},
        {" When", 2, .9f}, {" I", 9, .9f}, {" die", 10, .9f}, {" again", 11, .9f}}, 12);
    auto score = juce::JSON::parse("{\"notes\":[]}");
    juce::Array<juce::var> notes;
    for (int i = 0; i < 12; ++i) {
      auto* note = new juce::DynamicObject();
      note->setProperty("start", 2.2 + i * .55); note->setProperty("dur", .35);
      note->setProperty("confidence", .9); notes.add(juce::var(note));
    }
    score.getDynamicObject()->setProperty("notes", juce::var(notes));
    const auto window = AudioImport::lyricRetryWindow(original, score, 12);
    CHECK(window.firstWord == 2 && window.lastWord == 4 && window.start == 1 && window.end == 10);
    CHECK(window.unclaimedNotes == 12 && window.unclaimedSeconds > 4);
    auto covered = score.clone();
    for (auto& note : *covered["notes"].getArray()) note.getDynamicObject()->setProperty("lyric", "oh");
    CHECK(AudioImport::lyricRetryImprovesCoverage(score, covered, window));
    // Lyrics lower joint confidence without removing the sound: the original voiced intervals
    // still have to be covered, but the new text need not inherit the old hum's confidence.
    for (auto& note : *covered["notes"].getArray()) note.getDynamicObject()->setProperty("confidence", .4);
    CHECK(AudioImport::lyricRetryImprovesCoverage(score, covered, window));
    auto lost = covered.clone(); lost["notes"].getArray()->removeRange(0, 6);
    CHECK(!AudioImport::lyricRetryImprovesCoverage(score, lost, window));
    CHECK(!AudioImport::lyricRetryImprovesCoverage(score, juce::var(), window));
    const auto collapsed = juce::JSON::parse("{\"notes\":[{\"start\":2.2,\"dur\":6.5,\"confidence\":0.4,\"lyric\":\"oh\"}]}");
    CHECK(!AudioImport::lyricRetryImprovesCoverage(score, collapsed, window));
    auto withOutside = score.clone();
    withOutside["notes"].getArray()->add(juce::JSON::parse("{\"start\":0,\"dur\":0.5,\"confidence\":0.9,\"lyric\":\"We\"}"));
    CHECK(!AudioImport::lyricRetryImprovesCoverage(withOutside, covered, window));
    auto duplicated = lost.clone();
    const auto repeats = *lost["notes"].getArray();
    duplicated["notes"].getArray()->addArray(repeats);
    CHECK(!AudioImport::lyricRetryImprovesCoverage(score, duplicated, window));
    std::vector<Piece> local;
    for (int i = 0; i < 12; ++i) local.push_back({i < 6 ? " oh" : " la", .5 + i * .5, .9f});
    local.push_back({" When", 7.1, .9f}); local.push_back({" I", 8, .9f}); local.push_back({" die", 9, .9f});
    const auto recovered = AudioImport::mergeLyricRetry(original, local, window, 12);
    CHECK(recovered.size() == original.size() + 12);
    if (!recovered.isEmpty()) {
      CHECK(juce::JSON::toString(recovered[0]) == juce::JSON::toString(original[0]));
      CHECK(juce::JSON::toString(recovered[1]) == juce::JSON::toString(original[1]));
      CHECK(juce::JSON::toString(recovered.getLast()) == juce::JSON::toString(original.getLast()));
      CHECK(recovered[2]["word"].toString() == "oh" && static_cast<double>(recovered[2]["end"]) == 1.5);
    }
    auto changed = local; changed[12].text = " Something";
    CHECK(AudioImport::mergeLyricRetry(original, changed, window, 12).isEmpty());
    changed = local; changed[0].text = " oh-oh-oh-oh-oh";
    CHECK(AudioImport::mergeLyricRetry(original, changed, window, 12).isEmpty());
    changed = local; for (auto& piece : changed) piece.time = 9;
    CHECK(AudioImport::mergeLyricRetry(original, changed, window, 12).isEmpty());
    changed = local; changed[3].time = 30;
    CHECK(AudioImport::mergeLyricRetry(original, changed, window, 12).isEmpty());
    auto overlapping = original;
    overlapping.getReference(5) = original[5].clone();
    overlapping[5]["tokens"][0].getDynamicObject()->setProperty("time", 9.5);
    CHECK(AudioImport::mergeLyricRetry(overlapping, local, window, 12).isEmpty());
    changed = local; for (auto& piece : changed) piece.probability = .1f;
    CHECK(AudioImport::mergeLyricRetry(original, changed, window, 12).isEmpty());
    auto insufficient = window; insufficient.unclaimedNotes = 3;
    CHECK(AudioImport::mergeLyricRetry(original, local, insufficient, 12).isEmpty());
    // A long held vowel, quiet/noisy notes and notes that already have lyrics are not retries.
    auto held = juce::JSON::parse("{\"notes\":[{\"start\":2,\"dur\":7,\"confidence\":0.95}]}");
    CHECK(AudioImport::lyricRetryWindow(original, held, 12).firstWord < 0);
    for (auto& note : notes) note.getDynamicObject()->setProperty("confidence", .2);
    score.getDynamicObject()->setProperty("notes", juce::var(notes));
    CHECK(AudioImport::lyricRetryWindow(original, score, 12).firstWord < 0);
    for (auto& note : notes) {
      note.getDynamicObject()->setProperty("confidence", .9); note.getDynamicObject()->setProperty("lyric", "_");
    }
    score.getDynamicObject()->setProperty("notes", juce::var(notes));
    CHECK(AudioImport::lyricRetryWindow(original, score, 12).firstWord < 0);
  }

  // --audio FILE [--download] [--detailed] [--melody] [--no-lyric-retry] [--whisper-variant NAME[,NAME...]]: the
  // plugin's import of FILE (--detailed: with Detailed notes on; --melody: Melody only, no lyrics).
  if (argc >= 3 && juce::String(argv[1]) == "--audio") {
    bool download = false, detailed = false, melody = false, lyricRetry = true;
    juce::StringArray variant;
    for (int k = 3; k < argc; ++k) {
      const juce::String arg(argv[k]);
      if (arg == "--download") download = true;
      else if (arg == "--detailed") detailed = true;
      else if (arg == "--melody") melody = true;
      else if (arg == "--no-lyric-retry") lyricRetry = false;
      else if (arg == "--whisper-variant" && k + 1 < argc) variant.addTokens(argv[++k], ",", "");
      else { std::cerr << "unknown argument " << arg << std::endl; return 2; }
    }
    for (const auto& name : variant) {
      auto probe = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
      if (!applyWhisperVariant(probe, name)) { std::cerr << "unknown whisper variant " << name << std::endl; return 2; }
    }
    std::atomic<double> decodeStarted{0};  // outlives the importer's worker thread
    AudioImport importer;
    importer.retrySparseLyrics = lyricRetry;
    importer.inspectLyricRetry = [](const juce::var& evidence) {
      const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("out/native-lyrics-retry.json");
      file.getParentDirectory().createDirectory();
      file.replaceWithText(juce::JSON::toString(evidence));
    };
    if (!variant.isEmpty())
      importer.adjustRecognition = [variant, &decodeStarted](whisper_full_params& p) {
        for (const auto& name : variant) applyWhisperVariant(p, name);
        decodeStarted = juce::Time::getMillisecondCounterHiRes();
      };
    juce::MemoryBlock bytes;
    juce::File file(juce::String::fromUTF8(argv[2]));
    CHECK(file.loadFileAsData(bytes));
    // Match the browser btoa() upload before running the native conversion.
    juce::MemoryBlock upload;
    juce::MemoryOutputStream uploadStream(upload, false);
    CHECK(juce::Base64::convertFromBase64(uploadStream, juce::Base64::toBase64(bytes.getData(), bytes.getSize())));
    uploadStream.flush();
    CHECK(upload == bytes);
    // Where the file says it sat on the DAW's timeline (bext, or a Logic region drag's markers).
    const auto stamp = AudioImport::timelineStamp(upload);
    if (stamp.seconds > 0)
      std::cout << "timeline stamp: " << juce::String(stamp.seconds, 6) << " s from 00:00:00:00, originator \""
                << stamp.originator << "\"" << std::endl;
    else std::cout << "timeline stamp: none" << std::endl;
    const auto reply = importer.start(upload, file.getFileName(), download, melody, detailed);
    CHECK(!reply.getProperty("error", juce::var()).isString());
    auto last = juce::String();
    juce::var status;
    juce::StringArray phases;
    double phaseStart = juce::Time::getMillisecondCounterHiRes(), recognized = 0;
    for (int i = 0; i < 12000; ++i) {
      status = importer.status();
      auto message = status.getProperty("message", "").toString();
      if (message != last) {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (last.isNotEmpty()) phases.add(last + " " + juce::String((now - phaseStart) / 1000, 2) + " s");
        if (last.startsWith("Recognizing")) recognized = now;
        std::cout << message << std::endl; last = message; phaseStart = now;
      }
      auto state = status.getProperty("state", "").toString();
      if (state == "ready" || state == "error" || state == "cancelled") break;
      pump(.05);
    }
    std::cout << "phases: " << phases.joinIntoString(", ") << std::endl;
    if (decodeStarted > 0 && recognized > 0)
      std::cout << "whisper_full " << juce::String((recognized - decodeStarted) / 1000, 2) << " s (variant "
                << variant.joinIntoString(",") << ")" << std::endl;
    CHECK(status.getProperty("state", "").toString() == "ready");
    auto result = status.getProperty("result", juce::var());
    CHECK(result["detail"].toString() == (detailed ? "detailed" : "natural"));
    // The import result carries the same stamp the editor places the notes by.
    CHECK(stamp.seconds > 0 ? static_cast<double>(result.getProperty("timeReference", 0)) == stamp.seconds &&
                                  result["originator"].toString() == stamp.originator
                            : !result.hasProperty("timeReference"));
    auto score = result.getProperty("score", juce::var());
    // Every recognized word carries its whisper pieces with DTW completion times, and the last
    // word keeps its measured end (asrEnd) beside the end forced to the end of the file.
    if (auto* words = result.getProperty("words", juce::var()).getArray()) {
      for (const auto& word : *words) {
        auto* tokens = word.getProperty("tokens", juce::var()).getArray();
        CHECK(tokens && !tokens->isEmpty());
        if (!tokens) continue;
        juce::String text;
        for (const auto& token : *tokens) {
          const double time = token.getProperty("time", -1), p = token.getProperty("probability", -1);
          CHECK(token.getProperty("text", juce::var()).isString() && std::isfinite(time) && time >= 0 && p >= 0 && p <= 1);
          text += token.getProperty("text", "").toString();
        }
        CHECK(text.trim() == word.getProperty("word", "").toString());
      }
      // Every word lies inside the audio, in order (the engine rejects end < start).
      double previousStart = 0;
      for (const auto& word : *words) {
        const double start = word.getProperty("start", -1), end = word.getProperty("end", -1);
        CHECK(start >= previousStart && start <= end && end <= static_cast<double>(result.getProperty("duration", 0)));
        previousStart = start;
      }
      if (!words->isEmpty()) {
        const auto& lastWord = words->getReference(words->size() - 1);
        CHECK(lastWord.hasProperty("asrEnd") && static_cast<double>(lastWord.getProperty("asrEnd", -1)) >= 0);
        CHECK(static_cast<double>(lastWord.getProperty("end", -1)) == static_cast<double>(result.getProperty("duration", 0)));
      }
    }
    if (!failures) {
      NovoxProcessor audition;
      audition.setRateAndBufferSizeDetails(48000, 480); audition.prepareToPlay(48000,480);
      juce::MemoryBlock wav;
      juce::MemoryOutputStream decoded(wav, false);
      CHECK(juce::Base64::convertFromBase64(decoded, result.getProperty("original", "").toString().substring(22)));
      decoded.flush();
      juce::String error;
      CHECK(audition.auditionAudio(wav,error));
      juce::AudioBuffer<float> buffer(2,480); juce::MidiBuffer midi;
      float peak=0;
      // Songs can open quietly: listen until the recording is heard, at most its whole length.
      const int blocks=static_cast<int>(static_cast<double>(result.getProperty("duration", 0))*100)+1;
      for (int i=0;i<blocks && peak<=.01f;++i) { audition.processBlock(buffer,midi); peak=std::max(peak,buffer.getMagnitude(0,buffer.getNumSamples())); }
      CHECK(peak > .01f);
      audition.stopAudition(); audition.processBlock(buffer,midi);
      CHECK(buffer.getMagnitude(0,buffer.getNumSamples()) == 0);
      CHECK(importer.preview("{\"notes\":[{\"start\":0,\"dur\":1000000,\"pitch\":60}]}").getProperty("error",juce::var()).isString());
    }

    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile("out/native-audio-review.json");
    CHECK(output.getParentDirectory().createDirectory());
    CHECK(output.replaceWithText(juce::JSON::toString(result)));
    std::cout << juce::JSON::toString(score) << std::endl;
    if (!failures) {
      pump(.25);
      auto edited = score.clone();
      // Move the first note a fifth, so the edit changes the preview whatever pitch it had.
      if (auto* notes = edited.getProperty("notes",juce::var()).getArray()) {
        auto* first = notes->getReference(0).getDynamicObject();
        const double pitch = first->getProperty("pitch");
        first->setProperty("pitch", pitch > 100 ? pitch - 7 : pitch + 7);
      }
      CHECK(!importer.preview(juce::JSON::toString(edited)).getProperty("error", juce::var()).isString());
      for (int i=0;i<240 && importer.status().getProperty("state", "").toString()=="working";++i) pump(.1);
      CHECK(importer.status().getProperty("state", "").toString()=="ready");
      CHECK(importer.status().getProperty("result",juce::var()).getProperty("preview", "") != result.getProperty("preview", ""));
      CHECK(importer.status().getProperty("result",juce::var()).getProperty("original", "") == result.getProperty("original", ""));
    }
    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures ? 1 : 0;
  }
  // --regions FILE FILE... [--melody]: region-by-region import of real recordings, natively. Each
  // file is converted as the plugin does and placed at its timeline stamp the way the editor does
  // with Logic's playhead at bar 1 (120 bpm, bar 1 at a whole hour; no stamp: after the previous
  // one); every recording is kept as a source through a state save and load, and Compare >
  // Original over the whole song plays each one at its place. Writes the conversions (without
  // their audio) to out/native-regions.json for the editor's merge (plugin/ui/song-merge.js).
  if (argc >= 3 && juce::String(argv[1]) == "--regions") {
    bool melody = false;
    juce::StringArray files;
    for (int k = 2; k < argc; ++k) { const juce::String arg(argv[k]); if (arg == "--melody") melody = true; else files.add(arg); }
    NovoxProcessor proc; proc.setRateAndBufferSizeDetails(48000, 480); proc.prepareToPlay(48000, 480);
    juce::Array<juce::var> records, pieces, results;
    auto song = Song::defaultSong(); song.notes.clear(); song.useNoteLyrics = true;
    double next = 0, firstBeat = -1;
    for (int f = 0; f < files.size(); ++f) {
      juce::File file(files[f]); juce::MemoryBlock bytes;
      CHECK(file.loadFileAsData(bytes));
      AudioImport importer;
      CHECK(!importer.start(bytes, file.getFileName(), false, melody).getProperty("error", juce::var()).isString());
      for (int i = 0; i < 12000 && importer.status().getProperty("state", "").toString() == "working"; ++i) pump(.05);
      CHECK(importer.status().getProperty("state", "").toString() == "ready");
      auto result = importer.status().getProperty("result", juce::var());
      const double stamp = result.getProperty("timeReference", 0), seconds = result.getProperty("duration", 0);
      const bool logic = result.getProperty("originator", "").toString().startsWithIgnoreCase("Logic");
      const double beat = stamp > 0 && logic ? (stamp - std::floor(stamp / 3600) * 3600) * 2 : next;
      const auto id = "region-" + juce::String(f);
      auto* source = new juce::DynamicObject();
      source->setProperty("original", result["original"]); source->setProperty("waveform", result["waveform"]);
      source->setProperty("seconds", seconds); source->setProperty("bpm", 120); source->setProperty("startBeat", beat);
      CHECK(proc.storeSource(id, juce::var(source)));
      auto* record = new juce::DynamicObject();
      record->setProperty("recordingId", id); record->setProperty("startBeat", beat); record->setProperty("beats", seconds * 2);
      records.add(juce::var(record));
      int count = 0;
      if (auto* notes = result["score"]["notes"].getArray())
        for (const auto& n : *notes) { song.notes.add({beat + static_cast<double>(n["start"]) * 2, static_cast<double>(n["dur"]) * 2, juce::roundToInt(static_cast<double>(n["pitch"])), n["lyric"].toString()}); ++count; }
      if (firstBeat < 0) firstBeat = beat;
      auto* piece = new juce::DynamicObject();
      piece->setProperty("id", id); piece->setProperty("from", 0); piece->setProperty("to", seconds); piece->setProperty("at", (beat - firstBeat) / 2);
      pieces.add(juce::var(piece));
      std::cout << file.getFileName() << ": stamp " << juce::String(stamp, 4) << " s (" << result.getProperty("originator", "none").toString()
                << "), at beat " << juce::String(beat, 4) << ", " << juce::String(seconds, 3) << " s, " << count << " notes" << std::endl;
      next = beat + seconds * 2;
      auto light = result.clone();
      light.getDynamicObject()->removeProperty("original"); light.getDynamicObject()->removeProperty("preview");
      light.getDynamicObject()->removeProperty("pitchCurve"); light.getDynamicObject()->removeProperty("waveform");
      results.add(light);
    }
    song.sortNotes(); song.transcriptions = records;
    proc.setSongJson(song.toJson());
    juce::MemoryBlock state; proc.getStateInformation(state);
    NovoxProcessor restored; restored.setRateAndBufferSizeDetails(48000, 480); restored.prepareToPlay(48000, 480);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(Song::fromJson(restored.getSongJson()).recordingIds().size() == files.size());
    for (int f = 0; f < files.size(); ++f)
      CHECK(restored.getSource("region-" + juce::String(f))["original"] == proc.getSource("region-" + juce::String(f))["original"]);
    std::cout << "state: " << state.getSize() << " bytes for " << files.size() << " recordings" << std::endl;
    juce::String error;
    CHECK(restored.auditionSources(juce::var(pieces), false, error));
    if (error.isNotEmpty()) std::cerr << "  audition: " << error << "\n";
    std::vector<float> heard; juce::AudioBuffer<float> block(2, 480); juce::MidiBuffer midi;
    const double total = static_cast<double>(pieces.getLast()["at"]) + static_cast<double>(pieces.getLast()["to"]);
    while (heard.size() < static_cast<size_t>((total + .2) * 48000)) {
      restored.processBlock(block, midi);
      for (int i = 0; i < 480; ++i) heard.push_back(block.getSample(0, i));
    }
    for (const auto& piece : pieces) {
      const auto from = static_cast<size_t>(static_cast<double>(piece["at"]) * 48000), to = from + static_cast<size_t>(static_cast<double>(piece["to"]) * 48000);
      const float level = rms(heard, from, to);
      std::cout << "  Original " << piece["id"].toString() << " at " << juce::String(static_cast<double>(piece["at"]), 3) << " s: rms " << level << std::endl;
      CHECK(level > .005f);
    }
    if (pieces.size() > 1) {  // the silence between two regions stays silent
      const auto gapFrom = static_cast<size_t>((static_cast<double>(pieces[0]["at"]) + static_cast<double>(pieces[0]["to"]) + .05) * 48000);
      const auto gapTo = static_cast<size_t>((static_cast<double>(pieces[1]["at"]) - .05) * 48000);
      if (gapTo > gapFrom) CHECK(rms(heard, gapFrom, gapTo) == 0.0f);
    }
    juce::File::getCurrentWorkingDirectory().getChildFile("out/native-regions.json").replaceWithText(juce::JSON::toString(juce::var(results)));
    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures ? 1 : 0;
  }
  {
    AudioImport importer;
    CHECK(importer.start({}, "empty.wav", false, true).getProperty("error", juce::var()).isString());
    juce::MemoryBlock garbage("bad audio", 9);
    CHECK(!importer.start(garbage, "invalid.mp3", false, true).getProperty("error", juce::var()).isString());
    for (int i=0;i<100 && importer.status().getProperty("state", "").toString()=="working";++i) pump(.02);
    CHECK(importer.status().getProperty("state", "").toString()=="error");
  }

  // A Logic region export keeps its timeline stamp (Broadcast WAV bext) through import.
  {
    juce::MemoryBlock wav;
    {
      juce::StringPairArray meta;
      meta.set(juce::WavAudioFormat::bwavOriginator, "Logic Pro");
      meta.set(juce::WavAudioFormat::bwavTimeReference, juce::String((juce::int64)158826150));  // 01:00:01.5
      auto* stream = new juce::MemoryOutputStream(wav, false);
      std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, 44100, 1, 16, meta, 0));
      CHECK(writer != nullptr);
      juce::AudioBuffer<float> tone(1, 44100);
      for (int i = 0; i < tone.getNumSamples(); ++i) {
        const double t = i / 44100.0, env = std::min(1.0, std::min(t, 1.0 - t) / .05);
        tone.setSample(0, i, static_cast<float>(env * (.3 * std::sin(2 * juce::MathConstants<double>::pi * 220 * t) + .15 * std::sin(4 * juce::MathConstants<double>::pi * 220 * t))));
      }
      if (writer) writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
    }
    AudioImport importer;
    CHECK(!importer.start(wav, "Region.wav", false, true).getProperty("error", juce::var()).isString());
    // Progress text shown in the LCD keeps its ellipsis (UTF-8 read as ASCII showed "â€¦").
    bool sawEllipsis = false, sawMojibake = false;
    for (int i=0;i<400 && importer.status().getProperty("state", "").toString()=="working";++i) {
      const auto message = importer.status().getProperty("message", "").toString();
      sawEllipsis = sawEllipsis || message.endsWith(juce::String::charToString(juce::juce_wchar(0x2026)));
      sawMojibake = sawMojibake || message.containsChar(juce::juce_wchar(0xE2));
      pump(.01);
    }
    CHECK(sawEllipsis && !sawMojibake);
    const auto result = importer.status().getProperty("result", juce::var());
    CHECK(importer.status().getProperty("state", "").toString()=="ready");
    CHECK(std::fabs(static_cast<double>(result.getProperty("timeReference", 0)) - 3601.5) < 1e-6);
    CHECK(result.getProperty("originator", "").toString() == "Logic Pro");
  }

  // A region dragged from Logic onto the plugin arrives as an AIFF with no bext: its timeline
  // position is the marker named "Timestamp: <samples since 00:00:00:00>", its creator the COMT
  // comment. The import keeps both, as for a bext stamp.
  {
    juce::AudioBuffer<float> tone(1, 44100);
    for (int i = 0; i < tone.getNumSamples(); ++i) {
      const double t = i / 44100.0, env = std::min(1.0, std::min(t, 1.0 - t) / .05);
      tone.setSample(0, i, static_cast<float>(env * (.3 * std::sin(2 * juce::MathConstants<double>::pi * 220 * t) + .15 * std::sin(4 * juce::MathConstants<double>::pi * 220 * t))));
    }
    const juce::StringArray logicMarkers{"Tempo: 120.0", "Start", "End  ", "Timestamp: 158826150"};  // 01:00:01.5
    const auto region = logicAiff(tone, logicMarkers, "Logic Pro 12.4");
    const auto import = [](const juce::MemoryBlock& data) {
      AudioImport importer;
      CHECK(!importer.start(data, "VOCAL #20 merged.aif", false, true).getProperty("error", juce::var()).isString());
      for (int i=0;i<400 && importer.status().getProperty("state", "").toString()=="working";++i) pump(.05);
      CHECK(importer.status().getProperty("state", "").toString()=="ready");
      return importer.status().getProperty("result", juce::var());
    };
    auto result = import(region);
    CHECK(std::fabs(static_cast<double>(result.getProperty("timeReference", 0)) - 3601.5) < 1e-6);
    CHECK(result.getProperty("originator", "").toString() == "Logic Pro 12.4");
    CHECK(result.getProperty("score", juce::var()).getProperty("notes", juce::var()).size() > 0);
    // Without a creator comment the marker is still Logic's; AIFC keeps MARK after its other chunks.
    auto stamp = AudioImport::timelineStamp(logicAiff(tone, logicMarkers, {}));
    CHECK(std::fabs(stamp.seconds - 3601.5) < 1e-6 && stamp.originator == "Logic Pro");
    stamp = AudioImport::timelineStamp(logicAiff(tone, logicMarkers, "Logic Pro 12.4", true));
    CHECK(std::fabs(stamp.seconds - 3601.5) < 1e-6 && stamp.originator == "Logic Pro 12.4");
    // No markers, or none in Logic's timestamp form: no stamp, and the import has none.
    CHECK(AudioImport::timelineStamp(logicAiff(tone, {}, "Logic Pro 12.4")).seconds <= 0);
    for (const auto* name : {"Timestamp:", "Timestamp: 12x4", "Timestamp: -5", "Timestamp: 1234567890123456", "Start"})
      CHECK(AudioImport::timelineStamp(logicAiff(tone, {"Tempo: 120.0", name}, "Logic Pro 12.4")).seconds <= 0);
    result = import(logicAiff(tone, {}, "Logic Pro 12.4"));
    CHECK(!result.hasProperty("timeReference") && !result.hasProperty("originator"));
    // A malformed MARK chunk is skipped, never read past: a name longer than its chunk, a chunk
    // longer than the file, or a file cut off inside the chunk.
    const int mark = chunkAt(region, "MARK");
    CHECK(mark > 0);
    auto overlong = region;
    const int timestampName = chunkAt(region, "Timestamp", static_cast<size_t>(mark)) - 1;
    CHECK(timestampName > mark);
    static_cast<unsigned char*>(overlong.getData())[timestampName] = 255;
    CHECK(AudioImport::timelineStamp(overlong).seconds <= 0);
    auto huge = region;
    for (int i = 4; i < 8; ++i) static_cast<unsigned char*>(huge.getData())[mark + i] = 0xff;
    CHECK(AudioImport::timelineStamp(huge).seconds <= 0);
    juce::MemoryBlock cut(region.getData(), static_cast<size_t>(mark + 30));
    CHECK(AudioImport::timelineStamp(cut).seconds <= 0);
    auto many = region;  // more markers claimed than written: the written ones still count
    static_cast<unsigned char*>(many.getData())[mark + 8] = 0xff;
    CHECK(std::fabs(AudioImport::timelineStamp(many).seconds - 3601.5) < 1e-6);
    result = import(overlong);
    CHECK(!result.hasProperty("timeReference"));
    CHECK(AudioImport::timelineStamp(juce::MemoryBlock("FORM", 4)).seconds <= 0);
    // A WAV with the same marker as a cue label, and its software in LIST INFO.
    juce::MemoryBlock wav;
    {
      juce::StringPairArray meta;
      meta.set("NumCuePoints", "1"); meta.set("Cue0Identifier", "1"); meta.set("Cue0Offset", "0");
      meta.set("NumCueLabels", "1"); meta.set("CueLabel0Identifier", "1"); meta.set("CueLabel0Text", "Timestamp: 158826150");
      meta.set(juce::WavAudioFormat::riffInfoSoftware, "Logic Pro 12.4");
      auto* stream = new juce::MemoryOutputStream(wav, false);
      std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, 44100, 1, 16, meta, 0));
      CHECK(writer != nullptr);
      if (writer) writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
    }
    stamp = AudioImport::timelineStamp(wav);
    CHECK(std::fabs(stamp.seconds - 3601.5) < 1e-6 && stamp.originator == "Logic Pro 12.4");
  }

  // Recognized pieces become words: each starts at the previous word's end and ends at its last
  // piece; the last ends at the end of the file, keeping its measured end as asrEnd.
  {
    using Piece = AudioImport::Piece;
    auto words = AudioImport::wordsFromPieces({{" Hel", 0.5, .9f}, {"lo", 0.9, .8f}, {" world", 1.5, .7f}}, 3.0);
    CHECK(words.size() == 2);
    if (words.size() == 2) {
      CHECK(words[0]["word"].toString() == "Hello" && static_cast<double>(words[0]["start"]) == 0 &&
            static_cast<double>(words[0]["end"]) == 0.9 && std::fabs(static_cast<double>(words[0]["probability"]) - .8) < 1e-6);
      CHECK(!words[0].hasProperty("asrEnd") && words[0]["tokens"].size() == 2 &&
            words[0]["tokens"][1]["text"].toString() == "lo" && static_cast<double>(words[0]["tokens"][1]["time"]) == 0.9);
      CHECK(words[1]["word"].toString() == "world" && static_cast<double>(words[1]["start"]) == 0.9 &&
            static_cast<double>(words[1]["end"]) == 3.0 && static_cast<double>(words[1]["asrEnd"]) == 1.5);
    }
    CHECK(AudioImport::wordsFromPieces({}, 3.0).isEmpty());
    CHECK(AudioImport::wordsFromPieces({{" ", 0.2, 1}}, 3.0).isEmpty());
    // Pieces finishing together at the end of the file (CSD en011b, en047a): each later word still
    // takes 20 ms, so the old chain started the last word after the file's end, then forced its
    // end back to the file's end ("invalid timestamped word"). Times past the end are clamped.
    const double seconds = 2.01;
    std::vector<Piece> crowded{{" la", 1.0, 1}, {" la", 2.0, 1}, {" la", 2.0, 1}, {" la", 2.0, 1}, {" oh", 2.3, 1}};
    words = AudioImport::wordsFromPieces(crowded, seconds);
    CHECK(words.size() == 5);
    double previousStart = 0;
    for (const auto& word : words) {
      const double start = word["start"], end = word["end"];
      CHECK(start >= previousStart && start <= end && end <= seconds);
      previousStart = start;
      CHECK(static_cast<double>(word["tokens"][0]["time"]) >= 1.0);  // pieces keep their measured times
    }
    if (words.size() == 5) {
      CHECK(static_cast<double>(words[1]["end"]) == 2.0 && static_cast<double>(words[2]["end"]) == seconds);
      CHECK(static_cast<double>(words[4]["end"]) == seconds && std::fabs(static_cast<double>(words[4]["asrEnd"]) - 2.3) < 1e-9);
      CHECK(static_cast<double>(words[4]["tokens"][0]["time"]) == 2.3);
    }
    // The engine takes those words (it threw on the old ones).
    std::vector<float> tone(static_cast<size_t>(seconds * 16000));
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<float>(.3 * std::sin(2 * juce::MathConstants<double>::pi * 220 * i / 16000.0));
    std::unique_ptr<novox_engine, decltype(&novox_destroy)> engine(novox_create(44100), novox_destroy);
    const auto wordJson = juce::JSON::toString(juce::var(words));
    char* raw = novox_transcribe(engine.get(), tone.data(), tone.size(), 16000, wordJson.toRawUTF8(),
                                 AudioImport::transcribeOptions(false).toRawUTF8());
    CHECK(raw != nullptr);
    if (!raw) std::cerr << "  novox_transcribe: " << novox_last_error(engine.get()) << "\n";
    novox_free(raw);
  }

  // Imports use singing-v2 and pass the Detailed notes choice to the engine as "detail".
  {
    const auto natural = juce::JSON::parse(AudioImport::transcribeOptions(false));
    const auto detailed = juce::JSON::parse(AudioImport::transcribeOptions(true));
    CHECK(natural["algorithm"].toString() == "singing-v2" && natural["detail"].toString() == "natural");
    CHECK(detailed["algorithm"].toString() == "singing-v2" && detailed["detail"].toString() == "detailed");
    juce::AudioBuffer<float> tone(1, 32000);
    for (int i = 0; i < tone.getNumSamples(); ++i) {
      const double t = i / 16000.0, env = std::min(1.0, std::min(t, 2.0 - t) / .05);
      tone.setSample(0, i, static_cast<float>(env * (.3 * std::sin(2 * juce::MathConstants<double>::pi * 220 * t) + .15 * std::sin(4 * juce::MathConstants<double>::pi * 220 * t))));
    }
    const auto wav = MicrophoneCapture::encode(tone, tone.getNumSamples(), 16000);
    for (const bool detail : {false, true}) {
      AudioImport importer;
      CHECK(!importer.start(wav, "Singing.wav", false, true, detail).getProperty("error", juce::var()).isString());
      for (int i=0;i<400 && importer.status().getProperty("state", "").toString()=="working";++i) pump(.05);
      const auto result = importer.status().getProperty("result", juce::var());
      CHECK(importer.status().getProperty("state", "").toString()=="ready");
      CHECK(result["detail"].toString() == (detail ? "detailed" : "natural"));
      const auto transcription = result["score"]["transcription"];
      CHECK(transcription["algorithm"].toString() == "singing-v2");
      // The engine reports the detail it segmented with: the one asked for.
      CHECK(transcription["detail"].toString() == (detail ? "detailed" : "natural"));
    }
  }

  const juce::String songJson =
      R"({"lyrics": "la la la la", "voice": "std", "notes": [
        {"pitch": 60, "start": 0, "dur": 1}, {"pitch": 62, "start": 1, "dur": 1},
        {"pitch": 64, "start": 2, "dur": 1}, {"pitch": 65, "start": 3, "dur": 1}]})";

  NovoxProcessor p;
  {
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    CHECK(editor->isResizable());
    // The page, not the WebView2-missing message: on Windows, the WebView2 runtime is installed.
    CHECK(webBrowserOf(*editor) != nullptr);
    CHECK(editor->getWidth()==1000 && editor->getHeight()==660);
    editor->setSize(1280,800);
    CHECK(p.editorSize==juce::Point<int>(1280,800));
    editor.reset();
    editor.reset(p.createEditor());
    CHECK(editor->getWidth()==1280 && editor->getHeight()==800);
  }
  // Without a web view (a Windows PC without the WebView2 runtime) the editor says where to get one
  // instead of showing a blank page, keeps its size, and goes away cleanly.
  {
    NovoxEditor::testWithoutWebView = true;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    NovoxEditor::testWithoutWebView = false;
    const juce::HyperlinkButton* link = nullptr;
    for (auto* child : editor->getChildren())
      if (auto* l = dynamic_cast<juce::HyperlinkButton*>(child)) link = l;
    CHECK(webBrowserOf(*editor) == nullptr && link != nullptr && link->isVisible());
    CHECK(link && link->getURL().toString(false) == "https://developer.microsoft.com/microsoft-edge/webview2/");
    CHECK(link && editor->getLocalBounds().contains(link->getBounds()));
    CHECK(editor->getWidth()==1280 && editor->getHeight()==800);
    editor->setSize(800, 560);
    CHECK(link && editor->getLocalBounds().contains(link->getBounds()) && p.editorSize==juce::Point<int>(800,560));
    pump(0.1);
    editor.reset();
    p.editorSize = {1280, 800};
  }
  // This platform's files (Platform.h) and audio imports: every audio type the editor offers has a
  // decoder here, and every candidate with one is offered (m4a needs Core Audio, macOS only).
  {
#if JUCE_MAC
    CHECK(juce::String(NovoxPlatform::name()) == "mac");
    CHECK(NovoxPlatform::dataFolder() ==
          juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Application Support/Novox"));
#elif JUCE_WINDOWS
    CHECK(juce::String(NovoxPlatform::name()) == "windows");
    CHECK(NovoxPlatform::dataFolder() == juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Novox"));
#endif
    CHECK(AudioImport::modelFile().getParentDirectory() == NovoxPlatform::dataFolder());
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (const auto* extension : {"wav", "mp3", "aif", "aiff", "m4a", "flac", "ogg"})
      CHECK(AudioImport::extensions().contains(extension) == (formats.findFormatForFileExtension(extension) != nullptr));
#if JUCE_MAC
    CHECK(AudioImport::extensions().contains("m4a"));
#else
    CHECK(!AudioImport::extensions().contains("m4a"));
#endif
    // Lyrics recognition runs here (a Windows PC needs AVX2; CI runners have it), on 4 threads
    // except on Windows (one per physical core but one, at most 8).
    CHECK(AudioImport::recognitionUnavailable().isEmpty());
    const int threads = AudioImport::recognitionThreads();
#if JUCE_WINDOWS
    CHECK(threads >= 1 && threads <= 8);
#else
    CHECK(threads == 4);
#endif
  }
  FakePlayHead ph;
  p.setPlayHead(&ph);
  p.setRateAndBufferSizeDetails(48000, 512);
  p.prepareToPlay(48000, 512);
  p.setSongJson(songJson);
  waitForRender(p);

  // A Compare > Robot review (a copy of the render) ends when the DAW starts playing and when the
  // song changes: the DAW then hears the song in step, not the copy (it played on instead).
  {
    p.stopAudition();
    ph.playing = true;
    const auto expected = play(p, ph, 0.5, .1);
    ph.playing = false;
    play(p, ph, 0, .02);
    juce::String error;
    CHECK(p.auditionSong(2.5, 4, false, error));
    CHECK(rms(play(p, ph, 0, .05)) > .001f);  // stopped: the review plays
    ph.playing = true;
    const auto got = play(p, ph, 0.5, .1);
    float diff = 0;
    for (size_t i = 0; i < std::min(got.size(), expected.size()); ++i) diff = std::max(diff, std::fabs(got[i] - expected[i]));
    CHECK(rms(expected) > .001f && diff < 1e-5f);
    ph.playing = false;
    play(p, ph, 0, .02);
    CHECK(p.auditionSong(2.5, 4, false, error));
    p.setSongJson(songJson.replace("\"pitch\": 60", "\"pitch\": 61"));  // an audible edit
    CHECK(rms(play(p, ph, 0, .05)) < 1e-6f);
    waitForRender(p);
    p.setSongJson(songJson);
    waitForRender(p);  // restore the fixture for the following playback checks
  }
  testRenderStateChanges();

  // Edits faster than a render (typing) still bring fresh audio: a render is not cancelled once the
  // audio is a second behind the edits.
  {
    juce::String big = R"({"lyrics": "la", "voice": "std", "notes": [)";
    for (int i = 0; i < 600; ++i) big << (i ? "," : "") << "{\"pitch\": " << (55 + i % 12) << ", \"start\": " << i << ", \"dur\": 0.9}";
    big << "]}";
    NovoxProcessor typing;
    typing.prepareToPlay(48000, 512);
    typing.setSongJson(big);
    waitForRender(typing);
    const int before = typing.renderedVersion();
    bool fresh = false;
    for (int k = 0; k < 80 && !fresh; ++k) {
      typing.setSongJson(big.replace("\"la\"", "\"la" + juce::String(k) + "\""));
      pump(0.1);
      fresh = typing.renderedVersion() > before;
    }
    CHECK(before >= 0 && fresh);
    // The status says which edit the audio is from, and counts renders (the LCD's OUT OF DATE).
    waitForRender(typing);
    auto st = typing.getStatus();
    CHECK(typing.renderedVersion() == st.songVersion && st.rendersStarted > 0 && st.rendersCancelled >= 0 && st.secondsSinceRender >= 0);
    typing.setSongJson(big);
    CHECK(typing.renderedVersion() != typing.getStatus().songVersion);
    waitForRender(typing);
    CHECK(typing.renderedVersion() == typing.getStatus().songVersion && typing.getStatus().rendersStarted > st.rendersStarted);
    const auto settled = typing.getStatus();
    typing.setSongJson(big);  // an identical editor push changes no sound
    pump(.05);
    CHECK(typing.renderedVersion() == typing.getStatus().songVersion && typing.getStatus().rendersStarted == settled.rendersStarted);
  }

  // Editing audition reaches the plugin output with a stopped host, leaves
  // song state intact, and ends instead of creating a stuck monitoring note.
  ph.playing=false;
  play(p,ph,0,.02);
  const auto beforePreview=p.getSongJson();
  p.previewNote(60);
  std::vector<float> hum;
  for(int attempt=0;attempt<150;++attempt) {
    pump(.02);
    hum=play(p,ph,0,.02);
    if(rms(hum)>.001f) break;
  }
  auto humTail=play(p,ph,0,.6);
  hum.insert(hum.end(),humTail.begin(),humTail.end());
  CHECK(rms(hum)>.001f);
  CHECK(rms(hum,24000)<.00001f);
  CHECK(p.getSongJson()==beforePreview);
  CHECK(std::fabs(hum.front())<.001f);
  p.previewNote(64);pump(.25);
  ph.playing=true; // Starting transport cancels the edit hum.

  // 1. Playing from beat 0 matches the engine's own render of the same score.
  auto a = play(p, ph, 0, 2.0);
  std::cout << "  rms playing " << rms(a) << "\n";
  CHECK(rms(a) > 0.01f);
  novox_engine* e = novox_create(48000);
  novox_stream* s = novox_stream_create(e, Song::fromJson(songJson).toScoreJson(120).toRawUTF8());
  const double lead = novox_stream_lead(s);
  std::vector<float> ref(novox_stream_length(s));
  novox_stream_process(s, ref.data(), ref.size());
  novox_stream_destroy(s);
  const size_t offset = static_cast<size_t>(std::llround(lead * 48000));
  float maxDiff = 0;
  for (size_t i = 0; i < 48000; ++i) maxDiff = std::max(maxDiff, std::fabs(a[i] - ref[i + offset]));
  std::cout << "  max diff vs engine " << maxDiff << "\n";
  CHECK(maxDiff < 1e-6f);

  // Rounded PPQ must not skip/repeat samples at host buffer boundaries.
  // Keep sample time exact while alternating beat reports by +/- 10 samples.
  {
    juce::AudioBuffer<float> buf(2,512); juce::MidiBuffer midi;
    float jitterDiff = 0;
    for (int block=0;block<90;++block) {
      const int64_t sample=block*512;
      ph.sampleTime=sample;
      const int jitter=block==0?0:(block%2?10:-10);
      ph.ppq=(sample+jitter)/48000.0*2;
      p.processBlock(buf,midi);
      for(int i=0;i<512;++i) jitterDiff=std::max(jitterDiff,std::fabs(buf.getSample(0,i)-ref[offset+sample+i]));
    }
    std::cout << "  rounded PPQ max diff " << jitterDiff << "\n";
    CHECK(jitterDiff<1e-6f);
    // Even a small real timeline seek must follow the host sample clock.
    ph.sampleTime=48000; ph.ppq=2;
    p.processBlock(buf,midi);
    CHECK(std::fabs(buf.getSample(0,0)-ref[offset+48000])<1e-6f);
    ph.sampleTime.reset();
  }

  // The original clicking take was at 44.1 kHz. Verify the same host
  // rounding case at that rate, not just the default 48 kHz fixture.
  {
    NovoxProcessor rate441; FakePlayHead host441;
    rate441.setPlayHead(&host441); rate441.setRateAndBufferSizeDetails(44100,512);
    rate441.prepareToPlay(44100,512); rate441.setSongJson(songJson); waitForRender(rate441);
    novox_engine* engine441=novox_create(44100);
    novox_stream* stream441=novox_stream_create(engine441,Song::fromJson(songJson).toScoreJson(120).toRawUTF8());
    const auto pre441=static_cast<size_t>(std::llround(novox_stream_lead(stream441)*44100));
    std::vector<float> reference441(novox_stream_length(stream441));
    novox_stream_process(stream441,reference441.data(),reference441.size());
    juce::AudioBuffer<float> buf(2,512);juce::MidiBuffer midi;float difference441=0;
    for(int block=0;block<80;++block){
      const int64_t sample=block*512;host441.sampleTime=sample;
      const int jitter=block==0?0:(block%2?10:-10);
      host441.ppq=(sample+jitter)/44100.0*2;rate441.processBlock(buf,midi);
      for(int i=0;i<512;++i)difference441=std::max(difference441,std::fabs(buf.getSample(0,i)-reference441[pre441+sample+i]));
    }
    std::cout << "  44.1k rounded PPQ max diff " << difference441 << "\n";
    CHECK(difference441<1e-6f);
    novox_stream_destroy(stream441);novox_destroy(engine441);
  }

  // 2. Jumping to beat 2 (loop / scrub) plays exactly that part of the song.
  auto b = play(p, ph, 2.0, 0.5, 333);
  maxDiff = 0;
  for (size_t i = 0; i < b.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(b[i] - ref[i + offset + 48000]));
  CHECK(maxDiff < 1e-6f);

  // 3. Stopped transport is silent; far past the song is silent.
  ph.playing = false;
  CHECK(rms(play(p, ph, 0, 0.2)) == 0.0f);
  ph.playing = true;
  CHECK(rms(play(p, ph, 40, 0.2)) == 0.0f);

  // 4. A tempo change re-renders so notes stay on the beat: beat 2 at 60 bpm.
  ph.bpm = 60;
  play(p, ph, 0, 0.05);  // let the processor see the new tempo
  pump(0.1);
  waitForRender(p);
  auto c = play(p, ph, 2.0, 0.5);
  novox_stream* s60 = novox_stream_create(e, Song::fromJson(songJson).toScoreJson(60).toRawUTF8());
  std::vector<float> ref60(novox_stream_length(s60));
  novox_stream_process(s60, ref60.data(), ref60.size());
  const size_t off60 = static_cast<size_t>(std::llround(novox_stream_lead(s60) * 48000)) + 2 * 48000;
  novox_stream_destroy(s60);
  maxDiff = 0;
  for (size_t i = 0; i < c.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(c[i] - ref60[i + off60]));
  std::cout << "  tempo change max diff " << maxDiff << "\n";
  CHECK(maxDiff < 1e-6f);
  ph.bpm = 120;
  novox_destroy(e);

  // 5. MIDI capture: a note played at beat 8 for one beat lands in the song.
  p.setCaptureArmed(true);
  {
    juce::AudioBuffer<float> buf(2, 480);
    juce::MidiBuffer midi;
    ph.ppq = 8.0;
    midi.addEvent(juce::MidiMessage::noteOn(1, 67, 0.8f), 0);
    p.processBlock(buf, midi);
    midi.clear();
    ph.ppq = 9.0;
    midi.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
    p.processBlock(buf, midi);
  }
  pump(0.2);
  const auto song = Song::fromJson(p.getSongJson());
  bool found = false;
  for (const auto& n : song.notes)
    if (n.pitch == 67 && std::fabs(n.start - 8.0) < 1e-6 && std::fabs(n.dur - 1.0) < 1e-6) found = true;
  CHECK(found);
  CHECK(song.notes.size() == 5);
  p.setCaptureArmed(false);

  // 6. State round trip into a fresh instance.
  juce::MemoryBlock state;
  p.getStateInformation(state);
  NovoxProcessor q;
  q.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  CHECK(Song::fromJson(q.getSongJson()).notes.size() == 5);
  CHECK(q.getSongJson() == p.getSongJson());

  // Legacy Nvsg project state: old marker and unchanged JUCE string envelope.
  juce::MemoryBlock legacyState;
  {juce::MemoryOutputStream legacy(legacyState,false);
   legacy.writeString(p.getSongJson().replace("novox-song","new-virtual-singer-song"));}
  q.setStateInformation(legacyState.getData(),static_cast<int>(legacyState.getSize()));
  CHECK(q.getSongJson()==p.getSongJson());

  // Host timeline seconds/origin reach the editor so stamped audio can be placed;
  // MIDI imports keep the file's beat positions regardless of its tempo map.
  {
    ph.ppq=28; ph.seconds=14.2641; ph.editOrigin=3600.0;
    juce::AudioBuffer<float> buf(2,512); juce::MidiBuffer midi;
    p.processBlock(buf,midi);
    const auto st=p.getStatus();
    CHECK(std::fabs(st.beats-28)<1e-9 && std::fabs(st.seconds-14.2641)<1e-9 && std::fabs(st.editOrigin-3600)<1e-9);
    ph.seconds.reset(); ph.editOrigin.reset();
    p.processBlock(buf,midi);
    CHECK(std::isnan(p.getStatus().seconds) && std::isnan(p.getStatus().editOrigin));
    // 480 ticks/beat: 120 bpm, then 106 bpm at beat 24; a note at beat 28.
    const unsigned char midiFile[]={'M','T','h','d',0,0,0,6,0,0,0,1,0x01,0xE0,'M','T','r','k',0,0,0,29,
      0x00,0xFF,0x51,0x03,0x07,0xA1,0x20, 0xDA,0x00,0xFF,0x51,0x03,0x08,0xA3,0x16,
      0x8F,0x00,0x90,62,100, 0x83,0x60,0x80,62,0, 0x00,0xFF,0x2F,0x00};
    juce::String error;
    auto imported=juce::JSON::parse(p.importFile(juce::MemoryBlock(midiFile,sizeof(midiFile)),"",error));
    CHECK(error.isEmpty() && imported["units"].toString()=="beats");
    auto* notes=imported["notes"].getArray();
    CHECK(notes && notes->size()==1 && std::fabs(static_cast<double>((*notes)[0]["start"])-28)<1e-6);
  }

  // A VST3 host with no beat position (MuseScore 4: its sample position, from 4.7 a playing flag,
  // and calls only while it plays) is followed by its sample clock while it plays: seconds from its
  // start at the song's own tempo, which an imported score sets unrounded. A host that has given a
  // beat position is a DAW and keeps the beats path; the standalone app and AUs keep their own
  // transport, and so does any other VST3 host while it doesn't say it plays.
  {
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    NovoxProcessor vst;
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    CHECK(!vst.hostIsMuseScore() && juce::String(vst.getStatus().clock) == "own");
    vst.setNotationHost(true);  // as in MuseScore, known by its path
    FakePlayHead score;  // the notation app
    score.beats = false;
    score.playing = false;
    vst.setPlayHead(&score);
    vst.setRateAndBufferSizeDetails(48000, 512);
    vst.prepareToPlay(48000, 512);
    vst.setSongJson(songJson);
    vst.setPreviewTempo(66.67);
    waitForRender(vst);
    // Not called yet: MuseScore's clock all the same (its Play and ruler say to play the score).
    CHECK(vst.inDaw() && juce::String(vst.getStatus().clock) == "samples" && !vst.getStatus().playing);
    // Not called while stopped: a changed tempo renders once, not on every timer tick.
    const int started = vst.getStatus().rendersStarted;
    pump(0.3);
    CHECK(vst.getStatus().rendersStarted == started && std::fabs(vst.getStatus().bpm - 66.67) < 1e-9);
    novox_engine* engine = novox_create(48000);
    novox_stream* stream = novox_stream_create(engine, Song::fromJson(songJson).toScoreJson(66.67).toRawUTF8());
    const auto lead = static_cast<size_t>(std::llround(novox_stream_lead(stream) * 48000));
    std::vector<float> reference(novox_stream_length(stream));
    novox_stream_process(stream, reference.data(), reference.size());
    novox_stream_destroy(stream);
    novox_destroy(engine);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    // One host block of `n` samples reported at sample `at`; how far it is from the engine's render
    // at sample `heard` (where Novox should be: `at` unless it keeps its own count).
    const auto block = [&](int64_t at, int n = 512, int64_t heard = -1) {
      score.sampleTime = at;
      buf.setSize(2, n, false, false, true);
      vst.processBlock(buf, midi);
      float diff = 0;
      for (int i = 0; i < n; ++i) {
        const size_t k = lead + static_cast<size_t>(heard >= 0 ? heard : at) + static_cast<size_t>(i);
        diff = std::max(diff, std::fabs(buf.getSample(0, i) - (k < reference.size() ? reference[k] : 0.0f)));
      }
      return diff;
    };
    const auto silent = [&] { return rms({buf.getReadPointer(0), buf.getReadPointer(0) + buf.getNumSamples()}) == 0.0f; };
    // MuseScore 4.6 sends no playing flag: the first block can't tell, the next one carries straight on.
    block(24000);
    auto st = vst.getStatus();
    CHECK(juce::String(st.clock) == "samples" && !st.playing && !st.hostTransport && silent());
    float diff = 0;
    std::vector<float> heard;
    for (int b = 1; b <= 40; ++b) {
      diff = std::max(diff, block(24000 + b * 512));
      heard.insert(heard.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
    }
    st = vst.getStatus();
    std::cout << "  sample clock max diff " << diff << "\n";
    CHECK(st.playing && rms(heard) > 0.01f && diff < 1e-6f);
    CHECK(std::fabs(st.beats - (24000 + 40 * 512) / 48000.0 * 66.67 / 60) < 1e-9 &&
          std::fabs(st.seconds - (24000 + 40 * 512) / 48000.0) < 1e-9);
    // 4.6 rounds its position to whole microseconds, and gives the parts of a block split at a note
    // event the same one: Novox keeps its own count, without a gap or a jump.
    int64_t at = 24000 + 41 * 512;
    diff = 0;
    for (int b = 0; b < 30; ++b, at += 512) {
      const int64_t jitter = b % 3 == 0 ? 2 : b % 3 == 1 ? -1 : 0;
      if (b % 5 == 4) {
        diff = std::max(diff, block(at + jitter, 200, at));
        CHECK(!silent());
        diff = std::max(diff, block(at + jitter, 312, at + 200));
      } else {
        diff = std::max(diff, block(at + jitter, 512, at));
      }
      CHECK(vst.getStatus().playing && !silent());
    }
    CHECK(diff < 1e-6f);
    // 4.6 counts each block in whole microseconds (10666 of 10666.67 for 512 samples) and reports
    // it in float seconds: half a minute on, Novox still follows, within a few samples of its clock.
    {
      const int64_t from = 24000 + 71 * 512;
      bool followed = true;
      double furthest = 0;
      for (int64_t k = 0; k < 3000; ++k) {
        const int64_t micros = from * 1000000 / 48000 + k * (512 * 1000000 / 48000);
        block(static_cast<int64_t>(static_cast<float>(micros) / 1000000.f * 48000));
        const auto now = vst.getStatus();
        followed = followed && now.playing;
        furthest = std::max(furthest, std::fabs(now.seconds * 48000 - static_cast<double>(score.sampleTime.orFallback(0))));
      }
      std::cout << "  4.6 clock: furthest " << furthest << " samples from the host's\n";
      CHECK(followed && furthest <= 16);
    }
    // Stopped: MuseScore calls the plugin no more, and makes no last call. Novox stops counting it
    // as playing within a few timer ticks, so note previews and Compare sound again.
    pump(0.4);
    CHECK(!vst.getStatus().playing && juce::String(vst.getStatus().clock) == "samples");
    // A host that repeats one position for longer than a split block isn't playing.
    for (int b = 0; b < 12; ++b) block(60000);
    CHECK(!vst.getStatus().playing && silent());
    // MuseScore 4.7's playing flag: it sounds from the first block, also after a jump ...
    score.playing = true;
    diff = block(96000);
    for (int b = 1; b < 8; ++b) diff = std::max(diff, block(96000 + b * 512));
    CHECK(vst.getStatus().playing && diff < 1e-6f);
    // ... and its last call says it stopped, even though the block carries on.
    score.playing = false;
    block(96000 + 8 * 512);
    CHECK(!vst.getStatus().playing && silent() && juce::String(vst.getStatus().clock) == "samples");
    block(96000 + 9 * 512);
    CHECK(!vst.getStatus().playing);  // the flag is trusted from now on
    // MIDI capture lands on the song's beats from the sample clock: 2 s at 66.67 bpm.
    score.playing = true;
    vst.setCaptureArmed(true);
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);
    block(96000);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    block(96000 + 24000);
    midi.clear();
    vst.setCaptureArmed(false);
    pump(0.2);
    bool captured = false;
    for (const auto& note : Song::fromJson(vst.getSongJson()).notes)
      captured = captured || (note.pitch == 69 && std::fabs(note.start - 2 * 66.67 / 60) < 1e-6 && std::fabs(note.dur - 0.5 * 66.67 / 60) < 1e-6);
    CHECK(captured);
    // A host that gives a beat position is a DAW: its blocks without one fall back to the own
    // transport, as before, never the sample clock.
    score.beats = true;
    score.ppq = 4;
    block(0);
    CHECK(juce::String(vst.getStatus().clock) == "beats" && vst.getStatus().hostTransport && vst.getStatus().hostTempo);
    score.beats = false;
    score.playing = false;
    block(512);
    block(1024);
    CHECK(juce::String(vst.getStatus().clock) == "own" && !vst.getStatus().playing && !vst.getStatus().hostTempo);
  }
  // Any other VST3 host that gives no beat position: its sample clock only while it says it plays,
  // else Novox's own transport, which plays from its Play button as it did before.
  {
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
    NovoxProcessor vst;
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    FakePlayHead live;  // a live-hosting app: its sample counter runs, it never says it plays
    live.beats = false;
    live.playing = false;
    vst.setPlayHead(&live);
    vst.setRateAndBufferSizeDetails(48000, 512);
    vst.prepareToPlay(48000, 512);
    vst.setSongJson(songJson);
    waitForRender(vst);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    std::vector<float> heard;
    const auto blocks = [&](int64_t from, int count) {
      heard.clear();
      for (int b = 0; b < count; ++b) {
        live.sampleTime = from + b * 512;
        vst.processBlock(buf, midi);
        heard.insert(heard.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
      }
    };
    blocks(480000, 20);
    CHECK(juce::String(vst.getStatus().clock) == "own" && !vst.getStatus().playing && rms(heard) == 0.0f);
    vst.setPreviewPlaying(true);
    blocks(480000 + 20 * 512, 200);
    CHECK(juce::String(vst.getStatus().clock) == "own" && vst.getStatus().playing && rms(heard) > 0.01f);
    vst.setPreviewPlaying(false);
    // Saying it plays, it is followed by its clock; stopped again, the own transport is back.
    live.playing = true;
    blocks(0, 4);
    CHECK(juce::String(vst.getStatus().clock) == "samples" && vst.getStatus().playing);
    live.playing = false;
    blocks(4 * 512, 4);
    CHECK(juce::String(vst.getStatus().clock) == "own" && !vst.getStatus().playing && rms(heard) == 0.0f);
    pump(0.3);
    CHECK(juce::String(vst.getStatus().clock) == "own");
  }
  {
    // Outside a VST3 a playhead without beats is ignored, as before: in the standalone app (and
    // these tests), and in an AU, whose host without transport callbacks (auval, live-audio apps)
    // JUCE gives a free-running render clock that would read as playing.
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    FakePlayHead none;
    none.beats = false;
    none.playing = false;
    NovoxProcessor app;
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_AudioUnit);
    NovoxProcessor au;
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    for (auto* other : {&app, &au}) {
      other->setPlayHead(&none);
      other->prepareToPlay(48000, 512);
      for (int b = 0; b < 4; ++b) {
        none.sampleTime = 4800 + b * 512;
        other->processBlock(buf, midi);
      }
      CHECK(juce::String(other->getStatus().clock) == "own" && !other->getStatus().playing);
    }
    CHECK(!app.inDaw() && au.inDaw());
  }

  // Compressed MusicXML (.mxl) as MuseScore and Dorico export it: unzipped in the plugin (the
  // rootfile META-INF/container.xml names, else the first score inside) and imported with its own
  // lyrics, one syllable per note: the song's lyric line is not echoed back.
  {
    const auto note = [](const char* step, const char* syllabic, const char* text) {
      return juce::String("<note><pitch><step>") + step + "</step><octave>4</octave></pitch><duration>1</duration>"
             "<voice>1</voice><lyric number=\"1\"><syllabic>" + syllabic + "</syllabic><text>" + text + "</text></lyric></note>";
    };
    const auto score = [](const juce::String& notes) {
      return R"(<?xml version="1.0" encoding="UTF-8"?><score-partwise version="4.0"><part-list><score-part id="P1">)"
             R"(<part-name>Voice</part-name></score-part></part-list><part id="P1"><measure number="1"><attributes>)"
             R"(<divisions>1</divisions></attributes><sound tempo="66.67"/>)" + notes + "</measure></part></score-partwise>";
    };
    const auto song = score(note("C", "single", "the") + note("D", "single", "fire") + note("E", "begin", "burn") + note("F", "end", "ing"));
    const auto other = score(note("G", "single", "decoy"));
    const juce::String container = R"(<?xml version="1.0" encoding="UTF-8"?><container><rootfiles>)"
                                   R"(<rootfile full-path="score/Song.musicxml" media-type="application/vnd.recordare.musicxml+xml"/>)"
                                   R"(</rootfiles></container>)";
    const auto zip = [](const std::vector<std::pair<juce::String, juce::String>>& entries) {
      juce::ZipFile::Builder builder;
      for (const auto& [name, text] : entries)
        builder.addEntry(std::make_unique<juce::MemoryInputStream>(text.toRawUTF8(), text.getNumBytesAsUTF8(), true), 9, name,
                         juce::Time(2026, 9, 21, 12, 0));
      juce::MemoryBlock out;
      {
        juce::MemoryOutputStream stream(out, false);
        builder.writeToStream(stream, nullptr);
      }
      return out;
    };
    const auto lyricsOf = [](const juce::var& imported) {
      juce::StringArray sung;
      if (const auto* notes = imported["notes"].getArray())
        for (const auto& n : *notes) sung.add(n["lyric"].toString());
      return sung.joinIntoString(" ");
    };
    juce::String error;
    // The container names the score; a decoy listed before it is not read.
    const auto mxl = zip({{"mimetype", "application/vnd.recordare.musicxml"}, {"aaa.xml", other},
                          {"META-INF/container.xml", container}, {"score/Song.musicxml", song}});
    CHECK(mxl.getSize() > 4 && mxl[0] == 'P' && mxl[1] == 'K');
    const auto unzipped = NovoxProcessor::musicXmlFromMxl(mxl, error);
    CHECK(error.isEmpty() && unzipped.toString() == song);
    auto imported = juce::JSON::parse(p.importFile(mxl, "hel-lo world", error));
    CHECK(error.isEmpty() && imported["units"].toString() == "beats" && !imported.hasProperty("lyrics"));
    CHECK(lyricsOf(imported) == "the fire burn- ing");
    CHECK(imported["import"]["format"].toString() == "musicxml" && static_cast<bool>(imported["import"]["fileLyrics"]));
    CHECK(std::fabs(static_cast<double>(imported["tempo"]) - 66.67) < 1e-9);
    // Without a container: the first score outside META-INF.
    imported = juce::JSON::parse(p.importFile(zip({{"META-INF/other.txt", "x"}, {"Song.xml", song}, {"zzz.musicxml", other}}), "", error));
    CHECK(error.isEmpty() && lyricsOf(imported) == "the fire burn- ing");
    // The editor's part, voice and verse choice reaches the engine; a bad one comes back as its error.
    imported = juce::JSON::parse(p.importFile(mxl, "", error, juce::JSON::parse(R"({"track": 0, "partVoice": "1", "verse": 1})")));
    CHECK(error.isEmpty() && lyricsOf(imported) == "the fire burn- ing" && imported["import"]["partVoice"].toString() == "1");
    CHECK(p.importFile(mxl, "", error, juce::JSON::parse(R"({"track": 3})")).isEmpty() && error.contains("part 3"));
    // Errors say what is wrong instead of an empty result.
    const char junk[] = "PK not really a zip";
    CHECK(p.importFile(juce::MemoryBlock(junk, sizeof(junk)), "", error).isEmpty() && error.contains(".mxl"));
    CHECK(p.importFile(zip({{"META-INF/container.xml", container}, {"notes.txt", "la"}}), "", error).isEmpty() &&
          error.contains("no MusicXML score"));
    const char text[] = "hello";
    CHECK(p.importFile(juce::MemoryBlock(text, 5), "", error).isEmpty() && error.isNotEmpty());
  }

  // 7. Plan and voices reach the UI.
  juce::String err;
  CHECK(p.planJson(err).contains("syllables"));
  CHECK(p.voiceNames().contains("robot"));
  auto natural = juce::JSON::parse(p.lyricTokens("remember", "{}", err))["tokens"];
  CHECK(err.isEmpty() && natural.size() == 3 && natural[0].toString() == "remember-" && natural[2].toString().isEmpty());
  auto hyphenated = juce::JSON::parse(p.lyricTokens("re-mem-ber", "{}", err))["tokens"];
  CHECK(err.isEmpty() && hyphenated.size() == 3 && hyphenated[1].toString() == "mem-");
  auto custom = juce::JSON::parse(p.lyricTokens("cat", R"({"cat":"K AE1 T AH0 IY0"})", err))["tokens"];
  CHECK(err.isEmpty() && custom.size() == 3);
  CHECK(juce::JSON::parse(q.lyricTokens("cat", "{}", err))["tokens"].size() == 1);
  CHECK(p.lyricTokens("cat", "[1]", err).isEmpty() && err.isNotEmpty());
  CHECK(p.lyricTokens("cat", "{}", err).isNotEmpty() && err.isEmpty());
  const auto beforeLyricPreview = p.getSongJson();
  const auto beforeVersion = p.getStatus().songVersion;
  const auto preview = juce::JSON::parse(p.planSongJson(R"({"lyrics":"remember","notes":[{"pitch":60,"start":8,"dur":1},{"pitch":62,"start":9,"dur":1},{"pitch":64,"start":10,"dur":1}]})", 120, err));
  CHECK(err.isEmpty() && preview["syllables"].size() == 3 && preview["syllables"][0]["text"].toString() == "remember");
  CHECK(std::fabs(static_cast<double>(preview["syllables"][0]["start"]) - 4) < 1e-6);
  CHECK(p.getSongJson() == beforeLyricPreview && p.getStatus().songVersion == beforeVersion);
  for (const auto* bad : {"{", "[]", "{}", R"({"notes":[false]})", R"({"notes":[{"dur":"bad"}]})"})
    CHECK(p.planSongJson(bad, 120, err).isEmpty() && err.isNotEmpty());
  CHECK(p.planSongJson(beforeLyricPreview, 0, err).isEmpty() && err.isNotEmpty());
  CHECK(p.planSongJson(beforeLyricPreview, 1001, err).isEmpty() && err.isNotEmpty());
  CHECK(p.planSongJson(beforeLyricPreview, NAN, err).isEmpty() && err.isNotEmpty());
  CHECK(p.planSongJson(beforeLyricPreview, 120, err).isNotEmpty() && err.isEmpty());

  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
