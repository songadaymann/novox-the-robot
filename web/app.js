// Novox The Robot web demo. Plain ES module, no dependencies.
import { createSinger } from "./novox/index.js";

const $ = (id) => document.getElementById(id);
const SAMPLE_RATE = 44100;

// ---------------------------------------------------------------------------
// Built-in tunes. Everything here is original except "Twinkle Twinkle"
// (traditional, public domain).
const SONGS = [
  {
    name: "Hello, I'm a Singer",
    tempo: 100,
    lyrics: "hel-lo, I'm a new vir-tual sing-er _\ntype me a song and I will sing it for you _",
    notes: [
      "E4 0.5", "G4 1", "G4 0.5", "A4 0.5", "C5 1", "B4 0.5", "A4 0.5", "G4 0.75", "E4 0.25", "D4 1.5", "r 1",
      "E4 0.5", "G4 0.5", "A4 0.5", "C5 1", "A4 0.5", "G4 0.5", "A4 0.5", "C5 0.75", "D5 0.25", "E5 0.5", "D5 1", "C5 2",
    ],
  },
  {
    name: "Rain on the Window (waltz)",
    tempo: 132,
    lyrics: "rain on the win-dow _ soft as a sigh _\nclouds keep on roll-ing a-cross the sky",
    notes: [
      "E4 2", "A4 1", "B4 1", "C5 1.5", "B4 0.5", "A4 3",
      "E4 2", "A4 1", "B4 1", "C5 2", "B4 2", "r 1",
      "C5 2", "B4 1", "A4 1", "G4 1.5", "A4 0.5", "B4 1", "C5 1", "B4 1", "A4 3",
    ],
  },
  {
    name: "Robot Boogie",
    tempo: 120,
    lyrics: "beep boop, I am a ro-bot\nbeep boop, com-put-ing all day",
    notes: [
      "C4 0.5", "G4 0.5", "r 0.5", "E4 0.5", "E4 0.5", "G4 0.5", "A4 0.5", "G4 1", "r 0.5",
      "C5 0.5", "G4 0.5", "r 0.5", "A4 0.5", "G4 0.5", "E4 0.5", "D4 0.5", "C4 1.5",
    ],
  },
  {
    name: "Twinkle Twinkle (traditional)",
    tempo: 104,
    lyrics: "twin-kle twin-kle lit-tle star\nhow I won-der what you are",
    notes: [
      "C4 1", "C4 1", "G4 1", "G4 1", "A4 1", "A4 1", "G4 2",
      "F4 1", "F4 1", "E4 1", "E4 1", "D4 1", "D4 1", "C4 2",
    ],
  },
  {
    name: "Five-Note Scale (vowel check)",
    tempo: 90,
    lyrics: "la le li lo lu _ _ _ _",
    notes: ["C4 1", "D4 1", "E4 1", "F4 1", "G4 1", "F4 1", "E4 1", "D4 1", "C4 2"],
  },
];

