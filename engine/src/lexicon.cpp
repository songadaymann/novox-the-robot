#include "lexicon.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

#include "phonemes.hpp"

namespace nvs {

bool isVowelSymbol(const std::string& s) {
  const Phoneme* p = findPhoneme(s);
  return p != nullptr && p->cls == PhonClass::Vowel;
}

std::string Lexicon::normalize(const std::string& word) {
  std::string out;
  for (unsigned char c : word) {
    if (std::isalpha(c)) out += static_cast<char>(std::tolower(c));
    else if (c == '\'') out += '\'';
  }
  // Curly apostrophes arrive as UTF-8 bytes and are dropped above; strip
  // leading/trailing plain apostrophes too ('til, singin').
  while (!out.empty() && out.front() == '\'') out.erase(out.begin());
  while (!out.empty() && out.back() == '\'') out.pop_back();
  return out;
}

bool Lexicon::parseArpabet(const std::string& text, Pronunciation& out) {
  out.clear();
  std::istringstream in(text);
  std::string tok;
  while (in >> tok) {
    PhonToken t;
    std::string sym;
    for (char c : tok) {
      if (std::isdigit(static_cast<unsigned char>(c))) t.stress = c - '0';
      else sym += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (sym == "AH" && t.stress == 0) sym = "AX";
    if (!findPhoneme(sym)) return false;
    if (isVowelSymbol(sym) && t.stress < 0) t.stress = 1;
    t.symbol = sym;
    out.push_back(t);
  }
  return !out.empty();
}

int Lexicon::loadCmudict(const std::string& path) {
  std::ifstream in(path);
  if (!in) return -1;
  std::stringstream ss;
  ss << in.rdbuf();
  return loadCmudictText(ss.str());
}

int Lexicon::loadCmudictText(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  int count = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == ';') continue;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    std::string word = line.substr(0, sp);
    if (word.find('(') != std::string::npos) continue;  // keep first variant only
    std::string pron = line.substr(sp + 1);
    size_t hash = pron.find('#');
    if (hash != std::string::npos) pron = pron.substr(0, hash);
    Pronunciation p;
    if (parseArpabet(pron, p)) {
      entries_[word] = std::move(p);
      ++count;
    }
  }
  return count;
}

#ifdef NVS_EMBEDDED_DICT
extern const char* const kCmudictChunks[];
extern const size_t kCmudictChunkCount;
int Lexicon::loadEmbedded() {
  std::string text;
  for (size_t i = 0; i < kCmudictChunkCount; ++i) text += kCmudictChunks[i];
  return loadCmudictText(text);
}
#else
int Lexicon::loadEmbedded() { return -1; }
#endif

bool Lexicon::addWord(const std::string& word, const std::string& arpabet) {
  Pronunciation p;
  if (!parseArpabet(arpabet, p)) return false;
  user_[normalize(word)] = std::move(p);
  return true;
}

Pronunciation Lexicon::lookup(const std::string& word, bool* fromDictionary) const {
  std::string w = normalize(word);
  if (auto it = user_.find(w); it != user_.end()) {
    if (fromDictionary) *fromDictionary = true;
    return it->second;
  }
  if (auto it = entries_.find(w); it != entries_.end()) {
    if (fromDictionary) *fromDictionary = true;
    return it->second;
  }
  if (fromDictionary) *fromDictionary = false;
  return guessPronunciation(w);
}

// ---------------------------------------------------------------------------
// Spelling fallback. Deliberately simple: it only needs to be close enough for
// invented words and names; exact words belong in the dictionary.

