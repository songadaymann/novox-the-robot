// Sung-syllable inventory and lyric-sheet hyphenation (docs/SINGING-V2-DESIGN.md,
// track C).
//
// Syllables are the vowels of the lexicon pronunciation, split by maximal onset
// with the planner's English onset list, so they are the syllables Novox sings.
// Spelling pieces come from aligning the letters to those phonemes (a grapheme
// table and a small DP) and then moving each cut to where a lyric sheet puts it:
// after a short stressed vowel (ev-er-y, mag-ic), between doubled consonants
// (hap-py), before consonant + "le" (ta-ble), at compound, prefix and
// consonant-suffix boundaries (with-out, re-spect, harm-less), and never inside
// a digraph, "qu" or "x".
#include "syllables.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <utility>

#include "phonemes.hpp"
#include "planner.hpp"
#include "utf8.hpp"

namespace novox {

namespace {

bool isVowelLetter(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; }
bool isConsonantLetter(char c) { return c >= 'a' && c <= 'z' && !isVowelLetter(c); }

bool oneOf(const std::string& s, std::initializer_list<const char*> options) {
  for (const char* o : options)
    if (s == o) return true;
  return false;
}

bool endsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// ---------------------------------------------------------------------------
// Letters to phonemes

struct SpellingRule {
  const char* phonemes;  // one symbol, or two that one spelling covers ("K S" for x)
  const char* letters;
  float cost;            // 0 for the usual spelling, higher for rarer ones
};

// English spellings per phoneme, enough to place each letter of common words.
// Anything missing still aligns through the generic costs in align().
constexpr SpellingRule kSpellings[] = {
    {"P", "p", 0}, {"P", "pp", 0},
    {"B", "b", 0}, {"B", "bb", 0},
    {"T", "t", 0}, {"T", "tt", 0}, {"T", "th", .6f}, {"T", "d", .5f}, {"T", "bt", .3f}, {"T", "pt", .5f},
    {"D", "d", 0}, {"D", "dd", 0}, {"D", "t", .8f},
    {"K", "k", 0}, {"K", "c", 0}, {"K", "ck", 0}, {"K", "cc", .2f}, {"K", "ch", .3f}, {"K", "q", .2f},
    {"K", "kh", .4f}, {"K", "cq", .3f}, {"K", "lk", .5f},
    {"G", "g", 0}, {"G", "gg", 0}, {"G", "gh", .3f}, {"G", "gu", .3f},
    {"F", "f", 0}, {"F", "ff", 0}, {"F", "ph", .1f}, {"F", "gh", .3f}, {"F", "lf", .5f}, {"F", "v", .8f},
    {"V", "v", 0}, {"V", "vv", .2f}, {"V", "f", .5f}, {"V", "ph", .8f},
    {"TH", "th", 0}, {"DH", "th", 0},
    {"S", "s", 0}, {"S", "ss", 0}, {"S", "c", .1f}, {"S", "sc", .2f}, {"S", "ps", .3f}, {"S", "z", .8f},
    {"Z", "z", 0}, {"Z", "zz", 0}, {"Z", "s", .1f}, {"Z", "ss", .3f}, {"Z", "x", .5f},
    {"SH", "sh", 0}, {"SH", "ti", .2f}, {"SH", "ci", .2f}, {"SH", "si", .3f}, {"SH", "ssi", .2f},
    {"SH", "ch", .3f}, {"SH", "s", .4f}, {"SH", "ss", .3f}, {"SH", "sci", .3f}, {"SH", "ce", .4f},
    {"SH", "c", .6f}, {"SH", "t", .8f},
    {"ZH", "s", .2f}, {"ZH", "si", .2f}, {"ZH", "g", .4f}, {"ZH", "z", .4f}, {"ZH", "ge", .4f},
    {"CH", "ch", 0}, {"CH", "tch", 0}, {"CH", "t", .3f}, {"CH", "c", .6f}, {"CH", "ti", .4f},
    {"JH", "j", 0}, {"JH", "g", .1f}, {"JH", "dg", .1f}, {"JH", "gg", .4f}, {"JH", "d", .4f},
    {"JH", "di", .5f}, {"JH", "ge", .3f}, {"JH", "gi", .3f},
    {"M", "m", 0}, {"M", "mm", 0}, {"M", "mb", .2f}, {"M", "mn", .3f}, {"M", "lm", .4f}, {"M", "gm", .4f},
    {"N", "n", 0}, {"N", "nn", 0}, {"N", "kn", .1f}, {"N", "gn", .1f}, {"N", "pn", .3f}, {"N", "mn", .5f},
    {"NG", "ng", 0}, {"NG", "n", .2f}, {"NG", "ngue", .3f},
    {"L", "l", 0}, {"L", "ll", 0},
    {"R", "r", 0}, {"R", "rr", 0}, {"R", "wr", .1f}, {"R", "rh", .1f}, {"R", "rrh", .2f},
    {"W", "w", 0}, {"W", "wh", .1f}, {"W", "u", .3f}, {"W", "o", .8f},
    {"Y", "y", 0}, {"Y", "i", .4f}, {"Y", "j", .5f},
    {"HH", "h", 0}, {"HH", "wh", .2f}, {"HH", "j", .5f},
    {"K S", "x", 0}, {"G Z", "x", .1f}, {"K SH", "x", .3f}, {"G ZH", "x", .3f},
    {"K W", "qu", 0},

    {"AA", "o", 0}, {"AA", "a", .2f}, {"AA", "ah", .2f}, {"AA", "au", .3f}, {"AA", "aa", .2f},
    {"AA", "al", .5f}, {"AA", "ea", .8f},
    {"AE", "a", 0}, {"AE", "ai", .6f}, {"AE", "au", .6f},
    {"AH", "u", 0}, {"AH", "o", .3f}, {"AH", "a", .6f}, {"AH", "ou", .3f}, {"AH", "oo", .6f},
    {"AH", "oe", .6f}, {"AH", "uh", .2f}, {"AH", "ough", .4f},
    {"AX", "a", .2f}, {"AX", "e", .2f}, {"AX", "i", .3f}, {"AX", "o", .2f}, {"AX", "u", .2f},
    {"AX", "y", .5f}, {"AX", "ou", .3f}, {"AX", "io", .3f}, {"AX", "ia", .6f}, {"AX", "ai", .6f},
    {"AX", "eo", .4f}, {"AX", "ei", .6f}, {"AX", "ea", .6f}, {"AX", "iou", .4f}, {"AX", "eou", .5f},
    {"AX", "ie", .6f}, {"AX", "oi", .8f},
    {"AO", "o", .3f}, {"AO", "a", .4f}, {"AO", "au", .1f}, {"AO", "aw", .1f}, {"AO", "ou", .5f},
    {"AO", "oa", .4f}, {"AO", "augh", .1f}, {"AO", "ough", .2f}, {"AO", "al", .4f}, {"AO", "oo", .8f},
    {"AW", "ou", .1f}, {"AW", "ow", .1f}, {"AW", "au", .6f}, {"AW", "ough", .3f},
    {"AY", "i", .2f}, {"AY", "y", .2f}, {"AY", "ie", .2f}, {"AY", "igh", .1f}, {"AY", "eye", .2f},
    {"AY", "ye", .2f}, {"AY", "uy", .2f}, {"AY", "ai", .6f}, {"AY", "ei", .4f}, {"AY", "eigh", .4f},
    {"AY", "ay", .8f}, {"AY", "aye", .3f},
    {"EH", "e", 0}, {"EH", "ea", .3f}, {"EH", "a", .8f}, {"EH", "ai", .5f}, {"EH", "ie", .8f},
    {"EH", "ay", .8f}, {"EH", "u", 1}, {"EH", "eo", .5f}, {"EH", "ae", .4f}, {"EH", "ei", .8f},
    {"ER", "er", 0}, {"ER", "ir", 0}, {"ER", "ur", 0}, {"ER", "ear", .1f}, {"ER", "our", .2f},
    {"ER", "or", .2f}, {"ER", "ar", .2f}, {"ER", "yr", .3f}, {"ER", "re", .2f}, {"ER", "r", .4f},
    {"ER", "ere", .3f}, {"ER", "eur", .4f},
    {"EY", "a", .2f}, {"EY", "ai", .1f}, {"EY", "ay", .1f}, {"EY", "ei", .2f}, {"EY", "ey", .2f},
    {"EY", "ea", .5f}, {"EY", "eigh", .1f}, {"EY", "e", .8f}, {"EY", "ae", .5f}, {"EY", "aigh", .3f},
    {"IH", "i", 0}, {"IH", "y", .2f}, {"IH", "e", .3f}, {"IH", "ui", .4f}, {"IH", "ee", .6f},
    {"IH", "ie", .5f}, {"IH", "u", .8f}, {"IH", "o", 1}, {"IH", "a", .6f}, {"IH", "ea", .5f},
    {"IH", "ai", .6f}, {"IH", "ei", .6f},
    {"IY", "y", 0}, {"IY", "e", .2f}, {"IY", "ee", .1f}, {"IY", "ea", .1f}, {"IY", "i", .5f},
    {"IY", "ie", .2f}, {"IY", "ei", .3f}, {"IY", "ey", .1f}, {"IY", "eo", .3f}, {"IY", "ae", .4f},
    {"IY", "oe", .6f},
    {"OW", "o", .2f}, {"OW", "oa", .1f}, {"OW", "ow", .1f}, {"OW", "oe", .2f}, {"OW", "ou", .4f},
    {"OW", "ough", .2f}, {"OW", "oh", .2f}, {"OW", "eau", .2f}, {"OW", "ew", .6f}, {"OW", "au", .8f},
    {"OY", "oi", .1f}, {"OY", "oy", .1f},
    {"UH", "oo", .2f}, {"UH", "u", .3f}, {"UH", "ou", .4f}, {"UH", "o", .6f},
    {"UW", "oo", .1f}, {"UW", "u", .2f}, {"UW", "ew", .1f}, {"UW", "ue", .2f}, {"UW", "ou", .3f},
    {"UW", "o", .5f}, {"UW", "ui", .3f}, {"UW", "eu", .3f}, {"UW", "oe", .3f}, {"UW", "ough", .3f},
    {"UW", "wo", .6f},
    // Letters that spell a glide and a vowel (music, beauty, few, one) or a
    // vowel and a consonant (ta-ble).
    {"Y UW", "u", .2f}, {"Y UW", "eu", .2f}, {"Y UW", "ew", .2f}, {"Y UW", "ue", .2f},
    {"Y UW", "eau", .1f}, {"Y UW", "iew", .1f}, {"Y UW", "ieu", .2f}, {"Y UH", "u", .3f},
    {"Y AX", "u", .3f}, {"Y ER", "ur", .4f}, {"Y UH", "eu", .4f},
    {"W AH", "o", .5f}, {"AX L", "le", .1f},
};

struct Spelling {
  std::string second, letters;  // second phoneme ("" for one), letters
  float cost;
};

const std::unordered_map<std::string, std::vector<Spelling>>& spellings() {
  static const auto table = [] {
    std::unordered_map<std::string, std::vector<Spelling>> t;
    for (const auto& rule : kSpellings) {
      const std::string ph = rule.phonemes;
      const size_t space = ph.find(' ');
      t[ph.substr(0, space)].push_back(
          {space == std::string::npos ? "" : ph.substr(space + 1), rule.letters, rule.cost});
    }
    return t;
  }();
  return table;
}

// Cost of a letter that spells nothing: cheap where English writes silent
// letters (final e, doubled consonants, kn-, -mb, gh, apostrophes).
float silentCost(const std::string& s, size_t i) {
  const size_t n = s.size();
  const char c = s[i];
  const char prev = i > 0 ? s[i - 1] : '\0';
  const char next = i + 1 < n ? s[i + 1] : '\0';
  if (c == '\'') return 0;
  if (isConsonantLetter(c) && c == prev) return .1f;
  switch (c) {
    case 'e':
      if (i + 1 == n) return .1f;
      if (i + 2 == n && (next == 's' || next == 'd')) return .3f;
      if (isConsonantLetter(prev) && isConsonantLetter(next)) return .6f;  // some-thing, love-ly
      return 1.2f;
    case 'h':
      if (prev != '\0' && std::strchr("cgkprstw", prev)) return .4f;
      if (i == 0) return .8f;  // hour, honest
      if (i + 1 == n && isVowelLetter(prev)) return .3f;  // oh, ah
      return 1.5f;
    case 'g':
      if (next == 'h') return .3f;
      if (next == 'n') return .4f;
      break;
    case 'k':
      if (i == 0 && next == 'n') return .2f;
      break;
    case 'w':
      if (next == 'r' || next == 'h') return .3f;
      if (isVowelLetter(prev)) return .6f;
      break;
    case 'p':
      if (i == 0 && (next == 's' || next == 'n')) return .3f;
      break;
    case 'b':
      if (prev == 'm' || next == 't') return .3f;
      break;
    case 'l':
      if ((prev == 'a' || prev == 'o' || prev == 'u') && next != '\0' && std::strchr("kmfd", next)) return .4f;
      break;
    case 't':
      if ((prev == 's' || prev == 'f') && (next == 'l' || next == 'e' || next == 'm')) return .4f;
      break;
    case 'c':
      if ((prev == 's' || prev == 'x') && (next == 'e' || next == 'i' || next == 'y')) return .3f;
      break;
    case 'u':
      if (prev == 'g' || prev == 'q') return .3f;
      break;
    case 'n':
      if (prev == 'm' && i + 1 == n) return .4f;
      break;
    case 's':
      if (prev == 'i' && next == 'l') return .5f;
      break;
    case 'd':
      if (next == 'g') return .3f;
      break;
    default:
      break;
  }
  return isVowelLetter(c) ? 1.0f : 2.5f;
}

// Cost of a phoneme with no letters of its own (the schwa of "rhythm").
float insertedCost(const std::string& ph) {
  if (ph == "AX") return 2.5f;
  if (ph == "Y") return 1.5f;
  if (ph == "W" || ph == "HH") return 2.0f;
  return 5.0f;
}

constexpr float kNever = 1e30f;

// Letters [i, i+len) for a phoneme the table does not list for them.
float looseCost(const std::string& s, size_t i, size_t len, bool vowel) {
  bool allVowels = true;
  for (size_t t = i; t < i + len; ++t) allVowels = allVowels && isVowelLetter(s[t]);
  if (len == 1) {
    if (s[i] == '\'') return kNever;
    if (vowel) return allVowels ? 1.5f : 4.0f;
    return allVowels ? 4.0f : 3.0f;
  }
  return vowel && allVowels ? 1.8f + .3f * static_cast<float>(len - 2) : kNever;
}

struct Alignment {
  std::vector<size_t> groupStart, groupEnd;  // per letter: the grapheme it belongs to
  std::vector<int> first, last;              // per letter: phonemes its grapheme spells, -1 if silent
};

// Cheapest monotonic alignment of letters to phonemes: every letter is silent
// or part of one grapheme spelling one or two phonemes.
Alignment align(const std::string& s, const Pronunciation& p) {
  const size_t n = s.size(), m = p.size(), w = m + 1;
  std::vector<float> best((n + 1) * w, kNever);
  std::vector<unsigned char> stepLetters((n + 1) * w, 0), stepPhonemes((n + 1) * w, 0);
  const auto relax = [&](size_t i, size_t j, size_t di, size_t dj, float cost) {
    const size_t k = (i + di) * w + j + dj;
    if (cost < best[k]) {
      best[k] = cost;
      stepLetters[k] = static_cast<unsigned char>(di);
      stepPhonemes[k] = static_cast<unsigned char>(dj);
    }
  };
  const auto& table = spellings();
  best[0] = 0;
  for (size_t i = 0; i <= n; ++i) {
    for (size_t j = 0; j <= m; ++j) {
      const float c = best[i * w + j];
      if (c >= kNever) continue;
      if (i < n) relax(i, j, 1, 0, c + silentCost(s, i));
      if (j == m) continue;
      const std::string& ph = p[j].symbol;
      relax(i, j, 0, 1, c + insertedCost(ph));
      if (i == n) continue;
      if (const auto it = table.find(ph); it != table.end()) {
        for (const auto& sp : it->second) {
          if (!sp.second.empty() && (j + 1 == m || p[j + 1].symbol != sp.second)) continue;
          if (s.compare(i, sp.letters.size(), sp.letters) != 0) continue;
          relax(i, j, sp.letters.size(), sp.second.empty() ? 1 : 2, c + sp.cost);
        }
      }
      const bool vowel = isVowelSymbol(ph);
      for (size_t len = 1; len <= 3 && i + len <= n; ++len) {
        const float loose = looseCost(s, i, len, vowel);
        if (loose < kNever) relax(i, j, len, 1, c + loose);
      }
    }
  }
  Alignment a;
  a.groupStart.assign(n, 0);
  a.groupEnd.assign(n, 0);
  a.first.assign(n, -1);
  a.last.assign(n, -1);
  for (size_t i = n, j = m; i > 0 || j > 0;) {
    const size_t k = i * w + j, di = stepLetters[k], dj = stepPhonemes[k];
    for (size_t t = i - di; t < i; ++t) {
      a.groupStart[t] = i - di;
      a.groupEnd[t] = i;
      if (dj > 0) {
        a.first[t] = static_cast<int>(j - dj);
        a.last[t] = static_cast<int>(j - 1);
      }
    }
    i -= di;
    j -= dj;
  }
  return a;
}

// ---------------------------------------------------------------------------
// Syllables of one letter run (a word, or one part of a hyphenated compound)

struct Run {
  std::string letters;                 // lowercase letters and apostrophes
  Pronunciation pron;
  bool guessed = false;                // pronunciation from spelling rules: no stress
  std::vector<size_t> vowel;           // phoneme index of each nucleus
  std::vector<size_t> begin;           // phoneme index where each syllable starts
  std::vector<int> stress;
  std::vector<bool> optional, mergesForward;
  std::vector<size_t> cut;             // letter index where each piece after the first starts
  std::vector<float> keep;             // how strongly each cut resists merging
};

// Maximal onset as the planner splits words, except that "ng" never starts a
// syllable in English (sing-ing, fin-ger).
size_t onsetStart(const Pronunciation& p, size_t prevVowel, size_t nextVowel) {
  size_t split = nextVowel;
  while (split > prevVowel + 1 && p[split - 1].symbol != "NG" && legalOnset(p, split - 1, nextVowel)) --split;
  return split;
}

void syllabifyPhonemes(Run& r) {
  const Pronunciation& p = r.pron;
  for (size_t i = 0; i < p.size(); ++i)
    if (isVowelSymbol(p[i].symbol)) r.vowel.push_back(i);
  const size_t count = r.vowel.size();
  r.begin.assign(count, 0);
  for (size_t k = 1; k < count; ++k) r.begin[k] = onsetStart(p, r.vowel[k - 1], r.vowel[k]);
  for (size_t v : r.vowel) r.stress.push_back(std::max(0, p[v].stress));
  r.optional.assign(count, false);
  r.mergesForward.assign(count, false);
  if (r.guessed) return;  // no stress to go by
  for (size_t k = 0; k < count; ++k) {
    if (r.stress[k] != 0) continue;
    const std::string& v = p[r.vowel[k]].symbol;
    const bool vowelNext = k + 1 < count && r.vowel[k + 1] == r.vowel[k] + 1;
    // Syncope after the stress: ev(er)y, fam(i)ly, choc(o)late, diff(er)ent.
    if (k > 0 && k + 1 < count && r.stress[k - 1] >= 1 && (v == "AX" || v == "IH" || v == "ER")) {
      const size_t next = r.begin[k + 1];
      const bool liquidNext = next == r.vowel[k] + 1 && next < r.vowel[k + 1] &&
                              oneOf(p[next].symbol, {"L", "R", "N"});
      if (v == "ER" ? vowelNext : liquidNext) r.optional[k] = r.mergesForward[k] = true;
    }
    // An unstressed i/y vowel glides into the next unstressed vowel: se-ri-ous, In-di-an.
    if (vowelNext && (v == "IY" || v == "IH") && r.stress[k + 1] == 0) r.optional[k] = r.mergesForward[k] = true;
    // The r-vowel after a diphthong: fire, hour, flower, tired.
    if (k > 0 && v == "ER" && r.vowel[k] == r.vowel[k - 1] + 1 &&
        oneOf(p[r.vowel[k - 1]].symbol, {"AY", "AW", "OY"})) {
      r.optional[k] = true;
      r.mergesForward[k] = false;
    }
  }
}

// Short (checked) vowels close a stressed syllable with the next consonant, as
// dictionaries hyphenate: riv-er, mag-ic, cov-er, bod-y. "a", "e" and "u"
// spellings before r sound long and stay open (Ma-ry, he-ro, ju-ry).
bool shortVowel(const std::string& v, const std::string& letters, bool beforeR) {
  const char c = letters.empty() ? '\0' : letters[0];
  if (v == "AE" || v == "AH") return true;
  if (v == "EH") return !(beforeR && c == 'a');
  if (v == "IH") return !(beforeR && c == 'e');
  if (v == "UH") return !(beforeR && c == 'u');
  if (v == "AA") return c == 'o';  // bod-y, ton-ic; fa-ther, dra-ma
  return false;
}

// "-tion", "-sion", "-cial", "-gion": an sh/zh/ch/jh sound written with a
// following i stays with its syllable (vi-sion, spe-cial, re-li-gion).
bool softOnset(const Alignment& a, const std::string& s, const Pronunciation& p, size_t consonant,
               size_t from, size_t to) {
  if (!oneOf(p[consonant].symbol, {"SH", "ZH", "CH", "JH"})) return false;
  for (size_t t = from; t < to; ++t) {
    if (a.first[t] != static_cast<int>(consonant)) continue;
    const size_t end = a.groupEnd[t];
    if (s[end - 1] == 'i') return true;
    return end + 1 < s.size() && s[end] == 'i' && isVowelLetter(s[end + 1]);
  }
  return false;
}

// Letter where the syllable starting at phoneme `sp` begins, among the letters
// [from, to) between two nuclei. Silent letters stay with the syllable before
// (some-thing, Christ-mas), except a silent t or h right before the next vowel
// (lis-ten, of-ten, ve-hi-cle).
size_t letterCut(const Alignment& a, const std::string& s, size_t sp, size_t from, size_t to) {
  size_t cut = to;
  for (size_t t = from; t < to; ++t) {
    if (a.first[t] < 0) continue;
    if (a.first[t] >= static_cast<int>(sp)) {
      cut = a.groupStart[t];
      break;
    }
    if (a.last[t] >= static_cast<int>(sp)) {  // one grapheme on both sides: "qu" goes on, "x" stays
      cut = s[a.groupStart[t]] == 'q' ? a.groupStart[t] : a.groupEnd[t];
      break;
    }
  }
  if (to > from && a.first[to - 1] < 0 && (s[to - 1] == 't' || s[to - 1] == 'h')) cut = to - 1;
  return cut;
}

// Whether head + tail sound like the word: the same consonants (th of either
// voicing) and vowels, except where the word reduces a vowel (for-ev-er has
// F ER0 for "for", up-on has AX for "up"). An r-vowel counts as vowel + r.
bool soundsLike(const Pronunciation& word, const Pronunciation& head, const Pronunciation& tail) {
  struct Sound {
    std::string symbol;
    bool vowel, reduced;
  };
  const auto sounds = [](const Pronunciation& p, std::vector<Sound>& out) {
    for (const auto& t : p) {
      if (!isVowelSymbol(t.symbol)) {
        out.push_back({t.symbol == "DH" ? "TH" : t.symbol, false, false});
        continue;
      }
      const bool reduced = t.stress == 0 && oneOf(t.symbol, {"AX", "IH", "ER"});
      out.push_back({t.symbol == "AX" ? "AH" : t.symbol, true, reduced});
      if (t.symbol == "ER") out.push_back({"R", false, false});
    }
  };
  std::vector<Sound> w, parts;
  sounds(word, w);
  sounds(head, parts);
  sounds(tail, parts);
  if (w.size() != parts.size()) return false;
  for (size_t i = 0; i < w.size(); ++i) {
    if (w[i].vowel != parts[i].vowel) return false;
    if (w[i].symbol != parts[i].symbol && !(w[i].vowel && w[i].reduced)) return false;
  }
  return true;
}

size_t vowelCount(const Pronunciation& p) {
  size_t n = 0;
  for (const auto& t : p) n += isVowelSymbol(t.symbol) ? 1 : 0;
  return n;
}

// A compound of two dictionary words (with-out, for-ev-er, wher-ev-er,
// base-line): the letter index of the boundary, or 0. The dictionary is full of
// names and abbreviations ("erica", "lings", "ast"), so short parts must be
// real two-letter words, a head whose vowel the word reduces must be a function
// word (with-out, not ast-ray), and a vowel-initial second part must be a common
// compound ending that carries stress (not Am-erica, butt-on). A head may lend
// a silent e ("where").
size_t firstCompoundBoundary(const Run& r, const Lexicon& lex) {
  static const char* const kTwoLetterWords[] = {"an", "as", "at", "be", "by", "do", "go", "he",
                                                "if", "in", "is", "it", "me", "my", "no", "of",
                                                "on", "or", "so", "to", "up", "us", "we"};
  const auto wordLike = [](const std::string& w) {
    if (w.size() >= 3) return true;
    for (const char* two : kTwoLetterWords)
      if (w == two) return true;
    return false;
  };
  const std::string& s = r.letters;
  if (s.size() < 4 || r.vowel.size() < 2 || s.find('\'') != std::string::npos) return 0;
  for (size_t b = 2; b + 2 <= s.size(); ++b) {
    const std::string tail = s.substr(b);
    if (!wordLike(tail)) continue;
    if (isVowelLetter(tail[0]) && !oneOf(tail, {"out", "over", "ever", "off", "on", "in", "up", "upon", "all",
                                                "away", "else", "into", "about", "around", "along", "any",
                                                "every", "one", "own", "end", "ends", "eye", "eyes"}))
      continue;
    if (tail.size() == 2 && isVowelLetter(tail[0]) &&
        !oneOf(s.substr(0, b), {"with", "up", "there", "here", "where", "for", "be"}))
      continue;  // up-on, with-in; not vi-ol-in, sal-on
    bool knownTail = false;
    const Pronunciation pt = lex.lookup(tail, &knownTail);
    if (!knownTail) continue;
    for (const std::string& head : {s.substr(0, b), s.substr(0, b) + "e"}) {
      if ((head.size() > b && !isVowelLetter(tail[0])) || !wordLike(head)) continue;  // lent e only before a vowel
      bool knownHead = false;
      const Pronunciation ph = lex.lookup(head, &knownHead);
      if (!knownHead || !soundsLike(r.pron, ph, pt)) continue;
      const size_t headVowels = vowelCount(ph);
      if (headVowels == 0 || headVowels >= r.vowel.size()) continue;
      const auto& firstVowel = r.pron[r.vowel[0]];
      if (headVowels == 1 && firstVowel.stress == 0 && oneOf(firstVowel.symbol, {"AX", "IH", "ER"}) &&
          !oneOf(head, {"for", "with", "up", "to", "in", "on", "be", "an", "and", "of", "at", "by", "from",
                        "out", "there", "here", "where", "some", "any", "no", "through", "under", "over"}))
        continue;
      if (isVowelSymbol(pt.front().symbol) && r.stress[headVowels] == 0) continue;
      return b;
    }
  }
  return 0;
}

// A plural or -s verb whose own first match comes later than its stem's took
// a spurious part the stem does not have ("hundreds": "dreds" is no word, so
// "hund" + "reds" matched); it keeps the stem's boundary (hun-dred, hun-dreds).
size_t compoundBoundary(const Run& r, const Lexicon& lex) {
  const size_t own = firstCompoundBoundary(r, lex);
  if (own == 0 || r.letters.back() != 's' || r.pron.empty() ||
      (r.pron.back().symbol != "S" && r.pron.back().symbol != "Z"))
    return own;
  Run stem = r;
  stem.letters.pop_back();
  stem.pron.pop_back();
  bool known = false;
  const Pronunciation p = lex.lookup(stem.letters, &known);
  bool same = known && p.size() == stem.pron.size();
  for (size_t i = 0; same && i < p.size(); ++i) same = p[i].symbol == stem.pron[i].symbol;
  const size_t stemBoundary = same ? firstCompoundBoundary(stem, lex) : 0;
  return stemBoundary > 0 && stemBoundary < own ? stemBoundary : own;
}

// Whether "-ly" follows a whole stem (nice-ly, deep-ly, quick-ly, sharp-ly)
// rather than a vowel (ho-ly, fam-i-ly), a "-ble"/"-ple" word (prob-a-bly,
// sim-ply) or a cluster the onset rules already split (gent-ly, most-ly).
bool wholeStemBeforeLy(const std::string& s, size_t at, const Alignment& a) {
  const char last = s[at - 1], before = s[at - 2];
  if (last == 'e') return a.first[at - 1] < 0;
  if (!isConsonantLetter(last)) return false;
  if (isVowelLetter(before)) return !((before == 'a' || before == 'i') && last == 'b');
  const bool digraph = (before == 'c' && last == 'k') || (last == 'h' && std::strchr("cgpstw", before));
  return digraph || before == 'r' || before == 'l';
}

// Cuts for each syllable boundary of the run, lyric-sheet style.
void placeCuts(Run& r, const Lexicon* lex) {
  const std::string& s = r.letters;
  const Pronunciation& p = r.pron;
  const size_t n = s.size(), count = r.vowel.size();
  r.cut.clear();
  r.keep.clear();
  if (count < 2) return;
  if (n < count) {  // fewer letters than syllables ("w"): one letter per piece
    for (size_t i = 1; i < n; ++i) {
      r.cut.push_back(i);
      r.keep.push_back(10);
    }
    return;
  }
  // A run far longer than any word (the dictionary's longest has 28 letters) is
  // not one, and the alignment is quadratic in letters (a 20,000-letter token
  // would need ~2.4 GB): cut it evenly instead.
  constexpr size_t kMaxAlignedLetters = 64;
  if (n > kMaxAlignedLetters) {
    for (size_t g = 1; g < count; ++g) {
      r.cut.push_back(g * n / count);
      r.keep.push_back(10);
    }
    return;
  }
  const Alignment a = align(s, p);

  // Letters spelling each nucleus, [lo, hi); a vowel without letters sits
  // before the next spelled phoneme.
  std::vector<size_t> lo(count), hi(count);
  for (size_t k = 0; k < count; ++k) {
    const int v = static_cast<int>(r.vowel[k]);
    size_t from = n, to = 0;
    for (size_t t = 0; t < n; ++t) {
      if (a.first[t] >= 0 && a.first[t] <= v && v <= a.last[t]) {
        from = std::min(from, t);
        to = t + 1;
      }
    }
    if (from >= to) {
      from = n;
      for (size_t t = 0; t < n; ++t) {
        if (a.first[t] > v) {
          from = t;
          break;
        }
      }
      to = from;
    }
    lo[k] = from;
    hi[k] = to;
  }

  // Prefixes keep their spelling when unstressed: re-spect, de-stroy, un-a-ble, dis-ease.
  size_t prefix = 0;
  bool openPrefix = false;
  {
    struct Prefix {
      const char* letters;
      bool open;  // ends in its vowel
    };
    static const Prefix kPrefixes[] = {{"pre", true}, {"re", true}, {"de", true}, {"be", true},
                                       {"dis", false}, {"mis", false}, {"un", false}};
    for (const auto& pf : kPrefixes) {
      const size_t len = std::strlen(pf.letters);
      if (n < len + 2 || s.compare(0, len, pf.letters) != 0) continue;
      const std::string& v = p[r.vowel[0]].symbol;
      bool ok = hi[0] <= len && lo[1] >= len && a.groupStart[len] == len &&
                !(isConsonantLetter(s[len]) && s[len] == s[len + 1]);  // des-sert
      if (pf.open) ok = ok && (r.guessed || r.stress[0] == 0 || v == "IY");
      else if (pf.letters[0] == 'u') ok = ok && (v == "AH" || v == "AX");
      else ok = ok && (v == "IH" || v == "AX");
      if (ok) {
        prefix = len;
        openPrefix = pf.open;
      }
      break;
    }
  }

  const size_t compound = lex && !r.guessed ? compoundBoundary(r, *lex) : 0;
  size_t compoundGap = count;
  if (compound > 0) {
    size_t vowelsBefore = 0;
    for (size_t k = 0; k < count; ++k) vowelsBefore += hi[k] <= compound ? 1 : 0;
    if (vowelsBefore >= 1 && vowelsBefore < count && lo[vowelsBefore] >= compound) compoundGap = vowelsBefore - 1;
  }

  const bool ingEnding = (endsWith(s, "ing") || endsWith(s, "ings") || endsWith(s, "in'")) &&
                         p[r.vowel.back()].symbol == "IH" && (r.guessed || r.stress.back() == 0) &&
                         r.vowel.back() + 1 < p.size() &&
                         (p[r.vowel.back() + 1].symbol == "NG" || p[r.vowel.back() + 1].symbol == "N");

  bool closedByGuess = false;  // spelling-only rule alternates: tel-e-phone, dem-o-dex
  for (size_t g = 0; g + 1 < count; ++g) {
    const size_t va = r.vowel[g], vb = r.vowel[g + 1], consonants = vb - va - 1;
    const std::string& v = p[va].symbol;
    const std::string nucleus = s.substr(lo[g], hi[g] - lo[g]);
    size_t sp = r.begin[g + 1];
    bool closed = false;
    if (sp == va + 1 && consonants > 0 && !(g == 0 && openPrefix)) {
      if (r.guessed) {
        // Without stress, single e/i/y before one consonant read short (lic-u, riv-er);
        // a, o, u read long (ba-by, o-pen, mu-sic).
        closed = !closedByGuess && consonants == 1 && (nucleus == "e" || nucleus == "i" || nucleus == "y") &&
                 (v == "EH" || v == "IH");
      } else {
        closed = r.stress[g] >= 1 && shortVowel(v, nucleus, p[va + 1].symbol == "R") &&
                 !(consonants == 1 && softOnset(a, s, p, va + 1, hi[g], lo[g + 1]));
      }
      // An s cluster splits after the s (mis-ter, nos-tal-gia, Eas-ter, hon-es-ty), but
      // not after a word-initial schwa (a-stray) or an r-vowel (per-spec-tive).
      if (closed || (consonants >= 2 && p[va + 1].symbol == "S" && !(v == "AX" && g == 0) && v != "ER"))
        sp = va + 2;
    }
    closedByGuess = r.guessed && closed;
    // -ing keeps one consonant with a stem spelled with a vowel team or a short
    // vowel (feel-ing, poop-ing, wish-ing); a long single vowel lost its silent
    // e and reads open (ta-king, smi-ling). "nk" stays whole (think-ing).
    if (ingEnding && g + 2 == count) {
      const bool beforeR = consonants > 0 && p[va + 1].symbol == "R";
      const bool stem = nucleus.size() > 1 || (r.stress[g] >= 1 && shortVowel(v, nucleus, beforeR));
      if ((consonants == 1 && stem) || (consonants == 2 && p[va + 1].symbol == "NG" && p[va + 2].symbol == "K"))
        sp = vb;
    }
    size_t cut = letterCut(a, s, sp, hi[g], lo[g + 1]);

    // A vowel letter the pronunciation elides belongs to the next piece:
    // fam-ily, fav-orite, ev-ery, choc-olate (silent e only before r: some-thing).
    for (size_t t = hi[g]; t < lo[g + 1]; ++t) {
      const bool elided = a.first[t] < 0 && (s[t] == 'a' || s[t] == 'i' || s[t] == 'o' || s[t] == 'u' ||
                                             (s[t] == 'e' && t + 1 < n && s[t + 1] == 'r'));
      if (!elided) continue;
      bool coda = false;
      for (size_t u = hi[g]; u < cut; ++u) coda = coda || a.first[u] >= 0;
      if (t < cut || !coda) cut = t;
      break;
    }

    // An r-vowel before another vowel lends its r to it (a-round, mem-o-ry),
    // except unstressed "-er-" (ev-er-y, cov-er-ing).
    if (v == "ER" && consonants == 0 && hi[g] > lo[g] + 1 && s[hi[g] - 1] == 'r') {
      size_t rs = hi[g] - 1;
      while (rs > lo[g] && s[rs - 1] == 'r') --rs;
      if (r.stress[g + 1] >= 1 || s.compare(lo[g], rs - lo[g], "e") != 0) cut = rs;
    }

    // Doubled consonants split (hap-py, hel-lo, fal-ling, ad-dress) when the
    // second can start the syllable; otherwise both stay behind (pass-word).
    size_t q = hi[g];
    while (q > lo[g] && !isVowelLetter(s[q - 1])) --q;
    for (size_t t = q; t + 1 < lo[g + 1]; ++t) {
      if (!isConsonantLetter(s[t]) || s[t] != s[t + 1]) continue;
      const char after = t + 2 < n ? s[t + 2] : '\0';
      if (t + 2 == lo[g + 1] || after == 'r' || after == 'l' || after == 'h' || after == 'i') cut = t + 1;
      else cut = std::max(cut, t + 2);
      break;
    }
    // "ck" and "x" never start a piece (pock-et, tax-i); without an alignment we
    // can trust, neither does the second letter of a digraph (laugh-ing).
    if (s.compare(cut, 2, "ck") == 0) cut += 2;
    else if (cut > 0 && s[cut - 1] == 'c' && s[cut] == 'k') cut += 1;
    else if (s[cut] == 'x' && cut + 1 < lo[g + 1]) cut += 1;
    else if (r.guessed && cut > 0 && s[cut] == 'h' && std::strchr("tscpgw", s[cut - 1])) cut += 1;

    // A compound boundary only ever keeps a head's consonants with it
    // (wi-thout -> with-out), and an unstressed prefix outranks it (re-mem-ber).
    if (g == compoundGap && compound > cut && !(g == 0 && prefix > 0)) {
      cut = compound;
      closedByGuess = false;
    } else if (g == compoundGap) {
      compoundGap = count;
    }
    r.cut.push_back(cut);

    float keep = 10;
    if (r.stress[g + 1] >= 1) keep += 2;
    if (consonants == 0) keep -= 3;
    if ((r.optional[g] && r.mergesForward[g]) || (r.optional[g + 1] && !r.mergesForward[g + 1])) keep = 2;
    if (g == compoundGap) keep = 30;
    r.keep.push_back(keep);
  }

  // Consonant + "le" starts the last piece: ta-ble, peo-ple, whis-tle, mus-cle;
  // a digraph stays whole (tick-le).
  {
    const size_t l = lo[count - 1];
    if (l >= 2 && hi[count - 1] == l + 2 && s.compare(l, 2, "le") == 0 && isConsonantLetter(s[l - 1]) &&
        count - 2 != compoundGap) {
      const size_t g0 = a.groupStart[l - 1];
      const std::string grapheme = s.substr(g0, a.groupEnd[l - 1] - g0);
      if (grapheme.size() == 1 || s[l - 2] == s[l - 1] || oneOf(grapheme, {"sc", "st", "bt"}))
        r.cut[count - 2] = l - 1;
    }
  }
  if (prefix > 0 && compoundGap != 0) r.cut[0] = prefix;

  // Consonant suffixes keep their spelling: harm-less, love-ly, beau-ti-ful, base-ment.
  static const char* const kSuffixes[] = {"less", "ness", "ful", "ments", "ment", "ly",
                                          "ship", "hood", "some", "dom"};
  for (const char* suffix : kSuffixes) {
    if (!endsWith(s, suffix)) continue;
    const size_t at = n - std::strlen(suffix);
    if (at < 2 || lo[count - 1] < at || hi[count - 2] > at || (!r.guessed && r.stress.back() != 0) ||
        count - 2 == compoundGap)
      break;
    if (std::strcmp(suffix, "ly") == 0 && !wholeStemBeforeLy(s, at, a)) break;
    r.cut[count - 2] = at;
    break;
  }

  // Keep the cuts increasing with a letter in every piece.
  for (size_t g = 0; g < r.cut.size(); ++g) {
    const size_t least = g == 0 ? 1 : r.cut[g - 1] + 1;
    const size_t most = n - (r.cut.size() - g);
    r.cut[g] = std::min(std::max(r.cut[g], least), most);
  }
}

// Letter names, for spelled-out acronyms.
const char* const kLetterNames[26] = {
    "EY1", "B IY1", "S IY1", "D IY1", "IY1", "EH1 F", "JH IY1", "EY1 CH", "AY1", "JH EY1", "K EY1", "EH1 L", "EH1 M",
    "EH1 N", "OW1", "P IY1", "K Y UW1", "AA1 R", "EH1 S", "T IY1", "Y UW1", "V IY1", "", "EH1 K S", "W AY1", "Z IY1"};

// The dictionary reads these letters by their names (its abbreviation entries:
// "ba" = B.A., "baa" = B.A.A.).
bool spelledOut(const std::string& letters, const Pronunciation& pron) {
  size_t at = 0;
  for (char c : letters) {
    if (c < 'a' || c > 'z') return false;
    Pronunciation name;
    Lexicon::parseArpabet(kLetterNames[c - 'a'], name);
    for (const auto& p : name)
      if (at >= pron.size() || pron[at++].symbol != p.symbol) return false;
  }
  return at == pron.size();
}

// The spelling with every run of three or more equal letters, and every "aa",
// shortened to `keep` letters: how a held sound is written out ("nooo",
// "ooooh", "heyyy", "aah").
std::string shortenRuns(const std::string& s, size_t keep) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    while (j < s.size() && s[j] == s[i]) ++j;
    const bool held = j - i >= 3 || (s[i] == 'a' && j - i == 2);
    out.append(held ? std::min(keep, j - i) : j - i, s[i]);
    i = j;
  }
  return out;
}

