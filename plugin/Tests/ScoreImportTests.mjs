// node plugin/Tests/ScoreImportTests.mjs [--novox build/novox] [--library build/libnovox.dylib] [--python PY]
//
// The editor's MIDI and MusicXML import (plugin/ui/score-import.js) and the mock backend's importer
// (plugin/ui/backend.js), without a browser:
//   - the file's own lyrics stay on its notes, one syllable each, and the song's lyric line is used
//     only by a file without lyrics; MIDI melismas hold;
//   - tempo changes are placed by time at the first tempo (except in a DAW that gives its tempo);
//   - the part / voice / verse picker and the options it imports again with;
//   - warnings and errors as the LCD shows them;
//   - the mock answers like the engine (lyrics echoed only without the file's own, note lyrics,
//     the parts list, warnings), .mxl included.
// With the engine built (--library for novox_import in beats, as the plugin calls it, through the
// project's Python; --novox for the planner) it also imports tests/fixtures/notation and a
// "the fire burns our home" score and checks every note sings exactly its own syllable, that the
// lyric box text flows back onto the same notes, and that the tempo placement matches the engine's
// own seconds. Those checks are skipped, with a note, when the engine isn't built.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FOLLOWS_HOST, MIDI_HOLD_GAP, importChoices, importOptions, importedSong, parseImportReply, parsePitch,
  scoreImportMessage, tempoSeconds } from '../ui/score-import.js';
import { mockImportFile } from '../ui/backend.js';
import { labelNotes, lyricsFromNotes } from '../ui/lyric-text.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const arg = (name, fallback) => { const i = args.indexOf(name); return i < 0 ? fallback : args[i + 1]; };
const cli = resolve(ROOT, arg('--novox', 'build/novox'));
const library = resolve(ROOT, arg('--library', process.platform === 'darwin' ? 'build/libnovox.dylib' : 'build/libnovox.so'));
const venvPython = resolve(ROOT, 'tools/.venv/bin/python');
const python = arg('--python', process.env.PYTHON || (existsSync(venvPython) ? venvPython : 'python3'));
const fixture = (name) => new Uint8Array(readFileSync(resolve(ROOT, 'tests/fixtures/notation', name)));
let checks = 0;
const ok = (value, message) => { ++checks; assert.ok(value, message); };
const eq = (a, b, message) => { ++checks; assert.deepEqual(a, b, message); };
const near = (a, b, tol, message) => { ++checks; assert.ok(Math.abs(a - b) <= tol, `${message}: ${a} vs ${b}`); };

// ---- engine-shaped replies
const info = (o = {}) => ({ format: 'musicxml', part: 0, partVoice: '1', verse: 1, fileLyrics: true, repeats: false, unfolded: false,
  pickupBeats: 0, tempoMap: [{ beat: 0, seconds: 0, bpm: 120 }], timeSignatures: [],
  parts: [{ index: 0, id: 'P1', name: 'Voice', notes: 4, lyrics: 4, verses: [1], voices: [{ voice: '1', notes: 4, lyrics: 4 }] }], ...o });
const reply = (notes, o = {}, extra = {}) => ({ import: info(o), units: 'beats', tempo: (o.tempoMap || [{ bpm: 120 }])[0].bpm,
  notes: notes.map(([pitch, start, dur, lyric]) => ({ pitch, start, dur, ...(lyric === undefined ? {} : { lyric }) })), ...extra });

