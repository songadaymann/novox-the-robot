#include "import.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

#include "lexicon.hpp"
#include "utf8.hpp"

namespace novox {

namespace {

// Keeps the result singable by one voice: sorts by start, drops lower notes of
// chords, and trims notes that overlap the next one.
void makeMonophonic(std::vector<Note>& notes, std::vector<std::string>& warnings) {
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    if (std::fabs(a.start - b.start) > 1e-6) return a.start < b.start;
    return a.pitch > b.pitch;  // highest first within a chord
  });
  std::vector<Note> out;
  int chords = 0;
  for (const auto& n : notes) {
    if (!out.empty() && std::fabs(n.start - out.back().start) < 1e-6) {
      ++chords;
      if (out.back().lyric.empty()) out.back().lyric = n.lyric;
      continue;
    }
    if (!out.empty() && out.back().start + out.back().dur > n.start) {
      out.back().dur = std::max(0.01, n.start - out.back().start);
    }
    out.push_back(n);
  }
  if (chords > 0) warnings.push_back(std::to_string(chords) + " chord note(s) dropped (kept the top note)");
  notes.swap(out);
}

std::string trimCopy(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// The first number in a file's text ("2", "-1", "0.5", "c. 90" -> 90), read with
// '.' as the decimal point whatever the C locale; `fallback` when there is none.
double parseNumber(const std::string& text, double fallback) {
  size_t a = text.find_first_of("0123456789");
  if (a == std::string::npos) return fallback;
  if (a > 0 && text[a - 1] == '.') --a;
  if (a > 0 && (text[a - 1] == '-' || text[a - 1] == '+')) --a;
  size_t end = a + (text[a] == '-' || text[a] == '+' ? 1 : 0);
  double whole = 0;
  while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) && end - a < 15)
    whole = whole * 10 + (text[end++] - '0');
  if (end == text.size() || (text[end] != '.' && text[end] != 'e' && text[end] != 'E' &&
                             !std::isdigit(static_cast<unsigned char>(text[end]))))
    return text[a] == '-' ? -whole : whole;  // a whole number, as most are
  std::istringstream in(text.substr(a));
  in.imbue(std::locale::classic());
  double v = 0;
  if (!(in >> v) || !std::isfinite(v)) return fallback;
  return v;
}

// A number for JSON, printf-formatted, with '.' as the decimal point whatever the C locale.
std::string formatFixed(const char* format, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, format, v);
  std::string s = buf;
  std::replace(s.begin(), s.end(), ',', '.');
  return s;
}

// Up to six decimals, trailing zeros dropped: tempos and positions as the file gave them.
std::string formatNumber(double v) {
  std::string s = formatFixed("%.6f", v);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

// One note's lyric from a file's text: control characters dropped, and what
// joins two syllables on one note (a space, a no-break space, the elision
// undertie U+203F or U+035C, SMuFL's lyricsElision U+E550-E552) becomes the
// engine's one-note mark: "a‿and" -> "a~and".
std::string cleanLyricText(const std::string& raw) {
  const std::string s = validUtf8(raw);
  std::string out;
  bool join = false;
  for (size_t i = 0; i < s.size();) {
    size_t length = 1;
    const uint32_t cp = utf8Decode(s, i, length);
    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == '~' || cp == 0xA0 || cp == 0x203F ||
        cp == 0x035C || (cp >= 0xE550 && cp <= 0xE552)) {
      join = true;
    } else if (cp >= 0x20 && cp != 0x7F) {
      if (join && !out.empty()) out += '~';
      join = false;
      out.append(s, i, length);
    }
    i += length;
  }
  return out;
}

// Beat (quarter note) -> seconds through a tempo map. Points are set in time
// order; a second tempo at the same beat replaces the first.
class TempoMap {
 public:
  void set(double beat, double bpm) {
    if (!(bpm > 0) || !std::isfinite(bpm)) return;
    if (!points_.empty() && std::fabs(points_.back().beat - beat) < 1e-9) {
      points_.back().bpm = bpm;
      if (points_.size() > 1 && std::fabs(points_[points_.size() - 2].bpm - bpm) < 1e-9) points_.pop_back();
      return;
    }
    if (!points_.empty() && std::fabs(points_.back().bpm - bpm) < 1e-9) return;
    ImportInfo::Tempo t;
    t.beat = beat;
    t.bpm = bpm;
    if (!points_.empty()) {
      const auto& p = points_.back();
      t.seconds = p.seconds + (beat - p.beat) * 60.0 / p.bpm;
    }
    points_.push_back(t);
  }
  double seconds(double beat) const {
    if (points_.empty()) return beat * 0.5;  // 120 bpm
    auto it = std::upper_bound(points_.begin(), points_.end(), beat,
                               [](double b, const ImportInfo::Tempo& t) { return b < t.beat; });
    const auto& p = it == points_.begin() ? points_.front() : *std::prev(it);
    return p.seconds + (beat - p.beat) * 60.0 / p.bpm;
  }
  const std::vector<ImportInfo::Tempo>& points() const { return points_; }

 private:
  std::vector<ImportInfo::Tempo> points_;
};

// ---------------------------------------------------------------------------
// MIDI

class MidiReader {
 public:
  explicit MidiReader(const std::vector<uint8_t>& d) : d_(d) {}
  bool eof() const { return p_ >= end_; }
  size_t pos() const { return p_; }
  void setRange(size_t start, size_t end) {
    p_ = start;
    end_ = std::min(end, d_.size());
  }
  uint8_t u8() {
    if (p_ >= end_) throw std::runtime_error("MIDI: unexpected end of data");
    return d_[p_++];
  }
  uint32_t be(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | u8();
    return v;
  }
  uint32_t vlq() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      uint8_t b = u8();
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) return v;
    }
    throw std::runtime_error("MIDI: bad variable-length number");
  }
  std::string bytes(size_t n) {
    if (p_ + n > end_) throw std::runtime_error("MIDI: truncated event");
    std::string s(d_.begin() + static_cast<long>(p_), d_.begin() + static_cast<long>(p_ + n));
    p_ += n;
    return s;
  }
  void skip(size_t n) { p_ = std::min(end_, p_ + n); }

 private:
  const std::vector<uint8_t>& d_;
  size_t p_ = 0, end_ = 0;
};

struct MidiNote {
  uint32_t tick, endTick;
  int pitch;
};

struct MidiTrack {
  std::vector<MidiNote> notes;
  std::vector<std::pair<uint32_t, std::string>> lyrics;
  std::string name;
};

// Text from a MIDI meta event: NUL bytes dropped (one would cut the JSON short)
// and, when it isn't UTF-8, read as Latin-1 as older files write it.
std::string midiText(const std::string& payload) {
  std::string s;
  for (char c : payload)
    if (c != '\0') s += c;
  if (validUtf8(s) == s) return s;
  std::string latin1;
  for (unsigned char c : s) utf8Append(latin1, c);
  return latin1;
}

struct MidiLyric {
  uint32_t tick = 0;
  std::string text;  // one note's token, "hel-" when the word goes on
};

