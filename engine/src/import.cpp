#include "import.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace nvs {

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

// MIDI lyric syllables mark word boundaries with spaces and hyphens in various
// styles ("Hel-", "lo ", "/Next", "\\Line"). Normalize to our tokens.
std::string cleanMidiLyric(std::string s, bool& wordContinues) {
  for (char& c : s) {
    if (c == '/' || c == '\\' || c == '\r' || c == '\n') c = ' ';
  }
  bool trailingSpace = !s.empty() && s.back() == ' ';
  s = trimCopy(s);
  wordContinues = !s.empty() && s.back() == '-';
  (void)trailingSpace;
  return s;
}

}  // namespace

Score importMidi(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings) {
  MidiReader r(data);
  r.setRange(0, data.size());
  if (r.bytes(4) != "MThd") throw std::runtime_error("not a MIDI file");
  uint32_t hlen = r.be(4);
  size_t afterHeader = r.pos() + hlen;
  r.be(2);  // format
  uint32_t ntracks = r.be(2);
  uint32_t division = r.be(2);
  if (division & 0x8000) throw std::runtime_error("SMPTE-timed MIDI files aren't supported");
  r.setRange(afterHeader, data.size());

  std::map<uint32_t, double> tempo;  // tick -> microseconds per quarter
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
        } else if (type == 0x05) {
          track.lyrics.emplace_back(tick, payload);
        } else if (type == 0x03 && track.name.empty()) {
          track.name = payload;
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
  auto seconds = [&](uint32_t tk) {
    double sec = 0;
    auto it = tempo.begin();
    while (it != tempo.end()) {
      auto next = std::next(it);
      uint32_t segEnd = (next == tempo.end() || next->first > tk) ? tk : next->first;
      sec += static_cast<double>(segEnd - it->first) / division * it->second / 1e6;
      if (segEnd == tk) break;
      it = next;
    }
    return sec;
  };

  const MidiTrack& tr = tracks[static_cast<size_t>(chosen)];
  // Lyric events: use the chosen track's, else any track's (some files keep them apart).
  std::vector<std::pair<uint32_t, std::string>> lyrics = tr.lyrics;
  if (lyrics.empty()) {
    for (const auto& t : tracks) {
      if (!t.lyrics.empty()) {
        lyrics = t.lyrics;
        break;
      }
    }
  }

  Score score;
  score.tempo = 60e6 / tempo.begin()->second;
  score.lyrics = opts.lyrics;
  std::vector<MidiNote> notes = tr.notes;
  std::sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.tick < b.tick; });
  const uint32_t tolerance = std::max<uint32_t>(1, division / 8);
  size_t li = 0;
  for (const auto& mn : notes) {
    Note n;
    n.start = seconds(mn.tick);
    n.dur = seconds(mn.endTick) - n.start;
    n.pitch = static_cast<float>(mn.pitch);
    // Attach the lyric event nearest this note's start.
    while (li < lyrics.size() && lyrics[li].first + tolerance < mn.tick) ++li;
    if (li < lyrics.size() && lyrics[li].first <= mn.tick + tolerance) {
      bool cont = false;
      n.lyric = cleanMidiLyric(lyrics[li].second, cont);
      ++li;
    }
    score.notes.push_back(n);
  }
  makeMonophonic(score.notes, warnings);
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
      else if (!ent.empty() && ent[0] == '#') {
        unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::stoul(ent.substr(2), nullptr, 16)
                                                                           : std::stoul(ent.substr(1));
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
          out += static_cast<char>(0xC0 | (cp >> 6));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
          out += static_cast<char>(0xE0 | (cp >> 12));
          out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        }
      } else {
        out += "&" + ent + ";";
      }
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

}  // namespace

