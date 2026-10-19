// Exercises installed AU bundles through Apple's real wrapper and host callbacks.
// macOS: clang++ -std=c++17 plugin/Tests/AuHostSmoke.cpp -framework AudioToolbox
//        -framework CoreFoundation -o out/au-host-smoke && out/au-host-smoke
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double rate = 44100.0;
constexpr UInt32 frames = 512;
constexpr int blocks = 256;

void require(OSStatus status, const char* operation) {
  if (status != noErr)
    throw std::runtime_error(std::string(operation) + ": OSStatus " + std::to_string(status));
}

void pump(double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct Host {
  AudioUnit unit = nullptr;
  int64_t timelineSample = 0;
  int64_t renderSample = 0;
  int jitter = 0;
  bool playing = true;

  static OSStatus beatAndTempo(void* context, Float64* beat, Float64* tempo) {
    const auto& h = *static_cast<Host*>(context);
    if (beat) *beat = (h.timelineSample + h.jitter) / rate * 2.0;
    if (tempo) *tempo = 120.0;
    return noErr;
  }

  static OSStatus transport(void* context, Boolean* playing, Boolean* changed,
                            Float64* sample, Boolean* looping,
                            Float64* loopStart, Float64* loopEnd) {
    const auto& h = *static_cast<Host*>(context);
    if (playing) *playing = h.playing;
    if (changed) *changed = h.timelineSample == 0;
    if (sample) *sample = static_cast<double>(h.timelineSample);
    if (looping) *looping = false;
    if (loopStart) *loopStart = 0.0;
    if (loopEnd) *loopEnd = 0.0;
    return noErr;
  }

  explicit Host(OSType subtype) {
    const AudioComponentDescription description{
        kAudioUnitType_MusicDevice, subtype, 'Sngd', 0, 0};
    const auto component = AudioComponentFindNext(nullptr, &description);
    if (!component) throw std::runtime_error("Installed Audio Unit was not found");
    UInt32 version = 0;
    require(AudioComponentGetVersion(component, &version), "component version");
    std::cout << "  installed version 0x" << std::hex << version << std::dec << std::endl;
    require(AudioComponentInstanceNew(component, &unit), "instantiate installed AU");
    try {
      HostCallbackInfo callbacks{};
      callbacks.hostUserData = this;
      callbacks.beatAndTempoProc = beatAndTempo;
      callbacks.transportStateProc = transport;
      require(AudioUnitSetProperty(unit, kAudioUnitProperty_HostCallbacks,
                                   kAudioUnitScope_Global, 0, &callbacks, sizeof(callbacks)),
              "set host callbacks");
      AudioStreamBasicDescription format{};
      format.mSampleRate = rate;
      format.mFormatID = kAudioFormatLinearPCM;
      format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
      format.mBytesPerPacket = format.mBytesPerFrame = sizeof(float);
      format.mFramesPerPacket = 1;
      format.mChannelsPerFrame = 2;
      format.mBitsPerChannel = 32;
      require(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 0, &format, sizeof(format)),
              "set stereo 44.1 kHz output");
      const UInt32 maximum = frames;
      require(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                   kAudioUnitScope_Global, 0, &maximum, sizeof(maximum)),
              "set maximum buffer size");
      require(AudioUnitInitialize(unit), "initialize AU");
      render(); // Publish the host tempo/rate before allowing the renderer to finish.
      pump(2.0);
    } catch (...) {
      AudioComponentInstanceDispose(unit);
      unit = nullptr;
      throw;
    }
  }

  ~Host() {
    if (unit) {
      AudioUnitUninitialize(unit);
      AudioComponentInstanceDispose(unit);
    }
  }

  std::array<float, frames> render() {
    std::array<float, frames> left{}, right{};
    struct StereoBuffers { UInt32 count; AudioBuffer buffers[2]; } buffers{
        2, {{1, frames * sizeof(float), left.data()},
            {1, frames * sizeof(float), right.data()}}};
    AudioTimeStamp timestamp{};
    timestamp.mSampleTime = static_cast<double>(renderSample);
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    require(AudioUnitRender(unit, &flags, &timestamp, 0, frames,
                            reinterpret_cast<AudioBufferList*>(&buffers)),
            "render AU block");
    renderSample += frames;
    for (size_t i = 0; i < left.size(); ++i) {
      if (!std::isfinite(left[i]) || left[i] != right[i])
        throw std::runtime_error("AU output contains nonfinite or unequal stereo samples");
    }
    return left;
  }

  std::vector<float> pass(bool withJitter) {
    std::vector<float> samples;
    samples.reserve(blocks * frames);
    for (int block = 0; block < blocks; ++block) {
      timelineSample = static_cast<int64_t>(block) * frames;
      jitter = withJitter && block > 0 ? (block % 2 ? 10 : -10) : 0;
      const auto audio = render();
      samples.insert(samples.end(), audio.begin(), audio.end());
    }
    return samples;
  }
};

std::vector<float> test(OSType subtype, const char* name) {
  std::cout << name << std::endl;
  Host host(subtype);
  const auto baseline = host.pass(false);
  const auto jittered = host.pass(true); // Also tests a real timeline seek back to zero.
  double energy = 0.0;
  float maxDifference = 0.0f;
  for (size_t i = 0; i < baseline.size(); ++i) {
    energy += baseline[i] * baseline[i];
    maxDifference = std::max(maxDifference, std::fabs(baseline[i] - jittered[i]));
  }
  const double rms = std::sqrt(energy / baseline.size());
  std::cout << "  " << baseline.size() << " samples, RMS " << rms
            << ", rounded PPQ max difference " << maxDifference << std::endl;
  if (rms < 0.01 || maxDifference > 1e-6f)
    throw std::runtime_error("Rounded PPQ playback differs from continuous baseline");

  host.timelineSample = 22050;
  host.jitter = 0;
  const auto seek = host.render();
  float seekDifference = 0.0f;
  for (size_t i = 0; i < seek.size(); ++i)
    seekDifference = std::max(seekDifference, std::fabs(seek[i] - baseline[22050 + i]));
  std::cout << "  timeline seek max difference " << seekDifference << std::endl;
  if (seekDifference > 1e-6f) throw std::runtime_error("AU did not follow real timeline seek");

  host.playing = false;
  const auto stopped = host.render();
  if (std::any_of(stopped.begin(), stopped.end(), [](float s) { return s != 0.0f; }))
    throw std::runtime_error("Stopped AU transport is not silent");
  return baseline;
}
} // namespace

int main() {
  try {
    const auto legacy = test('Nvsg', "New Virtual Singer compatibility AU (Nvsg)");
    pump(0.1);
    const auto current = test('Nvox', "Novox The Robot AU (Nvox)");
    if (legacy != current) throw std::runtime_error("The two installed identities render different audio");
    std::cout << "PASS: both installed AUs match exactly at 44.1 kHz/512 samples; "
                 "rounded PPQ, seek and stop checks passed." << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
