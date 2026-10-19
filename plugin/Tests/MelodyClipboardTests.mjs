// node plugin/Tests/MelodyClipboardTests.mjs [--novox build/novox]
// Cross-instance copy/paste is a single editor transaction; these checks cover the pure model,
// including destructive-paste refusal, syllable boundaries and recording-provenance masking.
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { createMelodyClipboard, parseMelodyClipboard, melodyPasteConflicts, applyMelodyPaste,
  MelodyPasteConflictError, MAX_CLIPBOARD_BYTES, MAX_CLIPBOARD_NOTES } from "../ui/melody-clipboard.js";
import { labelNotes } from "../ui/lyric-text.js";
import { liveRanges, originalSegments } from "../ui/song-merge.js";

const note = (_id, start, dur = 1, lyric = "la", pitch = 60) => ({ _id, start, dur, lyric, pitch });
const song = (notes, more = {}) => ({ notes, lyrics: "", useNoteLyrics: true, ...more });
const word = (text, phonemes, continues = false) => ({ kind: "word", text: text + (continues ? "-" : ""),
  phon: phonemes.replace(/\|/g, " ").replace(/\s+/g, " ").trim(), vowel: phonemes.split("|")[1]?.trim(),
  syl: { text, phonemes, continues } });
const cont = (phonemes, continues = false) => ({ ...word("", phonemes, continues), kind: "cont", text: "-phonemes" });
const hold = (phonemes) => ({ kind: "hold", text: "_", phon: phonemes, vowel: phonemes });
const labels = (pairs) => new Map(pairs);
const json = (x) => JSON.stringify(x);
const freeze = (x) => { if (x && typeof x === "object") { Object.freeze(x); for (const v of Object.values(x)) freeze(v); } return x; };
let cases = 0;
function check(name, fn) { fn(); ++cases; }

const simple = song([note(1, 32, 0.5, "I"), note(2, 33, 1.25, "sing", 67)], { voice: "robot", tempo: 82,
  transcriptions: [{ recordingId: "private-recording", original: "/private/audio.wav" }] });
const simpleText = createMelodyClipboard(simple, new Set([1, 2]));
const simpleData = parseMelodyClipboard(simpleText);
check("absolute notes with no IDs, tempo, voice or source data", () => {
  assert.deepEqual(simpleData.notes.map(({ pitch, start, dur, lyric }) => ({ pitch, start, dur, lyric })),
    simple.notes.map(({ pitch, start, dur, lyric }) => ({ pitch, start, dur, lyric })));
  assert.doesNotMatch(simpleText, /private|recording|_id|tempo|voice/);
  assert.equal(simpleData.format, "novox.melody");
});
check("selection order is timeline order", () => {
  assert.equal(createMelodyClipboard(simple, new Set([2, 1])), simpleText);
});
check("empty or stale selection is rejected", () => {
  assert.throws(() => createMelodyClipboard(simple, []), /Select/);
  assert.throws(() => createMelodyClipboard(simple, [999]), /Select/);
});
check("flowing lyrics require current labels", () => {
  assert.throws(() => createMelodyClipboard({ ...simple, useNoteLyrics: false, lyrics: "I sing" }, [1]), /finish reading/);
});

const remember = song([note(10, 8, 1, ""), note(11, 9, 1, ""), note(12, 10, 1, "")],
  { lyrics: "remember", useNoteLyrics: false });
const remLabels = labels([[10, word("remember", "R | IH | ", true)],
  [11, cont("M | EH | M", true)], [12, cont("B | ER | ")]]);