// ---- the file's own lyrics, one syllable per note
{
  const fire = reply([['C4', 0, 1, 'the'], ['D4', 1, 1, 'fire'], ['E4', 2, 1, 'burns'], ['F4', 3, 1, 'our'], ['G4', 4, 1, 'home']]);
  const song = importedSong(fire);
  eq(song.noteLyrics, true, 'a file with lyrics sings them from its notes');
  eq(song.notes.map((n) => n.lyric), ['the', 'fire', 'burns', 'our', 'home'], 'one syllable per note, nothing shifts');
  eq(song.lyrics, null, 'the lyric line is not flowed over a file with lyrics');
  eq(song.notes.map((n) => n.start), [0, 1, 2, 3, 4]);
  eq(song.warnings, []);
  // An engine echo of the song's lyrics never replaces the file's own (older engines echoed them).
  eq(importedSong({ ...fire, lyrics: "hel-lo, I'm a new vir-tual sing-er _" }).lyrics, null);
  // Without fileLyrics (an older engine), lyrics on the notes still count.
  const { import: _, ...bare } = fire;
  eq(importedSong(bare).noteLyrics, true);
  // A file without lyrics: the song's line (echoed by the engine) flows onto its notes.
  const hum = reply([['C4', 0, 1], ['D4', 1, 1]], { fileLyrics: false }, { lyrics: 'hel-lo' });
  const plain = importedSong(hum);
  eq([plain.noteLyrics, plain.lyrics, plain.notes.map((n) => n.lyric)], [false, 'hel-lo', ['', '']]);
  eq(importedSong(reply([['C4', 0, 1]], { fileLyrics: false })).lyrics, null, 'no line: the song keeps its own');
  // Pitches as the engine writes them; out-of-range and empty notes dropped.
  eq([parsePitch('C4'), parsePitch('Bb3'), parsePitch('F#5'), parsePitch(61.6), parsePitch('x')].map(String), ['60', '58', '78', '62', 'NaN']);
  eq(importedSong(reply([['C4', 0, 0, 'a'], ['nope', 1, 1, 'b'], ['C4', 2, 1, 'c']])).notes.map((n) => n.lyric), ['c']);
}

// ---- MIDI: lyric-less notes right after a sung one hold it (melisma); after a rest they hum
{
  const midi = reply([['C4', 0, 1, 'the'], ['D4', 1, 1, 'fi-'], ['E4', 2, 0.9, 're'], ['F4', 2.95, 1], ['G4', 5, 1], ['A4', 6, 1, 'home']],
    { format: 'midi', partVoice: '', parts: [{ index: 0, name: '', notes: 6, lyrics: 4, verses: [1], voices: [] }] });
  const song = importedSong(midi);
  eq(song.notes.map((n) => n.lyric), ['the', 'fi-', 're', '_', '', 'home'], `holds within ${MIDI_HOLD_GAP} beat`);
  // MusicXML says where its melismas are; nothing is added.
  eq(importedSong({ ...midi, import: { ...midi.import, format: 'musicxml' } }).notes[3].lyric, '');
}

