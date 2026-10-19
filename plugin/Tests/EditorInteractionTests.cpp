// Native editor integration: two independent processors and real WKWebViews, the embedded UI,
// dictionary, native bridge and system clipboard. Never sends global keys or controls a DAW.
// macOS builds this test file as Objective-C++ solely for targeted WK edit commands and exact
// pasteboard-item preservation. Run the executable with an optional JSON report pathname.
#include "../Source/PluginEditor.h"
#include <juce_events/juce_events.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#if JUCE_MAC
#import <Cocoa/Cocoa.h>
#endif

namespace {
juce::Array<juce::var> results;
juce::Array<juce::var> diagnostics;

void pump(int milliseconds) {
  const auto end = juce::Time::getMillisecondCounterHiRes() + milliseconds;
  while (juce::Time::getMillisecondCounterHiRes() < end)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

void require(bool condition, const juce::String& name) {
  auto* result = new juce::DynamicObject();
  result->setProperty("name", name); result->setProperty("passed", condition);
  results.add(juce::var(result));
  std::cout << (condition ? "PASS " : "FAIL ") << name << std::endl;
  if (!condition) throw std::runtime_error(name.toStdString());
}

template<class T> T* descendant(juce::Component& parent) {
  if (auto* match = dynamic_cast<T*>(&parent)) return match;
  for (auto* child : parent.getChildren()) if (auto* match = descendant<T>(*child)) return match;
  return nullptr;
}

juce::var evaluate(juce::WebBrowserComponent& browser, const juce::String& script) {
  struct Reply { bool done = false; juce::var value; juce::String error; };
  auto reply = std::make_shared<Reply>();
  browser.evaluateJavascript(script, [reply](juce::WebBrowserComponent::EvaluationResult result) {
    if (const auto* error = result.getError()) reply->error = error->message;
    else if (const auto* value = result.getResult()) reply->value = *value;
    reply->done = true;
  });
  const auto end = juce::Time::getMillisecondCounterHiRes() + 10000;
  while (!reply->done && juce::Time::getMillisecondCounterHiRes() < end) pump(10);
  if (!reply->done) throw std::runtime_error("WebView evaluation timed out");
  if (reply->error.isNotEmpty()) throw std::runtime_error(reply->error.toStdString());
  return reply->value;
}

void waitFor(juce::WebBrowserComponent& browser, const juce::String& condition, const juce::String& name,
             int timeout = 20000) {
  const auto end = juce::Time::getMillisecondCounterHiRes() + timeout;
  while (juce::Time::getMillisecondCounterHiRes() < end) {
    if (static_cast<bool>(evaluate(browser, "Boolean(" + condition + ")"))) { require(true, name); return; }
    pump(30);
  }
  // This browser contains only the synthetic test fixture. Never log the system clipboard.
  try {
    const auto state = juce::JSON::parse(evaluate(browser, R"JS(JSON.stringify({
      flash:window.__novox?.S.flash,
      lyrics:window.__novox?.S.song.lyrics,
      notes:window.__novox?.S.song.notes.map(n=>({pitch:n.pitch,start:n.start,dur:n.dur,lyric:n.lyric})),
      lyricEditor:{hidden:document.getElementById('lyricEdit')?.hidden,value:document.getElementById('lyricEdit')?.value},
      activeElement:document.activeElement?.id,
      undo:window.__novox?.H.undo.length,redo:window.__novox?.H.redo.length,
      dialogs:Array.from(document.querySelectorAll('dialog[open]'),e=>e.id)
    }))JS").toString());
    auto* item = new juce::DynamicObject(); item->setProperty("condition", name); item->setProperty("fixture", state);
    const juce::var diagnostic(item); diagnostics.add(diagnostic);
    std::cerr << "Fixture diagnostics: " << juce::JSON::toString(diagnostic, true) << std::endl;
  } catch (const std::exception& error) { std::cerr << "Could not collect fixture diagnostics: " << error.what() << std::endl; }
  require(false, name);
}

juce::String quote(const juce::String& text) { return juce::JSON::toString(juce::var(text)); }

class TestWindow : public juce::DocumentWindow {
 public:
  explicit TestWindow(const juce::String& title)
      : DocumentWindow(title, juce::Colours::lightgrey, 0) { setUsingNativeTitleBar(true); }
  void closeButtonPressed() override {}
};

struct EditorHost {
  NovoxProcessor processor;
  std::unique_ptr<NovoxEditor> editor;
  std::unique_ptr<TestWindow> window;
  juce::WebBrowserComponent* browser = nullptr;
  EditorHost(const juce::String& title, const juce::String& song, int offset) {
    processor.setSongJson(song);
    editor = std::make_unique<NovoxEditor>(processor);
    window = std::make_unique<TestWindow>(title);
    window->setContentNonOwned(editor.get(), true);
    window->setBounds(60 + offset, 80 + offset, 1000, 700);
    window->setVisible(true);
    browser = descendant<juce::WebBrowserComponent>(*editor);
    if (!browser) throw std::runtime_error("Editor has no native browser");
#if JUCE_MAC
    // Do not activate this test application or displace the user's frontmost DAW window.
    if (auto* peer = window->getPeer()) [((NSView*)peer->getNativeHandle()).window orderBack:nil];
#endif
    waitFor(*browser, "window.__novox && window.__novox.S.loaded && window.__novox.S.plan", title + " booted");
    pump(300);
    evaluate(*browser, "window.__novox.H.undo.length=0;window.__novox.H.redo.length=0;true");
  }
  ~EditorHost() { window->clearContentComponent(); editor.reset(); }
};

const juce::String snapshotScript = R"JS(
JSON.stringify({notes:window.__novox.S.song.notes.map(n=>({pitch:n.pitch,start:n.start,dur:n.dur,lyric:n.lyric||""})),
lyrics:window.__novox.S.song.lyrics,noteMode:!!window.__novox.S.song.useNoteLyrics})
)JS";
juce::String snapshot(EditorHost& host) { return evaluate(*host.browser, snapshotScript).toString(); }

