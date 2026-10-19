import { correctedPitch } from "./pitch-correction.js";
import { detectKey, evidenceFor, quantizeNotes } from "./note-shaping.js";
import { createAudioImport, importPosition, importSummary } from "./audio-import.js";
import { labelNotes, lyricsFromNotes, pinnedLyric } from "./lyric-text.js";
import { applyNoteLyrics } from "./note-lyrics.js";
import { createMelodyClipboard, parseMelodyClipboard, melodyPasteConflicts, applyMelodyPaste } from "./melody-clipboard.js";
import { createLiveRangesCache, importSpan, isStarterSong, mergeImport, originalSegments, songTranscriptions } from "./song-merge.js";
// Novox The Robot plugin editor. Plain ES module, no dependencies.
//
// Sections:
//   1. constants + helpers        5. roll drawing (canvas)
//   2. voices + cartoon faces     6. roll input (mouse, keyboard, wheel)
//   3. model, undo, sync          7. toolbar, lyrics, voice panel, import
//   4. plan -> note labels        8. status + boot
import { createBackend } from "./backend.js";

const $ = (id) => document.getElementById(id);
const visibleSourceRanges = createLiveRangesCache();
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const r6 = (x) => Math.round(x * 1e6) / 1e6;
const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

// ===========================================================================
// 1. constants + helpers
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const BLACK = new Set([1, 3, 6, 8, 10]);
const isBlack = (m) => BLACK.has(((m % 12) + 12) % 12);
const pitchName = (m) => NAMES[((Math.round(m) % 12) + 12) % 12] + (Math.floor(Math.round(m) / 12) - 1);
const STEPS = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
function parsePitch(p) {
  if (typeof p === "number") return Math.round(p);
  const m = /^([A-Ga-g])([#b]*)(-?\d+)$/.exec(String(p).trim());
  if (!m) return NaN;
  let n = STEPS[m[1].toLowerCase()] + 12 * (+m[3] + 1);
  for (const c of m[2]) n += c === "#" ? 1 : -1;
  return n;
}
function fmtPos(b) {
  b = Math.max(0, b);
  const bar = Math.floor(b / 4) + 1, beat = Math.floor(b % 4) + 1, six = Math.floor((b % 1) * 4 + 1e-6) + 1;
  return `${bar}.${beat}.${six}`;
}
function fmtLen(d) {
  const k = Math.round(d * 1000) / 1000;
  const names = { 0.125: "1/32", 0.25: "1/16", 0.375: "dotted 1/16", 0.5: "1/8", 0.75: "dotted 1/8", 1: "1 beat", 1.5: "1½ beats", 2: "2 beats", 3: "3 beats", 4: "1 bar", 8: "2 bars" };
  return names[k] || `${k} beats`;
}
function rrect(c, x, y, w, h, r) {
  r = Math.max(0, Math.min(r, w / 2, h / 2));
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}
const FONT = getComputedStyle(document.body).fontFamily;
const isTyping = (el) => !!el && (el.tagName === "TEXTAREA" || el.tagName === "SELECT" || el.isContentEditable ||
  (el.tagName === "INPUT" && !["range", "checkbox", "radio", "button", "file"].includes(el.type)));

// ===========================================================================
// 2. voices + cartoon faces (shared with the web demo)
const BASE = { formantScale: 1, vibratoDepth: 0.35, portamento: 0.2, breathiness: 0.01, grit: 0, choirSize: 1 };
const VOICES = {
  std: { n: "Std Voice", d: "The house voice", face: { skin: "#f2c7a0", hair: "#7a5230" } },
  robot: { n: "Robot", d: "Pressed, pitch-perfect", face: { robot: 1 }, v: { vibratoDepth: 0, portamento: 0.02, breathiness: 0 } },
  male: { n: "Male", d: "Plain adult male", face: { skin: "#e8b58c", hair: "#3a2616" } },
  female: { n: "Female", d: "Brighter, softer", face: { skin: "#f6cfb0", hair: "#a0522d", style: "long", blush: 1 } },
  kid: { n: "Kid", d: "Small and bright", face: { skin: "#ffd9b8", hair: "#e0a030", kid: 1, blush: 1 }, v: { vibratoDepth: 0.12 } },
  soprano: { n: "Soprano", d: "High, rich, late vibrato", face: { skin: "#f3c8a8", hair: "#2a1a12", style: "bun", blush: 1 }, v: { formantScale: 1.2, vibratoDepth: 0.45, breathiness: 0.03 } },
  alto: { n: "Alto", d: "Warm and round", face: { skin: "#c98e66", hair: "#1c120c", style: "long" }, v: { formantScale: 1.12 } },
  tenor: { n: "Tenor", d: "Ringing, forward", face: { skin: "#e6b089", hair: "#4a2e1a" }, v: { formantScale: 1.04, vibratoDepth: 0.33 } },
  bass: { n: "Bass", d: "Big and low", face: { skin: "#d9a07a", hair: "#555", style: "bald", beard: "#444" }, v: { formantScale: 0.9, vibratoDepth: 0.25 } },
  gregorian: { n: "Gregorian", d: "Six monks, no vibrato", face: { skin: "#e3b692", hood: "#6b4a2b" }, v: { formantScale: 0.97, vibratoDepth: 0, portamento: 0.12, choirSize: 6 } },
  choir: { n: "Choir", d: "Eight voices together", group: [{ skin: "#e8b58c", hair: "#3a2616" }, { skin: "#d9a07a", hair: "#555", style: "bald" }, { skin: "#eab896", hair: "#b86b2a" }], v: { vibratoDepth: 0.25, choirSize: 8 } },
  "female-choir": { n: "Fem. Choir", d: "Eight airy sopranos", group: [{ skin: "#f6cfb0", hair: "#a0522d", style: "long" }, { skin: "#c98e66", hair: "#1c120c", style: "long" }, { skin: "#f3c8a8", hair: "#2a1a12", style: "bun" }], v: { formantScale: 1.17, breathiness: 0.07, choirSize: 8 } },
  karaoke: { n: "Karaoke", d: "Big scoops, bright tone", face: { skin: "#f5d0b0", hair: "#e0457b", style: "mohawk", blush: 1 }, v: { vibratoDepth: 0.18 } },
  "quick-vibrato": { n: "Quick Vib", d: "Fast, nervous shimmer", face: { skin: "#f0c29c", hair: "#6b3b8a", style: "mohawk" }, v: { vibratoDepth: 0.4 } },
  hifi: { n: "Hi-Fi", d: "Full bandwidth, modern", face: { skin: "#eab896", hair: "#2b2b2b", phones: 1, glasses: 1 }, v: { breathiness: 0.05 } },
  crooner: { n: "Crooner", d: "Breathy, slow, swoopy", face: { skin: "#e9bb95", hair: "#141414", style: "slick" }, v: { formantScale: 0.95, breathiness: 0.12, portamento: 0.12 } },
};
const voiceMeta = (name) => VOICES[name] || { n: String(name).replace(/(^|-)(\w)/g, (_, a, b) => (a ? " " : "") + b.toUpperCase()), d: "", face: { skin: "#f2c7a0", hair: "#666" } };
const presetValues = (name) => ({ ...BASE, ...(VOICES[name]?.v || {}) });

const SLIDERS = [
  { key: "formantScale", label: "Formant", min: 0.85, max: 1.35, step: 0.01, fmt: (v) => "×" + v.toFixed(2), help: "Lower for a deeper tone; higher for a brighter tone. Note pitch stays the same." },
  { key: "vibratoDepth", label: "Vibrato", min: 0, max: 0.8, step: 0.01, fmt: (v) => Math.round(v * 100) + " ¢" },
  { key: "portamento", label: "Glide", min: 0.02, max: 0.4, step: 0.01, fmt: (v) => Math.round(v * 1000) + " ms" },
  { key: "breathiness", label: "Breath", min: 0, max: 0.2, step: 0.005, fmt: (v) => Math.round(v * 500) + "%" },
  { key: "grit", label: "Grit", min: 0, max: 0.6, step: 0.01, fmt: (v) => Math.round((v / 0.6) * 100) + "%" },
  { key: "choirSize", label: "Choir", min: 1, max: 8, step: 1, fmt: (v) => (v === 1 ? "solo" : v + " voices") },
];

// Painted toy portraits: uneven ink silhouettes, enamel highlights and tiny scuffs.
// All details stay in the 54-unit artboard so the same drawing reads at 34 and 72px.
let faceSeq = 0;
function facePaint(hex, amount) {
  const rgb = hex.replace("#", "");
  const full = rgb.length === 3 ? [...rgb].map(c => c + c).join("") : rgb;
  return "#" + [0, 2, 4].map(i => Math.round(clamp(parseInt(full.slice(i, i + 2), 16) + amount, 0, 255)).toString(16).padStart(2, "0")).join("");
}
function faceSvg(o, size, inner = false) {
  const id = "f" + faceSeq++;
  const skin = o.skin || "#c3c0ba", hair = o.hair || "#34302e";
  const ink = "#171717", blue = "#1555ef", pink = "#eb78a1";
  const paint = (name, base) => `<linearGradient id="${id}-${name}" x1="0" y1="0" x2=".85" y2="1"><stop stop-color="${facePaint(base, 40)}"/><stop offset=".38" stop-color="${base}"/><stop offset="1" stop-color="${facePaint(base, -38)}"/></linearGradient>`;
  const path = (d, fill, width = 1.7) => `<path d="${d}" fill="${fill}" stroke="${ink}" stroke-width="${width}" stroke-linecap="round" stroke-linejoin="round"/>`;
  const glint = (d, color = "#fff7e9", width = 1) => `<path d="${d}" fill="none" stroke="${color}" stroke-width="${width}" stroke-linecap="round"/>`;
  let h = `<defs>${paint("skin", skin)}${paint("hair", hair)}${paint("metal", "#bcbab5")}${paint("blue", blue)}${paint("coat", o.hood || "#485264")}</defs>`;
  h += `<g stroke-linejoin="round">`;
  if (o.robot) {
    h += path("M14 45 Q25 41 40 45 L44 53 L10 53Z", `url(#${id}-metal)`);
    h += path("M23 39 L23 46 L32 47 L33 40Z", pink);
    h += path("M26 10 L27 3 L30 3 L29 11Z", `url(#${id}-metal)`, 1.3);
    h += path("M25 2 Q29 0 32 3 Q33 7 28 7 Q24 6 25 2Z", "#242424", 1.2);
    h += glint("M27 2.5L29 2");
    h += path("M20 13 Q22 7 28 8 Q35 9 36 15Z", `url(#${id}-blue)`);
    h += path("M8 21 Q3 20 3 26 L3 34 Q4 38 9 37 L12 24Z", `url(#${id}-blue)`);
    h += path("M44 23 Q50 22 51 29 L50 36 Q49 40 44 39Z", `url(#${id}-blue)`);
    h += path("M13 12 Q14 10 17 11 L44 16 Q47 16 46 20 L43 43 Q43 46 39 45 L10 41 Q8 41 9 37Z", `url(#${id}-metal)`, 2);
    h += glint("M13 17L14 13L20 14M12 34L11 39L16 40M41 19L43 20L42 25", "#fff9ed", 1.2);
    h += glint("M29 22L31 20M34 39L38 35M15 29L17 27", "#95958e", .55);
    h += `<circle cx="20" cy="24" r="2.5" fill="${ink}"/><circle cx="36" cy="26" r="2.5" fill="${ink}"/><circle cx="19.3" cy="23.1" r=".65" fill="#fff"/><circle cx="35.3" cy="25.1" r=".65" fill="#fff"/>`;
    h += `<ellipse class="mouth" cx="27" cy="35" rx="4" ry="1.5" fill="${ink}"/>`;
    h += `<path d="M17 32L16.5 37M21 32.5L20.5 37.5M33 34L32.5 39M37 34.5L36.5 39.5" stroke="${ink}" stroke-width="2.2"/>`;
    h += glint("M6 24L6 28M47 26L48 29M24 10L28 10", "#a9d9ff", 1.1);
  } else {
    if (o.style === "long") h += path("M10 46 Q6 27 13 14 Q19 5 31 9 Q47 10 46 30 L46 49Z", `url(#${id}-hair)`, 2);
    if (o.hood) h += path("M6 51 Q5 19 16 10 Q26 2 36 12 Q48 23 48 52Z", `url(#${id}-coat)`, 2);
    if (o.style === "bun") h += path("M22 12 Q16 8 21 3 Q27 -1 31 4 Q34 9 29 13Z", `url(#${id}-hair)`);
    h += path("M7 53 Q9 45 19 44 L35 44 Q44 46 47 53Z", `url(#${id}-coat)`);
    h += path("M22 39 L21 46 Q26 51 33 46 L32 39Z", `url(#${id}-skin)`, 1.3);
    h += path("M19 45 L25 50 L18 50Z", pink, 1.1) + path("M34 45 L28 50 L36 50Z", blue, 1.1);
    const shift = o.kid ? 2 : 0;
    h += `<g transform="translate(0 ${shift})">`;
    h += path("M12 25 Q7 23 8 29 Q8 33 13 33 M41 25 Q46 23 45 29 Q44 34 40 33", `url(#${id}-skin)`, 1.4);
    h += path(o.kid ? "M13 22 Q14 13 27 13 Q40 13 41 23 L40 34 Q38 43 27 44 Q15 43 13 34Z" : "M12 21 Q13 10 27 11 Q41 11 42 23 L40 35 Q37 44 27 45 Q17 44 13 35Z", `url(#${id}-skin)`, 1.9);
    h += glint("M15 22L16 18L19 16M15 34L17 37", facePaint(skin, 53), 1.25);
    if ((!o.style || o.style === "short") && !o.hood) h += path("M12 26 Q9 13 18 10 Q28 5 38 12 Q43 16 41 26 L37 19 Q30 16 23 20 L15 19Z", `url(#${id}-hair)`, 1.8);
    if (o.style === "slick") h += path("M12 27 Q6 12 21 8 Q35 5 41 16 L42 25 L37 20 L36 15 Q25 23 15 19Z", `url(#${id}-hair)`, 1.8);
    if (o.style === "long") h += path("M11 30 Q7 14 21 10 Q34 6 42 20 L42 32 L37 22 Q29 22 24 15 Q22 23 14 23Z", `url(#${id}-hair)`, 1.8);
    if (o.style === "bun") h += path("M12 26 Q9 11 26 10 Q43 10 42 26 L37 20 Q30 19 26 15 Q20 21 15 20Z", `url(#${id}-hair)`, 1.8);
    if (o.style === "bald") h += path("M12 22L16 20L15 29L12 29Z", `url(#${id}-hair)`, 1) + path("M39 20L42 23L40 30L38 28Z", `url(#${id}-hair)`, 1);
    if (o.style === "mohawk") h += path("M21 19 L20 10 L25 12 L26 3 L30 9 L34 5 L34 19Z", `url(#${id}-hair)`, 1.7);
    if (!o.hood && o.style !== "bald") h += glint(o.style === "mohawk" ? "M24 16L24 13M29 14L29 10" : "M16 16Q21 11 28 12M32 13L35 15", facePaint(hair, 65), 1.1);
    if (o.beard) h += path("M14 33 L18 35 L23 33 L27 35 L32 33 L37 35 L40 32 Q39 44 27 47 Q16 44 14 33Z", `url(#${id}-hair)`, 1.4);
    if (o.glasses) {
      h += path("M15 25L24 25L24 30Q19 33 15 29ZM30 25L39 25L38 30Q33 33 30 29Z", "#22252b", 1.4);
      h += glint("M24 27L30 27", ink, 1.4) + glint("M17 26L21 26M32 26L36 26", "#8ca9c9", .9);
    } else {
      h += `<ellipse cx="20" cy="28" rx="2" ry="2.4" fill="${ink}"/><ellipse cx="34" cy="28.5" rx="2" ry="2.4" fill="${ink}"/><circle cx="19.5" cy="27.2" r=".65" fill="#fff"/><circle cx="33.5" cy="27.7" r=".65" fill="#fff"/>`;
    }
    h += glint("M27 28L26 32L28 32", facePaint(skin, -50), .8);
    if (o.blush) h += `<path d="M16 33L19 34M35 34L38 33" stroke="#d7807e" stroke-width="1.8" stroke-linecap="round"/>`;
    h += `<ellipse class="mouth" cx="27" cy="37" rx="4" ry="1.5" fill="#321d20" stroke="${ink}" stroke-width=".7"/>`;
    h += glint("M25 41L28 41", o.beard ? facePaint(hair, 60) : facePaint(skin, 35), .75);
    h += `</g>`;
    if (o.phones) {
      h += glint("M9 28Q8 8 27 8Q47 8 45 29", ink, 3);
      h += path("M7 25L12 24L13 36L8 36Q5 32 7 25Z", `url(#${id}-blue)`, 1.5);
      h += path("M42 24L47 25Q49 32 46 36L41 36Z", `url(#${id}-blue)`, 1.5);
      h += glint("M8 27L8 31M44 26L45 28", "#c0e1ff", .9);
    }
    if (o.hood) h += glint("M11 43Q9 25 17 16M38 16Q44 24 43 40", facePaint(o.hood, 50), 1.1);
  }
  h += "</g>";
  return inner ? h : `<svg width="${size}" height="${size}" viewBox="0 0 54 54" aria-hidden="true">${h}</svg>`;
}
function voiceSvg(v, size) {
  if (!v.group) return faceSvg(v.face, size);
  let h = `<svg width="${size}" height="${size}" viewBox="0 0 54 54" aria-hidden="true">`;
  v.group.forEach((o, i) => {
    const x = [-11, 11, 0][i], y = [-6, -6, 7][i], sc = 0.61;
    h += `<g transform="translate(${27 + x - 27 * sc},${27 + y - 27 * sc}) scale(${sc})">${faceSvg(o, 54, true)}</g>`;
  });
  return h + "</svg>";
}
// ARPAbet vowel -> mouth [rx, ry]
const MOUTH = { AA: [6, 5], AO: [5, 5], AW: [5.5, 5], AY: [6, 4.5], AE: [7, 3.5], EH: [7, 3], EY: [7.5, 2.6], AH: [5.5, 4], AX: [5, 3.2], IY: [8, 1.6], IH: [7, 2], OW: [4, 4.5], OY: [4.5, 4.2], UH: [3.5, 3.5], UW: [3, 3], ER: [5, 2.8], M: [4, 0.8], N: [4.5, 1.4], NG: [4.5, 1.5], L: [5, 2.5] };
let mouthNow = "";
function setMouth(ph) {
  const key = ph || "";
  if (key === mouthNow) return;
  mouthNow = key;
  const [rx, ry] = MOUTH[ph] || (ph ? [5, 2.5] : [4, 1.5]);
  document.querySelectorAll("#bigface .mouth").forEach((m) => { m.setAttribute("rx", rx); m.setAttribute("ry", ry); });
}

// ===========================================================================
// 3. model, undo, sync
let B = null; // backend
let nextId = 1;
const S = {
  song: { lyrics: "", voice: "std", notes: [] },
  plan: null, planBpm: 120, planFailed: false, labels: new Map(),
  bpm: 120,
  status: { beats: 0, bpm: 120, playing: false, host: true, capture: false, rendering: false, error: "", songVersion: null },
  seenVersion: undefined,
  voices: Object.keys(VOICES),
  sel: new Set(), hover: null, hotPitch: null,
  snap: 0.25, ppb: 56, rowH: 14, lo: 36, hi: 96,
  lastDur: 1, loaded: false, flash: null,
};

function normSong(raw) {
  const s = raw && typeof raw === "object" && !Array.isArray(raw) ? raw : {};
  const out = { ...s };
  out.lyrics = typeof s.lyrics === "string" ? s.lyrics : "";
  out.voice = s.voice ?? "std";
  out.notes = (Array.isArray(s.notes) ? s.notes : [])
    .map((n) => {
      const o = { pitch: clamp(parsePitch(n.pitch), 0, 127), start: Math.max(0, +n.start || 0), dur: Math.max(1 / 64, +n.dur || 0.25), _id: nextId++ };
      if (n.lyric) o.lyric = String(n.lyric);
      return o;
    })
    .filter((n) => Number.isFinite(n.pitch));
  sortNotes(out.notes);
  // One record per audio import (song-merge.js). Songs saved before region-by-region import kept
  // their one import in `transcription`: it still reads (and sets) the latest import, but only the
  // list is saved.
  out.transcriptions = songTranscriptions(s);
  delete out.transcription;
  Object.defineProperty(out, "transcription", {
    configurable: true, enumerable: false,
    get() { return this.transcriptions[this.transcriptions.length - 1] ?? null; },
    set(v) { this.transcriptions = v && typeof v === "object" ? [v] : []; },
  });
  return out;
}
const sortNotes = (notes) => notes.sort((a, b) => a.start - b.start || a.pitch - b.pitch);
function serialize(song) {
  const { notes, ...rest } = song;
  return JSON.stringify({
    ...rest,
    notes: sortNotes([...notes]).map((n) => {
      const o = { pitch: n.pitch, start: r6(n.start), dur: r6(n.dur) };
      if (n.lyric) o.lyric = n.lyric;
      return o;
    }),
  });
}
// What a plan depends on in a song, as serialize() writes it (not the lyric text while the notes
// carry their own lyrics).
function notesKey(song) {
  const notes = sortNotes([...(song?.notes || [])]).map((n) => [n.pitch, r6(n.start), r6(n.dur), n.lyric || ""]);
  return JSON.stringify([notes, song?.dictionary ?? null, !!song?.useNoteLyrics, song?.useNoteLyrics ? "" : song?.lyrics]);
}
// Order-independent form for comparing our song with the plugin's copy.
function canon(song) {
  const sortKeys = (v) => Array.isArray(v) ? v.map(sortKeys) : v && typeof v === "object"
    ? Object.fromEntries(Object.keys(v).sort().map((k) => [k, sortKeys(v[k])])) : v;
  return JSON.stringify(sortKeys(JSON.parse(serialize(song))));
}
const noteById = (id) => S.song.notes.find((n) => n._id === id);
const selectedNotes = () => S.song.notes.filter((n) => S.sel.has(n._id));

// ---- undo / redo: stacks of serialized songs
const H = { undo: [], redo: [], coKey: null, coT: 0 };
function checkpoint(coalesceKey) {
  const now = performance.now();
  if (coalesceKey && H.coKey === coalesceKey && now - H.coT < 1200) { H.coT = now; return; }
  H.coKey = coalesceKey || null;
  H.coT = now;
  H.undo.push(serialize(S.song));
  if (H.undo.length > 300) H.undo.shift();
  H.redo.length = 0;
}
// Drop a checkpoint that turned out to change nothing (e.g. a drag that snapped back).
function dropNoopCheckpoint() {
  if (H.undo.length && H.undo[H.undo.length - 1] === serialize(S.song)) H.undo.pop();
}
function restore(json) {
  const old = selectedNotes().map((n) => `${n.pitch}@${r6(n.start)}`);
  S.song = normSong(JSON.parse(json));
  S.sel = new Set(S.song.notes.filter((n) => old.includes(`${n.pitch}@${r6(n.start)}`)).map((n) => n._id));
  H.coKey = null;
  restoreSource();
  onSongChanged({ lyrics: true });
}
function undo() {
  if (!H.undo.length) return flash("NOTHING TO UNDO");
  H.redo.push(serialize(S.song));
  restore(H.undo.pop());
}
function redo() {
  if (!H.redo.length) return flash("NOTHING TO REDO");
  H.undo.push(serialize(S.song));
  restore(H.redo.pop());
}

// ---- keep the line monophonic: the notes just edited ("winners") keep their
// extent; any other note they overlap is trimmed (or removed if nothing is left).
function resolveOverlaps(winnerIds = new Set()) {
  const eps = 1e-6, tiny = 1 / 64, before = S.song.notes.length;
  const W = sortNotes(S.song.notes.filter((n) => winnerIds.has(n._id)));
  // winners among themselves
  for (let i = 0; i < W.length - 1; i++) {
    if (W[i].start + W[i].dur > W[i + 1].start + eps) W[i].dur = W[i + 1].start - W[i].start;
  }
  const keep = [];
  for (const n of S.song.notes) {
    if (winnerIds.has(n._id)) { if (n.dur > tiny - eps) keep.push(n); continue; }
    let a = n.start, b = n.start + n.dur;
    for (const w of W) {
      const wa = w.start, wb = w.start + w.dur;
      if (wb <= a + eps || wa >= b - eps) continue;
      if (wa <= a + eps) a = wb; // winner covers our start: push start later
      else b = wa; // winner starts inside: trim our end
      if (b - a < tiny) break;
    }
    if (b - a >= tiny) { n.start = a; n.dur = b - a; keep.push(n); }
  }
  // any remaining overlaps (e.g. from imports): earlier note yields
  sortNotes(keep);
  const out = [];
  for (let i = 0; i < keep.length; i++) {
    const n = keep[i], next = keep[i + 1];
    if (next && n.start + n.dur > next.start + eps) {
      if (next.start - n.start < tiny) { if (winnerIds.has(n._id) && !winnerIds.has(next._id)) { keep[i + 1] = n; } continue; }
      n.dur = next.start - n.start;
    }
    out.push(n);
  }
  S.song.notes = out;
  for (const id of [...S.sel]) if (!out.some((n) => n._id === id)) S.sel.delete(id);
  const removed = before - out.length;
  if (removed > 0 && winnerIds.size) flash("REPLACED " + removed + (removed > 1 ? " NOTES" : " NOTE"), "one voice sings one note at a time · ⌘Z undoes");
  return removed;
}

// ---- sync with the plugin
let pushTimer = 0, inFlight = 0, lastPushed = "", needPull = false;
function schedulePush() {
  clearTimeout(pushTimer);
  pushTimer = setTimeout(pushNow, 150);
}
async function pushNow() {
  pushTimer = 0;
  const json = serialize(S.song);
  if (json !== lastPushed) {
    lastPushed = json;
    inFlight++;
    try { await B.setSong(json); } catch (e) { console.warn("setSong failed", e); }
    inFlight--;
    fetchPlan();
  }
  if (!inFlight && !pushTimer && needPull) { needPull = false; pullSong(); }
}
let planSeq = 0, planTimer = 0;
async function fetchPlan() {
  const my = ++planSeq;
  const plannedSong = lastPushed, bpm = S.bpm;
  let txt = "";
  try { txt = await B.getPlan(); } catch {}
  if (my !== planSeq) return;
  let plan = null;
  try { plan = txt ? JSON.parse(txt) : null; } catch {}
  S.plan = plan;
  S.planBpm = bpm;
  try { S.planKey = notesKey(JSON.parse(plannedSong)); } catch { S.planKey = ""; } // Never tag an older response as a newer song.
  S.planFailed = !plan && S.song.notes.length > 0;
  relabel();
  refreshTexts();
  dirty();
}
// The plugin changed the song itself (MIDI capture, project reload...).
async function pullSong() {
  if (pushTimer || inFlight || drag) { needPull = true; return; }
  let txt = "";
  try { txt = await B.getSong(); } catch { return; }
  if (pushTimer || inFlight || drag) { needPull = true; return; }
  let parsed;
  try { parsed = JSON.parse(txt); } catch { return; }
  const incoming = normSong(parsed);
  if (canon(incoming) === canon(S.song)) return;
  checkpoint();
  const wasEmpty = !S.song.notes.length;
  S.song = incoming;
  await restoreSource();
  S.sel.clear();
  lastPushed = serialize(incoming);
  onSongChanged({ lyrics: true, push: false });
  if (wasEmpty && !S.status.playing) scrollToNotes();
  fetchPlan();
}

function onSongChanged({ lyrics = false, push = true } = {}) {
  if (lyrics || document.activeElement !== els.lyrics) {
    if (els.lyrics.value !== S.song.lyrics) els.lyrics.value = S.song.lyrics;
  }
  syncVoiceUI();
  syncKeyUI();
  updateWorld();
  relabel();
  refreshTexts();
  dirty();
  if (push) schedulePush();
}

// ===========================================================================
// 4. plan -> what each note sings
function relabel() {
  S.labels = labelNotes(sortNotes([...S.song.notes]), S.plan, S.planBpm);
  if (mirrorNoteLyrics()) schedulePush();
}
// While the notes carry their own lyrics (an audio import or singing take, or after splitting or
// joining syllables), the lyric box shows those lyrics as text that flows back onto the same
// notes: hyphenated, "_" on held notes, "mm" on lyric-less ones. Editing the text then keeps every
// syllable where it was. The labels say which one-note words need "~" to stay on their note.
function mirrorNoteLyrics() {
  // Wait for a plan of these very notes: stale labels could drop a "~" a word needs.
  if (!S.song.useNoteLyrics || !S.plan || S.planKey !== notesKey(S.song)) return false;
  const text = lyricsFromNotes(S.song.notes, { labels: S.labels }) ?? "";
  if (text === S.song.lyrics) return false;
  S.song.lyrics = text;
  if (els.lyrics.value !== text) els.lyrics.value = text;
  return true;
}

// ===========================================================================
// 5. roll drawing
const els = {
  app: $("app"), grid: $("grid"), keys: $("keys"), ruler: $("ruler"), scroller: $("scroller"), sizer: $("sizer"), gridwrap: $("gridwrap"),
  lyrics: $("lyrics"), counts: $("counts"), warn: $("warn"), info: $("info"), snap: $("snap"), lyricEdit: $("lyricEdit"),
  lcd: $("lcd"), lcdState: $("lcdState"), lcdPos: $("lcdPos"), lcdLine: $("lcdLine"),
  capture: $("capture"), play: $("play"), playIco: $("playIco"), playLbl: $("playLbl"), tempo: $("tempo"),
  file: $("file"), mode: $("mode"),
};
function syncKeyUI() {
  const settings = S.song.keyCorrection || {root:-1,scale:'major',mode:'gentle'};
  $('keyRoot').value = String(settings.root);
  $('keyScale').value = settings.scale;
  $('keyMode').value = settings.mode;
  $('keyApply').disabled = Number(settings.root)<0 || !S.song.notes.length;
  syncKeyLabel();
}
// The bottom bar's Key button names the key in use ("Key: Off", "Key: E♭ minor").
function syncKeyLabel() {
  const root = Number($('keyRoot').value), btn = $('keyBtn');
  btn.textContent = root < 0 ? 'Key: Off' : `Key: ${$('keyRoot').selectedOptions[0].text} ${$('keyScale').value}`;
  btn.classList.toggle('on', root >= 0);
}
for (const id of ['keyRoot','keyScale','keyMode']) $(id).addEventListener('change', () => {
  S.song.keyCorrection = {...S.song.keyCorrection,root:Number($('keyRoot').value),scale:$('keyScale').value,mode:$('keyMode').value,auto:false};
  $('keyApply').disabled = Number($('keyRoot').value)<0 || !S.song.notes.length;
  syncKeyLabel();
  schedulePush();
});
$('keyApply').addEventListener('click', () => {
  const settings = S.song.keyCorrection;
  const candidates = S.sel.size ? selectedNotes() : S.song.notes;
  // Every import's evidence, the latest first (it replaced earlier imports where they overlap).
  const evidence = songTranscriptions(S.song).slice().reverse().flatMap(t => t.originalNotes || []);
  const changes = candidates.flatMap(n => {
    const original = evidenceFor(n, evidence);  // also behind a note since moved onto the grid or into the key
    const pitch = correctedPitch(n,settings,original);
    return pitch===n.pitch ? [] : [{note:n,before:n.pitch,after:pitch}];
  });
  if (!changes.length) {flash('NO CHANGES', settings.mode==='gentle' ? 'No uncertain notes close enough to the chosen scale' : 'Notes already fit the chosen scale');return;}
  checkpoint();
  S.song.keyCorrection = {...settings,changes:changes.map(c=>({start:c.note.start,dur:c.note.dur,before:c.before,after:c.after}))};
  S.sel.clear();
  for(const c of changes) {c.note.pitch=c.after;S.sel.add(c.note._id);}
  onSongChanged();scrollToNotes();flash('PITCH CORRECTED', `${changes.length} notes · Undo restores the previous pitches`);
});
const view = { w: 300, h: 200, dpr: 1 };
let needDraw = true;
const dirty = () => { needDraw = true; };

function songEnd() { return S.song.notes.reduce((m, n) => Math.max(m, n.start + n.dur), 0); }
function updateWorld() {
  let lo = 36, hi = 96;
  for (const n of S.song.notes) { lo = Math.min(lo, n.pitch - 2); hi = Math.max(hi, n.pitch + 2); }
  lo = Math.max(0, lo); hi = Math.min(127, hi);
  if (hi !== S.hi) els.scroller.scrollTop += (hi - S.hi) * S.rowH;
  S.lo = lo; S.hi = hi;
  const sx = els.scroller.scrollLeft;
  const beats = Math.max(64, Math.ceil((songEnd() + 16) / 4) * 4, Math.ceil((S.status.beats + 8) / 4) * 4, (sx + view.w) / S.ppb);
  const w = Math.ceil(beats * S.ppb) + "px", h = (hi - lo + 1) * S.rowH + "px";
  if (els.sizer.style.width !== w) els.sizer.style.width = w;
  if (els.sizer.style.height !== h) els.sizer.style.height = h;
}
// `setStyle` false: the canvas is sized by CSS (ruler, keys) and only the
// backing store follows its content box.
function sizeCanvas(cv, w, h, setStyle = true) {
  const dpr = window.devicePixelRatio || 1;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
    cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
    if (setStyle) { cv.style.width = w + "px"; cv.style.height = h + "px"; }
  }
  const c = cv.getContext("2d");
  c.setTransform(dpr, 0, 0, dpr, 0, 0);
  return c;
}
function measure() {
  view.w = els.scroller.clientWidth || 300;
  view.h = els.scroller.clientHeight || 200;
  updateWorld();
  dirty();
}

function noteAtBeat(b) { return S.song.notes.find((n) => b >= n.start && b < n.start + n.dur) || null; }

function draw() {
  drawTimeline();
  drawSourceWave();
  const sx = els.scroller.scrollLeft, sy = els.scroller.scrollTop;
  const { w: W, h: Hh } = view;
  const { ppb, rowH, hi, lo } = S;
  const c = sizeCanvas(els.grid, W, Hh);
  const xOf = (b) => b * ppb - sx, yOf = (p) => (hi - p) * rowH - sy;
  const st = S.status;
  const nowNote = st.playing ? noteAtBeat(st.beats) : null;

  // rows
  c.fillStyle = "#fff";
  c.fillRect(0, 0, W, Hh);
  const pTop = Math.min(hi, hi - Math.floor(sy / rowH)), pBot = Math.max(lo, hi - Math.floor((sy + Hh) / rowH));
  for (let p = pTop; p >= pBot; p--) {
    const y = yOf(p);
    if (isBlack(p)) { c.fillStyle = "#eef2f8"; c.fillRect(0, y, W, rowH); }
    if (p === S.hotPitch) { c.fillStyle = "rgba(59,142,232,.16)"; c.fillRect(0, y, W, rowH); }
    c.fillStyle = p % 12 === 0 ? "#c3cedd" : "#e6eaf0";
    c.fillRect(0, y + rowH - 1, W, 1);
  }
  // below the lowest row
  const yEnd = yOf(lo - 1);
  if (yEnd < Hh) { c.fillStyle = "#d9dde3"; c.fillRect(0, yEnd, W, Hh - yEnd); }

  // columns: sub-grid, beats, bars
  const b0 = Math.floor(sx / ppb), b1 = Math.ceil((sx + W) / ppb);
  const g = S.snap || 0.25;
  if (g < 1 && g * ppb >= 9) {
    c.fillStyle = "#f3f5f8";
    for (let b = b0; b <= b1; b += g) { const k = Math.round(b / g) * g; if (k % 1) c.fillRect(Math.round(xOf(k)), 0, 1, Hh); }
  }
  for (let b = b0; b <= b1; b++) {
    if (b % 4 && ppb < 10) continue;
    c.fillStyle = b % 4 === 0 ? "#b3bfce" : "#dfe4eb";
    c.fillRect(Math.round(xOf(b)), 0, 1, Hh);
  }

  // ties for held vowels (drawn under the notes)
  const notes = S.song.notes;
  const nh = rowH - 1;
  for (let i = 1; i < notes.length; i++) {
    const n = notes[i], l = S.labels.get(n._id);
    if (!l || l.kind !== "hold") continue;
    const p = notes[i - 1];
    const x1 = xOf(p.start + p.dur) - 4, y1 = yOf(p.pitch) + nh / 2, x2 = xOf(n.start) + 4, y2 = yOf(n.pitch) + nh / 2;
    if (Math.max(x1, x2) < -20 || Math.min(x1, x2) > W + 20) continue;
    c.strokeStyle = "#2f6fb8"; c.lineWidth = 1.3; c.setLineDash([3, 2]);
    c.beginPath(); c.moveTo(x1, y1);
    c.bezierCurveTo(x1 + 8, y1 + (y2 === y1 ? 7 : 0), x2 - 8, y2 + (y2 === y1 ? 7 : 0), x2, y2);
    c.stroke(); c.setLineDash([]);
  }

  // notes
  const fs = clamp(nh - 3, 9, 12);
  for (const n of notes) {
    const x = xOf(n.start) + 0.5, y = yOf(n.pitch) + 0.5, w = Math.max(n.dur * ppb - 1.5, 3);
    if (x > W || x + w < 0 || y > Hh || y + nh < 0) continue;
    const l = S.labels.get(n._id);
    const kind = l ? l.kind : "word";
    const sel = S.sel.has(n._id), hov = S.hover === n._id, isNow = nowNote === n;
    const gr = c.createLinearGradient(0, y, 0, y + nh);
    if (isNow) { gr.addColorStop(0, "#fff2c0"); gr.addColorStop(0.48, "#ffc53d"); gr.addColorStop(0.52, "#f0a800"); gr.addColorStop(1, "#ffe28a"); }
    else if (sel) { gr.addColorStop(0, "#9cc8ff"); gr.addColorStop(0.48, "#2f7ae0"); gr.addColorStop(0.52, "#1a5fc4"); gr.addColorStop(1, "#6fb0ff"); }
    else if (kind === "hum") { gr.addColorStop(0, "#f4f4f4"); gr.addColorStop(0.5, "#d3d8de"); gr.addColorStop(0.52, "#c0c7cf"); gr.addColorStop(1, "#e4e8ec"); }
    else if (kind === "hold") { gr.addColorStop(0, "#e6f2ff"); gr.addColorStop(0.5, "#b0d4fa"); gr.addColorStop(0.52, "#96c5f6"); gr.addColorStop(1, "#d8ebff"); }
    else { gr.addColorStop(0, "#bfe0ff"); gr.addColorStop(0.48, "#5aa8f4"); gr.addColorStop(0.52, "#2f84e3"); gr.addColorStop(1, "#9fd2ff"); }
    c.save();
    if (isNow) { c.shadowColor = "rgba(255,190,30,.95)"; c.shadowBlur = 12; }
    else if (hov) { c.shadowColor = "rgba(0,60,160,.45)"; c.shadowBlur = 5; }
    rrect(c, x, y, w, nh - 1, 4); c.fillStyle = gr; c.fill();
    c.restore();
    c.lineWidth = sel ? 1.5 : 1;
    c.strokeStyle = sel ? "#0a3474" : isNow ? "#a06a00" : kind === "hum" ? "#8a939c" : "rgba(0,40,110,.55)";
    rrect(c, x, y, w, nh - 1, 4); c.stroke();
    // gloss
    if (w > 6) { rrect(c, x + 3, y + 1, w - 6, (nh - 1) * 0.42, 3); c.fillStyle = "rgba(255,255,255,.5)"; c.fill(); }
    // resize grip
    if ((sel || hov) && w > 14) {
      c.fillStyle = sel ? "rgba(255,255,255,.75)" : "rgba(0,40,110,.4)";
      c.fillRect(x + w - 4, y + 3, 1, nh - 7); c.fillRect(x + w - 6, y + 3, 1, nh - 7);
    }
    // own-lyric marker
    if (n.lyric) { c.fillStyle = "#e89a00"; c.beginPath(); c.moveTo(x + w - 7, y); c.lineTo(x + w - 1, y); c.lineTo(x + w - 1, y + 6); c.fill(); }
    // label
    const label = l ? l.text : n.lyric || "";
    if (label && nh >= 8) {
      c.font = `${kind === "word" ? "bold " : kind === "hum" ? "italic " : ""}${fs}px ${FONT}`;
      c.textBaseline = "middle"; c.textAlign = "left";
      c.fillStyle = isNow ? "#3a2400" : sel ? "#fff" : kind === "hum" ? "#4a525a" : "#001a3a";
      const tw = c.measureText(label).width;
      if (tw + 7 <= w || w > 22) {
        c.save(); rrect(c, x + 1, y, w - 2, nh, 3); c.clip();
        c.fillText(label, x + 4, y + nh / 2); c.restore();
      } else {
        c.fillStyle = "#1f4f8e"; c.fillText(label, x, y - fs / 2 - 1);
      }
    }
  }

  // marquee
  if (drag && drag.mode === "marquee" && drag.moved) {
    const x0 = Math.min(drag.p0.wx, drag.p1.wx) - sx, y0 = Math.min(drag.p0.wy, drag.p1.wy) - sy;
    const w = Math.abs(drag.p1.wx - drag.p0.wx), h = Math.abs(drag.p1.wy - drag.p0.wy);
    c.fillStyle = "rgba(59,142,232,.14)"; c.fillRect(x0, y0, w, h);
    c.strokeStyle = "#3b8ee8"; c.lineWidth = 1; c.strokeRect(x0 + 0.5, y0 + 0.5, w, h);
  }

  // playhead
  const px = xOf(st.beats);
  if ((st.playing || st.beats > 0) && px > -4 && px < W + 4) {
    c.fillStyle = "rgba(224,50,31,.18)"; c.fillRect(px - 3, 0, 7, Hh);
    c.fillStyle = "#e0321f"; c.fillRect(px - 0.5, 0, 1.5, Hh);
  }

  drawRuler(sx);
  drawKeys(sy, nowNote);
}

function drawRuler(sx) {
  const cv = els.ruler, W = cv.clientWidth || view.w, Hh = cv.clientHeight || 20;
  const c = sizeCanvas(cv, W, Hh, false);
  const gr = c.createLinearGradient(0, 0, 0, Hh);
  gr.addColorStop(0, "#fbfbfb"); gr.addColorStop(1, "#dcdcdc");
  c.fillStyle = gr; c.fillRect(0, 0, W, Hh);
  const ppb = S.ppb, b0 = Math.floor(sx / ppb), b1 = Math.ceil((sx + W) / ppb);
  const barEvery = ppb * 4 < 34 ? (ppb * 4 < 18 ? 4 : 2) : 1;
  c.font = `10px ${FONT}`; c.textBaseline = "top"; c.textAlign = "left";
  for (let b = Math.max(0, b0); b <= b1; b++) {
    const x = Math.round(b * ppb - sx);
    if (b % 4 === 0) {
      c.fillStyle = "#8a8a8a"; c.fillRect(x, 0, 1, Hh);
      if ((b / 4) % barEvery === 0) { c.fillStyle = "#333"; c.fillText(String(b / 4 + 1), x + 3, 3); }
    } else if (ppb >= 10) {
      c.fillStyle = "#aaa"; c.fillRect(x, Hh - 5, 1, 5);
    }
  }
  // song end marker
  const end = songEnd();
  if (end > 0) {
    const x = end * ppb - sx;
    c.fillStyle = "rgba(59,142,232,.8)";
    c.beginPath(); c.moveTo(x, Hh - 7); c.lineTo(x + 6, Hh - 1); c.lineTo(x, Hh - 1); c.fill();
  }
  const st = S.status, px = st.beats * ppb - sx;
  if ((st.playing || st.beats > 0) && px > -8 && px < W + 8) {
    c.fillStyle = "#e0321f";
    c.beginPath(); c.moveTo(px - 6, 3); c.lineTo(px + 7, 3); c.lineTo(px + 0.5, Hh); c.fill();
  }
  c.fillStyle = "#9a9a9a"; c.fillRect(0, Hh - 1, W, 1);
}

function drawKeys(sy, nowNote) {
  const cv = els.keys, W = cv.clientWidth || 46, Hh = view.h;
  const c = sizeCanvas(cv, W, cv.clientHeight || Hh, false);
  const { rowH, hi, lo } = S;
  c.fillStyle = "#d9dde3"; c.fillRect(0, 0, W, cv.clientHeight || Hh);
  const hovNote = S.hover != null ? noteById(S.hover) : null;
  const pTop = Math.min(hi, hi - Math.floor(sy / rowH)), pBot = Math.max(lo, hi - Math.floor((sy + Hh) / rowH));
  for (let p = pTop; p >= pBot; p--) {
    const y = (hi - p) * rowH - sy, black = isBlack(p);
    const g = c.createLinearGradient(0, 0, W, 0);
    if (black) { g.addColorStop(0, "#1a1a1a"); g.addColorStop(1, "#4a4a4a"); } else { g.addColorStop(0, "#d6d6d6"); g.addColorStop(0.7, "#fff"); g.addColorStop(1, "#e8e8e8"); }
    let fill = g;
    if (nowNote && nowNote.pitch === p) fill = "#ffc53d";
    else if (p === S.hotPitch) fill = "#5aa8f4";
    else if (hovNote && hovNote.pitch === p) fill = black ? "#3a5f8e" : "#cfe4ff";
    c.fillStyle = fill;
    c.fillRect(0, y, black ? W - 12 : W, rowH);
    if (black) { c.fillStyle = "#f4f4f4"; c.fillRect(W - 12, y, 12, rowH); }
    c.fillStyle = p % 12 === 0 || p % 12 === 5 ? "#999" : "#c4c4c4";
    c.fillRect(0, y + rowH - 1, W, 1);
    if (p % 12 === 0 && rowH >= 9) {
      c.fillStyle = p === S.hotPitch ? "#fff" : "#333";
      c.font = `${Math.min(10, rowH - 2)}px ${FONT}`;
      c.textAlign = "right"; c.textBaseline = "middle";
      c.fillText(pitchName(p), W - 4, y + rowH / 2);
    }
  }
}

function frame() {
  if (needDraw) { needDraw = false; draw(); }
  requestAnimationFrame(frame);
}

// ---- view helpers
function zoomTo(ppb, anchorX = view.w / 2) {
  const beat = (els.scroller.scrollLeft + anchorX) / S.ppb;
  S.ppb = clamp(ppb, 6, 400);
  updateWorld();
  els.scroller.scrollLeft = Math.max(0, beat * S.ppb - anchorX);
  dirty();
}
function zoomRows(rowH, anchorY = view.h / 2) {
  const row = (els.scroller.scrollTop + anchorY) / S.rowH;
  S.rowH = clamp(Math.round(rowH), 8, 30);
  updateWorld();
  els.scroller.scrollTop = Math.max(0, row * S.rowH - anchorY);
  dirty();
}
function zoomFit() {
  const end = songEnd();
  if (!end) return zoomTo(56, 0);
  S.ppb = clamp((view.w - 24) / (end + 0.5), 6, 400);
  updateWorld();
  els.scroller.scrollLeft = 0;
  dirty();
}
function scrollToNotes() {
  const notes = S.song.notes;
  let mid = 66, first = 0;
  if (notes.length) {
    const ps = notes.map((n) => n.pitch);
    mid = (Math.min(...ps) + Math.max(...ps)) / 2;
    first = notes[0].start;
  }
  updateWorld();
  els.scroller.scrollTop = Math.max(0, (S.hi - mid) * S.rowH - view.h / 2);
  els.scroller.scrollLeft = Math.max(0, first * S.ppb - 40);
  dirty();
}

// ===========================================================================
// 6. roll input
const sc = els.scroller;
let drag = null;
const snapOn = () => S.snap > 0;
const snapB = (b, fn = "round") => (snapOn() ? Math[fn](b / S.snap + 1e-9) * S.snap : b);
const minDur = () => S.snap || 1 / 32;
const nudgeStep = () => S.snap || 0.25;

function evPos(e) {
  const r = sc.getBoundingClientRect();
  const x = e.clientX - r.left, y = e.clientY - r.top;
  return { x, y, wx: x + sc.scrollLeft, wy: y + sc.scrollTop, inView: x >= 0 && y >= 0 && x < sc.clientWidth && y < sc.clientHeight };
}
const beatAt = (wx) => wx / S.ppb;
const pitchAt = (wy) => clamp(S.hi - Math.floor(wy / S.rowH), 0, 127);
function hitNote(p) {
  const b = beatAt(p.wx), pitch = pitchAt(p.wy);
  const notes = S.song.notes;
  for (let i = notes.length - 1; i >= 0; i--) {
    const n = notes[i];
    if (n.pitch !== pitch || b < n.start || b > n.start + n.dur) continue;
    const endX = (n.start + n.dur) * S.ppb, wpx = n.dur * S.ppb;
    const edge = clamp(wpx / 3, 3, 8);
    return { note: n, edge: p.wx >= endX - edge };
  }
  return null;
}
function setCursor(cls) {
  for (const k of ["c-move", "c-moving", "c-resize", "c-copy"]) sc.classList.toggle(k, k === cls);
}

sc.addEventListener("pointerdown", (e) => {
  if (e.button !== 0) return;
  const p = evPos(e);
  if (!p.inView) return; // native scrollbar
  if (isTyping(document.activeElement)) document.activeElement.blur();
  sc.focus({ preventScroll: true });
  if (!els.lyricEdit.hidden) closeLyricEdit(true);
  const hit = hitNote(p);
  if (hit) {
    const n = hit.note;
    let justAdded = false;
    if (!S.sel.has(n._id)) {
      if (!e.shiftKey) S.sel.clear();
      S.sel.add(n._id);
      justAdded = true;
    }
    drag = { mode: hit.edge ? "resize" : "move", p0: p, p1: p, anchor: n, moved: false, dup: e.altKey && !hit.edge, shift: e.shiftKey, justAdded };
    setCursor(hit.edge ? "c-resize" : drag.dup ? "c-copy" : "c-moving");
  } else if (e.shiftKey || e.metaKey || e.ctrlKey) {
    drag = { mode: "marquee", p0: p, p1: p, moved: false, base: new Set(S.sel) };
  } else {
    drag = { mode: "create", p0: p, p1: p, moved: false };
  }
  try { sc.setPointerCapture(e.pointerId); } catch {}
  e.preventDefault();
  refreshInfo();
  dirty();
});

let previewTimer = 0;
function previewNote(pitch) {
  clearTimeout(previewTimer);
  if (S.status.playing) return;
  previewTimer = setTimeout(()=>B.previewNote?.(pitch),45);
}
function beginDrag(d) {
  if (d.mode === "move") {
    checkpoint();
    if (d.dup) {
      const copies = selectedNotes().map((n) => ({ ...n, _id: nextId++, _from: n._id }));
      S.song.notes.push(...copies);
      d.anchor = copies.find((c) => c._from === d.anchor._id) || copies[0];
      S.sel = new Set(copies.map((c) => c._id));
      copies.forEach((c) => delete c._from);
    }
    d.orig = new Map(selectedNotes().map((n) => [n._id, { start: n.start, pitch: n.pitch }]));
  } else if (d.mode === "resize") {
    checkpoint();
    d.orig = new Map(selectedNotes().map((n) => [n._id, { dur: n.dur }]));
    d.origEnd = d.anchor.start + d.anchor.dur;
  } else if (d.mode === "create") {
    checkpoint();
    S.sel.clear();
    const n = { pitch: pitchAt(d.p0.wy), start: Math.max(0, snapB(beatAt(d.p0.wx), "floor")), dur: minDur(), _id: nextId++ };
    S.song.notes.push(n);
    S.sel.add(n._id);
    d.anchor = n;
    previewNote(n.pitch);
  }
}

function updateDrag(d, p) {
  if (d.mode === "move") {
    const previousPitch = d.anchor.pitch;
    const o = d.orig.get(d.anchor._id);
    let dB = snapB(o.start + beatAt(p.wx - d.p0.wx)) - o.start;
    let dP = pitchAt(p.wy) - pitchAt(d.p0.wy);
    let minStart = Infinity, minP = 127, maxP = 0;
    for (const v of d.orig.values()) { minStart = Math.min(minStart, v.start); minP = Math.min(minP, v.pitch); maxP = Math.max(maxP, v.pitch); }
    dB = Math.max(dB, -minStart);
    dP = clamp(dP, -minP, 127 - maxP);
    for (const n of S.song.notes) {
      const v = d.orig.get(n._id);
      if (v) { n.start = v.start + dB; n.pitch = v.pitch + dP; }
    }
    if (d.anchor.pitch !== previousPitch) previewNote(d.anchor.pitch);
  } else if (d.mode === "resize") {
    const newEnd = Math.max(d.anchor.start + minDur(), snapB(d.origEnd + beatAt(p.wx - d.p0.wx)));
    const dD = newEnd - d.origEnd;
    for (const n of S.song.notes) {
      const v = d.orig.get(n._id);
      if (v) n.dur = Math.max(minDur(), v.dur + dD);
    }
  } else if (d.mode === "create") {
    const n = d.anchor;
    const end = Math.max(n.start + minDur(), snapB(beatAt(p.wx), "round"));
    n.dur = end - n.start;
  } else if (d.mode === "marquee") {
    const b0 = beatAt(Math.min(d.p0.wx, p.wx)), b1 = beatAt(Math.max(d.p0.wx, p.wx));
    const pHi = pitchAt(Math.min(d.p0.wy, p.wy)), pLo = pitchAt(Math.max(d.p0.wy, p.wy));
    S.sel = new Set(d.base);
    for (const n of S.song.notes) if (n.pitch >= pLo && n.pitch <= pHi && n.start < b1 && n.start + n.dur > b0) S.sel.add(n._id);
  }
}

sc.addEventListener("pointermove", (e) => {
  const p = evPos(e);
  if (!drag) {
    const hit = p.inView ? hitNote(p) : null;
    const id = hit ? hit.note._id : null;
    setCursor(hit ? (hit.edge ? "c-resize" : e.altKey ? "c-copy" : "c-move") : null);
    if (id !== S.hover) { S.hover = id; refreshInfo(); dirty(); }
    return;
  }
  const d = drag;
  d.p1 = p;
  if (!d.moved) {
    if (Math.hypot(p.x - d.p0.x, p.y - d.p0.y) < 3) return;
    d.moved = true;
    beginDrag(d);
  }
  // auto-scroll near the edges
  const m = 24;
  if (p.x > view.w - m) sc.scrollLeft += 12; else if (p.x < m && sc.scrollLeft > 0) sc.scrollLeft -= 12;
  if (p.y > view.h - m) sc.scrollTop += 8; else if (p.y < m) sc.scrollTop -= 8;
  updateDrag(d, evPos(e));
  refreshInfo();
  dirty();
});

function endDrag(cancel = false) {
  const d = drag;
  drag = null;
  if (!d) return;
  if (!d.moved) {
    if (d.mode === "move" || d.mode === "resize") {
      if (d.shift && !d.justAdded) S.sel.delete(d.anchor._id);
      else if (!d.shift) { S.sel.clear(); S.sel.add(d.anchor._id); }
    } else if (d.mode === "create") {
      S.sel.clear();
    }
  } else if (d.mode !== "marquee") {
    if (cancel) { restore(H.undo.pop()); return; }
    if (d.mode === "move") previewNote(d.anchor.pitch);
    if (d.mode === "create" || d.mode === "resize") S.lastDur = d.anchor.dur;
    resolveOverlaps(new Set(S.sel));
    sortNotes(S.song.notes);
    dropNoopCheckpoint();
    onSongChanged();
  }
  setCursor(null);
  refreshInfo();
  dirty();
  if (needPull) { needPull = false; pullSong(); }
}
sc.addEventListener("pointerup", () => endDrag());
sc.addEventListener("pointercancel", () => endDrag(true));
sc.addEventListener("pointerleave", () => { if (!drag && S.hover != null) { S.hover = null; refreshInfo(); dirty(); } });
sc.addEventListener("scroll", () => { updateWorld(); dirty(); if (!els.lyricEdit.hidden) closeLyricEdit(true); });

sc.addEventListener("dblclick", (e) => {
  const p = evPos(e);
  if (!p.inView) return;
  const hit = hitNote(p);
  if (hit) { openLyricEdit(hit.note); return; }
  checkpoint();
  const n = { pitch: pitchAt(p.wy), start: Math.max(0, snapB(beatAt(p.wx), "floor")), dur: S.lastDur, _id: nextId++ };
  S.song.notes.push(n);
  S.sel = new Set([n._id]);
  previewNote(n.pitch);
  resolveOverlaps(new Set([n._id]));
  onSongChanged();
});

// wheel: plain = scroll (native); cmd/ctrl = horizontal zoom; alt = row height
function onWheel(e, target) {
  const r = sc.getBoundingClientRect();
  if (e.ctrlKey || e.metaKey) {
    e.preventDefault();
    zoomTo(S.ppb * Math.exp(-e.deltaY * 0.0025), clamp(e.clientX - r.left, 0, view.w));
  } else if (e.altKey) {
    e.preventDefault();
    zoomRows(S.rowH * Math.exp(-e.deltaY * 0.002), clamp(e.clientY - r.top, 0, view.h));
  } else if (target === "keys") {
    e.preventDefault();
    sc.scrollTop += e.deltaY;
  } else if (target === "ruler") {
    e.preventDefault();
    sc.scrollLeft += e.deltaX || e.deltaY;
  }
}
sc.addEventListener("wheel", (e) => onWheel(e, "grid"), { passive: false });
els.keys.addEventListener("wheel", (e) => onWheel(e, "keys"), { passive: false });
els.ruler.addEventListener("wheel", (e) => onWheel(e, "ruler"), { passive: false });
// A click (or drag) on the ruler places the playhead on the grid: where Play starts and where Stop
// returns. Inside a DAW a click moves the DAW's playhead over MMC (locateHost).
els.ruler.addEventListener("pointerdown", (e) => {
  if (e.button !== 0 || e.ctrlKey) return;
  const beat = Math.max(0, snapB((e.offsetX + els.scroller.scrollLeft) / S.ppb));
  if (inDaw()) { requestLocate(beat); return; }
  els.ruler.setPointerCapture(e.pointerId);
  placePlayhead(e);
});
// ---- the DAW's transport over MMC (plugin/Source/HostMmc.h)
// No plugin API starts, stops or moves a DAW's transport. Novox sends MIDI Machine Control from its
// own MIDI port ("Novox The Robot"), which a DAW listening to MMC input follows: in Logic a setting of
// each project. The status that follows tells whether it listened, but only while the DAW calls the
// plugin (status.blocks advancing; Logic stops calling one on a track it isn't playing, leaving
// status.beats and status.playing as they were), and only from a real move:
// - a ruler click away from the playhead that lands there (or, playing, jumps there): MMC works;
// - one that lands elsewhere while stopped: MMC works, and the miss teaches the project's frame rate
//   (when the sent frame scaled by a standard rate explains it) or else where its bar 1 is, and the
//   locate goes again (twice at most);
// - a DAW calling the plugin that does not move, start or stop: MMC does not work; the hint names
//   the setting.
// What it learned is saved with the DAW project (setMmcState), as the setting is. Until MMC is known
// to work, Space reaches the DAW as a key press (hostSpace). GarageBand has no MMC input.
const MMC_HINT = "Logic: File › Project Settings › Synchronization › MIDI › Listen to MIDI Machine Control (MMC) input (each project; or save it in your template)";
const MMC_RATES = [24, 25, 29.97, 30];
const mmc = { works: false, offset: 0, rate: 0, loaded: false, pending: null, queued: null, start: null, stop: null,
  intent: null, shown: null, located: null, hinted: false, blocksAt: 0 };
const inDaw = () => !!(S.status.daw || S.status.host);
const noMmc = () => /garageband/i.test(S.status.hostName || "") || S.status.mmc?.port === false;
// Whether the DAW is calling the plugin now.
const hostLive = () => performance.now() - mmc.blocksAt < 400;
function mmcSave(works) {
  if (works === mmc.works && !mmc.dirty) return;
  mmc.works = works; mmc.dirty = false;
  B.setMmcState?.(mmc.works, mmc.offset, mmc.rate);
}
function mmcHint(state, line) { flash(state, line, 8000, MMC_HINT); }
// One locate at a time: a click while one is out waits, and only the latest waiting click goes.
function requestLocate(beat) {
  if (noMmc()) { flash("PLAYHEAD", "GarageBand's playhead can't be moved from a plugin"); return; }
  if (mmc.pending) { mmc.queued = beat; return; }
  locateHost(beat);
}
async function locateHost(beat, tries = 0) {
  const st = S.status;
  if (!Number.isFinite(+st.seconds)) { flash("PLAYHEAD", "This DAW does not tell plugins its time"); return; }
  const target = st.seconds + (beat - st.beats) * 60 / S.bpm;  // seconds from bar 1, as the DAW counts them
  const p = { beat, target, sent: target + mmc.offset, from: st.beats, fromSeconds: +st.seconds, blocks: st.blocks,
    playing: !!st.playing && hostLive(), live: hostLive(), at: performance.now(), tries, offset: mmc.offset, rate: mmc.rate };
  mmc.pending = p;
  const reply = await B.hostLocate?.(p.sent, mmc.rate);
  if (mmc.pending !== p) return;
  if (!reply?.sent) { mmc.pending = null; flash("PLAYHEAD", "No MIDI port to the DAW on this system"); return; }
  p.frame = +reply.frame || 0; p.fps = +reply.fps || 30;
  p.at = performance.now();
  if (Math.abs(beat - p.from) > 1) mmc.located = { beat, at: p.at };
  if (mmc.works && !p.playing) { mmc.shown = { beat, blocks: st.blocks }; S.status.beats = beat; dirty(); }
}
function settleLocate() {
  mmc.pending = null;
  const next = mmc.queued;
  mmc.queued = null;
  if (next !== null) locateHost(next);
}
function hostPlayStop() {
  if (!mmc.works || noMmc()) { B.hostSpace?.(); return; }
  const now = performance.now();
  // Toggle from what was asked a moment ago, not a status that has not caught up.
  const playing = mmc.intent && now - mmc.intent.at < 600 ? mmc.intent.playing : S.status.playing && hostLive();
  mmc.intent = { playing: !playing, at: now };
  const st = S.status;
  B.hostTransport?.(playing ? "stop" : "play").then((sent) => {
    if (!sent) { B.hostSpace?.(); return; }
    if (playing) { mmc.stop = { at: performance.now(), beats: st.beats }; mmc.start = null; }
    else { mmc.start = { at: performance.now(), blocks: st.blocks }; mmc.stop = null; }
  });
}
// MMC did not work: forget it for this project and say why; a Space press that went nowhere goes
// again as the key press.
function mmcFailed(state, line, { space = false } = {}) {
  mmcSave(false);
  if (space) B.hostSpace?.();
  mmcHint(state, line);
}
function checkMmc(st, prev) {
  const now = performance.now();
  if (st.blocks !== prev.blocks) mmc.blocksAt = now;
  if (!mmc.loaded && st.mmc) {
    mmc.loaded = true;
    mmc.works = !!st.mmc.works; mmc.offset = +st.mmc.offset || 0; mmc.rate = +st.mmc.rate || 0;
  }
  // A clicked position stays shown until the DAW reports again.
  if (mmc.shown) { if (st.blocks === mmc.shown.blocks) st.beats = mmc.shown.beat; else mmc.shown = null; }
  // Play and Stop show only while the DAW calls the plugin; fail only on that evidence.
  if (mmc.start) {
    if (st.playing && hostLive()) mmc.start = null;
    else if (now - mmc.start.at > 1500) {
      const failed = hostLive() && !st.playing && st.blocks - mmc.start.blocks >= 8;
      mmc.start = null;
      if (failed) mmcFailed("TRANSPORT", "The DAW did not start · in Logic turn on Listen to MMC Input", { space: true });
    }
  }
  if (mmc.stop) {
    if (!st.playing || !hostLive()) mmc.stop = null;
    else if (now - mmc.stop.at > 800 && st.beats - mmc.stop.beats > 0.25) {
      mmc.stop = null;
      mmcFailed("TRANSPORT", "The DAW did not stop · in Logic turn on Listen to MMC Input", { space: true });
    }
  }
  // A DAW that was not calling the plugin at the click starts where it was put.
  if (mmc.located && st.playing && !prev.playing) {
    if (!mmc.works && now - mmc.located.at < 10000 && Math.abs(st.beats - mmc.located.beat) < 0.25) mmcSave(true);
    mmc.located = null;
  }
  const p = mmc.pending;
  if (!p || p.frame === undefined) return;
  const age = now - p.at;
  if (st.blocks === p.blocks) {  // no word from the DAW since the click
    if (age > 1200) {
      settleLocate();
      if (!mmc.works && !mmc.hinted && p.tries === 0) { mmc.hinted = true; mmcHint("PLAYHEAD", "Did it move? If not, in Logic turn on Listen to MMC Input"); }
    }
    return;
  }
  const fps = p.fps || 30, tol = Math.max(1 / 32, 1.01 * (S.bpm / 60) / fps);
  if (p.playing || st.playing) {
    // Playing: a jump the playback alone does not explain, to about where it was put.
    const noMove = p.from + Math.min(age, 1500) / 1000 * S.bpm / 60;
    const jumped = Math.abs(st.beats - noMove) > 0.5, near = st.beats - p.beat > -tol && st.beats - p.beat < tol + Math.min(age, 1500) / 1000 * S.bpm / 60 + 0.25;
    if (jumped && near) { mmcSave(true); settleLocate(); }
    else if (age > 1200) { settleLocate(); if (p.tries === 0 && Math.abs(p.beat - p.from) > 1 && !jumped && hostLive()) mmcFailed("PLAYHEAD", "The DAW did not move · in Logic turn on Listen to MMC Input"); }
    return;
  }
  const moved = Math.abs(st.beats - p.from) > tol;
  if (Math.abs(st.beats - p.beat) < tol) { if (moved || p.tries) mmcSave(true); settleLocate(); return; }
  if (moved && Number.isFinite(+st.seconds)) {
    // Landed elsewhere: MMC works; learn why the landing missed, then go again.
    mmc.dirty = true;
    // The DAW read the sent frame at its own rate when that explains the miss; otherwise its bar 1
    // is elsewhere in timecode, and the miss becomes the offset added to every locate.
    const miss = st.seconds - p.target;  // seconds past the target, with p.offset already applied
    const implied = p.frame >= 4 ? p.frame / (p.frame / fps + miss) : 0;
    const rate = MMC_RATES.find((r) => Math.abs(implied - r) / r < 0.03 && Math.abs(r - fps) > 0.5);
    if (rate) mmc.rate = rate; else mmc.offset = p.offset - miss;
    mmcSave(true);
    mmc.pending = null;
    if (p.tries < 2) locateHost(p.beat, p.tries + 1); else settleLocate();
    return;
  }
  if (age > 1200) {
    if (p.tries > 0) { mmc.offset = p.offset; mmc.rate = p.rate; settleLocate(); return; }  // MMC is proven; the retry changed nothing
    settleLocate();
    if (Math.abs(p.beat - p.from) > tol && st.blocks - p.blocks >= 8 && hostLive())
      mmcFailed("PLAYHEAD", "The DAW did not move · in Logic turn on Listen to MMC Input");
  }
}
function placePlayhead(e) {
  const beat = Math.max(0, snapB((e.offsetX + els.scroller.scrollLeft) / S.ppb));
  B.setPlayhead?.(beat);
  S.status.beats = beat;  // drawn now; the next status confirms it
  dirty();
}

// piano keys: highlight a row (no sound)
els.keys.addEventListener("pointerdown", (e) => {
  const r = els.keys.getBoundingClientRect();
  const p = clamp(S.hi - Math.floor((e.clientY - r.top + sc.scrollTop) / S.rowH), S.lo, S.hi);
  S.hotPitch = S.hotPitch === p ? null : p;
  dirty();
});

// ---- lyric editor (double-click a note); one dictionary syllable per following note
let editingId = null;
let editingInitialValue = "";
let lyricEditPending = Promise.resolve(true);
function openLyricEdit(n) {
  editingId = n._id;
  const ed = els.lyricEdit;
  const l = S.labels.get(n._id);
  ed.hidden = false;
  ed.value = n.lyric || (l?.kind === "word" || l?.kind === "hold" ? l.text : "");
  editingInitialValue = ed.value.trim();
  const x = n.start * S.ppb - sc.scrollLeft, y = (S.hi - n.pitch) * S.rowH - sc.scrollTop;
  ed.style.left = clamp(x, 0, view.w - 90) + "px";
  ed.style.top = clamp(y - 4, 0, view.h - 22) + "px";
  ed.style.width = Math.max(90, n.dur * S.ppb) + "px";
  ed.placeholder = l && l.kind === "word" ? `lyric (now “${l.text}”)` : "word or syllables";
  ed.focus();
  ed.select();
}
function closeLyricEdit(commit) {
  const ed = els.lyricEdit;
  if (ed.hidden) return lyricEditPending;
  ed.hidden = true;
  const n = noteById(editingId);
  const originalText = editingInitialValue;
  editingId = null;
  if (!commit || !n) {
    lyricEditPending = lyricEditPending.then(() => true);
    return lyricEditPending;
  }
  const v = ed.value.trim();
  if (originalText === v) {
    lyricEditPending = lyricEditPending.then(() => true);
    return lyricEditPending;
  }
  const id = n._id;
  lyricEditPending = lyricEditPending.then(() => commitNoteLyric(id, v)).then(() => true).catch(error => {
    flash("CAN’T CHANGE LYRIC", String(error.message || error));
    const note = noteById(id);
    if (note && editingId === null) { openLyricEdit(note); els.lyricEdit.value = v; els.lyricEdit.select(); }
    return false;
  });
  return lyricEditPending;
}
async function planCandidate(song, bpm = S.bpm) {
  const text = await B.planSong(serialize(song), bpm);
  let plan;
  try { plan = JSON.parse(text); } catch {}
  if (!plan || !Array.isArray(plan.syllables)) throw new Error("The singer couldn’t assign these lyrics. Please try again.");
  return { plan, labels: labelNotes(sortNotes([...song.notes]), plan, bpm), bpm };
}
async function commitNoteLyric(id, text) {
  if (!noteById(id)) return;
  const before = notesKey(S.song), bpm = S.bpm;
  const labels = await currentLabels(true);
  if (!labels) throw new Error("The song changed while editing. Enter the lyric again.");
  const tokenResult = JSON.parse(await B.lyricTokens(text, JSON.stringify(S.song.dictionary || {})));
  if (!Array.isArray(tokenResult.tokens)) throw new Error(tokenResult.error || "Couldn’t split the word into syllables.");
  if (notesKey(S.song) !== before || S.bpm !== bpm) throw new Error("The song changed while editing. Enter the lyric again.");
  const edited = applyNoteLyrics({notes:S.song.notes, noteId:id, tokens:tokenResult.tokens, labels, useNoteLyrics:!!S.song.useNoteLyrics});
  if (edited.error) throw new Error(edited.error);
  const candidate = {...S.song, notes:edited.notes, useNoteLyrics:true};
  const preview = await planCandidate(candidate, bpm);
  if (notesKey(S.song) !== before || S.bpm !== bpm) throw new Error("The song changed while editing. Enter the lyric again.");
  candidate.lyrics = lyricsFromNotes(candidate.notes, {labels:preview.labels}) ?? "";
  checkpoint();
  S.song.notes = candidate.notes;
  S.song.useNoteLyrics = true;
  S.song.lyrics = candidate.lyrics;
  ++planSeq;
  S.plan = preview.plan; S.planBpm = bpm; S.planKey = notesKey(S.song); S.planFailed = false;
  onSongChanged({lyrics:true});
}
els.lyricEdit.addEventListener("keydown", (e) => {
  if (e.key === "Enter") { e.preventDefault(); closeLyricEdit(true); sc.focus({ preventScroll: true }); }
  else if (e.key === "Escape") { e.preventDefault(); closeLyricEdit(false); sc.focus({ preventScroll: true }); }
  e.stopPropagation();
});
els.lyricEdit.addEventListener("blur", () => closeLyricEdit(true));

// The OS clipboard carries melody between plugin instances, including separately hosted AUs.
let clipboardBusy = false;
const melodyShortcutAllowed = () => S.loaded && !isTyping(document.activeElement) && !document.querySelector("dialog[open]");
async function copyMelody() {
  if (clipboardBusy || !B) return;
  const selected = new Set(S.sel);
  clipboardBusy = true;
  try {
    if (!await closeLyricEdit(true)) return;
    for (const id of selected) if (!noteById(id)) throw new Error("The selection changed. Select the melody and copy again.");
    if (!selected.size) return flash("SELECT NOTES", "Select the melody to copy");
    const before = notesKey(S.song);
    const labels = await currentLabels(true);
    if (!labels || notesKey(S.song) !== before) throw new Error("The song changed. Copy the melody again.");
    const text = createMelodyClipboard(S.song, selected, {labels});
    await B.writeClipboard(text);
    flash("MELODY COPIED", `${selected.size} note${selected.size === 1 ? "" : "s"} · original timeline position included`);
  } catch (error) { flash("CAN’T COPY", String(error.message || error)); }
  finally { clipboardBusy = false; }
}
function confirmMelodyReplacement(count, pastedCount, start) {
  const dialog = $("pasteDialog");
  $("pasteMessage").textContent = `${pastedCount} pasted note${pastedCount === 1 ? "" : "s"} at ${fmtPos(start)} overlap${pastedCount === 1 ? "s" : ""} ${count} existing note${count === 1 ? "" : "s"}. Replace the overlapping parts?`;
  dialog.returnValue = "cancel";
  return new Promise(resolve => {
    dialog.addEventListener("close", () => resolve(dialog.returnValue === "replace"), {once:true});
    dialog.showModal();
  });
}
async function pasteMelody(clipboardText) {
  if (clipboardBusy || !B) return;
  clipboardBusy = true;
  try {
    if (!await closeLyricEdit(true)) return;
    const payload = parseMelodyClipboard(typeof clipboardText === "string" ? clipboardText : await B.readClipboard());
    const before = notesKey(S.song), bpm = S.bpm;
    const labels = await currentLabels(true);
    if (!labels || notesKey(S.song) !== before) throw new Error("The song changed. Paste the melody again.");
    const conflicts = melodyPasteConflicts(S.song.notes, payload);
    if (conflicts.count && !await confirmMelodyReplacement(conflicts.count, payload.notes.length, Math.min(...payload.notes.map(n => n.start)))) return;
    if (notesKey(S.song) !== before || S.bpm !== bpm) throw new Error("The song changed. Paste again to check the new overlaps.");
    const result = applyMelodyPaste(S.song, payload, {labels, overwrite:conflicts.count > 0});
    for (const n of result.notes) if (n._id == null) n._id = nextId++;
    const candidate = {...S.song, notes:result.notes, useNoteLyrics:true};
    const preview = await planCandidate(candidate, bpm);
    if (notesKey(S.song) !== before || S.bpm !== bpm) throw new Error("The song changed. Paste again to check the new overlaps.");
    candidate.lyrics = lyricsFromNotes(candidate.notes, {labels:preview.labels}) ?? "";
    checkpoint();
    S.song.notes = candidate.notes;
    S.song.useNoteLyrics = true;
    S.song.lyrics = candidate.lyrics;
    if (result.transcriptions) S.song.transcriptions = result.transcriptions;
    ++planSeq;
    S.plan = preview.plan; S.planBpm = bpm; S.planKey = notesKey(S.song); S.planFailed = false;
    S.sel = new Set(result.pastedNotes.map(n => n._id));
    onSongChanged({lyrics:true});
    showNotes(result.pastedNotes);
    flash("MELODY PASTED", `${result.pastedNotes.length} notes at their original position · ⌘Z undoes`);
  } catch (error) { flash("CAN’T PASTE", String(error.message || error)); }
  finally { clipboardBusy = false; }
}
$("copyMelody").addEventListener("click", () => copyMelody());
$("pasteMelody").addEventListener("click", () => pasteMelody());
// WKWebView's native Edit commands arrive as clipboard events. Text fields keep their own
// ordinary copy/paste; the keyboard handler below covers hosts that deliver key events instead.
document.addEventListener("copy", e => {
  if (!melodyShortcutAllowed()) return;
  e.preventDefault(); copyMelody();
});
document.addEventListener("paste", e => {
  if (!melodyShortcutAllowed()) return;
  e.preventDefault();
  pasteMelody(e.clipboardData ? e.clipboardData.getData("text/plain") : undefined);
});

// ---- keyboard shortcuts (ignored while typing)
document.addEventListener("keydown", (e) => {
  if (isTyping(e.target) || document.querySelector("dialog[open]")) return;
  const mod = e.metaKey || e.ctrlKey;
  const k = e.key;
  if (mod && (k === "c" || k === "C")) { e.preventDefault(); copyMelody(); return; }
  if (mod && (k === "v" || k === "V")) { e.preventDefault(); pasteMelody(); return; }
  if (mod && (k === "z" || k === "Z")) { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
  if (mod && (k === "y" || k === "Y")) { e.preventDefault(); redo(); return; }
  if (mod && (k === "a" || k === "A")) { e.preventDefault(); S.sel = new Set(S.song.notes.map((n) => n._id)); refreshInfo(); dirty(); return; }
  if (mod && (k === "d" || k === "D")) { e.preventDefault(); duplicateSelection(); return; }
  if (k === "Escape") { S.sel.clear(); S.hotPitch = null; refreshInfo(); dirty(); return; }
  // Space plays and stops: our transport, or in a DAW its transport (over MMC once the DAW is known
  // to listen, else as the key press the native side hands it).
  if (k === " " && !mod) { e.preventDefault(); if (!inDaw()) togglePlay(); else if (!e.repeat) hostPlayStop(); return; }
  if ((k === "Delete" || k === "Backspace") && S.sel.size) {
    e.preventDefault();
    checkpoint();
    S.song.notes = S.song.notes.filter((n) => !S.sel.has(n._id));
    S.sel.clear();
    S.hover = null;
    onSongChanged();
    return;
  }
  if (k.startsWith("Arrow") && S.sel.size && !mod) {
    e.preventDefault();
    const sel = selectedNotes();
    checkpoint("nudge");
    if (k === "ArrowUp" || k === "ArrowDown") {
      const d = (k === "ArrowUp" ? 1 : -1) * (e.shiftKey ? 12 : 1);
      if (sel.every((n) => n.pitch + d >= 0 && n.pitch + d <= 127)) sel.forEach((n) => (n.pitch += d));
    } else {
      const d = (k === "ArrowRight" ? 1 : -1) * nudgeStep();
      if (e.shiftKey) sel.forEach((n) => (n.dur = Math.max(minDur(), n.dur + d))); // shift: change length
      else if (sel.every((n) => n.start + d >= -1e-9)) sel.forEach((n) => (n.start = Math.max(0, n.start + d)));
    }
    resolveOverlaps(new Set(S.sel));
    sortNotes(S.song.notes);
    dropNoopCheckpoint();
    onSongChanged();
    const a = sel[0];
    if (a && !e.shiftKey) previewNote(a.pitch);
    if (a) { // keep it in view
      const y = (S.hi - a.pitch) * S.rowH;
      if (y < sc.scrollTop || y > sc.scrollTop + view.h - S.rowH) sc.scrollTop = y - view.h / 2;
    }
  }
});
function duplicateSelection() {
  const sel = selectedNotes();
  if (!sel.length) return;
  checkpoint();
  const start = Math.min(...sel.map((n) => n.start)), end = Math.max(...sel.map((n) => n.start + n.dur));
  const span = Math.ceil((end - start) / (S.snap || 0.25) - 1e-9) * (S.snap || 0.25);
  const copies = sel.map((n) => ({ ...n, start: n.start + span, _id: nextId++ }));
  S.song.notes.push(...copies);
  S.sel = new Set(copies.map((c) => c._id));
  resolveOverlaps(new Set(S.sel));
  onSongChanged();
}

// ===========================================================================
// 7. toolbar, lyrics, voice panel, import

// ---- info strip + counters
function refreshInfo() {
  $("copyMelody").disabled = !S.sel.size;
  const el = els.info;
  let id = drag && drag.anchor ? drag.anchor._id : S.hover;
  if (id == null && S.sel.size === 1) id = [...S.sel][0];
  const n = id != null ? noteById(id) : null;
  if (!n) {
    if (S.sel.size > 1) { el.innerHTML = `<b>${S.sel.size} notes</b> selected · arrows nudge · ⌫ deletes · ⌘D duplicates`; return; }
    el.textContent = S.song.notes.length
      ? "Drag to draw · shift-drag selects · double-click a note to enter lyrics"
      : "Draw some notes, import a file, or arm the keyboard icon and play your keyboard.";
    return;
  }
  const l = S.labels.get(n._id);
  const what = !l ? "…" : { word: `“${l.text}”`, cont: "rest of the word", hold: `holding “${l.syl?.text || l.vowel || "_"}”`, hum: "hummed" }[l.kind];
  el.innerHTML = `<b>${pitchName(n.pitch)}</b> · ${fmtPos(n.start)} · ${fmtLen(n.dur)} · ${escapeHtml(what)}` +
    (l && l.phon ? ` <span class="ph">${escapeHtml(l.phon)}</span>` : "") +
    (n.lyric ? ` <span class="own">· own lyric “${escapeHtml(n.lyric)}”</span>` : "");
}
function refreshTexts() {
  const n = S.song.notes.length;
  const labs = [...S.labels.values()].filter(Boolean);
  const sung = labs.filter((l) => l.kind === "word" || l.kind === "cont").length;
  const hum = labs.filter((l) => l.kind === "hum").length;
  const held = labs.filter((l) => l.kind === "hold").length;
  const pl = (k, w) => `${k} ${w}${k === 1 ? "" : "s"}`;
  let txt = S.plan ? `${pl(sung, "syllable")} · ${pl(n, "note")}` : pl(n, "note");
  if (S.plan && held) txt += ` · ${held} held`;
  if (S.plan && hum) txt += ` · ${hum} hummed`;
  els.counts.textContent = txt;
  const warnings = [...(S.plan?.warnings || [])];
  if (S.planFailed) warnings.unshift("The singer couldn't read these lyrics");
  els.counts.classList.toggle("off", !!(hum && S.song.lyrics.trim()) || warnings.length > 0);
  els.warn.hidden = !warnings.length;
  els.warn.textContent = warnings.length > 1 ? `${warnings[0]} (+${warnings.length - 1} more)` : warnings[0] || "";
  els.warn.title = warnings.join("\n");
  refreshInfo();
  refreshLcd();
}

// ---- LCD
// detail: the LCD tooltip while the message shows (e.g. every warning of an import).
function flash(state, line = "", ms = 2200, detail = "") { S.flash = { state, line, detail, until: performance.now() + ms }; refreshLcd(); }
// ---- is the audio heard up to date with the edits?
// status.renderedVersion is the song version the audio being played was rendered from. A render
// follows an edit within about a second; once the audio has been behind for STALE_MS the LCD says
// so, with what tells why in its tooltip (an older version once played over Logic, 2026-10-18), and
// the READY tooltip keeps a tally of such stretches for the session.
const STALE_MS = 3000;
const stale = { since: 0, count: 0, longest: 0, last: "" };
function trackStale(st) {
  const behind = Number.isInteger(st.renderedVersion) && st.renderedVersion >= 0 && st.renderedVersion !== st.songVersion;
  const now = performance.now();
  if (behind && !stale.since) stale.since = now;
  if (!behind && stale.since) {
    const ms = now - stale.since;
    if (ms > STALE_MS) { stale.count++; stale.longest = Math.max(stale.longest, ms); stale.last = new Date().toLocaleTimeString(); }
    stale.since = 0;
  }
}
const staleMs = () => (stale.since ? performance.now() - stale.since : 0);
function staleDetail(st) {
  const r = st.renders || {}, done = +r.secondsSinceDone;
  return [`Playing the audio of edit ${st.renderedVersion}; the song is at edit ${st.songVersion}.`,
    `Renders: ${r.started ?? "?"} started, ${r.cancelled ?? "?"} given up for a newer edit; the last finished ${done >= 0 ? Math.round(done) + " s ago" : "never"}.`,
    (st.playing ? "The DAW is playing." : "Stopped.") + (st.reviewing ? " A Compare review is playing." : "")].join("\n");
}
let lcdCache = "";
function refreshLcd() {
  const st = S.status;
  const vname = voiceMeta(voiceParts().preset).n;
  let state, line, cls = "";
  const flashing = S.flash && performance.now() < S.flash.until;
  let title = (flashing && S.flash.detail) || "";
  if (flashing) ({ state, line } = S.flash);
  else if (st.error) { state = "ERROR"; line = st.error; cls = "err"; }
  else if (staleMs() > STALE_MS) {
    state = "OUT OF DATE"; line = `you're hearing an earlier edit · ${Math.round(staleMs() / 1000)} s`; cls = "err"; title = staleDetail(st);
  }
  else if (st.reviewing && st.playing && inDaw()) { state = "REVIEW ♪"; line = "Compare is playing, not the song"; }
  else if (!S.loaded) { state = "LOADING…"; line = "warming up the vocal cords"; }
  else if (st.rendering) { state = "RENDERING…"; line = "the singer is taking a breath"; }
  else if (st.playing) {
    state = st.capture ? "● CAPTURING" : "PLAYING ♪";
    const n = noteAtBeat(st.beats), l = n && S.labels.get(n._id);
    line = l ? (l.kind === "hum" ? "♪ mmm…" : l.kind === "hold" ? `♪ ${l.syl?.text || ""} ~` : `♪ ${l.text}`) : "♪";
    if (st.capture) cls = "rec";
  } else if (st.capture) { state = "CAPTURE ARMED"; line = st.host ? "play the DAW and your keyboard" : "press Play and your keyboard"; cls = "rec"; }
  // Standalone shows the tempo as its own field at the end of this line.
  else {
    state = "READY"; line = `${vname} · ${st.host ? Math.round(S.bpm) + " bpm · " : ""}${S.song.notes.length} notes`;
    if (!title && stale.count) title = `The audio fell behind the edits ${stale.count}× this session (longest ${Math.round(stale.longest / 1000)} s, last at ${stale.last}).`;
  }
  if (els.lcd.title !== title) els.lcd.title = title;
  const pos = fmtPos(st.beats);
  const key = state + "|" + line + "|" + pos + "|" + cls;
  if (key === lcdCache) return;
  lcdCache = key;
  els.lcdState.textContent = state;
  els.lcdLine.textContent = line;
  els.lcdPos.textContent = pos;
  els.lcd.className = "lcd" + (cls ? " " + cls : "");
}

// ---- transport + capture
function togglePlay() { phraseMode=null;B.stopAudioReview?.();B.setTransport(!S.status.playing); }
els.play.addEventListener("click", togglePlay);
els.capture.addEventListener("click", () => {
  const on = !S.status.capture;
  S.status.capture = on; // optimistic
  refreshButtons();
  B.setCapture(on);
});
els.tempo.addEventListener("change", () => {
  const v = clamp(Math.round(+els.tempo.value || 120), 30, 300);
  els.tempo.value = v;
  B.setTempo(v);
});
els.tempo.addEventListener("keydown", (e) => { if (e.key === "Enter") els.tempo.blur(); });
function refreshButtons() {
  const st = S.status;
  els.capture.classList.toggle("on", !!st.capture);
  els.capture.classList.toggle("live", !!(st.capture && st.playing));
  els.capture.setAttribute("aria-pressed", !!st.capture);
  const help = $("captureHelp");
  help.hidden = !st.capture;
  help.textContent = st.host ? "Start playback in Logic, then play your MIDI keyboard to add notes." : "Press Play, then play your MIDI keyboard to add notes.";
  els.play.classList.toggle("on", !!st.playing);
  els.playIco.innerHTML = st.playing ? "&#9632;" : "&#9654;";
  els.playLbl.textContent = st.playing ? "Stop" : "Play";
  els.app.classList.toggle("host", !!st.host);
  els.mode.textContent = (st.host ? "In host · the DAW drives tempo and transport" : "Standalone") + (B && B.isMock ? " · mock backend" : "");
  const version = $("version"), v = st.version ? `Novox The Robot ${st.version} · free and open source · songaday.studio/plugins` : "";
  if (version && version.textContent !== v) version.textContent = v;
}

// ---- snap + zoom
els.snap.addEventListener("change", () => { S.snap = +els.snap.value; dirty(); els.snap.blur(); });
$("zoomIn").addEventListener("click", () => zoomTo(S.ppb * 1.4));
$("zoomOut").addEventListener("click", () => zoomTo(S.ppb / 1.4));
$("zoomFit").addEventListener("click", zoomFit);

// ---- clear
$("clear").addEventListener("click", () => {
  if (!S.song.notes.length) return flash("NOTHING TO CLEAR");
  checkpoint();
  S.song.notes = [];
  S.song.useNoteLyrics = false;
  S.song.transcriptions = [];
  S.sel.clear();
  onSongChanged();
  flash("CLEARED", "notes removed · ⌘Z brings them back");
});

// ---- lyrics
els.lyrics.addEventListener("input", () => {
  checkpoint("lyrics");
  S.song.lyrics = els.lyrics.value;
  S.song.useNoteLyrics = false;
  // Editing the lyric line switches back to flowing lyrics across notes.
  for (const n of S.song.notes) delete n.lyric;
  onSongChanged();
});

// ---- voice panel
function voiceParts() {
  const v = S.song.voice;
  if (!v || typeof v !== "object") return { preset: typeof v === "string" && v ? v : "std", ov: {} };
  const { preset, ...ov } = v;
  return { preset: preset || "std", ov };
}
function setVoice(preset, ov) {
  S.song.voice = Object.keys(ov).length ? { preset, ...ov } : preset;
  onSongChanged();
}
function buildGallery() {
  const g = $("gallery");
  g.innerHTML = "";
  for (const name of S.voices) {
    const v = voiceMeta(name);
    const b = document.createElement("button");
    b.className = "card";
    b.setAttribute("role", "radio");
    b.dataset.voice = name;
    b.setAttribute("aria-label", `${v.n}${v.d ? ", " + v.d : ""}`);
    // Who's who on hover: the native tooltip, plus an in-page tag for hosts whose web view shows
    // title tooltips late or not at all (keyboard focus shows the tag too).
    b.title = `${v.n}${v.d ? " · " + v.d : ""}`;
    b.innerHTML = voiceSvg(v, 44) + `<span class="nm" aria-hidden="true">${escapeHtml(v.n)}</span>` +
      `<span class="tip" aria-hidden="true"><b>${escapeHtml(v.n)}</b>${v.d ? ` · <i>${escapeHtml(v.d)}</i>` : ""}</span>`;
    b.addEventListener("click", () => {
      if (voiceParts().preset === name && !Object.keys(voiceParts().ov).length) return;
      checkpoint();
      setVoice(name, {});
    });
    b.addEventListener("pointerenter", () => placeTip(b));
    b.addEventListener("focus", () => placeTip(b));
    g.appendChild(b);
  }
  syncGalleryChecks();
}
// The cast is one radio group: Tab lands on the chosen singer, arrows/Home/End choose another.
function syncGalleryChecks() {
  const preset = voiceParts().preset;
  const cards = [...$("gallery").children];
  const chosen = cards.find((c) => c.dataset.voice === preset);
  for (const c of cards) {
    c.setAttribute("aria-checked", c === chosen);
    c.tabIndex = c === (chosen || cards[0]) ? 0 : -1;
  }
}
$("gallery").addEventListener("keydown", (e) => {
  const cards = [...$("gallery").children], i = cards.indexOf(document.activeElement);
  if (i < 0 || e.metaKey || e.ctrlKey || e.altKey) return;
  const step = { ArrowLeft: -1, ArrowUp: -1, ArrowRight: 1, ArrowDown: 1 }[e.key];
  let to = -1;
  if (step) to = (i + step + cards.length) % cards.length;
  else if (e.key === "Home") to = 0;
  else if (e.key === "End") to = cards.length - 1;
  else if (e.key === "Enter") to = i;
  else return;
  // Handled here, so the roll's arrow-key nudges leave the selection alone. Space stays Play/Stop,
  // also after a click on a portrait (which focuses it).
  e.preventDefault();
  e.stopPropagation();
  cards[to].focus();
  cards[to].click();
});
// Centre the tag under its portrait, but keep it inside the window.
function placeTip(card) {
  const tip = card.querySelector(".tip");
  if (!tip) return;
  tip.style.left = tip.style.transform = "";
  tip.style.display = "block";
  const box = card.getBoundingClientRect(), width = tip.offsetWidth, room = document.documentElement.clientWidth;
  tip.style.display = "";
  const left = Math.max(6, Math.min(room - 6 - width, box.left + box.width / 2 - width / 2));
  tip.style.left = `${Math.round(left - box.left)}px`;
  tip.style.transform = "none";
}
const sliderEls = {};
function buildSliders() {
  const host = $("sliders");
  host.innerHTML = "";
  for (const s of SLIDERS) {
    const row = document.createElement("div");
    row.className = "sl";
    row.title = (s.help ? s.help + " " : "") + "Double-click to return to the preset value";
    row.innerHTML = `<label for="sl-${s.key}">${s.label}</label><input type="range" id="sl-${s.key}" min="${s.min}" max="${s.max}" step="${s.step}"><output></output>`;
    const input = row.querySelector("input");
    input.addEventListener("input", () => {
      const { preset, ov } = voiceParts();
      checkpoint("slider-" + s.key);
      ov[s.key] = s.key === "choirSize" ? Math.round(+input.value) : +input.value;
      setVoice(preset, ov);
    });
    input.addEventListener("dblclick", () => {
      const { preset, ov } = voiceParts();
      if (!(s.key in ov)) return;
      checkpoint();
      delete ov[s.key];
      setVoice(preset, ov);
    });
    sliderEls[s.key] = { row, input, out: row.querySelector("output"), s };
    host.appendChild(row);
  }
}
let facePreset = null;
function syncVoiceUI() {
  const { preset, ov } = voiceParts();
  const v = voiceMeta(preset);
  if (facePreset !== preset) {
    facePreset = preset;
    $("bigface").innerHTML = voiceSvg(v, 64);
    $("vname").textContent = v.n;
    $("vsub").textContent = v.d;
    mouthNow = "?";
    setMouth(null);
    if (!S.voices.includes(preset)) { S.voices.push(preset); buildGallery(); }
  }
  syncGalleryChecks();
  const pv = presetValues(preset);
  for (const k in sliderEls) {
    const { row, input, out, s } = sliderEls[k];
    const val = k in ov ? +ov[k] : pv[k];
    if (document.activeElement !== input || +input.value !== val) input.value = val;
    out.textContent = s.fmt(val);
    row.classList.toggle("touched", k in ov);
  }
  const n = Object.keys(ov).length;
  $("modified").textContent = n ? `${n} change${n > 1 ? "s" : ""} on ${v.n}` : "Preset settings";
  $("reset").disabled = !n;
  const tb = $("tweakBtn");
  tb.classList.toggle("mod", !!n);
  tb.textContent = n ? `Tweaked (${n})` : "Tweak";
}
$("reset").addEventListener("click", () => {
  const { preset, ov } = voiceParts();
  if (!Object.keys(ov).length) return;
  checkpoint();
  setVoice(preset, {});
});

// ---- import (file picker or drag-and-drop)
function toBase64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}
const audioImport = createAudioImport({
  playAudioReview: async (...a) => B.playAudioReview ? B.playAudioReview(...a) : "Native audio output is unavailable",
  stopAudioReview: () => B.stopAudioReview?.(),
  audioImportStatus: (...a) => B.audioImportStatus(...a),
  startAudioImport: (...a) => B.startAudioImport(...a),
  cancelAudioImport: (...a) => B.cancelAudioImport(...a),
  previewAudioImport: (...a) => B.previewAudioImport(...a),
}, acceptRecording, flash);
// An audio import (a file, a region dragged from the DAW, a singing take) arrives as notes in
// seconds. With Add to song on, it joins the song at its place on the timeline (a recording with
// no position goes to the playhead) and replaces only the notes inside its recording's span
// (mergeImport in song-merge.js). Off, or over the starter song, it replaces the whole song at its
// position (beat 0 without one). Either way it is one Undo step, and its recording is kept.
// status: the transport when the recording arrived (importStatus), not after converting it.
async function acceptRecording(score, name, result, status = null) {
  const add = addToSong.checked;
  const offset = importPosition(result, status || S.status, S.bpm, add);
  // Seconds become beats at the current host tempo; never change Logic's tempo.
  const factor = S.bpm / 60;
  const beats = r6(Math.max(+result.duration || 0, ...score.notes.map(n => n.start + n.dur)) * factor);
  const recordingId = String(nextId++)+"-"+Date.now();
  const source={original:result.original,waveform:result.waveform || [],seconds:result.duration || 0,bpm:S.bpm,startBeat:offset};
  if(B.storeSource && !await B.storeSource(recordingId,source)) throw new Error("Could not retain the source recording");
  sourceRecordings.set(recordingId,source);
  const merge = add && S.song.notes.length > 0 && !isStarterSong(S.song, B.starterSong);
  // Flowing lyrics are pinned to the song's notes before the merge, which needs a plan of exactly
  // these notes; notes that carry their own lyrics only use the plan for "~" marks.
  const labels = merge ? await currentLabels(!S.song.useNoteLyrics) : null;
  checkpoint();
  const notes = score.notes.map(n => ({ pitch: clamp(Math.round(n.pitch), 0, 127), start: r6(offset+n.start*factor),
    dur: r6(n.dur*factor), lyric: n.lyric || "", _id: nextId++ }));
  // The import's evidence (whisper's words in transcriptWords, the notes as detected for key
  // correction) stays with its record through save and Undo.
  const record = {...score.transcription, recordingId, name, bpm: S.bpm,
    originalNotes: score.notes.map(n => ({...n, start: r6(offset+n.start*factor), dur: r6(n.dur*factor)})), originalNotesUnits: "beats"};
  const merged = mergeImport(merge ? S.song : {notes: [], transcriptions: []}, {notes, start: offset, beats, record},
    {labels, spanOf: spanOfRecord});
  // The notes' own syllables sing; the lyric box shows them all as text that flows back onto the
  // same notes if edited (refined with "~" marks once the plan arrives).
  S.song.notes = merged.notes;
  S.song.transcriptions = merged.transcriptions;
  S.song.useNoteLyrics = true;
  S.song.lyrics = merged.lyrics;
  S.sel = merge ? new Set(notes.map(n => n._id)) : new Set();
  const shaped = shapeImport(notes, score.notes, offset + beats);
  onSongChanged({lyrics:true});
  if (merge) showNotes(notes); else { zoomFit(); scrollToNotes(); }
  // A warning stays up long enough to read; the tooltip lists every one.
  const warnings = score.transcribeWarnings || [], summary = importSummary(score, name, {added: merge, at: fmtPos(offset)});
  const detail = [...(merge && merged.removed ? [`Replaced ${merged.removed} earlier note${merged.removed === 1 ? "" : "s"} in its span · ⌘Z undoes`] : []), ...warnings];
  const line = [summary.line, shaped.text].filter(Boolean).join(" · ");
  flash(summary.state, line, warnings.length ? 6000 : 2600, [shaped.detail, ...detail].filter(Boolean).join("\n"));
}
const keyName = (root, scale) => `${[...$("keyRoot").options].find(o => +o.value === root)?.text || "?"} ${scale}`;
// A new import's notes onto the grid (Snap to grid, the editor's Snap setting) and into the key
// (Snap to key), as an Undo step of its own after the import's, so one Undo returns the notes as
// sung. The key is detected from the whole song on every import and fills in Key, unless a key
// was chosen there by hand. imported[i] was detected as detected[i] (its detectedPitch breaks a
// tie between the two scale notes around it). Returns {text, detail} for the import's message.
function shapeImport(imported, detected, end) {
  const key = detectKey(S.song.notes), kc = S.song.keyCorrection;
  const chosen = kc && Number(kc.root) >= 0 && !kc.auto;
  if (key && !chosen) S.song.keyCorrection = {...(kc || {}), root: key.root, scale: key.scale, mode: kc?.mode || "gentle", auto: true};
  const settings = S.song.keyCorrection, keyed = keyImport.checked && settings && Number(settings.root) >= 0;
  const grid = snapImport.checked ? S.snap : 0;
  const ids = new Set(imported.map(n => n._id)), source = new Map(imported.map((n, i) => [n._id, detected[i]]));
  const mine = S.song.notes.filter(n => ids.has(n._id));
  let pitched = 0;
  const shaped = mine.map(n => {
    const pitch = keyed ? correctedPitch(n, {...settings, mode: "snap"}, source.get(n._id)) : n.pitch;
    if (pitch !== n.pitch) pitched++;
    return {...n, pitch};
  });
  const timed = grid > 0 ? quantizeNotes(shaped, grid, {end}) : shaped;
  const before = new Map(mine.map(n => [n._id, n]));
  const moved = timed.filter(t => Math.abs(t.start - before.get(t._id).start) > 1e-6 || Math.abs(t.dur - before.get(t._id).dur) > 1e-6).length;
  const named = key && !chosen ? `Key ${keyName(key.root, key.scale)}` : "";
  if (!pitched && !moved) return {text: named, detail: ""};
  checkpoint();  // the import as sung
  const byId = new Map(timed.map(t => [t._id, t]));
  for (const n of S.song.notes) { const t = byId.get(n._id); if (t) Object.assign(n, {start: t.start, dur: t.dur, pitch: t.pitch}); }
  sortNotes(S.song.notes);
  resolveOverlaps(ids);
  const done = [moved ? `on the ${els.snap.selectedOptions[0]?.text || ""} grid` : "", pitched ? `${pitched} into ${keyName(settings.root, settings.scale)}` : ""];
  return {text: [named, ...done].filter(Boolean).join(" · "), detail: "⌘Z returns the notes as sung"};
}
// Labels of the song's current notes, from a plan of exactly these notes: waits for one when
// `wait` previews this exact song, otherwise null when the display plan is stale or missing.
async function currentLabels(wait) {
  const fresh = () => S.plan && S.planKey === notesKey(S.song);
  if (!wait) return fresh() ? labelNotes(sortNotes([...S.song.notes]), S.plan, S.planBpm) : null;
  const before = notesKey(S.song), bpm = S.bpm;
  const preview = await planCandidate(S.song, bpm);
  return notesKey(S.song) === before && S.bpm === bpm ? preview.labels : null;
}
// Scroll (and zoom out if needed) so these notes are in view, centred on their pitches.
function showNotes(notes) {
  if (!notes.length) return;
  const first = Math.min(...notes.map(n => n.start)), last = Math.max(...notes.map(n => n.start + n.dur));
  const room = view.w - 60;
  if ((last - first) * S.ppb > room) S.ppb = clamp(room / (last - first + 0.5), 6, 400);
  updateWorld();
  if (first * S.ppb < sc.scrollLeft || last * S.ppb > sc.scrollLeft + view.w) sc.scrollLeft = Math.max(0, first * S.ppb - 40);
  const ps = notes.map(n => n.pitch), mid = (Math.min(...ps) + Math.max(...ps)) / 2;
  sc.scrollTop = Math.max(0, (S.hi - mid) * S.rowH - view.h / 2);
  dirty();
}
// Add to song (on: imports join the song; off: they replace it) and Detailed notes (off: one note
// per sung syllable) apply to the next audio import or singing take. They are preferences of this
// computer, not of the song, so they stay out of the DAW state.
const addToSong = $("addToSong"), detailedNotes = $("detailedNotes");
try { const stored = localStorage.getItem("novox.addToSong"); if (stored !== null) addToSong.checked = stored === "1"; } catch {}
try { detailedNotes.checked = localStorage.getItem("novox.detailedNotes") === "1"; } catch {}
addToSong.addEventListener("change", () => {
  try { localStorage.setItem("novox.addToSong", addToSong.checked ? "1" : "0"); } catch {}
});
detailedNotes.addEventListener("change", () => {
  try { localStorage.setItem("novox.detailedNotes", detailedNotes.checked ? "1" : "0"); } catch {}
});
// Snap to grid (top bar, on) and Snap imports to this key (Key menu, off) shape the next import's
// notes (shapeImport); like the other import options they belong to this computer, not the song.
const snapImport = $("snapImport"), keyImport = $("keyImport");
for (const [box, name] of [[snapImport, "novox.snapImport"], [keyImport, "novox.keyImport"]]) {
  try { const stored = localStorage.getItem(name); if (stored !== null) box.checked = stored === "1"; } catch {}
  box.addEventListener("change", () => { try { localStorage.setItem(name, box.checked ? "1" : "0"); } catch {} });
}
// The transport a recording without a timeline position is placed by: now, or for a singing take
// where the playhead was when singing started (with the transport running, the take lines up with
// what was playing). Taken when the recording arrives, as converting it takes seconds.
let takeStatus = null; // set while a singing take records or is being handed over
function importStatus(take = false) {
  const status = (take && takeStatus) || { ...S.status };
  takeStatus = null;
  return status;
}
async function importFileObj(file) {
  if (!file) return;
  if (micState.recording || micState.pending) {flash("RECORDING", "Stop singing recording before importing a file");return;}
  if (/\.(wav|mp3|aif|aiff|m4a|flac|ogg)$/i.test(file.name)) return audioImport.open(file, S.song.voice, detailedNotes.checked, importStatus());
  flash("IMPORTING…", file.name);
  let txt = "";
  try { txt = await B.importFile(toBase64(new Uint8Array(await file.arrayBuffer())), S.song.lyrics); } catch (e) { console.warn(e); }
  let score = null;
  try { score = txt ? JSON.parse(txt) : null; } catch {}
  if (!score || !Array.isArray(score.notes) || !score.notes.length) { flash("CAN'T IMPORT", `couldn't read notes from ${file.name}`); return; }
  const tempo = +score.tempo || S.bpm;
  const k = score.units === "beats" ? 1 : tempo / 60;
  const notes = score.notes.map((n) => ({ pitch: parsePitch(n.pitch), start: (+n.start || 0) * k, dur: (+n.dur || 0) * k, lyric: n.lyric }))
    .filter((n) => Number.isFinite(n.pitch) && n.dur > 0);
  const lyr = typeof score.lyrics === "string" && score.lyrics.trim() ? score.lyrics : lyricsFromNotes(notes, { emptyNote: "hold" });
  checkpoint();
  S.song.notes = notes.map((n) => ({ pitch: clamp(n.pitch, 0, 127), start: Math.max(0, n.start), dur: n.dur, _id: nextId++ }));
  S.song.useNoteLyrics = false;
  S.song.transcriptions = [];
  if (lyr) S.song.lyrics = lyr;
  S.sel.clear();
  resolveOverlaps();
  onSongChanged({ lyrics: true });
  if (!S.status.host && score.tempo) B.setTempo(Math.round(tempo));
  zoomFit();
  scrollToNotes();
  flash("IMPORTED ✓", `${S.song.notes.length} notes from ${file.name}`);
}
els.file.addEventListener("change", () => { importFileObj(els.file.files[0]); els.file.value = ""; });
$("importBtn").addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); els.file.click(); } });
let dragDepth = 0;
const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes("Files");
window.addEventListener("dragenter", (e) => { if (!hasFiles(e)) return; e.preventDefault(); dragDepth++; els.app.classList.add("dragging"); });
window.addEventListener("dragover", (e) => { if (hasFiles(e)) e.preventDefault(); });
window.addEventListener("dragleave", () => { if (--dragDepth <= 0) { dragDepth = 0; els.app.classList.remove("dragging"); } });
window.addEventListener("drop", (e) => {
  e.preventDefault();
  dragDepth = 0;
  els.app.classList.remove("dragging");
  const f = e.dataTransfer?.files?.[0];
  if (f) importFileObj(f);
  else if(B.readNativeDrop) B.readNativeDrop();
  else flash("CAN’T IMPORT", "This region did not provide an audio or MIDI file");
});

