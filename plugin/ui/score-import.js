// MIDI and MusicXML imports in the editor: what the engine's import (novox_import in beats, through
// the plugin's importFile) becomes in the song. Pure functions with no DOM, so the node tests run
// the same code the editor runs (plugin/Tests/ScoreImportTests.mjs).
//
// - Lyrics: a score has one syllable per note, so the notes keep the file's own lyrics
//   (useNoteLyrics). A one-note word with two vowels ("fire") stays on its note and nothing after
//   it moves, which flowing the words as a line would not do. The song's lyric line is sung only
//   by a file without lyrics (the engine then echoes it back as `lyrics`).
// - Tempo: a song has one tempo. A file whose tempo changes is placed by time at its first tempo,
//   so it sounds as written in the standalone app and lines up with a notation app's playback
//   clock (MuseScore's seconds; a host that gives a beat position but no tempo, as Dorico may,
//   appears to count it at its first tempo). In a DAW that gives its beats and tempo a file keeps
//   its beats, as it always has: it is a region of the DAW's tempo map.
// - Choices: a file with several parts (MIDI tracks), voices or verses offers them
//   (importChoices); a choice imports the file again with that option (importOptions).

// MIDI files leave the later notes of a melisma without lyric events: a lyric-less note starting
// within this many beats of the end of a sung one holds its syllable ("_"); later ones hum.
export const MIDI_HOLD_GAP = 0.25;

// Shown where Novox can't start, stop or move the host's playback: a host that gives plugins no
// beat position (status.clock "samples", MuseScore) and is followed by its sample clock.
export const FOLLOWS_HOST = "Start playback in your notation app; Novox follows it";

