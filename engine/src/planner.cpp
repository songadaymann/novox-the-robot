#include "planner.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace nvs {

namespace {

// ---------------------------------------------------------------------------
// Syllables

struct Syl {
  double start = 0, end = 0;  // note span (extended by melismas)
  std::string text;
  bool continues = false;
  Pronunciation onset, nucleus, coda;
};

bool isSonorant(const std::string& s) {
  const Phoneme* p = findPhoneme(s);
  return p && (p->cls == PhonClass::Nasal || p->cls == PhonClass::Liquid || p->cls == PhonClass::Glide);
}

// Consonant sequences English allows at the start of a syllable (beyond single
// consonants, which are always allowed). Used for maximal-onset splitting.
bool legalOnset(const Pronunciation& p, size_t from, size_t to) {
  size_t n = to - from;
  if (n <= 1) return true;
  std::string key;
  for (size_t i = from; i < to; ++i) key += (i > from ? " " : "") + p[i].symbol;
  static const std::set<std::string> ok = {
      "P R", "P L", "B R", "B L", "T R", "D R", "K R", "K L", "G R", "G L", "F R", "F L",
      "TH R", "SH R", "S P", "S T", "S K", "S M", "S N", "S L", "S W", "T W", "D W", "K W",
      "P Y", "B Y", "F Y", "M Y", "K Y", "HH Y", "V Y", "G W", "TH W",
      "S P R", "S T R", "S K R", "S P L", "S K W", "S K Y", "S P Y"};
  return ok.count(key) > 0;
}

// Splits one word's phonemes across `n` note syllables. Empty groups mean the
// word has fewer vowels than notes; those notes become melismas.
std::vector<Pronunciation> splitWord(const Pronunciation& pron, size_t n) {
  std::vector<Pronunciation> groups(n);
  std::vector<size_t> vowels;
  for (size_t i = 0; i < pron.size(); ++i)
    if (isVowelSymbol(pron[i].symbol)) vowels.push_back(i);

  if (vowels.empty() || n == 1) {
    groups[0] = pron;
    return groups;
  }

  // Which vowel indices start each group.
  size_t used = std::min(n, vowels.size());
  std::vector<size_t> cut(used + 1);  // phoneme index where each group begins
  cut[0] = 0;
  for (size_t g = 1; g < used; ++g) {
    size_t prevV = vowels[g - 1];
    size_t nextV = vowels[g];
    // Consonants strictly between prevV and nextV. Give the next syllable the
    // longest legal onset, the rest stays as coda.
    size_t split = nextV;
    while (split > prevV + 1 && legalOnset(pron, split - 1, nextV)) --split;
    cut[g] = split;
  }
  cut[used] = pron.size();
  for (size_t g = 0; g < used; ++g) {
    groups[g].assign(pron.begin() + static_cast<long>(cut[g]), pron.begin() + static_cast<long>(cut[g + 1]));
  }
  return groups;
}

void assignParts(Syl& s, const Pronunciation& group) {
  s.onset.clear(); s.nucleus.clear(); s.coda.clear();
  long first = -1, last = -1;
  for (size_t i = 0; i < group.size(); ++i) {
    if (isVowelSymbol(group[i].symbol)) {
      if (first < 0) first = static_cast<long>(i);
      last = static_cast<long>(i);
    }
  }
  if (first < 0) {
    // No vowel (a hum like "mm", or "shh"): sustain the last sonorant.
    for (long i = static_cast<long>(group.size()) - 1; i >= 0; --i) {
      if (isSonorant(group[static_cast<size_t>(i)].symbol)) { first = last = i; break; }
    }
    if (first < 0) {
      s.onset = group;
      s.nucleus.push_back({"AX", 0});
      return;
    }
  }
  s.onset.assign(group.begin(), group.begin() + first);
  s.nucleus.assign(group.begin() + first, group.begin() + last + 1);
  s.coda.assign(group.begin() + last + 1, group.end());
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

bool isExplicit(const std::string& t) { return t.size() >= 2 && t.front() == '{' && t.back() == '}'; }

std::string pronString(const Pronunciation& p) {
  std::string out;
  for (const auto& t : p) out += (out.empty() ? "" : " ") + t.symbol;
  return out;
}

// Flows a lyric line onto notes without lyrics. Hyphenated words keep the
// user's split; other words take one note per vowel in their pronunciation.
std::vector<Note> flowLyrics(const std::vector<Note>& notes, const std::string& line, const Lexicon& lex,
                             std::vector<std::string>& warnings) {
  std::vector<Note> out = notes;
  if (line.empty()) return out;
  std::vector<std::string> tokens;
  for (const auto& tok : tokenizeLyrics(line)) {
    bool userSplit = tok.back() == '-' || isExplicit(tok) || tok == "_" || tok == "+";
    // A token directly after a hyphenated one is the tail of that word.
    bool tail = !tokens.empty() && !tokens.back().empty() && tokens.back().back() == '-';
    if (userSplit || tail) {
      tokens.push_back(tok);
      continue;
    }
    size_t vowels = 0;
    for (const auto& t : lex.lookup(tok)) vowels += isVowelSymbol(t.symbol) ? 1 : 0;
    if (vowels <= 1) {
      tokens.push_back(tok);
      continue;
    }
    tokens.push_back(tok + "-");
    for (size_t k = 1; k + 1 < vowels; ++k) tokens.push_back("-");
    tokens.push_back("");
  }
  size_t t = 0;
  for (auto& note : out) {
    if (!note.lyric.empty()) continue;
    if (t >= tokens.size()) break;
    note.lyric = tokens[t++];
    if (note.lyric.empty()) note.lyric = "\x01";  // word tail (not a hum)
  }
  if (t < tokens.size()) {
    warnings.push_back(std::to_string(tokens.size() - t) + " lyric syllable(s) left over: not enough notes");
  }
  return out;
}

std::vector<Syl> buildSyllables(const std::vector<Note>& notes, const Lexicon& lex,
                                std::vector<std::string>& warnings) {
  std::vector<Syl> syls;
  std::vector<size_t> word;          // indices into syls for the word in progress
  std::vector<std::string> parts;

  auto finishWord = [&]() {
    if (word.empty()) return;
    bool anyExplicit = std::any_of(parts.begin(), parts.end(), isExplicit);
    std::vector<Pronunciation> groups;
    if (anyExplicit) {
      for (const auto& part : parts) {
        Pronunciation p;
        if (isExplicit(part)) {
          if (!Lexicon::parseArpabet(part.substr(1, part.size() - 2), p)) {
            warnings.push_back("bad phonemes " + part + ", humming instead");
            p = {{"M", -1}};
          }
        } else {
          p = part.empty() ? Pronunciation{{"M", -1}} : lex.lookup(part);
        }
        groups.push_back(p);
      }
    } else {
      std::string whole;
      for (const auto& part : parts) whole += part;
      Pronunciation p;
      if (Lexicon::normalize(whole).empty()) {
        p = {{"M", -1}};  // no lyric: hum, like the original Virtual Singer
      } else {
        bool known = true;
        p = lex.lookup(whole, &known);
        if (!known) warnings.push_back("'" + whole + "' not in dictionary, guessed " + pronString(p));
      }
      groups = splitWord(p, word.size());
    }
    for (size_t k = 0; k < word.size(); ++k) {
      Syl& s = syls[word[k]];
      if (groups[k].empty()) {
        // Fewer vowels than notes: this note continues the previous syllable.
        s.nucleus.clear();
        warnings.push_back("'" + s.text + "' has no vowel of its own; holding the previous one");
        continue;
      }
      assignParts(s, groups[k]);
    }
    word.clear();
    parts.clear();
  };

  for (const auto& note : notes) {
    std::string tok = note.lyric == "\x01" ? "" : trim(note.lyric);
    if ((tok == "_" || tok == "+") && !syls.empty()) {
      syls.back().end = std::max(syls.back().end, note.start + note.dur);
      continue;
    }
    bool continues = !tok.empty() && tok.back() == '-' && !isExplicit(tok);
    if (continues) tok.pop_back();
    Syl s;
    s.start = note.start;
    s.end = note.start + note.dur;
    s.text = tok;
    s.continues = continues;
    syls.push_back(s);
    word.push_back(syls.size() - 1);
    parts.push_back(tok);
    if (!continues) finishWord();
  }
  finishWord();

  // Merge syllables that ended up without a nucleus into the previous one.
  std::vector<Syl> out;
  for (auto& s : syls) {
    if (s.nucleus.empty() && !out.empty()) {
      out.back().end = std::max(out.back().end, s.end);
      out.back().text += "~" + s.text;
      continue;
    }
    if (s.nucleus.empty()) assignParts(s, {{"M", -1}});
    out.push_back(s);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Timeline

float naturalDur(const PhonToken& t, const Voice& v) {
  const Phoneme* p = findPhoneme(t.symbol);
  float d = p ? p->duration : 0.08f;
  return d / std::max(0.25f, v.consonantSpeed);
}

float sumDur(const Pronunciation& p, const Voice& v) {
  float s = 0;
  for (const auto& t : p) s += naturalDur(t, v);
  return s;
}

class Emitter {
 public:
  explicit Emitter(std::vector<Segment>& out) : out_(out) {}

  void add(SegKind kind, const std::string& label, double start, double end, const Params& p,
           bool takeNext = false) {
    if (end - start < 1e-4) return;
    // Fill any hole with silence.
    if (!out_.empty() && start > out_.back().end + 1e-6) {
      Segment sil;
      sil.kind = SegKind::Silence;
      sil.label = "_";
      sil.start = out_.back().end;
      sil.end = start;
      sil.p = silenceLike(out_.back().p);
      out_.push_back(sil);
    }
    Segment s;
    s.kind = kind;
    s.label = label;
    // Butt segments together exactly (no float slivers between them).
    s.start = out_.empty() ? start : (start <= out_.back().end + 1e-6 ? out_.back().end : start);
    s.end = end;
    s.p = p;
    s.takeNextFormants = takeNext;
    if (s.end > s.start) out_.push_back(s);
  }

  // A consonant occupying [start, start+dur).
  void consonant(const PhonToken& tok, double start, double dur, bool beforeVowel) {
    const Phoneme* ph = findPhoneme(tok.symbol);
    if (!ph) return;
    double end = start + dur;
    switch (ph->cls) {
      case PhonClass::Stop: {
        double burst = std::min(0.014, dur * 0.2);
        bool aspirate = !ph->voiced && beforeVowel;
        double asp = aspirate ? dur * 0.35 : 0.0;
        double closure = dur - burst - asp;
        add(SegKind::Closure, ph->symbol, start, start + closure, ph->target);
        Params b = ph->burst;
        copyFormants(b, ph->target);
        float strength = ph->voiced ? 0.6f : 1.0f;
        if (!beforeVowel) strength *= 0.9f;  // sung final stops are released, a bit softer
        b[AF] *= strength;
        b[AB] *= strength;
        b[AV] = ph->voiced ? 0.25f : 0.0f;
        add(SegKind::Burst, ph->symbol, start + closure, start + closure + burst, b);
        if (aspirate) {
          Params a = ph->target;
          a[AV] = 0; a[AH] = 0.45f; a[F1] = 450;
          add(SegKind::Aspiration, ph->symbol, start + closure + burst, end, a);
        }
        break;
      }
      case PhonClass::Affricate: {
        double closure = dur * 0.4;
        add(SegKind::Closure, ph->symbol, start, start + closure, ph->target);
        Params f = ph->burst;  // the fricative part (SH / ZH)
        copyFormants(f, ph->target);
        add(SegKind::Consonant, ph->symbol, start + closure, end, f);
        break;
      }
      case PhonClass::Aspirate:
        add(SegKind::Consonant, ph->symbol, start, end, ph->target, /*takeNext=*/true);
        break;
      default:
        add(SegKind::Consonant, ph->symbol, start, end, ph->target);
    }
  }

  void vowelRun(const Pronunciation& nucleus, double start, double end, const Voice& v) {
    // Interior consonants (e.g. "every" squeezed onto one note) keep natural
    // length; vowels share what's left, the first vowel getting the most.
    double consTime = 0, weights = 0;
    bool firstVowel = true;
    for (const auto& t : nucleus) {
      if (isVowelSymbol(t.symbol)) {
        weights += firstVowel ? 2.0 : 1.0;
        firstVowel = false;
      } else {
        consTime += naturalDur(t, v);
      }
    }
    double total = end - start;
    double consScale = consTime > total * 0.5 ? (total * 0.5) / consTime : 1.0;
    double vowelTime = total - consTime * consScale;
    double t = start;
    firstVowel = true;
    for (size_t i = 0; i < nucleus.size(); ++i) {
      const auto& tok = nucleus[i];
      const Phoneme* ph = findPhoneme(tok.symbol);
      if (!ph) continue;
      if (ph->cls != PhonClass::Vowel) {
        if (weights == 0) {  // hummed nucleus: sustain the sonorant
          add(SegKind::Vowel, ph->symbol, t, end, ph->target);
          t = end;
          continue;
        }
        double d = naturalDur(tok, v) * consScale;
        consonant(tok, t, d, true);
        t += d;
        continue;
      }
      double d = vowelTime * (firstVowel ? 2.0 : 1.0) / weights;
      firstVowel = false;
      if (ph->diphthong) {
        double off = std::min(0.16, d * 0.35);
        add(SegKind::Vowel, ph->symbol, t, t + d - off, ph->target);
        add(SegKind::Offglide, ph->symbol, t + d - off, t + d, ph->offglide);
      } else {
        add(SegKind::Vowel, ph->symbol, t, t + d, ph->target);
      }
      t += d;
    }
  }

 private:
  static void copyFormants(Params& dst, const Params& src) {
    for (int i = F1; i <= B5; ++i) dst[i] = src[i];
  }
  std::vector<Segment>& out_;
};

void setTransitions(std::vector<Segment>& segs) {
  for (size_t k = 0; k < segs.size(); ++k) {
    Segment& cur = segs[k];
    if (k == 0) {
      cur.tFreq = 0.001f;
      cur.tAmp = 0.02f;
      continue;
    }
    const Segment& prev = segs[k - 1];
    bool prevVowel = prev.kind == SegKind::Vowel || prev.kind == SegKind::Offglide;
    switch (cur.kind) {
      case SegKind::Silence: cur.tFreq = 0.001f; cur.tAmp = 0.035f; break;
      case SegKind::Burst: cur.tFreq = 0.004f; cur.tAmp = 0.002f; break;
      case SegKind::Closure: cur.tFreq = 0.03f; cur.tAmp = 0.012f; break;
      case SegKind::Aspiration: cur.tFreq = 0.01f; cur.tAmp = 0.006f; break;
      case SegKind::Offglide:
        cur.tFreq = static_cast<float>((cur.end - cur.start) * 0.9);
        cur.tAmp = 0.03f;
        break;
      case SegKind::Vowel:
        if (prev.kind == SegKind::Silence) { cur.tFreq = 0.001f; cur.tAmp = 0.03f; }
        else if (prevVowel) { cur.tFreq = 0.08f; cur.tAmp = 0.03f; }
        else { cur.tFreq = 0.055f; cur.tAmp = 0.018f; }
        break;
      case SegKind::Consonant:
        if (prev.kind == SegKind::Silence) { cur.tFreq = 0.001f; cur.tAmp = 0.015f; }
        else { cur.tFreq = 0.04f; cur.tAmp = 0.015f; }
        break;
    }
  }
}

// HH takes the formants of the vowel after it; silences split in two so each
// half matches its neighbor (formants never sweep while fading).
void resolveNeighbors(std::vector<Segment>& segs) {
  // Velar bursts sit just above the next vowel's F2 (high before "ee", low before "oo").
  for (size_t k = 0; k < segs.size(); ++k) {
    if (segs[k].kind != SegKind::Burst || (segs[k].label != "K" && segs[k].label != "G")) continue;
    for (size_t j = k + 1; j < segs.size() && segs[j].start - segs[k].end < 0.2; ++j) {
      if (segs[j].p[AV] > 0.5f) {
        segs[k].p[N1F] = std::clamp(segs[j].p[F2] * 1.15f, 1300.0f, 3000.0f);
        break;
      }
    }
  }
  for (size_t k = 0; k < segs.size(); ++k) {
    if (!segs[k].takeNextFormants) continue;
    for (size_t j = k + 1; j < segs.size() && segs[j].start - segs[k].end < 0.3; ++j) {
      if (segs[j].p[AV] > 0.5f) {
        for (int i = F1; i <= B5; ++i) segs[k].p[i] = segs[j].p[i];
        break;
      }
    }
  }
  std::vector<Segment> out;
  for (size_t k = 0; k < segs.size(); ++k) {
    Segment s = segs[k];
    if (s.kind != SegKind::Silence) {
      out.push_back(s);
      continue;
    }
    const Segment* prev = k > 0 ? &segs[k - 1] : nullptr;
    const Segment* next = k + 1 < segs.size() ? &segs[k + 1] : nullptr;
    if (prev && next) {
      double mid = (s.start + s.end) / 2;
      Segment a = s, b = s;
      a.end = mid;
      a.p = silenceLike(prev->p);
      b.start = mid;
      b.p = silenceLike(next->p);
      b.tAmp = 0.001f;  // silence to silence
      out.push_back(a);
      out.push_back(b);
    } else {
      s.p = silenceLike(prev ? prev->p : next ? next->p : s.p);
      out.push_back(s);
    }
  }
  segs.swap(out);
}

}  // namespace

Plan planScore(const Score& score, const Lexicon& lex) {
  Plan plan;
  const Voice& v = score.voice;
  std::vector<Note> notes = flowLyrics(score.notes, score.lyrics, lex, plan.warnings);
  std::vector<Syl> syls = buildSyllables(notes, lex, plan.warnings);
  if (syls.empty()) {
    plan.length = 0.5;
    return plan;
  }

  // Pre-roll so the first syllable's consonants can land before its note.
  double firstOnset = sumDur(syls[0].onset, v);
  plan.lead = std::max(0.05, firstOnset + 0.05 - syls[0].start);
  for (auto& s : syls) { s.start += plan.lead; s.end += plan.lead; }

  const size_t n = syls.size();
  // Where each syllable's pieces go, after consonants borrow time.
  std::vector<double> onsetStart(n), onsetScale(n, 1.0), vowelEnd(n), codaScale(n, 1.0);
  for (size_t i = 0; i < n; ++i) vowelEnd[i] = syls[i].end;

  onsetStart[0] = syls[0].start - firstOnset;
  for (size_t i = 0; i < n; ++i) {
    Syl& a = syls[i];
    if (i + 1 < n) a.end = std::min(a.end, syls[i + 1].start);  // no overlaps
    double coda = sumDur(a.coda, v);
    double nextOnset = i + 1 < n ? sumDur(syls[i + 1].onset, v) : 0.0;
    double gap = i + 1 < n ? std::max(0.0, syls[i + 1].start - a.end) : 0.0;
    double noteLen = a.end - a.start;
    double maxBorrow = noteLen * 0.6;
    double need = coda + std::max(0.0, nextOnset - gap);
    double scale = 1.0;
    if (need > maxBorrow && coda + nextOnset > 0) {
      scale = std::min(1.0, (maxBorrow + gap) / (coda + nextOnset));
    }
    codaScale[i] = scale;
    double codaEnd = a.end;
    if (i + 1 < n) {
      onsetScale[i + 1] = scale;
      onsetStart[i + 1] = syls[i + 1].start - nextOnset * scale;
      codaEnd = std::min(a.end, onsetStart[i + 1]);
    }
    vowelEnd[i] = codaEnd - coda * scale;
  }

  std::vector<Segment>& segs = plan.segments;
  Emitter emit(segs);
  // Silence from time 0 up to the first consonant.
  if (onsetStart[0] > 1e-6) {
    Segment sil;
    sil.kind = SegKind::Silence;
    sil.label = "_";
    sil.end = onsetStart[0];
    segs.push_back(sil);
  }
  for (size_t i = 0; i < n; ++i) {
    const Syl& s = syls[i];
    // Onset consonants, ending exactly on the beat.
    double t = onsetStart[i];
    for (const auto& tok : s.onset) {
      double d = naturalDur(tok, v) * onsetScale[i];
      emit.consonant(tok, t, d, true);
      t += d;
    }
    emit.vowelRun(s.nucleus, s.start, vowelEnd[i], v);
    t = vowelEnd[i];
    for (size_t c = 0; c < s.coda.size(); ++c) {
      double d = naturalDur(s.coda[c], v) * codaScale[i];
      emit.consonant(s.coda[c], t, d, false);
      t += d;
    }
    SyllableInfo info;
    info.start = s.start - plan.lead;
    info.end = s.end - plan.lead;
    info.text = s.text;
    info.continues = s.continues;
    info.phonemes = pronString(s.onset) + " | " + pronString(s.nucleus) + " | " + pronString(s.coda);
    plan.syllables.push_back(info);
  }
  // Tail silence for releases and reverb-free decay.
  double end = segs.empty() ? plan.lead : segs.back().end;
  emit.add(SegKind::Silence, "_", end, end + 0.3, silenceLike(segs.back().p));
  segs.back().p = silenceLike(segs.back().p);

  setTransitions(segs);
  resolveNeighbors(segs);

  // Pitch notes (every note, including melismas).
  const Note* prev = nullptr;
  for (const auto& note : score.notes) {
    PitchNote pn;
    pn.start = note.start + plan.lead;
    pn.end = note.start + note.dur + plan.lead;
    pn.midi = note.pitch;
    pn.legato = prev && std::fabs(note.start - (prev->start + prev->dur)) < 0.01;
    plan.notes.push_back(pn);
    prev = &note;
  }
  plan.length = segs.back().end;
  return plan;
}

std::string planToJson(const Plan& plan) {
  std::ostringstream o;
  o.precision(4);
  o << std::fixed;
  auto esc = [](const std::string& s) {
    std::string r;
    for (char c : s) {
      if (c == '"' || c == '\\') r += '\\';
      r += c;
    }
    return r;
  };
  o << "{\n  \"lead\": " << plan.lead << ",\n  \"length\": " << plan.length << ",\n";
  o << "  \"syllables\": [\n";
  for (size_t i = 0; i < plan.syllables.size(); ++i) {
    const auto& s = plan.syllables[i];
    o << "    {\"start\": " << s.start << ", \"end\": " << s.end << ", \"text\": \"" << esc(s.text)
      << "\", \"phonemes\": \"" << esc(s.phonemes) << "\", \"continues\": " << (s.continues ? "true" : "false") << "}" << (i + 1 < plan.syllables.size() ? "," : "") << "\n";
  }
  o << "  ],\n  \"segments\": [\n";
  static const char* kinds[] = {"silence", "vowel", "offglide", "closure", "burst", "aspiration", "consonant"};
  for (size_t i = 0; i < plan.segments.size(); ++i) {
    const auto& s = plan.segments[i];
    o << "    {\"label\": \"" << esc(s.label) << "\", \"kind\": \"" << kinds[static_cast<int>(s.kind)]
      << "\", \"start\": " << s.start << ", \"end\": " << s.end << "}"
      << (i + 1 < plan.segments.size() ? "," : "") << "\n";
  }
  o << "  ],\n  \"warnings\": [";
  for (size_t i = 0; i < plan.warnings.size(); ++i)
    o << (i ? ", " : "") << "\"" << esc(plan.warnings[i]) << "\"";
  o << "]\n}\n";
  return o.str();
}

}  // namespace nvs
