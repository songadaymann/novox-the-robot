#include "PluginProcessor.h"

#include "PluginEditor.h"

// ============================================================================
// SharedEngine

std::shared_ptr<SharedEngine> SharedEngine::get() {
  static std::mutex m;
  static std::weak_ptr<SharedEngine> instance;
  std::lock_guard<std::mutex> lock(m);
  auto e = instance.lock();
  if (!e) {
    e = std::shared_ptr<SharedEngine>(new SharedEngine());
    instance = e;
  }
  return e;
}

SharedEngine::SharedEngine() { engine_ = novox_create(48000); }
SharedEngine::~SharedEngine() { novox_destroy(engine_); }

bool SharedEngine::render(const juce::String& scoreJson, double sampleRate, std::vector<float>& out, double& lead,
                          juce::String& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  novox_set_sample_rate(engine_, sampleRate);
  novox_stream* s = novox_stream_create(engine_, scoreJson.toRawUTF8());
  if (!s) {
    error = novox_last_error(engine_);
    return false;
  }
  out.assign(novox_stream_length(s), 0.0f);
  novox_stream_process(s, out.data(), out.size());
  lead = novox_stream_lead(s);
  novox_stream_destroy(s);
  return true;
}

juce::String SharedEngine::plan(const juce::String& scoreJson, juce::String& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  char* p = novox_plan(engine_, scoreJson.toRawUTF8());
  if (!p) {
    error = novox_last_error(engine_);
    return {};
  }
  juce::String result = juce::String::fromUTF8(p);
  novox_free(p);
  return result;
}

juce::String SharedEngine::importFile(const juce::MemoryBlock& data, const juce::String& optionsJson,
                                      juce::String& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  char* p = novox_import(engine_, data.getData(), data.getSize(), optionsJson.toRawUTF8());
  if (!p) {
    error = novox_last_error(engine_);
    return {};
  }
  juce::String result = juce::String::fromUTF8(p);
  novox_free(p);
  return result;
}

juce::StringArray SharedEngine::voices() {
  char* names = novox_voice_names();
  juce::StringArray list = juce::StringArray::fromTokens(juce::String::fromUTF8(names), ",", "");
  novox_free(names);
  return list;
}

// ============================================================================
// Processor

namespace {

class RenderThread : public juce::Thread {
 public:
  explicit RenderThread(std::function<void()> body) : juce::Thread("Novox render"), body_(std::move(body)) {}
  void run() override { body_(); }

 private:
  std::function<void()> body_;
};

}  // namespace

NovoxProcessor::NovoxProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      engine_(SharedEngine::get()) {
  heldSince_.fill(-1.0);
  song_ = Song::defaultSong();
  worker_ = std::make_unique<RenderThread>([this] { renderWorker(); });
  worker_->startThread(juce::Thread::Priority::low);
  requestRender();
  startTimerHz(30);
}

NovoxProcessor::~NovoxProcessor() {
  stopTimer();
  worker_->signalThreadShouldExit();
  renderRequested_.signal();
  worker_->stopThread(5000);
}

bool NovoxProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  const auto& out = layouts.getMainOutputChannelSet();
  return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void NovoxProcessor::prepareToPlay(double sampleRate, int) {
  if (std::abs(sampleRate - sampleRate_.load()) > 0.5) {
    sampleRate_ = sampleRate;
    requestRender();
  }
}

// ---------------------------------------------------------------------------
// Audio thread

void NovoxProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  const int n = buffer.getNumSamples();
  const double sr = getSampleRate() > 0 ? getSampleRate() : sampleRate_.load();
  buffer.clear();

  // Where are we? Prefer the host transport; fall back to our own (standalone app).
  bool playing = false, host = false;
  double beats = 0, bpm = previewBpm_.load();
  if (auto* ph = getPlayHead()) {
    if (auto pos = ph->getPosition()) {
      if (auto ppq = pos->getPpqPosition()) {
        host = true;
        beats = *ppq;
        playing = pos->getIsPlaying();
        if (auto b = pos->getBpm()) bpm = *b;
      }
    }
  }
  if (!host) {
    playing = previewPlaying_.load();
    beats = previewSamples_ / sr * bpm / 60.0;
    if (playing) previewSamples_ += n;
  }
  hostTransport_ = host;
  playing_ = playing;
  playheadBeats_ = beats;
  hostBpm_ = bpm;

  // MIDI capture (only while the transport runs, so notes have a place in time).
  if (captureArmed_.load() && playing) {
    for (const auto meta : midi) {
      const auto msg = meta.getMessage();
      const double at = beats + meta.samplePosition / sr * bpm / 60.0;
      if (msg.isNoteOn()) {
        heldSince_[static_cast<size_t>(msg.getNoteNumber())] = at;
      } else if (msg.isNoteOff()) {
        auto& since = heldSince_[static_cast<size_t>(msg.getNoteNumber())];
        if (since >= 0 && at > since) {
          int start1, size1, start2, size2;
          captureFifo_.prepareToWrite(1, start1, size1, start2, size2);
          if (size1 > 0) {
            captureBuf_[static_cast<size_t>(start1)] = {msg.getNoteNumber(), since, at - since};
            captureFifo_.finishedWrite(1);
          }
        }
        since = -1.0;
      }
    }
  }

  if (!playing) return;

  std::shared_ptr<const Rendered> r;
  {
    const juce::SpinLock::ScopedTryLockType lock(renderedLock_);
    if (!lock.isLocked()) return;  // a new render is being swapped in; skip one block
    r = rendered_;
  }
  if (!r || r->audio.empty() || std::abs(r->sampleRate - sr) > 0.5) return;

  // Song time in the render: beats at the render's tempo, plus the pre-roll.
  const double seconds = beats * 60.0 / r->bpm + r->lead;
  const long long first = static_cast<long long>(std::llround(seconds * sr));
  const long long total = static_cast<long long>(r->audio.size());
  for (int i = 0; i < n; ++i) {
    const long long idx = first + i;
    if (idx < 0 || idx >= total) continue;
    const float s = r->audio[static_cast<size_t>(idx)];
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.setSample(ch, i, s);
  }
}

// ---------------------------------------------------------------------------
// Message thread

void NovoxProcessor::timerCallback() {
  // Captured notes -> song.
  int start1, size1, start2, size2;
  const int ready = captureFifo_.getNumReady();
  if (ready > 0) {
    captureFifo_.prepareToRead(ready, start1, size1, start2, size2);
    {
      std::lock_guard<std::mutex> lock(songMutex_);
      auto add = [&](int from, int count) {
        for (int i = 0; i < count; ++i) {
          const auto& c = captureBuf_[static_cast<size_t>(from + i)];
          SongNote note;
          note.pitch = c.pitch;
          note.start = c.start;
          note.dur = c.dur;
          song_.addNote(note);
        }
      };
      add(start1, size1);
      add(start2, size2);
    }
    captureFifo_.finishedRead(size1 + size2);
    ++songVersion_;
    requestRender();
  }

  // Tempo or sample-rate change -> re-render.
  {
    std::shared_ptr<const Rendered> r;
    {
      const juce::SpinLock::ScopedLockType lock(renderedLock_);
      r = rendered_;
    }
    if (r && !rendering_ &&
        (std::abs(r->bpm - hostBpm_.load()) > 0.005 || std::abs(r->sampleRate - sampleRate_.load()) > 0.5)) {
      requestRender();
    }
  }

  // Free renders the audio thread no longer uses.
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [](const std::shared_ptr<const Rendered>& p) { return p.use_count() == 1; }),
                 retired_.end());
}

void NovoxProcessor::requestRender() {
  rendering_ = true;
  renderRequested_.signal();
}

