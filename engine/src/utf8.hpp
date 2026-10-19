// UTF-8 for lyric text: lyrics arrive from speech recognisers and score files
// in any language, so the code that splits words into syllables must step over
// whole characters (never cut inside one), read accented Latin letters as the
// letters they are, and hand back valid UTF-8 whatever it was given.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace novox {

constexpr uint32_t kInvalidCodePoint = 0xFFFFFFFFu;

// The code point of the well-formed UTF-8 sequence at s[i] and its byte length
// in `length`; for a byte that starts none (a stray continuation byte, an
// overlong or truncated sequence, a surrogate) kInvalidCodePoint and length 1.
inline uint32_t utf8Decode(const std::string& s, size_t i, size_t& length) {
  length = 1;
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char b0 = byte(i);
  if (b0 < 0x80) return b0;
  size_t n = 0;
  uint32_t cp = 0, min = 0;
  if (b0 >= 0xC2 && b0 <= 0xDF) n = 2, cp = b0 & 0x1Fu, min = 0x80;
  else if (b0 >= 0xE0 && b0 <= 0xEF) n = 3, cp = b0 & 0x0Fu, min = 0x800;
  else if (b0 >= 0xF0 && b0 <= 0xF4) n = 4, cp = b0 & 0x07u, min = 0x10000;
  else return kInvalidCodePoint;
  if (i + n > s.size()) return kInvalidCodePoint;
  for (size_t k = 1; k < n; ++k) {
    if ((byte(i + k) & 0xC0) != 0x80) return kInvalidCodePoint;
    cp = (cp << 6) | (byte(i + k) & 0x3Fu);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return kInvalidCodePoint;
  length = n;
  return cp;
}

// Appends the UTF-8 encoding of a code point (U+FFFD for a surrogate or one
// beyond U+10FFFF).
inline void utf8Append(std::string& out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// `s` with every byte that starts no well-formed sequence replaced by U+FFFD:
// text the engine reports back (lyrics, warnings) is valid UTF-8 even when the
// caller's was not. Valid input comes back unchanged.
inline std::string validUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t length = 1;
    if (utf8Decode(s, i, length) == kInvalidCodePoint) out += "\xEF\xBF\xBD";
    else out.append(s, i, length);
    i += length;
  }
  return out;
}

// The lowercase ASCII letter a Latin letter is read as for pronunciation
// (e and é, n and ñ, s and ß, o and ø; æ and œ as e), or 0 when `cp` is not a
// Latin letter. Covers ASCII, Latin-1, Latin Extended-A, the pinyin, Romanian
// and Vietnamese letters of Latin Extended-B and Latin Extended Additional:
// the alphabets speech recognisers write. Pronunciation from the English
// rules is approximate for other languages, but the vowels, and so the
// syllables, are mostly where the spelling puts them.
inline char latinLetterBase(uint32_t cp) {
  if (cp < 0x80) {
    if (cp >= 'a' && cp <= 'z') return static_cast<char>(cp);
    if (cp >= 'A' && cp <= 'Z') return static_cast<char>(cp - 'A' + 'a');
    return 0;
  }
  // One entry per code point; '*' = not a letter (multiplication and division signs).
  static constexpr char kLatin1[] =  // U+00C0..U+00FF
      "aaaaaaeceeeeiiiidnooooo*ouuuuyts"
      "aaaaaaeceeeeiiiidnooooo*ouuuuyty";
  static constexpr char kExtendedA[] =  // U+0100..U+017F
      "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnoooooo"
      "eerrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
  static_assert(sizeof kLatin1 == 64 + 1 && sizeof kExtendedA == 128 + 1, "one entry per code point");
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0] == '*' ? 0 : kLatin1[cp - 0xC0];
  if (cp >= 0x100 && cp <= 0x17F) return kExtendedA[cp - 0x100];
  struct Range { uint32_t first, last; char base; };
  static constexpr Range kRanges[] = {
      {0x01A0, 0x01A1, 'o'}, {0x01AF, 0x01B0, 'u'},                         // Vietnamese o-horn, u-horn
      {0x01CD, 0x01CE, 'a'}, {0x01CF, 0x01D0, 'i'}, {0x01D1, 0x01D2, 'o'},  // pinyin third tone
      {0x01D3, 0x01DC, 'u'},                                                // pinyin u and u-umlaut tones
      {0x0218, 0x0219, 's'}, {0x021A, 0x021B, 't'},                         // Romanian comma below
      {0x1E9E, 0x1E9E, 's'},                                                // capital sharp s
      {0x1EA0, 0x1EB7, 'a'}, {0x1EB8, 0x1EC7, 'e'}, {0x1EC8, 0x1ECB, 'i'},  // Vietnamese tones
      {0x1ECC, 0x1EE3, 'o'}, {0x1EE4, 0x1EF1, 'u'}, {0x1EF2, 0x1EF9, 'y'},
  };
  for (const Range& r : kRanges)
    if (cp >= r.first && cp <= r.last) return r.base;
  return 0;
}

// Combining diacritics (U+0300..U+036F): an accent typed after its letter
// ("e" + U+0301) belongs to that letter.
inline bool isCombiningMark(uint32_t cp) { return cp >= 0x300 && cp <= 0x36F; }

// The letter at s[i], if one starts there: its byte length including any
// combining marks after it (1 for an ASCII letter, 2-3 for an accented one),
// and in `base` the ASCII letter it reads as; 0 when s[i] starts no letter.
inline size_t utf8LetterAt(const std::string& s, size_t i, char* base = nullptr) {
  size_t length = 1;
  const char b = latinLetterBase(utf8Decode(s, i, length));
  if (b == 0) return 0;
  if (base) *base = b;
  size_t end = i + length;
  for (size_t next = 1; end < s.size() && isCombiningMark(utf8Decode(s, end, next));) end += next;
  return end - i;
}

}  // namespace novox