// A track's lyric events as one syllable per tick of verse `verse` (1-based);
// `verses` receives how many verses there are, `ambiguous` the ticks with
// syllables for only some of them. Word boundaries come from whichever
// convention the file uses: "Hel-" "lo" (hyphens), "Hel" "lo " (MuseScore: a
// trailing space ends a word) or "Hel" "lo" " world" (karaoke files: a leading
// space, "/" or "\" starts one, a line break ends one).
//
// MuseScore writes the verses' syllables at the same tick, but only the verses
// that have one there: where another verse holds a melisma, a tick has fewer
// syllables than there are verses, and each goes to the verse it makes a word
// in ("Glo ri _ a" and "Sing to the Lord": "the" ends no word after "Glo ri"),
// by the dictionary when there is one, else to the lowest verse.
std::vector<MidiLyric> midiLyrics(const std::vector<std::pair<uint32_t, std::string>>& events, int verse,
                                  const Lexicon* lexicon, int* verses = nullptr, int* ambiguous = nullptr) {
  struct Raw {
    uint32_t tick;
    std::string text;
    bool startsWord = false, endsWord = false;  // a space, line mark or line break before / after it
  };
  const auto mark = [](char c) { return c == ' ' || c == '/' || c == '\\' || c == '\r' || c == '\n' || c == '\t'; };
  std::vector<Raw> raw;
  bool gap = false;     // a lone space or line mark between syllables
  bool spaced = false;  // the file marks words with spaces or line marks (a line break alone does not)
  for (const auto& [tick, payload] : events) {
    std::string s = midiText(payload);
    size_t a = 0, b = s.size();
    while (a < b && mark(s[a])) ++a;
    while (b > a && mark(s[b - 1])) --b;
    std::string text = s.substr(a, b - a);
    for (char& c : text)
      if (mark(c)) c = ' ';  // inside one event: two syllables on one note
    const std::string before = s.substr(0, a), after = s.substr(b);
    if (text.empty()) {
      if (!raw.empty()) raw.back().endsWord = true;
      gap = spaced = true;
      continue;
    }
    spaced = spaced || before.find_first_of(" /\\") != std::string::npos || after.find(' ') != std::string::npos;
    raw.push_back({tick, text, !before.empty() || gap, !after.empty()});
    gap = false;
  }
  const bool hyphens = std::any_of(raw.begin(), raw.end(), [](const Raw& r) { return r.text.back() == '-'; });
  // Whether a syllable ends its word, as far as it alone tells (karaoke files also end one where
  // the next syllable starts one), and its letters in the word.
  const auto ends = [&](const Raw& r) { return hyphens ? r.text.back() != '-' : !spaced || r.endsWord; };
  const auto stem = [&](const Raw& r) { return hyphens && r.text.back() == '-' ? r.text.substr(0, r.text.size() - 1) : r.text; };
  const auto known = [&](const std::string& word) {
    bool found = false;
    if (lexicon) lexicon->lookup(word, &found);
    return found;
  };

  // The ticks' syllables by verse: in order where every verse has one, else as they best make words.
  int most = 0;
  for (size_t i = 0; i < raw.size();) {
    size_t j = i;
    while (j < raw.size() && raw[j].tick == raw[i].tick) ++j;
    most = std::max(most, static_cast<int>(j - i));
    i = j;
  }
  const int count = std::max(1, most);
  std::vector<std::vector<const Raw*>> byVerse(static_cast<size_t>(count));
  std::vector<std::string> open(static_cast<size_t>(count));  // each verse's word so far ("" between words)
  int unsure = 0;
  for (size_t i = 0; i < raw.size();) {
    size_t j = i;
    while (j < raw.size() && raw[j].tick == raw[i].tick) ++j;
    const int k = static_cast<int>(j - i);
    std::vector<int> to(static_cast<size_t>(k));  // each syllable's verse (0-based): in order by default
    for (int e = 0; e < k; ++e) to[static_cast<size_t>(e)] = e;
    if (k < count) ++unsure;
    if (k < count && count <= 12) {
      double bestScore = -1e9;
      for (unsigned set = 0; set < (1u << count); ++set) {
        int size = 0;
        for (unsigned bits = set; bits; bits >>= 1) size += static_cast<int>(bits & 1);
        if (size != k) continue;
        double score = 0;
        size_t e = i;
        for (int v = 0; v < count; ++v) {
          const std::string& word = open[static_cast<size_t>(v)];
          if (!(set & (1u << v))) {
            score -= word.empty() ? 0 : 0.5;  // a melisma inside a word is the less likely one
            continue;
          }
          const Raw& r = raw[e++];
          const std::string before = r.startsWord ? "" : word;
          if (ends(r)) score += known(before + stem(r)) ? 2 : -1;
          else score += before.empty() ? 0 : 0.25;  // a word in progress goes on
        }
        if (score > bestScore + 1e-9) {
          bestScore = score;
          for (int v = 0, n = 0; v < count; ++v)
            if (set & (1u << v)) to[static_cast<size_t>(n++)] = v;
        }
      }
    }
    for (int e = 0; e < k; ++e) {
      const Raw& r = raw[i + static_cast<size_t>(e)];
      const auto v = static_cast<size_t>(to[static_cast<size_t>(e)]);
      byVerse[v].push_back(&r);
      open[v] = ends(r) ? "" : (r.startsWord ? "" : open[v]) + stem(r);
    }
    i = j;
  }
  if (verses) *verses = most;
  if (ambiguous) *ambiguous = unsure;

  const auto& chosen = byVerse[static_cast<size_t>(std::min(std::max(1, verse), count) - 1)];
  std::vector<MidiLyric> out;
  for (size_t k = 0; k < chosen.size(); ++k) {
    MidiLyric l;
    l.tick = chosen[k]->tick;
    l.text = cleanLyricText(chosen[k]->text);
    if (l.text.empty()) continue;
    const bool continues = !hyphens && !ends(*chosen[k]) && k + 1 < chosen.size() && !chosen[k + 1]->startsWord;
    if (continues && l.text.back() != '-') l.text += '-';
    out.push_back(l);
  }
  return out;
}

}  // namespace

