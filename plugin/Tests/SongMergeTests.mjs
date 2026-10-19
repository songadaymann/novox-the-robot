// node plugin/Tests/SongMergeTests.mjs: region-by-region audio import (plugin/ui/song-merge.js) and
// where an import lands (importPosition in plugin/ui/audio-import.js). No engine or browser needed.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {importPosition} from '../ui/audio-import.js';
import {createLiveRangesCache, importSpan, isStarterSong, liveRanges, mergeImport, MIN_KEPT_BEATS, originalSegments, songTranscriptions,
  STARTER_SONG} from '../ui/song-merge.js';

let id = 1;
const note = (start, dur, lyric, pitch = 60) => ({pitch, start, dur, ...(lyric === undefined ? {} : {lyric}), _id: id++});
const words = (notes) => notes.map(n => n.lyric);
const starts = (notes) => notes.map(n => n.start);
const record = (recordingId, extra = {}) => ({recordingId, algorithm: 'singing-v2', transcriptWords: [{word: recordingId, start: 0, end: 1}], ...extra});
const deepCopy = (v) => JSON.parse(JSON.stringify(v));

// Where an import lands: its timeline stamp; without one the playhead when added to the song, beat
// 0 when it replaces the song. Host (DAW) and standalone both report the playhead as status.beats.
const stamped = {timeReference: 3601.5, originator: 'Logic Pro 12.4'};
assert.equal(importPosition(stamped, {host: true, beats: 17, seconds: 8.5}, 120, true), 3);
assert.equal(importPosition(stamped, {host: true, beats: 17, seconds: 8.5}, 120, false), 3);
assert.equal(importPosition({}, {host: true, beats: 17.33333333}, 120, true), 17.3333);
assert.equal(importPosition({}, {host: false, beats: 9.5}, 120, true), 9.5);
assert.equal(importPosition({}, {host: true, beats: 17}, 120, false), 0);
assert.equal(importPosition({}, {host: true, beats: -2}, 120, true), 0);   // count-in before bar 1
assert.equal(importPosition({}, {}, 120, true), 0);
// A stamp at bar 1 is a position, not a missing one.
assert.equal(importPosition({timeReference: 3600, originator: 'Logic Pro'}, {host: false, beats: 9}, 120, true), 0);

// Import records: the list, a song saved before region-by-region import (one "transcription"),
// none; and the beats each covers (an old record's from its source recording).
assert.deepEqual(songTranscriptions({transcriptions: [record('a'), null, 3, record('b')]}).map(r => r.recordingId), ['a', 'b']);
assert.deepEqual(songTranscriptions({transcription: record('old')}).map(r => r.recordingId), ['old']);
assert.deepEqual(songTranscriptions({transcriptions: [record('new')], transcription: record('old')}).map(r => r.recordingId), ['new']);
assert.deepEqual(songTranscriptions({}), []);
assert.deepEqual(songTranscriptions({transcription: null}), []);
assert.deepEqual(importSpan({startBeat: 4, beats: 8}), [4, 12]);
assert.deepEqual(importSpan(record('old'), {startBeat: 3, seconds: 6, bpm: 120}), [3, 15]);
assert.deepEqual(importSpan(record('old'), {seconds: 6, bpm: 120}), [0, 12]);
assert.equal(importSpan(record('old')), null);
assert.equal(importSpan({startBeat: 2, beats: 0}), null);

// Adding region 2 after region 1: nothing of region 1 lies in region 2's span, so every note stays,
// the new notes join, and both records are listed (the new one with its place and length).
const region1 = [note(4, 1, 'hel-'), note(5, 1, 'lo'), note(7, 2, 'world')];
const song1 = {notes: region1, lyrics: 'hel-lo\nworld', useNoteLyrics: true,
  transcriptions: [record('r1', {startBeat: 4, beats: 6})]};
