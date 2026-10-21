#include "PluginProcessor.h"

#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

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
                          juce::String& error, const std::function<bool()>& cancelled) {
  novox_stream* s = nullptr;
  {
    // A stream owns its plan and voice once created; only planning reads the engine.
    std::lock_guard<std::mutex> lock(mutex_);
    novox_set_sample_rate(engine_, sampleRate);
    s = novox_stream_create(engine_, scoreJson.toRawUTF8());
    if (!s) {
      error = novox_last_error(engine_);
      return false;
    }
  }
  std::unique_ptr<novox_stream, decltype(&novox_stream_destroy)> stream(s, novox_stream_destroy);
  out.assign(novox_stream_length(s), 0.0f);
  constexpr size_t block = 16384;  // ~0.35 s of audio, a few ms to render
  for (size_t at = 0; at < out.size(); at += block) {
    if (cancelled && cancelled()) return false;
    novox_stream_process(s, out.data() + at, std::min(block, out.size() - at));
  }
  lead = novox_stream_lead(s);
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

juce::String SharedEngine::lyricTokens(const juce::String& text, const juce::String& dictionaryJson,
                                      juce::String& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  error.clear();
  char* p = novox_lyric_tokens(engine_, text.toRawUTF8(), dictionaryJson.toRawUTF8());
  if (!p) { error = novox_last_error(engine_); return {}; }
  const auto result = juce::String::fromUTF8(p);
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

// A render is cancelled for a newer song only while the last finished one is this recent.
constexpr double kStaleRenderMs = 1000;

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
  setNotationHost(wrapperType == wrapperType_VST3 && hostIsMuseScore());
  song_ = Song::defaultSong();
  worker_ = std::make_unique<RenderThread>([this] { renderWorker(); });
  // Edits depend on this result immediately. macOS maps low to utility QoS, which can
  // starve even a subsecond render for tens of seconds on a busy machine. Normal uses
  // ordinary default QoS; this remains a non-realtime worker below the DAW audio thread.
  worker_->startThread(juce::Thread::Priority::normal);
  requestRender();
  startTimerHz(30);
  // The MMC port opens with the instance, so the DAW has picked it up before the first message.
  if (inDaw()) {
    if (juce::MessageManager::existsAndIsCurrentThread()) mmc_ = HostMmc::open();
    else juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(alive_)] { if (alive.lock()) mmc_ = HostMmc::open(); });
  }
}

NovoxProcessor::~NovoxProcessor() {
  alive_.reset();
  stopTimer();
  worker_->signalThreadShouldExit();
  renderRequested_.signal();
  worker_->stopThread(5000);
}

bool NovoxProcessor::hostIsMuseScore() {
  const auto path = juce::PluginHostType::getHostPath();
  return path.containsIgnoreCase("MuseScore") || juce::File(path).getFileName().startsWithIgnoreCase("mscore");
}

