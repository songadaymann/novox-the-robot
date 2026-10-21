/* Novox The Robot: C API.
 *
 * A plain C interface so the same engine can sit behind a DAW plugin (JUCE),
 * Python (ctypes), and JavaScript (WebAssembly).
 *
 * Strings are UTF-8. Returned strings and sample buffers are owned by the
 * caller and must be released with novox_free().
 */
#ifndef NOVOX_H
#define NOVOX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
/* Only the DLL exports the API (its build defines NOVOX_BUILDING_DLL). Static
 * linking, and calls through the DLL's import library, need no attribute. */
#if defined(NOVOX_BUILDING_DLL)
#define NOVOX_API __declspec(dllexport)
#else
#define NOVOX_API
#endif
#else
#define NOVOX_API __attribute__((visibility("default")))
#endif

typedef struct novox_engine novox_engine;

/* Creates an engine rendering at `sample_rate` Hz (e.g. 44100, 48000). */
NOVOX_API novox_engine* novox_create(double sample_rate);
NOVOX_API void novox_destroy(novox_engine* engine);

/* Changes the sample rate used by later renders and streams. */
NOVOX_API void novox_set_sample_rate(novox_engine* engine, double sample_rate);

/* Loads a CMU-format pronouncing dictionary. Returns entries loaded, or -1. */
NOVOX_API int novox_load_dictionary(novox_engine* engine, const char* path);

/* Adds a pronunciation, e.g. ("hugh", "HH Y UW1"). Returns 0 on success. */
NOVOX_API int novox_add_word(novox_engine* engine, const char* word, const char* arpabet);

/* Renders a score (see engine/src/score.hpp for the JSON format) to mono
 * float samples. Returns 0 on success; on failure see novox_last_error(). */
NOVOX_API int novox_render(novox_engine* engine, const char* score_json, float** out_samples, size_t* out_count);

/* The timeline the engine would sing for a score, as JSON (syllables,
 * phoneme segments, warnings). NULL on error. */
NOVOX_API char* novox_plan(novox_engine* engine, const char* score_json);

/* Converts a MIDI file or (uncompressed) MusicXML file to a score JSON string
 * that novox_render / novox_stream_create accept. `options_json` may be NULL or e.g.
 * {"track": 1, "lyrics": "hel-lo world", "voice": "robot"}. "units": "beats" keeps the
 * file's own beat positions (ticks / divisions) instead of converting through its tempo
 * map to seconds. Import warnings are included in the result under "importWarnings".
 * NULL on error. */
NOVOX_API char* novox_import(novox_engine* engine, const void* data, size_t size, const char* options_json);

/* Transcribes a sung recording into a score: melody from pitch tracking, and,
 * when `words_json` is given, lyrics lined up with the notes. `words_json` is
 * the output of any speech recognizer with word timestamps, e.g. Whisper:
 *   [{"word": "hello", "start": 0.52, "end": 0.91, "probability": 0.97}, ...]
 * A word may also carry its sub-word tokens, each with the time the recognizer
 * finished it (whisper's DTW token times), which singing-v2 uses as soft anchors:
 *   "tokens": [{"text": " re", "time": 1.23, "probability": 0.9}, ...]
 * `options_json` may be NULL or an object, e.g. {"algorithm": "singing-v2"}:
 *   algorithm       "legacy" (default), "singing-v1" (native singing sequence
 *                   decoder) or "singing-v2" (syllable-first: lyric syllables
 *                   decide note starts, the octave is decided per note; word ends
 *                   and token times are soft anchors, word starts are not used).
 *   detail          singing-v2's note detail: "natural" (default: one note per
 *                   sung syllable plus real melodic steps; scoops, approaches from
 *                   up to a whole tone below, grace notes and droops at a
 *                   syllable's end belong to their note) or "detailed" (those that
 *                   hold a pitch are notes of their own, as annotators who mark
 *                   every pitch they hear write them; vibrato and glides without
 *                   a plateau stay one note). Validated but ignored by legacy and
 *                   singing-v1.
 *   roundPitches    snap note pitches to semitones (default true).
 *   minNoteSeconds (0.08), splitSemitones (0.7), onsetDb (5), silenceDb (-45),
 *   stabilizePitch (false; experimental settled-pitch and dropout checks for
 *                   held natural vocals): legacy and singing-v1 tuning, validated
 *                   but ignored by singing-v2.
 *   debugFrames     (development, default false) adds the pitch-analysis frames as
 *                   "frames": [[time, midi, db, clarity, confidence], ...]
 *                   (midi 0 = unvoiced).
 * Returns a seconds-based score JSON whose notes carry "confidence" (0..1) and
 * "pitchSpread", with the algorithm (and for singing-v2 its "detail" and
 * "tuningCents"), the words and the indices of words no note took under
 * "transcription", and any warnings under "transcribeWarnings";
 * NULL on error. */
NOVOX_API char* novox_transcribe(novox_engine* engine, const float* samples, size_t count, double sample_rate,
                                 const char* words_json, const char* options_json);

/* Space-separated phonemes for a line of text, e.g. "HH AX L OW | W ER L D". */
NOVOX_API char* novox_phonemize(novox_engine* engine, const char* text);

// Expand lyric text to one token per note using the same rules as novox_plan.
// dictionary_json is an optional {"word":"ARPABET"} object, local to this call.
// Returns {"tokens":[...]}; empty tokens are word tails or lone "~" marks.
// Free the result with novox_free. On invalid dictionary JSON returns NULL and
// sets novox_last_error. This never changes the shared engine's dictionary.
NOVOX_API char* novox_lyric_tokens(novox_engine* engine, const char* text, const char* dictionary_json);

/* Comma-separated built-in voice preset names. */
NOVOX_API char* novox_voice_names(void);

/* --- Streaming (real-time) rendering -------------------------------------
 * A stream renders one score incrementally, for plugins and live playback.
 * Create/seek/destroy may allocate; novox_stream_process never does and is safe
 * to call from an audio thread. Output is at a fixed level (no normalization). */
typedef struct novox_stream novox_stream;

NOVOX_API novox_stream* novox_stream_create(novox_engine* engine, const char* score_json);
NOVOX_API void novox_stream_destroy(novox_stream* stream);
NOVOX_API size_t novox_stream_length(const novox_stream* stream);   /* total samples */
NOVOX_API size_t novox_stream_position(const novox_stream* stream);
/* Seconds of pre-roll before score time 0 (onset consonants start early). */
NOVOX_API double novox_stream_lead(const novox_stream* stream);
NOVOX_API void novox_stream_seek(novox_stream* stream, size_t sample);
/* Writes `frames` mono samples to `out`; returns how many had content
 * (0 once the song has finished). */
NOVOX_API size_t novox_stream_process(novox_stream* stream, float* out, size_t frames);

NOVOX_API const char* novox_last_error(novox_engine* engine);
NOVOX_API void novox_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* NOVOX_H */