const region2Notes = [note(20.5, 1, 'good-'), note(21.5, 1, 'bye')];
let merged = mergeImport(song1, {notes: region2Notes, start: 20, beats: 6, record: record('r2')});
assert.deepEqual(words(merged.notes), ['hel-', 'lo', 'world', 'good-', 'bye']);
assert.deepEqual(merged.transcriptions.map(r => [r.recordingId, r.startBeat, r.beats]), [['r1', 4, 6], ['r2', 20, 6]]);
assert.equal(merged.useNoteLyrics, true);
assert.equal(merged.lyrics, 'hel-lo\nworld\ngood-bye');  // a rest of a beat or more starts a line
assert.deepEqual([merged.removed, merged.shortened, merged.dropped], [0, 0, 0]);
// Imports earlier in time than notes already there sort in front.
merged = mergeImport(song1, {notes: [note(0.5, 1, 'oh')], start: 0, beats: 2, record: record('r0')});
assert.deepEqual(words(merged.notes), ['oh', 'hel-', 'lo', 'world']);

// Inside the span [8, 14): notes starting there go (8 and 13.99 go, 14 stays); a note running into
// it from before ends at 8, unless less than MIN_KEPT_BEATS would be left; notes after it stay.
const crowded = {useNoteLyrics: true, lyrics: '', transcriptions: [], notes: [
  note(2, 1, 'a'), note(6, 2.5, 'long'), note(7.95, 0.5, 'stub'), note(8, 1, 'gone'), note(11, 1, 'gone'),
  note(13.99, 1, 'gone'), note(14, 1, 'after'), note(20, 1, 'later')]};
merged = mergeImport(crowded, {notes: [note(8.5, 2, 'new')], start: 8, beats: 6, record: record('n')});
assert.deepEqual(words(merged.notes), ['a', 'long', 'new', 'after', 'later']);
assert.deepEqual(starts(merged.notes), [2, 6, 8.5, 14, 20]);
assert.equal(merged.notes[1].dur, 2);
assert.deepEqual([merged.removed, merged.shortened], [4, 1]);
assert.equal(MIN_KEPT_BEATS, 0.125);
assert.equal(mergeImport({notes: [note(7.875, 1, 'x')]}, {notes: [], start: 8, beats: 1}).notes.length, 1);  // exactly the minimum stays
// A note over the whole span keeps only its part before it.
merged = mergeImport({useNoteLyrics: true, notes: [note(2, 20, 'held')]}, {notes: [note(9, 1, 'in')], start: 8, beats: 4});
assert.deepEqual(merged.notes.map(n => [n.lyric, n.start, n.dur]), [['held', 2, 6], ['in', 9, 1]]);

// Re-importing another take of region 1 swaps just that section: region 1's record lies wholly in
// the new span and is dropped (its recording is no longer needed); region 2 is untouched.
const twoRegions = mergeImport(song1, {notes: region2Notes, start: 20, beats: 6, record: record('r2')});
const song2 = {notes: twoRegions.notes, lyrics: twoRegions.lyrics, useNoteLyrics: true, transcriptions: twoRegions.transcriptions};
const takeB = [note(4.25, 1, 'here'), note(5.5, 2, 'comes')];
merged = mergeImport(song2, {notes: takeB, start: 4, beats: 6, record: record('r1b')});
assert.deepEqual(words(merged.notes), ['here', 'comes', 'good-', 'bye']);
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['r2', 'r1b']);
assert.equal(merged.dropped, 1);
assert.equal(merged.lyrics, 'here comes\ngood-bye');
// A take that covers only part of an earlier recording keeps that record (its other notes remain).
merged = mergeImport(song2, {notes: [note(6.5, 1, 'mid')], start: 6, beats: 2, record: record('part')});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['r1', 'r2', 'part']);
assert.deepEqual(words(merged.notes), ['hel-', 'lo', 'mid', 'good-', 'bye']);
// A retake of region 1 trimmed a little shorter (4-9.98 against 4-10): no note is left in region
// 1's last 0.02 beat, so its record goes too, and Original doesn't play its tail after take B.
merged = mergeImport(song2, {notes: takeB, start: 4, beats: 5.98, record: record('r1short')});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['r2', 'r1short']);
assert.equal(merged.dropped, 1);
assert.deepEqual(originalSegments(merged.transcriptions, () => ({original: 'data:audio/wav;base64,', seconds: 3, bpm: 120}), 0, 12),
  [{id: 'r1short', from: 0, to: 2.99, at: 0}]);