const fullWord = parseMelodyClipboard(createMelodyClipboard(remember, [10, 11, 12], { labels: remLabels }));
check("intact automatic multi-syllable word keeps spelling and sounds", () => {
  assert.deepEqual(fullWord.notes.map((n) => n.lyric), ["remember-", "-", ""]);
  const pasted = applyMelodyPaste(song([]), fullWord);
  assert.equal(pasted.lyrics, "remember");
  assert.deepEqual(pasted.notes.map((n) => n.start), [8, 9, 10]);
});
check("isolated middle syllable keeps its own phonemes", () => {
  const p = parseMelodyClipboard(createMelodyClipboard(remember, [11], { labels: remLabels }));
  assert.equal(p.notes[0].lyric, "{M EH M}");
});
check("partial word cannot silently become a different pronunciation", () => {
  const p = parseMelodyClipboard(createMelodyClipboard(remember, [10, 12], { labels: remLabels }));
  assert.deepEqual(p.notes.map((n) => n.lyric), ["{R IH}", "{B ER}"]);
});
check("isolated hold keeps source vowel", () => {
  const s = song([note(1, 0, 1, "go"), note(2, 1, 1, "_")]);
  const p = parseMelodyClipboard(createMelodyClipboard(s, [2], { labels: labels([[1, word("go", "G | OW | ")], [2, hold("OW")]]) }));
  assert.equal(p.notes[0].lyric, "{OW}");
});
check("a leading hold cannot adopt a destination vowel", () => {
  const s = song([note(1, 0, 1, "_")]);
  const p = parseMelodyClipboard(createMelodyClipboard(s, [1], { labels: labels([[1, word("_", " | M | ")]]) }));
  assert.equal(p.notes[0].lyric, "{M}");
});
check("intact hyphenated word remains readable", () => {
  const s = song([note(1, 0, 1, "hel-"), note(2, 1, 1, "lo")]);
  const p = parseMelodyClipboard(createMelodyClipboard(s, [1, 2]));
  assert.equal(applyMelodyPaste(song([]), p).lyrics, "hel-lo");
});

const bad = (mutate) => { const p = structuredClone(simpleData); mutate(p); assert.throws(() => parseMelodyClipboard(json(p))); };
for (const [name, mutate] of [
  ["future version", (p) => p.version = 2], ["other format", (p) => p.format = "notes"],
  ["seconds units", (p) => p.units = "seconds"], ["zero notes", (p) => p.notes = []],
  ["too many notes", (p) => p.notes = Array(MAX_CLIPBOARD_NOTES + 1).fill(p.notes[0])],
  ["NaN", (p) => p.notes[0].start = NaN], ["Infinity", (p) => p.notes[0].dur = Infinity],
  ["negative beat", (p) => p.notes[0].start = -1], ["zero duration", (p) => p.notes[0].dur = 0],
  ["negative duration", (p) => p.notes[0].dur = -1], ["tiny duration", (p) => p.notes[0].dur = 1e-20],
  ["shorter than persisted minimum", (p) => p.notes[0].dur = 1 / 128],
  ["huge finite beat", (p) => p.notes[0].start = 1e100], ["numeric string", (p) => p.notes[0].pitch = "60"],
  ["fractional pitch", (p) => p.notes[0].pitch = 60.1], ["out of range pitch", (p) => p.notes[0].pitch = 128],
  ["extra IDs", (p) => p.notes[0]._id = 100], ["source paths", (p) => p.notes[0].recordingId = "secret"],
  ["oversized lyric", (p) => p.notes[0].lyric = "a".repeat(2049)],
  ["bad group reference", (p) => p.notes[0].group = 100],
  ["bad phonemes", (p) => p.notes[0].phonemes = "<script>"],
  ["unknown phoneme", (p) => p.notes[0].phonemes = "ZZZZ"],
  ["unused groups", (p) => p.groups.push({ word: "extra", pronunciation: null })],
  ["overlapping melody notes", (p) => p.notes[1].start = p.notes[0].start],
  ["non-string dictionary value", (p) => p.groups[0].pronunciation = {}],
  ["control characters", (p) => p.notes[0].lyric = "x\u0000y"],
]) check(`reject ${name}`, () => bad(mutate));
check("malformed and overflowing JSON", () => {
  assert.throws(() => parseMelodyClipboard("{"));
  assert.throws(() => parseMelodyClipboard("ordinary copied text"));
  assert.throws(() => parseMelodyClipboard(simpleText.replace('"start":32', '"start":1e309')));
  assert.throws(() => parseMelodyClipboard(" ".repeat(MAX_CLIPBOARD_BYTES + 1)));
  assert.throws(() => parseMelodyClipboard("é".repeat(MAX_CLIPBOARD_BYTES / 2 + 1)));
});
check("object inputs revalidated and not trusted", () => {
  assert.throws(() => applyMelodyPaste(song([]), { ...simpleData, voice: "overwrite" }));
  assert.throws(() => melodyPasteConflicts([], { ...simpleData, units: "seconds" }));
});