void NovoxProcessor::setNotationHost(bool museScore) {
  museScore_ = museScore;
  if (!hostTransport_.load()) clock_ = museScore ? 2 : 0;
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
  bool playing = false, host = false, samplesClock = false, hostTempo = false;
  double beats = 0, bpm = previewBpm_.load(), hostSeconds = NAN, origin = NAN;
  juce::Optional<int64_t> hostSamples;
  if (auto* ph = getPlayHead()) {
    if (auto pos = ph->getPosition()) {
      if (auto ppq = pos->getPpqPosition()) {
        host = true;
        hostGavePpq_ = true;
        hostSamples = pos->getTimeInSamples();
        beats = *ppq;
        playing = pos->getIsPlaying();
        if (auto b = pos->getBpm()) {
          bpm = *b;
          hostTempo = true;
        }
        // Seconds paired with ppq let imports place timestamped audio on the host timeline.
        if (auto t = pos->getTimeInSeconds()) hostSeconds = *t;
        if (auto o = pos->getEditOriginTime()) origin = *o;
        // JUCE's AU wrapper reads the render timestamp's SMPTE type even when the host leaves it
        // unset, which reads as plain 24: from an AU that says nothing.
        if (auto f = pos->getFrameRate()) {
          const bool untold = wrapperType == wrapperType_AudioUnit && f->getBaseRate() == 24 && !f->isDrop() && !f->isPullDown();
          hostFrameRate_ = untold ? 0 : f->getBaseRate() | (f->isDrop() ? 1 << 8 : 0) | (f->isPullDown() ? 1 << 9 : 0);
        }
      } else if (const auto at = pos->getTimeInSamples(); at && wrapperType == wrapperType_VST3 && !hostGavePpq_) {
        // A VST3 host with no beat position (MuseScore 4 sends only its sample position, from 4.7
        // a playing flag, and calls instruments only while it plays): while it plays, follow its
        // sample clock, seconds from its start at the song's own tempo. JUCE fills in a sample time
        // and a playing flag even for a host that sends neither, so a host plays when it says so
        // (one that has ever said so is trusted from then on), or, MuseScore before 4.7, while its
        // blocks carry on from one another. 4.6 gives a block the position of its end in whole
        // microseconds (each block counted a little short), and the parts of a block split at note
        // events the same one: a position a few samples from where the last block ended, or the
        // last one again, carries on, and Novox keeps its own count there, a sample nearer the
        // host's whenever they drift apart. Any other host keeps Novox's own transport while it
        // doesn't play, as before (a host whose clock runs while stopped would otherwise play the
        // song). Not for AU: JUCE gives a host without transport callbacks (auval, live-audio apps)
        // a free-running clock.
        if (pos->getIsPlaying()) hostFlagsPlaying_ = true;
        const long long tolerance = std::max(16, n / 8), split = std::max(4096LL, 4LL * n);
        const bool again = *at == sampleClockLast_;
        const bool carriesOn = sampleClockNext_ >= 0 && (std::llabs(*at - sampleClockNext_) <= tolerance ||
                                                         (again && sampleClockSame_ < split));
        const bool follows = hostFlagsPlaying_ ? pos->getIsPlaying() : museScore_.load() && carriesOn;
        const long long drift = again ? 0 : *at - sampleClockNext_;
        const long long position = follows && carriesOn ? sampleClockNext_ + (drift > 8 ? 1 : drift < -8 ? -1 : 0)
                                                        : static_cast<long long>(*at);
        sampleClockSame_ = (again ? sampleClockSame_ : 0) + n;
        sampleClockLast_ = *at;
        sampleClockNext_ = position + n;
        if (follows) {
          samplesClock = playing = true;
          hostSamples = position;
          hostSeconds = static_cast<double>(position) / sr;
          beats = hostSeconds * bpm / 60.0;
        }
      }
    }
  }
  if (!host && !samplesClock) {
    playing = previewPlaying_.load();
    if (const double seek = previewSeek_.exchange(-1); seek >= 0) previewSamples_ = seek * 60.0 / bpm * sr;
    beats = previewSamples_ / sr * bpm / 60.0;
    if (playing) previewSamples_ += n;
  }
  hostTransport_ = host;
  hostTempo_ = hostTempo;
  // MuseScore keeps "samples" while stopped: it doesn't call the plugin then, so Novox's own
  // transport can't play there.
  clock_ = host ? 1 : samplesClock || (museScore_.load() && !hostGavePpq_) ? 2 : 0;
  playing_ = playing;
  playheadBeats_ = beats;
  blocks_.fetch_add(1, std::memory_order_relaxed);
  playheadSeconds_ = hostSeconds;
  editOrigin_ = origin;
  hostBpm_ = bpm;

  // Review audition is routed through the plugin's own output, even with the
  // host stopped. No allocation or ownership changes on the audio thread. While
  // the transport runs, note previews and loops end; and when it starts, any
  // review ends: a one-shot review is a copy of an earlier render that is not in
  // step with the DAW, and it would play instead of the song until its end.
  const bool started = playing && !wasPlaying_;
  if (started) transportStarted_ = true;
  wasPlaying_ = playing;
  {
    const juce::SpinLock::ScopedTryLockType lock(auditionLock_);
    if (lock.isLocked() && audition_ && playing && (audition_->notePreview || audition_->loop || transportStarted_)) {
      audition_->position = static_cast<double>(audition_->samples.size());
      audition_->loop = false;
    }
    if (lock.isLocked()) transportStarted_ = false;
    if (lock.isLocked() && audition_ && (audition_->loop || audition_->position < (audition_->end > 0 ? audition_->end : audition_->samples.size()))) {
      auto& a = *audition_;
      const double step = a.rate / sr;
      for (int i = 0; i < n; ++i) {
        const double finish = a.end > 0 ? a.end : a.samples.size();
        if (a.loop && a.position >= finish) a.position = a.begin + std::fmod(a.position - a.begin, finish - a.begin);
        const auto at = static_cast<size_t>(a.position);
        float value = 0;
        if (at < a.samples.size() && a.position < finish) {
          const auto next = a.loop && at + 1 >= finish ? static_cast<size_t>(a.begin) : std::min(at + 1, a.samples.size() - 1);
          value = a.samples[at] + static_cast<float>(a.position - at) * (a.samples[next] - a.samples[at]);
        }
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.setSample(ch, i, value);
        a.position += step;
      }
      cursorValid_ = false;
      return;
    }
  }
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

  if (!playing) { cursorValid_ = false; return; }

  // An offline bounce or render (isNonRealtime: a DAW's offline bounce, a freeze) doesn't wait for
  // the render of a song it has just loaded or changed, so the file would be silent or stale where
  // the song sings: wait for it here. Blocking is fine off the real-time path. (MuseScore's export
  // runs in real time, so it isn't one of these.)
  if (isNonRealtime()) waitForRender(sr, started);

  std::shared_ptr<const Rendered> r;
  {
    const juce::SpinLock::ScopedTryLockType lock(renderedLock_);
    if (!lock.isLocked()) return;  // a new render is being swapped in; skip one block
    r = rendered_;
  }
  if (!r || r->audio.empty() || std::abs(r->sampleRate - sr) > 0.5) { cursorValid_ = false; return; }

  // Song time in the render: beats at the render's tempo, plus the pre-roll.
  const double seconds = beats * 60.0 / r->bpm + r->lead;
  const long long requested = static_cast<long long>(std::llround(seconds * sr));
  long long first = requested;
  // Follow contiguous host sample time. Allow beat quantisation up to two MIDI
  // ticks (1/480 beat); larger changes still seek/loop immediately.
  const double tolerance = std::max(2.0, sr * 60.0 / r->bpm / 480.0);
  const bool contiguous = hostSamples && cursorHasHostSamples_
      ? *hostSamples == nextHostSample_ : !hostSamples && !cursorHasHostSamples_;
  if (cursorValid_ && cursorRender_ == r.get() && contiguous &&
      std::abs(static_cast<double>(requested - nextRenderSample_)) <= tolerance)
    first = nextRenderSample_;
  cursorRender_ = r.get(); cursorValid_ = true;
  nextRenderSample_ = first + n;
  cursorHasHostSamples_ = hostSamples.hasValue();
  if (hostSamples) nextHostSample_ = *hostSamples + n;
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
      ++songVersion_;
      ++audioRevision_;
      ++stateEdits_;
    }
    captureFifo_.finishedRead(size1 + size2);
    requestRender();
  }

  // A host followed by its sample clock that stops calling the plugin (MuseScore, stopped: it makes
  // no last call) leaves `playing` as its last block set it. Not playing once no block has come for
  // a while: note previews and Compare sound again, and renders can be cancelled.
  {
    const auto blocks = blocks_.load(std::memory_order_relaxed);
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (blocks != blocksSeen_) {
      blocksSeen_ = blocks;
      blocksSeenAt_ = now;
    } else if (clock_.load() == 2 && playing_.load() && now - blocksSeenAt_ > kHostGoneMs) {
      playing_ = false;
    }
  }

  // Tempo or sample-rate change -> re-render. Without a host tempo the song's own is the one
  // rendered (as renderWorker does): a host that isn't calling the plugin (MuseScore while
  // stopped) leaves hostBpm_ where it was, and must not keep a changed tempo re-rendering.
  {
    std::shared_ptr<const Rendered> r;
    {
      const juce::SpinLock::ScopedLockType lock(renderedLock_);
      r = rendered_;
    }
    const double bpm = hostTransport_.load() ? hostBpm_.load() : previewBpm_.load();
    if (r && !rendering_ &&
        (std::abs(r->bpm - bpm) > 0.005 || std::abs(r->sampleRate - sampleRate_.load()) > 0.5)) {
      requestRender();
    }
  }

  // Free renders the audio thread no longer uses.
  {
    const std::lock_guard<std::mutex> lock(retiredMutex_);
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [](const std::shared_ptr<const Rendered>& p) { return p.use_count() == 1; }),
                   retired_.end());
  }

  // Edits are part of the host's project: tell it once editing pauses (or every few seconds while it
  // goes on), so it saves them and asks before closing an unsaved project. A VST3 host is marked
  // dirty; MuseScore reads the state again only then, or when the editor closes, and a state with
  // imported recordings is large. JUCE's AU wrapper ignores nonParameterStateChanged and passes on
  // a ClassInfo change only with a parameter-info one (Novox has no parameters). A state the host
  // loads is not an edit.
  if (const int edits = stateEdits_.load(); edits != stateEditsNotified_) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (edits != stateEditsSeen_) {
      stateEditsSeen_ = edits;
      lastEditAt_ = now;
      if (firstEditAt_ < 0) firstEditAt_ = now;
    }
    if (now - lastEditAt_ >= kStateQuietMs || now - firstEditAt_ >= kStateMaxDelayMs) {
      stateEditsNotified_ = edits;
      firstEditAt_ = -1;
      auto details = ChangeDetails().withNonParameterStateChanged(true);
      if (wrapperType == wrapperType_AudioUnit) details = details.withParameterInfoChanged(true);
      updateHostDisplay(details);
    }
  }
}

