// Region-by-region audio import: adding an import to the song instead of replacing it. Pure
// functions with no DOM, so the node tests (plugin/Tests/SongMergeTests.mjs) run the same code the
// editor runs.
//
// An import record (one per audio import, oldest first, in song.transcriptions) keeps where its
// recording sits on the timeline (startBeat, and beats: the recording's length in beats at the
// tempo it was imported at), its recordingId (the source recording the plugin stores) and the
// engine's evidence (transcriptWords, originalNotes in beats, ...).
import { lyricsFromNotes, pinnedLyric } from "./lyric-text.js";

// A song note that an import shortens (it starts before the import and runs into it) is removed
// when less than this is left of it, in beats (a 32nd note).
export const MIN_KEPT_BEATS = 0.125;
const EPS = 1e-6;
const r6 = (x) => Math.round(x * 1e6) / 1e6; // what the plugin's JSON keeps exactly
const byTime = (a, b) => a.start - b.start || a.pitch - b.pitch;
const isHold = (t) => t === "_" || t === "+";

// The import records of a song, oldest first. Songs saved before region-by-region import
// (2026-10-17) kept their one import in song.transcription.
export function songTranscriptions(song) {
  const list = song?.transcriptions;
  if (Array.isArray(list)) return list.filter((t) => t && typeof t === "object");
  const one = song?.transcription;
  return one && typeof one === "object" ? [one] : [];
}

// The beats an import's recording covers: [startBeat, startBeat + beats]. An older record has
// neither; its source recording then says (startBeat, seconds at bpm). null when nothing does.
export function importSpan(record, source = null) {
  let start = Number(record?.startBeat), beats = Number(record?.beats);
  if (!Number.isFinite(start)) start = source ? Number(source.startBeat) || 0 : NaN;
  if (!(beats > 0) && source?.seconds > 0 && source?.bpm > 0) beats = (source.seconds * source.bpm) / 60;
  return Number.isFinite(start) && beats > 0 ? [start, start + beats] : null;
}

// Where each import's recording still stands behind the song's notes: its span, minus the spans of
// later imports (which replaced the notes there). One list of [from, to] beat ranges per record,
// in the records' order; an empty list when later imports cover it all.
export function liveRanges(records, spanOf = (r) => importSpan(r)) {
  const spans = records.map((r) => spanOf(r));
  return spans.map((span, i) => {
    let ranges = span ? [span.slice()] : [];
    for (let j = i + 1; j < spans.length && ranges.length; j++) {
      const s = spans[j];
      if (!s) continue;
      // A sparse pasted melody can leave hundreds of gaps in one source recording. The
      // surviving ranges are ordered and disjoint, so locate just the part this mask touches.
      // Rebuilding every untouched range for every mask made drawing that waveform expensive.
      let lo = 0, hi = ranges.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (s[0] >= ranges[mid][1] - EPS) lo = mid + 1;
        else hi = mid;
      }
      const first = lo;
      hi = ranges.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (s[1] <= ranges[mid][0] + EPS) hi = mid;
        else lo = mid + 1;
      }
      if (first === lo) continue;
      const a = ranges[first][0], b = ranges[lo - 1][1], pieces = [];
      if (s[0] > a + EPS) pieces.push([a, s[0]]);
      if (s[1] < b - EPS) pieces.push([s[1], b]);
      ranges.splice(first, lo - first, ...pieces);
    }
    return ranges;
  });
}

// Waveform and overview drawing ask for the same visibility on every playhead update. Only the
// ordered spans affect the result; checking them also notices source audio loaded after the song,
// changed import positions, a replacement, and Undo without retaining whole song snapshots.
export function createLiveRangesCache() {
  let previousSpans = [], previousRanges = [];
  return (records, spanOf = (r) => importSpan(r)) => {
    const spans = records.map((r) => spanOf(r));
    const unchanged = spans.length === previousSpans.length && spans.every((s, i) => {
      const previous = previousSpans[i];
      return !s ? !previous : previous && s[0] === previous[0] && s[1] === previous[1];
    });
    if (!unchanged) {
      let i = 0;
      previousRanges = liveRanges(records, () => spans[i++]);
      previousSpans = spans.map((s) => s ? s.slice() : null);
    }
    return previousRanges;
  };
}