Score importMidi(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings,
                 ImportInfo* info) {
  MidiReader r(data);
  r.setRange(0, data.size());
  if (r.bytes(4) != "MThd") throw std::runtime_error("not a MIDI file");
  uint32_t hlen = r.be(4);
  size_t afterHeader = r.pos() + hlen;
  r.be(2);  // format
  uint32_t ntracks = r.be(2);
  uint32_t division = r.be(2);
  if (division & 0x8000) throw std::runtime_error("SMPTE-timed MIDI files aren't supported");
  if (division == 0) throw std::runtime_error("MIDI: zero ticks per quarter note");
  r.setRange(afterHeader, data.size());

  std::map<uint32_t, double> tempo;               // tick -> microseconds per quarter
  std::map<uint32_t, std::pair<int, int>> meter;  // tick -> time signature
  std::vector<MidiTrack> tracks;
  for (uint32_t t = 0; t < ntracks && !r.eof(); ++t) {
    std::string id = r.bytes(4);
    uint32_t len = r.be(4);
    size_t start = r.pos();
    if (id != "MTrk") {
      r.setRange(start + len, data.size());
      continue;
    }
    r.setRange(start, start + len);
    MidiTrack track;
    uint32_t tick = 0;
    uint8_t status = 0;
    std::map<int, std::vector<uint32_t>> open;  // pitch -> start ticks (FIFO)
    while (!r.eof()) {
      tick += r.vlq();
      uint8_t b = r.u8();
      if (b == 0xFF) {
        uint8_t type = r.u8();
        uint32_t n = r.vlq();
        std::string payload = r.bytes(n);
        if (type == 0x51 && n == 3) {
          tempo[tick] = static_cast<double>((static_cast<uint8_t>(payload[0]) << 16) |
                                            (static_cast<uint8_t>(payload[1]) << 8) | static_cast<uint8_t>(payload[2]));
        } else if (type == 0x58 && n >= 2 && payload[0] > 0 && static_cast<uint8_t>(payload[1]) < 8) {
          meter[tick] = {static_cast<uint8_t>(payload[0]), 1 << static_cast<uint8_t>(payload[1])};
        } else if (type == 0x05) {
          track.lyrics.emplace_back(tick, payload);
        } else if (type == 0x03 && track.name.empty()) {
          track.name = midiText(payload);
        } else if (type == 0x2F) {
          break;
        }
        continue;
      }
      if (b == 0xF0 || b == 0xF7) {
        r.skip(r.vlq());
        continue;
      }
      uint8_t data1;
      if (b & 0x80) {
        status = b;
        data1 = r.u8();
      } else {
        if (!status) throw std::runtime_error("MIDI: running status without a status byte");
        data1 = b;
      }
      uint8_t kind = status & 0xF0;
      if (kind == 0xC0 || kind == 0xD0) continue;  // one data byte
      uint8_t data2 = r.u8();
      bool on = kind == 0x90 && data2 > 0;
      bool off = kind == 0x80 || (kind == 0x90 && data2 == 0);
      if (on) {
        open[data1].push_back(tick);
      } else if (off && !open[data1].empty()) {
        uint32_t s = open[data1].front();
        open[data1].erase(open[data1].begin());
        if (tick > s) track.notes.push_back({s, tick, data1});
      }
    }
    tracks.push_back(std::move(track));
    r.setRange(start + len, data.size());
  }

  // What each track holds (a chord counts once).
  const int verse = opts.verse > 0 ? opts.verse : 1;
  ImportInfo found;
  found.format = "midi";
  for (size_t i = 0; i < tracks.size(); ++i) {
    ImportInfo::Part part;
    part.index = static_cast<int>(i);
    part.name = tracks[i].name;
    std::set<uint32_t> starts;
    for (const auto& n : tracks[i].notes) starts.insert(n.tick);
    part.notes = static_cast<int>(starts.size());
    int verses = 0;
    part.lyrics = static_cast<int>(midiLyrics(tracks[i].lyrics, verse, opts.lexicon, &verses).size());
    for (int v = 1; v <= verses; ++v) part.verses.push_back(v);
    found.parts.push_back(part);
  }

  // Pick the track to sing.
  int chosen = -1;
  if (opts.track >= 0) {
    if (opts.track >= static_cast<int>(tracks.size()) || tracks[static_cast<size_t>(opts.track)].notes.empty())
      throw std::runtime_error("MIDI track " + std::to_string(opts.track) + " has no notes");
    chosen = opts.track;
  } else {
    for (size_t i = 0; i < tracks.size(); ++i) {  // prefer a track with lyrics
      if (!tracks[i].notes.empty() && !tracks[i].lyrics.empty()) {
        chosen = static_cast<int>(i);
        break;
      }
    }
    for (size_t i = 0; chosen < 0 && i < tracks.size(); ++i) {
      if (!tracks[i].notes.empty()) chosen = static_cast<int>(i);
    }
  }
  if (chosen < 0) throw std::runtime_error("MIDI file has no notes");
  int withNotes = 0;
  for (const auto& t : tracks) withNotes += t.notes.empty() ? 0 : 1;
  if (withNotes > 1 && opts.track < 0) {
    warnings.push_back("MIDI has " + std::to_string(withNotes) + " tracks with notes; using track " +
                       std::to_string(chosen) + " (choose with track)");
  }

  // Tick -> seconds through the tempo map.
  if (tempo.empty() || tempo.begin()->first != 0) tempo[0] = tempo.empty() ? 500000.0 : tempo.begin()->second;
  TempoMap map;
  for (const auto& [tk, us] : tempo) map.set(static_cast<double>(tk) / division, 60e6 / us);
  auto seconds = [&](uint32_t tk) { return map.seconds(static_cast<double>(tk) / division); };

  const MidiTrack& tr = tracks[static_cast<size_t>(chosen)];
  // Lyric events: use the chosen track's, else any track's (some files keep them apart).
  std::vector<std::pair<uint32_t, std::string>> events = tr.lyrics;
  if (events.empty()) {
    for (const auto& t : tracks) {
      if (!t.lyrics.empty()) {
        events = t.lyrics;
        break;
      }
    }
  }
  int verses = 0, unsure = 0;
  const std::vector<MidiLyric> lyrics = midiLyrics(events, verse, opts.lexicon, &verses, &unsure);
  if (verses > 1 && opts.verse <= 0)
    warnings.push_back("MIDI lyrics have " + std::to_string(verses) + " verses; sang verse 1 (choose with verse)");
  else if (verses > 0 && opts.verse > verses)
    warnings.push_back("the lyrics have no verse " + std::to_string(opts.verse) + "; sang verse " + std::to_string(verses));
  if (verses > 1 && unsure > 0)
    warnings.push_back(std::to_string(unsure) + " note(s) have lyrics for only some of the " + std::to_string(verses) +
                       " verses, matched to verses by their words; import the MusicXML for exact verses");

  Score score;
  score.tempo = 60e6 / tempo.begin()->second;
  std::vector<MidiNote> notes = tr.notes;
  std::sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.tick < b.tick; });
  const uint32_t tolerance = std::max<uint32_t>(1, division / 8);
  size_t li = 0;
  bool fileLyrics = false;
  for (const auto& mn : notes) {
    Note n;
    // Beat positions come straight from ticks, so a tempo map can't move notes.
    n.start = opts.beats ? static_cast<double>(mn.tick) / division : seconds(mn.tick);
    n.dur = (opts.beats ? static_cast<double>(mn.endTick) / division : seconds(mn.endTick)) - n.start;
    n.pitch = static_cast<float>(mn.pitch);
    // Attach the lyric event nearest this note's start.
    while (li < lyrics.size() && lyrics[li].tick + tolerance < mn.tick) ++li;
    if (li < lyrics.size() && lyrics[li].tick <= mn.tick + tolerance) {
      n.lyric = lyrics[li].text;
      fileLyrics = true;
      ++li;
    }
    score.notes.push_back(n);
  }
  // The file's own lyrics win; a lyric line given separately is for files without any.
  if (!fileLyrics) score.lyrics = opts.lyrics;
  else if (!opts.lyrics.empty()) warnings.push_back("sang the file's own lyrics (the lyrics given were not used)");
  makeMonophonic(score.notes, warnings);

  if (info) {
    *info = std::move(found);
    info->part = chosen;
    info->verse = verses > 0 ? std::min(verse, verses) : verse;
    info->fileLyrics = fileLyrics;
    info->tempos = map.points();
    for (const auto& [tk, m] : meter) {
      if (!info->meters.empty() && info->meters.back().beats == m.first && info->meters.back().beatType == m.second)
        continue;
      ImportInfo::Meter mt;
      mt.beat = static_cast<double>(tk) / division;
      mt.seconds = seconds(tk);
      mt.beats = m.first;
      mt.beatType = m.second;
      info->meters.push_back(mt);
    }
  }
  return score;
}

// ---------------------------------------------------------------------------
// Minimal XML DOM (enough for MusicXML)

namespace {

struct XmlNode {
  std::string name;
  std::map<std::string, std::string> attrs;
  std::string text;
  std::vector<std::unique_ptr<XmlNode>> children;

  const XmlNode* child(const std::string& n) const {
    for (const auto& c : children)
      if (c->name == n) return c.get();
    return nullptr;
  }
  std::string childText(const std::string& n, const std::string& fallback = "") const {
    const XmlNode* c = child(n);
    return c ? c->text : fallback;
  }
  bool has(const std::string& n) const { return attrs.count(n) > 0; }
  std::string attr(const std::string& n, const std::string& fallback = "") const {
    auto it = attrs.find(n);
    return it == attrs.end() ? fallback : it->second;
  }
};

class XmlParser {
 public:
  explicit XmlParser(const std::string& s) : s_(s) {}

  std::unique_ptr<XmlNode> parse() {
    skipProlog();
    auto root = element();
    if (!root) throw std::runtime_error("XML: no root element");
    return root;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  [[noreturn]] void fail(const std::string& m) {
    throw std::runtime_error("XML error at offset " + std::to_string(i_) + ": " + m);
  }
  bool at(const char* lit) const { return s_.compare(i_, std::char_traits<char>::length(lit), lit) == 0; }
  void skipUntil(const char* lit) {
    size_t p = s_.find(lit, i_);
    if (p == std::string::npos) fail(std::string("missing ") + lit);
    i_ = p + std::char_traits<char>::length(lit);
  }
  void ws() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
  }
  void skipProlog() {
    if (at("\xEF\xBB\xBF")) i_ += 3;
    while (true) {
      ws();
      if (at("<?")) skipUntil("?>");
      else if (at("<!--")) skipUntil("-->");
      else if (at("<!DOCTYPE")) skipDoctype();
      else break;
    }
  }
  void skipDoctype() {
    int depth = 0;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '[') ++depth;
      else if (c == ']') --depth;
      else if (c == '>' && depth <= 0) return;
    }
  }
  std::string name() {
    size_t a = i_;
    while (i_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i_])) && s_[i_] != '>' && s_[i_] != '/' &&
           s_[i_] != '=')
      ++i_;
    return s_.substr(a, i_ - a);
  }
  // Character reference "#233" / "#xE9" -> its UTF-8; false when malformed.
  static bool charRef(const std::string& ent, std::string& out) {
    const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
    const std::string digits = ent.substr(hex ? 2 : 1);
    if (digits.empty() || digits.size() > 8) return false;
    uint32_t cp = 0;
    for (char c : digits) {
      int d = -1;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      if (d < 0) return false;
      cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
    }
    if (cp == 0) return false;
    utf8Append(out, cp);  // beyond U+10FFFF or a surrogate: U+FFFD
    return true;
  }
  static std::string decode(const std::string& t) {
    std::string out;
    for (size_t k = 0; k < t.size(); ++k) {
      if (t[k] != '&') {
        out += t[k];
        continue;
      }
      size_t semi = t.find(';', k);
      if (semi == std::string::npos) {
        out += t[k];
        continue;
      }
      std::string ent = t.substr(k + 1, semi - k - 1);
      if (ent == "amp") out += '&';
      else if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else if (ent.empty() || ent[0] != '#' || !charRef(ent, out)) out += "&" + ent + ";";  // left as written
      k = semi;
    }
    return out;
  }
  std::unique_ptr<XmlNode> element() {
    ws();
    if (!at("<")) return nullptr;
    ++i_;
    auto node = std::make_unique<XmlNode>();
    node->name = name();
    while (true) {
      ws();
      if (at("/>")) {
        i_ += 2;
        return node;
      }
      if (at(">")) {
        ++i_;
        break;
      }
      std::string key = name();
      ws();
      if (!at("=")) fail("expected = in attribute");
      ++i_;
      ws();
      if (i_ >= s_.size()) fail("unterminated attribute");
      char q = s_[i_];
      if (q != '"' && q != '\'') fail("expected quoted attribute value");
      size_t end = s_.find(q, i_ + 1);
      if (end == std::string::npos) fail("unterminated attribute");
      node->attrs[key] = decode(s_.substr(i_ + 1, end - i_ - 1));
      i_ = end + 1;
    }
    // Content
    while (i_ < s_.size()) {
      if (at("</")) {
        skipUntil(">");
        node->text = trimCopy(decode(node->text));
        return node;
      }
      if (at("<!--")) {
        skipUntil("-->");
        continue;
      }
      if (at("<![CDATA[")) {
        size_t a = i_ + 9;
        skipUntil("]]>");
        node->text += s_.substr(a, i_ - 3 - a);
        continue;
      }
      if (at("<?")) {
        skipUntil("?>");
        continue;
      }
      if (at("<")) {
        node->children.push_back(element());
        continue;
      }
      size_t a = i_;
      while (i_ < s_.size() && s_[i_] != '<') ++i_;
      node->text += s_.substr(a, i_ - a);
    }
    fail("unterminated element <" + node->name + ">");
  }
};

