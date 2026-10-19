// Sung-syllable inventory for lyric-driven transcription: splits a word's
// pronunciation into syllables (maximal onset), classifies how each syllable's
// start shows up acoustically, and splits the word's spelling into matching
// lyric-sheet pieces ("re-", "mem-", "ber").
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lexicon.hpp"

namespace novox {

// How the start of a syllable shows up in audio.
enum class OnsetClass {
  None,       // vowel-initial: only a spectral/pitch change or a glottal dip marks it
  Unvoiced,   // p t k f s sh th ch h: a voicing break and/or frication energy
  Voiced,     // b d g v z dh zh jh: a vowel-band dip, sometimes a voicing dropout
  Sonorant,   // m n ng l r w y: a shallow vowel-band dip and a formant/spectral change
};

struct SungSyllable {
  std::string text;          // lyric-sheet spelling piece without hyphen ("mem"): letters and apostrophes
  std::vector<std::string> onset, nucleus, coda;  // ARPAbet symbols (no stress digits), split by maximal onset
  // Consonants between the previous vowel of the word and this one (the previous
  // syllable's coda plus this onset; a word's first syllable: its onset only).
  // Mixed runs take the most visible class: Unvoiced > Voiced > Sonorant. A "t"
  // between vowels before an unstressed one is a flap or glottal stop in sung
  // American English (wa-ter, lit-tle), so it counts as Voiced.
  OnsetClass onsetClass = OnsetClass::None;
  int stress = 0;            // 0 unstressed, 1 primary, 2 secondary
  // Unstressed and often elided in singing, so it may share a note with a
  // neighbour: the reduced middle of ev-er-y / fam-i-ly / choc-o-late and an
  // unstressed i/y vowel gliding into the next vowel (se-ri-ous) merge with the
  // next syllable; the r-vowel after a diphthong (fi-re, hou-r, flow-er) merges
  // with the previous one (mergesBack).
  bool optional = false;
  bool mergesBack = false;
  // Last syllable of a sung word: the token's final syllable, and inside a token
  // read as several words ("2027" -> twen-ty | twen-ty-sev-en) each word's last.
  bool endsWord = false;
  // First syllable of a part after a hyphen ("oh" of "Oh-oh", "eight" of
  // "twenty-eight"): vowels meeting there belong to two words, not a hiatus
  // inside one.
  bool startsPart = false;
};

// Syllables of one lyric token (a whisper word such as "remember," or "2027") in
// order, one per vowel phoneme of the lexicon pronunciation; a token with
// letters but no vowel ("hmm") gets one. Punctuation is ignored and curly
// apostrophes count as straight ones; hyphenated compounds are looked up part by
// part ("twenty-seven", "there-ing"); "-in'" is the -ing word sung with n
// ("goin'" -> go-in'); all-caps tokens of 2-3 letters that are not common words
// are spelled ("AI" -> A-I, "US" -> U-S), and the letter W alone is sung and
// shown as its name ("dou-ble-u"). Numbers and the symbols & % $ # are
// read out as the words sung for them, each number word a sung word of its own
// ("100" -> one | hun-dred, "2027" -> twen-ty | twen-ty-sev-en, "1st" -> first,
// "'90s" -> nine-ties, "49er" -> for-ty-nin-er; rules in syllables.cpp).
// So for such tokens the count can differ from the vowels of lexicon.lookup().
// Empty for tokens with nothing to sing. `text` pieces concatenate to the
// token's letters and apostrophes (for a token read out: to the words read),
// case and accents kept: an accented Latin letter ("tête", "año", "Straße") is
// one letter, read as its base letter for pronunciation, and never cut; other
// characters (CJK, emoji, punctuation) separate runs of letters like a space.
std::vector<SungSyllable> syllabify(const std::string& word, const Lexicon& lexicon);

// The characters of a lyric token that syllabify spells with: letters (ASCII,
// and accented Latin ones as whole UTF-8 sequences, see utf8.hpp; std::isalpha
// on UTF-8 bytes depends on the host's C locale, and the result must not) and
// apostrophes, straight or curly (U+2018/U+2019, 3 UTF-8 bytes).
// spellingCharAt: the byte length of such a character at s[i] (0 for any other)
// and in `letter` the lowercase ASCII letter it reads as, or '\'' for an apostrophe.
size_t spellingCharAt(const std::string& s, size_t i, char* letter = nullptr);
bool isAsciiLetter(unsigned char c);
size_t curlyApostropheAt(const std::string& s, size_t i);  // its byte length (3) at s[i], else 0

// Class of a consonant run (no flap rule): None when empty, Unvoiced if it has
// any of p t k f s sh th ch hh, else Voiced if it has any of b d g v z dh zh jh,
// else Sonorant. For a boundary between words use the previous word's last coda
// plus the next word's first onset.
OnsetClass consonantClass(const std::vector<std::string>& consonants);

// Splits a word's spelling into `parts` display pieces that read naturally,
// guided by its lexicon pronunciation ("anymore", 3 -> "an", "y", "more";
// "every", 2 -> "ev", "ery"). With fewer parts than syllables, optional
// syllables merge first; with more, spelling vowels split further ("nostalgia",
// 4 -> "nos", "tal", "gi", "a"). Pieces concatenate to `word` exactly (case and
// punctuation kept; non-letters stay with the piece before them; cuts fall
// between whole UTF-8 characters). Never returns
// empty pieces when the word has at least `parts` letters; otherwise the whole
// word comes first, padded with empty pieces. parts 0 behaves like 1.
std::vector<std::string> hyphenate(const std::string& word, size_t parts, const Lexicon& lexicon);

// The same from spelling alone (rule-guessed pronunciation, no stress), for
// callers without a lexicon. Less reliable on short-vowel words ("Simon").
std::vector<std::string> hyphenate(const std::string& word, size_t parts);

}  // namespace novox
