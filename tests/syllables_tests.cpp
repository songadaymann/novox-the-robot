// singing-v2 tests for engine/src/syllables.*
#include "v2_check.hpp"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <string>
#include <vector>

#include "lexicon.hpp"
#include "syllables.hpp"
#include "utf8.hpp"

namespace {

using novox::OnsetClass;

std::string joined(const std::vector<std::string>& pieces) {
  std::string s;
  for (size_t i = 0; i < pieces.size(); ++i) s += (i ? "-" : "") + pieces[i];
  return s;
}

std::string texts(const std::vector<novox::SungSyllable>& syllables) {
  std::vector<std::string> pieces;
  for (const auto& s : syllables) pieces.push_back(s.text);
  return joined(pieces);
}

// Sung words of a token: pieces joined by "-" inside a word, words by " | ".
std::string sung(const std::vector<novox::SungSyllable>& syllables) {
  std::string s;
  for (size_t k = 0; k < syllables.size(); ++k)
    s += syllables[k].text + (k + 1 == syllables.size() ? "" : syllables[k].endsWord ? " | " : "-");
  return s;
}

std::string classes(const std::vector<novox::SungSyllable>& syllables) {
  std::string s;
  for (const auto& syl : syllables)
    s += syl.onsetClass == OnsetClass::None ? '-' : syl.onsetClass == OnsetClass::Unvoiced ? 'U'
       : syl.onsetClass == OnsetClass::Voiced ? 'V' : 'S';
  return s;
}

std::string optionals(const std::vector<novox::SungSyllable>& syllables) {
  std::string s;
  for (const auto& syl : syllables) s += syl.optional ? 'o' : '.';
  return s;
}

size_t vowels(const novox::Lexicon& lex, const std::string& word) {
  size_t n = 0;
  for (const auto& t : lex.lookup(word)) n += novox::isVowelSymbol(t.symbol) ? 1 : 0;
  return n;
}

// Pieces must rebuild the word, number `parts`, and be non-empty when letters allow.
bool wellFormed(const std::string& word, const std::vector<std::string>& pieces, size_t parts) {
  std::string all;
  size_t letters = 0, empty = 0;
  for (const auto& p : pieces) {
    all += p;
    empty += p.empty() ? 1 : 0;
  }
  for (char c : word) letters += std::isalpha(static_cast<unsigned char>(c)) || c == '\'' ? 1 : 0;
  return all == word && pieces.size() == std::max<size_t>(parts, 1) && (letters < parts || empty == 0);
}

}  // namespace