int stepSemis(char step) {
  switch (step) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
// MusicXML: each part is read into measures (positions in quarter notes), the
// repeat structure is played out into a measure order, and the chosen voice's
// notes are laid along it.

struct XmlLyric {
  std::string text;         // the syllable; "a~and" for an elision; "" for an extender alone
  bool continues = false;   // syllabic begin/middle: the word goes on at the next syllable
  bool extend = false;      // an extender (melisma line) starts or continues here
  bool extendStop = false;  // ... or ends here
};

// A note or rest of one voice; a chord is already reduced to its top note.
struct XmlNote {
  double start = 0, dur = 0;  // quarter notes from the measure's start
  std::string voice;
  bool rest = false;
  float pitch = 0;            // sounding pitch (MIDI)
  bool tieStart = false, tieStop = false, slurStart = false, slurStop = false;
  std::map<int, XmlLyric> lyrics;  // by verse
  int dropped = 0;            // chord notes below the top one
};

// A jump or its target (<sound segno/dalsegno/coda/tocoda="name">).
struct XmlMark {
  bool on = false;
  std::string name;
};

struct XmlTempo {
  double pos = 0, bpm = 120;
  bool fromSound = false;  // <sound tempo>, which wins over a <metronome> mark at the same place
};

struct XmlMeasure {
  bool implicit = false;
  double length = 0;  // the furthest position any voice reached
  std::vector<XmlNote> notes;
  std::map<std::string, int> graces;  // grace notes per voice (skipped: they take no time)
  std::vector<XmlTempo> tempos;
  int timeBeats = 0, timeBeatType = 0;  // a time signature set here
  bool repeatForward = false, repeatBackward = false;
  int times = 0;  // how often a backward repeat's section is played (0 = not given)
  bool endingStart = false, endingStop = false;
  std::vector<int> endingNumbers;
  bool dacapo = false, fine = false;
  XmlMark segno, coda, dalsegno, tocoda;
};

// Length of a note value in quarter notes ("eighth" -> 0.5).
double noteValue(const std::string& type) {
  static const std::map<std::string, double> values = {
      {"maxima", 32},   {"long", 16},       {"breve", 8},          {"whole", 4},          {"half", 2},
      {"quarter", 1},   {"eighth", 0.5},    {"16th", 0.25},        {"32nd", 0.125},       {"64th", 1.0 / 16},
      {"128th", 1.0 / 32}, {"256th", 1.0 / 64}, {"512th", 1.0 / 128}, {"1024th", 1.0 / 256}};
  auto it = values.find(type);
  return it == values.end() ? 0 : it->second;
}

std::vector<int> numbersIn(const std::string& text) {  // "1, 2" -> {1, 2}
  std::vector<int> out;
  for (size_t i = 0; i < text.size();) {
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    int v = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && v < 100000) v = v * 10 + (text[i++] - '0');
    out.push_back(v);
  }
  return out;
}

void readSound(const XmlNode& snd, double pos, XmlMeasure& m) {
  if (snd.has("tempo")) {
    const double bpm = parseNumber(snd.attr("tempo"), 0);
    if (bpm > 0) m.tempos.push_back({pos, bpm, true});
  }
  // Jumps act where the measure ends, their targets where it starts.
  if (snd.attr("dacapo") == "yes") m.dacapo = true;
  if (snd.has("fine")) m.fine = true;
  if (snd.attr("forward-repeat") == "yes") m.repeatForward = true;
  for (auto [key, mark] : {std::pair<const char*, XmlMark*>{"segno", &m.segno}, {"coda", &m.coda},
                           {"dalsegno", &m.dalsegno}, {"tocoda", &m.tocoda}}) {
    if (snd.has(key)) *mark = {true, snd.attr(key)};
  }
}

// <metronome><beat-unit>quarter</beat-unit><beat-unit-dot/><per-minute>60</per-minute>:
// a tempo in quarter notes per minute (90 here). Metric modulations (note = note) carry none.
void readMetronome(const XmlNode& metro, double pos, XmlMeasure& m) {
  double unit = 0, perMinute = 0;
  int dots = 0, units = 0;
  for (const auto& c : metro.children) {
    if (c->name == "beat-unit") {
      if (++units == 1) unit = noteValue(c->text);
    } else if (c->name == "beat-unit-dot" && units == 1) {
      ++dots;
    } else if (c->name == "per-minute") {
      perMinute = parseNumber(c->text, 0);
    }
  }
  if (units == 1 && unit > 0 && perMinute > 0) m.tempos.push_back({pos, perMinute * unit * (2 - std::pow(0.5, dots)), false});
}

// <time><beats>3+2</beats><beat-type>8</beat-type></time>; composite signatures
// (several pairs) are added up in the first pair's beat type.
void readTime(const XmlNode& time, XmlMeasure& m) {
  if (time.child("senza-misura")) return;
  double quarters = 0, beats = 0;
  int beatType = 0;
  for (const auto& c : time.children) {
    if (c->name == "beats") {
      beats = 0;
      std::stringstream parts(c->text);
      for (std::string part; std::getline(parts, part, '+');) beats += parseNumber(part, 0);
    } else if (c->name == "beat-type") {
      const int type = static_cast<int>(parseNumber(c->text, 0));
      if (type <= 0 || beats <= 0) continue;
      if (beatType == 0) beatType = type;
      quarters += beats * 4.0 / type;
    }
  }
  const int total = static_cast<int>(std::lround(quarters * beatType / 4.0));
  if (beatType > 0 && total > 0) {
    m.timeBeats = total;
    m.timeBeatType = beatType;
  }
}

void readBarline(const XmlNode& bar, XmlMeasure& m) {
  if (const XmlNode* rep = bar.child("repeat")) {
    if (rep->attr("direction") == "forward") {
      m.repeatForward = true;
    } else if (rep->attr("direction") == "backward") {
      m.repeatBackward = true;
      m.times = std::max(m.times, static_cast<int>(std::min(100.0, parseNumber(rep->attr("times"), 0))));
    }
  }
  if (const XmlNode* end = bar.child("ending")) {
    const std::string type = end->attr("type");
    if (type == "start") {
      m.endingStart = true;
      m.endingNumbers = numbersIn(end->attr("number"));
    } else if (type == "stop" || type == "discontinue") {
      m.endingStop = true;
    }
  }
}

// One <lyric> as a syllable: its texts joined (an <elision> or an undertie
// between them becomes "~"), "-" added for syllabic begin/middle by the caller.
bool readLyric(const XmlNode& lyric, XmlLyric& out) {
  std::string raw, syllabic = "single";
  for (const auto& c : lyric.children) {
    if (c->name == "syllabic") syllabic = c->text;
    else if (c->name == "elision") raw += ' ';
    else if (c->name == "text") raw += c->text;
    else if (c->name == "extend") (c->attr("type") == "stop" ? out.extendStop : out.extend) = true;
  }
  out.text = cleanLyricText(raw);
  out.continues = syllabic == "begin" || syllabic == "middle";
  return !out.text.empty() || out.extend || out.extendStop;
}

