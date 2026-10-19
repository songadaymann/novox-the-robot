// node plugin/Tests/NoteLyricsTests.mjs --novox build/novox --library build/libnovox.dylib
// Uses the native token API and native planner; checks a note edit, its pane
// mirror, and a second plan flowed from that pane text all agree acoustically.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { applyNoteLyrics } from '../ui/note-lyrics.js';
import { labelNotes, lyricsFromNotes } from '../ui/lyric-text.js';

const args = process.argv.slice(2);
const arg = (name, fallback) => { const i = args.indexOf(name); return i < 0 ? fallback : args[i + 1]; };
const cli = resolve(arg('--novox', 'build/novox'));
const library = resolve(arg('--library', process.platform === 'darwin' ? 'build/libnovox.dylib' : 'build/libnovox.so'));
// The project's own (native arm64) Python when present: a system python3 may be x86 and unable to
// load an arm64 libnovox.
const venvPython = resolve('tools/.venv/bin/python');
const python = arg('--python', process.env.PYTHON || (existsSync(venvPython) ? venvPython : 'python3'));
const bpm = 120;
const plan = score => JSON.parse(execFileSync(cli, ['plan', '/dev/stdin'], { input: JSON.stringify(score), encoding: 'utf8' }));
const tokens = (text, dictionary = {}) => JSON.parse(execFileSync(python, ['-c', `
import ctypes, json, sys
lib = ctypes.CDLL(sys.argv[1])
lib.novox_create.argtypes = [ctypes.c_double]
lib.novox_create.restype = ctypes.c_void_p
lib.novox_lyric_tokens.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
lib.novox_lyric_tokens.restype = ctypes.c_void_p
lib.novox_free.argtypes = [ctypes.c_void_p]
lib.novox_destroy.argtypes = [ctypes.c_void_p]
e = lib.novox_create(48000)
p = lib.novox_lyric_tokens(e, sys.argv[2].encode(), sys.argv[3].encode())
assert p, 'native lyric tokenization failed'
print(ctypes.string_at(p).decode())
lib.novox_free(p)
lib.novox_destroy(e)
`, library, text, JSON.stringify(dictionary)], { encoding: 'utf8' })).tokens;
const notesOf = lyrics => lyrics.map((lyric, i) => ({ _id: i + 1, start: i * .5 + 8, dur: .5, pitch: 60 + i % 7, lyric,
  confidence: .9, source: { recordingId: 'keep-me', start: i * .25 } }));
const scoreOf = (notes, lyrics = null, dictionary = {}) => ({ tempo: bpm, units: 'beats', dictionary,
  ...(lyrics === null ? {} : { lyrics }), notes: notes.map(n => ({ ...n, ...(lyrics === null ? {} : { lyric: '' }) })) });
const labelsOf = (notes, lyrics = null, dictionary = {}) => labelNotes(notes, plan(scoreOf(notes, lyrics, dictionary)), bpm);
let cases = 0;
function edit({ notes, text, id, flowing = null, dictionary = {} }) {
  const before = JSON.stringify(notes), labels = labelsOf(notes, flowing, dictionary);
  const result = applyNoteLyrics({ notes, noteId: id, tokens: tokens(text, dictionary), labels, useNoteLyrics: flowing === null });
  assert.equal(result.error, undefined, result.error);
  assert.equal(JSON.stringify(notes), before, 'edit mutated its input');
  assert.equal(result.useNoteLyrics, true);
  assert.equal(result.notes.length, notes.length);
  result.notes.forEach((n, i) => assert.deepEqual({ ...n, lyric: '' }, { ...notes[i], lyric: '' }, 'edit moved a note or changed its metadata'));
  const after = plan(scoreOf(result.notes, null, dictionary));
  const newLabels = labelNotes(result.notes, after, bpm);
  const pane = lyricsFromNotes(result.notes, { labels: newLabels }) ?? '';
  const roundTrip = plan(scoreOf(result.notes, pane, dictionary));
  const normalized = p => p.syllables.map(s => ({ start: s.start, end: s.end, phonemes: s.phonemes, continues: s.continues }));
  assert.deepEqual(normalized(roundTrip), normalized(after), `pane changed the sung melody: ${pane}`);
  ++cases;
  return { ...result, pane, labels: newLabels, beforeLabels: labels, after };
}