// ===========================================================================
// 8. status + boot
function onStatus(s) {
  if (s.microphone) updateMicrophone(s.microphone);
  const prev = S.status;
  const st = { ...prev, ...s };
  // The transport starting ends a review (the plugin stops it), so a looping one does not come back.
  if (st.playing && !prev.playing) phraseMode = null;
  if(prev.rendering && !st.rendering && phraseMode==="robot" && $("phraseLoop").checked) setTimeout(()=>hearPhrase(false),0);
  if (st.beats == null || !Number.isFinite(+st.beats)) st.beats = 0;
  S.status = st;
  if (st.daw || st.host) checkMmc(st, prev);
  trackStale(st);
  if (+st.bpm > 0 && Math.abs(st.bpm - S.bpm) > 1e-3) {
    S.bpm = +st.bpm;
    relabel(); // keep labels roughly right until the new plan arrives
    clearTimeout(planTimer);
    planTimer = setTimeout(fetchPlan, 250);
  }
  if (document.activeElement !== els.tempo) {
    const t = String(Math.round(S.bpm));
    if (els.tempo.value !== t) els.tempo.value = t;
  }
  if (prev.capture !== st.capture || prev.playing !== st.playing || prev.host !== st.host) refreshButtons();
  if (S.loaded && st.songVersion !== S.seenVersion) { S.seenVersion = st.songVersion; pullSong(); }
  if (st.playing || prev.playing || st.beats !== prev.beats) {
    // follow the playhead
    if (st.playing && !drag) {
      const x = st.beats * S.ppb;
      if (x > sc.scrollLeft + view.w - 30 || x < sc.scrollLeft) {
        updateWorld();
        sc.scrollLeft = Math.max(0, x - view.w * 0.12);
      }
    }
    const n = st.playing ? noteAtBeat(st.beats) : null;
    const l = n && S.labels.get(n._id);
    setMouth(l ? l.vowel : null);
    dirty();
  }
  refreshLcd();
}