// A note's lyrics by verse (<lyric number="2">; verse 1 when unnumbered). The
// first lyric of a verse wins.
void readLyrics(const XmlNode& note, XmlNote& n) {
  for (const auto& c : note.children) {
    if (c->name != "lyric") continue;
    const int verse = std::max(1, static_cast<int>(parseNumber(c->attr("number"), 1)));
    XmlLyric l;
    if (!n.lyrics.count(verse) && readLyric(*c, l)) n.lyrics[verse] = l;
  }
}

void readNotations(const XmlNode& note, XmlNote& n) {
  for (const auto& c : note.children) {
    if (c->name == "tie") {
      if (c->attr("type") == "stop") n.tieStop = true;
      if (c->attr("type") == "start") n.tieStart = true;
    }
  }
  if (const XmlNode* nots = note.child("notations")) {
    for (const auto& c : nots->children) {
      if (c->name == "slur") {
        if (c->attr("type") == "start") n.slurStart = true;
        if (c->attr("type") == "stop") n.slurStop = true;
      } else if (c->name == "tied") {
        if (c->attr("type") == "stop") n.tieStop = true;
        if (c->attr("type") == "start") n.tieStart = true;
      }
    }
  }
}

float pitchOf(const XmlNode& pitch, int transpose) {
  const std::string step = pitch.childText("step", "C");
  const double octave = std::round(parseNumber(pitch.childText("octave"), 4));
  return static_cast<float>((octave + 1) * 12 + stepSemis(step.empty() ? 'C' : step[0]) +
                            parseNumber(pitch.childText("alter"), 0) + transpose);
}

std::vector<XmlMeasure> readPart(const XmlNode& part) {
  std::vector<XmlMeasure> measures;
  double divisions = 1;
  int transpose = 0;  // semitones from written to sounding pitch
  for (const auto& me : part.children) {
    if (me->name != "measure") continue;
    XmlMeasure m;
    m.implicit = me->attr("implicit") == "yes";
    double pos = 0, furthest = 0;
    long lead = -1;     // the chord being read: index of its first note in m.notes
    bool unsung = false;  // ... or it was a grace or cue chord
    auto quarters = [&](const XmlNode& e) { return std::max(0.0, parseNumber(e.childText("duration"), 0)) / divisions; };
    for (const auto& el : me->children) {
      const XmlNode& e = *el;
      if (e.name == "attributes") {
        const double d = parseNumber(e.childText("divisions"), 0);
        if (d > 0) divisions = d;
        if (const XmlNode* t = e.child("transpose")) {
          transpose = static_cast<int>(std::lround(parseNumber(t->childText("chromatic"), 0)) +
                                       12 * std::lround(parseNumber(t->childText("octave-change"), 0)));
        }
        if (const XmlNode* time = e.child("time")) readTime(*time, m);
      } else if (e.name == "backup") {
        pos = std::max(0.0, pos - quarters(e));
      } else if (e.name == "forward") {
        pos += quarters(e);
        furthest = std::max(furthest, pos);
      } else if (e.name == "direction") {
        const double at = std::max(0.0, pos + parseNumber(e.childText("offset"), 0) / divisions);
        if (const XmlNode* snd = e.child("sound")) readSound(*snd, at, m);
        for (const auto& dt : e.children) {
          if (dt->name != "direction-type") continue;
          if (const XmlNode* metro = dt->child("metronome")) readMetronome(*metro, at, m);
        }
      } else if (e.name == "sound") {
        readSound(e, pos, m);
      } else if (e.name == "barline") {
        readBarline(e, m);
      } else if (e.name == "note") {
        const bool chord = e.child("chord") != nullptr;
        if (e.child("grace")) {  // no time of its own
          if (!chord) ++m.graces[e.childText("voice", "1")];
          if (!chord) unsung = true;
          continue;
        }
        const double d = quarters(e);
        if (chord) {
          // Chords keep their top note (MuseScore writes them lowest first).
          if (unsung || lead < 0 || !e.child("pitch")) continue;
          XmlNote& top = m.notes[static_cast<size_t>(lead)];
          if (top.rest) continue;
          XmlNote extra;
          readNotations(e, extra);
          const float pitch = pitchOf(*e.child("pitch"), transpose);
          ++top.dropped;
          if (pitch > top.pitch) {
            top.pitch = pitch;
            top.tieStart = extra.tieStart;
            top.tieStop = extra.tieStop;
          }
          top.slurStart = top.slurStart || extra.slurStart;
          top.slurStop = top.slurStop || extra.slurStop;
          readLyrics(e, top);  // the lyric may sit on any note of the chord
          continue;
        }
        if (e.child("cue")) {  // takes its time in the voice, but nobody sings it
          pos += d;
          furthest = std::max(furthest, pos);
          unsung = true;
          continue;
        }
        unsung = false;
        XmlNote n;
        n.start = pos;
        n.dur = d;
        n.voice = e.childText("voice", "1");
        if (n.voice.empty()) n.voice = "1";
        const XmlNode* pitch = e.child("pitch");
        n.rest = e.child("rest") || !pitch;  // unpitched notes aren't sung either
        if (!n.rest) {
          n.pitch = pitchOf(*pitch, transpose);
          readNotations(e, n);
          readLyrics(e, n);
        }
        m.notes.push_back(n);
        lead = static_cast<long>(m.notes.size()) - 1;
        pos += d;
        furthest = std::max(furthest, pos);
      }
    }
    m.length = furthest;
    measures.push_back(std::move(m));
  }
  return measures;
}

// The repeat structure, tempos and time signatures of each measure, gathered
// from every part: notation apps write jumps and tempo marks in the top part
// only, which is often not the one sung. Lengths come from the longest voice.
std::vector<XmlMeasure> scoreStructure(const std::vector<std::vector<XmlMeasure>>& parts, size_t sung) {
  std::vector<XmlMeasure> bars(parts[sung].size());
  std::vector<size_t> order = {sung};
  for (size_t p = 0; p < parts.size(); ++p)
    if (p != sung) order.push_back(p);
  double nominal = 0;  // the time signature's measure length
  for (size_t i = 0; i < bars.size(); ++i) {
    XmlMeasure& b = bars[i];
    for (size_t p : order) {
      if (i >= parts[p].size()) continue;
      const XmlMeasure& m = parts[p][i];
      if (p == sung) b.implicit = m.implicit;
      b.length = std::max(b.length, m.length);
      if (!b.timeBeats && m.timeBeats) {
        b.timeBeats = m.timeBeats;
        b.timeBeatType = m.timeBeatType;
      }
      b.tempos.insert(b.tempos.end(), m.tempos.begin(), m.tempos.end());
      b.repeatForward = b.repeatForward || m.repeatForward;
      b.repeatBackward = b.repeatBackward || m.repeatBackward;
      b.times = std::max(b.times, m.times);
      if (m.endingStart && !b.endingStart) {
        b.endingStart = true;
        b.endingNumbers = m.endingNumbers;
      }
      b.endingStop = b.endingStop || m.endingStop;
      b.dacapo = b.dacapo || m.dacapo;
      b.fine = b.fine || m.fine;
      for (auto [mine, theirs] : {std::pair<XmlMark*, const XmlMark*>{&b.segno, &m.segno}, {&b.coda, &m.coda},
                                  {&b.dalsegno, &m.dalsegno}, {&b.tocoda, &m.tocoda}}) {
        if (!mine->on && theirs->on) *mine = *theirs;
      }
    }
    if (b.timeBeats) nominal = b.timeBeats * 4.0 / b.timeBeatType;
    if (b.length <= 0) b.length = nominal;  // an empty measure still takes its time
    // One tempo per position: <sound tempo> over a <metronome> mark, the sung part's first.
    std::stable_sort(b.tempos.begin(), b.tempos.end(), [](const XmlTempo& x, const XmlTempo& y) { return x.pos < y.pos; });
    std::vector<XmlTempo> merged;
    for (const XmlTempo& t : b.tempos) {
      if (!merged.empty() && std::fabs(merged.back().pos - t.pos) < 1e-6) {
        if (!merged.back().fromSound && t.fromSound) merged.back() = t;
        continue;
      }
      merged.push_back(t);
    }
    b.tempos.swap(merged);
  }
  return bars;
}