Run analyze(const std::string& letters, const Lexicon* lex) {
  Run r;
  r.letters = letters;
  bool known = false;
  r.pron = lex ? lex->lookup(letters, &known) : guessPronunciation(Lexicon::normalize(letters));
  // Sung lyrics are words: a held vowel written out is still one syllable, and
  // nobody sings "ba" as B.A. (acronyms written in capitals never get here).
  const bool abbreviation = lex && known && !letters.empty() && isVowelLetter(letters.back()) && spelledOut(letters, r.pron);
  if (lex && (!known || abbreviation)) {
    bool found = false;
    for (size_t keep : {2, 1}) {
      const std::string shorter = shortenRuns(letters, keep);
      if (shorter == letters) continue;
      bool hit = false;
      Pronunciation p = lex->lookup(shorter, &hit);
      if (hit && !spelledOut(shorter, p)) {
        r.pron = p;
        found = true;
        break;
      }
    }
    if (found) {
      known = true;
    } else if (abbreviation || shortenRuns(letters, 1) != letters) {
      r.pron = guessPronunciation(Lexicon::normalize(shortenRuns(letters, 1)));
      known = false;
    }
  }
  if (lex && endsWith(letters, "in'")) {  // goin', lovin': the -ing word sung with n (the
    bool knownIng = false;                // dictionary's "goin" is a surname)
    Pronunciation ing = lex->lookup(letters.substr(0, letters.size() - 1) + "g", &knownIng);
    if (knownIng && ing.size() >= 2 && ing.back().symbol == "NG") {
      ing.back().symbol = "N";
      r.pron = ing;
      known = true;
    }
  }
  r.guessed = !known;
  syllabifyPhonemes(r);
  placeCuts(r, lex);
  return r;
}