const STEPS = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
// A pitch as the engine writes it ("C#4", "Bb3" or a MIDI number) to a MIDI note number.
export function parsePitch(p) {
  if (typeof p === "number") return Math.round(p);
  const m = /^([A-Ga-g])([#b]*)(-?\d+)$/.exec(String(p).trim());
  if (!m) return NaN;
  let n = STEPS[m[1].toLowerCase()] + 12 * (+m[3] + 1);
  for (const c of m[2]) n += c === "#" ? 1 : -1;
  return n;
}

const r6 = (x) => Math.round(x * 1e6) / 1e6;
const isHold = (t) => t === "_" || t === "+";
const fmtBpm = (bpm) => String(Math.round(bpm * 100) / 100);
const countOf = (k, what) => `${k} ${what}${k === 1 ? "" : "s"}`;

// The engine's reply (a score, or {error}) as {score} or {error}.
export function parseImportReply(text) {
  let reply = null;
  try { reply = typeof text === "string" ? (text ? JSON.parse(text) : null) : text; } catch {}
  if (!reply || typeof reply !== "object") return { error: "the file could not be read" };
  if (reply.error) return { error: String(reply.error) };
  if (!Array.isArray(reply.notes) || !reply.notes.length) return { error: "the file has no notes to sing" };
  return { score: reply };
}

// The import's tempo map ([{beat, seconds, bpm}], beats in quarter notes), checked and in order.
function tempoMapOf(score) {
  const map = (Array.isArray(score.import?.tempoMap) ? score.import.tempoMap : [])
    .filter((t) => Number.isFinite(+t.beat) && Number.isFinite(+t.seconds) && +t.bpm > 0)
    .map((t) => ({ beat: +t.beat, seconds: +t.seconds, bpm: +t.bpm }))
    .sort((a, b) => a.beat - b.beat);
  if (!map.length && +score.tempo > 0) map.push({ beat: 0, seconds: 0, bpm: +score.tempo });
  return map;
}
// Seconds at `beat` through a tempo map (the engine's TempoMap::seconds).
export function tempoSeconds(map, beat) {
  if (!map.length) return beat * 0.5;
  let p = map[0];
  for (const t of map) { if (t.beat > beat) break; p = t; }
  return p.seconds + (beat - p.beat) * 60 / p.bpm;
}

// The song an import becomes: {notes: [{pitch, start, dur, lyric}] in beats, noteLyrics (the notes
// sing their own lyrics), lyrics (a line to flow onto the notes, or null to keep the song's),
// tempo (the song's tempo to set, unrounded, or null), timed (tempo changes were placed by time),
// warnings: [{text, short}], choices (importChoices)}.
// followsHostBeats: a DAW's beat position and tempo drive playback (status.host with
// status.hostTempo), which keeps the file in its own beats. bpm: the tempo for a reply in seconds
// without one of its own.
export function importedSong(score, { followsHostBeats = false, bpm = 120 } = {}) {
  const info = score.import && typeof score.import === "object" ? score.import : {};
  const map = tempoMapOf(score);
  const first = map.length ? map[0].bpm : +bpm > 0 ? +bpm : 120;
  const changes = map.filter((t, i) => i > 0 && Math.abs(t.bpm - map[i - 1].bpm) > 1e-9);
  const seconds = score.units !== "beats";
  const timed = !seconds && changes.length > 0 && !followsHostBeats;
  // Where a file position lands in the song: beats as written, or the time it is played at, in
  // beats at the first tempo (a reply in seconds is already time).
  const place = seconds ? (s) => s * first / 60 : timed ? (b) => tempoSeconds(map, b) * first / 60 : (b) => b;
  const notes = [];
  for (const n of Array.isArray(score.notes) ? score.notes : []) {
    const pitch = parsePitch(n.pitch), at = +n.start || 0, dur = +n.dur || 0;
    if (!Number.isFinite(pitch) || !(dur > 0)) continue;
    const start = Math.max(0, place(at)), end = place(at + dur);
    if (!(end > start)) continue;
    notes.push({ pitch: Math.min(127, Math.max(0, pitch)), start: r6(start), dur: r6(end - start), lyric: String(n.lyric ?? "").trim() });
  }
  notes.sort((a, b) => a.start - b.start || a.pitch - b.pitch);
  const noteLyrics = typeof info.fileLyrics === "boolean" ? info.fileLyrics : notes.some((n) => n.lyric && !isHold(n.lyric));
  if (noteLyrics && info.format === "midi") {
    // A melisma: hold the syllable before over the lyric-less notes that follow it closely.
    for (let i = 1; i < notes.length; i++) {
      const prev = notes[i - 1], n = notes[i];
      if (!n.lyric && prev.lyric && n.start - (prev.start + prev.dur) < MIDI_HOLD_GAP) n.lyric = "_";
    }
  }
  const lyrics = !noteLyrics && typeof score.lyrics === "string" && score.lyrics.trim() ? score.lyrics : null;
  const choices = importChoices(info);

  const warnings = [];
  if (timed) {
    const tempos = [...new Set(changes.map((t) => fmtBpm(t.bpm)))];
    warnings.push({
      text: `The tempo changes (to ${tempos.join(", ")} bpm): notes are placed by time at the first tempo, ${fmtBpm(first)} bpm, and the grid's bars follow that tempo`,
      short: `tempo changes: grid at ♩=${fmtBpm(first)}`,
    });
  } else if (changes.length) {
    warnings.push({ text: "The file's tempo changes are not followed: the DAW's tempo plays its beats", short: "tempo changes: the DAW's tempo plays" });
  }
  // The grid draws 4/4 bars from beat 0: say where the score's bars differ (other meters, a pickup).
  const meter = (Array.isArray(info.timeSignatures) ? info.timeSignatures : []).find((m) => !(+m.beats === 4 && +m.beatType === 4));
  const pickup = +info.pickupBeats > 0 ? +info.pickupBeats : 0;
  if (meter || pickup) {
    const what = [meter ? `is in ${meter.beats}/${meter.beatType}` : "", pickup ? `starts with a ${fmtBpm(pickup)}-beat pickup` : ""];
    warnings.push({ text: `The score ${what.filter(Boolean).join(" and ")}: the grid draws 4/4 bars from its first beat, so its bar numbers differ from the score's`,
      short: `grid: 4/4 bars${pickup ? ", pickup at bar 1" : ""}` });
  }
  for (const raw of Array.isArray(score.importWarnings) ? score.importWarnings : []) {
    const w = String(raw).trim();
    // The editor always offers the song's lyric line; a file's own lyrics winning is the point.
    if (!w || /lyrics given were not used/.test(w)) continue;
    // What the picker offers needs no warning; the rest lose the engine's option names.
    if (choices?.parts.length && /^(MusicXML has \d+ parts|MIDI has \d+ tracks)/.test(w)) continue;
    const text = w.replace(/;\s*choose with \w+\)/, ")").replace(/\s*\(choose with \w+\)/, "");
    warnings.push({ text, short: /no lyrics in the/.test(text) ? "no lyrics in the file: notes hum" : text });
  }
  return { notes, noteLyrics, lyrics, tempo: map.length || seconds ? first : null, timed, warnings, choices };
}

// What a file with several parts, voices or verses can sing instead, or null when there is no
// choice: {format, part, partVoice, verse, parts, voices, verses}, each list [{value, label}] and
// empty when it has a single entry. part / partVoice / verse are what the engine sang (verse 0:
// verse N on repeat pass N).
export function importChoices(info) {
  if (!info || !Array.isArray(info.parts)) return null;
  const midi = info.format === "midi";
  const sung = info.parts.find((p) => p.index === info.part);
  const parts = info.parts.filter((p) => p.notes > 0);
  const voices = (sung?.voices || []).filter((v) => v.notes > 0);
  const verses = (sung?.verses || []).filter((v) => Number.isInteger(v) && v > 0);
  const lyricless = (p) => (p.lyrics ? "" : " · no lyrics");
  const choices = {
    format: midi ? "midi" : "musicxml", part: info.part, partVoice: String(info.partVoice ?? ""), verse: +info.verse || 0,
    parts: parts.length > 1 ? parts.map((p) => ({ value: p.index,
      label: (String(p.name || "").trim() || `${midi ? "Track" : "Part"} ${p.index + 1}`) + lyricless(p) })) : [],
    voices: voices.length > 1 ? voices.map((v) => ({ value: String(v.voice), label: `Voice ${v.voice}` + lyricless(v) })) : [],
    verses: verses.length > 1 ? [...(info.unfolded ? [{ value: 0, label: "Verse per repeat" }] : []),
      ...verses.map((v) => ({ value: v, label: `Verse ${v}` }))] : [],
  };
  return choices.parts.length || choices.voices.length || choices.verses.length ? choices : null;
}

// The engine options ({track, partVoice, verse}) that import the file again with `key` ("part",
// "voice" or "verse") set to `value`, keeping the choices made before it: a new part starts from
// its own default voice and verse.
export function importOptions(choices, key, value) {
  const options = { track: key === "part" ? +value : choices.part };
  if (key === "part") return options;
  if (choices.format !== "midi" && choices.partVoice) options.partVoice = key === "voice" ? String(value) : choices.partVoice;
  if (key === "verse") options.verse = +value;
  else if (choices.verse > 0 && choices.verses.some((v) => v.value === choices.verse)) options.verse = choices.verse;
  return options;
}

// The LCD message after an import ({state, line, detail}): the note count, or the number of
// warnings and the first one (short form); the detail (the LCD tooltip) lists them all.
export function scoreImportMessage(song, name) {
  const notes = `${countOf(song.notes.length, "note")} from ${name}`;
  const w = song.warnings;
  if (!w.length) return { state: "IMPORTED ✓", line: notes, detail: "" };
  return { state: `IMPORTED · ${w.length} WARNING${w.length === 1 ? "" : "S"}`, line: w[0].short || w[0].text,
    detail: [notes, ...w.map((x) => x.text)].join("\n") };
}
