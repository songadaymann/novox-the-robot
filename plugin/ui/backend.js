// Backend bridge for the Novox The Robot editor.
//
// Inside the plugin (window.__JUCE__ exists) every call goes to a native
// function registered with WebBrowserComponent::Options::withNativeFunction.
// In a plain browser a small in-memory mock with the same interface is used,
// so the UI can be developed with `python3 -m http.server`.
//
// Interface (all async unless noted):
//   getSong() -> string JSON (beats)       setSong(json)
//   getPlan() -> string JSON ("" on error) getVoices() -> string[]
//   planSong(json,bpm) -> string JSON plan for an edit, without changing the processor song
//   setCapture(bool)  setTransport(bool)  setTempo(bpm)  setPlayhead(beats) (no host: where Play starts)
//   importFile(base64, lyrics) -> string JSON (seconds) or ""
//   startAudioImport(base64 or "" for the pending recording, name, download, melodyOnly, detailed)
//   storeSource(recordingId, source) -> bool   getSource(recordingId) -> source or null
//   writeClipboard(text), readClipboard() -> text (system clipboard; throw on failure)
//   lyricTokens(text, dictionaryJSON) -> string JSON {tokens:[strings]} (throw on failure)
//   playSourceReview([{id, from, to, at}], loop) -> "" or error: stored recordings (seconds
//     [from, to) of each, `at` seconds into the phrase; overlapping pieces are mixed)
//   onStatus(cb)  (sync; cb gets {beats,bpm,playing,host,capture,rendering,error,songVersion,
//     platform: "mac" | "windows" | "linux", importTypes: {audio: [extensions], score: [extensions]}})
//   hostSpace() -> bool: Space handed to the DAW's window as a key press
//   isMock (bool)   starterSong (mock only: the song it starts with, see song-merge.js)

const NATIVE = ["getSong", "setSong", "getPlan", "getVoices", "setCapture", "setTransport", "setTempo", "importFile", "audioImportStatus", "startAudioImport", "cancelAudioImport", "previewAudioImport", "playAudioReview", "stopAudioReview", "setEditorSize", "previewNote", "startMicrophone", "stopMicrophone", "cancelMicrophone", "readNativeDrop", "playSongReview", "playSourceReview", "storeSource", "getSource", "fileAction", "hostSpace", "setPlayhead", "hostTransport", "hostLocate", "setMmcState", "writeClipboard", "readClipboard", "lyricTokens", "planSong"];

const MAX_CLIPBOARD_BYTES = 4 * 1024 * 1024;
function clipboardText(text) {
  if (typeof text !== "string") throw new Error("Clipboard text is missing");
  if (new TextEncoder().encode(text).length > MAX_CLIPBOARD_BYTES)
    throw new Error("The clipboard is too large (maximum 4 MB)");
  return text;
}
function clipboardReply(reply) {
  if (!reply || typeof reply !== "object") throw new Error("The system clipboard did not respond. Please try again.");
  if (reply.error) throw new Error(String(reply.error));
  return clipboardText(reply.text);
}
function lyricArguments(text, dictionaryJSON) {
  if (typeof text !== "string" || typeof dictionaryJSON !== "string")
    throw new Error("Lyrics and pronunciation dictionary are required");
  if (new TextEncoder().encode(text).length > 64 * 1024 || new TextEncoder().encode(dictionaryJSON).length > 1024 * 1024)
    throw new Error("The lyric edit is too large");
}

export async function createBackend() {
  return window.__JUCE__ ? createNative() : createMock();
}

// ---------------------------------------------------------------------------
// Native (JUCE WebBrowserComponent)

// Same wire protocol as JUCE's own getNativeFunction; used if juce.js can't be
// loaded (e.g. its check_native_interop.js import isn't embedded).
function fallbackNativeFactory() {
  const pending = new Map();
  let nextId = 0;
  window.__JUCE__.backend.addEventListener("__juce__complete", ({ promiseId, result }) => {
    const p = pending.get(promiseId);
    if (p) { pending.delete(promiseId); p(result); }
  });
  return (name) => (...params) => new Promise((resolve) => {
    const resultId = nextId++;
    pending.set(resultId, resolve);
    window.__JUCE__.backend.emitEvent("__juce__invoke", { name, params, resultId });
  });
}

