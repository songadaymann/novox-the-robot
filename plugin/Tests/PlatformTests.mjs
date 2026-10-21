// node plugin/Tests/PlatformTests.mjs: what the editor says and does differently on Windows (and
// Linux) than on a Mac (plugin/ui/platform.js), without a browser.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {dawNames, importAccept, importRoute, importTypes, isBrowserKey, keyNames, platformOf, platformText,
  playheadNotice, singingMessage, spaceToWebView} from '../ui/platform.js';

// The plugin names its platform in the status; anything else (an older plugin, the browser mock
// without ?platform=) is a Mac.
assert.equal(platformOf({platform: 'windows'}), 'windows');
assert.equal(platformOf({platform: 'linux'}), 'linux');
for (const status of [{platform: 'mac'}, {}, null, {platform: 'beos'}]) assert.equal(platformOf(status), 'mac');

// Shortcut names: the Mac's text stays as written; elsewhere Ctrl, Alt and Del.
for (const text of ['⌘Z undoes', 'Zoom in (⌘ + scroll)', '⌥-drag copies', 'arrows nudge · ⌫ deletes · ⌘D duplicates'])
  assert.equal(keyNames(text, 'mac'), text);
assert.equal(keyNames('notes removed · ⌘Z brings them back', 'windows'), 'notes removed · Ctrl+Z brings them back');
assert.equal(keyNames('Zoom in (⌘ + scroll)', 'windows'), 'Zoom in (Ctrl + scroll)');
assert.equal(keyNames('⌘C / ⌘V copy / paste · ⌥-drag copies', 'linux'), 'Ctrl+C / Ctrl+V copy / paste · Alt-drag copies');
assert.equal(keyNames('<b>3 notes</b> selected · arrows nudge · ⌫ deletes · ⌘D duplicates', 'windows'),
             '<b>3 notes</b> selected · arrows nudge · Del deletes · Ctrl+D duplicates');
// Logic named for any DAW; on a Mac the text is as it was.
assert.equal(dawNames('Hold and drag the sung audio into Logic', 'windows'), 'Hold and drag the sung audio into your DAW');
assert.equal(dawNames('Start playback in Logic, then play your MIDI keyboard to add notes.', 'windows'),
             'Start playback in your DAW, then play your MIDI keyboard to add notes.');
assert.equal(dawNames('Hold and drag the notes into Logic', 'mac'), 'Hold and drag the notes into Logic');
assert.equal(dawNames('Logic Pro', 'windows'), 'Logic Pro');

// Every shortcut and every "Logic" in the page's own text (menus, tooltips, help) reads as the
// platform says it once the editor has applied the platform's text.
const html = readFileSync(new URL('../ui/index.html', import.meta.url), 'utf8')
  .replace(/&#8984;/g, '⌘').replace(/&#8997;/g, '⌥').replace(/&#9003;/g, '⌫');
assert.ok(html.includes('⌘Z') && html.includes('into Logic'));
assert.equal(platformText(html, 'mac'), html);
const windowsHtml = platformText(html, 'windows');
assert.doesNotMatch(windowsHtml, /[⌘⌥⌫]|\bLogic\b/);
assert.ok(windowsHtml.includes('Ctrl+C / Ctrl+V copy / paste') && windowsHtml.includes('Alt-drag copies') &&
          windowsHtml.includes('(Ctrl + scroll)') && windowsHtml.includes('into your DAW'));

// Import types: as the plugin reports them; without them, the Mac's. The file picker's list on a
// Mac is the one index.html has always had.
const mac = {platform: 'mac', importTypes: {audio: ['wav', 'mp3', 'aif', 'aiff', 'm4a', 'flac', 'ogg'], score: ['mid', 'midi', 'xml', 'musicxml', 'mxl']}};
const windows = {platform: 'windows', importTypes: {audio: ['wav', 'mp3', 'aif', 'aiff', 'flac', 'ogg'], score: ['mid', 'midi', 'xml', 'musicxml', 'mxl']}};
const pageAccept = /id="file" accept="([^"]+)"/.exec(html)[1];
assert.equal(importAccept(mac), pageAccept);
assert.equal(importAccept({}), pageAccept);
assert.deepEqual(importTypes({}), importTypes(mac));
assert.equal(importAccept(windows), '.wav,.mp3,.aif,.aiff,.flac,.ogg,.mid,.midi,.xml,.musicxml,.mxl');
// A file goes to the audio import, to the engine's importer (MIDI, MusicXML, anything else, which
// says if it can't), or, for audio this platform can't read, back with a message.
assert.deepEqual(importRoute('take.m4a', mac), {kind: 'audio'});
assert.deepEqual(importRoute('Take.WAV', windows), {kind: 'audio'});
assert.deepEqual(importRoute('song.musicxml', windows), {kind: 'file'});
assert.deepEqual(importRoute('notes.txt', windows), {kind: 'file'});
assert.deepEqual(importRoute('no extension', windows), {kind: 'file'});
assert.deepEqual(importRoute('take.m4a', windows), {kind: 'refuse', line: ".m4a files can't be read on Windows · use WAV, MP3, AIFF, FLAC or Ogg"});
assert.equal(importRoute('take.AAC', windows).kind, 'refuse');
// On a Mac every name goes where it always went (the old /\.(wav|mp3|aif|aiff|m4a|flac|ogg)$/i test).
for (const name of ['a.wav', 'b.MP3', 'c.aif', 'd.aiff', 'e.m4a', 'f.flac', 'g.ogg', 'h.aac', 'i.caf', 'j.mid', 'k.mxl', 'wav'])
  assert.equal(importRoute(name, mac).kind, /\.(wav|mp3|aif|aiff|m4a|flac|ogg)$/i.test(name) ? 'audio' : 'file', name);

