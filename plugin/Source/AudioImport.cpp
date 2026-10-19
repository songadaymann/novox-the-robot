#include "AudioImport.h"
#include "novox.h"
#include "transcribe.hpp"
#include "whisper.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
constexpr int rate = 16000;
constexpr int64_t modelBytes = 574041195;
const char* modelHash = "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2";
// whisper.cpp takes a window for a repetition loop, and decodes it again at a higher temperature,
// when the entropy of its last 32 tokens' counts falls under entropy_thold (natural log, about
// 11 equally common tokens at 2.4). whisper.cpp's default 2.4 stays: 1.5 kept sung vocable lines
// ("la la la") on Jonathan's dev takes, but on held-out full songs it let real loops through
// (CSD en020a lost its whole lyric to "1 liter, 2 liter..." x6; en002b repeated a line 44 times)
// and gained nothing elsewhere (tools/import_lab.py asr --variant entropy=1.5 to re-test).
constexpr float repetitionEntropy = 2.4f;
// The shortest word: one DTW step (an audio token is 20 ms), so words finishing together stay ordered.
constexpr double minWordSeconds = 0.02;
// juce::String reads a plain char* as ASCII, so text with "…" or "–" must say it is UTF-8
// (otherwise the LCD shows "â€¦").
juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }
juce::var errorResult(const juce::String& message) {
  auto* o = new juce::DynamicObject(); o->setProperty("error", message); return juce::var(o);
}
juce::String wavData(const std::vector<float>& audio, double sr) {
  juce::MemoryBlock bytes;
  juce::WavAudioFormat format;
  auto stream = std::make_unique<juce::MemoryOutputStream>(bytes, false);
  std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), sr, 1, 16, {}, 0));
  if (!writer) throw std::runtime_error("Could not create preview audio");
  stream.release();
  const float* channels[] = {audio.data()};
  if (!writer->writeFromFloatArrays(channels, 1, static_cast<int>(audio.size())))
    throw std::runtime_error("Could not write preview audio");
  writer.reset();
  return "data:audio/wav;base64," + juce::Base64::toBase64(bytes.getData(), bytes.getSize());
}
// The MARK marker names and COMT comments of an AIFF/AIFC file, read from its bytes. JUCE's reader
// stops reading an AIFC file's chunks at the first unknown one after SSND, and Logic writes MARK
// last, after its private LGWV and ResU chunks. Every count and length is checked against its
// chunk, so a malformed file yields fewer texts, never a read past the data.
struct AiffTexts { bool aiff = false; juce::StringArray markers, comments; };
AiffTexts aiffTexts(const juce::MemoryBlock& data) {
  AiffTexts texts;
  const auto* bytes = static_cast<const unsigned char*>(data.getData());
  const size_t size = data.getSize();
  const auto is = [bytes](size_t at, const char* id) { return std::memcmp(bytes + at, id, 4) == 0; };
  const auto u16 = [bytes](size_t at) { return static_cast<size_t>(bytes[at]) << 8 | bytes[at + 1]; };
  const auto u32 = [u16](size_t at) { return u16(at) << 16 | u16(at + 2); };
  const auto text = [bytes](size_t at, size_t count) {
    const auto* start = reinterpret_cast<const char*>(bytes + at);
    return juce::CharPointer_UTF8::isValidString(start, static_cast<int>(count))
        ? juce::String::fromUTF8(start, static_cast<int>(count)).trim() : juce::String();
  };
  if (size < 12 || !is(0, "FORM") || !(is(8, "AIFF") || is(8, "AIFC"))) return texts;
  texts.aiff = true;
  // Chunks run to the end of the data, so a FORM size written too small still finds MARK.
  for (size_t at = 12; at + 8 <= size;) {
    const size_t body = at + 8, length = u32(at + 4);
    if (length > size - body) break;
    const size_t end = body + length;
    if (is(at, "MARK") && length >= 2) {
      // Per marker: id (2), position (4), name as a pascal string padded to an even size.
      size_t p = body + 2;
      for (size_t n = u16(body); n > 0 && p + 7 <= end; --n) {
        const size_t count = bytes[p + 6];
        if (p + 7 + count > end) break;
        texts.markers.add(text(p + 7, count));
        p += 7 + count + (count % 2 == 0 ? 1 : 0);
      }
    } else if (is(at, "COMT") && length >= 2) {
      // Per comment: timestamp (4), marker id (2), text length (2), text padded to an even size.
      size_t p = body + 2;
      for (size_t n = u16(body); n > 0 && p + 8 <= end; --n) {
        const size_t count = u16(p + 6);
        if (p + 8 + count > end) break;
        texts.comments.add(text(p + 8, count));
        p += 8 + count + count % 2;
      }
    }
    at = end + length % 2;
  }
  return texts;
}
AudioImport::TimelineStamp stampOf(const juce::AudioFormatReader& reader, const juce::MemoryBlock& data) {
  const auto& meta = reader.metadataValues;
  if (reader.sampleRate <= 0) return {};
  // DAW exports stamp a region's timeline position into the Broadcast WAV header
  // (Logic: samples since 00:00:00:00, with bar 1 at 01:00:00:00 by default).
  const auto timeReference = meta[juce::WavAudioFormat::bwavTimeReference];
  if (timeReference.isNotEmpty() && timeReference.getLargeIntValue() > 0)
    return {timeReference.getLargeIntValue() / reader.sampleRate, meta[juce::WavAudioFormat::bwavOriginator].trim()};
  // A region dragged from Logic onto a plugin arrives as a temporary AIFF named after the region,
  // with no bext. Its markers are named "Tempo: 123.0", "Start", "End" and "Timestamp: <samples
  // since 00:00:00:00 at the file's rate>", and its COMT comment reads "Creator: Logic Pro 12.4".
  // The timestamp is taken as the time of the file's first sample (Logic puts it and "Start" at 0).
  // A WAV could carry the same marker as a cue label (JUCE reads LIST adtl labl chunks).
  auto texts = aiffTexts(data);
  juce::String creator;
  if (!texts.aiff) {
    for (int i = 0, n = juce::jmin(meta["NumCueLabels"].getIntValue(), 1000); i < n; ++i)
      texts.markers.add(meta["CueLabel" + juce::String(i) + "Text"].trim());
    creator = meta[juce::WavAudioFormat::riffInfoSoftware].trim();
  }
  for (const auto& marker : texts.markers) {
    if (!marker.startsWith("Timestamp:")) continue;
    const auto digits = marker.fromFirstOccurrenceOf(":", false, false).trim();
    if (digits.isEmpty() || digits.length() > 15 || !digits.containsOnly("0123456789")) continue;
    for (const auto& comment : texts.comments)
      if (comment.startsWith("Creator:")) { creator = comment.fromFirstOccurrenceOf(":", false, false).trim(); break; }
    // As far as we know only Logic writes this marker form, so a file that doesn't name its
    // creator is taken as Logic's (bar 1 at a whole hour, as the editor assumes for Logic).
    return {digits.getLargeIntValue() / reader.sampleRate, creator.isNotEmpty() ? creator : "Logic Pro"};
  }
  return {};
}
}

