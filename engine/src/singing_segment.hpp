// singing-v2 note segmentation: lyric syllables decide where notes begin.
// Audio is first split into phrases, the expected syllables (from the words
// and the lexicon) are placed at the best acoustic syllable boundaries with
// soft ASR timing anchors, and pitch may only add notes inside a syllable for
// a real melisma. Voiced audio no word claims becomes lyric-less notes found
// from acoustics alone; without words the whole take is segmented that way.
#pragma once

#include <string>
#include <vector>

#include "lexicon.hpp"
#include "singing_pitch.hpp"
#include "spectrum.hpp"
#include "transcribe.hpp"

namespace novox {

struct SegmentationResult {
  std::vector<TranscribedNote> notes;      // seconds; lyric tokens as alignLyrics produces them ("re-", "mem-", "ber", "_", "")
  std::vector<std::string> warnings;
  std::vector<size_t> unassignedWords;     // indices into the words passed in
  float tuning = 0;                        // semitones from A440 the note pitches were rounded against (estimateTuning)
};

// `x16k` is the analysed audio (see resampleTo16k); `pitch` its analysis.
// `words` may be empty (melody only). Word starts are unreliable (ASR word
// windows tile the recording); word ends and token times are soft anchors.
SegmentationResult segmentSinging(const std::vector<float>& x16k, const PitchAnalysis& pitch,
                                  const std::vector<Word>& words, const Lexicon& lexicon,
                                  const MelodyOptions& opts);

}  // namespace novox