async function boot() {
  B = await createBackend();
  B.onScoreFileDrop?.(file=> {
    const binary=atob(file.base64 || "");
    importFileObj({name:file.name,arrayBuffer:async()=>Uint8Array.from(binary,c=>c.charCodeAt(0)).buffer});
  });
  B.onAudioFileDrop?.((file) => {
    if (typeof file === "string") flash("CAN'T IMPORT", file);
    else audioImport.open({name:file.name}, S.song.voice, detailedNotes.checked, importStatus(true));
  });
  refreshButtons();
  const firstStatus = new Promise((res) => B.onStatus((s) => { res(); onStatus(s); }));
  let songTxt = "", voices = [];
  try { [songTxt, voices] = await Promise.all([B.getSong(), B.getVoices()]); } catch (e) { console.warn(e); }
  if (Array.isArray(voices) && voices.length) S.voices = voices.map(String);
  buildGallery();
  buildSliders();
  let parsed = {};
  try { parsed = songTxt ? JSON.parse(songTxt) : {}; } catch {}
  await Promise.race([firstStatus, new Promise((r) => setTimeout(r, 400))]); // learn the tempo first
  S.song = normSong(parsed);
  await restoreSource();
  lastPushed = serialize(S.song);
  S.seenVersion = S.status.songVersion;
  S.loaded = true;
  measure();
  onSongChanged({ lyrics: true, push: false });
  scrollToNotes();
  fetchPlan();
}

