#include "Song.h"

namespace {

juce::var noteToVar(const SongNote& n) {
  auto* o = new juce::DynamicObject();
  o->setProperty("pitch", n.pitch);
  o->setProperty("start", n.start);
  o->setProperty("dur", n.dur);
  if (n.lyric.isNotEmpty()) o->setProperty("lyric", n.lyric);
  return juce::var(o);
}

}  // namespace

juce::String Song::toJson() const {
  auto* o = new juce::DynamicObject();
  o->setProperty("format", "novox-song");
  o->setProperty("version", 1);
  if (keyCorrection.isObject()) o->setProperty("keyCorrection", keyCorrection);
  if (transcriptions.size() > 0) o->setProperty("transcriptions", transcriptions);
  o->setProperty("lyrics", lyrics);
  o->setProperty("useNoteLyrics", useNoteLyrics);
  o->setProperty("voice", voice);
  if (dictionary.isObject()) o->setProperty("dictionary", dictionary);
  juce::Array<juce::var> arr;
  for (const auto& n : notes) arr.add(noteToVar(n));
  o->setProperty("notes", arr);
  return juce::JSON::toString(juce::var(o), true);
}

Song Song::fromJson(const juce::String& json) {
  Song s;
  juce::var v = juce::JSON::parse(json);
  if (!v.isObject()) return s;
  s.keyCorrection = v.getProperty("keyCorrection", juce::var());
  const auto records = v.getProperty("transcriptions", juce::var());
  const auto legacy = v.getProperty("transcription", juce::var());
  if (records.isArray()) s.transcriptions = records;
  else if (legacy.isObject()) s.transcriptions = juce::Array<juce::var>{legacy};
  s.lyrics = v.getProperty("lyrics", "").toString();
  s.useNoteLyrics = static_cast<bool>(v.getProperty("useNoteLyrics", false));
  juce::var voice = v.getProperty("voice", "std");
  if (voice.isString() || voice.isObject()) s.voice = voice;
  juce::var dict = v.getProperty("dictionary", juce::var());
  if (dict.isObject()) s.dictionary = dict;
  if (auto* arr = v.getProperty("notes", juce::var()).getArray()) {
    for (const auto& nv : *arr) {
      SongNote n;
      n.pitch = juce::jlimit(0, 127, static_cast<int>(nv.getProperty("pitch", 60)));
      n.start = static_cast<double>(nv.getProperty("start", 0.0));
      n.dur = static_cast<double>(nv.getProperty("dur", 1.0));
      n.lyric = nv.getProperty("lyric", "").toString();
      if (n.dur > 0) s.notes.add(n);
    }
  }
  s.sortNotes();
  return s;
}

juce::String Song::toScoreJson(double bpm) const {
  auto* o = new juce::DynamicObject();
  o->setProperty("tempo", bpm);
  o->setProperty("units", "beats");
  o->setProperty("voice", voice);
  if (!useNoteLyrics && lyrics.isNotEmpty()) o->setProperty("lyrics", lyrics);
  if (dictionary.isObject()) o->setProperty("dictionary", dictionary);
  juce::Array<juce::var> arr;
  for (const auto& n : notes) arr.add(noteToVar(n));
  o->setProperty("notes", arr);
  return juce::JSON::toString(juce::var(o), true);
}

juce::StringArray Song::recordingIds() const {
  juce::StringArray ids;
  if (const auto* records = transcriptions.getArray())
    for (const auto& record : *records) {
      const auto id = record.getProperty("recordingId", "").toString();
      if (id.isNotEmpty()) ids.addIfNotAlreadyThere(id);
    }
  return ids;
}

void Song::sortNotes() {
  std::stable_sort(notes.begin(), notes.end(),
                   [](const SongNote& a, const SongNote& b) { return a.start < b.start; });
}

void Song::addNote(const SongNote& n) {
  const double end = n.start + n.dur;
  juce::Array<SongNote> kept;
  for (auto existing : notes) {
    const double eEnd = existing.start + existing.dur;
    if (eEnd <= n.start + 1e-6 || existing.start >= end - 1e-6) {
      kept.add(existing);  // no overlap
    } else if (existing.start < n.start) {
      existing.dur = n.start - existing.start;  // trim the tail of an earlier note
      kept.add(existing);
    }
    // notes starting inside the new one are replaced
  }
  kept.add(n);
  notes = kept;
  sortNotes();
}

Song Song::defaultSong() {
  // A short original phrase so a fresh instance sings something straight away.
  Song s;
  s.lyrics = "hel-lo, I'm a new vir-tual sing-er _";
  const int pitches[] = {64, 67, 69, 71, 72, 71, 69, 67, 64, 62};
  const double durs[] = {0.5, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1.5, 1.5};
  double t = 0;
  for (int i = 0; i < 10; ++i) {
    SongNote n;
    n.pitch = pitches[i];
    n.start = t;
    n.dur = durs[i];
    s.notes.add(n);
    t += durs[i];
  }
  return s;
}
