#include "PluginEditor.h"

#include "BinaryData.h"

#if JUCE_MAC
void* installNovoxSelectAll(juce::Component&, std::function<void()>);
void removeNovoxSelectAll(void*);
bool forwardNovoxHostSpace(juce::Component&);
void* installNovoxRegionDrop(juce::Component&,std::function<void(juce::String)>,std::function<void(juce::String)>);
void readNovoxRegionDrop(void*);
void removeNovoxRegionDrop(void*);
#endif

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
constexpr size_t maxClipboardBytes = 4 * 1024 * 1024;

juce::var clipboardReply(const juce::String& text, const juce::String& error = {}) {
  auto* reply = new juce::DynamicObject();
  reply->setProperty("text", text);
  reply->setProperty("error", error);
  return juce::var(reply);
}

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
      .withNativeFunction("writeClipboard", [](const Args& a, Completion done) {
        if (a.size() != 1 || !a[0].isString()) { done(clipboardReply({}, "Clipboard text is missing")); return; }
        const auto text = a[0].toString();
        if (text.getNumBytesAsUTF8() > maxClipboardBytes) {
          done(clipboardReply({}, "The copied melody is too large (maximum 4 MB)")); return;
        }
        juce::SystemClipboard::copyTextToClipboard(text);
        // JUCE's write API has no return value. Verify that the system pasteboard accepted it.
        done(clipboardReply({}, juce::SystemClipboard::getTextFromClipboard() == text
                                   ? juce::String() : "Could not write to the system clipboard. Please copy again."));
      })
      .withNativeFunction("readClipboard", [](const Args&, Completion done) {
        const auto text = juce::SystemClipboard::getTextFromClipboard();
        if (text.getNumBytesAsUTF8() > maxClipboardBytes) {
          done(clipboardReply({}, "The clipboard is too large (maximum 4 MB)")); return;
        }
        done(clipboardReply(text));
      })
      .withNativeFunction("lyricTokens", [&p](const Args& a, Completion done) {
        juce::String error, result;
        if (a.size() != 2 || !a[0].isString() || !a[1].isString()) error = "Lyrics and pronunciation dictionary are required";
        else if (a[0].toString().getNumBytesAsUTF8() > 64 * 1024 || a[1].toString().getNumBytesAsUTF8() > 1024 * 1024)
          error = "The lyric edit is too large";
        else result = p.lyricTokens(a[0].toString(), a[1].toString(), error);
        if (error.isNotEmpty()) {
          auto* reply = new juce::DynamicObject(); reply->setProperty("error", error);
          result = juce::JSON::toString(juce::var(reply));
        }
        done(result);
      })
      .withNativeFunction("planSong", [&p](const Args& a, Completion done) {
        juce::String error, result;
        if (a.size() != 2 || !a[0].isString()) error = "A song and tempo are required";
        else if (a[0].toString().getNumBytesAsUTF8() > 4 * 1024 * 1024) error = "The song is too large to preview";
        else result = p.planSongJson(a[0].toString(), static_cast<double>(a[1]), error);
        if (error.isNotEmpty()) {
          auto* reply = new juce::DynamicObject(); reply->setProperty("error", error);
          result = juce::JSON::toString(juce::var(reply));
        }
        done(result);
      })
      .withNativeFunction("setEditorSize", [this](const Args& a, Completion done) {
        int width = a.size() > 0 ? static_cast<int>(a[0]) : 1000;
        int height = a.size() > 1 ? static_cast<int>(a[1]) : 660;
        if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds())) {
          width = juce::jmin(width, display->userArea.getWidth()-80);
          height = juce::jmin(height, display->userArea.getHeight()-100);
        }
        setSize(juce::jlimit(760,2400,width), juce::jlimit(520,1600,height));
        done(juce::var());
      })
      .withNativeFunction("hostSpace", [this](const Args&, Completion done) {
#if JUCE_MAC
        done(proc_.wrapperType != juce::AudioProcessor::wrapperType_Standalone && forwardNovoxHostSpace(*this));
#else
        done(false);
#endif
      })
      .withNativeFunction("previewNote", [&p](const Args& a, Completion done) { if(a.size()) p.previewNote(static_cast<int>(a[0])); done(juce::var()); })
      .withNativeFunction("startMicrophone", [this](const Args&, Completion done) {
        if (microphonePending_ || static_cast<bool>(microphone_.status()["recording"])) {done("Already recording");return;}
        if (audioImport_.status()["state"].toString()=="working") {done("Wait for the current import to finish");return;}
        microphoneError_.clear();microphonePending_=true;
        juce::Component::SafePointer<NovoxEditor> safe(this);
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,[safe](bool granted) {
          juce::MessageManager::callAsync([safe,granted] {
            if (!safe) return;
            safe->microphonePending_=false;
            safe->microphoneError_=granted ? safe->microphone_.start() : "Microphone permission was denied. Enable it in System Settings.";
          });
        });
        done(juce::var());
      })
      .withNativeFunction("stopMicrophone", [this](const Args&, Completion done) {
        if (microphonePending_) {done("Wait for microphone permission");return;}
        pendingAudio_=microphone_.stop();pendingName_="Singing.wav";
        if (pendingAudio_.isEmpty()) {done("No microphone audio was recorded");return;}
        auto* o=new juce::DynamicObject();o->setProperty("name",pendingName_);
        browser_.emitEventIfBrowserIsVisible("audioFileDrop",juce::var(o));done(juce::var());
      })
      .withNativeFunction("cancelMicrophone", [this](const Args&, Completion done) {microphone_.cancel();done(juce::var());})
      .withNativeFunction("readNativeDrop", [this](const Args&,Completion done) {
#if JUCE_MAC
        readNovoxRegionDrop(regionDrop_);
#endif
        done(juce::var());
      })
      .withNativeFunction("storeSource", [&p](const Args& a,Completion done) {done(a.size()>1 && p.storeSource(a[0].toString(),a[1]));})
      .withNativeFunction("getSource", [&p](const Args& a,Completion done) {done(p.getSource(a.size()?a[0].toString():""));})
      .withNativeFunction("fileAction", [this,&p](const Args& a,Completion done) {
        const auto action=a.size()?a[0].toString():"";
        if(fileChooser_) {done("Finish the open file dialog first");return;}
        juce::MemoryBlock bytes;juce::String error,extension;
        const bool open=action=="open", song=action=="save" || action=="saveAs";
        const bool wave=action=="wav" || action=="dragWav",midi=action=="midi" || action=="dragMidi";
        if(!open && !song && !wave && !midi) {done("Unknown file action");return;}
        if(song) {p.writeState(bytes,false);extension="novox";}
        if(wave) {bytes=p.exportWave(error);extension="wav";}
        if(midi) {bytes=p.exportMidi(error);extension="mid";}
        if(error.isNotEmpty()) {done(error);return;}
        if(action.startsWith("drag")) {
          // DAWs may reference dragged audio rather than copying it: keep exports stable.
          auto folder=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Application Support/Novox/Exports").getChildFile(juce::Uuid().toString());
          folder.createDirectory();auto file=folder.getChildFile("Novox The Robot."+extension);
          if(!file.replaceWithData(bytes.getData(),bytes.getSize())) {done("Could not create export");return;}
          const bool started=juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()},false,&browser_);
          if(!started) folder.deleteRecursively();done(started?juce::String():"Keep the mouse held while dragging; you can also use Export");return;
        }
        if(action=="save" && songFile_.existsAsFile()) {done(songFile_.replaceWithData(bytes.getData(),bytes.getSize()) ? juce::String() : "Could not save song");return;}
        fileChooser_=std::make_unique<juce::FileChooser>(open ? "Open Novox song" : "Save "+extension,
          songFile_.existsAsFile() && song ? songFile_ : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Novox The Robot."+(open?juce::String("novox"):extension)),"*."+(open?juce::String("novox"):extension));
        juce::Component::SafePointer<NovoxEditor> safe(this);
        fileChooser_->launchAsync((open ? juce::FileBrowserComponent::openMode : juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::warnAboutOverwriting)|juce::FileBrowserComponent::canSelectFiles,
          [safe,bytes,open,song,extension,done](const juce::FileChooser& chooser) mutable {
            if(!safe) return;auto file=chooser.getResult();juce::String error;
            if(file!=juce::File()) {
              if(open) {juce::MemoryBlock data;if(file.getSize()>NovoxProcessor::maxStateBytes || !file.loadFileAsData(data)) error="Could not read song";
                else {juce::MemoryInputStream stream(data,false);auto parsed=juce::JSON::parse(stream.readString());
                  if(!parsed.isObject() || (parsed.hasProperty("novoxState") ? !parsed["song"].isObject() || !parsed["song"].hasProperty("notes") : !parsed.hasProperty("notes"))) error="Not a Novox song file";
                  else {safe->proc_.setStateInformation(data.getData(),static_cast<int>(data.getSize()));safe->songFile_=file;}}
              } else {file=file.withFileExtension(extension);if(!file.replaceWithData(bytes.getData(),bytes.getSize())) error="Could not save file";else if(song) safe->songFile_=file;}
            }
            if(file==juce::File()) error="cancelled";
            done(error);safe->fileChooser_.reset();
          });
      })
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
      // The DAW's transport over MMC (HostMmc): "play" / "stop"; a locate to `seconds` of the DAW's
      // timeline (from its bar 1, as status.seconds) at `fps` (24, 25, 29.97, 30; 0: the DAW's
      // reported rate, else 30), answering {sent, frame, fps}: the frame of its second it sent, for
      // the page to learn the DAW's rate from where it lands; and the project's MMC state to save.
      // Whether the DAW listens, the page learns from the status that follows.
      .withNativeFunction("hostTransport", [&p](const Args& a, Completion done) {
        const auto action = a.size() > 0 ? a[0].toString() : juce::String();
        const auto mmc = p.mmc();
        done(mmc != nullptr && (action == "play" ? mmc->play() : action == "stop" && mmc->stop()));
      })
      .withNativeFunction("hostLocate", [&p](const Args& a, Completion done) {
        const double seconds = a.size() > 0 ? static_cast<double>(a[0]) : NAN, fps = a.size() > 1 ? static_cast<double>(a[1]) : 0;
        const auto mmc = p.mmc();
        auto* o = new juce::DynamicObject();
        const juce::var reply(o);
        o->setProperty("sent", false);
        if (!mmc || !std::isfinite(seconds)) { done(reply); return; }
        const auto s = p.getStatus();
        HostMmc::FrameRate rate{s.frameBase > 0 ? s.frameBase : 30, s.frameDrop, s.framePullDown};
        if (std::abs(fps - 24) < .01 || std::abs(fps - 25) < .01 || std::abs(fps - 30) < .01) rate = {static_cast<int>(std::lround(fps))};
        else if (std::abs(fps - 29.97) < .01) rate = {30, false, true};
        // Bar 1's timecode: the DAW's edit origin when it gives one, else Logic's and GarageBand's
        // default 01:00:00:00 (the page learns a project set otherwise from where a locate lands).
        const juce::PluginHostType host;
        const double origin = std::isfinite(s.editOrigin) ? s.editOrigin * HostMmc::framesPerSecond(rate)
                              : host.isLogic() || host.isGarageBand() ? HostMmc::hourFrames(1, rate) : 0.0;
        o->setProperty("sent", mmc->locate(origin, seconds, rate));
        o->setProperty("frame", HostMmc::label(origin, seconds, rate).fr);
        o->setProperty("fps", HostMmc::framesPerSecond(rate) / (rate.base > 30 ? 2 : 1));
        done(reply);
      })
      .withNativeFunction("setMmcState", [&p](const Args& a, Completion done) {
        p.setMmcState({a.size() > 0 && static_cast<bool>(a[0]), a.size() > 1 ? static_cast<double>(a[1]) : 0.0,
                       a.size() > 2 ? static_cast<double>(a[2]) : 0.0});
        done(juce::var());
      })
      .withNativeFunction("setPlayhead",
                          [&p](const Args& a, Completion done) {
                            if (a.size() > 0) p.setPreviewCursor(static_cast<double>(a[0]));
                            done(juce::var());
                          })
      .withNativeFunction("setTempo",
                          [&p](const Args& a, Completion done) {
                            if (a.size() > 0) p.setPreviewTempo(static_cast<double>(a[0]));
                            done(juce::var());
                          })
      .withNativeFunction("playSongReview", [&p](const Args& a, Completion done) {
        juce::String error;p.auditionSong(a.size()>0 ? static_cast<double>(a[0]) : 0,a.size()>1 ? static_cast<double>(a[1]) : 0,a.size()>2 && static_cast<bool>(a[2]),error);done(error);
      })
      .withNativeFunction("playSourceReview", [&p](const Args& a, Completion done) {
        juce::String error;p.auditionSources(a.size()>0 ? a[0] : juce::var(),a.size()>1 && static_cast<bool>(a[1]),error);done(error);
      })
      .withNativeFunction("stopAudioReview", [&p](const Args&, Completion done) { p.stopAudition(); done(juce::var()); })
      .withNativeFunction("playAudioReview", [&p](const Args& a, Completion done) {
        juce::String error;
        const auto url = a.size() ? a[0].toString() : juce::String();
        juce::MemoryBlock data;
        juce::MemoryOutputStream decoded(data, false);
        if (url.length() > 90 * 1024 * 1024 || !url.startsWith("data:audio/wav;base64,") ||
            !juce::Base64::convertFromBase64(decoded, url.substring(22))) error = "Invalid preview audio";
        else { decoded.flush(); p.auditionAudio(data, error, a.size()>1 ? static_cast<double>(a[1]) : 0, a.size()>2 ? static_cast<double>(a[2]) : -1, a.size()>3 && static_cast<bool>(a[3])); }
        done(error);
      })
      .withNativeFunction("audioImportStatus", [this](const Args&, Completion done) { done(audioImport_.status()); })
      .withNativeFunction("previewAudioImport", [this](const Args& a, Completion done) { done(audioImport_.preview(a.size() ? a[0].toString() : "")); })
      .withNativeFunction("cancelAudioImport", [this](const Args&, Completion done) { audioImport_.cancel(); done(juce::var()); })
      .withNativeFunction("startAudioImport", [this](const Args& a, Completion done) {
        juce::MemoryBlock data;
        juce::String name;
        if (a.size() > 0 && a[0].toString().isNotEmpty()) {
          if (a[0].toString().length() > 90 * 1024 * 1024) { done("File is too large"); return; }
          juce::MemoryOutputStream decoded(data, false);
          if (!juce::Base64::convertFromBase64(decoded, a[0].toString())) { done("Could not decode recording"); return; }
          decoded.flush();
          name = a.size() > 1 ? a[1].toString() : "Recording";
        } else { data = pendingAudio_; name = pendingName_; }
        // (base64 or "" for the pending microphone/drop audio, name, download, melodyOnly, detailedNotes)
        done(audioImport_.start(std::move(data), name, a.size() > 2 && static_cast<bool>(a[2]),
                               a.size() > 3 && static_cast<bool>(a[3]), a.size() > 4 && static_cast<bool>(a[4])));
        pendingAudio_.reset();
      })
      .withNativeFunction("importFile", [&p](const Args& a, Completion done) {
        juce::MemoryBlock data;
        if (a.size() > 0) { juce::MemoryOutputStream decoded(data, false); juce::Base64::convertFromBase64(decoded, a[0].toString()); decoded.flush(); }
        juce::String error;
        juce::String score = p.importFile(data, a.size() > 1 ? a[1].toString() : juce::String(), error);
        done(score);
      });
}

