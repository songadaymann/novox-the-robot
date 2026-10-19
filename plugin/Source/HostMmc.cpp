#include "HostMmc.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr uint8_t kAllCall = 0x7f;
constexpr int kStop = 0x01, kPlay = 0x02, kLocate = 0x44;

bool supported(const HostMmc::FrameRate& r) { return r.base == 24 || r.base == 25 || r.base == 30 || r.base == 50 || r.base == 60; }

}  // namespace

double HostMmc::framesPerSecond(FrameRate r) {
  if (!supported(r)) r = {};
  return r.base * (r.pullDown ? 1000.0 / 1001.0 : 1.0);
}

double HostMmc::hourFrames(int hours, FrameRate r) {
  if (!supported(r)) r = {};
  // A drop-frame hour skips 2 labels in 54 of its 60 minutes (29.97: 107892 frames, 59.94 twice that).
  const double perHour = r.drop && (r.base == 30 || r.base == 60) ? (r.base * 3600.0 - (r.base / 15) * 54) : r.base * 3600.0;
  return hours * perHour;
}

std::shared_ptr<HostMmc> HostMmc::open() {
  static std::mutex m;
  static std::weak_ptr<HostMmc> instance;
  std::lock_guard<std::mutex> lock(m);
  if (auto existing = instance.lock()) return existing;
  // JUCE derives a virtual port's unique ID from its name, so a second process (another plugin host
  // process) with the same name fails: it takes the next name.
  for (int n = 1; n <= 4; ++n) {
    const juce::String name = n == 1 ? juce::String(portName) : juce::String(portName) + " " + juce::String(n);
    if (auto port = openNamed(name)) {
      std::shared_ptr<HostMmc> created(port.release());
      instance = created;
      return created;
    }
  }
  return nullptr;
}

std::unique_ptr<HostMmc> HostMmc::openNamed(const juce::String& name) {
#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
  auto port = juce::MidiOutput::createNewDevice(name);
  if (!port) return nullptr;
  return std::unique_ptr<HostMmc>(new HostMmc(std::move(port), name));
#else
  juce::ignoreUnused(name);
  return nullptr;
#endif
}

HostMmc::HostMmc(std::unique_ptr<juce::MidiOutput> port, juce::String name) : port_(std::move(port)), name_(std::move(name)) {}
HostMmc::~HostMmc() = default;

bool HostMmc::play() { return sendRaw(commandMessage(kPlay)); }
bool HostMmc::stop() { return sendRaw(commandMessage(kStop)); }
bool HostMmc::locate(double originFrames, double seconds, FrameRate rate) {
  const auto full = fullFrameMessage(originFrames, seconds, rate), mmc = locateMessage(originFrames, seconds, rate);
  return sendRaw(full) && sendRaw(mmc);
}

bool HostMmc::sendRaw(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!port_) return false;
  port_->sendMessageNow(juce::MidiMessage(bytes.data(), static_cast<int>(bytes.size())));
  return true;
}

std::vector<uint8_t> HostMmc::commandMessage(int command) {
  return {0xf0, 0x7f, kAllCall, 0x06, static_cast<uint8_t>(command & 0x7f), 0xf7};
}

HostMmc::Label HostMmc::label(double originFrames, double seconds, FrameRate rate) {
  if (!supported(rate)) rate = {};
  int base = rate.base;
  double exact = std::max(0.0, originFrames + seconds * framesPerSecond(rate));
  if (base > 30) {  // 50 and 60: MTC counts them as 25 and 30
    exact /= 2;
    base /= 2;
  }
  // Frames since 00:00:00:00 (a count, not yet a label), to the nearest: hosts drop subframes.
  long long count = std::llround(exact);
  const bool drop = rate.drop && base == 30;
  if (drop) {
    // Drop-frame numbering skips labels 00 and 01 at each minute but every tenth: the count's label.
    constexpr long long perTen = 17982, perMinute = 1798;
    const long long tens = count / perTen, rest = count % perTen;
    count += 18 * tens + (rest > 2 ? 2 * ((rest - 2) / perMinute) : 0);
  }
  const long long secondsLabel = count / base;
  const int rateBits = base == 24 ? 0 : base == 25 ? 1 : drop ? 2 : 3;
  Label l;
  l.hr = static_cast<uint8_t>((rateBits << 5) | static_cast<int>((secondsLabel / 3600) % 24));
  l.mn = static_cast<uint8_t>((secondsLabel / 60) % 60);
  l.sc = static_cast<uint8_t>(secondsLabel % 60);
  l.fr = static_cast<uint8_t>(count % base);
  return l;
}

std::vector<uint8_t> HostMmc::locateMessage(double originFrames, double seconds, FrameRate rate) {
  if (!std::isfinite(originFrames) || !std::isfinite(seconds)) return {};
  const auto l = label(originFrames, seconds, rate);
  return {0xf0, 0x7f, kAllCall, 0x06, kLocate, 0x06, 0x01, l.hr, l.mn, l.sc, l.fr, 0x00, 0xf7};
}

std::vector<uint8_t> HostMmc::fullFrameMessage(double originFrames, double seconds, FrameRate rate) {
  if (!std::isfinite(originFrames) || !std::isfinite(seconds)) return {};
  const auto l = label(originFrames, seconds, rate);
  return {0xf0, 0x7f, kAllCall, 0x01, 0x01, l.hr, l.mn, l.sc, l.fr, 0xf7};
}