window.__novox = { S, H }; // for debugging from the console
new ResizeObserver(measure).observe(els.gridwrap);
window.addEventListener("resize", measure);
buildSliders();
syncVoiceUI();
refreshInfo();
measure();
requestAnimationFrame(frame);
boot().catch((e) => { console.error(e); flash("ERROR", String(e && e.message || e)); });

// Host windows do not always expose a familiar resize border.
$("windowSize").addEventListener("change", async e => {
  const sizes = {normal:[1000,660],large:[1280,800],largest:[1600,1000]};
  const [width,height] = sizes[e.target.value] || sizes.normal;
  if (B.setEditorSize) await B.setEditorSize(width,height);
});

function selectAllNotes() {
  if (isTyping(document.activeElement)) return;
  S.sel = new Set(S.song.notes.map(n=>n._id)); refreshInfo(); dirty();
}
window.addEventListener("novox-select-all",selectAllNotes);
const timelineCanvas = $("timelineCanvas"), timelineToggle = $("timelineToggle");
timelineToggle.addEventListener("click",()=>{
  timelineCanvas.hidden = !timelineCanvas.hidden;
  timelineToggle.setAttribute("aria-expanded",String(!timelineCanvas.hidden));
  measure();
});
function timelineEnd() {
  return Math.max(16,Math.ceil((songEnd()+4)/4)*4,(sc.scrollLeft+sc.clientWidth)/S.ppb);
}
function drawTimeline() {
  const cv=$("timelineCanvas"); if (!cv || cv.hidden) return;
  const w=cv.clientWidth,h=cv.clientHeight,c=sizeCanvas(cv,w,h,false),end=timelineEnd();
  c.fillStyle="#fff";c.fillRect(0,0,w,h);
  const step=Math.max(4,Math.ceil(end/Math.max(1,w/65)/4)*4);
  c.font=`10px ${FONT}`;c.textBaseline="top";
  for(let b=0;b<end;b+=step) {
    const x=b/end*w;c.fillStyle="#ddd";c.fillRect(x,0,1,h);
    c.fillStyle="#555";c.fillText(String(b/4+1),x+3,2);
  }
  // Each imported recording, where it still stands behind the notes: a grey bar along the bottom.
  const records=songTranscriptions(S.song);
  visibleSourceRanges(records,spanOfRecord).forEach((ranges,k)=>{
    if(!sourceRecordings.get(records[k].recordingId)?.original) return;
    c.fillStyle="#9aa6b0";
    for(const [a,b] of ranges) c.fillRect(a/end*w,h-6,Math.max(2,(b-a)/end*w-1),3);
  });
  const lo=Math.min(48,...S.song.notes.map(n=>n.pitch)),hi=Math.max(72,...S.song.notes.map(n=>n.pitch));
  for(const n of S.song.notes) {
    c.fillStyle=S.sel.has(n._id)?"#1f5fae":"#73a9d4";
    c.fillRect(n.start/end*w,18+(hi-n.pitch)/(hi-lo+1)*(h-25),Math.max(2,n.dur/end*w),4);
  }
  c.strokeStyle="#1f5fae";c.lineWidth=2;
  c.strokeRect(sc.scrollLeft/S.ppb/end*w+1,1,Math.min(w-2,sc.clientWidth/S.ppb/end*w)-2,h-2);
  c.fillStyle="#a32929";c.fillRect(S.status.beats/end*w,0,1,h);
}
function navigateTimeline(e) {
  const rect=timelineCanvas.getBoundingClientRect();
  const beat=clamp((e.clientX-rect.left)/rect.width,0,1)*timelineEnd();
  sc.scrollLeft=Math.max(0,beat*S.ppb-sc.clientWidth/2);dirty();
}
timelineCanvas.addEventListener("pointerdown",e=>{timelineCanvas.focus();timelineCanvas.setPointerCapture(e.pointerId);navigateTimeline(e);});
timelineCanvas.addEventListener("pointermove",e=>{if(timelineCanvas.hasPointerCapture(e.pointerId))navigateTimeline(e);});
timelineCanvas.addEventListener("keydown",e=>{
  if(e.key==="ArrowLeft" || e.key==="ArrowRight") {e.preventDefault();e.stopPropagation();sc.scrollLeft+= (e.key==="ArrowLeft"?-1:1)*4*S.ppb;dirty();}
});