Score importMusicXml(const std::string& xml, const ImportOptions& opts, std::vector<std::string>& warnings) {
  auto root = XmlParser(xml).parse();
  if (root->name == "score-timewise") throw std::runtime_error("timewise MusicXML isn't supported (use partwise)");
  if (root->name != "score-partwise") throw std::runtime_error("not a MusicXML score");

  std::vector<const XmlNode*> parts;
  for (const auto& c : root->children)
    if (c->name == "part") parts.push_back(c.get());
  if (parts.empty()) throw std::runtime_error("MusicXML has no parts");
  size_t partIndex = opts.track >= 0 ? static_cast<size_t>(opts.track) : 0;
  if (partIndex >= parts.size()) throw std::runtime_error("MusicXML part " + std::to_string(opts.track) + " not found");
  if (parts.size() > 1 && opts.track < 0)
    warnings.push_back("MusicXML has " + std::to_string(parts.size()) + " parts; using the first (choose with track)");

  Score score;
  score.lyrics = opts.lyrics;
  double divisions = 1, tempo = 120;
  bool tempoSet = false;
  double sec = 0;  // time cursor
  std::string mainVoice;
  bool lastHadExtend = false, inSlur = false, anyLyric = false;
  int otherVoices = 0;

  auto noteSeconds = [&](double dur) { return dur / divisions * 60.0 / tempo; };

  for (const auto& m : parts[partIndex]->children) {
    if (m->name != "measure") continue;
    for (const auto& el : m->children) {
      const XmlNode& e = *el;
      if (e.name == "attributes") {
        std::string d = e.childText("divisions");
        if (!d.empty()) divisions = std::max(1.0, std::stod(d));
      } else if (e.name == "direction" || e.name == "sound") {
        const XmlNode* snd = e.name == "sound" ? &e : e.child("sound");
        if (snd && !snd->attr("tempo").empty()) {
          tempo = std::max(1.0, std::stod(snd->attr("tempo")));
          if (!tempoSet) score.tempo = tempo;
          tempoSet = true;
        }
      } else if (e.name == "backup") {
        sec -= noteSeconds(std::stod(e.childText("duration", "0")));
      } else if (e.name == "forward") {
        sec += noteSeconds(std::stod(e.childText("duration", "0")));
      } else if (e.name == "note") {
        if (e.child("grace") || e.child("cue")) continue;
        if (e.child("chord")) continue;  // doesn't advance time; top voice already taken
        double d = noteSeconds(std::stod(e.childText("duration", "0")));
        std::string voice = e.childText("voice", "1");
        if (mainVoice.empty() && !e.child("rest")) mainVoice = voice;
        bool mine = mainVoice.empty() || voice == mainVoice;
        double start = sec;
        sec += d;
        if (!mine) {
          ++otherVoices;
          continue;
        }
        if (e.child("rest")) {
          lastHadExtend = false;
          continue;
        }
        const XmlNode* pitch = e.child("pitch");
        if (!pitch) continue;  // unpitched
        std::string step = pitch->childText("step", "C");
        float midi = static_cast<float>((std::stoi(pitch->childText("octave", "4")) + 1) * 12 + stepSemis(step[0]));
        std::string alter = pitch->childText("alter");
        if (!alter.empty()) midi += static_cast<float>(std::stod(alter));

        bool tieStop = false, tieStart = false;
        for (const auto& c : e.children) {
          if (c->name == "tie") {
            if (c->attr("type") == "stop") tieStop = true;
            if (c->attr("type") == "start") tieStart = true;
          }
        }
        (void)tieStart;
        bool slurStart = false, slurStop = false;
        if (const XmlNode* nots = e.child("notations")) {
          for (const auto& c : nots->children) {
            if (c->name == "slur") {
              if (c->attr("type") == "start") slurStart = true;
              if (c->attr("type") == "stop") slurStop = true;
            }
          }
        }

        // Tied continuation: lengthen the previous note.
        if (tieStop && !score.notes.empty() && std::fabs(score.notes.back().pitch - midi) < 0.01f) {
          score.notes.back().dur = start + d - score.notes.back().start;
          continue;
        }

        Note n;
        n.start = start;
        n.dur = d;
        n.pitch = midi;
        const XmlNode* lyric = nullptr;
        for (const auto& c : e.children) {
          if (c->name == "lyric" && (c->attr("number", "1") == "1" || !lyric)) {
            lyric = c.get();
            if (c->attr("number", "1") == "1") break;
          }
        }
        if (lyric && !lyric->childText("text").empty()) {
          std::string syl = lyric->childText("syllabic", "single");
          n.lyric = lyric->childText("text");
          if (syl == "begin" || syl == "middle") n.lyric += "-";
          lastHadExtend = lyric->child("extend") != nullptr;
          anyLyric = true;
        } else if (lastHadExtend || inSlur || (lyric && lyric->child("extend")) ||
                   (anyLyric && !score.notes.empty() &&
                    std::fabs(score.notes.back().start + score.notes.back().dur - start) < 1e-3)) {
          // Melisma: an untexted note straight after a sung one continues its vowel.
          n.lyric = "_";
        }
        if (slurStart) inSlur = true;
        if (slurStop) inSlur = false;
        score.notes.push_back(n);
      }
    }
  }
  if (otherVoices > 0) warnings.push_back(std::to_string(otherVoices) + " note(s) in other voices skipped");
  if (!anyLyric && opts.lyrics.empty()) warnings.push_back("no lyrics in the MusicXML; notes will hum");
  // With a lyric line supplied, melisma marks from slurs would steal notes from it.
  if (!anyLyric && !opts.lyrics.empty())
    for (auto& n : score.notes) n.lyric.clear();
  makeMonophonic(score.notes, warnings);
  return score;
}

