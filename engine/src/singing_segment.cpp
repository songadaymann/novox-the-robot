// singing-v2 segmentation, syllables first:
//
//   1. articulation features and candidate vowel onsets (articulation.cpp)
//   2. phrases: voiced sound separated by >= 250 ms of silence or unvoiced noise
//   3. the expected syllables of the words (syllables.cpp) and the consonants
//      before each one, which decide what evidence its onset should show
//   4. one monotonic DP over the whole take tiles every phrase with segments
//      that are either the next lyric syllable(s), an unclaimed hum (voiced
//      audio no word claims, e.g. vocables the recogniser dropped) or nothing
//      (quiet tails, breaths); words may be skipped; ASR word ends are soft anchors
//   5. inside each segment, pitch adds melisma notes and strong acoustic
//      re-attacks can separate repeated notes on the same lyric
//   6. note pitches from the pitch module (assignNotePitches), each note's from
//      its own syllable's frames, the notes of one word decided together
#include "singing_segment.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>

#include "articulation.hpp"
#include "syllables.hpp"
#include "utf8.hpp"

namespace novox {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
inline double timeOf(size_t frame) { return static_cast<double>(frame) * kAnalysisHopSeconds; }
inline size_t framesOf(double seconds) { return static_cast<size_t>(std::lround(seconds / kAnalysisHopSeconds)); }

// ---------------------------------------------------------------------------
// Tuning. Costs are negative log-likelihoods: about 3 means "clearly worse".
// Values were set on the dev split of reference/corpus (tools/import_lab.py).

constexpr size_t kPhraseGap = 31;          // >= 248 ms without clearly sung frames separates phrases
constexpr float kPhraseBelowLoud = 28;     // dB: quieter voiced frames (tails, creak, breath) do not join phrases
constexpr double kMinSegment = 0.088;      // shortest syllable or hum (dev references: 1st percentile 0.11 s)
constexpr double kMaxSegment = 6.0;        // longest segment that ends at another candidate onset
constexpr double kLeadIn = 0.16;           // a phrase's first vowel may start this long after its first sound
constexpr double kShortTail = 0.15;        // a hum this short is a release or, between lyrics, an onset consonant: no note
// Duration prior: log-normal around the dev median vowel-to-vowel time (0.31 s,
// log sd 0.53); long held sound is common in singing, so that side is weaker.
constexpr double kSyllableMu = 0.30, kSyllableSigma = 0.6, kLongSigma = 1.0;
// Two lexicon syllables sung on one note (dev references: hiatus vowels about
// half the time, "fire", "our"; other syllables almost never).
constexpr float kMergeHiatus = 1.5f, kMergeOptional = 2.5f, kMergeOther = 4.0f;
// Lyric-less notes: leaving the lyrics needs a clear onset (p > 0.9), a run of
// dropped vocables continues more cheaply, and without any words (melody only)
// every onset with p > 0.27 starts a note.
constexpr float kHumBase = 2.2f, kHumNext = 1.0f, kHumAlone = -1.0f, kHumPerSecond = 0.3f;
// Inside a hum the DP chose, an onset starts another note as it would in a
// take without words (p > 0.27): audio no word claims holds as many notes
// whether or not other phrases have lyrics. (The stricter p > 0.5 left the
// unclaimed "la"s and non-English phrases of other singers under-split: vocadito
// dev +0.007 COnP F, reference/corpus dev unchanged, where hums are rare.)
constexpr float kHumSplit = kHumAlone;
// A transcript that cannot be the lyrics of the take: its words promise fewer
// syllables than this share of the clear onsets (p > 0.5) the phrases show
// (dev takes of both corpora with real or mis-heard but syllable-faithful
// words: 0.59-1.7; a recogniser's one-word guess for a 20 s Mandarin song:
// 0.01). Fewer onsets than the minimum are too little audio to judge.
constexpr float kThinTranscript = 0.25f, kClearOnset = 0.5f;
constexpr size_t kThinMinOnsets = 12;
constexpr float kHumVowelBelow = 15;       // dB: a hum's vowel band reaches within this of its phrase's
constexpr float kDropCost = 0.3f;          // a segment without a note (quiet sound only)
constexpr float kDropBelowLoud = 25;       // dB below loud singing where sound may be dropped
constexpr double kAnchorMargin = 3.0;      // syllables stay within this of their words' ASR anchors (keeps the DP linear)
// ASR word end (DTW completion of the last token) against audited references
// (dev split): the next vowel starts at most ~30 ms before the word's last
// letters are done (2nd percentile -72 ms), the word's own last vowel starts
// before its end (more than 30 ms after it in 1% of words), and the word's
// sound ends within -0.26..+0.13 s of its end (p10/p90). Gross ASR errors are
// left to skipping.
constexpr double kLateTolerance = 0.03, kLateScale = 0.05;
constexpr double kOverrunTolerance = 0.03, kOverrunScale = 0.05;
constexpr double kEarlyTolerance = 0.25, kEarlyScale = 0.4;
constexpr float kAnchorCap = 8;
// A word end costs its own word at most this (late start and late sound end
// together): whisper can finish a word ~0.8 s early when its first syllable is
// held, and one bad anchor must not outweigh a clear onset (about -3.5).
constexpr float kWordEndCap = 3;
constexpr float kTokenWeight = 0.5f;       // interior ASR tokens are weaker anchors than word ends
// Melisma: a split must explain this many st^2 x frames, between plateaus at least this far apart.
constexpr double kMelismaCost = 3.0, kMelismaStep = 0.8;
// Inside a syllable, frames whose vowel-band-to-low-band balance lies this far
// below the vowel's are a murmur: a nasal lowers it by 5-20 dB (the deep ones
// go), a diphthong's offglide ("die" -> /i/) by up to ~12 (it stays).
constexpr float kMurmurDb = 15;
constexpr size_t kTailGap = 8;  // unvoiced frames (64 ms) that separate a later sound from a note's fall-off
constexpr size_t kOnsetDipReach = 12;  // frames (96 ms) before a vowel onset that hold its consonant's dip (as articulation.cpp's dip)
constexpr size_t kOwnVowel = 5;        // frames (40 ms): a note's first frames are its own vowel, never the next consonant
// A note whose frames before that dip all stay this far under its frames after
// it has its vowel after the dip: its onset was placed on a murmur before its
// own vowel (dev: a "ter" onset on a -47 dB murmur, its vowel at -20 dB), and
// the frames after the dip are its own. A legato note's vowel is at least as
// loud as the consonant and rise that follow it.
constexpr float kOwnVowelDb = 10;
// A held pitch (a plateau): the 40 ms median-filtered contour's vibrato centre
// line (vibratoCentreLine) stays within +-kHeldTolerance st for kHeldFrames.
// A scoop, fall-off or glide keeps moving and never holds; a note sung with
// vibrato holds on its centre line. 80 ms: on dev, edge plateaus held that long
// at a distinct pitch are notes in both kinds of reference.
constexpr float kHeldTolerance = 0.2f;
constexpr size_t kHeldFrames = 10;
// A short first piece that holds a plateau is a note of its own when it lies
// at least kHeldHeadRise below the next one, or kMelismaStep above it. A held
// approach from below is the singer reaching for the note: annotators who mark
// everything they hear write it as a note, a score has one note (the trained
// CSD singer holds ~2 st under the target for ~180 ms; splitting those doubled
// CSD's fragments), and Novox sings the intended melody, so only an approach a
// whole tone and more below stays separate (detailed mode keeps the others,
// see kDetailedRules). A short last piece that holds a
// plateau is a note when it rises kHeldRise; a settled droop at the end of a
// syllable is still a release (on dev, downward settled tails are notes in the
// references about as often as not).
constexpr double kHeldHeadRise = 2.5;
constexpr double kHeldRise = 1.25;
// A quiet last piece (12 dB under the syllable's peak) that holds its pitch
// for kSoftNoteFrames without fading (its second half within kFadeDb of its
// first) is a note sung softly, not a release: a release fades as it goes.
constexpr size_t kSoftNoteFrames = 25;  // 200 ms
constexpr float kFadeDb = 1;
// How finely pitch divides a syllable (MelodyOptions::detail). Natural mode
// uses the rules above: the convention of a score and of the audited
// references, where a held approach up to a whole tone below, a settled droop
// at a syllable's end, a grace note under 80 ms, a gliding approach and a quiet
// tail all belong to their note. Detailed mode follows the annotators who mark
// every pitch they hear (vocadito, GTSinger): each of those is a note once it
// holds a pitch, and the same rules then divide every natural note further.
struct MelismaRules {
  size_t minSyllable;  // frames: a shorter span (or one with fewer voiced frames) is one note
  size_t minPiece;     // voiced frames per piece of the piecewise-constant fit
  size_t heldFrames;   // frames on the centre line within +-kHeldTolerance: a held pitch
  double headRise;     // a short first piece that holds this far below the next is a note
  double headDrop;     // ... or this far above it
  double tailRise;     // a short last piece that holds this far above the previous is a note
  double tailDrop;     // ... or this far below it (a settled droop)
  size_t softFrames;   // a quiet last piece held this long without fading is a note sung softly
  bool heldGlides;     // an edge piece that glides into (or out of) its note after (before) holding a pitch is a note
  bool heldGrace;      // a short inner piece that holds a pitch is a note (a grace or passing note)
  bool firmHolds;      // the exceptions above need the contour itself to hold, not just its vibrato centre line
};
// (Natural: no settled droop is a note of its own.)
constexpr MelismaRules kNaturalRules{24, 15, kHeldFrames, kHeldHeadRise, kMelismaStep, kHeldRise,
                                     std::numeric_limits<double>::infinity(), kSoftNoteFrames, false, false, false};
// Detailed: a pitch held for kGraceFrames is a note, the shortest grace note
// the vocadito annotators mark consistently (dev COnP F: 56 ms loses 0.006 on
// vocadito and 0.004 on GTSinger, whose shorter holds are mostly turns of
// glides; 72 ms gains GTSinger 0.003 but loses vocadito 0.008); a span holds
// two such pieces from twice that (12-24 frames score the same). Approaches
// count from a step below or above (kMelismaStep, as between any two
// plateaus; 0.6-1.25 st score within 0.002, natural's 2.5 st loses vocadito
// 0.029), held tails from a rise or droop of kHeldRise (0.8-1.0 st droops
// lose vocadito 0.003-0.005). Worth of each part on vocadito dev: held
// approaches +0.029, edges that glide after or before holding a pitch +0.015
// (GTSinger -0.003), grace pieces +0.007, droops +0.006, inner grace notes and
// quiet tails +0.001 each; GTSinger +0.004 in all. Holds must be firm (see
// Piece::firm): a synthetic leap into vibrato split without it, and no dev
// take changes with it.
constexpr size_t kGraceFrames = 8;  // 64 ms
constexpr MelismaRules kDetailedRules{2 * kGraceFrames, kGraceFrames, kGraceFrames, kMelismaStep, kMelismaStep, kHeldRise,
                                      kHeldRise, kGraceFrames, true, true, true};
constexpr float kPolyphonyWarning = 0.35f;  // PitchAnalysis::polyphony from which pitches are flagged unreliable
// The take's pitch reference weighs notes by how long they are held (a held
// note shows the singer's tuning best) up to this, so one long note cannot set it.
constexpr double kTuningHoldCap = 1.0;

// ---------------------------------------------------------------------------
// Lyric inventory

struct Syl {
  size_t word = 0;              // index into WordSlot list (one ASR word)
  size_t inWord = 0, ofWord = 1;  // position in its spoken word ("2027" is sung as three words)
  bool slotFirst = false, slotLast = false;  // first / last syllable of the ASR word
  BoundaryKind kind = BoundaryKind::Vowel;
  bool optional = false;        // may merge with the previous syllable cheaply
  bool hiatus = false;          // follows the previous vowel directly ("fi-re", "be-ing")
  double notBefore = -1;        // an ASR token inside the word finished earlier syllables by then (< 0: none)
  std::string piece;            // display spelling (without hyphen)
};

struct WordSlot {
  size_t index = 0;             // into the caller's words
  size_t first = 0, count = 0;  // syllables
  double end = -1;              // ASR end anchor, < 0 when unknown
  double lastLetters = -1;      // DTW time of the word's last token with letters (trailing punctuation excluded)
  float weight = 1;             // anchor weight from ASR probability
  float skip = 0;               // cost of leaving the word unassigned
  float probability = 1;
};

// The strongest consonant between two vowels (the previous coda, across word
// boundaries too, plus this onset) decides what the boundary looks like.
BoundaryKind boundaryKind(const std::vector<std::string>& coda, const std::vector<std::string>& onset) {
  std::vector<std::string> between = coda;
  between.insert(between.end(), onset.begin(), onset.end());
  switch (consonantClass(between)) {
    case OnsetClass::Unvoiced: return BoundaryKind::Unvoiced;
    case OnsetClass::Voiced: return BoundaryKind::Voiced;
    case OnsetClass::Sonorant: return BoundaryKind::Sonorant;
    case OnsetClass::None: break;
  }
  return BoundaryKind::Vowel;
}

// The letters of `text` as lowercase ASCII readings (apostrophes and other
// characters left out) and the byte offset where each ends.
void spelledLetters(const std::string& text, std::string& letters, std::vector<size_t>& ends) {
  for (size_t i = 0; i < text.size();) {
    char c = 0;
    size_t len = spellingCharAt(text, i, &c);
    if (len == 0) {
      len = 1;
      utf8Decode(text, i, len);
    } else if (c != '\'') {
      letters += c;
      ends.push_back(i + len);
    }
    i += len;
  }
}

// The syllables the words promise, in order, grouped by ASR word (slots).
// Words with nothing to sing are reported in `r` and get no slot.
void lyricInventory(const std::vector<Word>& words, const Lexicon& lexicon, double duration, std::vector<Syl>& syl,
                    std::vector<WordSlot>& slots, SegmentationResult& r) {
  std::vector<std::string> prevCoda;
  for (size_t wi = 0; wi < words.size(); ++wi) {
    const Word& w = words[wi];
    // syllabify reads the token as sung: punctuation dropped, numbers as
    // number words ("2027" -> twen-ty | twen-ty-sev-en), short acronyms as
    // letters ("AI" -> A-I), hyphenated compounds part by part.
    const std::vector<SungSyllable> all = syllabify(w.text, lexicon);
    if (all.empty()) {
      r.unassignedWords.push_back(wi);
      r.warnings.push_back("'" + w.text + "' has no letters to sing; left out");
      continue;
    }
    WordSlot slot;
    slot.index = wi;
    slot.first = syl.size();
    slot.probability = std::isfinite(w.probability) ? std::clamp(w.probability, 0.0f, 1.0f) : 1.0f;
    const bool known = std::isfinite(w.end) && w.end > 0 && w.end < duration - 0.05;
    slot.end = known ? w.end : -1;
    // ASR tokens are pieces of the word's UTF-8 bytes (byte-level BPE can end
    // one inside an accented letter); a letter is done with the token that
    // holds its last byte.
    std::string joined;
    std::vector<size_t> tokenEnd;  // byte end of each token in `joined`
    for (const auto& t : w.tokens) tokenEnd.push_back((joined += t.text).size());
    std::string tokenLetters;
    std::vector<size_t> letterEnd;
    spelledLetters(joined, tokenLetters, letterEnd);
    // Trailing punctuation tokens finish later (around the next word); the
    // next vowel must not start before the word's own letters are done.
    slot.lastLetters = slot.end;
    if (known) {
      size_t lastEnd = letterEnd.empty() ? 0 : letterEnd.back();
      for (size_t i = 0; i < joined.size(); ++i)
        if (joined[i] >= '0' && joined[i] <= '9') lastEnd = std::max(lastEnd, i + 1);
      for (size_t t = 0; lastEnd > 0 && t < w.tokens.size(); ++t) {
        if (tokenEnd[t] < lastEnd) continue;
        if (std::isfinite(w.tokens[t].time) && w.tokens[t].time > 0) slot.lastLetters = std::min(slot.end, w.tokens[t].time);
        break;
      }
    }
    slot.weight = std::clamp((slot.probability - 0.3f) / 0.4f, 0.2f, 1.0f);
    for (size_t begin = 0, k = 0; k < all.size(); ++k) {
      if (!all[k].endsWord && k + 1 < all.size()) continue;
      const size_t count = k + 1 - begin;  // one sung word: syllables [begin, k]
      for (size_t i = 0; i < count; ++i) {
        const SungSyllable& cur = all[begin + i];
        Syl s;
        s.word = slots.size();
        s.inWord = i;
        s.ofWord = count;
        s.kind = boundaryKind(i ? all[begin + i - 1].coda : prevCoda, cur.onset);
        // Foldable into the previous syllable: a vowel straight after another
        // vowel inside a word, one of them unstressed ("be-ing"; two stressed
        // vowels are re-attacked, as in spelled letters "A-I"), a schwa after a
        // single sonorant ("fam-ily"), or what syllabify marks optional: a
        // reduced syllable joins the next one (ev-er-y -> ev-ery), an r-vowel
        // after a diphthong the previous one (fi-re). Singers often sing these
        // as one note.
        if (i > 0) {
          const SungSyllable& prev = all[begin + i - 1];
          std::vector<std::string> between = prev.coda;
          between.insert(between.end(), cur.onset.begin(), cur.onset.end());
          const std::string v = cur.nucleus.empty() ? "" : cur.nucleus.front();
          const bool schwa = cur.stress == 0 && (v == "AH" || v == "AX" || v == "ER");
          s.hiatus = between.empty() && !cur.startsPart && (cur.stress == 0 || prev.stress == 0);
          s.optional = (cur.optional && cur.mergesBack) || (prev.optional && !prev.mergesBack) ||
                       (schwa && between.size() == 1 && (between[0] == "L" || between[0] == "N" || between[0] == "R"));
        }
        s.piece = cur.text;
        syl.push_back(s);
      }
      prevCoda = all[k].coda;
      begin = k + 1;
    }
    slot.count = syl.size() - slot.first;
    // Interior ASR tokens (" re" + "member"): a token's DTW time is when its
    // letters were done, so the syllable after the one holding its last
    // letter starts no earlier (a weak anchor; dev: 23 interior tokens).
    {
      std::string letters;
      std::vector<size_t> owner;  // syllable of each letter of the spoken text
      for (size_t k = slot.first; k < syl.size(); ++k) {
        std::vector<size_t> unused;
        spelledLetters(syl[k].piece, letters, unused);
        owner.resize(letters.size(), k);
      }
      std::vector<std::pair<size_t, double>> ends;  // letters done with the token, time
      for (size_t t = 0; t < w.tokens.size(); ++t)
        if (std::isfinite(w.tokens[t].time) && w.tokens[t].time > 0)
          ends.push_back({static_cast<size_t>(std::upper_bound(letterEnd.begin(), letterEnd.end(), tokenEnd[t]) -
                                              letterEnd.begin()),
                          w.tokens[t].time});
      if (tokenLetters == letters)
        for (const auto& [count, time] : ends) {
          if (count == 0 || count >= letters.size()) continue;
          for (size_t k = owner[count - 1] + 1; k < syl.size(); ++k) syl[k].notBefore = std::max(syl[k].notBefore, time);
        }
    }
    syl[slot.first].slotFirst = true;
    syl.back().slotLast = true;
    slot.skip = (2.5f + 2.0f * static_cast<float>(slot.count)) * std::clamp(slot.probability / 0.6f, 0.35f, 1.0f);
    slots.push_back(slot);
  }
}

// ---------------------------------------------------------------------------
// Phrases

struct Phrase {
  size_t first = 0, end = 0;  // frames [first, end): first = vowel onset of the first sound
};

std::vector<Phrase> findPhrases(const ArticulationFeatures& f) {
  // Phrases are built from clearly sung frames (a quiet creak or breath in a
  // rest must not glue two phrases together), then extended over the voiced
  // release tail that follows.
  std::vector<Phrase> out;
  const size_t n = f.frames;
  // Weak frames count: a loud syllable the path left unvoiced is still sung.
  auto strong = [&](size_t k) { return (f.voiced[k] && f.total[k] >= f.loud[k] - kPhraseBelowLoud) || f.weak[k]; };
  size_t k = 0;
  while (k < n) {
    while (k < n && !strong(k)) ++k;
    if (k >= n) break;
    size_t start = k, last = k, count = 0;
    for (size_t gap = 0; k < n && gap < kPhraseGap; ++k) {
      if (strong(k)) {
        gap = 0;
        last = k;
        ++count;
      } else {
        ++gap;
      }
    }
    k = last + 1;
    while (k < n && f.voiced[k]) ++k;  // release tail
    if (count < 4) continue;          // clicks and breath noise decode as a few voiced frames
    Phrase p;
    p.first = vowelOnsetBefore(f, start);
    if (!out.empty()) p.first = std::max(p.first, out.back().end);
    p.end = k;
    if (p.end >= p.first + framesOf(kMinSegment)) out.push_back(p);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Inside one syllable: notes from pitch plateaus (melisma), never from vibrato,
// scoops, overshoot or fall-offs.

// Returns the start frames of the notes after the first one in [first, end), and
// in `sungEnd` where the sung sound ends when a tail after it is not part of it.
std::vector<size_t> melismaStarts(const PitchAnalysis& pitch, const ArticulationFeatures& f, size_t first, size_t end,
                                  size_t& sungEnd, const MelismaRules& rules) {
  std::vector<size_t> out;
  if (end <= first + rules.minSyllable) return out;
  std::vector<float> c = consistentContour(pitch, first, end - 1);
  c.resize(end - first, 0);
  float peak = -1e9f;
  for (size_t k = first; k < end; ++k) peak = std::max(peak, f.total[k]);
  // The vowel's balance of vowel band against low band. A nasal or other
  // murmur (a coda /n/, a hummed lead-in, a low instrument under a fading
  // voice) keeps the low band and loses the vowel band: its pitch says nothing
  // about the sung note either.
  // Only compare timbre across a register change when the low-frequency band
  // still contains the fundamental. With one register, keep the usual vowel
  // reference (including high voices) so nasal tails remain excluded.
  std::vector<float> registerPitches;
  for (size_t i = 0; i < c.size(); ++i)
    if (c[i] > 0 && f.total[first + i] >= peak - 20) registerPitches.push_back(c[i]);
  std::sort(registerPitches.begin(), registerPitches.end());
  const double lowBandLimit = 69 + 12 * std::log2(400.0 / 440.0);
  const bool registerLeap = !registerPitches.empty() &&
      registerPitches[registerPitches.size() * 9 / 10] - registerPitches[registerPitches.size() / 10] >= 9.0f &&
      registerPitches[registerPitches.size() / 10] <= lowBandLimit &&
      registerPitches[registerPitches.size() * 9 / 10] > lowBandLimit;
  std::vector<float> balance;
  for (size_t i = 0; i < c.size(); ++i)
    // Above the low band's upper edge there is no fundamental in that band.
    // Its vanishing denominator is not evidence of a brighter vowel, so those
    // frames must not set the reference for lower notes in the same syllable.
    if (c[i] > 0 && (!registerLeap || pitch.frames[first + i].f0 <= 400) && f.total[first + i] >= peak - 20)
      balance.push_back(f.mid[first + i] - f.low[first + i]);
  float vowelBalance = -1e9f;
  if (!balance.empty()) {
    std::nth_element(balance.begin(), balance.begin() + static_cast<long>(balance.size() / 2), balance.end());
    vowelBalance = balance[balance.size() / 2];
  }
  std::vector<float> voiced;
  for (size_t i = 0; i < c.size(); ++i) {
    if (f.total[first + i] < peak - 20) c[i] = 0;  // release tails and onset consonants say nothing about the note
    if (f.mid[first + i] - f.low[first + i] < vowelBalance - kMurmurDb) c[i] = 0;
    if (c[i] > 0) voiced.push_back(c[i]);
  }
  if (voiced.size() < rules.minSyllable) return out;
  // consistentContour has already reconciled uncertain octave excursions while
  // preserving sustained changes of register. Do not fold genuine leaps here.
  // Median filter (40 ms) against single-frame jitter.
  std::vector<float> m(c.size(), 0);
  for (size_t i = 0; i < c.size(); ++i) {
    if (c[i] <= 0) continue;
    float w[5];
    size_t count = 0;
    for (size_t j = i >= 2 ? i - 2 : 0; j <= i + 2 && j < c.size(); ++j)
      if (c[j] > 0) w[count++] = c[j];
    // Only the median matters (and std::sort here trips a false GCC -Warray-bounds).
    std::nth_element(w, w + count / 2, w + count);
    m[i] = w[count / 2];
  }
  // Plateaus are found on the contour's centre line: a held note with vibrato
  // is flat there, while on the contour its held runs are the swings' turns.
  const std::vector<float> line = vibratoCentreLine(m);
  // Piecewise-constant fit: least squares plus a cost per extra note, every
  // piece at least 120 ms of voiced frames.
  const size_t n = m.size();
  std::vector<double> s1(n + 1, 0), s2(n + 1, 0), cnt(n + 1, 0);
  for (size_t i = 0; i < n; ++i) {
    const bool v = m[i] > 0;
    s1[i + 1] = s1[i] + (v ? m[i] : 0);
    s2[i + 1] = s2[i] + (v ? double(m[i]) * m[i] : 0);
    cnt[i + 1] = cnt[i] + (v ? 1 : 0);
  }
  auto sse = [&](size_t a, size_t b) {  // [a, b)
    const double k = cnt[b] - cnt[a];
    if (k <= 0) return 0.0;
    const double s = s1[b] - s1[a];
    return s2[b] - s2[a] - s * s / k;
  };
  auto mean = [&](size_t a, size_t b) {
    const double k = cnt[b] - cnt[a];
    return k > 0 ? (s1[b] - s1[a]) / k : 0.0;
  };
  const size_t minLen = rules.minPiece;  // voiced frames (natural: 120 ms) per plateau
  const size_t maxLen = 750;    // frames (6 s) per piece keeps this linear; equal neighbours merge below
  std::vector<double> best(n + 1, 1e18);
  std::vector<size_t> from(n + 1, 0);
  best[0] = 0;
  for (size_t b = 1; b <= n; ++b) {
    for (size_t a = b > maxLen ? b - maxLen : 0; a < b; ++a) {
      if (best[a] >= 1e17) continue;
      if (cnt[b] - cnt[a] < minLen && !(a == 0 && b == n) && b - a < maxLen) continue;
      const double cost = best[a] + sse(a, b) + (a ? kMelismaCost : 0);
      if (cost < best[b]) best[b] = cost, from[b] = a;
    }
  }
  std::vector<size_t> cuts;  // piece starts
  for (size_t b = n; b > 0; b = from[b]) cuts.push_back(from[b]);
  std::reverse(cuts.begin(), cuts.end());
  if (cuts.size() < 2) return out;
  // Validate: settled plateaus far enough apart. A short or gliding first
  // piece is a scoop, a short, gliding or fading last piece a fall-off, a short
  // excursion that returns (A-B-A) an ornament or vibrato: all one note. A
  // scoop or fall-off that joins a settled note leaves that note's pitch alone,
  // so each piece keeps the core its pitch and drift are measured on.
  struct Piece {
    size_t a, b;          // frames [a, b) of the window
    size_t coreA, coreB;  // the frames that set its pitch: [a, b) without the scoops and fall-offs it absorbed
    double centre, drift;
    float level;
    size_t held, heldA, heldB;  // longest plateau inside [a, b): frames, [heldA, heldB) of the window
    double plateau;             // its mean pitch
    size_t firm;                // longest run where the contour and its centre line both stay in one held band
  };
  auto makePiece = [&](size_t a, size_t b, size_t coreA, size_t coreB) {
    Piece p{a, b, coreA, coreB, mean(coreA, coreB), 0, -1e9f, 0, a, a, 0, 0};
    // Longest run of informative frames whose values stay within the held band
    // (a sliding window kept by monotonic deques of its minimum and maximum);
    // with `contour` the median-filtered contour must stay in the band too.
    auto longestHold = [&](bool contour, size_t& runA, size_t& runB) {
      std::vector<size_t> idx;
      for (size_t i = a; i < b; ++i)
        if (line[i] > 0) idx.push_back(i);
      auto low = [&](size_t j) { return contour ? std::min(line[idx[j]], m[idx[j]]) : line[idx[j]]; };
      auto high = [&](size_t j) { return contour ? std::max(line[idx[j]], m[idx[j]]) : line[idx[j]]; };
      size_t best = 0;
      std::deque<size_t> lo, hi;
      for (size_t i = 0, j = 0; j < idx.size(); ++j) {
        while (!lo.empty() && low(lo.back()) >= low(j)) lo.pop_back();
        while (!hi.empty() && high(hi.back()) <= high(j)) hi.pop_back();
        lo.push_back(j), hi.push_back(j);
        // (A frame whose contour lies outside the band around its line holds nothing.)
        while (i <= j && high(hi.front()) - low(lo.front()) > 2 * kHeldTolerance) {
          ++i;
          if (lo.front() < i) lo.pop_front();
          if (hi.front() < i) hi.pop_front();
        }
        if (j + 1 - i > best) best = j + 1 - i, runA = idx[i], runB = idx[j] + 1;
      }
      return best;
    };
    {
      p.held = longestHold(false, p.heldA, p.heldB);
      double sum = 0;
      for (size_t i = p.heldA; i < p.heldB; ++i)
        if (line[i] > 0) sum += line[i];
      p.plateau = p.held ? sum / static_cast<double>(p.held) : p.centre;
    }
    // A note sung with vibrato starts and ends off its centre line, whose
    // one-cycle mean there blends the swing with the approach to it: the
    // line can sit flat for ~100 ms where the voice never holds (a leap into
    // vibrato read as a held approach a semitone below). The contour itself
    // holds on a real approach, grace note or droop.
    if (rules.firmHolds) {
      size_t unusedA = 0, unusedB = 0;
      p.firm = longestHold(true, unusedA, unusedB);
    }
    const size_t len = coreB - coreA;
    double st = 0, stt = 0, sc = 0, stc = 0, k = 0;  // least-squares slope over the middle 60% (cuts sit mid-transition)
    for (size_t i = coreA + len / 5; i < coreB - len / 5; ++i) {
      if (m[i] <= 0) continue;
      const double t = static_cast<double>(i);
      st += t, stt += t * t, sc += m[i], stc += t * m[i], k += 1;
    }
    const double den = k * stt - st * st;
    if (k >= 3 && den > 0) p.drift = (k * stc - st * sc) / den * static_cast<double>(len);
    std::vector<float> lv;
    for (size_t i = coreA; i < coreB; ++i) lv.push_back(f.total[first + i]);
    std::nth_element(lv.begin(), lv.begin() + static_cast<long>(lv.size() / 2), lv.end());
    p.level = lv[lv.size() / 2];
    return p;
  };
  std::vector<Piece> pieces;
  for (size_t i = 0; i < cuts.size(); ++i) {
    const size_t b = i + 1 < cuts.size() ? cuts[i + 1] : n;
    pieces.push_back(makePiece(cuts[i], b, cuts[i], b));
  }
  // Vibrato extent: half the P90-P10 range of the contour around its pieces.
  std::vector<float> resid;
  for (const auto& p : pieces)
    for (size_t i = p.a; i < p.b; ++i)
      if (m[i] > 0) resid.push_back(static_cast<float>(m[i] - p.centre));
  std::sort(resid.begin(), resid.end());
  const double extent = resid.empty() ? 0 : 0.5 * (resid[resid.size() * 9 / 10] - resid[resid.size() / 10]);
  const double minStep = std::max(kMelismaStep, 1.2 * extent);
  // Pieces i and i + 1 become one note. `keep` (0 or 1) names the note a
  // scoop, fall-off or glide joins: it keeps its core when it is settled
  // (drifts less than half a semitone); otherwise both pieces set the pitch.
  auto mergeAt = [&](size_t i, int keep = -1) {
    const Piece& l = pieces[i];
    const Piece& r = pieces[i + 1];
    if (keep >= 0 && std::fabs((keep ? r : l).drift) > 0.5) keep = -1;
    pieces[i] = makePiece(l.a, r.b, keep == 1 ? r.coreA : l.coreA, keep == 0 ? l.coreB : r.coreB);
    pieces.erase(pieces.begin() + static_cast<long>(i) + 1);
  };
  // How long a piece sounds: from its first to its last informative frame (a
  // voicing gap or murmur before or after it is not part of the note).
  auto sounding = [&](const Piece& p) {
    size_t lo = p.b, hi = p.a;
    for (size_t i = p.a; i < p.b; ++i)
      if (m[i] > 0) lo = std::min(lo, i), hi = i + 1;
    return hi > lo ? hi - lo : size_t(0);
  };
  const size_t shortEdge = 25;   // frames (200 ms): scoops and fall-offs are shorter
  // The pitch a piece holds, for steps between notes: its plateau when it has
  // one (the piece mean also counts the half transitions at its cuts and so
  // shrinks short notes' steps: a sung 64-63-64 turn measured 0.6 st), else its centre.
  auto pitchOf = [&](const Piece& p) { return p.held >= rules.heldFrames ? p.plateau : p.centre; };
  // A short edge piece that holds a pitch is no scoop or fall-off (see
  // kHeldHeadRise / kHeldRise). `step` is the next piece's centre minus the head's, or the
  // tail's minus the previous piece's (centres, not plateaus: a held start's
  // transition counts toward the settling it is part of; plateau steps added
  // two settling splits on dev).
  // A piece holds a pitch of its own for `frames`: its plateau on the centre
  // line, and with firmHolds its contour too (see Piece::firm).
  auto holds = [&](const Piece& p, size_t frames) { return p.held >= frames && (!rules.firmHolds || p.firm >= frames); };
  auto heldHead = [&](const Piece& p, double step) {
    return holds(p, rules.heldFrames) && (step >= rules.headRise || -step >= rules.headDrop);
  };
  auto heldTail = [&](const Piece& p, double step) {
    return holds(p, rules.heldFrames) && (step >= rules.tailRise || -step >= rules.tailDrop);
  };
  auto medianLevel = [&](size_t a, size_t b) {
    std::vector<float> l;
    for (size_t i = a; i < b; ++i)
      if (m[i] > 0) l.push_back(f.total[first + i]);
    if (l.empty()) return -1e9f;
    std::nth_element(l.begin(), l.begin() + static_cast<long>(l.size() / 2), l.end());
    return l[l.size() / 2];
  };
  auto softNote = [&](const Piece& p) {
    const size_t half = (p.heldA + p.heldB) / 2;
    return holds(p, rules.softFrames) && medianLevel(half, p.heldB) >= medianLevel(p.heldA, half) - kFadeDb;
  };
  const size_t shortInner = 15;  // frames (120 ms)
  // Edges first (a release droop or a scoop must not be mistaken for a step),
  // then short inner pieces, then steps too small to be a new note.
  bool changed = true;
  while (changed && pieces.size() > 1) {
    changed = false;
    // A glide is a piece whose own drift covers much of the step (toward the
    // next note for a scoop, away from the previous one for a fall-off).
    auto glides = [&](const Piece& p, double step, size_t maxSpan) {
      const double toward = p.drift * (step > 0 ? 1 : -1);
      return toward >= std::fabs(step) || (p.b - p.a < maxSpan && toward >= 0.5 * std::fabs(step));
    };
    const Piece& tail = pieces.back();
    const double tailStep = tail.centre - pieces[pieces.size() - 2].centre;
    const bool quiet = tail.level < peak - 12 && !softNote(tail);
    // Detailed: a fall-off that settles on a pitch of its own (held, away from
    // the note) is a droop note.
    const bool tailGlides = glides(tail, tailStep, 44) && !(rules.heldGlides && heldTail(tail, tailStep));
    if (quiet || (sounding(tail) < shortEdge && !heldTail(tail, tailStep)) || tailGlides) {
      // A fall-off continues the note's voicing. A tail after a voicing gap is
      // another sound (an instrument under a released note, a breath): the note
      // ends at the gap, so the tail neither becomes a note nor sets its pitch.
      size_t sung = 0, gap = 0, run = 0;
      for (size_t i = pieces[pieces.size() - 2].a; i < tail.a; ++i)
        if (m[i] > 0) sung = i + 1;
      for (size_t i = sung; sung > 0 && i < tail.b && m[i] <= 0; ++i) {
        run = pitch.frames[first + i].midi > 0 ? 0 : run + 1;
        gap = std::max(gap, run);
      }
      if (gap >= kTailGap && sounding(tail) > 0) {
        sungEnd = first + sung;
        pieces.pop_back();
        changed = true;
        continue;
      }
      mergeAt(pieces.size() - 2, 0);
      changed = true;
      continue;
    }
    const Piece& head = pieces.front();
    const double headStep = pieces[1].centre - head.centre;
    // Detailed: a scoop that holds a pitch before it rises is an approach note.
    const bool headGlides = glides(head, headStep, 38) && !(rules.heldGlides && heldHead(head, headStep));
    if ((sounding(head) < shortEdge && !heldHead(head, headStep)) || headGlides) {
      mergeAt(0, 1);
      changed = true;
      continue;
    }
    for (size_t i = 1; i + 1 < pieces.size() && !changed; ++i) {
      // Short, or a glide whose own drift covers most of the way between its
      // neighbours (only a piece between them can be a glide from one to the
      // other: a turn such as 58-60-58 is a note however close its ends are).
      const double span = std::fabs(pieces[i + 1].centre - pieces[i - 1].centre);
      const bool between = (pieces[i].centre - pieces[i - 1].centre) * (pieces[i + 1].centre - pieces[i].centre) > 0;
      // (Natural pieces always last shortInner; detailed ones can be shorter, and
      // a short one that holds its pitch is a grace or passing note.)
      const bool shortPiece = pieces[i].b - pieces[i].a < shortInner && !(rules.heldGrace && holds(pieces[i], rules.heldFrames));
      if (!shortPiece && !(between && std::fabs(pieces[i].drift) >= 0.5 * span)) continue;
      const bool left = std::fabs(pieces[i].centre - pieces[i - 1].centre) <= std::fabs(pieces[i].centre - pieces[i + 1].centre);
      if (left) mergeAt(i - 1, 0);
      else mergeAt(i, 1);
      changed = true;
    }
    for (size_t i = 0; i + 1 < pieces.size() && !changed; ++i) {
      // A short excursion that returns (A-B-A) is an ornament or a vibrato
      // swing unless it holds its own pitch (a neighbour note in a turn).
      const bool returns = i + 2 < pieces.size() && pieces[i + 1].b - pieces[i + 1].a < 31 &&
                           !holds(pieces[i + 1], rules.heldFrames) && std::fabs(pitchOf(pieces[i + 2]) - pitchOf(pieces[i])) < 0.5;
      // Plateaus are measured on the centre line, so vibrato does not widen the step they need.
      const bool plateaus = pieces[i].held >= rules.heldFrames && pieces[i + 1].held >= rules.heldFrames;
      const double need = plateaus ? kMelismaStep : minStep;
      if (std::fabs(pitchOf(pieces[i + 1]) - pitchOf(pieces[i])) < need || returns) {
        mergeAt(i);
        changed = true;
      }
    }
  }
  for (size_t i = 1; i < pieces.size(); ++i) out.push_back(first + pieces[i].a);
  return out;
}

// ---------------------------------------------------------------------------
// The DP

struct Position {
  size_t frame = 0;      // vowel onset (node) or phrase end
  size_t soundEnd = 0;   // end of the preceding sound
  int phrase = -1;
  bool isEnd = false;    // phrase end
  bool isStart = false;  // phrase start
  int candidate = -1;    // into candidates
};

// What a DP step between two positions sings.
enum class Segment : unsigned char {
  None,
  Syllables,  // the next lyric syllable(s), one note (plus melisma notes)
  Hum,        // voiced audio no word claims: a lyric-less note
  Drop,       // quiet sound (tails, breaths), or a phrase's lead-in: no note
  Skip,       // whole words left unassigned
  Gap,        // the rest between two phrases
};

struct Back {
  int pos = -1;
  int s = -1;
  signed char h = 0;
  Segment label = Segment::None;
  unsigned char count = 0;  // syllables in a Syllables segment
};

struct Item {
  size_t from = 0, to = 0;                   // positions
  size_t start = 0, end = 0, soundEnd = 0;  // frames
  Segment label = Segment::None;             // Syllables or Hum
  size_t firstSyl = 0, count = 0;
};

}  // namespace

SegmentationResult segmentSinging(const std::vector<float>& x16k, const PitchAnalysis& pitch,
                                  const std::vector<Word>& words, const Lexicon& lexicon,
                                  const MelodyOptions& opts) {
  SegmentationResult r;
  // No early return for a take too short for one frame: the DP below then has no
  // positions and still reports every word as unassigned, as for silence.
  const ArticulationFeatures f = analyzeArticulation(x16k, pitch);
  const double duration = std::max(pitch.duration, static_cast<double>(x16k.size()) / kAnalysisRate);

  const std::vector<Phrase> phrases = findPhrases(f);
  const std::vector<BoundaryCandidate> cands = findBoundaryCandidates(f, pitch);

  std::vector<Syl> syl;
  std::vector<WordSlot> slots;
  lyricInventory(words, lexicon, duration, syl, slots, r);
  // Words far too few for what was sung where they say it was sung (a
  // recogniser's one-word guess at a language it was not set to): their few
  // syllables would claim a note or two and leave the rest to hums, so the take
  // is segmented as if it had no words. Every phrase start and every clear
  // onset inside a phrase counts as a sung syllable onset; only the span the
  // words cover counts, so a transcript that stops after the first verse
  // keeps its lyrics there. A recogniser that skips lines in the middle (common
  // on singing) looks the same in that count: word times tile the take (a word
  // starts where the previous one ends), so the word after the gap stretches
  // over the lines it did not hear. So the transcript is dropped only when at
  // least half of its words are stretched like that themselves, each holding
  // far more onsets than its syllables; a few such words are unheard lines,
  // which the segmentation leaves as hums while the other words keep their lyrics.
  bool thin = false;
  if (!syl.empty()) {
    std::vector<size_t> onsetFrames;  // phrase starts and the clear onsets inside phrases
    for (size_t p = 0, ci = 0; p < phrases.size(); ++p) {
      onsetFrames.push_back(phrases[p].first);
      while (ci < cands.size() && cands[ci].frame < phrases[p].first + 5) ++ci;
      for (; ci < cands.size() && cands[ci].frame + 5 < phrases[p].end; ++ci)
        if (cands[ci].any > kClearOnset) onsetFrames.push_back(cands[ci].frame);
    }
    const auto onsetsIn = [&](double from, double to) {
      return static_cast<size_t>(std::count_if(onsetFrames.begin(), onsetFrames.end(), [&](size_t k) {
        return timeOf(k) >= from && timeOf(k) <= to;
      }));
    };
    const auto tooFew = [](size_t syllables, size_t onsets) {
      return static_cast<float>(syllables) < kThinTranscript * static_cast<float>(onsets);
    };
    double from = duration, to = 0;
    size_t timed = 0, stretched = 0;
    for (const auto& w : slots) {
      const Word& word = words[w.index];
      if (!std::isfinite(word.start) || !std::isfinite(word.end)) continue;
      from = std::min(from, word.start), to = std::max(to, word.end);
      ++timed;
      if (tooFew(w.count, onsetsIn(word.start, word.end))) ++stretched;
    }
    if (to <= from) from = 0, to = duration;  // no usable times: the whole take
    const size_t onsets = onsetsIn(from, to);
    if (onsets >= kThinMinOnsets && tooFew(syl.size(), onsets) && 2 * stretched >= timed) {
      for (const auto& w : slots) r.unassignedWords.push_back(w.index);
      r.warnings.push_back("the transcript has " + std::to_string(syl.size()) + " syllable(s) for about " +
                           std::to_string(onsets) + " sung ones and cannot be the lyrics; imported melody only, without lyrics");
      syl.clear();
      slots.clear();
      thin = true;
    }
  }
  const size_t S = syl.size();
  std::vector<int> wordAt(S + 1, -1);  // slot starting at syllable index s (word boundary), else -1
  for (size_t k = 0; k < slots.size(); ++k) wordAt[slots[k].first] = static_cast<int>(k);
  auto isBoundary = [&](size_t s) { return s == S || wordAt[s] >= 0; };

  // Allowed syllable counts at each time, from the anchors (keeps the DP linear).
  std::vector<double> earliest(S, -1e9), latest(S, 1e9);
  {
    double prevAnchor = -1e9;
    for (const auto& w : slots) {
      for (size_t i = 0; i < w.count; ++i) earliest[w.first + i] = prevAnchor - kAnchorMargin;
      if (w.end >= 0) prevAnchor = std::max(prevAnchor, w.end);
    }
    double nextAnchor = 1e9;
    for (size_t k = slots.size(); k-- > 0;) {
      const auto& w = slots[k];
      if (w.end >= 0) nextAnchor = std::min(nextAnchor, w.end);
      for (size_t i = 0; i < w.count; ++i) latest[w.first + i] = nextAnchor + kAnchorMargin;
    }
    for (size_t s = 1; s < S; ++s) earliest[s] = std::max(earliest[s], earliest[s - 1]);
    for (size_t s = S; s-- > 1;) latest[s - 1] = std::min(latest[s - 1], latest[s]);
  }
  auto bandAt = [&](double t, size_t& lo, size_t& hi) {
    lo = static_cast<size_t>(std::lower_bound(latest.begin(), latest.end(), t) - latest.begin());
    hi = static_cast<size_t>(std::upper_bound(earliest.begin(), earliest.end(), t) - earliest.begin());
    if (hi < lo) hi = lo;
  };

  // Positions: phrase starts and ends, and the candidate onsets inside phrases.
  std::vector<Position> pos;
  std::vector<size_t> phraseStartPos, phraseEndPos;
  {
    size_t ci = 0;
    for (size_t p = 0; p < phrases.size(); ++p) {
      Position start;
      start.frame = start.soundEnd = phrases[p].first;
      start.phrase = static_cast<int>(p);
      start.isStart = true;
      phraseStartPos.push_back(pos.size());
      pos.push_back(start);
      while (ci < cands.size() && cands[ci].frame < phrases[p].first + 5) ++ci;
      for (; ci < cands.size() && cands[ci].frame + 5 < phrases[p].end; ++ci) {
        Position c;
        c.frame = cands[ci].frame;
        c.soundEnd = cands[ci].soundEnd;
        c.phrase = static_cast<int>(p);
        c.candidate = static_cast<int>(ci);
        pos.push_back(c);
      }
      Position end;
      end.frame = end.soundEnd = phrases[p].end;
      end.phrase = static_cast<int>(p);
      end.isEnd = true;
      phraseEndPos.push_back(pos.size());
      pos.push_back(end);
    }
  }
  // Peak levels from each position to the next: total (the drop rule) and
  // vowel band (a hum needs a vowel, not just a nasal murmur or a breath).
  std::vector<float> peakFrom(pos.size(), -1e9f), vowelFrom(pos.size(), -1e9f), phraseVowel(phrases.size(), -1e9f);
  for (size_t i = 0; i + 1 < pos.size(); ++i)
    for (size_t k = pos[i].frame; k < pos[i + 1].frame && k < f.frames; ++k) {
      peakFrom[i] = std::max(peakFrom[i], f.total[k]);
      vowelFrom[i] = std::max(vowelFrom[i], f.mid[k]);
    }
  // A phrase end's span is the rest before the next phrase (breaths, noise): not
  // part of the phrase, and counting it could leave no hum loud enough to tile
  // the phrase, so the DP would reach no final state.
  for (size_t i = 0; i < pos.size(); ++i)
    if (!pos[i].isEnd)
      phraseVowel[static_cast<size_t>(pos[i].phrase)] = std::max(phraseVowel[static_cast<size_t>(pos[i].phrase)], vowelFrom[i]);
  std::vector<float> peakToEnd(pos.size(), -1e9f), vowelToEnd(pos.size(), -1e9f);  // from a position to its phrase end
  for (size_t i = pos.size(); i-- > 0;)
    if (!pos[i].isEnd) peakToEnd[i] = std::max(peakFrom[i], peakToEnd[i + 1]), vowelToEnd[i] = std::max(vowelFrom[i], vowelToEnd[i + 1]);

  // DP tables: V[pos][2 * (s - lo[pos]) + h], h = 1 when the last segment was a
  // hum (a run of unclaimed vocables continues more cheaply than it starts).
  const size_t P = pos.size();
  std::vector<size_t> lo(P + 2), hi(P + 2);
  std::vector<std::vector<float>> V(P + 2);
  std::vector<std::vector<Back>> B(P + 2);
  const size_t START = P, FINAL = P + 1;
  for (size_t i = 0; i < P; ++i) bandAt(timeOf(pos[i].frame), lo[i], hi[i]);
  lo[START] = 0, hi[START] = S;
  lo[FINAL] = 0, hi[FINAL] = S;
  for (size_t i = 0; i < P + 2; ++i) {
    V[i].assign(2 * (hi[i] - lo[i] + 1), kInf);
    B[i].assign(2 * (hi[i] - lo[i] + 1), Back());
  }
  // A state below the target's band (its words' anchors are long past) can
  // still continue by leaving those words out; the skipped words are implied
  // by the gap in syllable indices when backtracking.
  auto at = [&](size_t p, size_t s, int h) { return 2 * (s - lo[p]) + static_cast<size_t>(h); };
  auto relax = [&](size_t to, size_t s, int h, float cost, int fromPos, int fromS, int fromH, Segment label,
                   unsigned char count) {
    while (s < lo[to]) {
      if (s >= S || wordAt[s] < 0) return;
      const auto& w = slots[static_cast<size_t>(wordAt[s])];
      cost += w.skip;
      s += w.count;
    }
    if (s > hi[to]) return;
    float& v = V[to][at(to, s, h)];
    if (cost < v) {
      v = cost;
      B[to][at(to, s, h)] = Back{fromPos, fromS, static_cast<signed char>(fromH), label, count};
    }
  };
  // Leaving whole words out at position p, from any word boundary to any later
  // one. The cheapest run of skips reaching each boundary is carried along in one
  // pass (linear in the band, which holds every word when anchors are missing);
  // like walking every run from its first boundary, equal costs keep the earliest.
  auto skipFrom = [&](size_t p) {
    for (int h = 0; h < 2; ++h) {
      float carried = kInf;  // cost of reaching boundary s by skips that left from `origin`
      size_t origin = 0;
      for (size_t s = lo[p]; s < S && s < hi[p]; ++s) {
        if (wordAt[s] < 0) continue;
        const auto& w = slots[static_cast<size_t>(wordAt[s])];
        if (s + w.count > hi[p]) break;
        const float own = V[p][at(p, s, h)];
        if (std::isfinite(own) && !(carried + w.skip <= own + w.skip)) carried = own, origin = s;
        carried += w.skip;
        if (std::isfinite(carried))
          relax(p, s + w.count, h, carried, static_cast<int>(p), static_cast<int>(origin), h, Segment::Skip, 0);
      }
    }
  };
  auto gapTo = [&](size_t from, size_t to) {  // silence between phrases: nothing changes
    for (size_t s = lo[from]; s <= hi[from]; ++s)
      for (int h = 0; h < 2; ++h) {
        const float cost = V[from][at(from, s, h)];
        if (std::isfinite(cost)) relax(to, s, h, cost, static_cast<int>(from), static_cast<int>(s), h, Segment::Gap, 0);
      }
  };
  V[START][0] = 0;
  skipFrom(START);
  if (P == 0) {
    gapTo(START, FINAL);
  } else {
    gapTo(START, 0);
  }

  auto chooseCost = [&](const Position& a, BoundaryKind kind, bool hum) -> float {
    if (a.isStart || a.candidate < 0) return 0;
    const auto& c = cands[static_cast<size_t>(a.candidate)];
    // Class evidence can only raise the onset probability: on dev, onset-class
    // models were no better than the class-free one (the expected consonant is
    // often reduced or the dictionary class wrong), and penalising mismatches hurt.
    const float p = hum ? c.any : std::max(c.evidence[static_cast<size_t>(kind)], c.any);
    return -std::log(std::max(0.02f, p)) + std::log(std::max(0.03f, 1 - c.any));
  };

  // Anything sung after word w-1 (the next word's first syllable or a hum
  // before it, at syllable index s) starts no earlier than w-1's ASR end.
  auto lateCost = [&](size_t s, double start) -> float {
    const size_t w = s < S ? syl[s].word : slots.size();
    if (w == 0) return 0;
    const WordSlot& prev = slots[w - 1];
    const double late = prev.lastLetters - start - kLateTolerance;
    return prev.end >= 0 && late > 0 ? prev.weight * std::min(kAnchorCap, static_cast<float>(late / kLateScale)) : 0.0f;
  };
  // Cost of syllables s..s+m-1 sung from position i to position j.
  struct Parts {
    float choose = 0, late = 0, merge = 0, duration = 0, early = 0, overrun = 0;
    float total() const { return choose + late + merge + duration + early + overrun; }
  };
  auto syllableCost = [&](size_t i, size_t j, size_t s, size_t m) {
    Parts c;
    const Position& a = pos[i];
    const Position& b = pos[j];
    const size_t soundEnd = std::max(b.soundEnd, a.frame + 1);
    const double ta = timeOf(a.frame), d = timeOf(b.frame - a.frame), sound = timeOf(soundEnd - a.frame);
    const Syl& first = syl[s];
    c.choose = chooseCost(a, first.kind, false);
    if (first.slotFirst) c.late = lateCost(s, ta);
    else if (first.notBefore >= 0 && first.notBefore - ta - kLateTolerance > 0)
      c.late = kTokenWeight * slots[first.word].weight *
               std::min(kAnchorCap, static_cast<float>((first.notBefore - ta - kLateTolerance) / kLateScale));
    for (size_t k = 1; k < m; ++k)
      c.merge += syl[s + k].hiatus ? kMergeHiatus : syl[s + k].optional ? kMergeOptional : kMergeOther;
    // Duration prior: onset to next onset for "too short", length of the sound
    // (rests excluded) for "too long"; a phrase's last note may be long.
    const double mu = kSyllableMu * (1 + 0.5 * static_cast<double>(m - 1));
    const double ld = std::log(d / mu), ls = std::log(std::max(sound, kMinSegment) / mu);
    if (ld < 0) c.duration = static_cast<float>(ld * ld / (2 * kSyllableSigma * kSyllableSigma));
    else if (!b.isEnd && ls > 0) c.duration = static_cast<float>(ls * ls / (2 * kLongSigma * kLongSigma));
    const Syl& last = syl[s + m - 1];
    const WordSlot& w = slots[last.word];
    if (last.slotLast && w.end >= 0) {  // the word ends: its sound ends near its ASR end
      const double early = timeOf(soundEnd) - kEarlyTolerance - w.end;
      if (early > 0) c.early = static_cast<float>(early / kEarlyScale);
      // ... and its last syllable starts before it. Without this a missed
      // legato onset shifts the following words a syllable late each until a
      // clear onset catches up, as the late anchors only bound the other side.
      const double overrun = ta - kOverrunTolerance - w.end;
      if (overrun > 0) c.overrun = static_cast<float>(overrun / kOverrunScale);
      const float total = c.early + c.overrun;
      const float scale = w.weight * (total > kWordEndCap ? kWordEndCap / total : 1.0f);
      c.early *= scale, c.overrun *= scale;
    }
    return c;
  };
  // A hum after a hum (a run of vocables the recogniser dropped) needs less
  // onset evidence than one that leaves the lyrics; without any words, every
  // note is a hum and needs a clear onset.
  auto humCost = [&](size_t i, size_t j, int afterHum) {
    const float base = S == 0 ? kHumAlone : afterHum ? kHumNext : kHumBase;
    return chooseCost(pos[i], BoundaryKind::Vowel, true) + base +
           kHumPerSecond * static_cast<float>(timeOf(pos[j].frame - pos[i].frame));
  };

  // Segment lengths are compared in whole frames: 88 ms and 160 ms are exact
  // multiples of a frame, and a difference of two absolute times lands on either
  // side of them depending on position and compiler (FMA contraction).
  const size_t minFrames = framesOf(kMinSegment), maxFrames = framesOf(kMaxSegment);
  const size_t leadInFrames = framesOf(kLeadIn), shortTailFrames = framesOf(kShortTail);
  for (size_t i = 0; i < P; ++i) {
    const Position& a = pos[i];
    if (a.isEnd) {
      skipFrom(i);
      const size_t next = static_cast<size_t>(a.phrase) + 1 < phrases.size() ? phraseStartPos[static_cast<size_t>(a.phrase) + 1] : FINAL;
      gapTo(i, next);
      continue;
    }
    skipFrom(i);
    bool any = false;
    for (float v : V[i]) any |= std::isfinite(v);
    if (!any) continue;
    const double ta = timeOf(a.frame);
    const size_t phraseEnd = phraseEndPos[static_cast<size_t>(a.phrase)];
    float peak = -1e9f, vowel = -1e9f;  // over [a, b)
    for (size_t j = i + 1; j <= phraseEnd; ++j) {
      const Position& b = pos[j];
      const size_t span = b.frame - a.frame;
      if (!b.isEnd && span > maxFrames && j > i + 1) {  // only the phrase end is still reachable
        peak = std::max(peak, peakToEnd[j - 1]);
        vowel = std::max(vowel, vowelToEnd[j - 1]);
        j = phraseEnd - 1;
        continue;
      }
      peak = std::max(peak, peakFrom[j - 1]);
      vowel = std::max(vowel, vowelFrom[j - 1]);
      if (span < minFrames && !(a.isStart && b.isEnd)) continue;
      // The phrase's first vowel may begin at a clear onset just after the
      // first audible sound (a sonorant or voiced consonant leads in).
      if (a.isStart && !b.isEnd && span <= leadInFrames) {
        for (size_t s = lo[i]; s <= hi[i]; ++s)
          for (int h = 0; h < 2; ++h) {
            const float base = V[i][at(i, s, h)];
            if (std::isfinite(base))
              relax(j, s, h, base, static_cast<int>(i), static_cast<int>(s), h, Segment::Drop, 0);
          }
      }
      const float hum[2] = {humCost(i, j, 0), humCost(i, j, 1)};
      // Quiet next to the singing around it (not just within a quiet phrase).
      const bool droppable = peak < f.nearbyLoud[a.frame] - kDropBelowLoud;
      // A hum needs a vowel (not just a nasal murmur or breath). A short last
      // piece of a phrase is its release, and between lyrics a short one is the
      // next syllable's onset consonant (its start and the vowel's both look like
      // onsets): not a new note. Without words every note is a hum.
      const bool humVowel = vowel >= phraseVowel[static_cast<size_t>(a.phrase)] - kHumVowelBelow &&
                            (span >= shortTailFrames || (a.isStart && b.isEnd) || (S == 0 && !b.isEnd));
      for (size_t s = lo[i]; s <= hi[i]; ++s)
        for (int h = 0; h < 2; ++h) {
          const float base = V[i][at(i, s, h)];
          if (!std::isfinite(base)) continue;
          const int fs = static_cast<int>(s), fi = static_cast<int>(i);
          if (isBoundary(s)) {
            if (humVowel) relax(j, s, 1, base + hum[h] + lateCost(s, ta), fi, fs, h, Segment::Hum, 0);
            if (droppable) relax(j, s, h, base + kDropCost, fi, fs, h, Segment::Drop, 0);
          }
          for (size_t m = 1; s < S && m <= 4 && syl[s].inWord + m <= syl[s].ofWord; ++m)
            relax(j, s + m, 0, base + syllableCost(i, j, s, m).total(), fi, fs, h, Segment::Syllables,
                  static_cast<unsigned char>(m));
        }
    }
  }
  skipFrom(FINAL);

  // Backtrack.
  std::vector<Item> items;
  std::vector<char> placed(slots.size(), 0);
  const int finalH = V[FINAL][at(FINAL, S, 1)] < V[FINAL][at(FINAL, S, 0)] ? 1 : 0;
  const float finalCost = V[FINAL][at(FINAL, S, finalH)];
  if (std::isfinite(finalCost)) {
    size_t p = FINAL, s = S;
    int h = finalH;
    while (!(p == START && s == 0 && h == 0)) {
      const Back bk = B[p][at(p, s, h)];
      if (bk.pos < 0) break;
      const size_t from = static_cast<size_t>(bk.pos);
      if (bk.label == Segment::Syllables || bk.label == Segment::Hum) {
        Item it;
        it.from = from;
        it.to = p;
        it.start = pos[from].frame;
        it.end = pos[p].frame;
        it.soundEnd = std::max(pos[p].soundEnd, it.start + 1);
        it.label = bk.label;
        it.firstSyl = static_cast<size_t>(bk.s);
        it.count = bk.count;
        items.push_back(it);
        if (bk.label == Segment::Syllables) placed[syl[it.firstSyl].word] = 1;
      }
      p = from;
      s = static_cast<size_t>(bk.s);
      h = bk.h;
    }
    std::reverse(items.begin(), items.end());
  }
  for (size_t k = 0; k < slots.size(); ++k) {
    if (placed[k]) continue;
    r.unassignedWords.push_back(slots[k].index);
    r.warnings.push_back("'" + words[slots[k].index].text + "' was not matched to sung audio");
  }
  std::sort(r.unassignedWords.begin(), r.unassignedWords.end());

  // Nothing says how many notes unclaimed audio holds, so a hum is segmented
  // like sung syllables from the acoustics alone: it is cut at every onset
  // more likely than not (a re-attack, even on one pitch), with the DP's
  // limits on how short a note and how vowel-less a hum may be. The DP's own
  // price for a hum after a hum is higher because it also guards the lyrics
  // against being left for hums.
  if (S > 0) {
    std::vector<Item> split;
    for (const auto& it : items) {
      if (it.label != Segment::Hum || it.to <= it.from + 1) {
        split.push_back(it);
        continue;
      }
      const size_t a = it.from, L = it.to - it.from;
      const float phraseV = phraseVowel[static_cast<size_t>(pos[a].phrase)];
      std::vector<float> best(L + 1, kInf);
      std::vector<size_t> prev(L + 1, 0);
      best[0] = 0;
      for (size_t x = 0; x < L; ++x) {
        if (!std::isfinite(best[x])) continue;
        float vowel = -1e9f;
        for (size_t y = x + 1; y <= L; ++y) {
          vowel = std::max(vowel, vowelFrom[a + y - 1]);
          const Position& from = pos[a + x];
          const Position& to = pos[a + y];
          const size_t span = to.frame - from.frame;
          if (y < L || x > 0) {  // the hum as a whole was allowed by the DP
            if (span < minFrames || vowel < phraseV - kHumVowelBelow || (to.isEnd && span < shortTailFrames)) continue;
          }
          const float c = best[x] + (x ? chooseCost(from, BoundaryKind::Vowel, true) + kHumSplit : 0.0f);
          if (c < best[y]) best[y] = c, prev[y] = x;
        }
      }
      std::vector<size_t> cuts;
      for (size_t y = L; y > 0; y = prev[y]) cuts.push_back(y);
      std::reverse(cuts.begin(), cuts.end());
      size_t x = 0;
      for (size_t y : cuts) {
        Item sub = it;
        sub.from = a + x;
        sub.to = a + y;
        sub.start = pos[sub.from].frame;
        sub.end = pos[sub.to].frame;
        sub.soundEnd = y == L ? it.soundEnd : std::max(pos[sub.to].soundEnd, sub.start + 1);
        split.push_back(sub);
        x = y;
      }
    }
    items.swap(split);
  }

  // Notes: melisma splits inside each segment, then pitches.
  // Each note knows its sung word (a syllable's spoken word, or the hum it
  // sustains): the pitch module decides a word's octave from all its notes.
  std::vector<long long> spokenWord(S, 0);
  for (size_t k = 1; k < S; ++k) spokenWord[k] = spokenWord[k - 1] + (syl[k].inWord == 0 ? 1 : 0);
  struct Draft {
    size_t start, end;
    std::string lyric;
    float wordConfidence;
    long long word;
    BoundaryKind onset;  // the consonants before a syllable's vowel; Vowel for none, melisma notes and hums
  };
  std::vector<Draft> drafts;
  size_t hums = 0, humEnd = 0;  // unclaimed passages, and where the last hum ended
  long long humWord = static_cast<long long>(S);  // hum groups are numbered after the spoken words
  for (const auto& it : items) {
    size_t sound = it.soundEnd;  // weak frames (a syllable the path left unvoiced) sound on past the voicing break
    for (size_t k = sound; k < it.end && k < f.frames; ++k)
      if (f.weak[k]) sound = k + 1;
    const size_t soundEnd = std::min(it.end, std::max(sound, it.start + framesOf(kMinSegment)));
    std::vector<size_t> starts = {it.start};
    size_t sungEnd = soundEnd;
    for (size_t k : melismaStarts(pitch, f, it.start, soundEnd, sungEnd, kNaturalRules)) starts.push_back(k);
    if (opts.detail == "detailed") {
      // Detailed mode keeps every natural note and divides each one further:
      // its approach, grace notes and droop are the edges and inner pieces of
      // its own span. (The detailed rules over the whole syllable instead lost
      // 0.2-1.2% of the natural notes' starts per dev corpus, where the finer
      // fit cut elsewhere, and scored no better.) Where the sung sound ends
      // stays the natural pass's decision.
      std::vector<size_t> finer;
      for (size_t k = 0; k < starts.size(); ++k) {
        const size_t a = starts[k], b = k + 1 < starts.size() ? starts[k + 1] : sungEnd;
        size_t unused = b;
        finer.push_back(a);
        for (size_t x : melismaStarts(pitch, f, a, b, unused, kDetailedRules)) finer.push_back(x);
      }
      starts.swap(finer);
    }
    // A singer can re-attack the same vowel without changing pitch or adding
    // a dictionary syllable. Preserve only strong independent acoustic attacks:
    // spectral changes alone can be a diphthong or a vibrato swing. Both sides
    // must contain a natural note's worth of actual pitched sound, so an onset
    // consonant or short release cannot become an extra note.
    if (it.label == Segment::Syllables) {
      std::vector<size_t> voicedCount(sungEnd - it.start + 1, 0);
      for (size_t k = it.start; k < sungEnd; ++k)
        voicedCount[k - it.start + 1] = voicedCount[k - it.start] + (pitch.frames[k].midi > 0 ? 1 : 0);
      auto c = std::lower_bound(cands.begin(), cands.end(), it.start,
          [](const BoundaryCandidate& candidate, size_t frame) { return candidate.frame < frame; });
      for (; c != cands.end() && c->frame < sungEnd; ++c) {
        if (c->any < 0.85f || !(c->breakBefore >= 0.024f || (c->dipMid >= 8 && c->rise >= 5))) continue;
        auto next = std::lower_bound(starts.begin(), starts.end(), c->frame);
        if (next == starts.begin() || (next != starts.end() && *next == c->frame)) continue;
        const size_t before = *(next - 1), after = next != starts.end() ? *next : sungEnd;
        const size_t at = voicedCount[c->frame - it.start];
        if (at - voicedCount[before - it.start] < kNaturalRules.minPiece ||
            voicedCount[after - it.start] - at < kNaturalRules.minPiece) continue;
        starts.insert(next, c->frame);
      }
    }
    std::string text;
    float confidence = 1;
    if (it.label == Segment::Syllables) {
      for (size_t k = 0; k < it.count; ++k) text += syl[it.firstSyl + k].piece;
      const Syl& last = syl[it.firstSyl + it.count - 1];
      if (last.inWord + 1 < last.ofWord) text += "-";
      confidence = slots[last.word].probability;
    }
    if (it.label == Segment::Hum) {
      if (hums == 0 || humEnd != it.start) ++hums;  // hums that follow each other are one passage
      humEnd = it.end;
    }
    for (size_t k = 0; k < starts.size(); ++k) {
      Draft d;
      d.start = starts[k];
      d.end = k + 1 < starts.size() ? starts[k + 1] : std::max(starts[k] + 1, sungEnd);
      // A melisma inside a hum moves pitch without a new onset: "_" sustains
      // the hum where "" would start a new "mm". A take without words keeps
      // every note lyric-less.
      d.lyric = k == 0 ? text : (S > 0 ? "_" : "");
      d.wordConfidence = confidence;
      d.word = it.label == Segment::Syllables ? spokenWord[it.firstSyl] : humWord;
      d.onset = k == 0 && it.label == Segment::Syllables ? syl[it.firstSyl].kind : BoundaryKind::Vowel;
      drafts.push_back(d);
    }
    if (it.label != Segment::Syllables) ++humWord;
  }
  // The lyric alignment can put a note on an unvoiced consonant well before
  // its vowel. A strong later attack after a real break can correct that start,
  // provided the discarded prefix contains neither sustained pitch nor the
  // weak periodic sound that we deliberately retain for rough syllables.
  for (auto& d : drafts) {
    const size_t earliest = d.start + framesOf(0.080);
    const size_t latest = std::min(d.end, d.start + framesOf(0.320));
    auto c = std::lower_bound(cands.begin(), cands.end(), earliest,
        [](const BoundaryCandidate& candidate, size_t frame) { return candidate.frame < frame; });
    size_t replacement = d.start;
    float strongest = 0;
    for (; c != cands.end() && c->frame < latest; ++c) {
      if (c->any < 0.80f || c->breakBefore < 0.080f || c->any <= strongest) continue;
      size_t prefixSound = 0, suffixVoiced = 0;
      for (size_t k = d.start; k < c->frame; ++k)
        prefixSound += pitch.frames[k].midi > 0 || f.weak[k] ? 1 : 0;
      for (size_t k = c->frame; k < d.end; ++k) suffixVoiced += pitch.frames[k].midi > 0 ? 1 : 0;
      if (prefixSound > framesOf(0.024) || suffixVoiced < kNaturalRules.minPiece) continue;
      strongest = c->any;
      replacement = c->frame;
    }
    d.start = replacement;
  }
  // A legato note runs up to the next vowel onset, so it holds the next
  // syllable's onset consonant and the pitch moving into that syllable (a /g/
  // closure already rising to the next note). Those frames are the next
  // syllable's: the note's pitch and octave come from the frames before the
  // consonant, which starts at the vowel-band minimum before the next vowel
  // (the dip the onset model measures, within kOnsetDipReach).
  std::vector<NoteSpan> spans;
  for (size_t i = 0; i < drafts.size(); ++i) {
    const Draft& d = drafts[i];
    NoteSpan sp;
    sp.first = d.start;
    sp.last = std::max(d.start, std::min(d.end, f.frames) - 1);
    sp.word = d.word;
    if (i + 1 < drafts.size() && drafts[i + 1].start == d.end && drafts[i + 1].onset != BoundaryKind::Vowel && d.end <= f.frames) {
      const size_t next = d.end, from = std::max(d.start + kOwnVowel, next >= kOnsetDipReach ? next - kOnsetDipReach : 0);
      size_t m = next;
      for (size_t k = from; k < next; ++k)
        if (m == next || f.mid[k] < f.mid[m]) m = k;
      // The cut must leave the note its vowel: when every loud frame lies past the
      // dip, the note's onset was placed on a murmur before its own vowel.
      float kept = -200, cut = -200;
      for (size_t k = d.start; k < m && k < pitch.frames.size(); ++k) kept = std::max(kept, pitch.frames[k].db);
      for (size_t k = m; k < next && k < pitch.frames.size(); ++k) cut = std::max(cut, pitch.frames[k].db);
      if (m < next && kept >= cut - kOwnVowelDb) sp.last = m - 1;
    }
    spans.push_back(sp);
  }
  assignNotePitches(pitch, spans);
  std::vector<float> centres, weights;  // per note, for the take's pitch reference
  for (size_t i = 0; i < drafts.size(); ++i) {
    const auto& d = drafts[i];
    float centre = spans[i].center;
    // A hum or melisma note needs pitch the path voiced; only a syllable takes a
    // pitch from candidates alone (assignNotePitches) or from its neighbours.
    bool pathVoiced = false;
    for (size_t k = d.start; k < d.end && k < pitch.frames.size(); ++k) pathVoiced = pathVoiced || pitch.frames[k].midi > 0;
    if (!pathVoiced && (d.lyric.empty() || d.lyric == "_")) continue;
    if (centre <= 0) {
      if (d.lyric.empty() || d.lyric == "_") continue;  // nothing pitched to show
      for (size_t k = i; k-- > 0;)
        if (spans[k].center > 0) { centre = spans[k].center; break; }
      if (centre <= 0)
        for (size_t k = i + 1; k < spans.size(); ++k)
          if (spans[k].center > 0) { centre = spans[k].center; break; }
      if (centre <= 0) continue;
    }
    TranscribedNote n;
    n.note.start = timeOf(d.start);
    n.note.dur = std::max(kAnalysisHopSeconds, timeOf(d.end) - timeOf(d.start));
    n.note.pitch = centre;
    n.note.lyric = d.lyric;
    n.detectedPitch = centre;
    n.pitchSpread = spans[i].spread;
    n.confidence = std::clamp(spans[i].confidence, 0.0f, 1.0f) * d.wordConfidence;
    r.notes.push_back(n);
    // A pitch borrowed from a neighbour says nothing about the tuning.
    centres.push_back(centre);
    weights.push_back(spans[i].center > 0 ? static_cast<float>(std::min(kTuningHoldCap, n.note.dur)) *
                                                std::clamp(spans[i].confidence, 0.0f, 1.0f)
                                          : 0.0f);
  }
  r.tuning = estimateTuning(centres, weights, pitch.polyphony).offset;
  if (opts.roundPitches)
    for (auto& n : r.notes) n.note.pitch = std::round(n.detectedPitch - r.tuning);
  if (!words.empty() && !thin && hums > 0)
    r.warnings.push_back(std::to_string(hums) + " sung passage(s) matched no transcribed word; imported without lyrics");
  // Dev takes: chords under the voice 0.64-0.66 (pitches unreliable), guitar
  // bleed 0.30 (pitches fine), solo voice and stems <= 0.21.
  if (pitch.polyphony >= kPolyphonyWarning)
    r.warnings.push_back("accompaniment or a second voice detected; pitches may be unreliable");

  return r;
}

}  // namespace novox
