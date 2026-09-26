// Novox The Robot plugin editor. Plain ES module, no dependencies.
//
// Sections:
//   1. constants + helpers        5. roll drawing (canvas)
//   2. voices + cartoon faces     6. roll input (mouse, keyboard, wheel)
//   3. model, undo, sync          7. toolbar, lyrics, voice panel, import
//   4. plan -> note labels        8. status + boot
import { createBackend } from "./backend.js";

const $ = (id) => document.getElementById(id);
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
  { key: "formantScale", label: "Size", min: 0.85, max: 1.35, step: 0.01, fmt: (v) => "×" + v.toFixed(2) },
  { key: "vibratoDepth", label: "Vibrato", min: 0, max: 0.8, step: 0.01, fmt: (v) => Math.round(v * 100) + " ¢" },
  { key: "portamento", label: "Glide", min: 0.02, max: 0.4, step: 0.01, fmt: (v) => Math.round(v * 1000) + " ms" },
  { key: "breathiness", label: "Breath", min: 0, max: 0.2, step: 0.005, fmt: (v) => Math.round(v * 500) + "%" },
  { key: "grit", label: "Grit", min: 0, max: 0.6, step: 0.01, fmt: (v) => Math.round((v / 0.6) * 100) + "%" },
  { key: "choirSize", label: "Choir", min: 1, max: 8, step: 1, fmt: (v) => (v === 1 ? "solo" : v + " voices") },
];