async function createNative() {
  let factory;
  try {
    const juce = await import("./juce.js");
    factory = juce.getNativeFunction;
    if (typeof factory !== "function") throw new Error("juce.js has no getNativeFunction");
  } catch (e) {
    console.warn("juce.js unavailable, using built-in native bridge:", e && e.message);
    factory = fallbackNativeFactory();
  }
  const fn = {};
  for (const n of NATIVE) fn[n] = factory(n);
  const asString = (v) => (v == null ? "" : typeof v === "string" ? v : JSON.stringify(v));
  return {
    isMock: false,
    writeClipboard: async text => { clipboardReply(await fn.writeClipboard(clipboardText(text))); },
    readClipboard: async () => clipboardReply(await fn.readClipboard()),
    lyricTokens: async (text, dictionaryJSON = "{}") => {
      lyricArguments(text, dictionaryJSON);
      const result = asString(await fn.lyricTokens(text, dictionaryJSON));
      const parsed = JSON.parse(result);
      if (parsed.error) throw new Error(String(parsed.error));
      if (!Array.isArray(parsed.tokens)) throw new Error("Could not divide the lyrics into syllables");
      return result;
    },
    storeSource:(id,source)=>fn.storeSource(id,source),
    getSource:id=>fn.getSource(id),
    fileAction:action=>fn.fileAction(action),
    hostSpace: () => fn.hostSpace(),
    startMicrophone: ()=>fn.startMicrophone(),
    stopMicrophone: ()=>fn.stopMicrophone(),
    cancelMicrophone: ()=>fn.cancelMicrophone(),
    readNativeDrop: ()=>fn.readNativeDrop(),
    onScoreFileDrop: cb=>window.__JUCE__.backend.addEventListener("scoreFileDrop",cb),
    previewNote: pitch => fn.previewNote(pitch),
    setEditorSize: (width,height) => fn.setEditorSize(width,height),
    getSong: async () => asString(await fn.getSong()),
    setSong: (json) => fn.setSong(json),
    getPlan: async () => asString(await fn.getPlan()),
    planSong: async (json, bpm) => {
      if (typeof json !== "string" || new TextEncoder().encode(json).length > 4 * 1024 * 1024)
        throw new Error("The song is too large to preview");
      const result = asString(await fn.planSong(json, +bpm));
      const parsed = JSON.parse(result);
      if (parsed.error) throw new Error(String(parsed.error));
      if (!Array.isArray(parsed.syllables)) throw new Error("Could not preview the lyric edit");
      return result;
    },
    getVoices: async () => {
      const v = await fn.getVoices();
      if (Array.isArray(v)) return v.map(String);
      try { return JSON.parse(v); } catch { return []; }
    },
    setCapture: (on) => fn.setCapture(!!on),
    setTransport: (on) => fn.setTransport(!!on),
    setPlayhead: (beats) => fn.setPlayhead(+beats),
    hostTransport: (action) => fn.hostTransport(String(action)),
    hostLocate: async (seconds, fps) => { const r = await fn.hostLocate(+seconds, +fps || 0); return r && typeof r === "object" ? r : { sent: !!r }; },
    setMmcState: (works, offset, rate) => fn.setMmcState(!!works, +offset || 0, +rate || 0),
    setTempo: (bpm) => fn.setTempo(+bpm),
    importFile: async (b64, lyrics) => asString(await fn.importFile(b64, lyrics || "")),
    playAudioReview: (...args) => fn.playAudioReview(...args),
    playSongReview: (...args) => fn.playSongReview(...args),
    playSourceReview: (pieces, loop) => fn.playSourceReview(pieces, !!loop),
    stopAudioReview: () => fn.stopAudioReview(),
    audioImportStatus: () => fn.audioImportStatus(),
    startAudioImport: (b64, name, download, melodyOnly, detailed) =>
      fn.startAudioImport(b64, name, !!download, !!melodyOnly, !!detailed),
    cancelAudioImport: () => fn.cancelAudioImport(),
    previewAudioImport: (score) => fn.previewAudioImport(score),
    onAudioFileDrop: (cb) => window.__JUCE__.backend.addEventListener("audioFileDrop", cb),
    onStatus(cb) {
      window.__JUCE__.backend.addEventListener("status", (s) => {
        if (typeof s === "string") { try { s = JSON.parse(s); } catch { return; } }
        cb(s || {});
      });
    },
  };
}

