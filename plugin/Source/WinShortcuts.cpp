// Windows counterpart of MacShortcuts.mm.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <juce_gui_basics/juce_gui_basics.h>

// Space for the host's transport: no plugin API starts or stops it, and the editor's web view
// takes every key press, so the page hands Space here when it is not typing. The press goes to the
// host's window that holds the editor's window (its parent, else its owner), as WM_KEYDOWN,
// WM_CHAR and WM_KEYUP: what JUCE does with a key the editor leaves unhandled
// (juce_Windowing_windows.cpp, forwardMessageToParent). Posted, as JUCE does, and as a fresh press:
// the page asks after its own key handling. Returns whether all three were posted; whether the
// host acts on them is up to the host.
bool forwardNovoxHostSpace(juce::Component& editor) {
  auto* peer = editor.getPeer();
  if (!peer) return false;
  const auto window = static_cast<HWND>(peer->getNativeHandle());
  if (!window) return false;
  HWND host = GetParent(window);
  if (!host) host = GetWindow(window, GW_OWNER);
  if (!host) return false;
  // lParam: a repeat count of 1 and Space's scan code; a release also has the previous-state and
  // transition bits.
  const LPARAM press = 1 | (static_cast<LPARAM>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC)) << 16);
  const LPARAM release = press | static_cast<LPARAM>(0xC0000000u);
  return PostMessageW(host, WM_KEYDOWN, VK_SPACE, press) && PostMessageW(host, WM_CHAR, L' ', press) &&
         PostMessageW(host, WM_KEYUP, VK_SPACE, release);
}