const sparse = parseMelodyClipboard(createMelodyClipboard(song([note(1, 2), note(2, 6)]), [1, 2]));
check("collisions cover any pitch, but exact-touch boundaries do not", () => {
  const dest = [note(1, 1), note(2, 2.5, 0.25, "different", 95), note(3, 3), note(4, 5), note(5, 7)];
  assert.deepEqual(melodyPasteConflicts(dest, sparse).affectedIds, [2]);
  assert.deepEqual(melodyPasteConflicts(dest, sparse).ranges, [[2, 3], [6, 7]]);
});
check("tiny floating-point boundary noise is not a replacement", () => {
  assert.equal(melodyPasteConflicts([note(1, 1, 1 + 1e-8)], sparse).count, 0);
});
check("replacement requires explicit confirmation", () => {
  const dest = freeze(song([note(1, 0, 8, "ah")]));
  assert.throws(() => applyMelodyPaste(dest, sparse), MelodyPasteConflictError);
  assert.equal(dest.notes[0].dur, 8);
});
check("sparse selection preserves gaps and both outside fragments", () => {
  const original = note(20, 0, 8, "ah"); original.detectedPitch = 60.25;
  const dest = freeze(song([original]));
  const out = applyMelodyPaste(dest, sparse, { overwrite: true, labels: labels([[20, word("ah", " | AA | ")]]) });
  assert.deepEqual(out.notes.map((n) => [n.start, n.dur]), [[0, 2], [2, 1], [3, 3], [6, 1], [7, 1]]);
  assert.deepEqual(out.affectedIds, [20]);
  assert.equal(out.count, 1);
  assert.equal(out.notes[0]._id, 20);
  assert.equal(out.fragmentNotes.length, 2);
  assert.ok(out.fragmentNotes.every((n) => n._id === undefined && !Object.hasOwn(n, "detectedPitch")));
  assert.ok(out.pastedNotes.every((n) => n._id === undefined && out.notes.includes(n)));
  assert.equal(original.dur, 8);
  assert.equal(original.detectedPitch, 60.25);
});
check("untouched notes retain exact duration and evidence", () => {
  const original = { ...note(20, 100.1, 0.1), detectedPitch: 59.75 };
  const out = applyMelodyPaste(song([original]), sparse);
  assert.deepEqual(out.notes.find((n) => n._id === 20), original);
});
check("subminimum fragments cannot regrow into overlaps on save and reload", () => {
  const tiny = 1 / 128, old = song([note(20, 2 - tiny, 1 + 2 * tiny, "ah")]);
  const out = applyMelodyPaste(old, sparse, { overwrite: true, labels: labels([[20, word("ah", " | AA | ")]]) });
  assert.equal(out.notes.some((n) => n._id === 20), false);
  assert.ok(out.notes.every((n) => n.dur >= 1 / 64));
  const restored = JSON.parse(json(out.notes)).map((n) => ({ ...n, dur: Math.max(1 / 64, n.dur) }));
  assert.deepEqual(restored, out.notes);
  assert.equal(melodyPasteConflicts(restored.filter((n) => !out.pastedNotes.some((p) => p.start === n.start)), sparse).count, 0);
});
check("destination word interrupted in a gap keeps pronunciation", () => {
  const old = song([note(10, 0, 1, "hel-"), note(11, 3, 1, "lo")]);
  const out = applyMelodyPaste(old, parseMelodyClipboard(createMelodyClipboard(song([note(1, 1.5, 0.5)]), [1])),
    { labels: labels([[10, word("hel", "HH | EH | ", true)], [11, word("lo", "L | OW | ")]]) });
  assert.deepEqual(out.notes.filter((n) => n._id).map((n) => n.lyric), ["{HH EH}", "{L OW}"]);
});
check("source word interrupted by existing gap notes keeps pronunciation", () => {
  const full = structuredClone(fullWord); full.notes[0].start = 0; full.notes[1].start = 2; full.notes[2].start = 3;
  const out = applyMelodyPaste(song([note(20, 1, 1, "la")]), full);
  assert.deepEqual(out.pastedNotes.map((n) => n.lyric), ["{R IH}", "{M EH M}", "{B ER}"]);
});
check("removing a word's tail preserves surviving syllable sound", () => {
  const incoming = parseMelodyClipboard(createMelodyClipboard(song([note(30, 9, 2)]), [30]));
  const out = applyMelodyPaste(remember, incoming, { labels: remLabels, overwrite: true });
  assert.equal(out.notes[0].lyric, "{R IH}");
});
check("dictionary conflicts never alter the destination dictionary", () => {
  const source = song([note(1, 0, 1, "read")], { dictionary: { read: "R EH D" } });
  const data = parseMelodyClipboard(createMelodyClipboard(source, [1], { labels: labels([[1, word("read", "R | EH | D")]]) }));
  const dest = freeze(song([], { dictionary: { read: "R IY D", names: "N EY M Z" }, voice: "std" }));
  const out = applyMelodyPaste(dest, data);
  assert.equal(out.notes[0].lyric, "{R EH D}");
  assert.deepEqual(dest.dictionary, { read: "R IY D", names: "N EY M Z" });
  assert.equal(applyMelodyPaste(song([], { dictionary: { READ: "r eh d" } }), data).notes[0].lyric, "read");
});
check("destination-only pronunciation override cannot change the copied sound", () => {
  const data = parseMelodyClipboard(createMelodyClipboard(song([note(1, 0, 1, "read")]), [1],
    { labels: labels([[1, word("read", "R | IY | D")]]) }));
  assert.equal(applyMelodyPaste(song([], { dictionary: { read: "R EH D" } }), data).notes[0].lyric, "{R IY D}");
});
check("inputs are immutable and independent across instances", () => {
  const source = freeze(structuredClone(simple)), data = freeze(structuredClone(simpleData));
  const before = json(source), dest = freeze(song([], { voice: "custom", tempo: 140 }));
  const a = applyMelodyPaste(dest, data), b = applyMelodyPaste(dest, data);
  a.notes[0].pitch = 80;
  assert.equal(b.notes[0].pitch, simple.notes[0].pitch);
  assert.equal(json(source), before);
  assert.equal(dest.voice, "custom"); assert.equal(dest.tempo, 140);
});
check("paste masks recordings only at occupied spans and excludes old pitch evidence", () => {
  const record = { recordingId: "source", startBeat: 0, beats: 10, bpm: 60, originalNotesUnits: "beats",
    originalNotes: [note(10, 1), note(11, 2), note(12, 3), note(13, 6), note(14, 7)] };
  const old = freeze(song([], { transcriptions: [record] }));
  const out = applyMelodyPaste(old, sparse);
  assert.deepEqual(out.transcriptions[0].originalNotes.map((n) => n._id), [10, 12, 14]);
  assert.deepEqual(liveRanges(out.transcriptions)[0], [[0, 2], [3, 6], [7, 10]]);
  assert.ok(out.transcriptions.slice(1).every((r) => r.kind === "melody-paste" && !r.recordingId && !r.originalNotes));
  assert.deepEqual(originalSegments(out.transcriptions, (id) => id === "source" ? { original: "audio", bpm: 60, startBeat: 0, seconds: 10 } : null, 0, 10),
    [{ id: "source", from: 0, to: 2, at: 0 }, { id: "source", from: 3, to: 6, at: 3 }, { id: "source", from: 7, to: 10, at: 7 }]);
  assert.equal(old.transcriptions[0].originalNotes.length, 5);
});
check("repeated sparse pastes compact masks without losing original audio gaps or Undo", () => {
  const before = song([], { transcriptions: [{ recordingId: "source", startBeat: 0, beats: 10 }] });
  const snapshot = json(before);
  let current = before;
  for (let i = 0; i < 25; i++) {
    const result = applyMelodyPaste(current, sparse, { overwrite: true });
    current = { ...current, ...result };
    assert.equal(current.transcriptions.length, 3);
    assert.deepEqual(liveRanges(current.transcriptions)[0], [[0, 2], [3, 6], [7, 10]]);
  }
  assert.equal(json(before), snapshot);
  assert.deepEqual(originalSegments(current.transcriptions, id => id === "source" ?
    { original: "audio", bpm: 60, startBeat: 0, seconds: 10 } : null, 0, 10),
    [{ id: "source", from: 0, to: 2, at: 0 }, { id: "source", from: 3, to: 6, at: 3 }, { id: "source", from: 7, to: 10, at: 7 }]);
});
check("paste mask compaction stops at a newer recording", () => {
  const oldMask = { kind: "melody-paste", startBeat: 2, beats: 1 };
  const records = [{ recordingId: "old", startBeat: 0, beats: 10 }, oldMask,
    { recordingId: "retake", startBeat: 2, beats: 1 }];
  const atSix = parseMelodyClipboard(createMelodyClipboard(song([note(1, 6)]), [1]));
  const out = applyMelodyPaste(song([], { transcriptions: records }), atSix);
  assert.deepEqual(out.transcriptions.slice(0, 3), records);
  assert.deepEqual(liveRanges(out.transcriptions)[2], [[2, 3]]);
});
check("seconds evidence converts to absolute beats before filtering", () => {
  const record = { recordingId: "seconds", startBeat: 32, beats: 8, bpm: 120, originalNotesUnits: "seconds",
    originalNotes: [note(10, 0, 0.5), note(11, 1, 0.5), note(12, 2, 0.5)] };
  const paste = parseMelodyClipboard(createMelodyClipboard(song([note(1, 34)]), [1]));
  const result = applyMelodyPaste(song([], { transcriptions: [record] }), paste).transcriptions[0];
  assert.equal(result.originalNotesUnits, "beats");
  assert.deepEqual(result.originalNotes.map((n) => [n._id, n.start, n.dur]), [[10, 32, 1], [12, 36, 1]]);
  assert.equal(record.originalNotes[1].start, 1);
});
check("ambiguous old evidence is cleared only when its record may overlap", () => {
  const records = [
    { startBeat: 0, beats: 10, originalNotes: [note(10, 2)] },
    { startBeat: 20, beats: 10, originalNotes: [note(20, 25)] },
    { originalNotesUnits: "seconds", originalNotes: [note(30, 1)] },
  ];
  const out = applyMelodyPaste(song([], { transcriptions: records }), sparse);
  assert.deepEqual(out.transcriptions.slice(0, 3).map((r) => r.originalNotes.length), [0, 1, 0]);
});

