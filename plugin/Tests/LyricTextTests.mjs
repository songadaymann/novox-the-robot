// node plugin/Tests/LyricTextTests.mjs: the lyric text the editor writes for notes that carry
// their own lyrics, and the note labels it reads from the engine's plan. No engine needed; the
// round trip through the engine itself is plugin/Tests/LyricRoundTrip.mjs.
import assert from 'node:assert/strict';
import {labelNotes, lyricsFromNotes, pinnedLyric} from '../ui/lyric-text.js';

// Notes one beat apart with no rests, from lyric tokens.
const notesOf = (tokens, {gapAfter = {}} = {}) => {
  let t = 0;
  return tokens.map((lyric, i) => { const n = {pitch: 60, start: t, dur: 1, lyric, _id: i}; t += 1 + (gapAfter[i] || 0); return n; });
};
const text = (tokens, opts) => lyricsFromNotes(notesOf(tokens), opts);

// Hyphenated pieces stay together, "_" stands for each held note (also inside a word, where the
// hyphen stays so the next piece is still read as the word's tail), and numbers arrive spelled.
assert.equal(text(['hel-', 'lo', '_', 'world']), 'hel-lo _ world');
assert.equal(text(['sun-', '_', 'set', 'breaks']), 'sun- _ set breaks');
assert.equal(text(['one', 'hun-', 'dred', 'for-', 'ty-', 'ni-', 'ners']), 'one hun-dred for-ty-ni-ners');
assert.equal(text(['re-', 'mem-', '_', '_', 'ber']), 're-mem- _ _ ber');
// Lyric-less notes read "mm" (a lone "~" inside a word); trailing ones need nothing.
assert.equal(text(['', '', 'I', 'love', '', '_', 'you', '', '']), 'mm mm I love mm _ you');
assert.equal(text(['sun-', '', 'day']), 'sun- ~ day');
assert.equal(text(['', '_', '']), null);
assert.equal(text(['_', '_']), null);
// MIDI files leave melisma notes without lyric events: there a missing lyric holds.
assert.equal(text(['la', '', 'lo'], {emptyNote: 'hold'}), 'la _ lo');
// A note's lyric with a space or an inner hyphen stays on one note (the flow drops the "~", so
// "I love" comes back as "Ilove": sung the same, since the dictionary ignores the space).
assert.equal(text(['I love', 'well-known', 'o-', 'k']), 'I~love well~known o-k');
// Explicit phonemes, punctuation and "+" holds pass through.
assert.equal(text(['{HH EH L OW}', 'you,', '+']), '{HH EH L OW} you, +');
// Pinned hums ("{M}") read "mm" like the importer's lyric-less notes.
assert.equal(text(['{M}', 'la', '{m}', 'sun-', '{M}', '{M}']), 'mm la mm sun- {M}');

// A one-note word with two or more vowels gets "~" so the flow keeps it on its note; the plan's
// label must be for this very lyric (a stale label marks nothing).
const word = (t, phonemes, continues = false) => ({kind: 'word', text: t + (continues ? '-' : ''), syl: {text: t, phonemes, continues}});
const marked = notesOf(['I', 'wanna', 'fire', 'go']);
const labels = new Map([[0, word('I', ' | AY | ')], [1, word('wanna', 'W | AA N AX | ')], [2, word('fired', 'F | AY ER | D')], [3, word('go', 'G | OW | ')]]);
assert.equal(lyricsFromNotes(marked, {labels}), 'I wanna~ fire go');
assert.equal(lyricsFromNotes(marked), 'I wanna fire go');
// Pieces of a hyphenated word and words that already carry "~" are left alone.
assert.equal(lyricsFromNotes(notesOf(['wan-', 'na']), {labels: new Map([[0, word('wan', 'W | AA N AX | ', true)]])}), 'wan-na');

// A word the flow split by its vowels and the editor then pinned ("singer-", then "") is written
// back plainly when each of its notes sings one vowel; otherwise exactly as pinned.
const cont = (phonemes, continues) => ({kind: 'cont', text: '-x', syl: {text: '', phonemes, continues}});
const pinned = notesOf(['the', 'singer-', '', 'remember-', '-', '', 'la']);
const pinLabels = new Map([[0, word('the', 'DH | AX | ')], [1, word('singer', 'S | IH | NG', true)], [2, cont(' | ER | ', false)],
  [3, word('remember', 'R | IH | ', true)], [4, cont('M | EH | M', true)], [5, cont('B | ER | ', false)], [6, word('la', 'L | AA | ')]]);
assert.equal(lyricsFromNotes(pinned, {labels: pinLabels}), 'the singer remember la');
pinLabels.set(2, cont(' | ER AX | ', false));
assert.equal(lyricsFromNotes(pinned, {labels: pinLabels}), 'the singer- ~ remember la');

// A rest of a beat or more starts a new line; so does a capitalized word (not "I") or a sentence
// mark once the line has three words. Never inside a word or before a "_".
assert.equal(lyricsFromNotes(notesOf(['oh', 'my', 'dar-', 'ling', 'you', 'are'], {gapAfter: {1: 1, 2: 2}})), 'oh my\ndar-ling you are');
assert.equal(lyricsFromNotes(notesOf(['song', '_', 'so', 'long'], {gapAfter: {0: 1}})), 'song _ so long');
assert.equal(text(['I', 'love', 'you', 'And', 'I', 'know', 'it', 'But', 'I’m', 'Sam']), 'I love you\nAnd I know it\nBut I’m Sam');
assert.equal(text(['we', 'go', 'Now', 'hi', 'there', 'friend.', 'yes']), 'we go Now hi there friend.\nyes');
// Notes are read in time order whatever order the song keeps them in.
assert.equal(lyricsFromNotes(notesOf(['a', 'b', 'c']).reverse()), 'a b c');

// What a note sings, from a plan: words, flow-split tails, holds and hums. "mm" sung as a plain
// hum shows as a hum like a note with no lyric; "hmm" is a word.
const plan = {syllables: [
  {start: 0, end: 0.5, text: 'mm', phonemes: ' | M | ', continues: false},
  {start: 0.5, end: 1.5, text: 'sing', phonemes: 'S | IH | NG', continues: true},
  {start: 1.5, end: 2, text: '', phonemes: ' | ER | ', continues: false},
  {start: 2, end: 2.5, text: 'hmm', phonemes: 'HH | M | ', continues: false},
  {start: 2.5, end: 3, text: '{M}', phonemes: ' | M | ', continues: false},
  {start: 3, end: 3.5, text: '', phonemes: ' | M | ', continues: false},
]};
const planNotes = [0, 0.5, 1, 1.5, 2, 2.5, 3].map((start, i) => ({start, dur: 0.5, _id: i}));
const got = labelNotes(planNotes, plan, 60);
assert.deepEqual([...got.values()].map(l => `${l.kind} ${l.text}`), ['hum mm', 'word sing-', 'hold _', 'cont -er', 'word hmm', 'hum mm', 'hum mm']);
assert.deepEqual([...labelNotes(planNotes, null, 60).values()], Array(7).fill(null));
// Pinning keeps hums explicit, keeps a split word open on its middle notes and ends it on the last.
assert.deepEqual([...got.values()].map(pinnedLyric), ['{M}', 'sing-', '_', '', 'hmm', '{M}', '{M}']);
assert.equal(pinnedLyric(cont(' | EH | ', true)), '-');
assert.equal(pinnedLyric(null), '');
console.log('Lyric text: pieces, holds, hums, one-note marks, pinned words, line breaks and note labels passed');