// What Compare > Original plays for the beats [begin, end]: the live part of each import's
// recording (see liveRanges), each at its own place. sourceOf(recordingId) gives a stored source
// ({original, seconds, bpm, startBeat}); records without one are skipped. Returns pieces
// {id, from, to, at}: seconds [from, to) of the recording, played `at` seconds after the first
// piece starts (so a phrase that begins before any recording starts with it). Pieces that overlap
// are mixed by the player. Empty when no recording lies under the range.
export function originalSegments(records, sourceOf, begin, end) {
  const spanOf = (r) => importSpan(r, sourceOf(r.recordingId));
  const live = liveRanges(records, spanOf);
  const pieces = [];
  records.forEach((r, i) => {
    const source = sourceOf(r.recordingId), span = spanOf(r);
    if (!source?.original || !(source.bpm > 0) || !span) return;
    const spb = 60 / source.bpm, length = source.seconds > 0 ? source.seconds : Infinity;
    for (const [a, b] of live[i]) {
      const x = Math.max(a, begin), y = Math.min(b, end);
      if (y - x <= EPS) continue;
      const from = Math.max(0, (x - span[0]) * spb), to = Math.min(length, (y - span[0]) * spb);
      if (to - from > 1e-4) pieces.push({ id: r.recordingId, from: r6(from), to: r6(to), beat: x, spb });
    }
  });
  if (!pieces.length) return [];
  pieces.sort((p, q) => p.beat - q.beat);
  const t0 = pieces[0].beat, spb = pieces[0].spb;
  return pieces.map(({ id, from, to, beat }) => ({ id, from, to, at: r6((beat - t0) * spb) }));
}

// Adds an audio import to the song. imported = {notes (beats, already at their place on the
// timeline, each with its lyric), start (beat), beats (the length of its recording in beats),
// record (its transcription record)}. labels (labelNotes of the song's notes, from a plan of these
// very notes) say what each song note sings, which pinning flowing lyrics needs. Changes nothing
// it is given; returns the song's new {notes, lyrics, useNoteLyrics, transcriptions} and what the
// merge did {removed, shortened, dropped}.
//
// With span = [start, start + beats]:
//   - a song note that starts inside the span (start <= note.start < start + beats) is removed;
//   - a song note that starts before the span and runs into it is shortened to end where the span
//     starts, and removed if less than minKept beats would be left;
//   - song notes after the span stay, and the import's notes join them;
//   - lyrics flowing from typed text (useNoteLyrics off) are first pinned to each song note
//     (pinnedLyric of its label; a note without a label keeps its own lyric), so the new notes
//     can't shift words; afterwards every note carries its own lyric and the lyric text mirrors
//     all of them (lyricsFromNotes), one line per phrase;
//   - a word left open just before the span ("hel-", whose tail the import replaced, or whose
//     tail now comes after the import's notes) ends there;
//   - the import's record joins the song's list (with its startBeat and beats), and an earlier
//     record it overlaps is dropped when no song note starts in what is left of that recording
//     (all of it inside the span, or a sliver without notes): its recording is no longer needed.
// Merging into {notes: [], transcriptions: []} is the same as replacing the song with the import.
export function mergeImport(song, imported, { labels = null, minKept = MIN_KEPT_BEATS, spanOf = (r) => importSpan(r) } = {}) {
  const start = Number(imported.start) || 0, end = start + Math.max(0, Number(imported.beats) || 0);
  const old = [...(song.notes || [])].sort(byTime);
  const pin = !song.useNoteLyrics;
  let removed = 0, shortened = 0;
  const fate = old.map((n) => {
    if (n.start >= start - EPS && n.start < end - EPS) return "remove";
    if (n.start < start - EPS && n.start + n.dur > start + EPS) return start - n.start >= minKept - EPS ? "shorten" : "remove";
    return "keep";
  });
  const before = [], after = [];
  let lastBefore = -1;
  old.forEach((n, i) => {
    if (fate[i] === "remove") { removed++; return; }
    const note = { ...n };
    if (pin) {
      const l = labels?.get(n._id);
      note.lyric = l ? pinnedLyric(l) : String(n.lyric ?? "");
    }
    if (fate[i] === "shorten") { note.dur = r6(start - n.start); shortened++; }
    if (note.start < start - EPS) { before.push(note); lastBefore = i; } else after.push(note);
  });
  // The last note before the span had its word go on into a note the import replaced, or into a
  // later note the import's notes now come before (an import placed in a gap): end it there.
  const inserted = (imported.notes || []).length > 0;
  if (lastBefore >= 0 && (inserted || fate[lastBefore + 1] === "remove")) closeWord(before);
  const notes = [...before, ...after, ...(imported.notes || []).map((n) => ({ ...n, lyric: String(n.lyric ?? "") }))].sort(byTime);

  // An earlier import this one overlaps keeps its record only while a note still starts in the
  // part of its recording no later import covers (liveRanges). A record wholly inside the span has
  // no such part; one that sticks out of it by a sliver with no note there (a retake of a region
  // trimmed a little shorter) is dropped too, so its recording isn't kept for nothing and Compare >
  // Original doesn't play the replaced take's tail after the new one.
  const earlier = songTranscriptions(song);
  const NEW = {}, spanFor = (r) => (r === NEW ? [start, end] : spanOf(r));
  const live = liveRanges([...earlier, NEW], spanFor);
  const transcriptions = earlier.filter((r, i) => {
    const s = spanFor(r);
    if (!s || s[1] <= start + EPS || s[0] >= end - EPS) return true; // the import doesn't touch it
    return live[i].some(([a, b]) => notes.some((n) => n.start >= a - EPS && n.start < b - EPS));
  });
  const dropped = earlier.length - transcriptions.length;
  if (imported.record) transcriptions.push({ ...imported.record, startBeat: r6(start), beats: r6(end - start) });
  return {
    notes, transcriptions, useNoteLyrics: true,
    lyrics: lyricsFromNotes(notes, { labels }) ?? "",
    removed, shortened, dropped,
  };
}