// ---------------------------------------------------------------------------
// Mock (plain browser). Open index.html?host=1 to pretend to be inside a DAW.

const MOCK_VOICES = ["std", "robot", "male", "female", "kid", "soprano", "alto", "tenor", "bass", "gregorian",
  "choir", "female-choir", "karaoke", "quick-vibrato", "hifi", "crooner"];

const DEMO_SONG = {
  lyrics: "hel-lo, I'm a new vir-tual sing-er _\ntype me a song and I will sing it for you _",
  voice: "std",
  notes: [
    [64, 0, 0.5], [67, 0.5, 1], [67, 1.5, 0.5], [69, 2, 0.5], [72, 2.5, 1], [71, 3.5, 0.5], [69, 4, 0.5],
    [67, 4.5, 0.75], [64, 5.25, 0.25], [62, 5.5, 1.5],
    [64, 8, 0.5], [67, 8.5, 0.5], [69, 9, 0.5], [72, 9.5, 1], [69, 10.5, 0.5], [67, 11, 0.5], [69, 11.5, 0.5],
    [72, 12, 0.75], [74, 12.75, 0.25], [76, 13, 0.5], [74, 13.5, 1], [72, 14.5, 2],
  ].map(([pitch, start, dur]) => ({ pitch, start, dur })),
};

const VOWEL_MAP = [["ee", "IY"], ["ea", "IY"], ["oo", "UW"], ["ou", "AW"], ["ow", "OW"], ["ai", "EY"], ["ay", "EY"], ["oi", "OY"],
  ["a", "AE"], ["e", "EH"], ["i", "IH"], ["o", "AA"], ["u", "AH"], ["y", "IY"]];