// ---------------------------------------------------------------------------
// Tokens: letter runs, acronyms, numbers

struct Letter {
  char c;          // lowercase ASCII letter it reads as ("e" for "É"), or apostrophe
  size_t at;       // byte offset in the token
  size_t len = 1;  // bytes as written (an accented letter is 2-3 UTF-8 bytes)
};
using LetterRun = std::vector<Letter>;

// Letters and apostrophes (straight or curly) of a token, split into runs at
// any other character ("twenty-seven" -> "twenty", "seven"). Other characters
// are stepped over whole, so no run starts or ends inside one.
std::vector<LetterRun> letterRuns(const std::string& word) {
  std::vector<LetterRun> runs(1);
  for (size_t i = 0; i < word.size();) {
    char c = 0;
    const size_t len = spellingCharAt(word, i, &c);
    if (len > 0) {
      runs.back().push_back({c, i, c == '\'' ? 1 : len});  // a curly apostrophe is written straight
      i += len;
      continue;
    }
    if (!runs.back().empty()) runs.emplace_back();
    size_t skip = 1;
    utf8Decode(word, i, skip);
    i += skip;
  }
  runs.erase(std::remove_if(runs.begin(), runs.end(),
                            [](const LetterRun& r) {
                              return std::all_of(r.begin(), r.end(), [](const Letter& l) { return l.c == '\''; });
                            }),
             runs.end());
  return runs;
}