NovoxEditor::NovoxEditor(NovoxProcessor& p) : juce::AudioProcessorEditor(p), proc_(p), browser_(makeOptions()) {
  const auto initialSize = proc_.editorSize;
  addAndMakeVisible(browser_);
  browser_.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
  setResizable(true, true);
  setResizeLimits(760, 520, 2400, 1600);
  setSize(initialSize.x, initialSize.y);
#if JUCE_MAC
  selectAllMonitor_ = installNovoxSelectAll(browser_, [this] {
    browser_.evaluateJavascript("window.dispatchEvent(new Event('novox-select-all'))");
  });
#endif
  startTimerHz(30);
}

NovoxEditor::~NovoxEditor() {
#if JUCE_MAC
  removeNovoxSelectAll(selectAllMonitor_);
  removeNovoxRegionDrop(regionDrop_);
#endif
  stopTimer(); proc_.stopAudition();
}

void NovoxEditor::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xffe8e8e8)); }

void NovoxEditor::resized() {
  browser_.setBounds(getLocalBounds());
  proc_.editorSize = {getWidth(),getHeight()};
}

void NovoxEditor::timerCallback() {
#if JUCE_MAC
  if (!regionDrop_) {
    juce::Component::SafePointer<NovoxEditor> safe(this);
    regionDrop_=installNovoxRegionDrop(browser_,[safe](juce::String path) {
      if(safe) safe->filesDropped({path},0,0);
    },[safe](juce::String error) {
      if(safe) safe->browser_.emitEventIfBrowserIsVisible("audioFileDrop",error);
    });
  }
#endif
  const auto s = proc_.getStatus();
  auto* o = new juce::DynamicObject();
  o->setProperty("beats", s.beats);
  if (std::isfinite(s.seconds)) o->setProperty("seconds", s.seconds);
  if (std::isfinite(s.editOrigin)) o->setProperty("editOrigin", s.editOrigin);
  o->setProperty("bpm", s.bpm);
  o->setProperty("playing", s.playing);
  o->setProperty("host", s.hostTransport);
  o->setProperty("capture", s.capture);
  o->setProperty("rendering", s.rendering);
  o->setProperty("error", s.error);
  o->setProperty("songVersion", s.songVersion);
  o->setProperty("renderedVersion", proc_.renderedVersion());
  o->setProperty("reviewing", proc_.reviewing());
  auto* renders = new juce::DynamicObject();
  renders->setProperty("started", s.rendersStarted);
  renders->setProperty("cancelled", s.rendersCancelled);
  renders->setProperty("secondsSinceDone", s.secondsSinceRender);
  o->setProperty("renders", juce::var(renders));
  o->setProperty("blocks", static_cast<int>(s.blocks & 0x3fffffff));
  if (s.frameBase > 0) o->setProperty("frameRate", s.frameBase);
#ifdef NOVOX_VERSION_LABEL
  o->setProperty("version", juce::String(NOVOX_VERSION_LABEL));
#endif
  o->setProperty("daw", s.daw);
  if (s.daw) {
    auto* m = new juce::DynamicObject();
    m->setProperty("port", proc_.mmc() != nullptr);
    m->setProperty("works", s.mmc.works);
    m->setProperty("offset", s.mmc.offset);
    m->setProperty("rate", s.mmc.rate);
    o->setProperty("mmc", juce::var(m));
  }
  static const juce::String hostName = juce::PluginHostType().getHostDescription();
  o->setProperty("hostName", hostName);
  auto mic=microphone_.status();
  mic.getDynamicObject()->setProperty("pending",microphonePending_);
  if(microphoneError_.isNotEmpty()) mic.getDynamicObject()->setProperty("error",microphoneError_);
  o->setProperty("microphone",mic);
  browser_.emitEventIfBrowserIsVisible("status", juce::var(o));
}