void NovoxProcessor::waitForRender(double sampleRate, bool tempoToo) {
  const double until = juce::Time::getMillisecondCounterHiRes() + kOfflineRenderWaitMs;
  for (;;) {
    const int revision = audioRevision_.load();
    const double bpm = hostTransport_.load() ? hostBpm_.load() : previewBpm_.load();  // as renderWorker renders
    bool ready = false, otherTempo = false;
    {
      const juce::SpinLock::ScopedLockType lock(renderedLock_);
      if (rendered_ && rendered_->audioRevision == revision && std::abs(rendered_->sampleRate - sampleRate) <= 0.5) {
        otherTempo = std::abs(rendered_->bpm - bpm) > 0.005;
        ready = !tempoToo || !otherTempo;
      }
    }
    // A tempo that changes during the bounce is followed as in real time: a render in the background.
    if (ready) {
      if (otherTempo && !rendering_.load()) requestRender();
      return;
    }
    {
      const std::lock_guard<std::mutex> lock(errorMutex_);
      if (renderError_.isNotEmpty() && failedRevision_ == revision) return;  // this song doesn't render
    }
    if (juce::Time::getMillisecondCounterHiRes() > until) return;
    if (!rendering_.load()) requestRender();
    juce::Thread::sleep(2);
  }
}

void NovoxProcessor::requestRender() {
  songRenderRequested_ = true;
  rendering_ = true;
  renderRequested_.signal();
}

