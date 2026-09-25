/* New Virtual Singer: C API.
 *
 * A plain C interface so the same engine can sit behind a DAW plugin (JUCE),
 * Python (ctypes), and JavaScript (WebAssembly).
 *
 * Strings are UTF-8. Returned strings and sample buffers are owned by the
 * caller and must be released with nvs_free().
 */
#ifndef NVS_H
#define NVS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define NVS_API __declspec(dllexport)
#else
#define NVS_API __attribute__((visibility("default")))
#endif

typedef struct nvs_engine nvs_engine;

/* Creates an engine rendering at `sample_rate` Hz (e.g. 44100, 48000). */
NVS_API nvs_engine* nvs_create(double sample_rate);
NVS_API void nvs_destroy(nvs_engine* engine);

/* Loads a CMU-format pronouncing dictionary. Returns entries loaded, or -1. */
NVS_API int nvs_load_dictionary(nvs_engine* engine, const char* path);

/* Adds a pronunciation, e.g. ("hugh", "HH Y UW1"). Returns 0 on success. */
NVS_API int nvs_add_word(nvs_engine* engine, const char* word, const char* arpabet);

/* Renders a score (see engine/src/score.hpp for the JSON format) to mono
 * float samples. Returns 0 on success; on failure see nvs_last_error(). */
NVS_API int nvs_render(nvs_engine* engine, const char* score_json, float** out_samples, size_t* out_count);

/* The timeline the engine would sing for a score, as JSON (syllables,
 * phoneme segments, warnings). NULL on error. */
NVS_API char* nvs_plan(nvs_engine* engine, const char* score_json);

/* Converts a MIDI file or (uncompressed) MusicXML file to a score JSON string
 * that nvs_render / nvs_stream_create accept. `options_json` may be NULL or e.g.
 * {"track": 1, "lyrics": "hel-lo world", "voice": "robot"}. Import warnings are
 * included in the result under "importWarnings". NULL on error. */
NVS_API char* nvs_import(nvs_engine* engine, const void* data, size_t size, const char* options_json);

/* Space-separated phonemes for a line of text, e.g. "HH AX L OW | W ER L D". */
NVS_API char* nvs_phonemize(nvs_engine* engine, const char* text);

/* Comma-separated built-in voice preset names. */
NVS_API char* nvs_voice_names(void);

/* --- Streaming (real-time) rendering -------------------------------------
 * A stream renders one score incrementally, for plugins and live playback.
 * Create/seek/destroy may allocate; nvs_stream_process never does and is safe
 * to call from an audio thread. Output is at a fixed level (no normalization). */
typedef struct nvs_stream nvs_stream;

NVS_API nvs_stream* nvs_stream_create(nvs_engine* engine, const char* score_json);
NVS_API void nvs_stream_destroy(nvs_stream* stream);
NVS_API size_t nvs_stream_length(const nvs_stream* stream);   /* total samples */
NVS_API size_t nvs_stream_position(const nvs_stream* stream);
/* Seconds of pre-roll before score time 0 (onset consonants start early). */
NVS_API double nvs_stream_lead(const nvs_stream* stream);
NVS_API void nvs_stream_seek(nvs_stream* stream, size_t sample);
/* Writes `frames` mono samples to `out`; returns how many had content
 * (0 once the song has finished). */
NVS_API size_t nvs_stream_process(nvs_stream* stream, float* out, size_t frames);

NVS_API const char* nvs_last_error(nvs_engine* engine);
NVS_API void nvs_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* NVS_H */
