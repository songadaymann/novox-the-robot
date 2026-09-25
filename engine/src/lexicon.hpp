// Word -> phoneme lookup. Uses the CMU Pronouncing Dictionary when loaded,
// user entries first, and a small spelling-rule fallback for unknown words.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace nvs {

struct PhonToken {
  std::string symbol;  // ARPAbet without stress, schwa mapped to "AX"
  int stress = -1;     // 0/1/2 for vowels, -1 for consonants
};

using Pronunciation = std::vector<PhonToken>;

class Lexicon {
 public:
  // Loads a cmudict-format file ("word PH1 PH2 ..."). Returns entries loaded, or -1.
  int loadCmudict(const std::string& path);

  // Parses cmudict-format text already in memory. Returns entries loaded.
  int loadCmudictText(const std::string& text);

  // Loads the dictionary compiled into the engine, if the build embedded one.
  // Returns entries loaded, or -1 if this build has no embedded dictionary.
  int loadEmbedded();

  // Adds or replaces a user pronunciation, e.g. addWord("hugh", "HH Y UW1").
  bool addWord(const std::string& word, const std::string& arpabet);

  // Pronunciation for one word. `fromDictionary` reports whether it was found
  // or guessed from spelling.
  Pronunciation lookup(const std::string& word, bool* fromDictionary = nullptr) const;

  size_t size() const { return entries_.size(); }

  // Parses "HH AH0 L OW1" into tokens; returns false on unknown symbols.
  static bool parseArpabet(const std::string& text, Pronunciation& out);

  // Lowercase letters and apostrophes only.
  static std::string normalize(const std::string& word);

 private:
  std::unordered_map<std::string, Pronunciation> entries_;
  std::unordered_map<std::string, Pronunciation> user_;
};

// Rough English spelling-to-sound rules for words missing from the dictionary.
Pronunciation guessPronunciation(const std::string& word);

bool isVowelSymbol(const std::string& symbol);

}  // namespace nvs