void NovoxProcessor::renderWorker() {
  while (!worker_->threadShouldExit()) {
    renderRequested_.wait(-1);
    if (worker_->threadShouldExit()) break;
    const int previewPitch = notePreviewPitch_.exchange(-1);
    if (previewPitch >= 0 && !playing_.load()) {
      Song preview;
      { std::lock_guard<std::mutex> lock(songMutex_); preview.voice = song_.voice; }
      preview.notes.add({0,.4,previewPitch,"{M}"});
      auto audio = std::make_unique<Audition>();
      audio->rate = sampleRate_.load();
      audio->notePreview = true;
      double lead = 0; juce::String error;
      if (engine_->render(preview.toScoreJson(120),audio->rate,audio->samples,lead,error) && notePreviewPitch_.load()<0 && !playing_.load()) {
        const size_t trim = std::min(audio->samples.size(),static_cast<size_t>(lead*audio->rate));
        audio->samples.erase(audio->samples.begin(),audio->samples.begin()+static_cast<long>(trim));
        const size_t fade = static_cast<size_t>(audio->rate*.02);
        for(size_t i=0;i<audio->samples.size();++i)
          audio->samples[i] *= .65f*static_cast<float>(std::min({1.0,double(i)/std::max<size_t>(1,fade),double(audio->samples.size()-1-i)/std::max<size_t>(1,fade)}));
        { const juce::SpinLock::ScopedLockType lock(auditionLock_); audition_.swap(audio); }
      }
    }
    if (!songRenderRequested_.exchange(false)) continue;
    rendering_ = true;
    ++rendersStarted_;
    juce::String score;
    int version, audioRevision;
    const double bpm = hostTransport_.load() ? hostBpm_.load() : previewBpm_.load();
    const double sr = getSampleRate() > 0 ? getSampleRate() : sampleRate_.load();
    {
      std::lock_guard<std::mutex> lock(songMutex_);
      score = song_.toScoreJson(bpm);
      version = songVersion_.load();
      audioRevision = audioRevision_.load();
    }
    auto r = std::make_shared<Rendered>();
    juce::String error;
    // A newer song (the next keystroke of a lyric edit) makes this render moot: start that one. But
    // never while the transport runs, nor once the audio heard is a second behind the edits: a stream
    // of changes must not keep every render from finishing (the old one would go on playing).
    const bool cancellable = !playing_.load() && juce::Time::getMillisecondCounterHiRes() - lastRenderDone_.load() < kStaleRenderMs;
    const bool ok = engine_->render(score, sr, r->audio, r->lead, error, [this, cancellable] {
      return worker_->threadShouldExit() || (cancellable && songRenderRequested_.load());
    });
    if (!ok && error.isEmpty()) {
      ++rendersCancelled_;
      continue;
    }
    lastRenderDone_ = juce::Time::getMillisecondCounterHiRes();
    {
      std::lock_guard<std::mutex> lock(errorMutex_);
      renderError_ = ok ? juce::String() : error;
      failedRevision_ = ok ? -1 : audioRevision;
    }
    if (ok) {
      r->sampleRate = sr;
      r->bpm = bpm;
      r->version = version;
      r->audioRevision = audioRevision;
      std::shared_ptr<const Rendered> old;
      {
        const juce::SpinLock::ScopedLockType lock(renderedLock_);
        old = std::move(rendered_);
        rendered_ = r;
      }
      // Hand the old render to the message thread to free (never on the audio thread): the timer
      // drains this list, and it stops with the processor, so nothing queued outlives it.
      if (old) {
        const std::lock_guard<std::mutex> lock(retiredMutex_);
        retired_.push_back(std::move(old));
      }
    }
    rendering_ = false;
  }
}

juce::String NovoxProcessor::getSongJson() const {
  std::lock_guard<std::mutex> lock(songMutex_);
  return song_.toJson();
}