void NovoxProcessor::renderWorker() {
  while (!worker_->threadShouldExit()) {
    renderRequested_.wait(-1);
    if (worker_->threadShouldExit()) break;
    rendering_ = true;
    juce::String score;
    int version;
    const double bpm = hostTransport_.load() ? hostBpm_.load() : previewBpm_.load();
    const double sr = getSampleRate() > 0 ? getSampleRate() : sampleRate_.load();
    {
      std::lock_guard<std::mutex> lock(songMutex_);
      score = song_.toScoreJson(bpm);
      version = songVersion_.load();
    }
    auto r = std::make_shared<Rendered>();
    juce::String error;
    const bool ok = engine_->render(score, sr, r->audio, r->lead, error);
    {
      std::lock_guard<std::mutex> lock(errorMutex_);
      renderError_ = ok ? juce::String() : error;
    }
    if (ok) {
      r->sampleRate = sr;
      r->bpm = bpm;
      r->version = version;
      std::shared_ptr<const Rendered> old;
      {
        const juce::SpinLock::ScopedLockType lock(renderedLock_);
        old = std::move(rendered_);
        rendered_ = r;
      }
      // Hand the old render to the message thread to free (never on the audio thread).
      juce::MessageManager::callAsync([this, old]() mutable {
        if (old) retired_.push_back(std::move(old));
      });
    }
    rendering_ = false;
  }
}

juce::String NovoxProcessor::getSongJson() const {
  std::lock_guard<std::mutex> lock(songMutex_);
  return song_.toJson();
}

void NovoxProcessor::setSongJson(const juce::String& json) {
  {
    std::lock_guard<std::mutex> lock(songMutex_);
    song_ = Song::fromJson(json);
  }
  ++songVersion_;
  requestRender();
}

juce::String NovoxProcessor::planJson(juce::String& error) {
  juce::String score;
  {
    std::lock_guard<std::mutex> lock(songMutex_);
    score = song_.toScoreJson(hostTransport_.load() ? hostBpm_.load() : previewBpm_.load());
  }
  return engine_->plan(score, error);
}

juce::String NovoxProcessor::importFile(const juce::MemoryBlock& data, const juce::String& lyrics,
                                      juce::String& error) {
  auto* o = new juce::DynamicObject();
  o->setProperty("lyrics", lyrics);
  return engine_->importFile(data, juce::JSON::toString(juce::var(o)), error);
}

void NovoxProcessor::setPreviewPlaying(bool playing) {
  if (playing && !previewPlaying_.load()) previewSamples_ = 0;  // start from the top
  previewPlaying_ = playing;
}

void NovoxProcessor::setPreviewTempo(double bpm) {
  previewBpm_ = juce::jlimit(20.0, 400.0, bpm);
  if (!hostTransport_.load()) requestRender();
}

NovoxProcessor::Status NovoxProcessor::getStatus() const {
  Status s;
  s.beats = playheadBeats_.load();
  s.bpm = hostTransport_.load() ? hostBpm_.load() : previewBpm_.load();
  s.playing = playing_.load();
  s.hostTransport = hostTransport_.load();
  s.capture = captureArmed_.load();
  s.rendering = rendering_.load();
  s.songVersion = songVersion_.load();
  {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(errorMutex_));
    s.error = renderError_;
  }
  return s;
}

// ---------------------------------------------------------------------------
// State

void NovoxProcessor::getStateInformation(juce::MemoryBlock& destData) {
  juce::MemoryOutputStream(destData, false).writeString(getSongJson());
}

void NovoxProcessor::setStateInformation(const void* data, int sizeInBytes) {
  juce::MemoryInputStream in(data, static_cast<size_t>(sizeInBytes), false);
  const juce::String json = in.readString();
  if (json.isNotEmpty()) {
    {
      std::lock_guard<std::mutex> lock(songMutex_);
      song_ = Song::fromJson(json);
    }
    ++songVersion_;
    requestRender();  // thread-safe: just wakes the render thread
  }
}

juce::AudioProcessorEditor* NovoxProcessor::createEditor() { return new NovoxEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NovoxProcessor(); }