// Singing capture uses native input in both standalone and plugin modes.
let micState = {recording:false,pending:false}, micStopping=false, lastMicError="";
function updateMicrophone(state) {
  micState=state;
  const button=$("singCapture"),active=state.recording || state.pending;
  button.classList.toggle("on",active);button.setAttribute("aria-pressed",String(active));
  button.disabled=!!state.pending || micStopping;
  els.capture.disabled=active;
  $("singHelp").hidden=!active;
  $("singCancel").disabled=!!state.pending;
  $("singLevel").value=state.level || 0;
  $("singMessage").textContent=state.pending ? "Allow microphone access to record your singing." : `Sing, then click the singing icon to stop and convert · ${Math.floor(state.seconds || 0)}s / 60s`;
  if(state.error && state.error!==lastMicError) {takeStatus=null;flash("CAN’T RECORD",state.error);}
  lastMicError=state.error || "";
  if(state.full && !micStopping) stopSinging();
}
async function stopSinging() {
  if(micStopping) return;
  micStopping=true;$("singCapture").disabled=true;
  try {
    const reply=await B.stopMicrophone();
    if(typeof reply==="string" && reply) {takeStatus=null;flash("CAN’T RECORD",reply);}
  } catch(e) {takeStatus=null;flash("CAN’T RECORD",String(e));}
  finally {micStopping=false;}
}
$("singCapture").addEventListener("click",async()=>{
  if(micState.recording) return stopSinging();
  if(!B.startMicrophone) {flash("MICROPHONE UNAVAILABLE","Singing capture is available in the native app and plugin");return;}
  if(S.status.capture) {S.status.capture=false;B.setCapture(false);refreshButtons();}
  try {
    updateMicrophone({pending:true});takeStatus={...S.status};
    const reply=await B.startMicrophone();
    if(typeof reply==="string" && reply) {takeStatus=null;updateMicrophone({recording:false,pending:false});flash("CAN’T RECORD",reply);}
  } catch(e) {takeStatus=null;updateMicrophone({recording:false,pending:false});flash("CAN’T RECORD",String(e));}
});
$("singCancel").addEventListener("click",async()=>{
  if(micState.pending) return;
  await B.cancelMicrophone?.();takeStatus=null;updateMicrophone({recording:false,pending:false});flash("RECORDING CANCELLED");
});