// ...but a note still starting in what sticks out keeps it; records the import doesn't touch stay
// whatever their notes.
merged = mergeImport({...song2, notes: [...song2.notes, note(9.99, 0.5, 'tail')]}, {notes: takeB, start: 4, beats: 5.98, record: record('r1short')});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['r1', 'r2', 'r1short']);
merged = mergeImport({...song2, notes: []}, {notes: [note(40, 1, 'far')], start: 40, beats: 2, record: record('far')});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['r1', 'r2', 'far']);

// Merging changes nothing it was given, so the Undo snapshot taken before it is the old song.
const before = deepCopy(song2), importedCopy = {notes: deepCopy(takeB), start: 4, beats: 6, record: record('r1b')};
const importArg = deepCopy(importedCopy);
mergeImport(song2, importArg);
assert.deepEqual(song2, before);
assert.deepEqual(importArg, importedCopy);
// The song after a merge (what the next Undo snapshot holds) keeps notes, lyric text, note-lyrics
// mode and the record list, and every number the plugin's JSON writes back (15 decimals) is
// unchanged, so the editor sees its own song come back and adds no extra Undo step.
const after = {...song2, notes: merged.notes, lyrics: merged.lyrics, useNoteLyrics: merged.useNoteLyrics, transcriptions: merged.transcriptions};
const juceJson = (v) => JSON.parse(JSON.stringify(v, (k, x) => typeof x === 'number' && !Number.isInteger(x) ? +x.toFixed(15) : x));
const thirds = mergeImport({notes: []}, {notes: [note(1 / 3, 0.1 + 0.2, 'x')], start: 1 / 3, beats: (0.1 + 0.2) * 7,
  record: record('t', {originalNotes: [{start: 0.1 + 0.2, dur: 1 / 3}]})});
assert.deepEqual(juceJson(thirds.transcriptions.map(({startBeat, beats}) => ({startBeat, beats}))),
  thirds.transcriptions.map(({startBeat, beats}) => ({startBeat, beats})));
assert.deepEqual(Object.keys(juceJson(after)).sort(), ['lyrics', 'notes', 'transcriptions', 'useNoteLyrics']);
assert.deepEqual(songTranscriptions(juceJson(before)).map(r => r.recordingId), ['r1', 'r2']);

// Lyrics flowing from typed text are pinned to the song's notes first (each note's syllable from
// the plan's labels), so the new notes can't shift words; then the notes carry their own lyrics
// and the text mirrors the whole song, a line per phrase.
const typed = [note(0, 1), note(1, 1), note(2, 1), note(3, 1), note(4, 1)];
const word = (text, continues = false) => ({kind: 'word', text: text + (continues ? '-' : ''), syl: {text, phonemes: ' | AH | ', continues}});
const labels = new Map([[typed[0]._id, word('sun', true)], [typed[1]._id, {kind: 'hold', text: '_'}],
  [typed[2]._id, word('shine')], [typed[3]._id, {kind: 'hum', text: 'mm'}], [typed[4]._id, word('day')]]);