Score importAny(const std::vector<uint8_t>& data, const ImportOptions& opts, std::vector<std::string>& warnings) {
  if (data.size() >= 4 && data[0] == 'M' && data[1] == 'T' && data[2] == 'h' && data[3] == 'd')
    return importMidi(data, opts, warnings);
  if (data.size() >= 2 && data[0] == 'P' && data[1] == 'K')
    throw std::runtime_error("compressed MusicXML (.mxl) isn't supported yet; export uncompressed .musicxml/.xml");
  std::string text(data.begin(), data.end());
  size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
  if (first != std::string::npos && text[first] == '<') return importMusicXml(text, opts, warnings);
  throw std::runtime_error("unrecognized file: expected MIDI or MusicXML");
}

std::string scoreToJson(const Score& score) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  auto esc = [](const std::string& s) {
    std::string r;
    for (char c : s) {
      if (c == '"' || c == '\\') r += '\\';
      if (c == '\n') {
        r += "\\n";
        continue;
      }
      r += c;
    }
    return r;
  };
  std::ostringstream o;
  o << "{\n  \"units\": \"seconds\",\n";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", score.tempo);
  o << "  \"tempo\": " << buf << ",\n";
  if (score.voice.name != "std") o << "  \"voice\": \"" << esc(score.voice.name) << "\",\n";
  if (!score.lyrics.empty()) o << "  \"lyrics\": \"" << esc(score.lyrics) << "\",\n";
  if (!score.dictionary.empty()) {
    o << "  \"dictionary\": {";
    for (size_t i = 0; i < score.dictionary.size(); ++i)
      o << (i ? ", " : "") << "\"" << esc(score.dictionary[i].first) << "\": \"" << esc(score.dictionary[i].second) << "\"";
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
      std::snprintf(buf, sizeof buf, "%.3f", n.pitch);
      pitch = buf;
    }
    std::snprintf(buf, sizeof buf, "%.4f", n.start);
    std::string start = buf;
    std::snprintf(buf, sizeof buf, "%.4f", n.dur);
    o << "    {\"pitch\": " << pitch << ", \"start\": " << start << ", \"dur\": " << buf;
    if (!n.lyric.empty()) o << ", \"lyric\": \"" << esc(n.lyric) << "\"";
    o << "}" << (i + 1 < score.notes.size() ? "," : "") << "\n";
  }
  o << "  ]\n}\n";
  return o.str();
}

}  // namespace nvs