// The real planner is an optional extra: pinning a cut syllable must actually sing the same
// phonemes, including an automatically split word and an isolated held vowel.
const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const ni = process.argv.indexOf("--novox"), novox = resolve(root, ni < 0 ? "build/novox" : process.argv[ni + 1]);
if (existsSync(novox)) {
  const plan = (s) => {
    const p = spawnSync(novox, ["plan", "/dev/stdin"], { input: json({ tempo: 60, units: "beats", voice: "std",
      notes: s.notes, ...(s.useNoteLyrics ? {} : { lyrics: s.lyrics }), dictionary: s.dictionary }), encoding: "utf8" });
    assert.equal(p.status, 0, p.stderr);
    return JSON.parse(p.stdout);
  };
  check("real engine preserves an isolated automatically split syllable", () => {
    const source = song([note(1, 12, 1, ""), note(2, 13, 1, ""), note(3, 14, 1, "")],
      { useNoteLyrics: false, lyrics: "remember" });
    const before = plan(source), ls = labelNotes(source.notes, before, 60);
    const copied = parseMelodyClipboard(createMelodyClipboard(source, [2], { labels: ls }));
    const after = plan(applyMelodyPaste(song([]), copied));
    assert.equal(after.syllables[0].phonemes, before.syllables[1].phonemes);
    assert.equal(after.syllables[0].start, 13);
  });
  check("real engine preserves a held vowel pasted after another vowel", () => {
    const source = song([note(1, 0, 1, "go"), note(2, 1, 1, "_")]);
    const before = plan(source), ls = labelNotes(source.notes, before, 60);
    const copied = parseMelodyClipboard(createMelodyClipboard(source, [2], { labels: ls }));
    const after = plan(applyMelodyPaste(song([note(3, 0, 1, "me")]), copied));
    assert.match(after.syllables.at(-1).phonemes, /OW/);
    assert.equal(after.syllables.at(-1).start, 1);
  });
  check("real engine keeps a complete copied word ordinary text", () => {
    const before = plan(remember), ls = labelNotes(remember.notes, before, 60);
    const data = parseMelodyClipboard(createMelodyClipboard(remember, [10, 11, 12], { labels: ls }));
    const pasted = applyMelodyPaste(song([]), data), after = plan(pasted);
    assert.equal(pasted.lyrics, "remember");
    assert.deepEqual(after.syllables.map((s) => s.phonemes), before.syllables.map((s) => s.phonemes));
  });
} else console.log("Melody clipboard: real-planner checks skipped (engine CLI is not built).");
console.log(`Melody clipboard: ${cases} checks passed.`);