// ---- tempo: a file whose tempo changes is placed by time at its first tempo
{
  const map = [{ beat: 0, seconds: 0, bpm: 66.67 }, { beat: 19, seconds: 19 * 60 / 66.67, bpm: 60 }];
  eq(tempoSeconds(map, 19 + 3), 19 * 60 / 66.67 + 3);
  const hymn = reply([['G4', 0, 1, 'A'], ['E5', 19, 3, 'Home']], { tempoMap: map });
  const song = importedSong(hymn);
  eq(song.tempo, 66.67, 'the song keeps the first tempo unrounded');
  ok(song.timed);
  eq(song.notes[0].start, 0);
  near(song.notes[1].start, 19, 1e-6, 'the change point stays where it was');
  near(song.notes[1].dur, 3 * 66.67 / 60, 1e-6, 'after it, beats at 60 bpm take their time at 66.67');
  eq(song.warnings.length, 1);
  ok(/66\.67/.test(song.warnings[0].text) && /60 bpm/.test(song.warnings[0].text) && /grid/.test(song.warnings[0].text), song.warnings[0].text);
  eq(scoreImportMessage(song, 'Hymn.mxl'), { state: 'IMPORTED · 1 WARNING', line: 'tempo changes: grid at ♩=66.67',
    detail: `2 notes from Hymn.mxl\n${song.warnings[0].text}` });
  // In a DAW that gives its beats and tempo, a file is a region of its tempo map: beats kept, a
  // score as well as a MIDI file, as before.
  const midi = { ...hymn, import: { ...hymn.import, format: 'midi' } };
  for (const file of [midi, hymn]) {
    const daw = importedSong(file, { followsHostBeats: true });
    eq([daw.timed, daw.notes[1].start, daw.notes[1].dur, daw.tempo], [false, 19, 3, 66.67]);
    ok(/DAW's tempo/.test(daw.warnings[0].text));
  }
  // Elsewhere (standalone, MuseScore's clock, a host that gives beats but no tempo) both are timed.
  ok(importedSong(hymn).timed && importedSong(midi).timed);
  // One tempo: beats as written. A reply in seconds is time already.
  eq(importedSong(reply([['C4', 2, 1, 'a']], { tempoMap: [{ beat: 0, seconds: 0, bpm: 90 }] })).notes[0].start, 2);
  const seconds = importedSong({ units: 'seconds', tempo: 90, notes: [{ pitch: 'C4', start: 2, dur: 1, lyric: 'a' }] });
  eq([seconds.notes[0].start, seconds.notes[0].dur, seconds.tempo], [3, 1.5, 90]);
}

// ---- warnings and errors for the LCD
{
  // The grid's 4/4 bars from beat 0 against the score's meter and pickup.
  const hymn = importedSong(reply([['C4', 0, 1, 'a']], { pickupBeats: 1, timeSignatures: [{ beat: 0, seconds: 0, beats: 3, beatType: 4 }] }));
  eq(hymn.warnings.map((w) => w.short), ['grid: 4/4 bars, pickup at bar 1']);
  ok(/3\/4 and starts with a 1-beat pickup/.test(hymn.warnings[0].text), hymn.warnings[0].text);
  eq(importedSong(reply([['C4', 0, 1, 'a']], { timeSignatures: [{ beat: 0, seconds: 0, beats: 4, beatType: 4 }] })).warnings, []);
  const w = importedSong(reply([['C4', 0, 1, 'a']], {}, { importWarnings: [
    "sang the file's own lyrics (the lyrics given were not used)",
    '1 note(s) in other voices skipped (sang voice 1; choose with partVoice)',
    'lyric verse(s) 2 not sung (choose with verse)',
    'MusicXML has 2 parts; using part 1 (Voice) (choose with track)'] })).warnings.map((x) => x.text);
  eq(w, ['1 note(s) in other voices skipped (sang voice 1)', 'lyric verse(s) 2 not sung',
    'MusicXML has 2 parts; using part 1 (Voice)'], 'the editor always offers its line; option names are the engine CLI\'s');
  eq(importedSong(reply([['C4', 0, 1]], { fileLyrics: false }, { importWarnings: ['no lyrics in the MusicXML; notes will hum'] })).warnings[0].short,
    'no lyrics in the file: notes hum');
  eq(scoreImportMessage(importedSong(reply([['C4', 0, 1, 'a'], ['D4', 1, 1, 'b']])), 'x.xml'),
    { state: 'IMPORTED ✓', line: '2 notes from x.xml', detail: '' });
  eq(parseImportReply('{"error": "this is compressed MusicXML (.mxl): unzip it"}'), { error: 'this is compressed MusicXML (.mxl): unzip it' });
  eq(parseImportReply(''), { error: 'the file could not be read' });
  eq(parseImportReply('{"notes": []}'), { error: 'the file has no notes to sing' });
  ok(parseImportReply('{"notes": [{"pitch": "C4", "start": 0, "dur": 1}]}').score);
  ok(/notation app/.test(FOLLOWS_HOST));
}

// ---- the picker: what can be chosen, and the options it imports again with
{
  const hymn = info({ part: 1, verse: 0, repeats: true, unfolded: true, parts: [
    { index: 0, id: 'P1', name: 'Piano', notes: 10, lyrics: 0, verses: [], voices: [{ voice: '1', notes: 5, lyrics: 0 }, { voice: '5', notes: 5, lyrics: 0 }] },
    { index: 1, id: 'P2', name: 'Voice', notes: 11, lyrics: 9, verses: [1, 2], voices: [{ voice: '1', notes: 10, lyrics: 9 }, { voice: '2', notes: 1, lyrics: 0 }] },
    { index: 2, id: 'P3', name: '', notes: 0, lyrics: 0, verses: [], voices: [] }] });
  const c = importChoices(hymn);
  eq(c.parts, [{ value: 0, label: 'Piano · no lyrics' }, { value: 1, label: 'Voice' }], 'parts with notes, the default the engine chose');
  eq(c.voices, [{ value: '1', label: 'Voice 1' }, { value: '2', label: 'Voice 2 · no lyrics' }]);
  eq(c.verses, [{ value: 0, label: 'Verse per repeat' }, { value: 1, label: 'Verse 1' }, { value: 2, label: 'Verse 2' }]);
  eq([c.part, c.partVoice, c.verse], [1, '1', 0]);
  eq(importOptions(c, 'part', '0'), { track: 0 }, 'a new part starts from its own default voice and verse');
  eq(importOptions(c, 'voice', '2'), { track: 1, partVoice: '2' });
  eq(importOptions(c, 'verse', '2'), { track: 1, partVoice: '1', verse: 2 });
  const chosen = importChoices({ ...hymn, verse: 2 });
  eq(importOptions(chosen, 'voice', '2'), { track: 1, partVoice: '2', verse: 2 }, 'a chosen verse stays');
  // Nothing to choose: no picker.
  eq(importChoices(info()), null);
  eq(importChoices(undefined), null);
  // No repeats: no "per repeat" verse.
  eq(importChoices(info({ parts: [{ ...info().parts[0], verses: [1, 2] }] })).verses.map((v) => v.value), [1, 2]);
  // MIDI tracks: unnamed ones by number; no voices.
  const midi = importChoices({ format: 'midi', part: 1, partVoice: '', verse: 1, parts: [
    { index: 0, name: 'Piano', notes: 30, lyrics: 0, verses: [], voices: [] },
    { index: 1, name: '', notes: 12, lyrics: 12, verses: [1], voices: [] }] });
  eq(midi.parts, [{ value: 0, label: 'Piano · no lyrics' }, { value: 1, label: 'Track 2' }]);
  eq([midi.voices, midi.verses], [[], []]);
  eq(importOptions(midi, 'part', 0), { track: 0 });
  // The engine's "has N parts" warning is the picker's job.
  eq(importedSong({ ...reply([['C4', 0, 1, 'a']]), import: hymn, importWarnings: ['MusicXML has 3 parts; using part 1 (Voice) (choose with track)'] }).warnings, []);
}

// ---- the mock backend's importer answers like the engine
const zip = async (entries) => {  // a zip (deflate) the way notation apps write .mxl
  const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc32 = (b) => { let c = ~0; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
  const local = [], central = [];
  let offset = 0;
  for (const [name, text] of entries) {
    const raw = new TextEncoder().encode(text), file = new TextEncoder().encode(name);
    const data = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate-raw'))).arrayBuffer());
    const head = (sig, size) => { const b = new Uint8Array(size), v = new DataView(b.buffer); v.setUint32(0, sig, true); return [b, v]; };
    const [lh, l] = head(0x04034b50, 30);
    l.setUint16(4, 20, true); l.setUint16(8, 8, true); l.setUint32(14, crc32(raw), true);
    l.setUint32(18, data.length, true); l.setUint32(22, raw.length, true); l.setUint16(26, file.length, true);
    const [ch, c] = head(0x02014b50, 46);
    c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(10, 8, true); c.setUint32(16, crc32(raw), true);
    c.setUint32(20, data.length, true); c.setUint32(24, raw.length, true); c.setUint16(28, file.length, true); c.setUint32(42, offset, true);
    local.push(lh, file, data);
    central.push(ch, file);
    offset += 30 + file.length + data.length;
  }
  const size = central.reduce((s, b) => s + b.length, 0), end = new Uint8Array(22);
  const v = new DataView(end.buffer);
  v.setUint32(0, 0x06054b50, true); v.setUint16(8, entries.length, true); v.setUint16(10, entries.length, true);
  v.setUint32(12, size, true); v.setUint32(16, offset, true);
  return new Uint8Array(await new Blob([...local, ...central, end]).arrayBuffer());
};
const mock = async (bytes, lyrics = "hel-lo, I'm a new vir-tual sing-er _", options = {}) => JSON.parse(await mockImportFile(bytes, lyrics, options));
const lyricsOf = (r) => r.notes.map((n) => n.lyric ?? '');
{
  const hymn = await mock(fixture('musescore-hymn.musicxml'));
  eq([hymn.units, hymn.lyrics, hymn.import.part, hymn.import.partVoice, hymn.import.fileLyrics], ['beats', undefined, 1, '1', true],
    'the voice part with lyrics; the line given is not echoed');
  eq(hymn.import.parts.map((p) => [p.name, p.lyrics > 0]), [['Piano', false], ['Voice', true]]);
  eq(lyricsOf(hymn).slice(0, 4), ['A', 'ma-', 'zing', 'grace~and'], 'note lyrics, an elision on one note');
  ok(hymn.importWarnings.some((w) => /has 2 parts/.test(w)) && hymn.importWarnings.some((w) => /lyrics given were not used/.test(w)));
  eq(hymn.import.tempoMap[0].bpm, 66.67);
  // Repeats are reported as the engine does (verse per repeat), with the meter and the pickup.
  eq([hymn.import.repeats, hymn.import.unfolded, hymn.import.verse, hymn.import.pickupBeats], [true, true, 0, 1]);
  eq(hymn.import.timeSignatures.map((t) => [t.beat, t.beats, t.beatType]), [[0, 3, 4]]);
  eq(importChoices(hymn.import).verses[0], { value: 0, label: 'Verse per repeat' });
  // One verse throughout: a measure without it (the first ending) sings the verse below, as the engine does.
  const second = await mock(fixture('musescore-hymn.musicxml'), '', { verse: 2 });
  eq([second.import.verse, lyricsOf(second).slice(1, 7)], [2, ['pre-', 'cious', 'love', 'so', 'sweet', '_']]);
  const piano = await mock(fixture('musescore-hymn.musicxml'), 'la la', { track: 0 });
  eq([piano.lyrics, piano.import.fileLyrics, lyricsOf(piano).every((l) => !l)], ['la la', false, true], 'a part without lyrics sings the line given');
  const alto = await mock(fixture('musescore-hymn.musicxml'), '', { track: 1, partVoice: '2' });
  eq([alto.import.partVoice, alto.notes.length, alto.lyrics], ['2', 1, undefined]);
  ok(alto.importWarnings.some((w) => /no lyrics in the MusicXML/.test(w)));
  eq(await mock(fixture('musescore-hymn.musicxml'), '', { track: 5 }), { error: 'MusicXML part 5 not found' });
  const verse2 = await mock(fixture('musescore-coda.musicxml'), '', { verse: 2 });
  eq(verse2.import.verse, 2);
  // .mxl: the container's rootfile, else the first score.
  const text = new TextDecoder().decode(fixture('dorico-carol.musicxml'));
  const container = '<?xml version="1.0"?><container><rootfiles><rootfile full-path="Carol.musicxml"/></rootfiles></container>';
  const carol = await mock(await zip([['mimetype', 'application/vnd.recordare.musicxml'], ['META-INF/container.xml', container],
    ['Carol.musicxml', text]]));
  eq([carol.import.part, lyricsOf(carol).slice(0, 3)], [0, ['Glo-', 'ri-', 'a~in']]);
  eq(lyricsOf(await mock(await zip([['Carol.xml', text]]))).slice(0, 2), ['Glo-', 'ri-']);
  eq(await mock(await zip([['readme.txt', 'x']])), { error: 'This .mxl file holds no MusicXML score' });
  eq(await mock(new TextEncoder().encode('PK no zip')), { error: 'This .mxl file is not a readable zip archive (compressed MusicXML)' });
  eq(await mock(new TextEncoder().encode('hello')), { error: 'unrecognized file: expected MIDI or MusicXML' });
  // MIDI: the fixed melody, with its lyrics on its notes.
  const midi = await mock(new TextEncoder().encode('MThd'));
  eq([midi.import.format, midi.import.fileLyrics, midi.lyrics, lyricsOf(midi)[0]], ['midi', true, undefined, 'twin-']);
  // Through the editor's import: lyrics kept, picker offered.
  const song = importedSong(hymn);
  ok(song.noteLyrics && song.choices.parts.length === 2 && song.timed);
}

// ---- the real engine (novox_import in beats as the plugin calls it) and planner
const haveEngine = existsSync(library) && existsSync(cli);
if (!haveEngine) console.log(`Score import: real-engine checks skipped (no engine at ${library} and ${cli}).`);
else {
  const engineImport = (bytes, options) => JSON.parse(execFileSync(python, ['-c', `
import ctypes, json, sys
lib = ctypes.CDLL(sys.argv[1])
lib.novox_create.argtypes = [ctypes.c_double]; lib.novox_create.restype = ctypes.c_void_p
lib.novox_import.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]; lib.novox_import.restype = ctypes.c_void_p
lib.novox_last_error.argtypes = [ctypes.c_void_p]; lib.novox_last_error.restype = ctypes.c_char_p
lib.novox_free.argtypes = [ctypes.c_void_p]; lib.novox_destroy.argtypes = [ctypes.c_void_p]
data = sys.stdin.buffer.read()
e = lib.novox_create(48000)
p = lib.novox_import(e, data, len(data), sys.argv[2].encode())
print(ctypes.string_at(p).decode() if p else json.dumps({"error": lib.novox_last_error(e).decode()}))
if p: lib.novox_free(p)
lib.novox_destroy(e)
`, library, JSON.stringify(options)], { input: bytes, encoding: 'utf8' }));
  const plan = (score) => JSON.parse(execFileSync(cli, ['plan', '-'], { input: JSON.stringify(score), encoding: 'utf8', stdio: ['pipe', 'pipe', 'ignore'] }));
  // The song as the plugin sends it to the planner (Song::toScoreJson): note lyrics, no line.
  const scoreOf = (notes, bpm, lyrics = null) => ({ tempo: bpm, units: 'beats', voice: 'std', ...(lyrics === null ? {} : { lyrics }),
    notes: notes.map((n) => ({ pitch: n.pitch, start: n.start, dur: n.dur, ...(lyrics === null && n.lyric ? { lyric: n.lyric } : {}) })) });
  const asEditor = (o) => ({ units: 'beats', lyrics: "hel-lo, I'm a new vir-tual sing-er _", ...o });
  // Every note sings its own syllable ("_" holds the one before); the lyric box text flows back the same.
  const singsOwn = (song, bpm, what) => {
    const notes = song.notes.map((n, i) => ({ ...n, _id: i + 1 }));
    const p = plan(scoreOf(notes, bpm)), labels = labelNotes(notes, p, bpm);
    notes.forEach((n) => {
      const l = labels.get(n._id);
      if (n.lyric === '_') eq(l?.kind, 'hold', `${what}: "_" holds`);
      else if (n.lyric) eq([l?.kind, l?.text], ['word', n.lyric], `${what}: "${n.lyric}" on its own note`);
    });
    const text = lyricsFromNotes(notes, { labels });
    const flowed = plan(scoreOf(notes, bpm, text));
    const norm = (x) => x.syllables.map((s) => [s.start, s.end, s.phonemes, s.continues]);
    eq(norm(flowed), norm(p), `${what}: the lyric box "${text}" flows back onto the same notes`);
    return text;
  };
  const xml = (notes, tempo = 120) => `<?xml version="1.0" encoding="UTF-8"?><score-partwise version="4.0"><part-list><score-part id="P1">`
    + `<part-name>Voice</part-name></score-part></part-list><part id="P1"><measure number="1"><attributes><divisions>1</divisions></attributes>`
    + `<sound tempo="${tempo}"/>` + notes.map(([step, syllabic, text]) => `<note><pitch><step>${step}</step><octave>4</octave></pitch>`
    + `<duration>1</duration><voice>1</voice><lyric number="1"><syllabic>${syllabic}</syllabic><text>${text}</text></lyric></note>`).join('')
    + '</measure></part></score-partwise>';

  // "the fire burns our home": "fire" and "our" have two vowels and stay on one note each.
  const fire = engineImport(new TextEncoder().encode(xml([['C', 'single', 'the'], ['D', 'single', 'fire'], ['E', 'single', 'burns'],
    ['F', 'single', 'our'], ['G', 'single', 'home']])), asEditor({}));
  eq(fire.lyrics, undefined, 'the engine does not echo the line over a file with lyrics');
  const fireSong = importedSong(fire);
  eq(fireSong.notes.map((n) => n.lyric), ['the', 'fire', 'burns', 'our', 'home']);
  eq(singsOwn(fireSong, 120, 'fire'), 'the fire~ burns our~ home');

  // MIDI with lyric events and a melisma note without one (format 0, 480 ticks per beat).
  const vlq = (n) => { const out = [n & 0x7f]; while ((n >>= 7)) out.unshift((n & 0x7f) | 0x80); return out; };
  const events = [];
  const at = (delta, ...bytes) => events.push(...vlq(delta), ...bytes);
  const lyric = (text) => [0xff, 0x05, text.length, ...new TextEncoder().encode(text)];
  at(0, 0xff, 0x51, 3, 0x07, 0xa1, 0x20);  // 120 bpm
  const melody = [[60, 'the'], [62, 'fire'], [64, null], [65, 'burns'], [67, 'home']];
  for (const [pitch, text] of melody) {
    if (text) at(0, ...lyric(text));
    at(0, 0x90, pitch, 100);
    at(480, 0x80, pitch, 0);
  }
  at(0, 0xff, 0x2f, 0);
  const smf = new Uint8Array([...new TextEncoder().encode('MThd'), 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0,
    ...new TextEncoder().encode('MTrk'), 0, 0, (events.length >> 8) & 255, events.length & 255, ...events]);
  const midi = engineImport(smf, asEditor({}));
  ok(midi.import.format === 'midi' && midi.import.fileLyrics && midi.lyrics === undefined, 'MIDI lyrics are the file\'s own');
  const midiSong = importedSong(midi);
  eq(midiSong.notes.map((n) => n.lyric), ['the', 'fire', '_', 'burns', 'home'], 'a melisma note holds "fire"');
  singsOwn(midiSong, 120, 'midi');

  // The fixtures, as MuseScore and Dorico lay them out.
  for (const name of ['musescore-hymn.musicxml', 'dorico-carol.musicxml', 'musescore-coda.musicxml']) {
    const bytes = fixture(name), engine = engineImport(bytes, asEditor({})), m = await mock(bytes);
    ok(!engine.error, `${name}: ${engine.error}`);
    eq([m.import.part, m.import.partVoice, m.import.fileLyrics, m.lyrics], [engine.import.part, engine.import.partVoice, engine.import.fileLyrics, engine.lyrics],
      `${name}: the mock chooses like the engine`);
    eq([m.import.repeats, m.import.unfolded, m.import.verse, m.import.pickupBeats, m.import.timeSignatures[0]?.beats],
      [engine.import.repeats, engine.import.unfolded, engine.import.verse, engine.import.pickupBeats, engine.import.timeSignatures[0]?.beats],
      `${name}: the mock reports repeats, the verse, the pickup and the meter like the engine`);
    eq(m.import.parts.map((p) => [p.index, p.name, p.lyrics > 0, p.voices.map((v) => v.voice)]),
      engine.import.parts.map((p) => [p.index, p.name, p.lyrics > 0, p.voices.map((v) => v.voice)]), `${name}: the mock lists the parts like the engine`);
    const song = importedSong(engine);
    ok(song.noteLyrics && song.lyrics === null, `${name}: the notes keep the score's lyrics`);
    singsOwn(song, song.tempo, name);
    // Placed by time at the first tempo = the engine's own seconds at that tempo.
    const seconds = engineImport(bytes, { lyrics: '' });
    const first = engine.import.tempoMap[0].bpm;
    eq(song.tempo, first);
    seconds.notes.forEach((n, i) => {
      near(song.notes[i].start, n.start * first / 60, 1e-3, `${name}: note ${i} start`);
      near(song.notes[i].dur, n.dur * first / 60, 1e-3, `${name}: note ${i} length`);
    });
    // Each choice imports again with what the picker asks for.
    for (const [key, list] of Object.entries({ part: song.choices?.parts || [], voice: song.choices?.voices || [], verse: song.choices?.verses || [] })) {
      for (const option of list) {
        const again = engineImport(bytes, asEditor(importOptions(song.choices, key, option.value)));
        ok(!again.error, `${name}: ${key} ${option.value}: ${again.error}`);
        const want = { part: ['part', option.value], voice: ['partVoice', option.value], verse: ['verse', option.value] }[key];
        eq(again.import[want[0]], want[1], `${name}: ${key} ${option.label}`);
        const chosen = importedSong(again);
        ok(chosen.notes.length > 0 && (chosen.noteLyrics || chosen.lyrics === asEditor({}).lyrics), `${name}: ${key} ${option.label} sings`);
      }
    }
  }
  const hymn = importedSong(engineImport(fixture('musescore-hymn.musicxml'), asEditor({})));
  eq([hymn.tempo, hymn.timed], [66.67, true], 'the hymn slows to 60: placed by time at 66.67');
  eq(hymn.choices.parts.map((p) => p.label), ['Piano · no lyrics', 'Voice']);
  eq(hymn.choices.verses[0], { value: 0, label: 'Verse per repeat' });
  const compressed = engineImport(await zip([['x.musicxml', 'x']]), {});
  ok(/compressed MusicXML/.test(compressed.error), 'the engine itself asks for the plugin to unzip');
}

console.log(`Score import: lyrics kept one syllable per note, MIDI melismas, tempo placement, the part/voice/verse picker, ` +
  `LCD warnings and the mock importer passed (${checks} checks${haveEngine ? ', with the engine' : ''}).`);
