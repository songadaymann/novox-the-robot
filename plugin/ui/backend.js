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
//   setCapture(bool)  setTransport(bool)  setTempo(bpm)
//   importFile(base64, lyrics) -> string JSON (seconds) or ""
//   onStatus(cb)  (sync; cb gets {beats,bpm,playing,host,capture,rendering,error,songVersion})
//   isMock (bool)

const NATIVE = ["getSong", "setSong", "getPlan", "getVoices", "setCapture", "setTransport", "setTempo", "importFile"];

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
    getSong: async () => asString(await fn.getSong()),
    setSong: (json) => fn.setSong(json),
    getPlan: async () => asString(await fn.getPlan()),
    getVoices: async () => {
      const v = await fn.getVoices();
      if (Array.isArray(v)) return v.map(String);
      try { return JSON.parse(v); } catch { return []; }
    },
    setCapture: (on) => fn.setCapture(!!on),
    setTransport: (on) => fn.setTransport(!!on),
    setTempo: (bpm) => fn.setTempo(+bpm),
    importFile: async (b64, lyrics) => asString(await fn.importFile(b64, lyrics || "")),
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
  lyrics: "hel-lo, I'm a new vir-tual sing-er _ type me a song and I will sing it for you _",
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
  const m = /^([^aeiouy]*)([aeiouy]+)(.*)$/.exec(w);
  if (!m) return `${w.toUpperCase().split("").join(" ")} | AX | `;
  const vowel = (VOWEL_MAP.find(([k]) => m[2].startsWith(k)) || [0, "AH"])[1];
  const cons = (s) => s.replace(/'/g, "").toUpperCase().split("").join(" ");
  return `${cons(m[1])} | ${vowel} | ${cons(m[3].replace(/[aeiouy]/g, ""))}`.trim();
}

function mockTokens(lyrics) {
  const out = [];
  for (const word of String(lyrics || "").split(/\s+/).filter(Boolean)) {
    if (word === "_") { out.push({ hold: true }); continue; }
    const parts = word.split("-").filter(Boolean);
    parts.forEach((p, i) => out.push({ text: p.replace(/[.,!?;:"]/g, ""), continues: i < parts.length - 1 }));
  }
  return out;
}

function mockPlan(song, bpm) {
  const spb = 60 / bpm;
  const notes = [...(song.notes || [])].sort((a, b) => a.start - b.start);
  const toks = mockTokens(song.lyrics);
  const syllables = [], warnings = [];
  let ti = 0;
  for (const n of notes) {
    const start = n.start * spb, end = (n.start + n.dur) * spb;
    if (n.lyric) {
      syllables.push({ start, end, text: n.lyric, phonemes: fakePhonemes(n.lyric), continues: false });
      continue;
    }
    const t = toks[ti++];
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

function createMock() {
  const params = new URLSearchParams(location.search);
  const host = params.get("host") === "1";
  let songJson = JSON.stringify(DEMO_SONG);
  let bpm = 110, playing = false, capture = false, rendering = false, version = 1, beats = 0;
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
      if (!capture && beats > songEnd() + 4) { playing = false; beats = 0; }
    }
    const status = { beats, bpm, playing, host, capture, rendering, error: "", songVersion: version };
    for (const cb of listeners) cb(status);
  }, 33);

  return {
    isMock: true,
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
    getVoices: () => delay([...MOCK_VOICES]),
    setCapture: (on) => { record("setCapture", on); capture = !!on; lastCapturedBeat = Math.floor(beats); return delay(undefined); },
    setTransport: (on) => { record("setTransport", on); playing = !!on; beats = 0; lastCapturedBeat = -1; return delay(undefined); },
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
    onStatus: (cb) => listeners.push(cb),
  };
}