void NovoxProcessor::setSongJson(const juce::String& json) {
  auto incoming = Song::fromJson(json);
  bool audibleChange;
  {
    std::lock_guard<std::mutex> lock(songMutex_);
    // Compare the effective score, not the saved editor state. In pinned-lyric mode the
    // pane is a readable mirror; changing it or import provenance requires no new audio.
    // The fixed tempo makes this comparison independent of the current host transport.
    audibleChange = incoming.toScoreJson(120) != song_.toScoreJson(120);
    if (audibleChange || incoming.toJson() != song_.toJson()) ++stateEdits_;
    song_ = std::move(incoming);
    ++songVersion_;
    if (audibleChange) ++audioRevision_;
  }
  if (!audibleChange) return;
  {  // a Compare > Robot review is a copy of the old render: it ends (a looping one restarts on the new)
    std::unique_ptr<Audition> stale;
    {
      const juce::SpinLock::ScopedLockType lock(auditionLock_);
      if (audition_ && audition_->song) stale.swap(audition_);
    }
  }
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

juce::String NovoxProcessor::planSongJson(const juce::String& songJson, double bpm, juce::String& error) {
  error.clear();
  if (!std::isfinite(bpm) || bpm < 1 || bpm > 1000) {
    error = "The preview tempo must be between 1 and 1000 BPM.";
    return {};
  }
  juce::var value;
  if (juce::JSON::parse(songJson, value).failed() || !value.isObject() || !value["notes"].isArray()) {
    error = "The preview needs a song with a notes array.";
    return {};
  }
  for (const auto& note : *value["notes"].getArray()) {
    if (!note.isObject()) { error = "The preview contains an invalid note."; return {}; }
    for (const auto* key : {"start", "dur", "pitch"}) {
      const auto number = note.getProperty(key, 0.0);
      if (!(number.isInt() || number.isInt64() || number.isDouble()) || !std::isfinite(static_cast<double>(number))) {
        error = "The preview contains an invalid note position or pitch.";
        return {};
      }
    }
  }
  return engine_->plan(Song::fromJson(songJson).toScoreJson(bpm), error);
}

juce::String NovoxProcessor::importFile(const juce::MemoryBlock& data, const juce::String& lyrics,
                                      juce::String& error, const juce::var& choice) {
  error.clear();
  juce::MemoryBlock xml;
  const bool zipped = data.getSize() >= 2 && data[0] == 'P' && data[1] == 'K';
  if (zipped) {
    xml = musicXmlFromMxl(data, error);
    if (xml.isEmpty()) return {};
  }
  auto* o = new juce::DynamicObject();
  o->setProperty("lyrics", lyrics);
  o->setProperty("units", "beats");  // keep a region's bar position exact, whatever its tempo map
  // The part (MIDI track), voice and verse chosen in the editor; the engine checks them.
  for (const auto* key : {"track", "partVoice", "verse"})
    if (choice.hasProperty(key)) o->setProperty(key, choice[key]);
  return engine_->importFile(zipped ? xml : data, juce::JSON::toString(juce::var(o)), error);
}

juce::MemoryBlock NovoxProcessor::musicXmlFromMxl(const juce::MemoryBlock& zip, juce::String& error) {
  static constexpr juce::int64 maxXml = 64 * 1024 * 1024;  // the editor's import limit, uncompressed
  juce::MemoryInputStream in(zip, false);
  juce::ZipFile files(in);
  const auto read = [&files, &error](const juce::ZipFile::ZipEntry* entry) {
    juce::MemoryBlock out;
    if (entry == nullptr) return out;
    if (entry->uncompressedSize > maxXml) { error = "The score in this .mxl file is larger than 64 MB"; return out; }
    std::unique_ptr<juce::InputStream> stream(files.createStreamForEntry(*entry));
    bool ok = stream != nullptr;
    if (ok) {
      juce::MemoryOutputStream bytes(out, false);
      ok = bytes.writeFromInputStream(*stream, maxXml + 1) <= maxXml;
    }
    if (!ok) {
      error = "Could not unpack " + entry->filename + " from the .mxl file";
      out.reset();
    }
    return out;
  };
  if (files.getNumEntries() == 0) {
    error = "This .mxl file is not a readable zip archive (compressed MusicXML)";
    return {};
  }
  // META-INF/container.xml names the score: its first <rootfile full-path="...">.
  juce::String path;
  if (const auto container = read(files.getEntry("META-INF/container.xml", true)); !container.isEmpty()) {
    if (const auto xml = juce::parseXML(container.toString())) {
      std::function<const juce::XmlElement*(const juce::XmlElement&)> rootfile = [&](const juce::XmlElement& e) -> const juce::XmlElement* {
        if (e.getTagNameWithoutNamespace() == "rootfile") return &e;
        for (const auto* child : e.getChildIterator())
          if (const auto* found = rootfile(*child)) return found;
        return nullptr;
      };
      if (const auto* found = rootfile(*xml)) path = found->getStringAttribute("full-path").trimCharactersAtStart("/");
    }
  }
  error.clear();
  const juce::ZipFile::ZipEntry* score = path.isNotEmpty() ? files.getEntry(path, true) : nullptr;
  for (int i = 0; score == nullptr && i < files.getNumEntries(); ++i) {
    const auto* entry = files.getEntry(i);
    const auto name = entry->filename.toLowerCase();
    if (!name.startsWith("meta-inf/") && !name.endsWith("/") && (name.endsWith(".musicxml") || name.endsWith(".xml")))
      score = entry;
  }
  if (score == nullptr) {
    error = "This .mxl file holds no MusicXML score";
    return {};
  }
  auto xml = read(score);
  if (xml.isEmpty() && error.isEmpty()) error = "The score in this .mxl file is empty";
  return xml;
}

void NovoxProcessor::setPreviewPlaying(bool playing) {
  if (playing != previewPlaying_.load()) previewSeek_ = previewCursor_.load();  // the audio thread moves there
  previewPlaying_ = playing;
}

void NovoxProcessor::setPreviewCursor(double beats) {
  if (!std::isfinite(beats)) return;
  previewCursor_ = std::max(0.0, beats);
  previewSeek_ = previewCursor_.load();
}

void NovoxProcessor::setPreviewTempo(double bpm) {
  const double tempo = juce::jlimit(20.0, 400.0, bpm);
  if (std::abs(tempo - previewBpm_.exchange(tempo)) > 1e-9) ++stateEdits_;
  if (!hostTransport_.load()) requestRender();
}

NovoxProcessor::Status NovoxProcessor::getStatus() const {
  Status s;
  s.beats = playheadBeats_.load();
  s.seconds = playheadSeconds_.load();
  s.editOrigin = editOrigin_.load();
  s.bpm = hostTransport_.load() ? hostBpm_.load() : previewBpm_.load();
  s.playing = playing_.load();
  s.hostTransport = hostTransport_.load();
  s.hostTempo = hostTempo_.load();
  static const char* const clocks[] = {"own", "beats", "samples"};
  s.clock = clocks[juce::jlimit(0, 2, clock_.load())];
  s.capture = captureArmed_.load();
  s.rendering = rendering_.load();
  s.songVersion = songVersion_.load();
  const int rate = hostFrameRate_.load();
  s.frameBase = rate & 0xff;
  s.frameDrop = (rate >> 8) & 1;
  s.framePullDown = (rate >> 9) & 1;
  s.blocks = blocks_.load(std::memory_order_relaxed);
  s.daw = inDaw();
  s.rendersStarted = rendersStarted_.load();
  s.rendersCancelled = rendersCancelled_.load();
  const double done = lastRenderDone_.load();
  s.secondsSinceRender = done > 0 ? (juce::Time::getMillisecondCounterHiRes() - done) / 1000.0 : -1;
  s.mmc = mmcState();
  {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(errorMutex_));
    s.error = renderError_;
  }
  return s;
}