// A ruler click without MMC: GarageBand says so every time (as before); a DAW without Novox's MIDI
// port (always on Windows) gets a neutral hint, once; with the port, the click tries MMC.
assert.deepEqual(playheadNotice({platform: 'mac', hostName: 'GarageBand', mmc: {port: true}}),
                 {line: "GarageBand's playhead can't be moved from a plugin"});
const neutral = {line: 'Move the playhead in your DAW; Novox follows it', once: true};
assert.deepEqual(playheadNotice({platform: 'windows', hostName: 'Ableton Live', mmc: {port: false}}), neutral);
assert.deepEqual(playheadNotice({platform: 'mac', hostName: 'Ableton Live', mmc: {port: false}}), neutral);
assert.equal(playheadNotice({platform: 'mac', hostName: 'Logic Pro', mmc: {port: true}}), null);
assert.doesNotMatch(playheadNotice({platform: 'windows', mmc: {port: false}}).line, /Logic|GarageBand/);

// Browser keys a web view acts on by itself are blocked away from the Mac; the editor's own
// shortcuts, typing (AltGr is Ctrl+Alt) and the Mac's keys pass.
const key = (k, o = {}) => ({key: k, code: o.code || '', ctrlKey: !!o.ctrl, altKey: !!o.alt, metaKey: !!o.meta, shiftKey: !!o.shift});
const blocked = [key('F5'), key('F3'), key('BrowserRefresh'), key('r', {ctrl: true}), key('R', {ctrl: true, shift: true}),
  key('p', {ctrl: true}), key('f', {ctrl: true}), key('g', {ctrl: true}), key('=', {ctrl: true}), key('+', {ctrl: true, shift: true}),
  key('-', {ctrl: true}), key('0', {ctrl: true}), key('+', {ctrl: true, code: 'NumpadAdd'}), key('к', {ctrl: true, code: 'KeyR'})];
for (const e of blocked) {
  assert.ok(isBrowserKey(e, 'windows'), JSON.stringify(e));
  assert.ok(!isBrowserKey(e, 'mac'), JSON.stringify(e));
}
const allowed = [key(' '), key('r'), key('f'), key('0'), key('Delete'), key('z', {ctrl: true}), key('Z', {ctrl: true, shift: true}),
  key('y', {ctrl: true}), key('c', {ctrl: true}), key('v', {ctrl: true}), key('a', {ctrl: true}), key('d', {ctrl: true}),
  key('@', {ctrl: true, alt: true, code: 'KeyQ'}), key('}', {ctrl: true, alt: true, code: 'Digit0'})];
for (const e of allowed) assert.ok(!isBrowserKey(e, 'windows'), JSON.stringify(e));

// Space the editor could not hand to the DAW: a Mac's web view gets it (and passes it on to the
// DAW) only with nothing focused; elsewhere the web view never gets it, so a focused button,
// checkbox or the piano roll (a focusable scroller) can't act on it.
const element = (tagName) => ({tagName});
for (const target of [null, element('BODY'), element('HTML')]) assert.ok(spaceToWebView('mac', target));
for (const target of [element('BUTTON'), element('INPUT'), element('DIV'), element('SUMMARY'), element('LABEL'), element('CANVAS')])
  assert.ok(!spaceToWebView('mac', target), target.tagName);
for (const platform of ['windows', 'linux'])
  for (const target of [null, element('BODY'), element('BUTTON'), element('DIV')]) assert.ok(!spaceToWebView(platform, target));

// The singing line: on Windows a take that has stayed silent for 3 s points at the privacy setting.
const recording = (seconds) => ({recording: true, pending: false, seconds});
assert.equal(singingMessage({pending: true}, 0, 'windows'), 'Allow microphone access to record your singing.');
assert.equal(singingMessage(recording(2.5), 0, 'windows'), 'Sing, then click the singing icon to stop and convert · 2s / 60s');
assert.equal(singingMessage(recording(3.2), 0, 'windows'), 'No sound from the microphone · check Settings > Privacy & security > Microphone · 3s / 60s');
assert.equal(singingMessage(recording(3.2), 0.01, 'windows'), 'Sing, then click the singing icon to stop and convert · 3s / 60s');
assert.equal(singingMessage(recording(9), 0, 'mac'), 'Sing, then click the singing icon to stop and convert · 9s / 60s');
console.log('Platform: key names, DAW wording, import types and routes, playhead notices, Space, browser keys and the silent-microphone hint passed');