let faceSeq = 0;
function faceSvg(o, size, inner = false) {
  const id = "f" + faceSeq++;
  const cx = 27, cy = o.kid ? 30 : 28, r = o.kid ? 15 : 17;
  const skin = o.robot ? "#b9c4cf" : o.skin, hair = o.hair || "#333";
  let h = `<defs><radialGradient id="${id}" cx=".4" cy=".35" r=".8"><stop offset="0" stop-color="#fff" stop-opacity=".6"/><stop offset=".5" stop-color="#fff" stop-opacity="0"/></radialGradient></defs>`;
  if (o.hood) h += `<path d="M8 50 Q6 12 27 8 Q48 12 46 50Z" fill="${o.hood}"/><path d="M8 50 Q6 12 27 8 Q48 12 46 50Z" fill="url(#${id})"/>`;
  if (o.style === "long") h += `<path d="M${cx - r - 3} ${cy + 18} Q${cx - r - 4} ${cy - r - 6} ${cx} ${cy - r - 4} Q${cx + r + 4} ${cy - r - 6} ${cx + r + 3} ${cy + 18}Z" fill="${hair}"/>`;
  if (o.robot) {
    h += `<line x1="${cx}" y1="4" x2="${cx}" y2="12" stroke="#666" stroke-width="2"/><circle cx="${cx}" cy="4" r="3" fill="#ff5a4f"/>`;
    h += `<rect x="${cx - r - 3}" y="${cy - 4}" width="4" height="9" rx="1.5" fill="#8894a0"/><rect x="${cx + r - 1}" y="${cy - 4}" width="4" height="9" rx="1.5" fill="#8894a0"/>`;
    h += `<rect x="${cx - r}" y="${cy - r}" width="${2 * r}" height="${2 * r + 2}" rx="7" fill="${skin}" stroke="#7d8894"/>`;
  } else {
    h += `<ellipse cx="${cx}" cy="${cy}" rx="${r}" ry="${r + 1}" fill="${skin}"/>`;
  }
  if ((!o.style || o.style === "short") && !o.hood && !o.robot) h += `<path d="M${cx - r} ${cy - 2} Q${cx - r} ${cy - r - 5} ${cx} ${cy - r - 3} Q${cx + r} ${cy - r - 5} ${cx + r} ${cy - 2} Q${cx + 6} ${cy - r + 3} ${cx - r} ${cy - 2}Z" fill="${hair}"/>`;
  if (o.style === "slick") h += `<path d="M${cx - r} ${cy - 1} Q${cx - r - 1} ${cy - r - 6} ${cx + 2} ${cy - r - 4} Q${cx + r + 2} ${cy - r - 3} ${cx + r} ${cy - 3} Q${cx + r - 8} ${cy - r + 1} ${cx - r + 5} ${cy - r + 6} Q${cx - r + 1} ${cy - 4} ${cx - r} ${cy - 1}Z" fill="${hair}"/>`;
  if (o.style === "long") h += `<path d="M${cx - r} ${cy} Q${cx - 4} ${cy - r - 2} ${cx + r} ${cy - 4} Q${cx + r} ${cy - r - 4} ${cx} ${cy - r - 3} Q${cx - r} ${cy - r - 2} ${cx - r} ${cy}Z" fill="${hair}"/>`;
  if (o.style === "bun") h += `<circle cx="${cx}" cy="${cy - r - 3}" r="6" fill="${hair}"/><path d="M${cx - r} ${cy - 2} Q${cx} ${cy - r - 8} ${cx + r} ${cy - 2} Q${cx} ${cy - r + 2} ${cx - r} ${cy - 2}Z" fill="${hair}"/>`;
  if (o.style === "bald") h += `<path d="M${cx - r} ${cy + 2} Q${cx - r} ${cy - 6} ${cx - r + 3} ${cy - 4}M${cx + r} ${cy + 2} Q${cx + r} ${cy - 6} ${cx + r - 3} ${cy - 4}" stroke="${hair}" stroke-width="3" fill="none"/>`;
  if (o.style === "mohawk") h += `<path d="M${cx - 4} ${cy - r + 2} L${cx - 2} ${cy - r - 9} L${cx + 2} ${cy - r - 7} L${cx + 4} ${cy - r + 2}Z" fill="${hair}"/>`;
  if (o.beard) h += `<path d="M${cx - r + 2} ${cy + 2} Q${cx} ${cy + r + 12} ${cx + r - 2} ${cy + 2} Q${cx} ${cy + 10} ${cx - r + 2} ${cy + 2}Z" fill="${o.beard}"/>`;
  const ey = cy - 2;
  if (o.robot) h += `<rect x="${cx - 10}" y="${ey - 4}" width="7" height="6" rx="1.5" fill="#1b2b3a"/><rect x="${cx + 3}" y="${ey - 4}" width="7" height="6" rx="1.5" fill="#1b2b3a"/><rect x="${cx - 9}" y="${ey - 3}" width="3" height="2" fill="#7df9ff"/><rect x="${cx + 4}" y="${ey - 3}" width="3" height="2" fill="#7df9ff"/>`;
  else if (o.glasses) h += `<rect x="${cx - 12}" y="${ey - 4}" width="10" height="7" rx="2" fill="#111"/><rect x="${cx + 2}" y="${ey - 4}" width="10" height="7" rx="2" fill="#111"/><path d="M${cx - 2} ${ey - 1}h4" stroke="#111" stroke-width="1.5"/>`;
  else h += `<ellipse cx="${cx - 6}" cy="${ey}" rx="1.9" ry="2.4" fill="#222"/><ellipse cx="${cx + 6}" cy="${ey}" rx="1.9" ry="2.4" fill="#222"/><circle cx="${cx - 5.4}" cy="${ey - 0.8}" r=".7" fill="#fff"/><circle cx="${cx + 6.6}" cy="${ey - 0.8}" r=".7" fill="#fff"/>`;
  if (o.blush) h += `<ellipse cx="${cx - 10}" cy="${cy + 5}" rx="3" ry="1.8" fill="#ff8a8a" opacity=".45"/><ellipse cx="${cx + 10}" cy="${cy + 5}" rx="3" ry="1.8" fill="#ff8a8a" opacity=".45"/>`;
  h += `<ellipse class="mouth" cx="${cx}" cy="${cy + 8}" rx="4" ry="1.5" fill="${o.robot ? "#1b2b3a" : "#5a1d1d"}"/>`;
  if (o.phones) h += `<path d="M${cx - r - 1} ${cy + 2} Q${cx - r} ${cy - r - 8} ${cx} ${cy - r - 7} Q${cx + r} ${cy - r - 8} ${cx + r + 1} ${cy + 2}" stroke="#333" stroke-width="3" fill="none"/><rect x="${cx - r - 5}" y="${cy - 3}" width="7" height="11" rx="3" fill="#e0457b"/><rect x="${cx + r - 2}" y="${cy - 3}" width="7" height="11" rx="3" fill="#e0457b"/>`;
  if (!o.robot) h += `<ellipse cx="${cx}" cy="${cy}" rx="${r}" ry="${r + 1}" fill="url(#${id})"/>`;
  return inner ? h : `<svg width="${size}" height="${size}" viewBox="0 0 54 54" aria-hidden="true">${h}</svg>`;
}
function voiceSvg(v, size) {
  if (!v.group) return faceSvg(v.face, size);
  let h = `<svg width="${size}" height="${size}" viewBox="0 0 54 54" aria-hidden="true">`;
  v.group.forEach((o, i) => {
    const x = [-12, 12, 0][i], y = [-4, -4, 6][i], sc = 0.62;
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
  document.querySelectorAll("#bigface .mouth, #chipFace .mouth").forEach((m) => { m.setAttribute("rx", rx); m.setAttribute("ry", ry); });
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
  let txt = "";
  try { txt = await B.getPlan(); } catch {}
  if (my !== planSeq) return;
  const bpm = S.bpm; // the plan is in seconds at the tempo current when it was made
  let plan = null;
  try { plan = txt ? JSON.parse(txt) : null; } catch {}
  S.plan = plan;
  S.planBpm = bpm;
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
  updateWorld();
  relabel();
  refreshTexts();
  dirty();
  if (push) schedulePush();
}

// ===========================================================================
// 4. plan -> what each note sings
function relabel() {
  const labels = new Map();
  const syls = (S.plan?.syllables || []).map((s) => ({ ...s, b0: (s.start * S.planBpm) / 60, b1: (s.end * S.planBpm) / 60 }));
  const notes = sortNotes([...S.song.notes]);
  const tol = 0.04;
  for (const n of notes) {
    if (!S.plan) { labels.set(n._id, null); continue; }
    let best = -1, bestD = tol;
    for (let i = 0; i < syls.length; i++) {
      const d = Math.abs(syls[i].b0 - n.start);
      if (d < bestD) { bestD = d; best = i; }
      if (syls[i].b0 > n.start + tol) break;
    }
    if (best >= 0) {
      const s = syls[best], prev = syls[best - 1];
      const ph = String(s.phonemes || "");
      const phon = ph.replace(/\|/g, " ").replace(/\s+/g, " ").trim();
      const vowel = (ph.split("|")[1] || "").trim().split(" ")[0];
      if (s.text) labels.set(n._id, { kind: "word", text: s.text + (s.continues ? "-" : ""), syl: s, phon, vowel });
      else if (prev && prev.continues) labels.set(n._id, { kind: "cont", text: "-" + phon.toLowerCase().replace(/ /g, ""), syl: s, phon, vowel });
      else labels.set(n._id, { kind: "hum", text: "mm", syl: s, phon: phon || "M", vowel: "M" });
      continue;
    }
    const s = syls.find((x) => x.b0 < n.start + tol && x.b1 > n.start + tol);
    if (s) {
      const vowel = (String(s.phonemes || "").split("|")[1] || "").trim().split(" ")[0];
      labels.set(n._id, { kind: "hold", text: "_", syl: s, phon: vowel, vowel });
    } else {
      labels.set(n._id, { kind: "hum", text: "mm", syl: null, phon: "M", vowel: "M" });
    }
  }
  S.labels = labels;
}

// ===========================================================================
// 5. roll drawing
const els = {
  app: $("app"), grid: $("grid"), keys: $("keys"), ruler: $("ruler"), scroller: $("scroller"), sizer: $("sizer"), gridwrap: $("gridwrap"),
  lyrics: $("lyrics"), counts: $("counts"), warn: $("warn"), info: $("info"), snap: $("snap"), lyricEdit: $("lyricEdit"),
  lcd: $("lcd"), lcdState: $("lcdState"), lcdPos: $("lcdPos"), lcdLine: $("lcdLine"),
  capture: $("capture"), play: $("play"), playIco: $("playIco"), playLbl: $("playLbl"), tempo: $("tempo"),
  file: $("file"), mode: $("mode"), voiceChip: $("voiceChip"),
};
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
  }
}