// Flowing lyrics are pinned before inserting a word in the middle. Later words
// retain their notes, and both natural and explicit hyphenation use three notes.
for (const text of ['remember', 're-mem-ber']) {
  const result = edit({ notes: notesOf(Array(5).fill('')), text, id: 2, flowing: 'before~ la la la after~' });
  assert.deepEqual(result.changedIds, [2, 3, 4]);
  assert.equal(result.notes[0].lyric, 'before');
  assert.equal(result.notes[4].lyric, 'after');
  assert.match(result.pane, text === 'remember' ? /before~ remember after~/ : /before~ re-mem-ber after~/);
}
const words = edit({ notes: notesOf(['one', '', '', '', 'end']), text: 'hello world', id: 2 });
assert.deepEqual(words.changedIds, [2, 3, 4]);
assert.equal(words.pane, 'one hello world end');
const unordered = notesOf(['one', 'two', 'three', 'four', 'five']);
const sortedIds = [5, 1, 4, 2, 3];
const result = edit({ notes: sortedIds.map(id => unordered.find(n => n._id === id)), text: 'remember', id: 2 });
assert.deepEqual(result.notes.map(n => n._id), sortedIds, 'input order was unexpectedly changed');
assert.deepEqual(result.changedIds, [2, 3, 4]);
assert.equal(result.pane, 'one remember five');

const clear = edit({ notes: notesOf(['one', 'two', 'three']), text: ' ', id: 2 });
assert.equal(clear.pane, 'one mm three');
assert.equal(clear.labels.get(2).kind, 'hum');
const oneNote = edit({ notes: notesOf(['one', 'two', 'three']), text: 'wanna~', id: 2 });
assert.equal(oneNote.pane, 'one wanna~ three');
assert.deepEqual(oneNote.changedIds, [2]);
const explicit = edit({ notes: notesOf(['one', 'two', 'three']), text: '{W AA N AH}', id: 2 });
assert.equal(explicit.notes[1].lyric, '{W AA N AH}');
const loneMark = edit({ notes: notesOf(['one', 'two', 'three']), text: '~', id: 2 });
assert.equal(loneMark.labels.get(2).kind, 'hum');
const holding = edit({ notes: notesOf(['one', 'two', 'three']), text: '_', id: 2 });
assert.equal(holding.labels.get(2).kind, 'hold');
const custom = edit({ notes: notesOf(['one', '', '', '', 'end']), text: 'cat', id: 2, dictionary: { cat: 'K AE1 T AH0 IY0' } });
assert.deepEqual(custom.changedIds, [2, 3, 4]);
assert.equal(custom.pane, 'one cat end');
const dangling = edit({ notes: notesOf(['one', 'two', 'moon']), text: 'sun-', id: 2 });
assert.equal(dangling.pane, 'one sun moon');

// An edit beginning/ending within a word must not accidentally glue the rest
// of that word to the replacement. Every retained fragment keeps its phonemes.
for (const id of [1, 2, 3]) {
  const cut = edit({ notes: notesOf(['remember-', '-', '', 'end']), text: 'cat', id });
  for (const n of cut.notes) if (n._id !== id) {
    assert.equal(cut.labels.get(n._id).phon, cut.beforeLabels.get(n._id).phon, 'partial edit changed a neighboring syllable');
    if (n._id < 4) assert.match(n.lyric, /^\{.+\}$/);
  }
  assert.equal(cut.notes[3].lyric, 'end');
}
const heldCut = edit({ notes: notesOf(['sun-', '_', 'set', 'end']), text: 'cat', id: 2 });
assert.equal(heldCut.labels.get(1).phon, heldCut.beforeLabels.get(1).phon);
assert.equal(heldCut.labels.get(3).phon, heldCut.beforeLabels.get(3).phon);
const afterHold = edit({ notes: notesOf(['sun', '_', '_', 'end']), text: 'cat', id: 1 });
assert.equal(afterHold.labels.get(2).phon, afterHold.beforeLabels.get(2).phon);
assert.equal(afterHold.labels.get(3).phon, afterHold.beforeLabels.get(3).phon);

// Refuse before changing anything if the word runs past the melody, a plan is
// unavailable, or a note disappeared while a request was in flight.
const short = notesOf(['one', 'end']), saved = JSON.stringify(short), labels = labelsOf(short);
const overflow = applyNoteLyrics({ notes: short, noteId: 2, tokens: tokens('remember'), labels, useNoteLyrics: true });
assert.equal(overflow.code, 'not-enough-notes');
assert.equal(overflow.required, 3); assert.equal(overflow.available, 1);
assert.equal(applyNoteLyrics({ notes: short, noteId: 2, tokens: ['cat'], labels: new Map() }).code, 'missing-labels');
assert.equal(applyNoteLyrics({ notes: short, noteId: 100, tokens: ['cat'], labels }).code, 'missing-note');
assert.equal(JSON.stringify(short), saved);
console.log(`Note lyric edits: ${cases} native planner/API cases, pane round trips and failure guards passed.`);
