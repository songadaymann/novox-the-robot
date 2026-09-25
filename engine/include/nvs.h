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

/* Space-separated phonemes for a line of text, e.g. "HH AX L OW | W ER L D". */
NVS_API char* nvs_phonemize(nvs_engine* engine, const char* text);

/* Comma-separated built-in voice preset names. */
NVS_API char* nvs_voice_names(void);

NVS_API const char* nvs_last_error(nvs_engine* engine);
NVS_API void nvs_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* NVS_H */
