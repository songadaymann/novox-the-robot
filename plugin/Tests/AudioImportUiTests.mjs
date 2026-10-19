// node plugin/Tests/AudioImportUiTests.mjs: the editor's audio import flow without a browser.
import assert from 'node:assert/strict';
import {createAudioImport, importPosition, importSummary, timelineBeats, timelinePosition} from '../ui/audio-import.js';

// A clean import reports its notes; warnings put their count in the state row and the most
// serious one (short form) on the line, whatever order the engine lists them in.
assert.deepEqual(importSummary({notes:[{}, {}]}, 'take.wav'), {state:'IMPORTED ✓', line:'2 notes from take.wav'});
assert.deepEqual(importSummary({notes:[{}], transcribeWarnings:[]}, 'take.wav'), {state:'IMPORTED ✓', line:'1 note from take.wav'});
const perWord = "'la' was not matched to sung audio";
const accompaniment = 'accompaniment or a second voice detected; pitches may be unreliable';
const melodyOnly = 'the transcript has 3 syllable(s) for about 40 sung ones and cannot be the lyrics; imported melody only, without lyrics';
assert.deepEqual(importSummary({notes:[{}], transcribeWarnings:[perWord, perWord, accompaniment]}, 'x.wav'),
                 {state:'IMPORTED · 3 WARNINGS', line:'accompaniment heard: check pitches'});
assert.deepEqual(importSummary({notes:[{}], transcribeWarnings:[accompaniment, melodyOnly]}, 'x.wav'),
                 {state:'IMPORTED · 2 WARNINGS', line:"lyrics didn't fit: melody only"});
assert.deepEqual(importSummary({notes:[{}], transcribeWarnings:[perWord]}, 'x.wav'), {state:'IMPORTED · 1 WARNING', line:perWord});
// Added to the song (Add to song): where it went.
assert.deepEqual(importSummary({notes:[{}, {}, {}]}, 'Region.aif', {added:true, at:'2.1.1'}), {state:'ADDED ✓', line:'3 notes at 2.1.1 · Region.aif'});
assert.deepEqual(importSummary({notes:[{}], transcribeWarnings:[accompaniment]}, 'x.wav', {added:true, at:'5.1.1'}),
                 {state:'ADDED · 1 WARNING', line:'accompaniment heard: check pitches'});

// A region stamped 01:00:01.5 (Logic's bar 1 is 01:00:00:00) lands on beat 3 at 120 bpm, from a
// Logic region drag's AIFF marker as from a bext stamp: playhead at beat 0 in a host, in the
// standalone, or with the host's edit origin (VST3). The host's seconds/beats pair at the playhead
// carries earlier tempo changes. Unknown creators without an edit origin, and unstamped files,
// start at beat 0.
const drag = {timeReference: 3601.5, originator: 'Logic Pro 12.4'};
assert.equal(timelineBeats(drag, {host: true, beats: 0, seconds: 0}, 120), 3);
assert.equal(timelineBeats(drag, {host: false, beats: 0}, 120), 3);
assert.equal(timelineBeats({timeReference: 3601.5, originator: 'Logic Pro'}, {host: true, beats: 8, seconds: 4}, 120), 3);
assert.equal(timelineBeats({timeReference: 3601.5, originator: 'Field Recorder'}, {host: true, beats: 0, seconds: 0, editOrigin: 3600}, 120), 3);
assert.equal(timelineBeats({timeReference: 3601.5, originator: 'Field Recorder'}, {host: true, beats: 0, seconds: 0}, 120), 0);
assert.equal(timelineBeats({}, {host: true, beats: 0, seconds: 0}, 120), 0);
// timelinePosition tells "no position" (null) from a stamp at bar 1 (0).
assert.equal(timelinePosition({}, {host: true, beats: 0, seconds: 0}, 120), null);
assert.equal(timelinePosition({timeReference: 3601.5, originator: 'Field Recorder'}, {host: true, beats: 0, seconds: 0}, 120), null);
assert.equal(timelinePosition({timeReference: 3600, originator: 'Logic Pro'}, {host: false, beats: 0}, 120), 0);
assert.equal(timelinePosition(drag, {host: false, beats: 0}, 120), 3);
// Jonathan's real drag of "2 Drums mixed merged" (Timestamp: 158819159 at 44.1 kHz, 123 bpm).
assert.equal(timelineBeats({timeReference: 158819159 / 44100, originator: 'Logic Pro 12.4'}, {host: true, beats: 0, seconds: 0}, 123), 2.75);

// The Detailed notes choice made when a recording is opened reaches the native bridge as the
// fifth startAudioImport argument, for a chosen file and for pending (microphone/drop) audio.
globalThis.document = {
  createElement: () => ({addEventListener() {}, showModal() {}, close() {}, open: false}),
  body: {append() {}},
};
const calls = [];
const B = {
  audioImportStatus: async () => ({modelAvailable: true}),
  startAudioImport: async (...args) => { calls.push(args); return {error: 'stop here'}; },
  cancelAudioImport() {},
};
const messages = [];
const flow = createAudioImport(B, async () => {}, (...m) => messages.push(m));
const file = {name: 'take.wav', size: 3, arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer};
await flow.open(file, 'std', true);
await flow.open({name: 'Singing.wav'}, 'std');
assert.deepEqual(calls, [['AQID', 'take.wav', false, false, true], ['', 'Singing.wav', false, false, false]]);
assert.deepEqual(messages.filter(m => m[0] === 'CAN’T IMPORT').length, 2);

// The transport given when a recording arrives comes back with its conversion (acceptScore's
// fourth argument): a recording without a timeline position goes to where the playhead was then,
// not to where a running transport has moved by the time the conversion is ready.
const accepted = [];
let polls = 0;
const ready = {state: 'ready', result: {name: 'take.wav', duration: 2, score: {notes: [{pitch: 60, start: 0, dur: 1}]}}};
const converting = createAudioImport({
  audioImportStatus: async () => (polls++ < 1 ? {modelAvailable: true} : polls < 3 ? {state: 'working'} : ready),
  startAudioImport: async () => ({}),
  cancelAudioImport() {},
}, async (...args) => accepted.push(args), () => {});
const atDrop = {host: true, playing: true, beats: 8, seconds: 4};
await converting.open(file, 'std', false, atDrop);
for (let i = 0; i < 40 && !accepted.length; i++) await new Promise(r => setTimeout(r, 50));
assert.equal(accepted.length, 1);
assert.equal(accepted[0][3], atDrop);
assert.equal(importPosition(accepted[0][2], accepted[0][3], 120, true), 8);
console.log('Audio import UI: warning summaries, timeline placement, the Detailed notes bridge argument and the transport at drop passed');