AudioImport::AudioImport() : juce::Thread("Novox audio import") {}
AudioImport::~AudioImport() { cancel(); stopThread(-1); }
juce::File AudioImport::modelFile() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
      .getChildFile("Application Support")
#endif
      .getChildFile("Novox").getChildFile("ggml-large-v3-turbo-q5_0.bin");
}
void AudioImport::publish(const juce::String& state, const juce::String& message, juce::var result) {
  std::lock_guard<std::mutex> lock(mutex_); state_ = state; message_ = message; result_ = result;
}
juce::var AudioImport::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto* o = new juce::DynamicObject();
  o->setProperty("state", state_); o->setProperty("message", message_);
  if (state_ == "ready") o->setProperty("result", result_);
  o->setProperty("modelAvailable", modelFile().getSize() == modelBytes);
  return juce::var(o);
}
juce::String AudioImport::transcribeOptions(bool detailedNotes) {
  auto* o = new juce::DynamicObject();
  o->setProperty("algorithm", "singing-v2");
  o->setProperty("detail", detailedNotes ? "detailed" : "natural");
  return juce::JSON::toString(juce::var(o), true);
}
juce::Array<juce::var> AudioImport::wordsFromPieces(const std::vector<Piece>& pieces, double seconds) {
  juce::Array<juce::var> words;
  juce::String wordText;
  juce::Array<juce::var> wordTokens;
  double wordStart = 0, wordEnd = 0, previousEnd = 0;
  float wordProbability = 1;
  auto finishWord = [&] {
    if (wordText.trim().isEmpty()) return;
    auto* w = new juce::DynamicObject(); w->setProperty("word", wordText.trim());
    w->setProperty("start", wordStart); w->setProperty("end", wordEnd);
    w->setProperty("probability", wordProbability); w->setProperty("tokens", juce::var(wordTokens));
    words.add(juce::var(w));
    previousEnd = wordEnd; wordText.clear(); wordTokens.clear(); wordProbability = 1;
  };
  for (const auto& piece : pieces) {
    if (piece.text.startsWithChar(' ') && wordText.isNotEmpty()) finishWord();
    if (wordText.isEmpty()) wordStart = previousEnd;
    wordText += piece.text;
    wordEnd = std::max(wordStart + minWordSeconds, piece.time);
    wordProbability = std::min(wordProbability, piece.probability);
    // Per-piece completion times let the engine anchor syllables inside long words.
    auto* tokenData = new juce::DynamicObject(); tokenData->setProperty("text", piece.text);
    tokenData->setProperty("time", piece.time);
    tokenData->setProperty("probability", juce::jlimit(0.0f, 1.0f, piece.probability));
    wordTokens.add(juce::var(tokenData));
  }
  finishWord();
  if (words.isEmpty()) return words;
  // The final sung vowel can continue past the model's last text token; asrEnd keeps the
  // measured end so the engine can tell it from the forced one.
  auto* last = words.getReference(words.size() - 1).getDynamicObject();
  last->setProperty("asrEnd", last->getProperty("end"));
  // A run of words can share one DTW time at the end of the file (a repetition loop decoded in
  // the last window: CSD en002a puts 89 words in its last 0.5 s), and each still takes
  // minWordSeconds, so the chain of starts ran past the end of the audio and the last word,
  // forced to end there, ended before it started (the engine rejects it). Such words now sit at
  // the end of the audio; words already inside it are unchanged.
  for (auto& word : words) {
    auto* w = word.getDynamicObject();
    w->setProperty("start", std::min(static_cast<double>(w->getProperty("start")), seconds));
    w->setProperty("end", std::min(static_cast<double>(w->getProperty("end")), seconds));
  }
  last->setProperty("end", seconds);
  return words;
}
AudioImport::LyricRetryWindow AudioImport::lyricRetryWindow(const juce::Array<juce::var>& words,
                                                           const juce::var& score, double seconds) {
  LyricRetryWindow best;
  const auto* notes = score["notes"].getArray();
  if (!notes || words.size() < 2) return best;
  // A held vowel alone is not missing lyrics. Require several separately detected, confident
  // notes without text inside a long recognition gap. Ordinary short gaps never invoke ASR again.
  for (int i = 0; i < words.size(); ++i) {
    const double a = words[i]["start"], b = words[i].getProperty("asrEnd", words[i]["end"]);
    if (!std::isfinite(a) || !std::isfinite(b) || b - a < 4.0) continue;
    LyricRetryWindow candidate;
    candidate.firstWord = std::max(0, i - 1);
    candidate.lastWord = std::min(words.size() - 1, i + 1);
    candidate.start = words[candidate.firstWord]["start"];
    candidate.end = std::min(seconds, static_cast<double>(words[candidate.lastWord]["end"]));
    if (candidate.start < 0 || candidate.end - candidate.start > 30.0) continue;
    for (const auto& note : *notes) {
      const double start = note["start"], duration = note["dur"];
      if (start < a - .1 || start >= b || duration < .08 ||
          static_cast<double>(note.getProperty("confidence", 0)) < .65 ||
          note.getProperty("lyric", "").toString().isNotEmpty()) continue;
      ++candidate.unclaimedNotes;
      candidate.unclaimedSeconds += std::max(0.0, std::min(start + duration, b) - std::max(start, a));
    }
    if (candidate.unclaimedNotes >= 6 && candidate.unclaimedSeconds >= 2.0 &&
        candidate.unclaimedSeconds > best.unclaimedSeconds) best = candidate;
  }
  return best;
}
juce::Array<juce::var> AudioImport::mergeLyricRetry(const juce::Array<juce::var>& words,
    const std::vector<Piece>& localPieces, const LyricRetryWindow& window, double seconds) {
  if (window.firstWord < 0 || window.lastWord < window.firstWord || window.lastWord >= words.size() ||
      window.end <= window.start || window.end - window.start > 30 || localPieces.empty()) return {};
  const auto normalized = [](const juce::String& text) {
    return text.toLowerCase().retainCharacters("abcdefghijklmnopqrstuvwxyz");
  };
  const auto vocable = [](const juce::String& word) {
    return word == "oh" || word == "ooh" || word == "ah" || word == "la" || word == "na";
  };
  juce::StringArray originalContext, replacementContext;
  int originalVocables = 0;
  for (int i = window.firstWord; i <= window.lastWord; ++i) {
    const auto word = normalized(words[i]["word"].toString());
    if (vocable(word)) ++originalVocables;
    else if (word.isNotEmpty()) originalContext.add(word);
  }
  // Keep a lexical anchor: a completely unrecognized or instrumental passage cannot supply one.
  if (originalContext.isEmpty()) return {};
  const double duration = window.end - window.start;
  double previousPiece = 0;
  for (const auto& piece : localPieces) {
    if (!std::isfinite(piece.time) || piece.time < previousPiece || piece.time > duration + .05 ||
        !std::isfinite(piece.probability)) return {};
    previousPiece = piece.time;
  }
  auto replacement = wordsFromPieces(localPieces, duration);
  int addedVocables = -originalVocables, crowded = 0;
  double previousEnd = 0, vocableProbability = 0;
  int vocableCount = 0;
  for (const auto& wordData : replacement) {
    const auto word = normalized(wordData["word"].toString());
    const double end = wordData.getProperty("asrEnd", wordData["end"]);
    if (end - previousEnd < .08) ++crowded;
    previousEnd = end;
    if (vocable(word)) {
      ++addedVocables; ++vocableCount;
      vocableProbability += static_cast<double>(wordData["probability"]);
    } else if (word.isNotEmpty()) replacementContext.add(word);
  }
  // Repetition is allowed only as separately timed, plausible vocables. A single "oh-oh-oh..."
  // token, extra lexical lines, or many words stamped at the crop end cannot pass these checks.
  if (replacementContext != originalContext || addedVocables < 4 ||
      addedVocables > window.unclaimedNotes * 3 / 2 + 2 ||
      replacement.size() > static_cast<int>(duration / .15) ||
      crowded > std::max(1, replacement.size() / 10) ||
      vocableCount == 0 || vocableProbability / vocableCount < .35) return {};
  std::vector<Piece> merged;
  const auto appendOriginal = [&merged](const juce::var& word) {
    if (const auto* tokens = word["tokens"].getArray())
      for (const auto& token : *tokens)
        merged.push_back({token["text"].toString(), static_cast<double>(token["time"]),
                          static_cast<float>(token["probability"])});
  };
  for (int i = 0; i < window.firstWord; ++i) appendOriginal(words[i]);
  for (size_t i = 0; i < localPieces.size(); ++i) {
    const auto& piece = localPieces[i];
    // A crop's first token is a new word even when the decoder omits its leading space.
    const auto text = i == 0 && !piece.text.startsWithChar(' ') ? " " + piece.text : piece.text;
    merged.push_back({text, std::min(seconds, piece.time + window.start), piece.probability});
  }
  for (int i = window.lastWord + 1; i < words.size(); ++i) appendOriginal(words[i]);
  double previousTime = 0;
  for (const auto& piece : merged) {
    if (!std::isfinite(piece.time) || piece.time < previousTime || piece.time > seconds) return {};
    previousTime = piece.time;
  }
  return wordsFromPieces(merged, seconds);
}
bool AudioImport::lyricRetryImprovesCoverage(const juce::var& original, const juce::var& alternative,
                                            const LyricRetryWindow& window) {
  const auto unclaimed = [&window](const juce::var& score) {
    double result = 0;
    if (const auto* notes = score["notes"].getArray()) for (const auto& note : *notes) {
      const double start = note["start"], duration = note["dur"], end = start + duration;
      const double inside = std::max(0.0, std::min(end, window.end) - std::max(start, window.start));
      if (note.getProperty("lyric", "").toString().isEmpty()) result += inside;
    }
    return result;
  };
  const double before = unclaimed(original);
  if (before <= 0 || unclaimed(alternative) >= before * .5) return false;
  // Note confidence also includes lyric confidence. Compare actual time coverage of the
  // original confident notes, not the confidence of newly recognized syllables. Merge the
  // alternative's intervals so duplicate/overlapping notes cannot inflate this coverage.
  std::vector<std::pair<double, double>> intervals, merged;
  int alternativeInside = 0, alternativeOutside = 0;
  if (const auto* notes = alternative["notes"].getArray()) for (const auto& note : *notes) {
    const double start = note["start"], end = start + static_cast<double>(note["dur"]);
    if (std::isfinite(start) && std::isfinite(end) && end > start) {
      intervals.emplace_back(start, end);
      const double middle = (start + end) * .5;
      if (middle >= window.start && middle < window.end) ++alternativeInside;
      else ++alternativeOutside;
    }
  }
  std::sort(intervals.begin(), intervals.end());
  for (const auto& interval : intervals) {
    if (merged.empty() || interval.first > merged.back().second) merged.push_back(interval);
    else merged.back().second = std::max(merged.back().second, interval.second);
  }
  const auto covered = [&merged](double start, double end) {
    double total = 0;
    for (const auto& interval : merged)
      total += std::max(0.0, std::min(end, interval.second) - std::max(start, interval.first));
    return total;
  };
  double inside = 0, outside = 0, insideCovered = 0, outsideCovered = 0;
  int originalInside = 0, originalOutside = 0;
  if (const auto* notes = original["notes"].getArray()) for (const auto& note : *notes) {
    if (static_cast<double>(note.getProperty("confidence", 0)) < .65) continue;
    const double start = note["start"], end = start + static_cast<double>(note["dur"]);
    const double middle = (start + end) * .5;
    if (middle >= window.start && middle < window.end) ++originalInside;
    else ++originalOutside;
    const double a = std::max(start, window.start), b = std::min(end, window.end);
    inside += std::max(0.0, b - a); insideCovered += covered(a, b);
    outside += std::max(0.0, std::min(end, window.start) - start) + std::max(0.0, end - std::max(start, window.end));
    outsideCovered += covered(start, std::min(end, window.start)) + covered(std::max(start, window.end), end);
  }
  // Covering all the sound with one long note would still erase its articulation. Allow some
  // realignment, but reject a collapse of the original confident note inventory on either side.
  return inside > 0 && insideCovered >= inside * .9 && outsideCovered >= outside * .95 &&
      alternativeInside * 4 >= originalInside * 3 && alternativeOutside * 4 >= originalOutside * 3;
}
AudioImport::TimelineStamp AudioImport::timelineStamp(const juce::MemoryBlock& data) {
  juce::AudioFormatManager formats; formats.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(
      std::make_unique<juce::MemoryInputStream>(data, false)));
  return reader ? stampOf(*reader, data) : TimelineStamp{};
}
juce::var AudioImport::start(juce::MemoryBlock data, juce::String name, bool download, bool melodyOnly,
                             bool detailedNotes) {
  if (isThreadRunning()) return errorResult("An audio conversion is already running");
  if (data.getSize() == 0) return errorResult("The recording is empty or could not be read");
  if (data.getSize() > 64 * 1024 * 1024)
    return errorResult("Choose an audio file smaller than 64 MB");
  if (!melodyOnly && modelFile().getSize() != modelBytes && !download)
    return errorResult("The local lyrics model must be downloaded first");
  previewScore_.clear();
  data_ = std::move(data); name_ = std::move(name); download_ = download; melodyOnly_ = melodyOnly;
  detailed_ = detailedNotes;
  publish("working", utf8("Reading audio…")); startThread(); return {};
}
juce::var AudioImport::preview(const juce::String& score) {
  if (isThreadRunning()) return errorResult("Please wait for the current conversion");
  { std::lock_guard<std::mutex> lock(mutex_);
    if (!result_.isObject()) return errorResult("Import a recording first");
    previousResult_ = result_.clone();
  }
  auto parsed = juce::JSON::parse(score);
  auto* notes = parsed.getProperty("notes", juce::var()).getArray();
  if (!notes || notes->isEmpty() || notes->size() > 10000) return errorResult("Invalid note list");
  for (const auto& n : *notes) {
    const double start = n.getProperty("start", -1), duration = n.getProperty("dur", -1), pitch = n.getProperty("pitch", -1);
    if (!std::isfinite(start) || !std::isfinite(duration) || !std::isfinite(pitch) || start < 0 || duration <= 0 ||
        start + duration > 300 || pitch < 0 || pitch > 127) return errorResult(utf8("Notes must fit within five minutes and MIDI pitches 0–127"));
  }
  previewScore_ = score; publish("working", utf8("Preparing Novox preview…")); startThread(); return {};
}
void AudioImport::cancel() { signalThreadShouldExit(); }