std::string lowercase(const LetterRun& run) {
  std::string s;
  for (const auto& l : run) s += l.c;
  return s;
}

// The run as written (case kept, curly apostrophes straightened), and in `at`
// the byte offset of each of its letters plus its length: letter cuts become
// byte cuts there, never inside an accented letter.
std::string display(const std::string& word, const LetterRun& run, std::vector<size_t>& at) {
  std::string s;
  at.clear();
  for (const auto& l : run) {
    at.push_back(s.size());
    if (l.c == '\'') s += '\'';
    else s.append(word, l.at, l.len);
  }
  at.push_back(s.size());
  return s;
}

// "AI", "US", "TV", "DJ": an all-caps token of 2-3 letters that is not a common
// word sung loudly. W (three syllables) is left to the dictionary.
bool isAcronym(const std::string& word, const LetterRun& run) {
  static const char* const kWords[] = {
      "oh", "no", "so", "go", "hey", "yes", "yea", "la", "na", "da", "ooh", "ah", "uh", "ha", "my", "me",
      "we", "be", "do", "up", "on", "in", "it", "is", "to", "of", "or", "an", "am", "at", "as", "by",
      "he", "if", "and", "the", "you", "for", "but", "not", "all", "can", "her", "was", "one", "our",
      "out", "day", "get", "has", "him", "his", "how", "man", "new", "now", "old", "see", "two", "who",
      "boy", "did", "its", "let", "put", "say", "she", "too", "yo", "ya", "ow", "aw", "oo", "mm", "hmm",
      "shh", "ok", "are", "any", "end", "far", "fly", "god", "got", "hot", "may", "off", "own",
      "run", "sad", "sky", "try", "why", "yet", "die", "cry", "bye", "eye", "lie", "low", "mad", "war"};
  if (run.size() < 2 || run.size() > 3) return false;
  for (const auto& l : run) {
    const char c = word[l.at];
    if (c < 'A' || c > 'Z' || c == 'W') return false;
  }
  const std::string lower = lowercase(run);
  for (const char* w : kWords)
    if (lower == w) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Numbers and symbols read out
//
// Speech recognisers write numbers as digits ("100", "2027", "1st", "'90s",
// "$5", "3:30"), but a singer sings words, and the notes must show those
// words' syllables ("one", "hun-", "dred"). The reading is the plain sung
// style: no "and" inside a number ("105" one hundred five).
//   Cardinals up to 999,999,999: "1,000" one thousand, "250000" two hundred
//     fifty thousand, "0" zero. Longer digit strings, and codes with a leading
//     zero ("007" oh oh seven), digit by digit.
//   Years: four digits 1100-2099 written without a comma are read pair by
//     pair ("1999" nineteen ninety-nine, "1900" nineteen hundred, "1905"
//     nineteen oh five, "2027" twenty twenty-seven), except round thousands
//     and 2001-2009, which are sung as cardinals ("2000" two thousand, "2005"
//     two thousand five).
//   Ordinals: "1st" first, "21st" twenty-first, "100th" one hundredth
//     (cardinal reading; nobody sings "1999th" as a year).
//   Decades and plurals: "1990s" / "1990's" nineteen nineties, "'90s"
//     nineties, "1900s" nineteen hundreds.
//   Decimals: "2.5" two point five, ".5" point five (digits after the point
//     one by one). Times: "3:30" three thirty, "3:05" three oh five, "3:00"
//     three o'clock.
//   Symbols: "$5" five dollars, "$1" one dollar, "$5.50" five dollars fifty
//     cents (also pounds and euros), "50%" fifty percent, "#1" number one,
//     "&" and.
//   Letters right after a number: am/pm, a single letter and 2-3 capitals are
//     said by their names ("3pm" three P M, "3D" three D); other letters
//     extend the last number word into one word ("49er" forty-niner, "4ever"
//     four-ever; see joinSuffix).
// Each number word is its own sung word; a compound ("twenty-seven") is one.

const char* const kOnes[20] = {"zero",    "one",     "two",     "three",     "four",     "five",    "six",
                               "seven",   "eight",   "nine",    "ten",       "eleven",   "twelve",  "thirteen",
                               "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char* const kTens[10] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

// Longest digit string read as a cardinal (999,999,999); longer ones are codes
// or phone numbers, read digit by digit.
constexpr size_t kMaxCardinalDigits = 9;
// Four-digit numbers in this range (round thousands and 2001-2009 excepted)
// are plausible years and are read as years.
constexpr long long kFirstYear = 1100, kLastYear = 2099;

void underThousand(long long n, std::vector<std::string>& out) {
  if (n >= 100) {
    out.push_back(kOnes[n / 100]);
    out.push_back("hundred");
    n %= 100;
    if (n == 0) return;
  }
  if (n < 20) {
    out.push_back(kOnes[n]);
    return;
  }
  out.push_back(std::string(kTens[n / 10]) + (n % 10 ? std::string("-") + kOnes[n % 10] : ""));
}

void cardinal(long long n, std::vector<std::string>& out) {
  if (n == 0) {
    out.push_back("zero");
    return;
  }
  const long long millions = n / 1000000, thousands = n / 1000 % 1000, rest = n % 1000;
  if (millions > 0) {
    underThousand(millions, out);
    out.push_back("million");
  }
  if (thousands > 0) {
    underThousand(thousands, out);
    out.push_back("thousand");
  }
  if (rest > 0) underThousand(rest, out);
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

void digitByDigit(const std::string& digits, const char* zero, std::vector<std::string>& out) {
  for (char c : digits) out.push_back(c == '0' ? zero : kOnes[c - '0']);
}

// Words for a run of digits. `asYear`: a plausible year may be read as one.
void readInteger(const std::string& digits, bool asYear, std::vector<std::string>& out) {
  if (digits.empty()) return;
  if (digits.size() > kMaxCardinalDigits || (digits.size() > 1 && digits[0] == '0')) {
    digitByDigit(digits, "oh", out);
    return;
  }
  const long long n = std::stoll(digits);
  const bool year = asYear && digits.size() == 4 && n >= kFirstYear && n <= kLastYear && n % 1000 != 0 &&
                    !(n > 2000 && n < 2010);
  if (!year) {
    cardinal(n, out);
    return;
  }
  underThousand(n / 100, out);
  const long long rest = n % 100;
  if (rest == 0) {
    out.push_back("hundred");
  } else if (rest < 10) {
    out.push_back("oh");
    out.push_back(kOnes[rest]);
  } else {
    underThousand(rest, out);
  }
}

// The last part of a number word ("seven" of "twenty-seven") turned into an
// ordinal ("twenty-seventh") or a plural ("nineties").
void lastPart(const std::string& word, std::string& head, std::string& tail) {
  const size_t hyphen = word.rfind('-');
  head = hyphen == std::string::npos ? "" : word.substr(0, hyphen + 1);
  tail = hyphen == std::string::npos ? word : word.substr(hyphen + 1);
}

std::string ordinal(std::string word) {
  static const std::pair<const char*, const char*> kIrregular[] = {
      {"one", "first"}, {"two", "second"}, {"three", "third"}, {"five", "fifth"},
      {"eight", "eighth"}, {"nine", "ninth"}, {"twelve", "twelfth"}};
  std::string head, tail;
  lastPart(word, head, tail);
  for (const auto& [cardinalWord, ordinalWord] : kIrregular)
    if (tail == cardinalWord) return head + ordinalWord;
  if (endsWith(tail, "y")) return head + tail.substr(0, tail.size() - 1) + "ieth";  // twentieth
  return head + tail + "th";
}

std::string plural(std::string word) {
  std::string head, tail;
  lastPart(word, head, tail);
  if (endsWith(tail, "y")) return head + tail.substr(0, tail.size() - 1) + "ies";  // nineties
  if (endsWith(tail, "s") || endsWith(tail, "x")) return head + tail + "es";        // sixes
  return head + tail + "s";
}

// Letters written onto a number word, as one word: the joined spelling when
// the dictionary knows it, with the silent e of the number dropped before a
// vowel as English suffixes do ("nine" + "er" -> "niner"); otherwise the two
// kept apart by a hyphen ("four" + "ever" -> "four-ever"), so each is read
// as it is spelled and they still sing as one word. `written` is the suffix
// as written, `reading` its letters as lowercase ASCII (see spellingCharAt).
std::string joinSuffix(const std::string& word, const std::string& written, const std::string& reading,
                       const Lexicon& lexicon) {
  std::string head, tail;
  lastPart(word, head, tail);
  std::vector<std::string> stems;
  if (endsWith(tail, "e") && !reading.empty() && isVowelLetter(reading[0])) stems.push_back(tail.substr(0, tail.size() - 1));
  stems.push_back(tail);
  for (const auto& stem : stems) {
    bool known = false;
    lexicon.lookup(stem + reading, &known);
    if (known) return head + stem + written;
  }
  return word + "-" + written;
}

// A word sung for part of a token.
struct SpokenWord {
  std::string text;          // number words in lowercase, a compound hyphenated ("twenty-seven"); or letters as written
  bool letterNames = false;  // say each letter by its name ("pm" of "3pm", "D" of "3D")
};

// The currency symbol at s[i]: its byte length (0 for none) and in `unit` the
// singular name of the currency.
size_t currencyAt(const std::string& s, size_t i, const char** unit) {
  const auto at = [&](const char* symbol) { return s.compare(i, std::strlen(symbol), symbol) == 0; };
  if (at("$")) return *unit = "dollar", 1;
  if (at("\xC2\xA3")) return *unit = "pound", 2;     // pound sign
  if (at("\xE2\x82\xAC")) return *unit = "euro", 3;  // euro sign
  return 0;
}

// Tokens read through spokenWords rather than spelled: they hold a digit or a
// symbol that is sung as a word.
bool needsReading(const std::string& word) {
  return std::any_of(word.begin(), word.end(), [](char c) { return isAsciiDigit(c) || c == '&' || c == '%'; });
}

// Whether a number starts at s[i]: a digit, or a decimal point before one
// that follows no digit (".5").
bool numberAt(const std::string& s, size_t i) {
  if (i >= s.size()) return false;
  if (isAsciiDigit(s[i])) return true;
  return s[i] == '.' && i + 1 < s.size() && isAsciiDigit(s[i + 1]) && (i == 0 || !isAsciiDigit(s[i - 1]));
}

// Byte length of the apostrophe (straight or curly) at s[i], else 0.
size_t apostropheAt(const std::string& s, size_t i) { return s[i] == '\'' ? 1 : curlyApostropheAt(s, i); }

// The words a singer sings for a token with digits or symbols (see the rules
// above), in order.
std::vector<SpokenWord> spokenWords(const std::string& s, const Lexicon& lexicon) {
  std::vector<SpokenWord> out;
  const char* unit = nullptr;  // the currency written before this number
  bool numberSign = false;     // a '#' written before this number
  for (size_t i = 0; i < s.size();) {
    if (numberAt(s, i)) {
      // The number: digits with thousands separators (a comma before exactly
      // three digits), then a decimal fraction or clock minutes.
      std::string whole, fraction;
      std::vector<std::string> clock;
      bool grouped = false, decimal = false;
      size_t j = i;
      while (j < s.size() && isAsciiDigit(s[j])) whole += s[j++];
      while (!whole.empty() && j + 3 < s.size() && s[j] == ',' && isAsciiDigit(s[j + 1]) && isAsciiDigit(s[j + 2]) &&
             isAsciiDigit(s[j + 3]) && (j + 4 >= s.size() || !isAsciiDigit(s[j + 4]))) {
        whole.append(s, j + 1, 3);
        grouped = true;
        j += 4;
      }
      if (j + 1 < s.size() && s[j] == '.' && isAsciiDigit(s[j + 1])) {
        decimal = true;
        for (++j; j < s.size() && isAsciiDigit(s[j]);) fraction += s[j++];
      } else {
        while (!whole.empty() && j + 2 < s.size() && s[j] == ':' && isAsciiDigit(s[j + 1]) && isAsciiDigit(s[j + 2]) &&
               (j + 3 >= s.size() || !isAsciiDigit(s[j + 3]))) {
          clock.push_back(s.substr(j + 1, 2));
          j += 3;
        }
      }
      // Letters written straight after it ("st", "s", "'s", "pm", "er"),
      // without outer apostrophes: [from, to) as written, `bare` as read.
      size_t end = j;
      std::string read;
      for (char letter = 0; end < s.size();) {
        const size_t len = spellingCharAt(s, end, &letter);
        if (len == 0) break;
        read += letter;
        end += len;
      }
      size_t from = j, to = end;
      while (from < to && apostropheAt(s, from) > 0) from += apostropheAt(s, from);
      while (to > from && (s[to - 1] == '\'' || (to - from >= 3 && curlyApostropheAt(s, to - 3) > 0)))
        to -= s[to - 1] == '\'' ? 1 : 3;
      const size_t lead = read.find_first_not_of('\''), trail = read.find_last_not_of('\'');
      const std::string bare = lead == std::string::npos ? "" : read.substr(lead, trail + 1 - lead);
      const bool integer = !decimal && clock.empty() && !whole.empty();
      const bool isOrdinal = integer && (bare == "st" || bare == "nd" || bare == "rd" || bare == "th");
      const bool isPlural = integer && bare == "s";

      std::vector<std::string> words;
      if (numberSign) words.push_back("number");
      const bool cents = unit && decimal && fraction.size() == 2;
      if (cents) {
        const bool dollars = !whole.empty() && whole.find_first_not_of('0') != std::string::npos;
        if (dollars) {
          readInteger(whole, false, words);
          words.push_back(std::string(unit) + (whole == "1" ? "" : "s"));
        }
        if (fraction != "00" || !dollars) {
          cardinal(std::stoll(fraction), words);
          words.push_back(fraction == "01" ? "cent" : "cents");
        }
      } else {
        readInteger(whole, integer && !grouped && !isOrdinal, words);
        if (decimal) {
          words.push_back("point");
          digitByDigit(fraction, "zero", words);
        }
        for (size_t g = 0; g < clock.size(); ++g) {
          if (g == 0 && clock[g] == "00") words.push_back("o'clock");
          else readInteger(clock[g], false, words);
        }
        if (unit) words.push_back(std::string(unit) + (integer && whole == "1" ? "" : "s"));
      }
      if (isOrdinal) words.back() = ordinal(words.back());
      if (isPlural) words.back() = plural(words.back());
      for (const auto& w : words) out.push_back({w, false});
      unit = nullptr;
      numberSign = false;
      if (!isOrdinal && !isPlural && !bare.empty()) {
        const std::string letters = s.substr(from, to - from);
        bool capitals = true, plain = true;  // ASCII capitals only; ASCII letters only
        for (char ch : letters) {
          capitals = capitals && ch >= 'A' && ch <= 'Z';
          plain = plain && isAsciiLetter(static_cast<unsigned char>(ch));
        }
        const bool names = plain && (bare == "am" || bare == "pm" || bare.size() == 1 || (capitals && bare.size() <= 3));
        if (names) out.push_back({letters, true});
        else out.back().text = joinSuffix(out.back().text, letters, bare, lexicon);
      }
      i = end;
      continue;
    }
    const char* currency = nullptr;
    if (const size_t len = currencyAt(s, i, &currency)) {
      if (numberAt(s, i + len)) unit = currency;  // a symbol alone is not sung
      i += len;
      continue;
    }
    if (s[i] == '#') {
      numberSign = numberAt(s, i + 1);
      ++i;
      continue;
    }
    if (s[i] == '&' || s[i] == '%') {
      out.push_back({s[i] == '&' ? "and" : "percent", false});
      ++i;
      continue;
    }
    if (spellingCharAt(s, i) > 0) {
      // Letters before a number or a symbol ("MP" of "MP3", "R" of "R&B"): a
      // word of their own, spelled as usual.
      size_t j = i;
      const char* ignored = nullptr;
      while (j < s.size() && !numberAt(s, j) && s[j] != '&' && s[j] != '%' && s[j] != '#' && !currencyAt(s, j, &ignored)) {
        size_t len = 1;
        utf8Decode(s, j, len);
        j += len;
      }
      out.push_back({s.substr(i, j - i), false});
      i = j;
      continue;
    }
    size_t len = 1;
    utf8Decode(s, i, len);
    i += len;
  }
  return out;
}

// Natural cuts of one run and how strongly each resists merging.
using Cut = std::pair<size_t, float>;

std::vector<Cut> runCuts(const std::string& word, const LetterRun& run, const Lexicon* lex) {
  std::vector<Cut> cuts;
  if (isAcronym(word, run)) {
    for (size_t i = 1; i < run.size(); ++i) cuts.push_back({i, 10.f});
    return cuts;
  }
  const Run r = analyze(lowercase(run), lex);
  for (size_t g = 0; g < r.cut.size(); ++g) cuts.push_back({r.cut[g], r.keep[g]});
  return cuts;
}

// Whether a piece may start at letter p: not inside a digraph or "qu", and not
// with "ck", "x" or an apostrophe.
bool canStartPiece(const std::string& s, size_t p) {
  const char c = s[p], prev = s[p - 1];
  if (c == '\'' || c == 'x' || s.compare(p, 2, "ck") == 0) return false;
  if (c == 'k' && prev == 'c') return false;
  if (c == 'u' && prev == 'q') return false;
  return !(c == 'h' && std::strchr("tscpgw", prev));
}

// Keeps parts-1 cuts: drops the weakest (ties: the one making the shortest
// piece), or adds spelling candidates and then halves the longest piece.
// `s` holds the token's lowercase letters.
std::vector<size_t> chooseCuts(std::vector<Cut> cuts, std::vector<Cut> extra, const std::string& s, size_t parts) {
  const size_t letters = s.size();
  std::sort(cuts.begin(), cuts.end());
  const auto pieceEdge = [&](size_t i) { return i < cuts.size() ? cuts[i].first : letters; };
  while (!cuts.empty() && cuts.size() + 1 > parts) {
    size_t worst = 0, worstLength = 0;
    for (size_t i = 0; i < cuts.size(); ++i) {
      const size_t length = pieceEdge(i + 1) - (i > 0 ? cuts[i - 1].first : 0);
      if (i == 0 || cuts[i].second < cuts[worst].second ||
          (cuts[i].second == cuts[worst].second && length < worstLength)) {
        worst = i;
        worstLength = length;
      }
    }
    cuts.erase(cuts.begin() + static_cast<long>(worst));
  }
  std::stable_sort(extra.begin(), extra.end(), [](const Cut& x, const Cut& y) { return x.second > y.second; });
  for (const auto& e : extra) {
    if (cuts.size() + 1 >= parts) break;
    if (e.first == 0 || e.first >= letters ||
        std::any_of(cuts.begin(), cuts.end(), [&](const Cut& c) { return c.first == e.first; }))
      continue;
    cuts.push_back(e);
    std::sort(cuts.begin(), cuts.end());
  }
  while (cuts.size() + 1 < parts) {
    size_t start = 0, longestStart = 0, longest = 0;
    for (size_t i = 0; i <= cuts.size(); ++i) {
      const size_t end = pieceEdge(i);
      if (end - start > longest) {
        longest = end - start;
        longestStart = start;
      }
      start = end;
    }
    if (longest < 2) break;
    // Nearest the middle where a piece can start (later one on ties: rock'-n').
    const size_t middle = longestStart + longest / 2;
    size_t at = middle;
    for (size_t d = 0; d < longest; ++d) {
      const size_t later = middle + d, earlier = middle - std::min(d, longest / 2);
      if (later < longestStart + longest && canStartPiece(s, later)) {
        at = later;
        break;
      }
      if (earlier > longestStart && canStartPiece(s, earlier)) {
        at = earlier;
        break;
      }
    }
    cuts.push_back({at, 0.f});
    std::sort(cuts.begin(), cuts.end());
  }
  std::vector<size_t> out;
  for (const auto& c : cuts) out.push_back(c.first);
  return out;
}

std::vector<std::string> hyphenateWith(const std::string& word, size_t parts, const Lexicon* lex) {
  parts = std::max<size_t>(parts, 1);
  const auto runs = letterRuns(word);
  std::vector<size_t> at;  // byte offset of each letter
  std::string letters;
  for (const auto& run : runs) {
    for (const auto& l : run) at.push_back(l.at);
    letters += lowercase(run);
  }
  std::vector<std::string> out;
  if (parts == 1 || at.size() < parts) {
    out.push_back(word);
    out.resize(parts);
    return out;
  }
  std::vector<Cut> cuts, extra;
  size_t base = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    if (i > 0) cuts.push_back({base, 100.f});  // hyphens and other separators
    for (const auto& c : runCuts(word, runs[i], lex)) cuts.push_back({base + c.first, c.second});
    base += runs[i].size();
  }
  if (cuts.size() + 1 < parts && lex) {  // more pieces than syllables: try how it is spelled
    base = 0;
    for (const auto& run : runs) {
      for (const auto& c : runCuts(word, run, nullptr)) extra.push_back({base + c.first, c.second});
      base += run.size();
    }
  }
  size_t from = 0;
  for (size_t c : chooseCuts(cuts, extra, letters, parts)) {
    out.push_back(word.substr(from, at[c] - from));
    from = at[c];
  }
  out.push_back(word.substr(from));
  return out;
}

// Appends one run's syllables; `text` is the run as written and `at` the byte
// offset of each of its letters plus its length (see display).
void appendSyllables(const Run& r, const std::string& text, const std::vector<size_t>& at,
                     std::vector<SungSyllable>& out) {
  const Pronunciation& p = r.pron;
  const auto piece = [&](size_t k) -> std::string {
    if (k > r.cut.size()) return "";
    const size_t from = k == 0 ? 0 : at[r.cut[k - 1]], to = k < r.cut.size() ? at[r.cut[k]] : text.size();
    return text.substr(from, to - from);
  };
  const auto symbols = [&](size_t from, size_t to) {
    std::vector<std::string> v;
    for (size_t i = from; i < to; ++i) v.push_back(p[i].symbol);
    return v;
  };
  if (r.vowel.empty()) {  // "hmm", "shh": hum the last sonorant
    SungSyllable s;
    s.text = text;
    size_t nucleus = p.size();
    for (size_t i = p.size(); i-- > 0;) {
      const Phoneme* ph = findPhoneme(p[i].symbol);
      if (ph && (ph->cls == PhonClass::Nasal || ph->cls == PhonClass::Liquid || ph->cls == PhonClass::Glide)) {
        nucleus = i;
        break;
      }
    }
    s.onset = symbols(0, std::min(nucleus, p.size()));
    if (nucleus < p.size()) {
      s.nucleus = symbols(nucleus, nucleus + 1);
      s.coda = symbols(nucleus + 1, p.size());
    }
    s.onsetClass = consonantClass(s.onset);
    out.push_back(s);
    return;
  }
  for (size_t k = 0; k < r.vowel.size(); ++k) {
    SungSyllable s;
    const size_t v = r.vowel[k], end = k + 1 < r.vowel.size() ? r.begin[k + 1] : p.size();
    s.text = piece(k);
    s.onset = symbols(r.begin[k], v);
    s.nucleus = symbols(v, v + 1);
    s.coda = symbols(v + 1, end);
    const auto between = symbols(k == 0 ? 0 : r.vowel[k - 1] + 1, v);
    s.onsetClass = consonantClass(between);
    // A t between vowels before an unstressed one is flapped (wa-ter, beau-ti-ful).
    if (k > 0 && r.stress[k] == 0 &&
        (between == std::vector<std::string>{"T"} || between == std::vector<std::string>{"R", "T"}))
      s.onsetClass = OnsetClass::Voiced;
    s.stress = r.stress[k];
    s.optional = r.optional[k];
    s.mergesBack = r.optional[k] && !r.mergesForward[k];
    out.push_back(s);
  }
}

// The name of the letter W as sung, spelled as a lyric sheet writes it.
constexpr const char* kNameOfW = "double-u";

void syllabifyLetters(const std::string& word, const Lexicon& lexicon, std::vector<SungSyllable>& out);

// Appends each letter of `text` (ASCII letters) sung as its name: one syllable
// per letter, and W's three ("dou-ble-u").
void appendLetterNames(const std::string& text, const Lexicon& lexicon, std::vector<SungSyllable>& out) {
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i] >= 'A' && text[i] <= 'Z' ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
    if (c == 'w') {
      syllabifyLetters(kNameOfW, lexicon, out);
      out.back().endsWord = false;
      continue;
    }
    Run letter;
    Lexicon::parseArpabet(kLetterNames[c - 'a'], letter.pron);
    syllabifyPhonemes(letter);
    appendSyllables(letter, text.substr(i, 1), {0, 1}, out);
  }
}

