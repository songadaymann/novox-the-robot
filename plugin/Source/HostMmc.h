#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// MIDI Machine Control (MMC) from a virtual MIDI port named "Novox The Robot". No plugin API starts,
// stops or moves a host's transport; a host that listens to MMC input does it on these messages
// (Logic: File > Project Settings > Synchronization > MIDI > Listen to MIDI Machine Control (MMC)
// input, a per-project setting). Messages are for device 7F (all-call), the ID Logic answers by
// default. A locate is sent twice: as an MTC Full Frame message, which Logic's guide documents for
// moving its playhead, then as the MMC Locate command (Logic follows it too, without subframes). Both
// carry whole frames, rounded to the nearest. One port per process while any plugin instance holds
// it, opened with the instance so the host has picked it up before the first message. A virtual port
// needs macOS or Linux; elsewhere open() returns nullptr.
class HostMmc {
 public:
  // A host's timecode: base rate (24, 25, 30, 50, 60), 29.97/59.94 drop-frame numbering, and the
  // 1000/1001 pull-down of 23.976, 29.97 and 59.94.
  struct FrameRate {
    int base = 30;
    bool drop = false;
    bool pullDown = false;
  };
  // A timecode label as MTC sends it: hours with the rate in bits 5-6 (24, 25, 30 drop, 30), minutes,
  // seconds, frames. Rates over 30 are counted in pairs of frames (MTC has no code for them).
  struct Label {
    uint8_t hr = 0, mn = 0, sc = 0, fr = 0;
  };

  static std::shared_ptr<HostMmc> open();
  // A port of its own under `name` (tests), not shared.
  static std::unique_ptr<HostMmc> openNamed(const juce::String& name);
  ~HostMmc();

  bool play();
  bool stop();
  // Moves the host to `seconds` of real time after `originFrames`, the frame count of its bar 1 from
  // 00:00:00:00 (see hourFrames; a host's edit origin in seconds times framesPerSecond).
  bool locate(double originFrames, double seconds, FrameRate rate);
  bool sendRaw(const std::vector<uint8_t>& bytes);
  juce::String name() const { return name_; }

  // Frames from 00:00:00:00 to the label hh:00:00:00 (Logic puts bar 1 at 01:00:00:00).
  static double hourFrames(int hours, FrameRate rate);
  // Frames per second of real time (29.97 for 30 with pull-down).
  static double framesPerSecond(FrameRate rate);
  // The label nearest `seconds` after `originFrames`.
  static Label label(double originFrames, double seconds, FrameRate rate);

  // The messages: F0 7F 7F 06 <command> F7; the Locate F0 7F 7F 06 44 06 01 hr mn sc fr 00 F7; the
  // Full Frame F0 7F 7F 01 01 hr mn sc fr F7. Empty for a time that is not finite.
  static std::vector<uint8_t> commandMessage(int command);
  static std::vector<uint8_t> locateMessage(double originFrames, double seconds, FrameRate rate);
  static std::vector<uint8_t> fullFrameMessage(double originFrames, double seconds, FrameRate rate);

  static constexpr const char* portName = "Novox The Robot";

 private:
  HostMmc(std::unique_ptr<juce::MidiOutput> port, juce::String name);
  std::mutex mutex_;
  std::unique_ptr<juce::MidiOutput> port_;
  juce::String name_;
};
