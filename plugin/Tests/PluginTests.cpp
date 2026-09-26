// Hosts NovoxProcessor with a fake DAW playhead and checks transport sync,
// tempo changes, MIDI capture and state save/restore.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <iostream>

#include "../Source/PluginProcessor.h"
#include "novox.h"

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
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo p;
    p.setPpqPosition(ppq);
    p.setBpm(bpm);
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

}  // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI init;
  const juce::String songJson =
      R"({"lyrics": "la la la la", "voice": "std", "notes": [
        {"pitch": 60, "start": 0, "dur": 1}, {"pitch": 62, "start": 1, "dur": 1},
        {"pitch": 64, "start": 2, "dur": 1}, {"pitch": 65, "start": 3, "dur": 1}]})";

  NovoxProcessor p;
  FakePlayHead ph;
  p.setPlayHead(&ph);
  p.setRateAndBufferSizeDetails(48000, 512);
  p.prepareToPlay(48000, 512);
  p.setSongJson(songJson);
  waitForRender(p);

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

  // 7. Plan and voices reach the UI.
  juce::String err;
  CHECK(p.planJson(err).contains("syllables"));
  CHECK(p.voiceNames().contains("robot"));

  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