void shortcut(EditorHost& host, const juce::String& key) {
  evaluate(*host.browser, "document.getElementById('scroller').focus();document.dispatchEvent(new KeyboardEvent('keydown',{key:"
           + quote(key) + ",metaKey:true,bubbles:true,cancelable:true}));true");
}

void editLyric(EditorHost& host, int index, const juce::String& text) {
  auto& browser = *host.browser;
  evaluate(browser, "(()=>{const s=window.__novox.S,n=s.song.notes[" + juce::String(index) +
    "];const sc=document.getElementById('scroller');sc.scrollLeft=Math.max(0,n.start*s.ppb-100);"
    "sc.scrollTop=Math.max(0,(s.hi-n.pitch)*s.rowH-80);return true})()");
  pump(50);
  evaluate(browser, "(()=>{const s=window.__novox.S,n=s.song.notes[" + juce::String(index) +
    "];const sc=document.getElementById('scroller'),r=sc.getBoundingClientRect();"
    "sc.dispatchEvent(new MouseEvent('dblclick',{bubbles:true,cancelable:true,"
    "clientX:r.left+n.start*s.ppb-sc.scrollLeft+Math.min(10,n.dur*s.ppb/2),"
    "clientY:r.top+(s.hi-n.pitch+.5)*s.rowH-sc.scrollTop}));return !document.getElementById('lyricEdit').hidden})()");
  require(static_cast<bool>(evaluate(browser, "!document.getElementById('lyricEdit').hidden")), "Double click opens note lyric editor");
  evaluate(browser, "(()=>{const e=document.getElementById('lyricEdit');e.value=" + quote(text) +
    ";e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new KeyboardEvent('keydown',"
    "{key:'Enter',bubbles:true,cancelable:true}));return true})()");
}