void AudioImport::run() {
  try {
    const auto check = [&] { if (threadShouldExit()) throw std::runtime_error("Cancelled"); };
    if (previewScore_.isNotEmpty()) {
      std::unique_ptr<novox_engine, decltype(&novox_destroy)> engine(novox_create(44100), novox_destroy);
      float* rendered = nullptr; size_t length = 0;
      if (novox_render(engine.get(), previewScore_.toRawUTF8(), &rendered, &length))
        throw std::runtime_error(novox_last_error(engine.get()));
      std::vector<float> audio(rendered, rendered + length); novox_free(rendered);
      check();
      previousResult_.getDynamicObject()->setProperty("preview", wavData(audio, 44100));
      previousResult_.getDynamicObject()->setProperty("score", juce::JSON::parse(previewScore_));
      publish("ready", "", previousResult_); previousResult_ = juce::var(); return;
    }
    check(); publish("working", utf8("Reading audio…"));
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(
        std::make_unique<juce::MemoryInputStream>(data_, false)));
    if (!reader) throw std::runtime_error("Could not decode this audio. Try a WAV or MP3 file");
    const double seconds = reader->lengthInSamples / reader->sampleRate;
    if (seconds <= 0 || seconds > 300 || reader->numChannels > 2)
      throw std::runtime_error("Use a recording between 0 and 5 minutes, with at most 2 channels");
    const auto stamp = stampOf(*reader, data_);
    // Decode in bounded blocks and resample with JUCE's anti-aliasing audio source.
    juce::AudioFormatReaderSource source(reader.release(), true);
    juce::ResamplingAudioSource resampler(&source, false, 2);
    auto* decoded = source.getAudioFormatReader();
    // Average stereo channels after anti-aliased resampling; mono is duplicated by ReaderSource.
    resampler.setResamplingRatio(decoded->sampleRate / rate);
    resampler.prepareToPlay(4096, rate);
    std::vector<float> audio(static_cast<size_t>(std::ceil(seconds * rate)));
    juce::AudioBuffer<float> block(2, 4096);
    for (size_t at = 0; at < audio.size(); at += 4096) {
      check(); const int count = static_cast<int>(std::min<size_t>(4096, audio.size() - at));
      resampler.getNextAudioBlock({&block, 0, count});
      for (int j = 0; j < count; ++j) audio[at + static_cast<size_t>(j)] =
          (block.getSample(0, j) + block.getSample(1, j)) * 0.5f;
    }
    resampler.releaseResources();
    auto model = modelFile();
    if (!melodyOnly_ && model.getSize() != modelBytes) {
      if (!download_) throw std::runtime_error("Lyrics model is missing");
      publish("working", utf8("Downloading local lyrics model (574 MB)…"));
      if (!model.getParentDirectory().createDirectory()) throw std::runtime_error("Cannot create model folder");
      // Unique partial files avoid races across plugin instances. Verify before publishing.
      auto partial = model.getSiblingFile(juce::Uuid().toString() + ".part");
      struct Cleanup { juce::File f; ~Cleanup() { f.deleteFile(); } } cleanup{partial};
      juce::URL url("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin");
      auto input = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
          .withConnectionTimeoutMs(15000).withNumRedirectsToFollow(10));
      if (!input) throw std::runtime_error("Model download failed; check your connection and retry");
      auto output = partial.createOutputStream();
      if (!output) throw std::runtime_error("Cannot save lyrics model");
      char downloadBlock[65536]; int64_t total = 0;
      while (!input->isExhausted()) {
        check(); const auto n = input->read(downloadBlock, sizeof(downloadBlock)); if (n <= 0) break;
        if (!output->write(downloadBlock, static_cast<size_t>(n))) throw std::runtime_error("Model disk write failed");
        total += n;
        if (total > modelBytes) throw std::runtime_error("Unexpected model download size");
      }
      output.reset(); check();
      if (total != modelBytes || juce::SHA256(partial).toHexString() != modelHash)
        throw std::runtime_error("Incomplete or invalid model download; retry");
      if (!partial.moveFileTo(model)) throw std::runtime_error("Cannot install lyrics model");
    }
    check(); publish("working", utf8("Finding notes…"));
    auto frames = novox::trackSingingPitch(audio.data(), audio.size(), rate);
    juce::Array<juce::var> pitchCurve, waveform;
    for (size_t i = 0; i < frames.size(); i += 3) {
      juce::Array<juce::var> point; point.add(frames[i].time); point.add(frames[i].midi);
      pitchCurve.add(juce::var(point));
    }
    for (size_t at = 0; at < audio.size(); at += 256) {
      float peak = 0; for (size_t i = at; i < std::min(audio.size(), at + 256); ++i) peak = std::max(peak, std::abs(audio[i]));
      waveform.add(peak);
    }
    juce::Array<juce::var> words;
    std::unique_ptr<whisper_context, decltype(&whisper_free)> ctx(nullptr, whisper_free);
    auto wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    const auto recognize = [&](const float* samples, int count, whisper_full_params settings,
                               bool required) {
      std::vector<Piece> pieces;
      if (whisper_full(ctx.get(), settings, samples, count) != 0) {
        check();
        if (required) throw std::runtime_error("Lyrics recognition failed");
        return pieces;
      }
      for (int i = 0; i < whisper_full_n_segments(ctx.get()); ++i) {
        for (int t = 0; t < whisper_full_n_tokens(ctx.get(), i); ++t) {
          const auto token = whisper_full_get_token_data(ctx.get(), i, t);
          if (token.id >= whisper_token_eot(ctx.get())) continue;
          // DTW marks completion of each text token, not its onset.
          pieces.push_back({juce::String::fromUTF8(whisper_full_get_token_text(ctx.get(), i, t)),
                            std::max(0.0, (token.t_dtw >= 0 ? token.t_dtw : token.t1) / 100.0), token.p});
        }
      }
      check();
      return pieces;
    };
    if (!melodyOnly_) {
      publish("working", utf8("Recognizing lyrics locally…"));
      auto params = whisper_context_default_params();
      params.flash_attn = false;
      params.dtw_token_timestamps = true;
      params.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;
      ctx.reset(whisper_init_from_file_with_params(model.getFullPathName().toRawUTF8(), params));
      if (!ctx) throw std::runtime_error("Cannot load lyrics model. Delete it from Application Support/Novox and retry");
      wp.language = "en"; wp.n_threads = 4; wp.no_context = true;
      wp.token_timestamps = true;
      wp.entropy_thold = repetitionEntropy;
      wp.print_progress = wp.print_realtime = wp.print_timestamps = wp.print_special = false;
      wp.abort_callback = [](void* u) { return static_cast<AudioImport*>(u)->threadShouldExit(); };
      wp.abort_callback_user_data = this;
      if (adjustRecognition) adjustRecognition(wp);
      words = wordsFromPieces(recognize(audio.data(), static_cast<int>(audio.size()), wp, true), seconds);
    }
    check(); publish("working", utf8("Aligning lyrics and preparing preview…"));
    std::unique_ptr<novox_engine, decltype(&novox_destroy)> engine(novox_create(44100), novox_destroy);
    // C API provides the embedded lexicon for word alignment.
    const auto align = [&](const juce::Array<juce::var>& transcript) {
      const auto wordJson = juce::JSON::toString(juce::var(transcript));
      char* raw = novox_transcribe(engine.get(), audio.data(), audio.size(), rate,
                                  wordJson.toRawUTF8(), transcribeOptions(detailed_).toRawUTF8());
      if (!raw) throw std::runtime_error(novox_last_error(engine.get()));
      const auto text = juce::String::fromUTF8(raw); novox_free(raw);
      check(); return juce::JSON::parse(text);
    };
    auto score = align(words);
    if (retrySparseLyrics && ctx) {
      const auto window = lyricRetryWindow(words, score, seconds);
      if (window.firstWord >= 0) {
        publish("working", utf8("Checking missing sung syllables…"));
        auto retrySettings = wp;
        // Only this short, acoustically supported gap keeps the deterministic decode when
        // Whisper regards real repeated vocables as a loop. The full take retains its safeguards.
        retrySettings.temperature = 0;
        retrySettings.temperature_inc = 0;
        const auto begin = static_cast<size_t>(window.start * rate);
        const auto end = std::min(audio.size(), static_cast<size_t>(std::ceil(window.end * rate)));
        const auto localPieces = recognize(audio.data() + begin, static_cast<int>(end - begin), retrySettings, false);
        auto replacement = mergeLyricRetry(words, localPieces, window, seconds);
        juce::var evidence(new juce::DynamicObject());
        if (inspectLyricRetry) {
          auto* e = evidence.getDynamicObject();
          e->setProperty("start", window.start); e->setProperty("end", window.end);
          e->setProperty("unclaimedNotes", window.unclaimedNotes);
          e->setProperty("words", juce::var(wordsFromPieces(localPieces, window.end - window.start)));
          e->setProperty("merged", !replacement.isEmpty()); e->setProperty("accepted", false);
        }
        if (!replacement.isEmpty()) {
          try {
            auto alternative = align(replacement);
            if (inspectLyricRetry) evidence.getDynamicObject()->setProperty("score", alternative);
            if (lyricRetryImprovesCoverage(score, alternative, window)) {
              words = std::move(replacement); score = std::move(alternative);
              if (inspectLyricRetry) evidence.getDynamicObject()->setProperty("accepted", true);
            }
          } catch (const std::exception& error) {
            // A failed optional alignment must not discard the valid first import.
            // Cancellation still exits promptly through the outer worker handler.
            check();
            if (inspectLyricRetry) evidence.getDynamicObject()->setProperty("error", utf8(error.what()));
          }
        }
        if (inspectLyricRetry) inspectLyricRetry(evidence);
      }
    }
    ctx.reset();
    const auto scoreJson = juce::JSON::toString(score);
    auto* noteList = score.getProperty("notes", juce::var()).getArray();
    if (!noteList || noteList->isEmpty()) throw std::runtime_error("No sung notes found. Try a clear solo vocal recording");
    float* rendered = nullptr; size_t length = 0;
    if (novox_render(engine.get(), scoreJson.toRawUTF8(), &rendered, &length))
      throw std::runtime_error(novox_last_error(engine.get()));
    std::vector<float> preview(rendered, rendered + length); novox_free(rendered);
    auto* result = new juce::DynamicObject(); result->setProperty("name", name_);
    result->setProperty("score", score); result->setProperty("words", juce::var(words)); result->setProperty("duration", seconds);
    result->setProperty("detail", detailed_ ? "detailed" : "natural");
    if (stamp.seconds > 0) { result->setProperty("timeReference", stamp.seconds); result->setProperty("originator", stamp.originator); }
    result->setProperty("waveform", juce::var(waveform)); result->setProperty("pitchCurve", juce::var(pitchCurve));
    result->setProperty("original", wavData(audio, rate)); result->setProperty("preview", wavData(preview, 44100));
    if (melodyOnly_) result->setProperty("notice", "Melody only: add lyrics before accepting");
    else if (words.isEmpty()) result->setProperty("notice", "No lyrics recognized: add or correct lyrics before accepting");
    check(); data_.reset(); publish("ready", "", juce::var(result));
  } catch (const std::exception& e) {
    data_.reset(); publish(threadShouldExit() ? "cancelled" : "error", utf8(e.what()),
                           previewScore_.isNotEmpty() ? previousResult_ : juce::var());
  }
}