namespace {

bool isV(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

void push(Pronunciation& p, const char* sym, int stress = -1) {
  PhonToken t;
  t.symbol = sym;
  t.stress = isVowelSymbol(sym) ? (stress < 0 ? 1 : stress) : -1;
  p.push_back(t);
}

bool startsWith(const std::string& s, size_t i, const char* lit) {
  return s.compare(i, std::char_traits<char>::length(lit), lit) == 0;
}

}  // namespace

Pronunciation guessPronunciation(const std::string& raw) {
  std::string w;
  for (char c : raw) if (c != '\'') w += c;
  Pronunciation p;
  const size_t n = w.size();
  size_t i = 0;

  // "Magic e": vowel + single consonant + final e -> long vowel.
  auto magicE = [&](size_t at) {
    return at + 2 == n - 1 && w[n - 1] == 'e' && !isV(w[at + 1]) && n > 3;
  };

  while (i < n) {
    char c = w[i];
    char next = i + 1 < n ? w[i + 1] : '\0';

    // Multi-letter patterns first.
    if (startsWith(w, i, "tch")) { push(p, "CH"); i += 3; continue; }
    if (startsWith(w, i, "igh")) { push(p, "AY"); i += 3; continue; }
    if (startsWith(w, i, "ch")) { push(p, "CH"); i += 2; continue; }
    if (startsWith(w, i, "sh")) { push(p, "SH"); i += 2; continue; }
    if (startsWith(w, i, "th")) { push(p, "TH"); i += 2; continue; }
    if (startsWith(w, i, "ph")) { push(p, "F"); i += 2; continue; }
    if (startsWith(w, i, "wh")) { push(p, "W"); i += 2; continue; }
    if (startsWith(w, i, "ck")) { push(p, "K"); i += 2; continue; }
    if (startsWith(w, i, "ng")) { push(p, "NG"); i += 2; continue; }
    if (startsWith(w, i, "qu")) { push(p, "K"); push(p, "W"); i += 2; continue; }
    if (startsWith(w, i, "ee") || startsWith(w, i, "ea")) { push(p, "IY"); i += 2; continue; }
    if (startsWith(w, i, "oo")) { push(p, "UW"); i += 2; continue; }
    if (startsWith(w, i, "ou")) { push(p, "AW"); i += 2; continue; }
    if (startsWith(w, i, "ow")) { push(p, i + 2 == n ? "OW" : "AW"); i += 2; continue; }
    if (startsWith(w, i, "ai") || startsWith(w, i, "ay")) { push(p, "EY"); i += 2; continue; }
    if (startsWith(w, i, "oi") || startsWith(w, i, "oy")) { push(p, "OY"); i += 2; continue; }
    if (startsWith(w, i, "au") || startsWith(w, i, "aw")) { push(p, "AO"); i += 2; continue; }
    if (startsWith(w, i, "oa")) { push(p, "OW"); i += 2; continue; }
    if (startsWith(w, i, "ie") && i + 2 == n) { push(p, "AY"); i += 2; continue; }
    if (startsWith(w, i, "er") || startsWith(w, i, "ir") || startsWith(w, i, "ur")) {
      push(p, "ER"); i += 2; continue;
    }
    if (startsWith(w, i, "ar")) { push(p, "AA"); push(p, "R"); i += 2; continue; }
    if (startsWith(w, i, "or")) { push(p, "AO"); push(p, "R"); i += 2; continue; }

    // Doubled consonants sound once.
    if (!isV(c) && c == next) { ++i; continue; }

    switch (c) {
      case 'a': push(p, magicE(i) ? "EY" : "AE"); break;
      case 'e':
        if (i == n - 1 && n > 2) break;  // silent final e
        push(p, magicE(i) ? "IY" : "EH");
        break;
      case 'i': push(p, magicE(i) ? "AY" : "IH"); break;
      case 'o': push(p, magicE(i) ? "OW" : "AA"); break;
      case 'u': push(p, magicE(i) ? "UW" : "AH"); break;
      case 'y':
        if (i == 0) push(p, "Y");
        else if (i == n - 1) push(p, n <= 3 ? "AY" : "IY");
        else push(p, "IH");
        break;
      case 'c': push(p, (next == 'e' || next == 'i' || next == 'y') ? "S" : "K"); break;
      case 'g': push(p, (next == 'e' || next == 'i' || next == 'y') ? "JH" : "G"); break;
      case 'x': push(p, "K"); push(p, "S"); break;
      case 'j': push(p, "JH"); break;
      case 'q': push(p, "K"); break;
      case 'b': push(p, "B"); break;
      case 'd': push(p, "D"); break;
      case 'f': push(p, "F"); break;
      case 'h': push(p, "HH"); break;
      case 'k': push(p, "K"); break;
      case 'l': push(p, "L"); break;
      case 'm': push(p, "M"); break;
      case 'n': push(p, "N"); break;
      case 'p': push(p, "P"); break;
      case 'r': push(p, "R"); break;
      case 's': push(p, (i > 0 && i == n - 1 && isV(w[i - 1])) ? "Z" : "S"); break;
      case 't': push(p, "T"); break;
      case 'v': push(p, "V"); break;
      case 'w': push(p, "W"); break;
      case 'z': push(p, "Z"); break;
      default: break;
    }
    ++i;
  }
  if (p.empty()) push(p, "AX");
  return p;
}

}  // namespace nvs