bool NovoxEditor::isInterestedInFileDrag(const juce::StringArray& files) {
  if (files.size() != 1) return false;
  return juce::File(files[0]).hasFileExtension("wav;mp3;aif;aiff;m4a;flac;ogg;mid;midi;xml;musicxml;mxl");
}
void NovoxEditor::filesDropped(const juce::StringArray& files, int, int) {
  if (!isInterestedInFileDrag(files)) {
    browser_.emitEventIfBrowserIsVisible("audioFileDrop","Drop one audio or MIDI file at a time");return;
  }
  if(static_cast<bool>(microphone_.status()["recording"])) {
    browser_.emitEventIfBrowserIsVisible("audioFileDrop","Stop singing recording before importing a file");return;
  }
  juce::File file(files[0]);
  if (file.getSize() > 64 * 1024 * 1024) {
    browser_.emitEventIfBrowserIsVisible("audioFileDrop", "File is larger than 64 MB"); return;
  }
  if (!file.existsAsFile() || file.getSize()<=0) {
    browser_.emitEventIfBrowserIsVisible("audioFileDrop","The dragged region did not provide a readable file. Export it as MIDI or audio, then drop that file.");return;
  }
  if(file.hasFileExtension("mid;midi;xml;musicxml;mxl")) {
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data)) {browser_.emitEventIfBrowserIsVisible("audioFileDrop","Could not read the dropped file");return;}
    auto* o=new juce::DynamicObject();o->setProperty("name",file.getFileName());
    o->setProperty("base64",juce::Base64::toBase64(data.getData(),data.getSize()));
    browser_.emitEventIfBrowserIsVisible("scoreFileDrop",juce::var(o));return;
  }
  if (!file.loadFileAsData(pendingAudio_)) {browser_.emitEventIfBrowserIsVisible("audioFileDrop","Could not read the dropped recording");return;}
  pendingName_ = file.getFileName();
  auto* o = new juce::DynamicObject(); o->setProperty("name", pendingName_);
  browser_.emitEventIfBrowserIsVisible("audioFileDrop", juce::var(o));
}
