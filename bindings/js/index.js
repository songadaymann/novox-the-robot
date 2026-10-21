// Novox The Robot for JavaScript (browser, worker, Node). The engine is the
// same C++ core, compiled to WebAssembly.
//
//   import { createSinger } from "novox";
//   const singer = await createSinger({ sampleRate: 48000 });
//   const audio = singer.render({ tempo: 100, lyrics: "hel-lo world", notes: ["C4 1", "E4 1", "G4 2"] });
//   // audio: Float32Array of mono samples at singer.sampleRate

import createNovoxModule from "./novox_wasm.js";

export class NovoxError extends Error {}

export async function createSinger({ sampleRate = 48000, moduleOptions = {} } = {}) {
  const Module = await createNovoxModule(moduleOptions);
  return new Singer(Module, sampleRate);
}

export class Singer {
  constructor(Module, sampleRate) {
    this.M = Module;
    this.sampleRate = sampleRate;
    this.engine = Module._novox_create(sampleRate);
  }

  destroy() {
    if (this.engine) this.M._novox_destroy(this.engine);
    this.engine = 0;
  }

  // --- internals ------------------------------------------------------------
  _error() {
    return this.M.UTF8ToString(this.M._novox_last_error(this.engine));
  }
  _withString(text, fn) {
    const ptr = this.M.stringToNewUTF8(text);
    try {
      return fn(ptr);
    } finally {
      this.M._free(ptr);
    }
  }
  _takeString(ptr) {
    if (!ptr) throw new NovoxError(this._error());
    try {
      return this.M.UTF8ToString(ptr);
    } finally {
      this.M._novox_free(ptr);
    }
  }
  static _scoreText(score) {
    return typeof score === "string" ? score : JSON.stringify(score);
  }

  // --- API ------------------------------------------------------------------
  /** Sing a score (object or JSON string). Returns a Float32Array, peak-normalized. */
  render(score) {
    const M = this.M;
    const out = M._malloc(8);  // float* + size_t (wasm32: 4 bytes each)
    try {
      const rc = this._withString(Singer._scoreText(score), (s) => M._novox_render(this.engine, s, out, out + 4));
      if (rc !== 0) throw new NovoxError(this._error());
      const ptr = M.HEAPU32[out >> 2];
      const count = M.HEAPU32[(out + 4) >> 2];
      const audio = new Float32Array(M.HEAPF32.buffer, ptr, count).slice();
      M._novox_free(ptr);
      return audio;
    } finally {
      M._free(out);
    }
  }

  /** The syllables and phoneme segments the engine would sing. */
  plan(score) {
    return JSON.parse(this._takeString(this._withString(Singer._scoreText(score), (s) => this.M._novox_plan(this.engine, s))));
  }

  /** "HH AX L OW | W ER L D" */
  phonemize(text) {
    return this._takeString(this._withString(text, (s) => this.M._novox_phonemize(this.engine, s)));
  }

  voices() {
    return this._takeString(this.M._novox_voice_names()).split(",");
  }

  /** Add a pronunciation, e.g. addWord("hugh", "HH Y UW1"). */
  addWord(word, arpabet) {
    const rc = this._withString(word, (w) => this._withString(arpabet, (a) => this.M._novox_add_word(this.engine, w, a)));
    if (rc !== 0) throw new NovoxError(this._error());
  }

  /** MIDI or MusicXML bytes (Uint8Array / ArrayBuffer) -> score object. The file's own lyrics
   *  are sung when it has them (`lyrics` is for a file without any); `partVoice`, `verse` and
   *  `unfold: false` pick a MusicXML voice, one lyric verse and repeats as written. What the
   *  file holds (parts, tempo map, time signatures, pickup) is under `import`. */
  importFile(bytes, { lyrics = "", track = -1, voice = "", partVoice = "", verse = 0, unfold = true } = {}) {
    const M = this.M;
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const buf = M._malloc(data.length);
    try {
      M.HEAPU8.set(data, buf);
      const opts = JSON.stringify({ lyrics, track, verse, unfold, ...(voice ? { voice } : {}), ...(partVoice ? { partVoice: String(partVoice) } : {}) });
      const json = this._takeString(this._withString(opts, (o) => M._novox_import(this.engine, buf, data.length, o)));
      return JSON.parse(json);
    } finally {
      M._free(buf);
    }
  }

  /** Incremental renderer (for AudioWorklets and live playback). */
  stream(score) {
    const ptr = this._withString(Singer._scoreText(score), (s) => this.M._novox_stream_create(this.engine, s));
    if (!ptr) throw new NovoxError(this._error());
    return new Stream(this.M, ptr);
  }

  /** Encode mono float samples as a 16-bit WAV file. */
  toWav(samples, sampleRate = this.sampleRate) {
    const n = samples.length;
    const buf = new ArrayBuffer(44 + n * 2);
    const v = new DataView(buf);
    const str = (o, s) => [...s].forEach((c, i) => v.setUint8(o + i, c.charCodeAt(0)));
    str(0, "RIFF"); v.setUint32(4, 36 + n * 2, true); str(8, "WAVE");
    str(12, "fmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
    v.setUint32(24, sampleRate, true); v.setUint32(28, sampleRate * 2, true); v.setUint16(32, 2, true);
    v.setUint16(34, 16, true); str(36, "data"); v.setUint32(40, n * 2, true);
    for (let i = 0; i < n; i++) v.setInt16(44 + i * 2, Math.max(-1, Math.min(1, samples[i])) * 32767, true);
    return new Uint8Array(buf);
  }
}

export class Stream {
  constructor(Module, ptr) {
    this.M = Module;
    this.ptr = ptr;
    this.buf = 0;
    this.bufFrames = 0;
  }
  get length() { return this.M._novox_stream_length(this.ptr) >>> 0; }
  get position() { return this.M._novox_stream_position(this.ptr) >>> 0; }
  /** Seconds of pre-roll before score time 0. */
  get lead() { return this.M._novox_stream_lead(this.ptr); }
  seek(sample) { this.M._novox_stream_seek(this.ptr, sample); }
  /** Next `frames` samples as a Float32Array (a copy). */
  process(frames) {
    if (frames > this.bufFrames) {
      if (this.buf) this.M._free(this.buf);
      this.buf = this.M._malloc(frames * 4);
      this.bufFrames = frames;
    }
    this.M._novox_stream_process(this.ptr, this.buf, frames);
    return new Float32Array(this.M.HEAPF32.buffer, this.buf, frames).slice();
  }
  destroy() {
    if (this.buf) this.M._free(this.buf);
    if (this.ptr) this.M._novox_stream_destroy(this.ptr);
    this.ptr = this.buf = 0;
  }
}