// One sung word spelled with letters: the token's letter runs in order.
void syllabifyLetters(const std::string& word, const Lexicon& lexicon, std::vector<SungSyllable>& out) {
  const size_t first = out.size();
  std::vector<size_t> at;
  const auto runs = letterRuns(word);
  for (size_t k = 0; k < runs.size(); ++k) {
    const LetterRun& run = runs[k];
    const size_t begin = out.size();
    const std::string text = display(word, run, at);
    if (run.size() == 1 && run[0].c == 'w') {
      // The letter W is sung by its three-syllable name, which its one letter
      // cannot carry (the notes showed "W-", "-" and an empty, lyric-less
      // note): the notes show the name ("dou-ble-u").
      syllabifyLetters(kNameOfW, lexicon, out);
      out.back().endsWord = false;
      continue;
    }
    if (isAcronym(word, run)) appendLetterNames(text, lexicon, out);  // ASCII capitals only: one byte per letter
    else appendSyllables(analyze(lowercase(run), &lexicon), text, at, out);
    // A run after a hyphen starts a new part of the word ("Oh-oh", "twenty-eight").
    if (k > 0 && out.size() > begin && word.find('-', runs[k - 1].back().at + 1) < run.front().at)
      out[begin].startsPart = true;
  }
  if (out.size() > first) out.back().endsWord = true;
}