// The measure a jump goes to: the mark with the jump's name (exporters spell
// them "segno", "1", "coda"...), else any such mark; a segno is looked for
// before the jump first, a coda after it.
size_t findMark(const std::vector<XmlMeasure>& bars, XmlMark XmlMeasure::*field, const std::string& name, size_t from,
                bool after) {
  const size_t n = bars.size();
  for (bool byName : {true, false}) {
    auto match = [&](size_t j) { return (bars[j].*field).on && (!byName || (bars[j].*field).name == name); };
    if (after) {
      for (size_t j = from + 1; j < n; ++j)
        if (match(j)) return j;
      for (size_t j = 0; j <= from && j < n; ++j)
        if (match(j)) return j;
    } else {
      for (size_t j = std::min(from + 1, n); j-- > 0;)
        if (match(j)) return j;
      for (size_t j = from + 1; j < n; ++j)
        if (match(j)) return j;
    }
  }
  return n;
}

struct XmlPlay {
  size_t bar;
  int pass;  // which time through this music: verse N is sung on pass N
};

// Plays out repeats (with times), endings, D.C., D.S., To Coda and Fine into
// the order the measures sound in, as MuseScore plays them back (repeatlist.cpp):
// an end repeat goes back to the latest start repeat (or the start), and each
// end repeat after a start adds its passes to that start's count, so
// |: A :| B :| plays A A B A B and |: A |: B :| C :| D plays A B B C B C D.
// Repeats are not taken again after a D.C./D.S., which plays the last ending.
std::vector<XmlPlay> playOrder(const std::vector<XmlMeasure>& bars, bool unfold, std::vector<std::string>& warnings) {
  const size_t n = bars.size();
  std::vector<XmlPlay> order;
  if (!unfold) {
    for (size_t i = 0; i < n; ++i) order.push_back({i, 1});
    return order;
  }

  // Endings run from their start to a stop (or the backward repeat closing
  // them); endings that follow each other form one set ([1. ... :| [2. ...).
  struct Ending {
    size_t first, last, lastInSet = 0;
    std::vector<int> numbers;
    int setMax = 0;  // the highest pass number in the set
  };
  std::vector<Ending> endings;
  std::vector<long> endingOf(n, -1);
  long open = -1;
  for (size_t i = 0; i < n; ++i) {
    if (bars[i].endingStart) {
      endings.push_back({i, i, 0, bars[i].endingNumbers, 0});
      open = static_cast<long>(endings.size()) - 1;
    }
    if (open >= 0) {
      endingOf[i] = open;
      endings[static_cast<size_t>(open)].last = i;
    }
    if (bars[i].endingStop || bars[i].repeatBackward) open = -1;
  }
  for (size_t g = 0; g < endings.size();) {
    size_t h = g;
    while (h + 1 < endings.size() && endings[h + 1].first == endings[h].last + 1) ++h;
    int most = 0;
    for (size_t k = g; k <= h; ++k)
      for (int v : endings[k].numbers) most = std::max(most, v);
    for (size_t k = g; k <= h; ++k) {
      endings[k].lastInSet = h;
      endings[k].setMax = most;
    }
    g = h + 1;
  }

  // How often an end repeat sends the music back (its times, else as many as its endings, else
  // twice), the start each measure belongs to, and how many passes each start's music has.
  const auto timesAt = [&](size_t i) {
    const long e = endingOf[i];
    return bars[i].times > 0 ? bars[i].times : e >= 0 ? std::max(2, endings[static_cast<size_t>(e)].setMax) : 2;
  };
  std::vector<size_t> startOf(n, 0);
  std::vector<int> passes(n, 1);
  for (size_t i = 0, s = 0; i < n; ++i) {
    if (bars[i].repeatForward) s = i;
    startOf[i] = s;
    if (bars[i].repeatBackward) passes[s] += timesAt(i) - 1;
  }

  const size_t kMostBars = 20000;
  std::vector<int> played(n, 0), backs(n, 0);  // backs: times each end repeat was reached
  std::vector<bool> jumped(n, false), toCoda(n, false);
  size_t i = 0, repeatStart = 0;
  int pass = 1;
  bool returning = false, afterJump = false;
  while (i < n) {
    if (order.size() >= kMostBars) {
      warnings.push_back("repeats stopped after " + std::to_string(kMostBars) + " measures");
      break;
    }
    const XmlMeasure& b = bars[i];
    if (b.repeatForward && !returning) {
      repeatStart = i;
      pass = afterJump ? passes[i] : 1;  // after a jump, its last pass
    }
    returning = false;
    const long e = endingOf[i];
    const Ending* ending = e >= 0 ? &endings[static_cast<size_t>(e)] : nullptr;
    if (ending && ending->first == i) {
      const bool lastOfSet = ending->lastInSet == static_cast<size_t>(e);
      const bool listed = std::find(ending->numbers.begin(), ending->numbers.end(), pass) != ending->numbers.end();
      const bool take = afterJump ? lastOfSet : listed || (lastOfSet && pass > ending->setMax);
      if (!take) {
        i = ending->last + 1;
        continue;
      }
    }
    ++played[i];
    int verse = played[i];
    if (ending) {
      verse = afterJump && !ending->numbers.empty()
                  ? *std::max_element(ending->numbers.begin(), ending->numbers.end())
                  : pass;
    }
    order.push_back({i, verse});

    if (afterJump && b.fine) break;
    if (afterJump && b.tocoda.on && !toCoda[i]) {
      toCoda[i] = true;
      const size_t coda = findMark(bars, &XmlMeasure::coda, b.tocoda.name, i, true);
      if (coda < n) {
        i = coda;
        continue;
      }
      warnings.push_back("To Coda without a coda; played on");
    }
    if (b.repeatBackward && !afterJump && ++backs[i] < timesAt(i) && pass < passes[repeatStart]) {
      ++pass;
      i = repeatStart;
      returning = true;
      continue;
    }
    if ((b.dacapo || b.dalsegno.on) && !jumped[i]) {
      jumped[i] = true;
      size_t target = 0;
      if (!b.dacapo) target = findMark(bars, &XmlMeasure::segno, b.dalsegno.name, i, false);
      if (target < n) {
        afterJump = true;
        repeatStart = startOf[target];
        pass = passes[repeatStart];
        i = target;
        returning = true;
        continue;
      }
      warnings.push_back("D.S. without a segno; played on");
    }
    ++i;
  }
  return order;
}

// The verse to sing in a measure on pass `want`: that verse where the measure
// has it, or where its melisma line comes into the measure (`held`: MuseScore
// writes an extender on its first note only); where it has only others (a
// refrain printed once, an ending with its own verse), the nearest verse
// below, else the lowest.
int measureVerse(const XmlMeasure& m, const std::string& voice, int want, bool held) {
  std::set<int> have;
  for (const XmlNote& n : m.notes)
    if (n.voice == voice)
      for (const auto& l : n.lyrics) have.insert(l.first);
  if (have.empty() || have.count(want) || held) return want;
  auto it = have.lower_bound(want);
  return it != have.begin() ? *std::prev(it) : *have.begin();
}

std::string joinNumbers(const std::set<int>& v) {
  std::string s;
  for (int x : v) s += (s.empty() ? "" : ", ") + std::to_string(x);
  return s;
}

}  // namespace