// Source recordings remain in this editor session, keyed so Undo restores the right sources.
const sourceRecordings = new Map();
const spanOfRecord = (record) => importSpan(record, sourceRecordings.get(record.recordingId));
let phraseMode=null;
function phraseBounds() {
  const selected=S.song.notes.filter(n=>S.sel.has(n._id));
  const notes=selected.length ? selected : S.song.notes;
  return notes.length ? [Math.min(...notes.map(n=>n.start)),Math.max(...notes.map(n=>n.start+n.dur))] : [0,0];
}
// The original recordings under the roll, each import at its own place: the part of it that still
// stands behind the notes (later imports replaced the rest), with a blue tick where it starts.
function drawSourceWave() {
  const records=songTranscriptions(S.song), sources=records.map(r=>sourceRecordings.get(r.recordingId));
  const any=sources.some(source=>source?.original);
  $("hearOriginal").disabled=!any;
  const [begin,end]=phraseBounds();
  $("phraseRange").textContent=S.sel.size ? `Selected phrase · ${begin.toFixed(1)}–${end.toFixed(1)} beats` : "Whole song · select notes for a phrase";
  const canvas=$("sourceWave");canvas.hidden=!any;
  if(!any) return;
  const width=Math.max(1,Math.round(els.scroller.getBoundingClientRect().right-canvas.getBoundingClientRect().left)),height=40;canvas.style.width=width+"px";canvas.width=width;canvas.height=height;
  const c=canvas.getContext("2d");c.clearRect(0,0,width,height);
  // Match the roll's left keyboard gutter and horizontal scrolling.
  const gutter=els.scroller.getBoundingClientRect().left-canvas.getBoundingClientRect().left;
  const xOf=b=>gutter+b*S.ppb-els.scroller.scrollLeft;
  const firstBeat=(els.scroller.scrollLeft-1)/S.ppb, lastBeat=firstBeat+(width-gutter+2)/S.ppb;
  const live=visibleSourceRanges(records,spanOfRecord);
  records.forEach((record,k)=>{
    const source=sources[k], span=spanOfRecord(record);
    if(!source?.original || !span) return;
    const peaks=source.waveform || [], beats=span[1]-span[0], count=peaks.length;
    if(count) {
      const w=Math.max(1,beats*S.ppb/count);
      c.fillStyle="#677784";
      for(const [a,b] of live[k]) {
        const from=Math.max(a,firstBeat), to=Math.min(b,lastBeat);
        if(to<=from) continue;
        for(let i=Math.max(0,Math.floor((from-span[0])/beats*count));i<Math.min(count,Math.ceil((to-span[0])/beats*count));i++) {
          const x=xOf(span[0]+i/count*beats);
          if(x<gutter || x>width) continue;
          const h=Math.max(1,Number(peaks[i])*36);c.fillRect(x,20-h/2,w,h);
        }
      }
    }
    const x0=Math.round(xOf(span[0]));
    if(live[k].length && x0>=gutter && x0<=width) {c.fillStyle="#3b8ee8";c.fillRect(x0,3,1,34);}
  });
  c.strokeStyle="#a0a0a0";c.beginPath();c.moveTo(gutter,20);c.lineTo(width,20);c.stroke();
}
async function hearPhrase(original) {
  const [begin,end]=phraseBounds();if(end<=begin) return flash("SELECT A PHRASE","Add notes before listening");
  if(S.status.playing) return flash("STOP PLAYBACK","Stop playback before comparing a phrase");
  const loop=$("phraseLoop").checked;
  try {
    let error;
    if(original) {
      // Each import's recording under the phrase, at its own place (mixed where they overlap).
      const pieces=originalSegments(songTranscriptions(S.song),id=>sourceRecordings.get(id),begin,end);
      if(!pieces.length) return flash("NO RECORDING","No original recording lies under these notes");
      error=B.playSourceReview ? await B.playSourceReview(pieces,loop) : "Phrase listening requires the native app or plugin";
    } else error=B.playSongReview ? await B.playSongReview(begin,end,loop) : "Phrase listening requires the native app or plugin";
    if(error) flash("CAN’T PLAY",error);
    else phraseMode=original ? "original" : "robot";
  } catch(e) {flash("CAN’T PLAY",String(e));}
}
$("hearOriginal").addEventListener("click",()=>hearPhrase(true));
$("hearRobot").addEventListener("click",()=>hearPhrase(false));
$("hearStop").addEventListener("click",()=>{phraseMode=null;B.stopAudioReview?.();});
$("phraseLoop").addEventListener("change",()=>{phraseMode=null;B.stopAudioReview?.();});