#if JUCE_MAC
class PasteboardGuard {
  NSPasteboard* board = [NSPasteboard generalPasteboard];
  NSMutableArray* saved = [[NSMutableArray alloc] init];
  NSInteger ownedChange = -1;
 public:
  PasteboardGuard() {
    for (NSPasteboardItem* item in board.pasteboardItems) {
      NSPasteboardItem* copy = [[NSPasteboardItem alloc] init];
      for (NSPasteboardType type in item.types) {
        NSData* data = [item dataForType:type];
        if (!data) { [copy release]; [saved release]; throw std::runtime_error("Could not preserve all clipboard formats; test stopped"); }
        [copy setData:data forType:type];
      }
      [saved addObject:copy]; [copy release];
    }
  }
  void markOwned() { ownedChange = board.changeCount; }
  ~PasteboardGuard() {
    // If the human copied something while the test ran, that newer clipboard takes precedence.
    if (ownedChange >= 0 && board.changeCount == ownedChange) {
      [board clearContents];
      if (saved.count) [board writeObjects:saved];
    }
    [saved release];
  }
};

void nativeEdit(EditorHost& host, SEL action) {
  auto* native = descendant<juce::NSViewComponent>(*host.browser);
  if (!native) throw std::runtime_error("WKWebView native view not found");
  NSView* view = (NSView*)native->getView();
  [view.window makeFirstResponder:view];
  // Same targeted responder action used by JUCE's Cmd-C / Cmd-V implementation. The action
  // generates real WK clipboard events; no global key event or other application is touched.
  if (![NSApp sendAction:action to:view.window.firstResponder from:view])
    throw std::runtime_error("WKWebView did not handle native edit command");
}
#else
struct PasteboardGuard { void markOwned() {} };
#endif

void copy(EditorHost& host, PasteboardGuard& guard) {
  evaluate(*host.browser, "window.__novox.S.sel=new Set(window.__novox.S.song.notes.map(n=>n._id));"
                         "document.getElementById('scroller').focus();true");
#if JUCE_MAC
  const auto previousChange = [NSPasteboard generalPasteboard].changeCount;
  nativeEdit(host, @selector(copy:));
#else
  shortcut(host, "c");
#endif
  const auto end = juce::Time::getMillisecondCounterHiRes() + 15000;
  while (juce::Time::getMillisecondCounterHiRes() < end) {
    const auto value = juce::JSON::parse(juce::SystemClipboard::getTextFromClipboard());
    const bool changed =
#if JUCE_MAC
      [NSPasteboard generalPasteboard].changeCount != previousChange;
#else
      true;
#endif
    if (changed && value["format"].toString() == "novox.melody") { guard.markOwned(); require(true, "Native Copy writes system clipboard melody"); return; }
    pump(30);
  }
  require(false, "Native Copy writes system clipboard melody");
}

void paste(EditorHost& host) {
  evaluate(*host.browser, "document.getElementById('scroller').focus();true");
#if JUCE_MAC
  nativeEdit(host, @selector(paste:));
#else
  shortcut(host, "v");
#endif
}