Score importMusicXml(const std::string& xml, const ImportOptions& opts, std::vector<std::string>& warnings,
                     ImportInfo* info) {
  auto root = XmlParser(xml).parse();
  if (root->name == "score-timewise") throw std::runtime_error("timewise MusicXML isn't supported (use partwise)");
  if (root->name != "score-partwise") throw std::runtime_error("not a MusicXML score");

  std::map<std::string, std::string> partNames;
  if (const XmlNode* list = root->child("part-list"))
    for (const auto& sp : list->children)
      if (sp->name == "score-part") partNames[sp->attr("id")] = sp->childText("part-name");
  std::vector<const XmlNode*> partNodes;
  for (const auto& c : root->children)
    if (c->name == "part") partNodes.push_back(c.get());
  if (partNodes.empty()) throw std::runtime_error("MusicXML has no parts");
  std::vector<std::vector<XmlMeasure>> parts;
  for (const XmlNode* p : partNodes) parts.push_back(readPart(*p));

  // What each part holds, voice by voice (in the order they first sing).
  ImportInfo found;
  found.format = "musicxml";
  std::vector<std::set<int>> partVerses(parts.size());
  for (size_t p = 0; p < parts.size(); ++p) {
    ImportInfo::Part part;
    part.index = static_cast<int>(p);
    part.id = partNodes[p]->attr("id");
    part.name = partNames[part.id];
    for (const XmlMeasure& m : parts[p]) {
      for (const XmlNote& n : m.notes) {
        if (n.rest) continue;
        auto v = std::find_if(part.voices.begin(), part.voices.end(), [&](const ImportInfo::Voice& x) { return x.id == n.voice; });
        if (v == part.voices.end()) {
          part.voices.push_back({n.voice, 0, 0});
          v = std::prev(part.voices.end());
        }
        const bool sung = std::any_of(n.lyrics.begin(), n.lyrics.end(), [](const auto& l) { return !l.second.text.empty(); });
        ++v->notes;
        v->lyrics += sung ? 1 : 0;
        ++part.notes;
        part.lyrics += sung ? 1 : 0;
        for (const auto& [verse, l] : n.lyrics)
          if (!l.text.empty()) partVerses[p].insert(verse);
      }
    }
    part.verses.assign(partVerses[p].begin(), partVerses[p].end());
    found.parts.push_back(part);
  }

  // The part: as asked, else the first with lyrics, else the first with notes.
  int chosen = -1;
  if (opts.track >= 0) {
    if (opts.track >= static_cast<int>(parts.size()))
      throw std::runtime_error("MusicXML part " + std::to_string(opts.track) + " not found");
    chosen = opts.track;
  } else {
    for (const auto& p : found.parts)
      if (chosen < 0 && p.lyrics > 0) chosen = p.index;
    for (const auto& p : found.parts)
      if (chosen < 0 && p.notes > 0) chosen = p.index;
    if (chosen < 0) throw std::runtime_error("MusicXML has no notes");
  }
  const ImportInfo::Part& part = found.parts[static_cast<size_t>(chosen)];
  if (part.notes == 0) throw std::runtime_error("MusicXML part " + std::to_string(chosen) + " has no notes");
  if (parts.size() > 1 && opts.track < 0) {
    warnings.push_back("MusicXML has " + std::to_string(parts.size()) + " parts; using part " + std::to_string(chosen) +
                       (part.name.empty() ? "" : " (" + part.name + ")") + " (choose with track)");
  }

  // The voice: as asked, else the first to sing with lyrics (a soprano's melismas leave it fewer
  // syllables than the alto's, but not half as many), else the first to sing.
  std::string voice = opts.partVoice;
  if (!voice.empty()) {
    if (std::none_of(part.voices.begin(), part.voices.end(), [&](const ImportInfo::Voice& v) { return v.id == voice; }))
      throw std::runtime_error("MusicXML part " + std::to_string(chosen) + " has no voice " + voice);
  } else {
    int most = 0;
    for (const auto& v : part.voices) most = std::max(most, v.lyrics);
    for (const auto& v : part.voices) {
      if (most > 0 && v.lyrics * 2 >= most) {
        voice = v.id;
        break;
      }
    }
    if (voice.empty()) voice = part.voices.front().id;
  }
  int otherVoices = 0;
  for (const auto& v : part.voices)
    if (v.id != voice) otherVoices += v.notes;

  const std::vector<XmlMeasure>& own = parts[static_cast<size_t>(chosen)];
  const std::vector<XmlMeasure> bars = scoreStructure(parts, static_cast<size_t>(chosen));
  const bool hasRepeats = std::any_of(bars.begin(), bars.end(), [](const XmlMeasure& b) {
    return b.repeatForward || b.repeatBackward || b.endingStart || b.dacapo || b.dalsegno.on || b.tocoda.on;
  });
  const std::vector<XmlPlay> order = playOrder(bars, opts.unfold, warnings);

  // Tempo and time signature in force where each measure starts, in written
  // order: a repeat goes back to the tempo written there.
  std::vector<double> tempoAt(bars.size());
  std::vector<std::pair<int, int>> meterAt(bars.size());
  {
    double bpm = 120;
    std::pair<int, int> meter = {0, 0};
    for (size_t i = 0; i < bars.size(); ++i) {
      tempoAt[i] = bpm;
      for (const XmlTempo& t : bars[i].tempos) bpm = t.bpm;
      if (bars[i].timeBeats) meter = {bars[i].timeBeats, bars[i].timeBeatType};
      meterAt[i] = meter;
    }
  }

  // Lay the voice's notes along the measures in play order, in beats.
  Score score;
  TempoMap tempo;
  std::vector<ImportInfo::Meter> meters;
  std::set<int> versesSung;
  double barStart = 0;
  bool lastHadExtend = false, inSlur = false, sung = false;
  // By verse: its melisma line goes on, in play order whichever verse is sung. Not into music that
  // doesn't follow it (back to a repeat's start, a jump, or one ending after another as written),
  // nor into an ending for other passes than that verse's.
  std::map<int, bool> extending;
  std::vector<const std::vector<int>*> endingAt(bars.size(), nullptr);  // the numbers of the ending a measure is in
  for (size_t i = 0; i < bars.size(); ++i) {
    endingAt[i] = bars[i].endingStart ? &bars[i].endingNumbers : i > 0 ? endingAt[i - 1] : nullptr;
    if (i > 0 && !bars[i].endingStart && (bars[i - 1].endingStop || bars[i - 1].repeatBackward)) endingAt[i] = nullptr;
  }
  size_t lastBar = 0;
  int chordNotes = 0, graces = 0;  // in the voice as written
  for (const XmlMeasure& m : own) {
    auto g = m.graces.find(voice);
    graces += g == m.graces.end() ? 0 : g->second;
    for (const XmlNote& n : m.notes) chordNotes += n.voice == voice ? n.dropped : 0;
  }
  for (const XmlPlay& play : order) {
    const XmlMeasure& bar = bars[play.bar];
    tempo.set(barStart, tempoAt[play.bar]);
    for (const XmlTempo& t : bar.tempos) tempo.set(barStart + t.pos, t.bpm);
    const auto& meter = meterAt[play.bar];
    if (meter.first > 0 && (meters.empty() || meters.back().beats != meter.first || meters.back().beatType != meter.second)) {
      ImportInfo::Meter mt;
      mt.beat = barStart;
      mt.beats = meter.first;
      mt.beatType = meter.second;
      meters.push_back(mt);
    }
    const XmlMeasure& m = own[play.bar];
    if (&play != &order.front() &&
        (play.bar != lastBar + 1 || (bars[play.bar].endingStart && endingAt[lastBar])))
      extending.clear();
    lastBar = play.bar;
    const int want = opts.verse > 0 ? opts.verse : play.pass;
    const auto* ending = endingAt[play.bar];
    const bool otherPasses = ending && !ending->empty() && std::find(ending->begin(), ending->end(), want) == ending->end();
    const int verse = measureVerse(m, voice, want, extending[want] && !otherPasses);
    for (const XmlNote& xn : m.notes) {
      if (xn.voice != voice) continue;
      if (xn.rest) extending.clear();
      for (const auto& [v, l] : xn.lyrics) extending[v] = (l.extend || (extending[v] && l.text.empty())) && !l.extendStop;
    }
    for (const XmlNote& xn : m.notes) {
      if (xn.voice != voice) continue;
      const double start = barStart + xn.start;
      if (xn.rest) {
        lastHadExtend = false;
        continue;
      }
      auto it = xn.lyrics.find(verse);
      const XmlLyric* lyric = it == xn.lyrics.end() ? nullptr : &it->second;
      const bool text = lyric && !lyric->text.empty();
      // Tied continuation: lengthen the previous note, unless this verse starts a syllable here.
      if (xn.tieStop && !text && !score.notes.empty()) {
        Note& prev = score.notes.back();
        if (std::fabs(prev.pitch - xn.pitch) < 0.01f && std::fabs(prev.start + prev.dur - start) < 1e-6) {
          prev.dur = start + xn.dur - prev.start;
          continue;
        }
      }
      Note n;
      n.start = start;
      n.dur = xn.dur;
      n.pitch = xn.pitch;
      if (text) {
        n.lyric = lyric->text;
        if (lyric->continues && n.lyric.back() != '-') n.lyric += '-';
        lastHadExtend = lyric->extend;
        sung = true;
        versesSung.insert(verse);
      } else if (lastHadExtend || inSlur || (lyric && (lyric->extend || lyric->extendStop)) ||
                 (sung && !score.notes.empty() &&
                  std::fabs(score.notes.back().start + score.notes.back().dur - start) < 1e-6)) {
        // Melisma: an untexted note straight after a sung one continues its vowel.
        n.lyric = "_";
        if (lyric && lyric->extendStop) lastHadExtend = false;
      }
      if (xn.slurStart) inSlur = true;
      if (xn.slurStop) inSlur = false;
      score.notes.push_back(n);
    }
    barStart += bar.length;
  }

  // Beats -> seconds through the tempo map (the plugin keeps beats).
  if (!opts.beats) {
    for (Note& n : score.notes) {
      const double end = tempo.seconds(n.start + n.dur);
      n.start = tempo.seconds(n.start);
      n.dur = end - n.start;
    }
  }
  // The tempo at time 0: 120 (as notation apps play a score without a mark) when the first mark is later.
  score.tempo = tempo.points().empty() ? 120 : tempo.points().front().bpm;

  if (otherVoices > 0)
    warnings.push_back(std::to_string(otherVoices) + " note(s) in other voices skipped (sang voice " + voice +
                       "; choose with partVoice)");
  if (chordNotes > 0) warnings.push_back(std::to_string(chordNotes) + " chord note(s) dropped (kept the top note)");
  if (graces > 0) warnings.push_back(std::to_string(graces) + " grace note(s) skipped");
  // The file's own lyrics win; a lyric line given separately is for a part without any.
  if (sung) {
    if (!opts.lyrics.empty()) warnings.push_back("sang the file's own lyrics (the lyrics given were not used)");
    std::set<int> voiceVerses, unsung;
    for (const XmlMeasure& m : own)
      for (const XmlNote& n : m.notes)
        if (n.voice == voice)
          for (const auto& [v, l] : n.lyrics)
            if (!l.text.empty()) voiceVerses.insert(v);
    for (int v : voiceVerses)
      if (!versesSung.count(v)) unsung.insert(v);
    if (opts.verse > 0 && !voiceVerses.count(opts.verse))
      warnings.push_back("the lyrics have no verse " + std::to_string(opts.verse) + "; sang verse " + joinNumbers(versesSung));
    else if (opts.verse <= 0 && !unsung.empty())
      warnings.push_back("lyric verse(s) " + joinNumbers(unsung) + " not sung (choose with verse)");
  } else {
    score.lyrics = opts.lyrics;
    if (opts.lyrics.empty()) warnings.push_back("no lyrics in the MusicXML; notes will hum");
    // With a lyric line supplied, melisma marks from slurs would steal notes from it.
    else
      for (auto& n : score.notes) n.lyric.clear();
  }
  makeMonophonic(score.notes, warnings);

  if (info) {
    *info = std::move(found);
    info->part = chosen;
    info->partVoice = voice;
    info->verse = opts.verse > 0 ? opts.verse : hasRepeats && opts.unfold ? 0 : 1;
    info->fileLyrics = sung;
    info->repeats = hasRepeats;
    info->unfolded = hasRepeats && opts.unfold;
    // A short first measure is a pickup: time 0 is its start, as in notation apps.
    if (!bars.empty() && bars[0].length > 0) {
      const double nominal = bars[0].timeBeats > 0 ? bars[0].timeBeats * 4.0 / bars[0].timeBeatType : 0;
      const bool shortOne = nominal > 0 && bars[0].length < nominal - 1e-6;
      if ((bars[0].implicit && (nominal == 0 || shortOne)) || (shortOne && bars.size() > 1))
        info->pickupBeats = bars[0].length;
    }
    info->tempos = tempo.points();
    for (auto& mt : meters) mt.seconds = tempo.seconds(mt.beat);
    info->meters = meters;
  }
  return score;
}

