#pragma once

#include <juce_core/juce_core.h>

// What differs between the platforms the plugin runs on, for the editor and audio import.
namespace NovoxPlatform {

// status.platform: the page names keys (⌘ or Ctrl) and settings by it (plugin/ui/platform.js).
inline const char* name() {
#if JUCE_MAC
  return "mac";
#elif JUCE_WINDOWS
  return "windows";
#else
  return "linux";
#endif
}

// Novox's own folder on this computer: the lyrics model, the web view's data (Windows) and the
// files dragged out of the editor. macOS: ~/Library/Application Support/Novox. Windows:
// %LOCALAPPDATA%\Novox, which stays on this PC (the roaming %APPDATA% would copy the 574 MB model
// between PCs). Linux: ~/.config/Novox.
inline juce::File dataFolder() {
#if JUCE_WINDOWS
  return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Novox");
#elif JUCE_MAC
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Application Support/Novox");
#else
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Novox");
#endif
}

// That folder as the editor's messages name it.
inline juce::String dataFolderName() {
#if JUCE_WINDOWS
  return "%LOCALAPPDATA%\\Novox";
#elif JUCE_MAC
  return "Application Support/Novox";
#else
  return "~/.config/Novox";
#endif
}

// Where people let applications use the microphone.
inline juce::String microphoneSettings() {
#if JUCE_WINDOWS
  return "Settings > Privacy & security > Microphone";
#else
  return "System Settings";
#endif
}

}  // namespace NovoxPlatform