void run() {
  PasteboardGuard clipboard;
  const juce::String sourceSong = R"JSON({"lyrics":"one two three four five six","voice":"std","notes":[
    {"pitch":60,"start":8,"dur":1},{"pitch":62,"start":9,"dur":1},{"pitch":64,"start":10,"dur":1},
    {"pitch":65,"start":11,"dur":1},{"pitch":67,"start":12,"dur":1},{"pitch":69,"start":13,"dur":1}]})JSON";
  const juce::String targetSong = R"JSON({"lyrics":"we sing","voice":"robot","useNoteLyrics":true,"notes":[
    {"pitch":55,"start":0,"dur":1,"lyric":"we"},{"pitch":57,"start":1,"dur":1,"lyric":"sing"}]})JSON";
  EditorHost source("Novox test source", sourceSong, 0);
  EditorHost target("Novox test destination", targetSong, 30);
  const auto initialSource = snapshot(source), initialTarget = snapshot(target);

  editLyric(source, 1, "remember");
  waitFor(*source.browser, "window.__novox.S.song.lyrics.trim()==='one remember five six'", "Natural multisyllabic word updates correct lyric-pane position");
  require(static_cast<bool>(evaluate(*source.browser,
    "window.__novox.S.song.notes[1].lyric==='remember-' && window.__novox.S.song.notes[2].lyric==='-' && "
    "!window.__novox.S.song.notes[3].lyric && window.__novox.H.undo.length===1")), "Dictionary syllables occupy following notes with one Undo");
  shortcut(source, "z");
  require(snapshot(source) == initialSource, "One Undo restores pre-lyric song");
  editLyric(source, 1, "re-mem-ber");
  waitFor(*source.browser, "window.__novox.S.song.lyrics.trim()==='one re-mem-ber five six'", "Explicit syllable dashes update matching pane position");
  require(static_cast<bool>(evaluate(*source.browser,
    "window.__novox.S.song.notes[1].lyric==='re-' && window.__novox.S.song.notes[2].lyric==='mem-' && "
    "window.__novox.S.song.notes[3].lyric==='ber' && window.__novox.H.undo.length===1")), "Explicit syllables occupy following notes with one Undo");
  // The synthetic double click does not send a pointerdown selection. Select through the real
  // key handler before opening the failing edit, so the later Copy button is genuinely enabled.
  shortcut(source, "a");
  const auto beforeOverflow = snapshot(source);
  editLyric(source, 5, "remember");
  waitFor(*source.browser, "window.__novox.S.flash?.state==='CAN\\u2019T CHANGE LYRIC'", "Too few following notes produces an error");
  require(snapshot(source) == beforeOverflow && static_cast<int>(evaluate(*source.browser, "window.__novox.H.undo.length")) == 1,
          "Failed lyric edit leaves song and Undo unchanged");
  require(static_cast<bool>(evaluate(*source.browser,
    "!document.getElementById('lyricEdit').hidden && document.getElementById('lyricEdit').value==='remember'")),
    "Failed lyric text stays available for correction");
  require(static_cast<bool>(evaluate(*source.browser, "!document.getElementById('copyMelody').disabled")),
          "Copy command is enabled during failed pending edit");
#if JUCE_MAC
  const auto clipboardBeforeFailedCopy = [NSPasteboard generalPasteboard].changeCount;
#else
  const auto clipboardBeforeFailedCopy = juce::SystemClipboard::getTextFromClipboard();