Score importAny(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings,
                ImportInfo* info) {
  if (data.size() >= 4 && data[0] == 'M' && data[1] == 'T' && data[2] == 'h' && data[3] == 'd')
    return importMidi(data, opts, warnings, info);
  if (data.size() >= 2 && data[0] == 'P' && data[1] == 'K')
    throw std::runtime_error("this is compressed MusicXML (.mxl): unzip it and import the .musicxml file inside, "
                             "or export uncompressed MusicXML (.musicxml)");
  std::string text(data.begin(), data.end());
  size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
  if (first != std::string::npos && text[first] == '<') return importMusicXml(text, opts, warnings, info);
  throw std::runtime_error("unrecognized file: expected MIDI or MusicXML");
}

std::string jsonEscape(const std::string& raw) {
  static const char* hex = "0123456789abcdef";
  std::string r;
  for (unsigned char c : validUtf8(raw)) {
    if (c == '"' || c == '\\') {
      r += '\\';
      r += static_cast<char>(c);
    } else if (c == '\n') {
      r += "\\n";
    } else if (c < 0x20) {
      r += "\\u00";
      r += hex[c >> 4];
      r += hex[c & 15];
    } else {
      r += static_cast<char>(c);
    }
  }
  return r;
}

std::string scoreToJson(const Score& score, bool beats) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  std::ostringstream o;
  o << "{\n  \"units\": \"" << (beats ? "beats" : "seconds") << "\",\n";
  o << "  \"tempo\": " << formatNumber(score.tempo) << ",\n";
  if (score.voice.name != "std") o << "  \"voice\": \"" << jsonEscape(score.voice.name) << "\",\n";
  if (!score.lyrics.empty()) o << "  \"lyrics\": \"" << jsonEscape(score.lyrics) << "\",\n";
  if (!score.dictionary.empty()) {
    o << "  \"dictionary\": {";
    for (size_t i = 0; i < score.dictionary.size(); ++i)
      o << (i ? ", " : "") << "\"" << jsonEscape(score.dictionary[i].first) << "\": \""
        << jsonEscape(score.dictionary[i].second) << "\"";
    o << "},\n";
  }
  o << "  \"notes\": [\n";
  for (size_t i = 0; i < score.notes.size(); ++i) {
    const Note& n = score.notes[i];
    std::string pitch;
    if (std::fabs(n.pitch - std::round(n.pitch)) < 1e-4f) {
      int m = static_cast<int>(std::lround(n.pitch));
      pitch = std::string("\"") + names[((m % 12) + 12) % 12] + std::to_string(m / 12 - 1) + "\"";
    } else {
      pitch = formatFixed("%.3f", n.pitch);
    }
    const char* timeFormat = beats ? "%.6f" : "%.4f";
    o << "    {\"pitch\": " << pitch << ", \"start\": " << formatFixed(timeFormat, n.start)
      << ", \"dur\": " << formatFixed(timeFormat, n.dur);
    if (!n.lyric.empty()) o << ", \"lyric\": \"" << jsonEscape(n.lyric) << "\"";
    o << "}" << (i + 1 < score.notes.size() ? "," : "") << "\n";
  }
  o << "  ]\n}\n";
  return o.str();
}

std::string importInfoToJson(const ImportInfo& info) {
  auto str = [](const std::string& s) { return "\"" + jsonEscape(s) + "\""; };
  auto flag = [](bool b) { return b ? "true" : "false"; };
  std::ostringstream o;
  o << "{\"format\": " << str(info.format) << ", \"part\": " << info.part << ", \"partVoice\": " << str(info.partVoice)
    << ", \"verse\": " << info.verse << ", \"fileLyrics\": " << flag(info.fileLyrics)
    << ", \"repeats\": " << flag(info.repeats) << ", \"unfolded\": " << flag(info.unfolded)
    << ", \"pickupBeats\": " << formatNumber(info.pickupBeats) << ",\n    \"tempoMap\": [";
  for (size_t i = 0; i < info.tempos.size(); ++i) {
    const auto& t = info.tempos[i];
    o << (i ? ", " : "") << "{\"beat\": " << formatNumber(t.beat) << ", \"seconds\": " << formatNumber(t.seconds)
      << ", \"bpm\": " << formatNumber(t.bpm) << "}";
  }
  o << "],\n    \"timeSignatures\": [";
  for (size_t i = 0; i < info.meters.size(); ++i) {
    const auto& m = info.meters[i];
    o << (i ? ", " : "") << "{\"beat\": " << formatNumber(m.beat) << ", \"seconds\": " << formatNumber(m.seconds)
      << ", \"beats\": " << m.beats << ", \"beatType\": " << m.beatType << "}";
  }
  o << "],\n    \"parts\": [";
  for (size_t i = 0; i < info.parts.size(); ++i) {
    const auto& p = info.parts[i];
    o << (i ? ",\n      " : "\n      ") << "{\"index\": " << p.index << ", \"id\": " << str(p.id)
      << ", \"name\": " << str(p.name) << ", \"notes\": " << p.notes << ", \"lyrics\": " << p.lyrics << ", \"verses\": [";
    for (size_t k = 0; k < p.verses.size(); ++k) o << (k ? ", " : "") << p.verses[k];
    o << "], \"voices\": [";
    for (size_t k = 0; k < p.voices.size(); ++k) {
      const auto& v = p.voices[k];
      o << (k ? ", " : "") << "{\"voice\": " << str(v.id) << ", \"notes\": " << v.notes << ", \"lyrics\": " << v.lyrics
        << "}";
    }
    o << "]}";
  }
  o << "]}";
  return o.str();
}

}  // namespace novox
