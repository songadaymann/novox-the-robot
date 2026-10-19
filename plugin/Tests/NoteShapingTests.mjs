// node plugin/Tests/NoteShapingTests.mjs
import assert from "node:assert/strict";
import { detectKey, evidenceFor, quantizeNotes } from "../ui/note-shaping.js";

let checks = 0;
const check = (fn, name) => { fn(); checks++; if (process.env.VERBOSE) console.log("ok", name); };
const melody = (pitches, dur = 0.5) => pitches.map((pitch, i) => ({ pitch, start: i * dur, dur }));

check(() => {
  // "Twinkle twinkle" in D major, and the same tune in E minor's relative-major set (G major).
  assert.deepEqual(detectKey(melody([62, 62, 69, 69, 71, 71, 69, 67, 67, 66, 66, 64, 64, 62])),
    { root: 2, scale: "major", coverage: 1 });
  const minor = detectKey(melody([64, 67, 71, 64, 66, 67, 69, 71, 72, 71, 69, 67, 66, 64, 64, 64]));
  assert.equal(minor.scale, "minor"); assert.equal(minor.root, 4);
}, "major and minor");

check(() => {
  // One chromatic passing note does not move the key; coverage reports it.
  const k = detectKey(melody([60, 62, 64, 65, 66, 67, 69, 71, 72, 67, 64, 60]));
  assert.equal(k.root, 0); assert.equal(k.scale, "major"); assert.ok(k.coverage < 1 && k.coverage > 0.9);
}, "chromatic note");

check(() => {
  assert.equal(detectKey(melody([60, 62, 64])), null);
  assert.equal(detectKey([]), null);
}, "too few notes");

check(() => {
  // Starts and ends to the nearest 1/16; legato stays legato.
  const q = quantizeNotes([{ pitch: 60, start: 0.11, dur: 0.52 }, { pitch: 62, start: 0.63, dur: 0.4 }], 0.25);
  assert.deepEqual(q.map((n) => [n.start, n.dur]), [[0, 0.75], [0.75, 0.25]]);
}, "nearest line, legato");

check(() => {
  // Two quick syllables inside one step take consecutive lines; nothing shorter than a step.
  const q = quantizeNotes([{ pitch: 60, start: 1.02, dur: 0.06 }, { pitch: 62, start: 1.09, dur: 0.07 },
    { pitch: 64, start: 1.18, dur: 0.6 }], 0.25);
  assert.deepEqual(q.map((n) => [n.start, n.dur]), [[1, 0.25], [1.25, 0.25], [1.5, 0.25]]);
  for (const n of q) assert.ok(n.dur >= 0.25 - 1e-9);
  for (let i = 1; i < q.length; i++) assert.ok(q[i].start >= q[i - 1].start + q[i - 1].dur - 1e-9);
}, "collisions");

check(() => {
  // A real rest (half a step or more) stays a rest; lyrics and other fields ride along.
  const q = quantizeNotes([{ pitch: 60, start: 0, dur: 0.5, lyric: "la" }, { pitch: 62, start: 0.75, dur: 0.5, lyric: "di" }], 0.25);
  assert.deepEqual(q.map((n) => [n.start, n.dur, n.lyric]), [[0, 0.5, "la"], [0.75, 0.5, "di"]]);
}, "rests and fields");

check(() => {
  // The import's span end cuts a note that would run past it; grid 0 only copies.
  const q = quantizeNotes([{ pitch: 60, start: 3.9, dur: 0.95 }], 0.25, { end: 4.6 });
  assert.deepEqual([q[0].start, q[0].dur], [4, 0.5]);
  const notes = [{ pitch: 60, start: 0.13, dur: 0.3 }];
  const copy = quantizeNotes(notes, 0);
  assert.deepEqual(copy, notes); assert.notEqual(copy[0], notes[0]);
}, "span end and grid off");

check(() => {
  const evidence = [{ pitch: 61.0, start: 0.1, dur: 0.5, detectedPitch: 61.3 }, { pitch: 64, start: 0.7, dur: 0.4, detectedPitch: 63.9 }];
  // A note moved to the grid and into the key still finds the detection behind it.
  assert.equal(evidenceFor({ pitch: 62, start: 0, dur: 0.75 }, evidence), evidence[0]);
  assert.equal(evidenceFor({ pitch: 64, start: 0.75, dur: 0.25 }, evidence), evidence[1]);
  assert.equal(evidenceFor({ pitch: 70, start: 0, dur: 0.75 }, evidence), null);
  assert.equal(evidenceFor({ pitch: 62, start: 2, dur: 0.5 }, evidence), null);
}, "evidence");

console.log(`NoteShapingTests: ${checks} checks passed`);