// ---------------------------------------------------------------------------
// Voice presets: face + blurb for the gallery, and the preset's values for the
// sliders (mirrors engine/src/voice.cpp; only the six fields the sliders use).
const BASE = { formantScale: 1, vibratoDepth: 0.35, portamento: 0.2, breathiness: 0.01, grit: 0, choirSize: 1 };
const VOICES = {
  std: { n: "Std Voice", d: "The house voice", face: { skin: "#f2c7a0", hair: "#7a5230" } },
  robot: { n: "Robot", d: "Pressed, pitch-perfect, no vibrato", face: { robot: 1 }, v: { vibratoDepth: 0, portamento: 0.02, breathiness: 0 } },
  male: { n: "Male", d: "Plain adult male", face: { skin: "#e8b58c", hair: "#3a2616" } },
  female: { n: "Female", d: "Brighter formants, softer source", face: { skin: "#f6cfb0", hair: "#a0522d", style: "long", blush: 1 } },
  kid: { n: "Kid", d: "Small and bright", face: { skin: "#ffd9b8", hair: "#e0a030", kid: 1, blush: 1 }, v: { vibratoDepth: 0.12 } },
  soprano: { n: "Soprano", d: "High, rich, late vibrato", face: { skin: "#f3c8a8", hair: "#2a1a12", style: "bun", blush: 1 }, v: { formantScale: 1.2, vibratoDepth: 0.45, breathiness: 0.03 } },
  alto: { n: "Alto", d: "Warm and round", face: { skin: "#c98e66", hair: "#1c120c", style: "long" }, v: { formantScale: 1.12 } },
  tenor: { n: "Tenor", d: "Ringing, forward", face: { skin: "#e6b089", hair: "#4a2e1a" }, v: { formantScale: 1.04, vibratoDepth: 0.33 } },
  bass: { n: "Bass", d: "Big and low", face: { skin: "#d9a07a", hair: "#555", style: "bald", beard: "#444" }, v: { formantScale: 0.9, vibratoDepth: 0.25 } },
  gregorian: { n: "Gregorian", d: "Six monks, no vibrato", face: { skin: "#e3b692", hood: "#6b4a2b" }, v: { formantScale: 0.97, vibratoDepth: 0, portamento: 0.12, choirSize: 6 } },
  choir: { n: "Choir", d: "Eight voices together", group: [{ skin: "#e8b58c", hair: "#3a2616" }, { skin: "#d9a07a", hair: "#555", style: "bald" }, { skin: "#eab896", hair: "#b86b2a" }], v: { vibratoDepth: 0.25, choirSize: 8 } },
  "female-choir": { n: "Female Choir", d: "Eight airy sopranos", group: [{ skin: "#f6cfb0", hair: "#a0522d", style: "long" }, { skin: "#c98e66", hair: "#1c120c", style: "long" }, { skin: "#f3c8a8", hair: "#2a1a12", style: "bun" }], v: { formantScale: 1.17, breathiness: 0.07, choirSize: 8 } },
  karaoke: { n: "Karaoke", d: "Big scoops, bright tone", face: { skin: "#f5d0b0", hair: "#e0457b", style: "mohawk", blush: 1 }, v: { vibratoDepth: 0.18 } },
  "quick-vibrato": { n: "Quick Vibrato", d: "Fast, nervous shimmer", face: { skin: "#f0c29c", hair: "#6b3b8a", style: "mohawk" }, v: { vibratoDepth: 0.4 } },
  hifi: { n: "Hi-Fi", d: "Full bandwidth, modern", face: { skin: "#eab896", hair: "#2b2b2b", phones: 1, glasses: 1 }, v: { breathiness: 0.05 } },
  crooner: { n: "Crooner", d: "Breathy, slow, swoopy", face: { skin: "#e9bb95", hair: "#141414", style: "slick" }, v: { formantScale: 0.95, breathiness: 0.12, portamento: 0.12 } },
};
const presetValues = (name) => ({ ...BASE, ...(VOICES[name]?.v || {}) });

const SLIDERS = [
  { key: "formantScale", label: "Size", min: 0.85, max: 1.35, step: 0.01, ends: ["big", "small"], fmt: (v) => "×" + v.toFixed(2) },
  { key: "vibratoDepth", label: "Vibrato", min: 0, max: 0.8, step: 0.01, fmt: (v) => Math.round(v * 100) + " ¢" },
  { key: "portamento", label: "Glide", min: 0.02, max: 0.4, step: 0.01, fmt: (v) => Math.round(v * 1000) + " ms" },
  { key: "breathiness", label: "Breath", min: 0, max: 0.2, step: 0.005, fmt: (v) => Math.round(v * 500) + "%" },
  { key: "grit", label: "Grit", min: 0, max: 0.6, step: 0.01, fmt: (v) => Math.round((v / 0.6) * 100) + "%" },
  { key: "choirSize", label: "Choir", min: 1, max: 8, step: 1, fmt: (v) => (v === 1 ? "solo" : v + " voices") },
];

// ---------------------------------------------------------------------------
// Cartoon faces (inline SVG), after the Aqua voice-designer mockup.
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
function setMouth(ph) {
  const [rx, ry] = MOUTH[ph] || (ph ? [5, 2.5] : [4, 1.5]);
  document.querySelectorAll("#bigface .mouth").forEach((m) => { m.setAttribute("rx", rx); m.setAttribute("ry", ry); });
}