// Fetch the recordings of the song's imports that this editor hasn't seen (a reopened editor,
// a loaded project or song file).
async function restoreSource() {
  if(!B.getSource) return;
  const ids=new Set(songTranscriptions(S.song).map(t=>t.recordingId).filter(id=>id && !sourceRecordings.has(id)));
  for(const id of ids) {const source=await B.getSource(id);if(source?.original) sourceRecordings.set(id,source);}
  if(ids.size) dirty();
}
for(const button of document.querySelectorAll("[data-file-action]")) {
  const action=button.dataset.fileAction;
  button.addEventListener(action.startsWith("drag") ? "pointerdown" : "click",async e=>{
    if(action.startsWith("drag")) e.preventDefault();
    if(!B.fileAction) return flash("NATIVE APP REQUIRED","File export is available in the app and plugin");
    try {clearTimeout(pushTimer);await pushNow();const error=await B.fileAction(action);if(error && error!=="cancelled") flash("CAN’T SAVE",error);else if(!error && !action.startsWith("drag")) flash("FILE READY");}catch(e) {flash("FILE ERROR",String(e));}
  });
}


// Pin flowing lyrics before local surgery so later words keep their assignments.
function pinNoteLyrics() {
  if(S.song.useNoteLyrics) return true;
  if(!S.plan) {flash("WAIT FOR LYRICS","The singer is still assigning words to notes");return false;}
  for(const n of S.song.notes) n.lyric=pinnedLyric(S.labels.get(n._id));
  S.song.useNoteLyrics=true;return true;
}
let syllableNoteId=null;
$("splitSyllables").addEventListener("click",()=>{
  const selected=selectedNotes();if(selected.length!==1) return flash("SELECT ONE NOTE","Choose the note containing the word to split");
  syllableNoteId=selected[0]._id;$("syllableText").value=selected[0].lyric || S.labels.get(syllableNoteId)?.text || "";
  $("syllableDialog").showModal();$("syllableText").focus();$("syllableText").select();
});
$("syllableDialog").addEventListener("close",()=>{
  if($("syllableDialog").returnValue!=="apply") return;
  const parts=$("syllableText").value.trim().split(/[-\s]+/).filter(Boolean);
  const note=S.song.notes.find(n=>n._id===syllableNoteId);
  if(!note || parts.length<2 || parts.length>16 || note.dur/parts.length<.03125) return flash("CHECK SYLLABLES","Enter two or more syllables separated by hyphens");
  checkpoint();if(!pinNoteLyrics()) {dropNoopCheckpoint();return;}const duration=note.dur/parts.length;
  const replacements=parts.map((part,i)=>({...note,_id:nextId++,start:note.start+i*duration,dur:duration,lyric:part+(i<parts.length-1?"-":"")}));
  S.song.notes=S.song.notes.filter(n=>n._id!==note._id).concat(replacements).sort((a,b)=>a.start-b.start);
  S.sel=new Set(replacements.map(n=>n._id));onSongChanged();flash("SYLLABLES SPLIT",`${parts.length} notes · drag their edges to adjust timing`);
});
$("joinSyllables").addEventListener("click",()=>{
  const notes=selectedNotes().sort((a,b)=>a.start-b.start);
  if(notes.length<2) return flash("SELECT NOTES","Select adjacent notes of the same pitch to join");
  const begin=notes[0].start,end=Math.max(...notes.map(n=>n.start+n.dur));
  if(notes.some(n=>n.pitch!==notes[0].pitch) || S.song.notes.some(n=>!S.sel.has(n._id) && n.start<end && n.start+n.dur>begin)) return flash("CAN’T JOIN","Choose adjacent notes of the same pitch");
  checkpoint();if(!pinNoteLyrics()) {dropNoopCheckpoint();return;}
  let lyric="";for(const n of notes) {const token=n.lyric || "";if(!token || token==="_") continue;lyric=lyric.endsWith("-") ? lyric.slice(0,-1)+token : lyric+(lyric?" ":"")+token;}lyric=lyric.replace(/-$/," ").trim();
  const merged={...notes[0],start:begin,dur:end-begin,lyric};S.song.notes=S.song.notes.filter(n=>!S.sel.has(n._id)).concat([merged]).sort((a,b)=>a.start-b.start);
  S.sel=new Set([merged._id]);onSongChanged();flash("NOTES JOINED");
});
function moveLyric(direction) {
  const selected=selectedNotes();if(selected.length!==1) return flash("SELECT ONE NOTE","Choose the lyric to move");
  const notes=[...S.song.notes].sort((a,b)=>a.start-b.start),index=notes.findIndex(n=>n._id===selected[0]._id),target=notes[index+direction];
  if(!target) return flash("END OF SONG");checkpoint();if(!pinNoteLyrics()) {dropNoopCheckpoint();return;}
  [selected[0].lyric,target.lyric]=[target.lyric || "",selected[0].lyric || ""];
  S.sel=new Set([target._id]);onSongChanged();flash("LYRICS SWAPPED");
}
$("lyricLeft").addEventListener("click",()=>moveLyric(-1));$("lyricRight").addEventListener("click",()=>moveLyric(1));

