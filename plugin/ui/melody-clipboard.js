// The editor's cross-instance melody clipboard. Beats are absolute: a phrase copied at bar 9
// stays at bar 9, even when the other instance has a different voice or host tempo. This module
// has no DOM or native clipboard access and never changes either song it is given.
import { lyricsFromNotes, pinnedLyric } from "./lyric-text.js";
import { songTranscriptions } from "./song-merge.js";

export const MELODY_CLIPBOARD_FORMAT = "novox.melody";
export const MELODY_CLIPBOARD_VERSION = 1;
export const MAX_CLIPBOARD_BYTES = 2 * 1024 * 1024;
export const MAX_CLIPBOARD_NOTES = 10000;
export const MIN_CLIPBOARD_NOTE_BEATS = 1 / 64; // normSong's persisted minimum
const MAX_BEATS = 1000000, MAX_TEXT = 2048, EPS = 1e-6;
const PHONEMES = new Set("IY IH EH AE AA AO UH UW AH AX ER EY AY AW OW OY P B T D K G S Z SH ZH F V TH DH CH JH M N NG L R W Y HH".split(" "));
const byTime = (a, b) => a.start - b.start || a.pitch - b.pitch;
const plain = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const isHold = (t) => t === "_" || t === "+";
const explicit = (t) => t.startsWith("{") && t.endsWith("}");
const openWord = (t) => t.endsWith("-") && !explicit(t);
const normalizeWord = (s) => s.toLowerCase().replace(/[^a-z']/g, "").replace(/^'+|'+$/g, "");
const bytes = (s) => new TextEncoder().encode(s).length;
const fail = (message) => { throw new Error(message); };
const ownKeys = (value, allowed) => Object.keys(value).every((key) => allowed.includes(key));

export class MelodyPasteConflictError extends Error {
  constructor(count) {
    super(`Pasting would replace part of ${count} existing note${count === 1 ? "" : "s"}.`);
    this.name = "MelodyPasteConflictError";
    this.count = count;
  }
}

function validNote(n) {
  return plain(n) && Number.isInteger(n.pitch) && n.pitch >= 0 && n.pitch <= 127 &&
    typeof n.start === "number" && Number.isFinite(n.start) && n.start >= 0 &&
    typeof n.dur === "number" && Number.isFinite(n.dur) && n.dur >= MIN_CLIPBOARD_NOTE_BEATS &&
    n.start + n.dur <= MAX_BEATS && n.start + n.dur > n.start;
}

function textField(v) { return typeof v === "string" && v.length <= MAX_TEXT && !/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(v); }
function phonemeField(v) {
  return textField(v) && v.split(/\s+/).filter(Boolean).every((p) => /^[A-Z]+[0-2]?$/.test(p) && PHONEMES.has(p.replace(/[0-2]$/, "")));
}

// Only the fields needed to paste music can cross the process boundary. In particular no note
// IDs, voice/tempo settings, recording paths or detection evidence are accepted from a clipboard.
export function parseMelodyClipboard(text) {
  if (typeof text !== "string" || text.length > MAX_CLIPBOARD_BYTES || bytes(text) > MAX_CLIPBOARD_BYTES)
    fail("The clipboard melody is too large or is not text.");
  let value;
  try { value = JSON.parse(text); } catch { fail("The clipboard does not contain a Novox melody."); }
  if (!plain(value) || value.format !== MELODY_CLIPBOARD_FORMAT || value.version !== MELODY_CLIPBOARD_VERSION ||
      value.units !== "beats" || !ownKeys(value, ["format", "version", "units", "notes", "groups"]))
    fail("The clipboard does not contain a supported Novox melody.");
  if (!Array.isArray(value.notes) || !value.notes.length || value.notes.length > MAX_CLIPBOARD_NOTES ||
      !Array.isArray(value.groups) || !value.groups.length || value.groups.length > value.notes.length)
    fail("The clipboard melody has an invalid number of notes.");
  const groups = value.groups.map((g) => {
    if (!plain(g) || !ownKeys(g, ["word", "pronunciation"]) || !textField(g.word) ||
        normalizeWord(g.word) !== g.word || !(g.pronunciation === null || textField(g.pronunciation)))
      fail("The clipboard melody has invalid word data.");
    return { word: g.word, pronunciation: g.pronunciation };
  });
  const notes = value.notes.map((n) => {
    if (!validNote(n) || !ownKeys(n, ["pitch", "start", "dur", "lyric", "phonemes", "group"]) ||
        !textField(n.lyric) || !phonemeField(n.phonemes) ||
        !Number.isInteger(n.group) || n.group < 0 || n.group >= groups.length)
      fail("The clipboard melody contains an invalid note.");
    return { pitch: n.pitch, start: n.start, dur: n.dur, lyric: n.lyric, phonemes: n.phonemes, group: n.group };
  }).sort(byTime);
  for (let i = 1; i < notes.length; ++i) {
    if (notes[i - 1].start + notes[i - 1].dur > notes[i].start + EPS)
      fail("The clipboard melody contains overlapping notes.");
  }
  const used = new Set(notes.map((n) => n.group));
  if (used.size !== groups.length) fail("The clipboard melody has unused word data.");
  return { format: value.format, version: value.version, units: "beats", notes, groups };
}

function payloadOf(payload) {
  // Revalidate object callers too; the public pure functions must not depend on an earlier check.
  return parseMelodyClipboard(typeof payload === "string" ? payload : JSON.stringify(payload));
}

function dictionaryPronunciation(dictionary, word) {
  let found = null;
  if (!plain(dictionary) || !word) return found;
  for (const [key, value] of Object.entries(dictionary)) if (normalizeWord(key) === word && typeof value === "string")
    found = value.trim().replace(/\s+/g, " ").toUpperCase();
  return found;
}

function phonemesOf(label) {
  return String(label?.phon || label?.syl?.phonemes || "").replace(/\|/g, " ").replace(/\s+/g, " ").trim().toUpperCase();
}

function asPhonemes(label, fallback = "") {
  const phonemes = phonemesOf(label) || fallback;
  if (!phonemes) fail("Wait for Novox to finish reading the lyrics, then try again.");
  return `{${phonemes}}`;
}

// Word grouping follows the same hyphen/hold convention as the planner. A hold belongs to the
// word before it, even after that word's last syllable. This lets a copied isolated hold retain
// its own vowel instead of accidentally holding the destination's previous word.
function sourceNotes(song, labels) {
  const notes = [...(song.notes || [])].sort(byTime).map((n) => {
    if (!validNote(n)) fail("The song contains an invalid note.");
    const label = labels?.get(n._id);
    if (!song.useNoteLyrics && !label && (song.lyrics || n.lyric))
      fail("Wait for Novox to finish reading the lyrics, then try again.");
    const lyric = label ? pinnedLyric(label) : String(n.lyric || "{M}");
    return { note: n, label, lyric, group: -1 };
  });
  const groups = [];
  let group = null, open = false;
  for (const entry of notes) {
    if (!group || (!isHold(entry.lyric) && !open)) {
      group = { members: [], word: "" };
      groups.push(group);
    }
    entry.group = groups.length - 1;
    group.members.push(entry);
    if (!isHold(entry.lyric)) {
      if (!explicit(entry.lyric)) group.word += entry.lyric.replace(/-+$/, "");
      open = openWord(entry.lyric);
    }
  }
  for (const g of groups) g.word = normalizeWord(g.word);
  return { notes, groups };
}

// Fresh labelNotes() labels preserve automatic word splitting and the sound of partial-word
// selections. Complete words keep ordinary text; only cut words/isolated held notes need ARPABET.
export function createMelodyClipboard(song, selectedIds, { labels = null } = {}) {
  const selected = new Set(selectedIds || []);
  const { notes, groups } = sourceNotes(song, labels);
  const chosen = notes.filter((e) => selected.has(e.note._id));
  if (!chosen.length) fail("Select the melody notes you want to copy.");
  if (chosen.length > MAX_CLIPBOARD_NOTES) fail("Select fewer notes to copy.");
  const groupMap = new Map(), copiedGroups = [];
  const completeGroups = groups.map((g) => {
    const syllables = g.members.filter((m) => !isHold(m.lyric));
    return syllables.length > 0 && syllables.every((m) => selected.has(m.note._id));
  });
  const copied = chosen.map((e) => {
    const source = groups[e.group];
    if (!groupMap.has(e.group)) {
      groupMap.set(e.group, copiedGroups.length);
      copiedGroups.push({ word: source.word, pronunciation: dictionaryPronunciation(song.dictionary, source.word) });
    }
    const lyric = completeGroups[e.group] ? e.lyric : asPhonemes(e.label);
    return { pitch: e.note.pitch, start: e.note.start, dur: e.note.dur, lyric,
      phonemes: phonemesOf(e.label), group: groupMap.get(e.group) };
  });
  const text = JSON.stringify({ format: MELODY_CLIPBOARD_FORMAT, version: MELODY_CLIPBOARD_VERSION,
    units: "beats", notes: copied, groups: copiedGroups });
  parseMelodyClipboard(text);
  return text;
}

function rangesOf(notes) {
  const ranges = [];
  for (const n of [...notes].sort(byTime)) {
    const prior = ranges[ranges.length - 1], end = n.start + n.dur;
    if (prior && n.start <= prior[1] + EPS) prior[1] = Math.max(prior[1], end);
    else ranges.push([n.start, end]);
  }
  return ranges;
}

function firstRange(ranges, start) {
  let lo = 0, hi = ranges.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (ranges[mid][1] <= start + EPS) lo = mid + 1; else hi = mid; }
  return lo;
}

function intersecting(ranges, n) {
  const result = [], end = n.start + n.dur;
  for (let i = firstRange(ranges, n.start); i < ranges.length && ranges[i][0] < end - EPS; ++i)
    result.push(ranges[i]);
  return result;
}

// Monophonic replacement is about time, regardless of pitch. Sparse selections replace only
// their individual spans: a phrase's unselected gaps do not become a destructive bounding box.
export function melodyPasteConflicts(notes, payload) {
  const data = payloadOf(payload), ranges = rangesOf(data.notes);
  const affected = (notes || []).filter((n) => intersecting(ranges, n).length);
  return { affectedIds: affected.map((n) => n._id), count: affected.length, ranges };
}

function portableNote(n) { return { pitch: n.pitch, start: n.start, dur: n.dur, lyric: String(n.lyric || "") }; }

function subtractRanges(n, ranges) {
  let start = n.start;
  const end = n.start + n.dur, parts = [];
  for (const [a, b] of intersecting(ranges, n)) {
    if (a - start >= MIN_CLIPBOARD_NOTE_BEATS) parts.push([start, Math.min(a, end)]);
    start = Math.max(start, b);
  }
  // A smaller remainder would grow back to 1/64 on state restore and overlap the paste.
  if (end - start >= MIN_CLIPBOARD_NOTE_BEATS) parts.push([start, end]);
  return parts;
}

function copiedLabel(n) {
  const hold = isHold(n.lyric), cont = n.lyric === "" || n.lyric === "-";
  return { kind: hold ? "hold" : cont ? "cont" : "word", text: n.lyric,
    phon: n.phonemes, syl: { text: cont ? "" : n.lyric.replace(/-$/, ""), phonemes: n.phonemes, continues: openWord(n.lyric) } };
}

function withoutPastedEvidence(record, ranges) {
  if (!Array.isArray(record.originalNotes)) return { ...record };
  const beat = Number(record.startBeat), length = Number(record.beats), bpm = Number(record.bpm);
  const knownSpan = Number.isFinite(beat) && beat >= 0 && Number.isFinite(length) && length > 0;
  const touches = !knownSpan || intersecting(ranges, { start: beat, dur: length }).length > 0;
  let evidence = record.originalNotes;
  if (record.originalNotesUnits === "seconds" && Number.isFinite(beat) && beat >= 0 && Number.isFinite(bpm) && bpm > 0) {
    evidence = evidence.map((n) => ({ ...n, start: beat + n.start * bpm / 60, dur: n.dur * bpm / 60 }));
  } else if (record.originalNotesUnits !== "beats") {
    // Older recordings did not always record their units or timeline origin. Guessing can
    // attach an unrelated detected pitch to a pasted note; retain only provably remote records.
    return { ...record, originalNotes: touches ? [] : [...evidence] };
  }
  return { ...record, originalNotesUnits: "beats", originalNotes: evidence.filter((n) =>
    Number.isFinite(n.start) && Number.isFinite(n.dur) && n.dur > 0 && !intersecting(ranges, n).length) };
}

// Returned pastedNotes and fragmentNotes are references into notes. They deliberately have no
// IDs: the editor assigns fresh IDs after this succeeds. At most the first surviving fragment
// keeps the original ID. One editor checkpoint immediately before committing makes one Undo.
export function applyMelodyPaste(song, payload, { labels = null, overwrite = false } = {}) {
  const data = payloadOf(payload), conflict = melodyPasteConflicts(song.notes, data);
  if (conflict.count && !overwrite) throw new MelodyPasteConflictError(conflict.count);
  const { notes: old, groups: oldGroups } = sourceNotes(song, labels);
  const entries = [], fragmentNotes = [], pastedNotes = [], damaged = new Set();
  for (const entry of old) {
    const n = entry.note, pieces = subtractRanges(n, conflict.ranges);
    const changed = pieces.length !== 1 || pieces[0][0] !== n.start || pieces[0][1] !== n.start + n.dur;
    if (changed) damaged.add(entry.group);
    pieces.forEach(([a, b], i) => {
      // The clipboard does not invent original-pitch/confidence metadata for edited fragments.
      const note = changed ? portableNote(n) : { ...n };
      if (i === 0 && n._id !== undefined) note._id = n._id;
      else { delete note._id; fragmentNotes.push(note); }
      if (changed) { note.start = a; note.dur = b - a; }
      note.lyric = entry.lyric;
      entries.push({ note, label: entry.label, group: entry.group, origin: "old", changed, original: n });
    });
  }
  for (const n of data.notes) {
    const note = portableNote(n);
    pastedNotes.push(note);
    entries.push({ note, label: copiedLabel(n), group: n.group, origin: "paste", phonemes: n.phonemes });
  }
  entries.sort((a, b) => byTime(a.note, b.note));

  // Inserting into a gap can interrupt a held/hyphenated word even without replacing a note.
  // Preserve those syllables explicitly so they cannot join a new neighbour's word or vowel.
  const positions = new Map();
  entries.forEach((e, i) => {
    const key = `${e.origin}:${e.group}`;
    const pos = positions.get(key) || [];
    pos.push(i); positions.set(key, pos);
  });
  const interrupted = new Set([...positions].filter(([, p]) => p[p.length - 1] - p[0] + 1 !== p.length).map(([k]) => k));
  for (const e of entries) {
    let needsPhonemes = interrupted.has(`${e.origin}:${e.group}`);
    if (e.origin === "old") needsPhonemes ||= damaged.has(e.group) && oldGroups[e.group].members.length > 1;
    else {
      const group = data.groups[e.group];
      needsPhonemes ||= dictionaryPronunciation(song.dictionary, group.word) !== group.pronunciation;
    }
    // A trailing fragment must not repeat the beginning consonants of the cut note. Its vowel
    // (and original final consonants) is the part that remains after the inserted melody.
    if (e.origin === "old" && e.changed && e.note.start > e.original.start + EPS && e.label) {
      const parts = String(e.label.syl?.phonemes || "").split("|");
      const remaining = parts.length === 3 ? `${parts[1]} ${parts[2]}`.replace(/\s+/g, " ").trim() : e.label.vowel;
      e.note.lyric = asPhonemes(null, remaining || phonemesOf(e.label));
    } else if (needsPhonemes && !explicit(e.note.lyric)) e.note.lyric = asPhonemes(e.label, e.phonemes);
  }
  // Temporary identities allow lyricsFromNotes to recognize complete automatically split words
  // before the editor allocates real IDs. They never escape into the returned notes or clipboard.
  const displayLabels = new Map();
  const displayNotes = entries.map((e, i) => {
    const note = { ...e.note, _id: i };
    if (e.label && e.note.lyric === pinnedLyric(e.label)) displayLabels.set(i, e.label);
    return note;
  });
  // A pasted melody has no source recording in this instance. Mask only its occupied spans in
  // the existing import history; liveRanges/originalSegments already subtract later records and
  // skip masks without a recordingId. Keep the source files for Undo and the unaltered spans.
  // Original detected notes that touch the paste cannot be used as pitch-correction evidence.
  const previous = songTranscriptions(song);
  const transcriptions = previous.map((record) => withoutPastedEvidence(record, conflict.ranges));
  if (previous.length) {
    // Consecutive paste masks all hide the same older recordings. Keep their union so repeated
    // pastes do not multiply the work of waveform drawing or bloat every later song edit. Stop
    // at an actual import: a mask before that import must not hide its newly recorded audio.
    const masks = conflict.ranges.map(([start, end]) => ({ start, dur: end - start }));
    while (transcriptions.length) {
      const last = transcriptions[transcriptions.length - 1];
      if (last.kind !== "melody-paste" || !ownKeys(last, ["kind", "startBeat", "beats"]) ||
          !Number.isFinite(last.startBeat) || !Number.isFinite(last.beats) || last.beats <= 0) break;
      transcriptions.pop();
      masks.push({ start: last.startBeat, dur: last.beats });
    }
    for (const [start, end] of rangesOf(masks))
      transcriptions.push({ kind: "melody-paste", startBeat: start, beats: end - start });
  }
  return { notes: entries.map((e) => e.note), lyrics: lyricsFromNotes(displayNotes, { labels: displayLabels }) ?? "", transcriptions,
    useNoteLyrics: true, affectedIds: conflict.affectedIds, count: conflict.count, pastedNotes, fragmentNotes };
}