const flowing = {notes: typed, lyrics: 'sun- _ shine mm day', useNoteLyrics: false, transcriptions: []};
merged = mergeImport(flowing, {notes: [note(8, 1, 'I'), note(9, 1, 'sing')], start: 8, beats: 3, record: record('s')}, {labels});
assert.deepEqual(words(merged.notes), ['sun-', '_', 'shine', '{M}', 'day', 'I', 'sing']);
assert.equal(merged.useNoteLyrics, true);
assert.equal(merged.lyrics, 'sun- _ shine mm day\nI sing');
// Pinning happens before the span cuts: a pinned word whose tail the import replaced ends at the
// span ("sun-" alone would glue onto the import's first word).
merged = mergeImport(flowing, {notes: [note(2, 1, 'I'), note(3, 1, 'sing')], start: 2, beats: 2, record: record('s')}, {labels});
assert.deepEqual(words(merged.notes), ['sun', '_', 'I', 'sing', 'day']);
// Without a plan's label a note keeps the lyric it has.
merged = mergeImport({...flowing, notes: [note(0, 1, 'own'), note(1, 1)]}, {notes: [note(4, 1, 'new')], start: 4, beats: 1});
assert.deepEqual(words(merged.notes), ['own', '', 'new']);
// Words the song's notes already carry: an open word whose next note the import replaced is
// ended; a pinned middle note of a flow-split word becomes its last; explicit phonemes and words
// whose tail stays are left alone.
const ended = (lyrics, start) => words(mergeImport({useNoteLyrics: true, notes: lyrics.map((l, i) => note(i, 1, l))},
  {notes: [note(start, 1, 'X')], start, beats: 1}).notes);
assert.deepEqual(ended(['hel-', 'lo'], 1), ['hel', 'X']);
assert.deepEqual(ended(['re-', '_', '-', ''], 3), ['re-', '_', '', 'X']);
assert.deepEqual(ended(['{HH EH-}', 'lo'], 1), ['{HH EH-}', 'X']);
assert.deepEqual(ended(['hel-', 'lo'], 5), ['hel-', 'lo', 'X']);
// An import placed in a gap inside a word ("hel-" ... "lo" later): its notes come between the
// pieces, so the open word ends before them instead of gluing onto the import's first word.
merged = mergeImport({useNoteLyrics: true, notes: [note(0, 1, 'hel-'), note(10, 1, 'lo')]}, {notes: [note(4, 1, 'I'), note(5, 1, 'sing')], start: 4, beats: 2});
assert.deepEqual(words(merged.notes), ['hel', 'I', 'sing', 'lo']);

// Merging into an empty song is replacing the song: just the import, one record.
merged = mergeImport({notes: [], transcriptions: []}, {notes: region2Notes, start: 20, beats: 6, record: record('only')});
assert.deepEqual(words(merged.notes), ['good-', 'bye']);
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['only']);
assert.equal(merged.lyrics, 'good-bye');

// A song saved before region-by-region import: its one record becomes the first of the list, or is
// dropped when the new import covers its recording (whose span its source recording gives).
const oldSong = {notes: [note(3, 1, 'old')], lyrics: 'old', useNoteLyrics: true, transcription: record('legacy')};
const oldSource = {startBeat: 3, seconds: 1, bpm: 120};
merged = mergeImport(oldSong, {notes: [note(10, 1, 'new')], start: 10, beats: 2, record: record('n')},
  {spanOf: r => importSpan(r, r.recordingId === 'legacy' ? oldSource : null)});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['legacy', 'n']);
merged = mergeImport(oldSong, {notes: [note(3, 1, 'new')], start: 2, beats: 4, record: record('n')},
  {spanOf: r => importSpan(r, r.recordingId === 'legacy' ? oldSource : null)});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['n']);
// Without its source the old record's span is unknown, and it stays.
merged = mergeImport(oldSong, {notes: [note(3, 1, 'new')], start: 2, beats: 4, record: record('n')});
assert.deepEqual(merged.transcriptions.map(r => r.recordingId), ['legacy', 'n']);

// What each recording still stands behind: its span minus later imports' spans.
assert.deepEqual(liveRanges([{startBeat: 0, beats: 20}, {startBeat: 4, beats: 4}, {startBeat: 18, beats: 6}]),
  [[[0, 4], [8, 18]], [[4, 8]], [[18, 24]]]);