// Ends the word open at the end of `notes` (looking back past held notes): "hel-" becomes "hel",
// and a pinned middle note of a word ("-") becomes its last note ("").
function closeWord(notes) {
  for (let j = notes.length - 1; j >= 0; j--) {
    const t = String(notes[j].lyric ?? "").trim();
    if (isHold(t)) continue;
    if (t === "-") notes[j].lyric = "";
    else if (t.endsWith("-") && !(t.startsWith("{") && t.endsWith("}"))) notes[j].lyric = t.replace(/-+$/, "");
    return;
  }
}

// The song a new plugin instance sings (Song::defaultSong, plugin/Source/Song.cpp). It is only
// there to be heard, so an import replaces it even with Add to song on.
export const STARTER_SONG = {
  lyrics: "hel-lo, I'm a new vir-tual sing-er _",
  notes: [[64, 0, 0.5], [67, 0.5, 1], [69, 1.5, 0.5], [71, 2, 0.5], [72, 2.5, 0.5], [71, 3, 0.5], [69, 3.5, 0.5],
    [67, 4, 0.5], [64, 4.5, 1.5], [62, 6, 1.5]].map(([pitch, start, dur]) => ({ pitch, start, dur })),
};
// Whether `song` is still exactly a starter song (same notes and lyric text, no imports, no
// note lyrics); its voice may differ.
export function isStarterSong(song, starter = STARTER_SONG) {
  if (!song || !starter || songTranscriptions(song).length || song.useNoteLyrics) return false;
  if (String(song.lyrics ?? "").trim() !== String(starter.lyrics ?? "").trim()) return false;
  const a = [...(song.notes || [])].sort(byTime), b = [...(starter.notes || [])].sort(byTime);
  return a.length === b.length && a.every((n, i) => !n.lyric && n.pitch === b[i].pitch &&
    Math.abs(n.start - b[i].start) < EPS && Math.abs(n.dur - b[i].dur) < EPS);
}