// ---------------------------------------------------------------------------
// State

bool NovoxProcessor::storeSource(const juce::String& id,const juce::var& source) {
  const auto audio=source["original"].toString();
  const double bpm=source["bpm"],seconds=source["seconds"];
  if(id.isEmpty() || id.length()>100 || !source.isObject() || !audio.startsWith("data:audio/wav;base64,") || audio.length()>90*1024*1024 || !std::isfinite(bpm) || bpm<20 || bpm>400 || !std::isfinite(seconds) || seconds<=0 || seconds>301) return false;
  std::lock_guard<std::mutex> lock(songMutex_);sources_[id]=source.clone();return true;
}
juce::var NovoxProcessor::getSource(const juce::String& id) const {
  std::lock_guard<std::mutex> lock(songMutex_);auto found=sources_.find(id);return found==sources_.end() ? juce::var() : found->second.clone();
}
// State (DAW project and .novox file): {"novoxState":1, "song", "sources":[{"id","source"}], "tempo"},
// with each source recording the song's imports refer to saved once. States saved before
// 2026-10-17 had one "sourceId"/"source"; older ones are the bare song JSON. Loading a state adds
// its recordings to the ones this instance holds: an open editor's Undo can bring back the song
// from before the load, and its recordings must still play and save.
void NovoxProcessor::setMmcState(const MmcState& s) {
  const auto before = mmcState();
  storeMmc(s);
  const auto after = mmcState();
  if (after.works != before.works || after.offset != before.offset || after.rate != before.rate) ++stateEdits_;
}
void NovoxProcessor::storeMmc(const MmcState& s) {
  mmcWorks_ = s.works;
  mmcOffset_ = std::isfinite(s.offset) ? juce::jlimit(-7200.0, 7200.0, s.offset) : 0.0;
  mmcRate_ = std::isfinite(s.rate) && s.rate >= 0 && s.rate <= 60 ? s.rate : 0.0;
}
NovoxProcessor::MmcState NovoxProcessor::mmcState() const { return {mmcWorks_.load(), mmcOffset_.load(), mmcRate_.load()}; }