#endif
  evaluate(*source.browser, "window.__novox.S.flash=null;document.getElementById('copyMelody').click();true");
  waitFor(*source.browser, "window.__novox.S.flash?.state==='CAN\\u2019T CHANGE LYRIC'", "Copy stops when pending lyric edit fails");
  require(snapshot(source) == beforeOverflow &&
#if JUCE_MAC
    [NSPasteboard generalPasteboard].changeCount == clipboardBeforeFailedCopy,
#else
    juce::SystemClipboard::getTextFromClipboard() == clipboardBeforeFailedCopy,
#endif
    "Failed pending edit does not replace clipboard or change song");
  evaluate(*source.browser,
    "document.getElementById('lyricEdit').dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true,cancelable:true}));true");
  waitFor(*source.browser, "document.getElementById('lyricEdit').hidden", "Escape cancels failed pending edit");

  copy(source, clipboard);
  paste(target);
  waitFor(*target.browser, "window.__novox.S.song.notes.length===8", "Second instance pastes from actual system clipboard");
  require(static_cast<bool>(evaluate(*target.browser,
    "window.__novox.S.song.notes.slice(2).every((n,i)=>n.start===i+8 && n.dur===1) && "
    "window.__novox.S.song.notes[0].lyric==='we' && window.__novox.S.song.notes[1].lyric==='sing' && "
    "window.__novox.S.song.voice==='robot' && window.__novox.H.undo.length===1")), "Paste preserves timeline, existing lyrics and destination voice");
  require(static_cast<bool>(evaluate(*target.browser,
    "window.__novox.S.song.lyrics.replace(/\\s+/g,' ').trim()==='we sing one re-mem-ber five six'")), "Pasted lyrics appear at correct pane position");
  shortcut(target, "z");
  require(snapshot(target) == initialTarget, "One Undo restores entire cross-instance paste");
  pump(500); // Complete that undo's processor push before replacing the test fixture.

  target.processor.setSongJson(R"JSON({"lyrics":"old old","voice":"robot","useNoteLyrics":true,"notes":[
    {"pitch":50,"start":8,"dur":3,"lyric":"old"},{"pitch":51,"start":11,"dur":3,"lyric":"old"}]})JSON");
  waitFor(*target.browser, "window.__novox.S.song.notes.length===2 && window.__novox.S.song.notes[0].pitch===50", "Overlap fixture reaches second editor");
  pump(300);
  evaluate(*target.browser, "window.__novox.H.undo.length=0;window.__novox.H.redo.length=0;true");
  const auto beforeConflict = snapshot(target);
  paste(target);
  waitFor(*target.browser, "document.getElementById('pasteDialog').open", "Overlap requires confirmation before mutation");
  require(snapshot(target) == beforeConflict, "Open confirmation has not changed the song");
  evaluate(*target.browser, "document.querySelector('#pasteDialog button[value=cancel]').click();true");
  pump(150);
  require(snapshot(target) == beforeConflict && static_cast<int>(evaluate(*target.browser, "window.__novox.H.undo.length")) == 0,
          "Cancel leaves song and Undo unchanged");
  paste(target);
  waitFor(*target.browser, "document.getElementById('pasteDialog').open", "Retry asks for overwrite again");
  evaluate(*target.browser, "document.querySelector('#pasteDialog button[value=replace]').click();true");
  waitFor(*target.browser, "window.__novox.S.song.notes.length===6", "Confirmed overwrite installs copied melody");
  require(static_cast<int>(evaluate(*target.browser, "window.__novox.H.undo.length")) == 1, "Confirmed overwrite has one Undo step");
  shortcut(target, "z");
  require(snapshot(target) == beforeConflict, "One Undo restores overwritten melody and lyrics");
  require(snapshot(source) == beforeOverflow, "Other instance remains unchanged by paste and Undo");

#if JUCE_MAC
  evaluate(*source.browser, "(()=>{const e=document.getElementById('lyrics');e.value='normal text copy';e.focus();e.select();return true})()");
  nativeEdit(source, @selector(copy:));
  const auto end = juce::Time::getMillisecondCounterHiRes() + 5000;
  while (juce::SystemClipboard::getTextFromClipboard() != "normal text copy" && juce::Time::getMillisecondCounterHiRes() < end) pump(20);
  require(juce::SystemClipboard::getTextFromClipboard() == "normal text copy", "Native Copy in a text field remains ordinary text copy");
  clipboard.markOwned();
  juce::SystemClipboard::copyTextToClipboard("ordinary pasted text"); clipboard.markOwned();
  nativeEdit(source, @selector(paste:));
  waitFor(*source.browser, "document.getElementById('lyrics').value==='ordinary pasted text'", "Native Paste in a text field remains ordinary text paste");
#endif
}
}  // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI initialise;
#if JUCE_MAC
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
#endif
  juce::String error;
  try { run(); }
  catch (const std::exception& failure) { error = failure.what(); std::cerr << "Editor interaction test stopped: " << error << std::endl; }
  auto* report = new juce::DynamicObject();
  report->setProperty("passed", error.isEmpty()); report->setProperty("checks", juce::var(results)); report->setProperty("error", error);
  report->setProperty("diagnostics", juce::var(diagnostics));
  if (argc > 1) {
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[1]));
    file.getParentDirectory().createDirectory();
    if (!file.replaceWithText(juce::JSON::toString(juce::var(report), true))) {
      std::cerr << "Could not save test report" << std::endl; return 2;
    }
  }
  return error.isEmpty() ? 0 : 1;
}