$("syllableText").addEventListener("keydown",e=>{if(e.key==="Enter") {e.preventDefault();$("syllableDialog").close("apply");}});

// ===========================================================================
// Popovers: Export, Tweak (voice), Key, Edit, Compare, View. A [data-pop] button toggles the
// element it names; a click outside or Escape closes it; [data-close] items close it after acting.
let openPop = null;
function closePop(focusButton = false) {
  if (!openPop) return;
  const { btn, pop } = openPop;
  openPop = null;
  pop.hidden = true;
  btn.setAttribute("aria-expanded", "false");
  btn.classList.remove("open");
  if (focusButton) btn.focus();
}
function openPopFor(btn) {
  closePop();
  const pop = $(btn.dataset.pop);
  pop.hidden = false;
  btn.setAttribute("aria-expanded", "true");
  btn.classList.add("open");
  openPop = { btn, pop };
}
for (const btn of document.querySelectorAll("[data-pop]")) {
  btn.setAttribute("aria-expanded", "false");
  btn.setAttribute("aria-controls", btn.dataset.pop);
  btn.addEventListener("click", () => (openPop && openPop.btn === btn ? closePop() : openPopFor(btn)));
}
for (const item of document.querySelectorAll(".pop [data-close]")) item.addEventListener("click", () => closePop());
document.addEventListener("pointerdown", (e) => {
  if (openPop && !openPop.pop.contains(e.target) && !openPop.btn.contains(e.target)) closePop();
}, true);
window.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && openPop && !document.querySelector("dialog[open]")) { e.stopPropagation(); closePop(true); }
}, true);
// index.html#voices opens the voice tweaks, #pop=<id> any popover (for screenshots and docs).
{
  const m = /^#(?:pop=([\w-]+)|(voices))$/.exec(location.hash);
  const btn = m && document.querySelector(`[data-pop="${m[2] ? "tweakPop" : m[1]}"]`);
  if (btn) openPopFor(btn);
}