void NovoxProcessor::getStateInformation(juce::MemoryBlock& destData) { writeState(destData, true); }
void NovoxProcessor::writeState(juce::MemoryBlock& destData, bool withProject) {
  auto* state=new juce::DynamicObject();
  {std::lock_guard<std::mutex> lock(songMutex_);
    state->setProperty("novoxState",1);state->setProperty("song",juce::JSON::parse(song_.toJson()));
    juce::Array<juce::var> saved;
    for(const auto& id:song_.recordingIds()) {
      auto found=sources_.find(id);if(found==sources_.end()) continue;
      auto* entry=new juce::DynamicObject();entry->setProperty("id",id);entry->setProperty("source",found->second);saved.add(juce::var(entry));
    }
    if(!saved.isEmpty()) state->setProperty("sources",saved);
  }
  state->setProperty("tempo",getStatus().bpm);
  if(withProject) {
    const auto m=mmcState();auto* o=new juce::DynamicObject();
    o->setProperty("works",m.works);o->setProperty("offset",m.offset);o->setProperty("rate",m.rate);state->setProperty("mmc",juce::var(o));
  }
  destData.reset();juce::MemoryOutputStream(destData,false).writeString(juce::JSON::toString(juce::var(state),true));
}
void NovoxProcessor::setStateInformation(const void* data,int sizeInBytes) { loadState(data,sizeInBytes); }
void NovoxProcessor::openSongFile(const void* data,int sizeInBytes) { if(loadState(data,sizeInBytes)) ++stateEdits_; }
bool NovoxProcessor::loadState(const void* data,int sizeInBytes) {
  if(!data || sizeInBytes<=0 || sizeInBytes>maxStateBytes) return false;
  juce::MemoryInputStream in(data,static_cast<size_t>(sizeInBytes),false);
  auto state=juce::JSON::parse(in.readString());if(!state.isObject()) return false;
  const bool wrapped=state["novoxState"]==juce::var(1);
  auto song=wrapped ? state["song"] : state;if(!song.isObject()) return false;
  {std::lock_guard<std::mutex> lock(songMutex_);song_=Song::fromJson(juce::JSON::toString(song));++songVersion_;++audioRevision_;}
  if(wrapped) {
    if(const auto* saved=state["sources"].getArray()) for(const auto& entry:*saved) storeSource(entry["id"].toString(),entry["source"]);
    if(state.hasProperty("sourceId")) storeSource(state["sourceId"].toString(),state["source"]);
    const double tempo=state["tempo"];if(std::isfinite(tempo) && tempo>=20 && tempo<=400) previewBpm_=tempo;
    // Only a DAW project carries it; a .novox file leaves this project's as it was.
    if(const auto m=state["mmc"];m.isObject()) storeMmc({static_cast<bool>(m["works"]),static_cast<double>(m["offset"]),static_cast<double>(m["rate"])});
  }
  stopAudition();requestRender();
  return true;
}
juce::MemoryBlock NovoxProcessor::exportMidi(juce::String& error) const {
  Song song;{std::lock_guard<std::mutex> lock(songMutex_);song=song_;}
  if(song.notes.isEmpty()) {error="Add notes before exporting";return{};}
  const double bpm=getStatus().bpm;juce::MidiMessageSequence track;
  track.addEvent(juce::MidiMessage::tempoMetaEvent(static_cast<int>(60000000/bpm)),0);
  for(const auto& n:song.notes) {
    track.addEvent(juce::MidiMessage::noteOn(1,n.pitch,static_cast<juce::uint8>(100)),n.start*960);
    track.addEvent(juce::MidiMessage::noteOff(1,n.pitch),(n.start+n.dur)*960);
    if(n.lyric.isNotEmpty()) {auto text=n.lyric.toUTF8();track.addEvent(juce::MidiMessage::textMetaEvent(5,juce::String(text)),n.start*960);}
  }
  track.sort();track.updateMatchedPairs();juce::MidiFile midi;midi.setTicksPerQuarterNote(960);midi.addTrack(track);
  juce::MemoryBlock bytes;juce::MemoryOutputStream out(bytes,false);if(!midi.writeTo(out,1)) {error="Could not write MIDI";return{};}return bytes;
}
juce::MemoryBlock NovoxProcessor::exportWave(juce::String& error) const {
  std::shared_ptr<const Rendered> r;{const juce::SpinLock::ScopedLockType lock(const_cast<juce::SpinLock&>(renderedLock_));r=rendered_;}
  if(!r || rendering_.load() || r->audioRevision!=audioRevision_.load()) {error="Wait for the singer to finish rendering";return{};}
  auto first=static_cast<size_t>(std::max(0.0,std::round(r->lead*r->sampleRate)));
  if(first>=r->audio.size()) {error="Add notes before exporting";return{};}
  juce::MemoryBlock bytes;auto stream=std::make_unique<juce::MemoryOutputStream>(bytes,false);juce::WavAudioFormat format;
  std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(),r->sampleRate,1,24,{},0));
  if(!writer) {error="Could not write WAV";return{};}stream.release();const float* channels[]={r->audio.data()+first};
  if(!writer->writeFromFloatArrays(channels,1,static_cast<int>(r->audio.size()-first))) {error="Could not write WAV";return{};}writer.reset();return bytes;
}

juce::AudioProcessorEditor* NovoxProcessor::createEditor() { return new NovoxEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NovoxProcessor(); }