assert.deepEqual(liveRanges([{startBeat: 4, beats: 4}, {startBeat: 2, beats: 10}]), [[], [[2, 12]]]);
assert.deepEqual(liveRanges([{startBeat: 0, beats: 4}, {}, {startBeat: 4, beats: 4}]), [[[0, 4]], [], [[4, 8]]]);

// Sparse paste masks leave every gap of the original recording audible. The optimized range
// subtraction must also keep the earlier algorithm's exact tolerance at interval boundaries.
const sparseMasks = [{startBeat: 0, beats: 390}, ...Array.from({length: 390}, (_, i) =>
  ({kind: 'melody-paste', startBeat: i, beats: 0.5}))];
assert.deepEqual(liveRanges(sparseMasks)[0], Array.from({length: 390}, (_, i) => [i + 0.5, i + 1]));
function referenceRanges(spans) {
  return spans.map((span, i) => {
    let ranges = span ? [span.slice()] : [];
    for (let j = i + 1; j < spans.length; j++) {
      const mask = spans[j];
      if (!mask) continue;
      ranges = ranges.flatMap(([a, b]) => {
        if (mask[1] <= a + 1e-6 || mask[0] >= b - 1e-6) return [[a, b]];
        const parts = [];
        if (mask[0] > a + 1e-6) parts.push([a, mask[0]]);
        if (mask[1] < b - 1e-6) parts.push([mask[1], b]);
        return parts;
      });
    }
    return ranges;
  });
}
let rangeSeed = 0x12345678;
const randomRange = () => { rangeSeed = (Math.imul(rangeSeed, 1664525) + 1013904223) >>> 0; return rangeSeed / 2 ** 32; };
for (let trial = 0; trial < 256; trial++) {
  const spans = Array.from({length: 20}, () => {
    if (randomRange() < .05) return null;
    const start = Math.floor(randomRange() * 100) / 4;
    const end = start + Math.floor(randomRange() * 50 + 1) / 4;
    return [start, end].map(value => value + (Math.floor(randomRange() * 7) - 3) * 1e-6);
  });
  assert.deepEqual(liveRanges(spans, value => value), referenceRanges(spans));
}

// Drawing may reuse visibility, but source loading, retakes, mutation of a saved span and Undo
// must all invalidate it. Record identity and metadata alone do not affect visible time ranges.
const cachedRanges = createLiveRangesCache();
const cacheRecords = [{startBeat: 0, beats: 8}, {startBeat: 2, beats: 2}];
const firstVisible = cachedRanges(cacheRecords);
assert.deepEqual(firstVisible, [[[0, 2], [4, 8]], [[2, 4]]]);
assert.equal(cachedRanges(deepCopy(cacheRecords)), firstVisible);
cacheRecords[1].startBeat = 4;
assert.deepEqual(cachedRanges(cacheRecords), [[[0, 4], [6, 8]], [[4, 6]]]);
assert.deepEqual(cachedRanges(cacheRecords.slice(0, 1)), [[[0, 8]]]);
assert.deepEqual(cachedRanges(cacheRecords), [[[0, 4], [6, 8]], [[4, 6]]]);
let loadedSpan = null;
assert.deepEqual(cachedRanges([{}], () => loadedSpan), [[]]);
loadedSpan = [4, 8];
assert.deepEqual(cachedRanges([{}], () => loadedSpan), [[[4, 8]]]);
loadedSpan[1] = 9;
assert.deepEqual(cachedRanges([{}], () => loadedSpan), [[[4, 9]]]);

// Compare > Original over a phrase: each recording's live part under it, at its own place. Region 1
// (beats 4-10 at 120 bpm = 3 s), region 2 (20-26); a phrase from beat 8 to 22 hears region 1's last
// second, then region 2's first second, 6 s later (the gap stays silent); leading silence is cut.
const sources = new Map([['r1', {original: 'data:audio/wav;base64,', seconds: 3, bpm: 120, startBeat: 4}],
  ['r2', {original: 'data:audio/wav;base64,', seconds: 3, bpm: 120, startBeat: 20}],
  ['quiet', {seconds: 3, bpm: 120, startBeat: 0}]]);