function fakePhonemes(text) {
  const w = text.toLowerCase().replace(/[^a-z']/g, "");
  if (!w) return "M |  | ";
  if (/^m+$/.test(w)) return " | M | "; // "mm" hums
  const m = /^([^aeiouy]*)([aeiouy]+)(.*)$/.exec(w);
  if (!m) return `${w.toUpperCase().split("").join(" ")} | AX | `;
  const vowel = (VOWEL_MAP.find(([k]) => m[2].startsWith(k)) || [0, "AH"])[1];
  const cons = (s) => s.replace(/'/g, "").toUpperCase().split("").join(" ");
  return `${cons(m[1])} | ${vowel} | ${cons(m[3].replace(/[aeiouy]/g, ""))}`.trim();
}

// The engine's lyric flow without its dictionary (no splitting by vowels): "hel-lo" gives a note
// per piece, "_" holds, "wanna~" stays on one note and a lone "~" leaves a note without a lyric.
const piece = (p) => p === "_" || p === "+" ? { hold: true }
  : { text: p.replace(/[-~]/g, "").replace(/[.,!?;:"]/g, ""), continues: p.endsWith("-") };
function mockTokens(lyrics) {
  const out = [];
  for (const word of String(lyrics || "").split(/\s+/).filter(Boolean)) {
    if (word.includes("~")) out.push(piece(word));
    else for (const p of word.split(/(?<=-)/)) out.push(piece(p));
  }
  return out;
}

function mockPlan(song, bpm) {
  const spb = 60 / bpm;
  const notes = [...(song.notes || [])].sort((a, b) => a.start - b.start);
  const toks = mockTokens(song.useNoteLyrics ? "" : song.lyrics);
  const syllables = [], warnings = [];
  let ti = 0;
  for (const n of notes) {
    const start = n.start * spb, end = (n.start + n.dur) * spb;
    const t = n.lyric ? piece(n.lyric) : toks[ti++];
    if (t && t.hold && syllables.length) { syllables[syllables.length - 1].end = end; continue; }
    if (t && !t.hold) syllables.push({ start, end, text: t.text, phonemes: fakePhonemes(t.text), continues: t.continues });
    else syllables.push({ start, end, text: "", phonemes: "M |  | ", continues: false });
  }
  const left = toks.slice(ti).filter((t) => !t.hold).length;
  if (left) warnings.push(`${left} syllable${left > 1 ? "s" : ""} of lyrics had no note to sing on`);
  const length = notes.length ? Math.max(...notes.map((n) => (n.start + n.dur) * spb)) : 0;
  return { lead: 0.05, length, syllables, segments: [], warnings };
}

// Tiny MusicXML reader so "Import" can be exercised without the engine.
function mockParseMusicXml(text) {
  const doc = new DOMParser().parseFromString(text, "application/xml");
  if (doc.querySelector("parsererror") || !doc.querySelector("note")) return null;
  const tempo = +(doc.querySelector("sound[tempo]")?.getAttribute("tempo")) || 100;
  let divisions = 1, t = 0;
  const notes = [];
  const STEP = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
  for (const el of doc.querySelectorAll("attributes > divisions, note")) {
    if (el.tagName === "divisions") { divisions = +el.textContent || 1; continue; }
    const beats = (+el.querySelector("duration")?.textContent || 0) / divisions;
    if (el.querySelector("chord")) continue;
    if (!el.querySelector("rest") && el.querySelector("pitch")) {
      const step = el.querySelector("step").textContent.trim();
      const alter = +(el.querySelector("alter")?.textContent || 0);
      const oct = +el.querySelector("octave").textContent;
      const lyr = el.querySelector("lyric");
      let lyric;
      if (lyr) {
        const syl = lyr.querySelector("syllabic")?.textContent || "single";
        lyric = (lyr.querySelector("text")?.textContent || "").trim() + (syl === "begin" || syl === "middle" ? "-" : "");
      }
      const n = { pitch: STEP[step] + alter + 12 * (oct + 1), start: t * 60 / tempo, dur: beats * 60 / tempo };
      if (lyric) n.lyric = lyric;
      notes.push(n);
    }
    t += beats;
  }
  return { units: "seconds", tempo, notes };
}

// A recording "converted" by the mock: a short sung phrase whose words and pitches depend on the
// file name. "@bar5" in the name stamps it like a region Logic drags onto the plugin (at bar 5 of
// the current tempo); without it the recording has no timeline position. Different "take X" names
// give different words and pitches, so takes of one region can be told apart.
const MOCK_TAKES = [["hel-", "lo", "my", "dar-", "ling", "_"], ["good-", "bye", "my", "sweet", "friend", "_"],
  ["here", "comes", "the", "sun-", "shine", "_"], ["sing", "it", "a-", "gain", "to-", "night"]];
function mockRecording(name, bpm, detailed) {
  let hash = 0;
  for (const c of String(name).replace(/@bar[\d.]+/i, "")) hash = (hash * 31 + c.charCodeAt(0)) >>> 0;
  const words = MOCK_TAKES[hash % MOCK_TAKES.length], base = 57 + (hash % 7);
  const starts = [0.5, 1.0, 1.5, 2.25, 3.0, 4.0], durs = [0.5, 0.5, 0.75, 0.75, 1.0, 1.5], steps = [0, 2, 4, 5, 7, 4];
  const notes = words.map((lyric, i) => ({ pitch: base + steps[i], start: starts[i], dur: durs[i], confidence: 0.8, pitchSpread: 0.3, detectedPitch: base + steps[i] + 0.2, lyric }));
  const duration = 6;
  const bar = /@bar([\d.]+)/i.exec(name);
  const silence = new Uint8Array(44 + 1600); // 50 ms of 16 kHz mono silence
  const view = new DataView(silence.buffer);
  [..."RIFF"].forEach((c, i) => view.setUint8(i, c.charCodeAt(0))); view.setUint32(4, silence.length - 8, true);
  [..."WAVEfmt "].forEach((c, i) => view.setUint8(8 + i, c.charCodeAt(0)));
  view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, 16000, true); view.setUint32(28, 32000, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  [..."data"].forEach((c, i) => view.setUint8(36 + i, c.charCodeAt(0))); view.setUint32(40, 1600, true);
  return {
    name, duration, detail: detailed ? "detailed" : "natural",
    ...(bar ? { timeReference: 3600 + (+bar[1] - 1) * 4 * 60 / bpm, originator: "Logic Pro 12.4" } : {}),
    score: {
      units: "seconds", tempo: bpm, transcribeWarnings: [], notes,
      transcription: { algorithm: "singing-v2", detail: detailed ? "detailed" : "natural",
        transcriptWords: words.filter((w) => w !== "_").map((w, i) => ({ word: w.replace(/-$/, ""), start: starts[i], end: starts[i] + durs[i] })) },
    },
    words: [],
    waveform: Array.from({ length: duration * 62 }, (_, i) => (notes.some((n) => i / 62 >= n.start && i / 62 < n.start + n.dur) ? 0.3 + 0.4 * Math.abs(Math.sin(i / 3)) : 0.01)),
    original: "data:audio/wav;base64," + btoa(String.fromCharCode(...silence)),
  };
}

function createMock() {
  const params = new URLSearchParams(location.search);
  const host = params.get("host") === "1";
  // ?host=1&mmc=1: a DAW that follows MMC; mmcBias: seconds its bar 1 is off from where Novox assumes;
  // mmcRate: the frame rate it reads a locate's frames at; mmcLatency: ms before it acts. ?host=1
  // alone: a DAW that ignores MMC. It calls the plugin on every tick (status.blocks), or with
  // ?frozen=1 only while playing (Logic, for a track it is not playing).
  // ?platform=windows (or linux): the plugin on that platform: Ctrl key names, no m4a import and no
  // MMC port (as ?port=0 on a Mac). ?space=1: a DAW that plays and stops on the Space the editor
  // hands it (hostSpace); without it the hand-over fails, as where the editor has no DAW window.
  const platform = ["windows", "linux"].includes(params.get("platform")) ? params.get("platform") : "mac";
  const mmcPort = platform === "mac" && params.get("port") !== "0", spaceWorks = params.get("space") === "1";
  const importTypes = { audio: ["wav", "mp3", "aif", "aiff", ...(platform === "mac" ? ["m4a"] : []), "flac", "ogg"],
    score: ["mid", "midi", "xml", "musicxml", "mxl"] };
  const mmcListens = params.get("mmc") === "1", mmcBias = +params.get("mmcBias") || 0;
  const mmcRate = +params.get("mmcRate") || 0, mmcLatency = +params.get("mmcLatency") || 0, frozen = params.get("frozen") === "1";
  const staleAudio = params.get("stale") === "1";
  let blocks = 0, mmcState = { works: false, offset: 0, rate: 0 };
  const later = (fn) => (mmcLatency ? setTimeout(fn, mmcLatency) : fn());
  // Audio imports convert straight away; open index.html?model=0 to see the model download dialog.
  const modelAvailable = params.get("model") !== "0";
  const sources = new Map();
  let importState = { state: "idle" }, importTimer = 0;
  let songJson = JSON.stringify(DEMO_SONG);
  let bpm = 110, playing = false, capture = false, rendering = false, version = 1, beats = 0, cursor = 0;
  let lastTick = performance.now(), renderTimer = 0, lastCapturedBeat = -1;
  const listeners = [];
  const log = (window.__mockLog = []);
  const record = (name, arg) => { log.push({ name, arg }); console.log("[mock]", name, arg === undefined ? "" : arg); };
  const delay = (v, ms = 15) => new Promise((r) => setTimeout(() => r(v), ms));
  const songEnd = () => {
    const s = JSON.parse(songJson);
    return Math.max(0, ...(s.notes || []).map((n) => n.start + n.dur));
  };

  setInterval(() => {
    const now = performance.now(), dt = (now - lastTick) / 1000;
    lastTick = now;
    if (playing) {
      beats += (dt * bpm) / 60;
      // Pretend a MIDI keyboard is being played while capture is armed.
      if (capture && Math.floor(beats) !== lastCapturedBeat) {
        lastCapturedBeat = Math.floor(beats);
        const s = JSON.parse(songJson);
        const scale = [60, 62, 64, 65, 67, 69, 71, 72];
        s.notes = (s.notes || []).filter((n) => !(n.start < lastCapturedBeat + 1 && n.start + n.dur > lastCapturedBeat));
        s.notes.push({ pitch: scale[lastCapturedBeat % scale.length], start: lastCapturedBeat, dur: 0.75 });
        s.notes.sort((a, b) => a.start - b.start);
        songJson = JSON.stringify(s);
        version++;
      }
      if (!host && !capture && beats > songEnd() + 4) { playing = false; beats = cursor; }
    }
    if (host && (playing || !frozen)) blocks++;
    // ?stale=1: the audio stays at the first render (a render that never finishes).
    const status = { beats, bpm, playing, host, capture, rendering, error: "", songVersion: version, version: "1.0 beta",
      renderedVersion: staleAudio ? 1 : version, renders: { started: version, cancelled: staleAudio ? version - 1 : 0, secondsSinceDone: 0.5 },
      platform, importTypes,
      ...(host ? { daw: true, seconds: beats * 60 / bpm, blocks, hostName: "Mock DAW", mmc: { port: mmcPort, ...mmcState } } : {}) };
    for (const cb of listeners) cb(status);
  }, 33);

  return {
    isMock: true,
    writeClipboard: async text => {
      clipboardText(text);
      if (!navigator.clipboard?.writeText) throw new Error("System clipboard access is unavailable in this browser. Use localhost or the Novox app.");
      try { await navigator.clipboard.writeText(text); }
      catch { throw new Error("The browser could not write to the system clipboard. Allow clipboard access and try again."); }
    },
    readClipboard: async () => {
      if (!navigator.clipboard?.readText) throw new Error("System clipboard access is unavailable in this browser. Use localhost or the Novox app.");
      let text;
      try { text = await navigator.clipboard.readText(); }
      catch { throw new Error("The browser could not read the system clipboard. Allow clipboard access and try again."); }
      return clipboardText(text);
    },
    lyricTokens: async (text, dictionaryJSON = "{}") => {
      lyricArguments(text, dictionaryJSON);
      // Browser preview has no pronunciation engine. Explicit syllables match the native
      // notation; unhyphenated words remain a single token here. Test natural words natively.
      const tokens = [];
      for (const word of text.match(/\{[^}]*\}|\S+/g) || []) {
        if (word.startsWith("{")) tokens.push(word);
        else if (word.includes("~")) tokens.push(word.replace(/[-~]/g, ""));
        else for (const p of word.split(/(?<=-)/)) if (p) tokens.push(p);
      }
      return JSON.stringify({ tokens });
    },
    getSong: () => delay(songJson),
    setSong: (json) => {
      const s = JSON.parse(json);
      record("setSong", { voice: s.voice, notes: s.notes.length, lyrics: s.lyrics });
      songJson = json;
      version++;
      rendering = true;
      clearTimeout(renderTimer);
      renderTimer = setTimeout(() => (rendering = false), 450);
      return delay(undefined);
    },
    getPlan: () => {
      try { return delay(JSON.stringify(mockPlan(JSON.parse(songJson), bpm)), 25); } catch { return delay(""); }
    },
    planSong: async (json, previewBpm) => {
      if (typeof json !== "string" || new TextEncoder().encode(json).length > 4 * 1024 * 1024)
        throw new Error("The song is too large to preview");
      return JSON.stringify(mockPlan(JSON.parse(json), +previewBpm));
    },
    getVoices: () => delay([...MOCK_VOICES]),
    setCapture: (on) => { record("setCapture", on); capture = !!on; lastCapturedBeat = Math.floor(beats); return delay(undefined); },
    // Like the standalone app: Play starts from the top, Stop leaves the playhead where it stopped.
    setTransport: (on) => { record("setTransport", on); if (!!on !== playing) beats = cursor; playing = !!on; lastCapturedBeat = -1; return delay(undefined); },
    setPlayhead: (b) => { record("setPlayhead", b); if (!host) cursor = beats = Math.max(0, +b || 0); return delay(undefined); },
    setTempo: (v) => { record("setTempo", v); bpm = Math.min(300, Math.max(30, +v || 120)); return delay(undefined); },
    importFile: (b64, lyrics) => {
      record("importFile", { bytes: Math.floor((b64.length * 3) / 4), lyrics });
      let text = "";
      try { text = new TextDecoder().decode(Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))); } catch {}
      const parsed = /<score-partwise|<score-timewise/.test(text) ? mockParseMusicXml(text) : null;
      const score = parsed || {
        units: "seconds", tempo: 96,
        notes: [["C4", 0, 0.625, "twin-"], ["C4", 0.625, 0.625, "kle"], ["G4", 1.25, 0.625, "twin-"], ["G4", 1.875, 0.625, "kle"],
          ["A4", 2.5, 0.625, "lit-"], ["A4", 3.125, 0.625, "tle"], ["G4", 3.75, 1.25, "star"]]
          .map(([pitch, start, dur, lyric]) => ({ pitch, start, dur, lyric })),
      };
      return delay(JSON.stringify(score), 60);
    },
    starterSong: DEMO_SONG,
    audioImportStatus: () => delay({ ...importState, modelAvailable }),
    startAudioImport: (b64, name, download, melodyOnly, detailed) => {
      record("startAudioImport", { name, download: !!download, melodyOnly: !!melodyOnly, detailed: !!detailed });
      clearTimeout(importTimer);
      importState = { state: "working", message: "Converting recording…" };
      importTimer = setTimeout(() => { importState = { state: "ready", result: mockRecording(name, bpm, detailed) }; }, 120);
      return delay({});
    },
    cancelAudioImport: () => { clearTimeout(importTimer); importState = { state: "cancelled", message: "Cancelled" }; return delay(undefined); },
    storeSource: (id, source) => { record("storeSource", { id, startBeat: source.startBeat, seconds: source.seconds }); sources.set(id, structuredClone(source)); return delay(true); },
    getSource: (id) => delay(sources.has(id) ? structuredClone(sources.get(id)) : null),
    playSourceReview: (pieces, loop) => { record("playSourceReview", { pieces, loop: !!loop }); return delay(""); },
    playSongReview: (begin, end, loop) => { record("playSongReview", { begin, end, loop: !!loop }); return delay(""); },
    stopAudioReview: () => { record("stopAudioReview"); return delay(undefined); },
    previewAudioImport: () => delay({error: "Audio conversion requires the native app/plugin"}),
    onAudioFileDrop: () => {},
    // Recorders so the file buttons and window sizes can be checked in a browser.
    fileAction: (action) => { record("fileAction", action); return delay(""); },
    hostSpace: () => { record("hostSpace"); if (host && spaceWorks) later(() => { playing = !playing; }); return delay(host && spaceWorks); },
    hostTransport: (action) => { record("hostTransport", action); if (host && mmcPort && mmcListens) later(() => { playing = action === "play"; }); return delay(host && mmcPort); },
    hostLocate: (seconds, fps) => {
      record("hostLocate", seconds);
      // The label Novox sends (bar 1 at 01:00:00:00, as in Logic), read back at the DAW's own rate.
      const rate = +fps || 30, base = Math.round(rate), count = Math.round((3600 + seconds) * rate);
      const frame = count % base, landed = Math.floor(count / base) + frame / (mmcRate || rate) - 3600 + mmcBias;
      if (host && mmcPort && mmcListens) later(() => { beats = Math.max(0, landed) * bpm / 60; });
      return delay({ sent: host && mmcPort, frame, fps: rate });
    },
    setMmcState: (works, offset, rate) => { record("setMmcState", { works, offset, rate }); mmcState = { works: !!works, offset: +offset || 0, rate: +rate || 0 }; return delay(undefined); },
    setEditorSize: (width, height) => { record("setEditorSize", [width, height]); return delay(undefined); },
    onStatus: (cb) => listeners.push(cb),
  };
}