bool NovoxProcessor::auditionAudio(const juce::MemoryBlock& data, juce::String& error, double start, double end, bool loop) {
  if (data.getSize() > 64 * 1024 * 1024) { error = "Preview is too large"; return false; }
  juce::WavAudioFormat format;
  auto stream = std::make_unique<juce::MemoryInputStream>(data, false);
  std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.get(), false));
  if (!reader) { error = "Could not read preview audio"; return false; }
  stream.release();
  if (reader->sampleRate <= 0 || reader->lengthInSamples <= 0 || reader->lengthInSamples / reader->sampleRate > 301) {
    error = "Invalid preview length"; return false;
  }
  auto audio = std::make_unique<Audition>(); audio->rate = reader->sampleRate;
  audio->samples.resize(static_cast<size_t>(reader->lengthInSamples));
  juce::AudioBuffer<float> decoded(1, static_cast<int>(reader->lengthInSamples));
  if (!reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true)) { error = "Could not decode preview"; return false; }
  std::copy_n(decoded.getReadPointer(0), audio->samples.size(), audio->samples.data());
  if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || (end >= 0 && end <= start)) {error="Invalid phrase range";return false;}
  audio->begin = std::min(start * audio->rate, static_cast<double>(audio->samples.size()));
  audio->end = end < 0 ? audio->samples.size() : std::min(end * audio->rate, static_cast<double>(audio->samples.size()));
  if (audio->end <= audio->begin) {error="No audio in this phrase";return false;}
  audio->position=audio->begin;audio->loop=loop;
  { const juce::SpinLock::ScopedLockType lock(auditionLock_); audition_.swap(audio); }
  return true;
}
bool NovoxProcessor::auditionSources(const juce::var& pieces, bool loop, juce::String& error) {
  const auto* list=pieces.getArray();
  if(!list || list->isEmpty() || list->size()>256) {error="Select notes with an original recording";return false;}
  struct Piece {juce::String audio; double from, to, at;};
  std::vector<Piece> wanted;
  {
    std::lock_guard<std::mutex> lock(songMutex_);
    for(const auto& piece:*list) {
      const double from=piece["from"],to=piece["to"],at=piece["at"];
      auto found=sources_.find(piece["id"].toString());
      if(found==sources_.end()) {error="The original recording is missing";return false;}
      if(!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(at) || from<0 || to<=from || at<0) {error="Invalid phrase range";return false;}
      if(at+(to-from)>600) {error="Select a shorter phrase (ten minutes at most)";return false;}
      wanted.push_back({found->second["original"].toString(),from,to,at});  // the string is shared, not copied
    }
  }
  // Mixed at the first recording's rate (all imports keep 16 kHz); others are resampled linearly.
  double rate=0;std::vector<float> mix;
  for(const auto& piece:wanted) {
    juce::MemoryBlock wav;
    {juce::MemoryOutputStream decoded(wav,false);
     if(!piece.audio.startsWith("data:audio/wav;base64,") || !juce::Base64::convertFromBase64(decoded,piece.audio.substring(22))) {error="Could not read the original recording";return false;}}
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::MemoryInputStream(wav,false),true));
    if(!reader || reader->sampleRate<=0 || reader->lengthInSamples<=0) {error="Could not read the original recording";return false;}
    if(rate<=0) rate=reader->sampleRate;
    const auto first=static_cast<juce::int64>(std::floor(piece.from*reader->sampleRate));
    const auto last=std::min(reader->lengthInSamples,static_cast<juce::int64>(std::ceil(piece.to*reader->sampleRate)));
    if(last<=first) continue;
    juce::AudioBuffer<float> audio(1,static_cast<int>(last-first));
    if(!reader->read(&audio,0,audio.getNumSamples(),first,true,true)) {error="Could not decode the original recording";return false;}
    const double step=reader->sampleRate/rate;
    const auto count=static_cast<size_t>(std::floor((audio.getNumSamples()-1)/step))+1;
    const auto offset=static_cast<size_t>(std::llround(piece.at*rate));
    if(mix.size()<offset+count) mix.resize(offset+count,0.0f);
    const float* in=audio.getReadPointer(0);
    for(size_t i=0;i<count;++i) {
      const double position=static_cast<double>(i)*step;const auto k=static_cast<int>(position);
      const float a=in[k],b=in[std::min(k+1,audio.getNumSamples()-1)];
      mix[offset+i]+=a+static_cast<float>(position-k)*(b-a);
    }
  }
  if(mix.empty()) {error="No audio in this phrase";return false;}
  float peak=0;for(const float v:mix) peak=std::max(peak,std::abs(v));
  if(peak>1) for(auto& v:mix) v/=peak;  // overlapping takes can sum past full scale
  auto audio=std::make_unique<Audition>();audio->rate=rate;audio->samples=std::move(mix);
  audio->begin=0;audio->end=static_cast<double>(audio->samples.size());audio->position=0;audio->loop=loop;
  {const juce::SpinLock::ScopedLockType lock(auditionLock_);audition_.swap(audio);}
  return true;
}
bool NovoxProcessor::auditionSong(double startBeat, double endBeat, bool loop, juce::String& error) {
  std::shared_ptr<const Rendered> rendered;
  {const juce::SpinLock::ScopedLockType lock(renderedLock_);rendered=rendered_;}
  if (!rendered || rendered->audioRevision!=audioRevision_.load() || rendering_.load()) {error="Wait for the singer to finish rendering";return false;}
  if (!std::isfinite(startBeat) || !std::isfinite(endBeat) || startBeat<0 || endBeat<=startBeat) {error="Select a phrase to hear";return false;}
  auto audio=std::make_unique<Audition>();audio->samples=rendered->audio;audio->rate=rendered->sampleRate;
  audio->begin=std::min(static_cast<double>(audio->samples.size()),(startBeat*60/rendered->bpm+rendered->lead)*audio->rate);
  audio->end=std::min(static_cast<double>(audio->samples.size()),(endBeat*60/rendered->bpm+rendered->lead)*audio->rate);
  if(audio->end<=audio->begin) {error="No audio in this phrase";return false;}
  audio->position=audio->begin;audio->loop=loop;audio->song=true;
  {const juce::SpinLock::ScopedLockType lock(auditionLock_);audition_.swap(audio);}return true;
}
bool NovoxProcessor::reviewing() {
  const juce::SpinLock::ScopedLockType lock(auditionLock_);
  if (!audition_ || audition_->notePreview) return false;
  return audition_->loop || audition_->position < (audition_->end > 0 ? audition_->end : static_cast<double>(audition_->samples.size()));
}
int NovoxProcessor::renderedVersion() {
  std::shared_ptr<const Rendered> rendered;
  { const juce::SpinLock::ScopedLockType lock(renderedLock_); rendered = rendered_; }
  if (!rendered) return -1;
  const std::lock_guard<std::mutex> lock(songMutex_);
  return rendered->audioRevision == audioRevision_.load() ? songVersion_.load() : rendered->version;
}
void NovoxProcessor::stopAudition() {
  std::unique_ptr<Audition> previous;
  { const juce::SpinLock::ScopedLockType lock(auditionLock_); previous.swap(audition_); }
}

void NovoxProcessor::previewNote(int pitch) {
  if (playing_.load()) return;
  {const juce::SpinLock::ScopedLockType lock(auditionLock_);if(audition_ && audition_->loop) return;}
  notePreviewPitch_ = juce::jlimit(0,127,pitch);
  renderRequested_.signal();
}