function updateDrag(d, p) {
  if (d.mode === "move") {
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

// piano keys: highlight a row (no sound)
els.keys.addEventListener("pointerdown", (e) => {
  const r = els.keys.getBoundingClientRect();
  const p = clamp(S.hi - Math.floor((e.clientY - r.top + sc.scrollTop) / S.rowH), S.lo, S.hi);
  S.hotPitch = S.hotPitch === p ? null : p;
  dirty();
});

// ---- lyric override editor (double-click a note)
let editingId = null;
function openLyricEdit(n) {
  editingId = n._id;
  const ed = els.lyricEdit;
  ed.hidden = false;
  ed.value = n.lyric || "";
  const x = n.start * S.ppb - sc.scrollLeft, y = (S.hi - n.pitch) * S.rowH - sc.scrollTop;
  ed.style.left = clamp(x, 0, view.w - 90) + "px";
  ed.style.top = clamp(y - 4, 0, view.h - 22) + "px";
  ed.style.width = Math.max(90, n.dur * S.ppb) + "px";
  const l = S.labels.get(n._id);
  ed.placeholder = l && l.kind === "word" ? `own lyric (now “${l.text}”)` : "own lyric";
  ed.focus();
  ed.select();
}
function closeLyricEdit(commit) {
  const ed = els.lyricEdit;
  if (ed.hidden) return;
  ed.hidden = true;
  const n = noteById(editingId);
  editingId = null;
  if (!commit || !n) return;
  const v = ed.value.trim();
  if ((n.lyric || "") === v) return;
  checkpoint();
  if (v) n.lyric = v; else delete n.lyric;
  onSongChanged();
}
els.lyricEdit.addEventListener("keydown", (e) => {
  if (e.key === "Enter") { e.preventDefault(); closeLyricEdit(true); sc.focus({ preventScroll: true }); }
  else if (e.key === "Escape") { e.preventDefault(); closeLyricEdit(false); sc.focus({ preventScroll: true }); }
  e.stopPropagation();
});
els.lyricEdit.addEventListener("blur", () => closeLyricEdit(true));

// ---- keyboard shortcuts (ignored while typing)
document.addEventListener("keydown", (e) => {
  if (isTyping(e.target)) return;
  const mod = e.metaKey || e.ctrlKey;
  const k = e.key;
  if (mod && (k === "z" || k === "Z")) { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
  if (mod && (k === "y" || k === "Y")) { e.preventDefault(); redo(); return; }
  if (mod && (k === "a" || k === "A")) { e.preventDefault(); S.sel = new Set(S.song.notes.map((n) => n._id)); refreshInfo(); dirty(); return; }
  if (mod && (k === "d" || k === "D")) { e.preventDefault(); duplicateSelection(); return; }
  if (k === "Escape") { S.sel.clear(); S.hotPitch = null; refreshInfo(); dirty(); return; }
  if (k === " " && !S.status.host && !mod) { e.preventDefault(); togglePlay(); return; }
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
  const el = els.info;
  let id = drag && drag.anchor ? drag.anchor._id : S.hover;
  if (id == null && S.sel.size === 1) id = [...S.sel][0];
  const n = id != null ? noteById(id) : null;
  if (!n) {
    if (S.sel.size > 1) { el.innerHTML = `<b>${S.sel.size} notes</b> selected · arrows nudge · ⌫ deletes · ⌘D duplicates`; return; }
    el.textContent = S.song.notes.length
      ? "Drag to draw a note · shift-drag to select · double-click a note to give it its own lyric"
      : "Draw some notes, import a MIDI file, or arm Capture and play your keyboard.";
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
function flash(state, line = "") { S.flash = { state, line, until: performance.now() + 2200 }; refreshLcd(); }
let lcdCache = "";
function refreshLcd() {
  const st = S.status;
  const vname = voiceMeta(voiceParts().preset).n;
  let state, line, cls = "";
  if (S.flash && performance.now() < S.flash.until) ({ state, line } = S.flash);
  else if (st.error) { state = "ERROR"; line = st.error; cls = "err"; }
  else if (!S.loaded) { state = "LOADING…"; line = "warming up the vocal cords"; }
  else if (st.rendering) { state = "RENDERING…"; line = "the singer is taking a breath"; }
  else if (st.playing) {
    state = st.capture ? "● CAPTURING" : "PLAYING ♪";
    const n = noteAtBeat(st.beats), l = n && S.labels.get(n._id);
    line = l ? (l.kind === "hum" ? "♪ mmm…" : l.kind === "hold" ? `♪ ${l.syl?.text || ""} ~` : `♪ ${l.text}`) : "♪";
    if (st.capture) cls = "rec";
  } else if (st.capture) { state = "CAPTURE ARMED"; line = st.host ? "play the DAW and your keyboard" : "press Play and your keyboard"; cls = "rec"; }
  else { state = "READY"; line = `${vname} · ${Math.round(S.bpm)} bpm · ${S.song.notes.length} notes`; }
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
function togglePlay() { B.setTransport(!S.status.playing); }
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
  els.play.classList.toggle("on", !!st.playing);
  els.playIco.innerHTML = st.playing ? "&#9632;" : "&#9654;";
  els.playLbl.textContent = st.playing ? "Stop" : "Play";
  els.app.classList.toggle("host", !!st.host);
  els.mode.textContent = (st.host ? "In host · the DAW drives tempo and transport" : "Standalone") + (B && B.isMock ? " · mock backend" : "");
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
  S.sel.clear();
  onSongChanged();
  flash("CLEARED", "notes removed · ⌘Z brings them back");
});

// ---- lyrics
els.lyrics.addEventListener("input", () => {
  checkpoint("lyrics");
  S.song.lyrics = els.lyrics.value;
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
    b.title = `${v.n}${v.d ? " · " + v.d : ""}`;
    b.innerHTML = voiceSvg(v, 34) + `<span>${escapeHtml(v.n)}</span>`;
    b.addEventListener("click", () => {
      if (voiceParts().preset === name && !Object.keys(voiceParts().ov).length) return;
      checkpoint();
      setVoice(name, {});
    });
    g.appendChild(b);
  }
}
const sliderEls = {};
function buildSliders() {
  const host = $("sliders");
  host.innerHTML = "";
  for (const s of SLIDERS) {
    const row = document.createElement("div");
    row.className = "sl";
    row.title = "Double-click to return to the preset value";
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
    $("chipFace").innerHTML = voiceSvg(v, 26);
    $("vname").textContent = v.n;
    $("vsub").textContent = v.d;
    $("chipName").textContent = v.n;
    mouthNow = "?";
    setMouth(null);
    if (!S.voices.includes(preset)) { S.voices.push(preset); buildGallery(); }
  }
  for (const c of $("gallery").children) c.setAttribute("aria-checked", c.dataset.voice === preset);
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
}
$("reset").addEventListener("click", () => {
  const { preset, ov } = voiceParts();
  if (!Object.keys(ov).length) return;
  checkpoint();
  setVoice(preset, {});
});
function setPanel(open) {
  els.app.classList.toggle("novoice", !open);
  els.voiceChip.setAttribute("aria-expanded", open);
  requestAnimationFrame(measure);
}
els.voiceChip.addEventListener("click", () => setPanel(els.app.classList.contains("novoice")));
// In drawer mode (narrow window) clicking the roll closes the drawer.
sc.addEventListener("pointerdown", () => { if (window.innerWidth <= 900 && !els.app.classList.contains("novoice")) setPanel(false); }, true);

// ---- import (file picker or drag-and-drop)
function toBase64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}
function lyricsFromNotes(notes) {
  if (!notes.some((n) => n.lyric && String(n.lyric).trim())) return null;
  const toks = notes.map((n) => String(n.lyric || "").trim() || "_");
  let out = "";
  toks.forEach((t, i) => {
    const prev = out.slice(-1);
    if (t === "_" && prev === "-") out = out.slice(0, -1); // melisma inside a word
    const glue = out && !(prev === "-" && t !== "_") ? " " : "";
    out += glue + t;
  });
  return out.replace(/-\s*$/, "").trim();
}
async function importFileObj(file) {
  if (!file) return;
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
  const lyr = typeof score.lyrics === "string" && score.lyrics.trim() ? score.lyrics : lyricsFromNotes(notes);
  checkpoint();
  S.song.notes = notes.map((n) => ({ pitch: clamp(n.pitch, 0, 127), start: Math.max(0, n.start), dur: n.dur, _id: nextId++ }));
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
});

// ===========================================================================
// 8. status + boot
function onStatus(s) {
  const prev = S.status;
  const st = { ...prev, ...s };
  if (st.beats == null || !Number.isFinite(+st.beats)) st.beats = 0;
  S.status = st;
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
if (window.innerWidth <= 900) setPanel(false);
buildSliders();
syncVoiceUI();
refreshInfo();
measure();
requestAnimationFrame(frame);
boot().catch((e) => { console.error(e); flash("ERROR", String(e && e.message || e)); });