void testSyllablesV2(int& checks, int& failures) {
  novox::Lexicon lex;
  const bool dictionary = lex.loadEmbedded() > 0;

  // Spelling pieces that read like a lyric sheet, at the lexicon's syllable count.
  struct Case {
    const char* word;
    const char* pieces;
  };
  const Case lyricSheet[] = {
      {"anymore", "an-y-more"}, {"hello", "hel-lo"}, {"remember", "re-mem-ber"}, {"beautiful", "beau-ti-ful"},
      {"little", "lit-tle"}, {"every", "ev-er-y"}, {"morning", "mor-ning"}, {"something", "some-thing"},
      {"telephone", "tel-e-phone"}, {"millimeters", "mil-li-me-ters"}, {"Simon", "Si-mon"},
      {"discover", "dis-cov-er"}, {"creatures", "crea-tures"}, {"harmless", "harm-less"},
      {"follicularum", "fol-lic-u-la-rum"}, {"darling", "dar-ling"}, {"singing", "sing-ing"},
      {"water", "wa-ter"}, {"baby", "ba-by"}, {"happy", "hap-py"}, {"music", "mu-sic"}, {"open", "o-pen"},
      {"people", "peo-ple"}, {"table", "ta-ble"}, {"idea", "i-de-a"}, {"create", "cre-ate"}, {"poem", "po-em"},
      {"don't", "don't"}, {"won't", "won't"}, {"I'm", "I'm"}, {"you're", "you're"}, {"they've", "they've"},
      // short stressed vowels close, long ones stay open
      {"river", "riv-er"}, {"magic", "mag-ic"}, {"tiger", "ti-ger"}, {"Mary", "Ma-ry"}, {"very", "ver-y"},
      {"hero", "he-ro"}, {"sugar", "sug-ar"}, {"body", "bod-y"}, {"father", "fa-ther"},
      // r-vowels: unstressed -er- keeps its r, others lend it to the next vowel
      {"memory", "mem-o-ry"}, {"camera", "cam-er-a"}, {"around", "a-round"}, {"hurry", "hur-ry"},
      // digraphs, x, qu, -tion, consonant-le, silent letters
      {"pocket", "pock-et"}, {"taxi", "tax-i"}, {"exact", "ex-act"}, {"liquid", "li-quid"}, {"equal", "e-qual"},
      {"mission", "mis-sion"}, {"vision", "vi-sion"}, {"special", "spe-cial"}, {"religion", "re-li-gion"},
      {"question", "ques-tion"}, {"whistle", "whis-tle"}, {"muscle", "mus-cle"}, {"tickle", "tick-le"},
      {"single", "sin-gle"}, {"listen", "lis-ten"}, {"often", "of-ten"}, {"million", "mil-lion"},
      {"popular", "pop-u-lar"}, {"value", "val-ue"},
      // s clusters, prefixes, suffixes, compounds
      {"mister", "mis-ter"}, {"nostalgia", "nos-tal-gia"}, {"yesterday", "yes-ter-day"}, {"astray", "a-stray"},
      {"respect", "re-spect"}, {"destroy", "de-stroy"}, {"unable", "un-a-ble"}, {"dessert", "des-sert"},
      {"nicely", "nice-ly"}, {"deeply", "deep-ly"}, {"simply", "sim-ply"}, {"probably", "prob-a-bly"},
      {"holy", "ho-ly"}, {"lovely", "love-ly"}, {"basement", "base-ment"}, {"priceless", "price-less"},
      {"without", "with-out"}, {"forever", "for-ev-er"}, {"wherever", "wher-ev-er"}, {"upon", "up-on"},
      {"password", "pass-word"}, {"nighttime", "night-time"}, {"everything", "ev-ery-thing"},
      {"America", "A-mer-i-ca"}, {"violin", "vi-o-lin"}, {"chocolate", "choc-olate"},
      // -ing
      {"feeling", "feel-ing"}, {"falling", "fal-ling"}, {"coming", "com-ing"}, {"taking", "ta-king"},
      {"thinking", "think-ing"}, {"doing", "do-ing"}, {"shifting", "shif-ting"},
  };
  if (dictionary) {
    for (const auto& c : lyricSheet) {
      const auto syllables = novox::syllabify(c.word, lex);
      const auto pieces = joined(novox::hyphenate(c.word, syllables.size(), lex));
      V2_CHECK(pieces == c.pieces);
      V2_CHECK(texts(syllables) == c.pieces);
      if (pieces != c.pieces) std::cerr << "    " << c.word << ": " << pieces << " (want " << c.pieces << ")\n";
    }

    // The syllable count is the vowel count the rest of the engine uses.
    for (const auto& c : lyricSheet)
      V2_CHECK(novox::syllabify(c.word, lex).size() == std::max<size_t>(1, vowels(lex, c.word)));
    for (const char* w : {"fire", "hour", "every", "family", "serious", "the", "a", "hmm", "rhythm", "W"})
      V2_CHECK(novox::syllabify(w, lex).size() == std::max<size_t>(1, vowels(lex, w)));

    // Other piece counts: optional syllables merge first, spelling splits further.
    V2_CHECK(joined(novox::hyphenate("every", 2, lex)) == "ev-ery");
    V2_CHECK(joined(novox::hyphenate("family", 2, lex)) == "fam-ily");
    V2_CHECK(joined(novox::hyphenate("fire", 1, lex)) == "fire");
    V2_CHECK(joined(novox::hyphenate("nostalgia", 4, lex)) == "nos-tal-gi-a");
    V2_CHECK(joined(novox::hyphenate("another", 2, lex)) == "a-nother");
    V2_CHECK(joined(novox::hyphenate("remember", 1, lex)) == "remember");
    V2_CHECK(joined(novox::hyphenate("America!", 4, lex)) == "A-mer-i-ca!");
    V2_CHECK(joined(novox::hyphenate("twenty-seven", 4, lex)) == "twen-ty--sev-en");  // "ty-": the hyphen stays
    V2_CHECK(joined(novox::hyphenate("twenty-seven", 2, lex)) == "twenty--seven");

    // Phonemes, stress, onset classes (coda + onset since the previous vowel).
    const auto remember = novox::syllabify("remember", lex);
    using Symbols = std::vector<std::string>;
    V2_CHECK(remember.size() == 3 && remember[1].onset == Symbols{"M"} && remember[1].nucleus == Symbols{"EH"} &&
             remember[1].coda == Symbols{"M"} && remember[2].onset == Symbols{"B"});
    V2_CHECK(remember[0].stress == 0 && remember[1].stress == 1 && remember[2].stress == 0);
    V2_CHECK(classes(remember) == "SSV");                      // r | m | m+b
    V2_CHECK(classes(novox::syllabify("baby", lex)) == "VV");
    V2_CHECK(classes(novox::syllabify("mister", lex)) == "SU");  // "st" counts as unvoiced
    V2_CHECK(classes(novox::syllabify("about", lex)) == "-V");   // vowel-initial word
    V2_CHECK(classes(novox::syllabify("hello", lex)) == "US");
    V2_CHECK(classes(novox::syllabify("water", lex)) == "SV");   // flapped t
    V2_CHECK(classes(novox::syllabify("today", lex)) == "UV");   // stressed d, not a flap
    V2_CHECK(classes(novox::syllabify("idea", lex)) == "-V-");   // hiatus
    V2_CHECK(classes(novox::syllabify("blue", lex)) == "V");     // "bl" counts as voiced
    V2_CHECK(classes(novox::syllabify("street", lex)) == "U");
    V2_CHECK(classes(novox::syllabify("singing", lex)) == "US");  // ng between the vowels
    V2_CHECK(novox::syllabify("singing", lex)[0].coda == std::vector<std::string>{"NG"});  // ng never starts one

    // Optional (often elided) syllables.
    V2_CHECK(optionals(novox::syllabify("every", lex)) == ".o.");
    V2_CHECK(optionals(novox::syllabify("family", lex)) == ".o.");
    V2_CHECK(optionals(novox::syllabify("fire", lex)) == ".o");
    V2_CHECK(optionals(novox::syllabify("hour", lex)) == ".o");
    V2_CHECK(optionals(novox::syllabify("serious", lex)) == ".o.");
    V2_CHECK(optionals(novox::syllabify("beautiful", lex)) == "...");
    V2_CHECK(optionals(novox::syllabify("telephone", lex)) == "...");
    V2_CHECK(optionals(novox::syllabify("America", lex)) == "....");
    V2_CHECK(optionals(novox::syllabify("animal", lex)) == "...");
    V2_CHECK(novox::syllabify("fire", lex)[1].mergesBack && !novox::syllabify("every", lex)[1].mergesBack);

    // Tokens as whisper writes them.
    const auto year = novox::syllabify("2027", lex);
    V2_CHECK(texts(year) == "twen-ty-twen-ty-sev-en");
    V2_CHECK(year.size() == 6 && year[1].endsWord && !year[3].endsWord && year[5].endsWord);
    V2_CHECK(texts(novox::syllabify("1999", lex)) == "nine-teen-nine-ty-nine");
    V2_CHECK(texts(novox::syllabify("1,000", lex)) == "one-thou-sand");
    // Numbers are sung as words (rules in syllables.cpp): every number word is
    // a sung word of its own, a compound ("twenty-seven") one word.
    const Case numbers[] = {
        // Cardinals, plain sung style without "and".
        {"0", "ze-ro"}, {"7", "sev-en"}, {"11", "e-lev-en"}, {"40", "for-ty"}, {"70", "sev-en-ty"},
        {"100", "one | hun-dred"}, {"105", "one | hun-dred | five"}, {"1,000", "one | thou-sand"},
        {"10,000", "ten | thou-sand"}, {"100,000", "one | hun-dred | thou-sand"}, {"1,000,000", "one | mil-lion"},
        {"2,027", "two | thou-sand | twen-ty-sev-en"},  // a comma marks a cardinal, never a year
        {"999999999", "nine | hun-dred | nine-ty-nine | mil-lion | nine | hun-dred | nine-ty-nine | thou-sand | "
                      "nine | hun-dred | nine-ty-nine"},
        {"1234567890", "one | two | three | four | five | six | sev-en | eight | nine | oh"},  // too long: digits
        {"007", "oh | oh | sev-en"}, {"1,2,3", "one | two | three"}, {"9/11", "nine | e-lev-en"},
        // Years.
        {"1999", "nine-teen | nine-ty-nine"}, {"2027", "twen-ty | twen-ty-sev-en"}, {"2000", "two | thou-sand"},
        {"1900", "nine-teen | hun-dred"}, {"2005", "two | thou-sand | five"}, {"1905", "nine-teen | oh | five"},
        {"2010", "twen-ty | ten"}, {"1100", "e-lev-en | hun-dred"}, {"(1999)", "nine-teen | nine-ty-nine"},
        // Ordinals, decades and plurals.
        {"1st", "first"}, {"2nd", "sec-ond"}, {"3rd", "third"}, {"12th", "twelfth"}, {"20th", "twen-ti-eth"},
        {"21st", "twen-ty-first"}, {"100th", "one | hun-dredth"}, {"#1st", "num-ber | first"},
        {"1990s", "nine-teen | nine-ties"}, {"1990's", "nine-teen | nine-ties"}, {"90s", "nine-ties"},
        {"'90s", "nine-ties"}, {"\xE2\x80\x99" "90s", "nine-ties"}, {"1900s", "nine-teen | hun-dreds"},
        {"6s", "six-es"},
        // Decimals, times, symbols.
        {"2.5", "two | point | five"}, {"0.3", "ze-ro | point | three"}, {".5", "point | five"}, {"2.", "two"},
        {"3:30", "three | thir-ty"}, {"3:05", "three | oh | five"}, {"10:00.", "ten | o'-clock"},
        {"$5", "five | dol-lars"}, {"$1", "one | dol-lar"}, {"$5.50", "five | dol-lars | fif-ty | cents"},
        {"$0.99", "nine-ty-nine | cents"}, {"\xE2\x82\xAC" "20", "twen-ty | eu-ros"}, {"50%", "fif-ty | per-cent"},
        {"#1", "num-ber | one"}, {"&", "and"}, {"R&B", "R | and | B"}, {"$", ""}, {"#", ""},
        // Letters with numbers.
        {"49er,", "for-ty-ni-ner"}, {"49ers", "for-ty-ni-ners"}, {"4ever", "four-ev-er"}, {"3pm", "three | p-m"},
        {"3:30am", "three | thir-ty | a-m"}, {"9AM", "nine | A-M"}, {"3D", "three | D"}, {"MP3", "M-P | three"},
        {"R2D2", "R | two | D | two"}, {"COVID-19", "CO-VID | nine-teen"}, {"19th-century", "nine-teenth | cen-tu-ry"},
        {"4W", "four | dou-ble-u"},
        // The letter W is sung by its name, which its one letter cannot carry;
        // a plural keeps its stem's compound split (not "hund-reds").
        {"W,", "dou-ble-u"}, {"hundreds", "hun-dreds"},
    };
    for (const auto& c : numbers) {
      const auto syllables = novox::syllabify(c.word, lex);
      const bool ok = sung(syllables) == c.pieces;
      V2_CHECK(ok);
      if (!ok) std::printf("    syllabify(%s) = %s, want %s\n", c.word, sung(syllables).c_str(), c.pieces);
    }
    // A compound number word's parts start after its hyphen (no hiatus: "ty" and
    // "eight" of "28" are two words' vowels), and only the word's last syllable ends it.
    const auto eight = novox::syllabify("28", lex);
    V2_CHECK(eight.size() == 3 && eight[2].startsPart && !eight[1].startsPart && !eight[1].endsWord && eight[2].endsWord);
    V2_CHECK(novox::syllabify("Oh-oh", lex)[1].startsPart && !novox::syllabify("hello", lex)[1].startsPart);
    // Whatever digits and symbols a token holds, its pieces are letters: no digit
    // reaches a note, and no piece is empty.
    for (const char* w : {"1é", "5\xC3\x89\xC3\x89", ",000", "1:2", "12:345", "1.2.3", "..5", "$1.01", "$0.00", "$.50",
                          "1-800-555-1234", "B-52s", "one2three", "%%", "x2", "24h", "0.05", "\xC2\xA3" "3",
                          "99999999999999999999999", "1,000,000,000", "4am,", "2's", "12:00:05", "1/2"}) {
      const auto syllables = novox::syllabify(w, lex);
      V2_CHECK(!syllables.empty() && syllables.back().endsWord);
      for (const auto& syl : syllables)
        V2_CHECK(!syl.text.empty() && std::none_of(syl.text.begin(), syl.text.end(), [](char ch) { return ch >= '0' && ch <= '9'; }));
    }
    V2_CHECK(texts(novox::syllabify("AI", lex)) == "A-I");
    V2_CHECK(texts(novox::syllabify("US", lex)) == "U-S");
    V2_CHECK(novox::syllabify("us", lex).size() == 1 && novox::syllabify("OH", lex).size() == 1);
    V2_CHECK(texts(novox::syllabify("America!", lex)) == "A-mer-i-ca");
    V2_CHECK(texts(novox::syllabify("\"Remember,", lex)) == "Re-mem-ber");
    V2_CHECK(texts(novox::syllabify("HELLO", lex)) == "HEL-LO");
    V2_CHECK(texts(novox::syllabify("twenty-seven", lex)) == "twen-ty-sev-en");
    V2_CHECK(texts(novox::syllabify("there-ing", lex)) == "there-ing");
    V2_CHECK(texts(novox::syllabify("ev'ry", lex)) == "ev'-ry");
    V2_CHECK(texts(novox::syllabify("goin'", lex)) == "go-in'");  // dropped g: read as "going"
    V2_CHECK(texts(novox::syllabify("lovin'", lex)) == "lov-in'");
    V2_CHECK(texts(novox::syllabify("feelin'", lex)) == "feel-in'");
    V2_CHECK(novox::syllabify("goin'", lex)[1].coda == std::vector<std::string>{"N"});
    V2_CHECK(joined(novox::hyphenate("rock'n'roll", 3)) == "rock'-n'-roll");
    V2_CHECK(texts(novox::syllabify("don\xE2\x80\x99t", lex)) == "don't");  // curly apostrophe
    V2_CHECK(novox::syllabify("don\xE2\x80\x99t", lex)[0].coda == (std::vector<std::string>{"N", "T"}));
    V2_CHECK(texts(novox::syllabify("a", lex)) == "a" && texts(novox::syllabify("I", lex)) == "I");
    // Vocables as recognisers write them are one sung syllable each: the
    // dictionary's first "ba" (and "baa") is the degree B.A., and a held vowel
    // may be written out ("nooo"). Capitals and real letter-name words stay spelled.
    for (const char* w : {"ba", "Ba,", "baa", "aah", "aaah", "ooo", "ooooh", "nooo", "sooo", "heyyy", "whoaaa"})
      V2_CHECK(novox::syllabify(w, lex).size() == 1);
    V2_CHECK(texts(novox::syllabify("ba-da-ba", lex)) == "ba-da-ba");
    V2_CHECK(texts(novox::syllabify("baaaby", lex)) == "baaa-by");
    V2_CHECK(texts(novox::syllabify("BA", lex)) == "B-A");
    V2_CHECK(texts(novox::syllabify("ok", lex)) == "o-k");
    V2_CHECK(texts(novox::syllabify("bazaar", lex)) == "ba-zaar");
    V2_CHECK(novox::syllabify("committee", lex).size() == 3);
    const auto hum = novox::syllabify("hmm", lex);
    V2_CHECK(hum.size() == 1 && hum[0].nucleus == std::vector<std::string>{"M"} && hum[0].endsWord);
    V2_CHECK(novox::syllabify("...", lex).empty() && novox::syllabify("", lex).empty());
    // Letters are the same whatever the host's C locale (a Python host runs in
    // a UTF-8 locale, where isalpha(0xE2) can be true).
    {
      const std::string naive = texts(novox::syllabify("na\xC3\xAFve", lex));
      const std::string saved = std::setlocale(LC_CTYPE, nullptr);
      if (std::setlocale(LC_CTYPE, "en_US.UTF-8") || std::setlocale(LC_CTYPE, "C.UTF-8")) {
        V2_CHECK(novox::syllabify("\xE2\x99\xAA", lex).empty());  // a music note
        V2_CHECK(texts(novox::syllabify("na\xC3\xAFve", lex)) == naive);
      }
      std::setlocale(LC_CTYPE, saved.c_str());
    }

    // Accented Latin letters (French, Spanish, German from a recogniser) are
    // letters, read as their base letter and kept as written; no piece ends
    // inside a UTF-8 character ("tête" once came back as "t", "te").
    V2_CHECK(texts(novox::syllabify("t\xC3\xAAte", lex)) == "t\xC3\xAAte");
    V2_CHECK(texts(novox::syllabify("caf\xC3\xA9", lex)) == "ca-f\xC3\xA9");
    V2_CHECK(texts(novox::syllabify("coraz\xC3\xB3n", lex)) == "co-ra-z\xC3\xB3n");
    V2_CHECK(texts(novox::syllabify("ni\xC3\xB1o", lex)) == "ni-\xC3\xB1o");
    V2_CHECK(texts(novox::syllabify("M\xC3\xA4" "dchen", lex)) == "M\xC3\xA4" "d-chen");
    V2_CHECK(texts(novox::syllabify("na\xC3\xAFve", lex)) == "na-\xC3\xAFve");
    V2_CHECK(texts(novox::syllabify("t\xC3\xAAte-\xC3\xA0-t\xC3\xAAte", lex)) == "t\xC3\xAAte-\xC3\xA0-t\xC3\xAAte");
    V2_CHECK(texts(novox::syllabify("Stra\xC3\x9F" "e", lex)) == "Stra\xC3\x9F" "e");
    // An accent typed after its letter (decomposed form) stays with it.
    V2_CHECK(texts(novox::syllabify("cafe\xCC\x81", lex)) == "ca-fe\xCC\x81");
    // Other scripts and emoji are not letters: they separate words, whole.
    V2_CHECK(texts(novox::syllabify("love\xE2\x9D\xA4\xEF\xB8\x8F", lex)) == "love");  // red heart
    V2_CHECK(novox::syllabify("\xF0\x9F\x8E\xB5", lex).empty());                       // musical note emoji
    V2_CHECK(novox::syllabify("\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD1\x8C", lex).empty());  // Cyrillic
    V2_CHECK(texts(novox::syllabify("caf\xC3", lex)) == "caf");  // a truncated character is dropped, not cut
    for (const char* w : {"t\xC3\xAAte", "coraz\xC3\xB3n", "\xC3\xBC" "ber", "fianc\xC3\xA9" "e", "Br\xC3\xBBl\xC3\xA9" "e",
                          "Z\xC3\xBCrich", "\xC5\x93uvre", "\xC3\x89T\xC3\x89", "Vi\xE1\xBB\x87t", "love\xE2\x9D\xA4\xEF\xB8\x8F",
                          "\xF0\x9F\x8E\xB5la", "cafe\xCC\x81", "caf\xC3"}) {
      for (const auto& syl : novox::syllabify(w, lex)) V2_CHECK(!syl.text.empty() && novox::validUtf8(syl.text) == syl.text);
      for (size_t parts = 1; parts <= 6; ++parts) {
        const auto pieces = novox::hyphenate(w, parts, lex);
        std::string all;
        for (const auto& piece : pieces) all += piece;
        V2_CHECK(all == w && pieces.size() == parts);
        // Each piece is whole characters: valid where the token is.
        if (novox::validUtf8(w) == w)
          for (const auto& piece : pieces) V2_CHECK(novox::validUtf8(piece) == piece);
      }
    }

    // Unknown words take the spelling-rule pronunciation.
    bool known = true;
    lex.lookup("follicularum", &known);
    V2_CHECK(!known);
    V2_CHECK(novox::syllabify("follicularum", lex).size() == vowels(lex, "follicularum"));
    V2_CHECK(texts(novox::syllabify("Novox", lex)) == "No-vox");
  }

  // UTF-8 helpers: letters and their readings, and repair of invalid text.
  {
    char c = 0;
    V2_CHECK(novox::spellingCharAt("\xC3\xA9", 0, &c) == 2 && c == 'e');       // e acute
    V2_CHECK(novox::spellingCharAt("\xC3\x9F", 0, &c) == 2 && c == 's');       // sharp s
    V2_CHECK(novox::spellingCharAt("\xC3\x91", 0, &c) == 2 && c == 'n');       // capital n tilde
    V2_CHECK(novox::spellingCharAt("\xE1\xBB\x87", 0, &c) == 3 && c == 'e');  // Vietnamese e
    V2_CHECK(novox::spellingCharAt("e\xCC\x81", 0, &c) == 3 && c == 'e');      // e + combining acute
    V2_CHECK(novox::spellingCharAt("\xE2\x80\x99", 0, &c) == 3 && c == '\'');  // curly apostrophe
    V2_CHECK(novox::spellingCharAt("\xC3\x97", 0) == 0);                       // multiplication sign
    V2_CHECK(novox::spellingCharAt("\xC3", 0) == 0 && novox::spellingCharAt("\xA9", 0) == 0);
    V2_CHECK(novox::validUtf8("t\xC3\xAAte \xF0\x9F\x8E\xB5") == "t\xC3\xAAte \xF0\x9F\x8E\xB5");
    V2_CHECK(novox::validUtf8("t\xC3t") == "t\xEF\xBF\xBDt");
    V2_CHECK(novox::validUtf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");          // overlong
    V2_CHECK(novox::validUtf8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // surrogate
  }

  // Spelling alone (no lexicon).
  V2_CHECK(joined(novox::hyphenate("baby", 2)) == "ba-by");
  V2_CHECK(joined(novox::hyphenate("happy", 2)) == "hap-py");
  V2_CHECK(joined(novox::hyphenate("water", 2)) == "wa-ter");
  V2_CHECK(joined(novox::hyphenate("pocket", 2)) == "pock-et");
  V2_CHECK(joined(novox::hyphenate("beautiful", 3)) == "beau-ti-ful");
  V2_CHECK(joined(novox::hyphenate("nostalgia", 4)) == "nos-tal-gi-a");
  V2_CHECK(joined(novox::hyphenate("telephone", 3)) == "tel-e-phone");
  V2_CHECK(joined(novox::hyphenate("follicularum", 5)) == "fol-lic-u-la-rum");
  V2_CHECK(joined(novox::hyphenate("remember", 3)) == "re-mem-ber");

  // Structure: pieces rebuild the word exactly, never empty when letters allow.
  for (const char* w : {"anymore", "hello", "America!", "twenty-seven", "don't", "I", "a", "eyes", "strengths",
                        "rhythm", "W", "2027", "Mississippi", "o'clock", "\"Yes,\"", "naïve", "hmm", "nooo",
                        "baaaby", "Ba,"}) {
    for (size_t parts = 0; parts <= 8; ++parts) {
      V2_CHECK(wellFormed(w, novox::hyphenate(w, parts), parts));
      if (dictionary) V2_CHECK(wellFormed(w, novox::hyphenate(w, parts, lex), parts));
    }
  }
  V2_CHECK(joined(novox::hyphenate("go", 3)) == "go--");    // more parts than letters: word, then empties
  V2_CHECK(joined(novox::hyphenate("go", 2)) == "g-o");
  V2_CHECK(novox::hyphenate("hello", 0) == std::vector<std::string>{"hello"});
  V2_CHECK(joined(novox::hyphenate("2027", 3)) == "2027--");

  V2_CHECK(novox::consonantClass({}) == OnsetClass::None);
  V2_CHECK(novox::consonantClass({"S", "T"}) == OnsetClass::Unvoiced);
  V2_CHECK(novox::consonantClass({"B", "L"}) == OnsetClass::Voiced);
  V2_CHECK(novox::consonantClass({"N", "D"}) == OnsetClass::Voiced);
  V2_CHECK(novox::consonantClass({"M", "Y"}) == OnsetClass::Sonorant);

  // Deterministic.
  if (dictionary) V2_CHECK(texts(novox::syllabify("hallelujah", lex)) == texts(novox::syllabify("hallelujah", lex)));

  // A garbage token far longer than any word is cut evenly, without the quadratic
  // letter alignment (10,000 letters took ~600 MB and seconds there).
  {
    std::string garbage;
    while (garbage.size() < 10000) garbage += "banana";
    const auto syl = novox::syllabify(garbage, lex);
    std::string rebuilt;
    bool nonEmpty = true;
    for (const auto& s : syl) rebuilt += s.text, nonEmpty &= !s.text.empty();
    V2_CHECK(syl.size() == vowels(lex, garbage) && syl.size() > 1000);
    V2_CHECK(rebuilt == garbage && nonEmpty);
    V2_CHECK(wellFormed(garbage, novox::hyphenate(garbage, 7, lex), 7));
    V2_CHECK(wellFormed(garbage, novox::hyphenate(garbage, 3000), 3000));
  }
}
