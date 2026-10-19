#include <juce_gui_extra/juce_gui_extra.h>
#import <Cocoa/Cocoa.h>
#include <functional>

namespace {
// A host can embed multiple plugin editors in one NSWindow. Only the editor whose
// actual native web view owns first responder may receive the shortcut.
bool ownsResponder(juce::Component& component, NSWindow* window) {
  if (!window || !component.isShowing()) return false;
  if (auto* native = dynamic_cast<juce::NSViewComponent*>(&component)) {
    NSView* view = (NSView*)native->getView();
    NSResponder* responder = window.firstResponder;
    if (view && view.window == window && [responder isKindOfClass:[NSView class]] &&
        (responder == view || [(NSView*)responder isDescendantOf:view])) return true;
  }
  for (auto* child : component.getChildren()) if (ownsResponder(*child, window)) return true;
  return false;
}
}  // namespace

void* installNovoxSelectAll(juce::Component& editor, std::function<void()> action) {
  auto* component = &editor;
  id token = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
    const auto flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (flags == NSEventModifierFlagCommand && [event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"a"]) {
      if (ownsResponder(*component, event.window)) action();
    }
    return event; // Preserve native Select All in text fields.
  }];
  return [token retain];
}
void removeNovoxSelectAll(void* token) {
  if (token) { [NSEvent removeMonitor:(id)token]; [(id)token release]; }
}

// Space for the host's transport: no plugin API starts or stops it, and the
// editor's web view takes every key press, so the page hands Space here when it
// is not typing. The press goes to the host view above the editor, made first
// responder for it, as an unhandled key would go (JUCE's AU wrapper does the
// same for Live). A fresh event: the page asks after its own key handling, when
// the original is no longer current. Returns false outside a host view.
bool forwardNovoxHostSpace(juce::Component& editor) {
  auto* peer = editor.getPeer();
  if (!peer) return false;
  NSView* view = (NSView*)peer->getNativeHandle();
  NSView* host = view.superview;
  NSWindow* window = view.window;
  if (!host || !window) return false;
  auto key = [&](NSEventType type) {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0
                           timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:window.windowNumber
                             context:nil characters:@" " charactersIgnoringModifiers:@" " isARepeat:NO keyCode:49];
  };
  NSEvent* down = key(NSEventTypeKeyDown);
  NSEvent* up = key(NSEventTypeKeyUp);
  if (!down || !up) return false;
  NSResponder* previous = window.firstResponder;
  [window makeFirstResponder:host];
  [host keyDown:down];
  [host keyUp:up];
  if (view.window == window && previous) [window makeFirstResponder:previous];
  return true;
}
