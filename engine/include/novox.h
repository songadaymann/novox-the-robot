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
#define NOVOX_API __declspec(dllexport)
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
 * {"track": 1, "lyrics": "hel-lo world", "voice": "robot"}. Import warnings are
 * included in the result under "importWarnings". NULL on error. */
NOVOX_API char* novox_import(novox_engine* engine, const void* data, size_t size, const char* options_json);

/* Space-separated phonemes for a line of text, e.g. "HH AX L OW | W ER L D". */
NOVOX_API char* novox_phonemize(novox_engine* engine, const char* text);

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