void syllabifyToken(const std::string& word, const Lexicon& lexicon, std::vector<SungSyllable>& out) {
  if (!needsReading(word)) {
    syllabifyLetters(word, lexicon, out);
    return;
  }
  for (const auto& spoken : spokenWords(word, lexicon)) {
    if (!spoken.letterNames) {
      syllabifyLetters(spoken.text, lexicon, out);
    } else if (!spoken.text.empty()) {
      appendLetterNames(spoken.text, lexicon, out);
      out.back().endsWord = true;
    }
  }
}

}  // namespace

bool isAsciiLetter(unsigned char c) { return c < 0x80 && std::isalpha(c); }

size_t curlyApostropheAt(const std::string& s, size_t i) {
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  if (i + 2 >= s.size() || byte(i) != 0xE2 || byte(i + 1) != 0x80) return 0;
  return byte(i + 2) == 0x98 || byte(i + 2) == 0x99 ? 3 : 0;
}

size_t spellingCharAt(const std::string& s, size_t i, char* letter) {
  char c = 0;
  size_t len = utf8LetterAt(s, i, &c);
  if (len == 0 && s[i] == '\'') c = '\'', len = 1;
  if (len == 0 && (len = curlyApostropheAt(s, i)) > 0) c = '\'';
  if (len > 0 && letter) *letter = c;
  return len;
}

OnsetClass consonantClass(const std::vector<std::string>& consonants) {
  if (consonants.empty()) return OnsetClass::None;
  bool voiced = false;
  for (const auto& c : consonants) {
    if (oneOf(c, {"P", "T", "K", "F", "S", "SH", "TH", "CH", "HH"})) return OnsetClass::Unvoiced;
    voiced = voiced || oneOf(c, {"B", "D", "G", "V", "Z", "DH", "ZH", "JH"});
  }
  return voiced ? OnsetClass::Voiced : OnsetClass::Sonorant;
}

std::vector<SungSyllable> syllabify(const std::string& word, const Lexicon& lexicon) {
  std::vector<SungSyllable> out;
  syllabifyToken(word, lexicon, out);
  return out;
}

std::vector<std::string> hyphenate(const std::string& word, size_t parts, const Lexicon& lexicon) {
  return hyphenateWith(word, parts, &lexicon);
}

std::vector<std::string> hyphenate(const std::string& word, size_t parts) {
  return hyphenateWith(word, parts, nullptr);
}

}  // namespace novox