// ---------------------------------------------------------------------------
// Notes: compact text <-> list of {start, dur, midi} in seconds.
const STEPS = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
function parsePitch(p) {
  if (typeof p === "number") return p;
  const m = /^([A-Ga-g])([#b]*)(-?\d+)$/.exec(String(p).trim());
  if (!m) return NaN;
  let n = STEPS[m[1].toLowerCase()] + 12 * (+m[3] + 1);
  for (const c of m[2]) n += c === "#" ? 1 : -1;
  return n;
}
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const pitchName = (m) => NAMES[((Math.round(m) % 12) + 12) % 12] + (Math.floor(Math.round(m) / 12) - 1);
const noteTokens = (text) => text.split(/[,;\n]+/).map((s) => s.trim()).filter(Boolean);

function notesFromText(text, tempo) {
  const spb = 60 / tempo, notes = [];
  let t = 0;
  for (const tok of noteTokens(text)) {
    const [p, d] = tok.split(/\s+/);
    const beats = parseFloat(d);
    if (!(beats > 0)) throw new Error(`“${tok}” needs a length in beats, like “C4 1”`);
    if (!/^r(est)?$/i.test(p)) {
      const midi = parsePitch(p);
      if (Number.isNaN(midi)) throw new Error(`“${p}” isn't a pitch (try C4, F#3, Bb4 or r for a rest)`);
      notes.push({ start: t, dur: beats * spb, midi, beat: t / spb, beats });
    }
    t += beats * spb;
  }
  return notes;
}
const fmtNum = (x) => String(Math.round(x * 1000) / 1000);
function formatNotes(list) {
  let out = "", line = [];
  list.forEach((tok, i) => {
    line.push(tok);
    const nextIsRest = /^r /.test(list[i + 1] || "");
    if (/^r /.test(tok) || (line.length >= 8 && !nextIsRest)) { out += line.join(", ") + ",\n"; line = []; }
  });
  return (out + line.join(", ")).replace(/,\n$/, "");
}

// ---------------------------------------------------------------------------
// State + UI
let singer = null, audioCtx = null;
let voiceName = "std";
const overrides = {};
let current = null; // { score, key, notes, plan, labels, error }
let cache = { key: "", samples: null };
let playing = null; // { src, t0, lead, dur }
let selected = -1, hovered = -1;

const els = {
  lyrics: $("lyrics"), notes: $("notes"), tempo: $("tempo"), song: $("song"),
  sing: $("sing"), stop: $("stop"), download: $("download"), file: $("file"),
  grid: $("grid"), keys: $("keys"), scroll: $("rollscroll"), info: $("noteInfo"), warn: $("warn"),
  lcd: $("lcd"), lcdState: $("lcdState"), lcdTime: $("lcdTime"), lcdLine: $("lcdLine"),
};

function lcd(state, line, isErr = false) {
  els.lcdState.textContent = state;
  if (line != null) els.lcdLine.textContent = line;
  els.lcd.classList.toggle("err", isErr);
}
const fmtTime = (s) => { s = Math.max(0, s); const m = Math.floor(s / 60); return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`; };

// song menu
SONGS.forEach((s, i) => els.song.add(new Option(s.name, i)));
els.song.add(new Option("Custom", "custom"));
function loadSong(i) {
  const s = SONGS[i];
  els.lyrics.value = s.lyrics;
  els.tempo.value = s.tempo;
  els.notes.value = formatNotes(s.notes);
  els.song.value = String(i);
  selected = -1;
  update();
}
els.song.addEventListener("change", () => { if (els.song.value !== "custom") loadSong(+els.song.value); });

// voice gallery
function buildGallery() {
  const g = $("gallery");
  g.innerHTML = "";
  for (const name of singer.voices()) {
    const v = VOICES[name] || { n: name, d: "", face: { skin: "#f2c7a0", hair: "#666" } };
    const b = document.createElement("button");
    b.className = "card";
    b.setAttribute("role", "radio");
    b.setAttribute("aria-checked", name === voiceName);
    b.title = v.d;
    b.innerHTML = voiceSvg(v, 40) + `<span>${v.n}</span>`;
    b.onclick = () => chooseVoice(name);
    g.appendChild(b);
  }
}
function chooseVoice(name) {
  voiceName = name;
  for (const k in overrides) delete overrides[k];
  const v = VOICES[name] || { n: name, d: "" };
  [...$("gallery").children].forEach((c, i) => c.setAttribute("aria-checked", singer.voices()[i] === name));
  $("bigface").innerHTML = voiceSvg(v, 84);
  $("vname").textContent = v.n;
  $("vsub").textContent = v.d;
  syncSliders();
  update();
}

// sliders
const sliderEls = {};
function buildSliders() {
  const host = $("sliders");
  for (const s of SLIDERS) {
    const row = document.createElement("div");
    row.className = "sl";
    row.innerHTML = `<label for="sl-${s.key}">${s.label}</label><input type="range" id="sl-${s.key}" min="${s.min}" max="${s.max}" step="${s.step}"><output></output>` +
      (s.ends ? `<div class="ends"><span>${s.ends[0]}</span><span>${s.ends[1]}</span></div>` : "");
    const input = row.querySelector("input"), out = row.querySelector("output");
    input.addEventListener("input", () => {
      const v = +input.value;
      overrides[s.key] = s.key === "choirSize" ? Math.round(v) : v;
      out.textContent = s.fmt(v);
      row.classList.add("touched");
      markModified();
      update();
    });
    input.addEventListener("dblclick", () => { delete overrides[s.key]; syncSliders(); update(); });
    sliderEls[s.key] = { row, input, out, s };
    host.appendChild(row);
  }
}
function syncSliders() {
  const pv = presetValues(voiceName);
  for (const k in sliderEls) {
    const { row, input, out, s } = sliderEls[k];
    const v = k in overrides ? overrides[k] : pv[k];
    input.value = v;
    out.textContent = s.fmt(v);
    row.classList.toggle("touched", k in overrides);
  }
  markModified();
}
function markModified() {
  const n = Object.keys(overrides).length;
  $("modified").textContent = n ? `${n} change${n > 1 ? "s" : ""} on top of ${VOICES[voiceName]?.n || voiceName}` : "Preset settings";
}
$("reset").onclick = () => { for (const k in overrides) delete overrides[k]; syncSliders(); update(); };

// ---------------------------------------------------------------------------
// Score building + planning
function buildScore() {
  const tempo = Math.min(300, Math.max(30, +els.tempo.value || 100));
  const voice = Object.keys(overrides).length ? { preset: voiceName, ...overrides } : voiceName;
  return {
    tempo, voice,
    lyrics: els.lyrics.value.replace(/\s+/g, " ").trim(),
    notes: noteTokens(els.notes.value),
  };
}

function update() {
  if (!singer) return;
  const score = buildScore();
  const key = JSON.stringify(score);
  const res = { score, key, notes: [], plan: null, labels: [], error: null };
  try {
    res.notes = notesFromText(els.notes.value, score.tempo);
    if (!res.notes.length) throw new Error("Add some notes to sing.");
    res.plan = singer.plan(score);
    res.labels = labelNotes(res.notes, res.plan.syllables);
  } catch (e) {
    res.error = e.message || String(e);
  }
  current = res;
  if (selected >= res.notes.length) selected = -1;
  els.sing.disabled = !!res.error;
  els.download.disabled = !!res.error;
  if (res.error) {
    if (!playing) lcd("CAN'T SING THAT", res.error, true);
  } else if (!playing) {
    lcd("READY", `${VOICES[voiceName]?.n || voiceName} · ${score.tempo} bpm · ${fmtTime(res.plan.length)}`);
    els.lcdTime.textContent = fmtTime(0);
  }
  const w = res.plan?.warnings || [];
  els.warn.hidden = !w.length;
  els.warn.textContent = w.length ? (w.length === 1 ? w[0] : `${w.length} guesses: ${w[0]} …`) : "";
  els.warn.title = w.join("\n");
  const sung = res.labels.filter((l) => l.kind === "word" || l.kind === "cont").length;
  $("counts").textContent = res.error ? "" : `${res.notes.length} notes · ${sung} syllables sung`;
  showInfo();
  drawRoll();
}
let pending = 0;
const scheduleUpdate = () => { clearTimeout(pending); pending = setTimeout(update, 200); };
els.lyrics.addEventListener("input", scheduleUpdate);
els.notes.addEventListener("input", () => { els.song.value = "custom"; scheduleUpdate(); });
els.tempo.addEventListener("input", () => { els.song.value = "custom"; scheduleUpdate(); });

// Map each note to what's sung on it.
function labelNotes(notes, syls) {
  const eps = 0.002;
  return notes.map((n) => {
    let i = syls.findIndex((s) => Math.abs(s.start - n.start) < eps);
    if (i >= 0) {
      const s = syls[i], prev = syls[i - 1];
      const phon = s.phonemes.replace(/\|/g, " ").replace(/\s+/g, " ").trim();
      const vowel = (s.phonemes.split("|")[1] || "").trim();
      if (s.text) return { kind: "word", text: s.text + (s.continues ? "-" : ""), syl: s, phon, vowel };
      if (prev && prev.continues) return { kind: "cont", text: "-" + phon.toLowerCase().replace(/ /g, ""), syl: s, phon, vowel };
      return { kind: "hum", text: "mm", syl: s, phon: phon || "M", vowel: "M" };
    }
    i = syls.findIndex((s) => s.start < n.start + eps && s.end > n.start + eps);
    if (i >= 0) {
      const s = syls[i];
      const vowel = (s.phonemes.split("|")[1] || "").trim();
      return { kind: "hold", text: "‿", syl: s, phon: vowel, vowel };
    }
    return { kind: "hum", text: "mm", syl: null, phon: "M", vowel: "M" };
  });
}

function showInfo() {
  const el = els.info;
  el.classList.toggle("error", !!current?.error);
  if (current?.error) { el.textContent = current.error; return; }
  const i = hovered >= 0 ? hovered : selected;
  if (i < 0 || !current.labels[i]) { el.textContent = "Hover or tap a note to see its phonemes."; return; }
  const n = current.notes[i], l = current.labels[i];
  const what = { word: `“${l.text}”`, cont: "rest of the word", hold: `holding “${l.syl?.text || l.vowel}”`, hum: "hummed" }[l.kind];
  el.innerHTML = `<b>${pitchName(n.midi)}</b> · ${fmtNum(n.beats)} beat${n.beats === 1 ? "" : "s"} · ${escapeHtml(what)} · <span class="ph">${escapeHtml(l.phon || "–")}</span>`;
}
const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

// ---------------------------------------------------------------------------
// Piano roll (canvas)
const KEYS_W = 40, PAD = 10;
let geo = null; // { lo, hi, rowH, pps, w, h }

function drawRoll(now = null) {
  const cv = els.grid, kc = els.keys, dpr = window.devicePixelRatio || 1;
  const viewW = els.scroll.clientWidth;
  const notes = current?.notes || [];
  const total = Math.max(current?.plan?.length || 0, ...notes.map((n) => n.start + n.dur), 1);
  let lo = Math.min(...notes.map((n) => n.midi), 60), hi = Math.max(...notes.map((n) => n.midi), 72);
  lo = Math.floor(lo) - 1; hi = Math.ceil(hi) + 1;
  while (hi - lo < 14) { hi++; if (hi - lo < 14) lo--; }
  // Rows at least 18px tall (roll grows up to 420px, then rows shrink);
  // at least 80px per beat so syllables fit (scrolls sideways if needed).
  const narrow = window.innerWidth < 760;
  const rows = hi - lo + 1, minH = narrow ? 200 : 260;
  const rowH = Math.max(Math.min(420, Math.max(minH, rows * (narrow ? 14 : 18))) / rows, 6);
  const H = Math.round(rows * rowH);
  const sbar = els.scroll.offsetHeight - els.scroll.clientHeight; // classic scrollbars
  $("roll").style.height = H + 2 + sbar + "px";
  const spbFit = 60 / (current?.score?.tempo || 100);
  const pps = Math.max((viewW - PAD * 2) / total, (narrow ? 60 : 80) / spbFit);
  const W = Math.max(viewW, Math.ceil(total * pps + PAD * 2));
  geo = { lo, hi, rowH, pps, W, H };
  const yOf = (m) => (hi - m) * rowH;
  const xOf = (t) => PAD + t * pps;

  // keys
  kc.width = KEYS_W * dpr; kc.height = H * dpr; kc.style.height = H + "px";
  let k = kc.getContext("2d");
  k.setTransform(dpr, 0, 0, dpr, 0, 0);
  for (let m = lo; m <= hi; m++) {
    const black = [1, 3, 6, 8, 10].includes(((m % 12) + 12) % 12), y = yOf(m);
    const g = k.createLinearGradient(0, 0, KEYS_W, 0);
    if (black) { g.addColorStop(0, "#1a1a1a"); g.addColorStop(1, "#4a4a4a"); } else { g.addColorStop(0, "#d9d9d9"); g.addColorStop(0.7, "#fff"); g.addColorStop(1, "#e8e8e8"); }
    const active = now && now.midi === m;
    k.fillStyle = active ? "#5aa8f4" : g;
    k.fillRect(0, y, KEYS_W, rowH);
    k.fillStyle = "#9a9a9a"; k.fillRect(0, y + rowH - 0.5, KEYS_W, 0.5);
    if (m % 12 === 0 && rowH >= 8) {
      k.fillStyle = "#333"; k.font = `${Math.min(10, rowH - 1)}px ${getComputedStyle(document.body).fontFamily}`;
      k.textAlign = "right"; k.textBaseline = "middle"; k.fillText(pitchName(m), KEYS_W - 4, y + rowH / 2);
    }
  }

  // grid
  cv.width = W * dpr; cv.height = H * dpr; cv.style.width = W + "px"; cv.style.height = H + "px";
  const c = cv.getContext("2d");
  c.setTransform(dpr, 0, 0, dpr, 0, 0);
  c.fillStyle = "#fff"; c.fillRect(0, 0, W, H);
  for (let m = lo; m <= hi; m++) {
    const y = yOf(m);
    if ([1, 3, 6, 8, 10].includes(((m % 12) + 12) % 12)) { c.fillStyle = "#eef2f8"; c.fillRect(0, y, W, rowH); }
    c.fillStyle = m % 12 === 0 ? "#c9d3e0" : "#e3e8ef"; c.fillRect(0, y + rowH - 0.5, W, 0.5);
  }
  const tempo = current?.score?.tempo || 100, spb = 60 / tempo;
  for (let b = 0, t = 0; t <= total + 1e-6; b++, t = b * spb) {
    c.fillStyle = b % 4 === 0 ? "#b7c2d0" : "#e6eaf0";
    c.fillRect(Math.round(xOf(t)), 0, 1, H);
    if (b % 4 === 0 && pps * spb * 4 > 40) { c.fillStyle = "#8a96a6"; c.font = "10px " + getComputedStyle(document.body).fontFamily; c.textAlign = "left"; c.textBaseline = "top"; c.fillText(String(b / 4 + 1), xOf(t) + 3, 2); }
  }

  // notes
  const font = getComputedStyle(document.body).fontFamily;
  const nh = Math.max(rowH - 1, 6), r = Math.min(5, nh / 2);
  notes.forEach((n, i) => {
    const l = current.labels[i] || { kind: "hum", text: "" };
    const x = xOf(n.start) + 0.5, y = yOf(n.midi) + 0.5, w = Math.max(n.dur * pps - 2, 4);
    const isNow = now && now.index === i, isSel = i === selected || i === hovered;
    const g = c.createLinearGradient(0, y, 0, y + nh);
    if (l.kind === "hum") { g.addColorStop(0, "#f2f2f2"); g.addColorStop(0.5, "#cfd4da"); g.addColorStop(0.52, "#bcc3cb"); g.addColorStop(1, "#e2e6ea"); }
    else if (l.kind === "hold") { g.addColorStop(0, "#e2f0ff"); g.addColorStop(0.5, "#a8d0fa"); g.addColorStop(0.52, "#8cbff5"); g.addColorStop(1, "#d3e8ff"); }
    else if (isNow) { g.addColorStop(0, "#fff2c0"); g.addColorStop(0.48, "#ffc53d"); g.addColorStop(0.52, "#f0a800"); g.addColorStop(1, "#ffe28a"); }
    else { g.addColorStop(0, "#bfe0ff"); g.addColorStop(0.48, "#5aa8f4"); g.addColorStop(0.52, "#2f84e3"); g.addColorStop(1, "#9fd2ff"); }
    c.save();
    if (isNow) { c.shadowColor = "rgba(255,200,40,.95)"; c.shadowBlur = 14; }
    c.beginPath(); c.roundRect(x, y, w, nh - 1, r); c.fillStyle = g; c.fill();
    c.restore();
    c.lineWidth = isSel ? 2 : 1;
    c.strokeStyle = isSel ? "#0b3e86" : l.kind === "hum" ? "#8a939c" : "rgba(0,40,110,.55)";
    c.beginPath(); c.roundRect(x, y, w, nh - 1, r); c.stroke();
    // gloss
    c.beginPath(); c.roundRect(x + 3, y + 1, Math.max(w - 6, 1), (nh - 1) * 0.42, [r, r, 2, 2]);
    c.fillStyle = "rgba(255,255,255,.55)"; c.fill();
    // tie from previous note for held vowels
    if (l.kind === "hold" && i > 0) {
      const p = notes[i - 1];
      c.strokeStyle = "#2f6fb8"; c.lineWidth = 1.2; c.setLineDash([3, 2]);
      c.beginPath(); c.moveTo(xOf(p.start + p.dur) - 3, yOf(p.midi) + nh / 2); c.lineTo(x + 3, y + nh / 2); c.stroke(); c.setLineDash([]);
    }
    // label
    const fs = Math.max(9, Math.min(13, nh - 3));
    c.font = `${l.kind === "word" ? "bold " : l.kind === "hum" ? "italic " : ""}${fs}px ${font}`;
    c.textBaseline = "middle"; c.textAlign = "left";
    const label = l.text;
    const inside = c.measureText(label).width + 8 <= w;
    c.fillStyle = isNow ? "#3a2400" : l.kind === "hum" ? "#4a525a" : "#001a3a";
    if (inside || nh < 11) {
      c.save(); c.beginPath(); c.rect(x, y - 2, w, nh + 4); c.clip();
      c.fillText(label, x + 4, y + nh / 2); c.restore();
    } else {
      // too narrow: write the syllable just above the note
      c.fillStyle = "#1f4f8e"; c.fillText(label, x, y - fs / 2 - 1);
    }
  });

  // playhead
  if (now) {
    const x = xOf(now.t);
    c.fillStyle = "rgba(224,50,31,.25)"; c.fillRect(x - 3, 0, 7, H);
    c.fillStyle = "#e0321f"; c.fillRect(x - 0.5, 0, 1.5, H);
    c.beginPath(); c.moveTo(x - 5, 0); c.lineTo(x + 6, 0); c.lineTo(x + 0.5, 7); c.fill();
  }
}
window.addEventListener("resize", () => drawRoll(playing ? nowAt() : null));

function noteAt(ev) {
  if (!geo || !current) return -1;
  const rect = els.grid.getBoundingClientRect();
  const x = ev.clientX - rect.left, y = ev.clientY - rect.top;
  const t = (x - PAD) / geo.pps;
  const m = geo.hi - Math.floor(y / geo.rowH);
  let best = -1;
  current.notes.forEach((n, i) => { if (t >= n.start && t <= n.start + n.dur && Math.round(n.midi) === m) best = i; });
  if (best < 0) current.notes.forEach((n, i) => { if (t >= n.start && t <= n.start + n.dur) best = i; });
  return best;
}
els.grid.addEventListener("mousemove", (e) => { const i = noteAt(e); if (i !== hovered) { hovered = i; showInfo(); if (!playing) drawRoll(); } });
els.grid.addEventListener("mouseleave", () => { hovered = -1; showInfo(); if (!playing) drawRoll(); });
els.grid.addEventListener("click", (e) => { selected = noteAt(e); showInfo(); if (!playing) drawRoll(); });

// ---------------------------------------------------------------------------
// Rendering + playback
// Let the LCD repaint before the (synchronous) render. Not rAF: it stalls in background tabs.
const nextFrame = () => new Promise((r) => setTimeout(r, 30));
async function renderCurrent() {
  if (cache.key === current.key) return cache.samples;
  lcd("RENDERING…", "the singer is taking a breath");
  await nextFrame();
  const t0 = performance.now();
  const samples = singer.render(current.score);
  cache = { key: current.key, samples };
  $("engineStatus").textContent = `Engine ready · rendered ${fmtTime(samples.length / singer.sampleRate)} in ${Math.round(performance.now() - t0)} ms`;
  return samples;
}

async function sing() {
  if (!current || current.error) return;
  stop();
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  if (audioCtx.state === "suspended") await audioCtx.resume();
  let samples;
  try { samples = await renderCurrent(); }
  catch (e) { lcd("CAN'T SING THAT", e.message, true); return; }
  if (!samples.length) { lcd("NOTHING TO SING", "the render came back empty", true); return; }
  const buf = audioCtx.createBuffer(1, samples.length, singer.sampleRate);
  buf.copyToChannel(samples, 0);
  const src = audioCtx.createBufferSource();
  src.buffer = buf;
  const gain = audioCtx.createGain();
  gain.gain.value = 0.85;
  src.connect(gain).connect(audioCtx.destination);
  const t0 = audioCtx.currentTime + 0.05;
  src.start(t0);
  playing = { src, t0, lead: current.plan.lead, dur: buf.duration, notes: current.notes, labels: current.labels };
  src.onended = () => { if (playing && playing.src === src) finish(); };
  els.sing.classList.add("on");
  els.stop.disabled = false;
  requestAnimationFrame(tick);
}
function nowAt() {
  const p = playing;
  const at = audioCtx.currentTime - p.t0;
  const t = at - p.lead;
  const index = p.notes.findIndex((n) => t >= n.start && t < n.start + n.dur);
  return { at, t, index, midi: index >= 0 ? p.notes[index].midi : null };
}
function tick() {
  if (!playing) return;
  const now = nowAt();
  drawRoll(now);
  els.lcdTime.textContent = `${fmtTime(now.at)} / ${fmtTime(playing.dur)}`;
  const l = now.index >= 0 ? playing.labels[now.index] : null;
  lcd("SINGING ♪", l ? (l.kind === "word" ? `♪ ${l.text}` : l.kind === "hum" ? "♪ mmm…" : `♪ ${l.syl?.text || ""} ~`) : "♪");
  setMouth(l ? l.vowel : null);
  // keep the playhead in view
  const x = PAD + Math.max(0, now.t) * geo.pps, sc = els.scroll;
  if (x > sc.scrollLeft + sc.clientWidth * 0.8 || x < sc.scrollLeft) sc.scrollLeft = Math.max(0, x - sc.clientWidth * 0.2);
  requestAnimationFrame(tick);
}
function stop() {
  if (!playing) return;
  const p = playing;
  playing = null;
  try { p.src.onended = null; p.src.stop(); } catch {}
  finish(true);
}
function finish(stopped = false) {
  playing = null;
  els.sing.classList.remove("on");
  els.stop.disabled = true;
  setMouth(null);
  update();
  if (!current.error) lcd(stopped ? "STOPPED" : "DONE ✓", `${VOICES[voiceName]?.n || voiceName} · press Sing to go again`);
}
els.sing.onclick = sing;
els.stop.onclick = stop;
document.addEventListener("keydown", (e) => {
  if (e.code !== "Space" || e.target.closest("textarea, input, select, button, label")) return;
  e.preventDefault();
  playing ? stop() : sing();
});

els.download.onclick = async () => {
  if (!current || current.error) return;
  let samples;
  try { samples = await renderCurrent(); } catch (e) { lcd("CAN'T SING THAT", e.message, true); return; }
  const blob = new Blob([singer.toWav(samples)], { type: "audio/wav" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  const name = (els.song.value === "custom" ? "my-song" : SONGS[+els.song.value].name).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
  a.download = `${name}-${voiceName}.wav`;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  if (!playing) lcd("SAVED", `${a.download} · ${(blob.size / 1024 / 1024).toFixed(1)} MB`);
};

// ---------------------------------------------------------------------------
// MIDI / MusicXML import
const VOWELS = new Set(["IY", "IH", "EH", "AE", "AA", "AO", "UH", "UW", "AH", "AX", "ER", "EY", "AY", "AW", "OW", "OY"]);
// The file's lyrics as a line that flows back one syllable per note: "hel-" "lo" joins as "hel-lo",
// and a one-note word the flow would split by its vowels gets the one-note mark ("fire~").
function lyricLine(words) {
  let open = false;  // the last syllable left its word open ("hel-")
  return words.map((w) => {
    if (w === "_") return w;
    const fresh = !open && !w.endsWith("-") && !w.includes("~") && !w.startsWith("{");
    open = w.endsWith("-");
    const vowels = fresh ? singer.phonemize(w).split(/\s+/).filter((p) => VOWELS.has(p)).length : 0;
    return vowels > 1 ? w + "~" : w;
  }).join(" ").replace(/- /g, "-");
}
function applyImported(score, fileName) {
  // The file's first tempo, unrounded (66.67 stays 66.67): placing notes by time at a rounded tempo
  // would drift from the score.
  const tempo = +score.tempo > 0 ? +score.tempo : 120;
  const toBeats = (sec) => (score.units === "seconds" ? (sec * tempo) / 60 : sec);
  const notes = [...score.notes].map((n) => (typeof n === "string" ? null : n)).filter(Boolean);
  let t = 0;
  notes.forEach((n) => { if (n.start == null) n.start = t; t = n.start + n.dur; });
  notes.sort((a, b) => a.start - b.start);
  const toks = [], words = [];
  let pos = 0;
  for (const n of notes) {
    const start = toBeats(n.start), dur = toBeats(n.dur);
    if (start - pos > 0.01) toks.push(`r ${fmtNum(start - pos)}`);
    toks.push(`${typeof n.pitch === "number" ? pitchName(n.pitch) : n.pitch} ${fmtNum(dur)}`);
    pos = Math.max(pos, start + dur);
    if (n.lyric) words.push(n.lyric);
    else if (words.length) words.push("_"); // keep later lyrics aligned
  }
  if (!toks.length) throw new Error("No notes found in that file.");
  if (words.some((w) => w !== "_")) els.lyrics.value = lyricLine(words);
  els.tempo.value = fmtNum(tempo);
  els.notes.value = formatNotes(toks);
  const opt = [...els.song.options].find((o) => o.value === "custom");
  opt.textContent = `Custom (from ${fileName})`;
  els.song.value = "custom";
  selected = -1;
  update();
}
async function importBytes(bytes, fileName) {
  try {
    const score = singer.importFile(bytes);
    applyImported(score, fileName);
    // The importer's warnings (other parts and voices, tempo changes...): the first on the LCD, all in its tooltip.
    const warnings = (Array.isArray(score.importWarnings) ? score.importWarnings : []).map((w) => String(w).replace(/\s*\(choose with \w+\)|;\s*choose with \w+/g, ""));
    const changes = Array.isArray(score.import?.tempoMap) && score.import.tempoMap.length > 1;
    if (changes) warnings.unshift(`the tempo changes: notes are placed by time at the first tempo, ${fmtNum(+score.tempo)} bpm`);
    els.lcd.title = warnings.join("\n");
    if (!playing && !current.error) lcd("IMPORTED", warnings.length ? `${fileName} · ${warnings[0]}` : `${fileName} · ${current.notes.length} notes`);
  } catch (e) {
    lcd("COULDN'T READ FILE", `${fileName}: ${e.message}`, true);
  }
}
els.file.addEventListener("change", async () => {
  const f = els.file.files[0];
  if (f) await importBytes(new Uint8Array(await f.arrayBuffer()), f.name);
  els.file.value = "";
});
$("openLbl").addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); els.file.click(); } });
const win = $("win");
let dragDepth = 0;
win.addEventListener("dragenter", (e) => { if ([...e.dataTransfer.types].includes("Files")) { dragDepth++; win.classList.add("dragging"); } });
win.addEventListener("dragleave", () => { if (--dragDepth <= 0) { dragDepth = 0; win.classList.remove("dragging"); } });
win.addEventListener("dragover", (e) => e.preventDefault());
win.addEventListener("drop", async (e) => {
  e.preventDefault(); dragDepth = 0; win.classList.remove("dragging");
  const f = e.dataTransfer.files[0];
  if (f && singer) await importBytes(new Uint8Array(await f.arrayBuffer()), f.name);
});

// ---------------------------------------------------------------------------
// Boot
async function boot() {
  loadSongFields(0);
  try {
    singer = await createSinger({ sampleRate: SAMPLE_RATE });
  } catch (e) {
    lcd("ENGINE FAILED", e.message || String(e), true);
    $("engineStatus").textContent = "Engine failed to load";
    return;
  }
  $("engineStatus").textContent = `Engine ready · formant synthesis · ${SAMPLE_RATE / 1000} kHz`;
  buildGallery();
  buildSliders();
  chooseVoice("std");
  loadSong(0);
  // expose for tinkering from the console
  window.novox = { singer, buildScore, get current() { return current; }, importBytes, sing, stop };
}
function loadSongFields(i) {
  const s = SONGS[i];
  els.lyrics.value = s.lyrics; els.tempo.value = s.tempo; els.notes.value = formatNotes(s.notes); els.song.value = String(i);
}
boot();