const sourceOf = (rid) => sources.get(rid);
const recs = [record('r1', {startBeat: 4, beats: 6}), record('r2', {startBeat: 20, beats: 6})];
assert.deepEqual(originalSegments(recs, sourceOf, 8, 22), [{id: 'r1', from: 2, to: 3, at: 0}, {id: 'r2', from: 0, to: 1, at: 6}]);
assert.deepEqual(originalSegments(recs, sourceOf, 0, 100), [{id: 'r1', from: 0, to: 3, at: 0}, {id: 'r2', from: 0, to: 3, at: 8}]);
assert.deepEqual(originalSegments(recs, sourceOf, 1, 5), [{id: 'r1', from: 0, to: 0.5, at: 0}]);
assert.deepEqual(originalSegments(recs, sourceOf, 11, 19), []);
// A later take over part of region 1 replaces it there; a record without audio plays nothing.
const retake = [...recs, record('t', {startBeat: 6, beats: 2}), record('quiet', {startBeat: 0, beats: 2})];
sources.set('t', {original: 'data:audio/wav;base64,', seconds: 1, bpm: 120, startBeat: 6});
assert.deepEqual(originalSegments(retake, sourceOf, 4, 10),
  [{id: 'r1', from: 0, to: 1, at: 0}, {id: 't', from: 0, to: 1, at: 1}, {id: 'r1', from: 2, to: 3, at: 2}]);
// An old record (no startBeat/beats) plays from its source's place.
assert.deepEqual(originalSegments([record('r2')], sourceOf, 21, 23), [{id: 'r2', from: 0.5, to: 1.5, at: 0}]);

// The starter song (Song::defaultSong) is replaced by an import even with Add to song on; any
// change of notes, lyric text, note lyrics or an import makes the song the user's. Its voice may
// differ. Another starter (the browser mock's demo) can be given.
const starter = {...deepCopy(STARTER_SONG), voice: 'robot', useNoteLyrics: false, transcriptions: []};
assert.equal(isStarterSong(starter), true);
assert.equal(isStarterSong({...starter, notes: [...starter.notes].reverse()}), true);
assert.equal(isStarterSong({...starter, lyrics: 'my own words'}), false);
assert.equal(isStarterSong({...starter, notes: starter.notes.slice(1)}), false);
assert.equal(isStarterSong({...starter, notes: starter.notes.map((n, i) => i ? n : {...n, pitch: 65})}), false);
assert.equal(isStarterSong({...starter, transcriptions: [record('x')]}), false);
assert.equal(isStarterSong({...starter, useNoteLyrics: true}), false);
assert.equal(isStarterSong({...starter, notes: starter.notes.map((n, i) => i ? n : {...n, lyric: 'hey'})}), false);
assert.equal(isStarterSong({notes: [], lyrics: ''}), false);
assert.equal(isStarterSong({notes: [{pitch: 60, start: 0, dur: 1}], lyrics: 'la'}, {notes: [{pitch: 60, start: 0, dur: 1}], lyrics: 'la '}), true);
// STARTER_SONG is the plugin's default song.
const cpp = readFileSync(new URL('../Source/Song.cpp', import.meta.url), 'utf8');
const array = (name) => /\{([^}]*)\}/.exec(cpp.slice(cpp.indexOf(name)))[1].split(',').map(Number);
const pitches = array('pitches[] ='), durs = array('durs[] =');
let t = 0;
assert.deepEqual(STARTER_SONG.notes, pitches.map((pitch, i) => { const n = {pitch, start: t, dur: durs[i]}; t += durs[i]; return n; }));
assert.equal(STARTER_SONG.lyrics, /s\.lyrics = "([^"]*)";/.exec(cpp.slice(cpp.indexOf('Song Song::defaultSong')))[1]);

console.log('Song merge: import placement, spans, shortening, removal, retakes, lyric pinning, records, Undo snapshot, old songs, Original pieces and the starter song passed');
