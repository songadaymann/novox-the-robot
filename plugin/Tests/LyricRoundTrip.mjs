// node plugin/Tests/LyricRoundTrip.mjs [--novox build/novox] [--bpm 120] [--show N] [--score FILE] [TAG ...]
//
// Checks on real imports that the lyric text the editor writes for imported notes flows back onto
// the same notes. For every score.json an import-lab run stored under TAG (default s4-held-natural
// and s4-held-detailed, all four corpora), it does what the editor does with the engine's planner:
//   1. places the notes the way an audio import does (beats at --bpm) and plans them with their
//      own lyrics (useNoteLyrics on), then labels each note from that plan;
//   2. writes the lyric text from the notes and those labels (lyricsFromNotes);
//   3. plans the same notes again with their lyrics removed and the text flowing over them, which
//      is what editing the lyric box does;
// and requires every note to show the same lyric (labelNotes) and the two plans to sing the same
// syllables (times, phonemes, word continuation; a hum may read "" or "mm"). Synthetic cases run
// first: odd tokens, and typed lyrics pinned onto notes (as splitting a syllable does) and then
// written back. Needs the engine CLI (cmake --build build); the corpus part needs the lab outputs
// in out/import-lab* and is skipped without them. --score FILE checks one more import: a score.json,
// or the plugin's out/native-audio-review.json (NovoxPluginTests --audio), and prints its text.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync, readdirSync, readFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {labelNotes, lyricsFromNotes, pinnedLyric} from '../ui/lyric-text.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const CORPORA = ['out/import-lab', 'out/import-lab-vocadito', 'out/import-lab-gtsinger', 'out/import-lab-csd'];
const PARALLEL = 8;

const args = process.argv.slice(2);
const option = (name, fallback) => { const i = args.indexOf(name); if (i < 0) return fallback; const v = args[i + 1]; args.splice(i, 2); return v; };
const novox = resolve(ROOT, option('--novox', 'build/novox'));
const bpm = +option('--bpm', '120');
const show = +option('--show', '0');
const extra = option('--score', null);
const tags = args.length ? args : ['s4-held-natural', 's4-held-detailed'];

if (!existsSync(novox)) { console.log(`Lyric round trip: skipped (no engine CLI at ${novox})`); process.exit(0); }
const corpusCases = [];
for (const corpus of CORPORA) {
  const dir = join(ROOT, corpus);
  if (!existsSync(dir)) continue;
  for (const id of readdirSync(dir)) for (const tag of tags) {
    const file = join(dir, id, 'runs', tag, 'score.json');
    if (existsSync(file)) corpusCases.push({corpus: corpus.replace('out/', ''), id, tag, file});
  }
}

function plan(score) {
  return new Promise((done, fail) => {
    const p = spawn(novox, ['plan', '/dev/stdin']);
    let out = '', err = '';
    p.stdout.on('data', d => out += d);
    p.stderr.on('data', d => err += d);
    p.on('close', code => code ? fail(new Error(err || `novox exited ${code}`)) : done(JSON.parse(out)));
    p.stdin.end(JSON.stringify(score));
  });
}

// The score the plugin plans (Song::toScoreJson): beats at the host tempo, lyrics only when the
// notes do not carry their own.
const r6 = x => Math.round(x * 1e6) / 1e6;
const scoreOf = (notes, lyrics) => ({tempo: bpm, units: 'beats', voice: 'std', ...(lyrics === null ? {} : {lyrics}),
  notes: notes.map(n => ({pitch: n.pitch, start: r6(n.start), dur: r6(n.dur), ...(lyrics === null && n.lyric ? {lyric: n.lyric} : {})}))});

const sameLabel = (a, b) => a && b && a.kind === b.kind && a.text === b.text;
const humText = t => t === '' || /^\{?\s*m+\s*\}?$/i.test(t);
const sameSyllable = (a, b) => a.start === b.start && a.end === b.end && a.phonemes === b.phonemes && a.continues === b.continues &&
  (a.text === b.text || (humText(a.text) && humText(b.text) && a.phonemes.replace(/[\s|]/g, '') === 'M'));

// Notes as an audio import places them: seconds converted to beats at the host tempo.
function importedNotes(score) {
  const factor = bpm / 60;
  let id = 0;
  return score.notes.map(n => ({pitch: Math.round(n.pitch), start: n.start * factor, dur: n.dur * factor, lyric: n.lyric || '', _id: id++}))
    .sort((a, b) => a.start - b.start || a.pitch - b.pitch);
}

async function check(c) {
  const notes = c.notes || importedNotes(JSON.parse(readFileSync(c.file, 'utf8')));
  const planA = await plan(scoreOf(notes, null));
  const labelsA = labelNotes(notes, planA, bpm);
  const text = lyricsFromNotes(notes, {labels: labelsA}) ?? '';
  const planB = await plan(scoreOf(notes, text));
  const labelsB = labelNotes(notes, planB, bpm);
  const problems = [];
  notes.forEach((n, i) => {
    const a = labelsA.get(n._id), b = labelsB.get(n._id);
    if (!sameLabel(a, b)) problems.push(`note ${i} (${n.start.toFixed(2)} beats) "${n.lyric}": ${a?.kind} "${a?.text}" became ${b?.kind} "${b?.text}"`);
  });
  if (planA.syllables.length !== planB.syllables.length) problems.push(`${planA.syllables.length} syllables became ${planB.syllables.length}`);
  else planA.syllables.forEach((s, i) => { if (!sameSyllable(s, planB.syllables[i])) problems.push(`syllable ${i}: ${JSON.stringify(s)} became ${JSON.stringify(planB.syllables[i])}`); });
  for (const w of planB.warnings || []) if (/left over/.test(w)) problems.push(w);
  const marks = (text.match(/~/g) || []).length, holds = notes.filter(n => n.lyric === '_').length;
  const {notes: _, ...info} = c;
  return {...info, notes: notes.length, text, marks, holds, problems};
}

// Synthetic cases: tokens the importer may write, one note each, with a short rest every 4 notes.
const synth = (name, tokens) => ({corpus: 'synthetic', id: name, tag: '', notes: tokens.map((lyric, i) =>
  ({pitch: 60 + (i % 5), start: i * 0.5 + Math.floor(i / 4) * 0.25, dur: 0.5, lyric, _id: i}))});
const synthetic = [
  synth('tokens', ['', 'I', 'wanna', 'fire', 'our', 'sun-', '_', 'set', 'trea-', '_', 'sures', 're-', '_', 'member', 'sun-', '', 'day',
    'one', 'hun-', 'dred', 'for-', 'ty-', 'ni-', 'ners', 'Mm', '_', '{M}', "day's", '_', '_', 'à', 'tête', '']),
  synth('leading holds', ['_', '_', 'la', '', '', '_', 'la']),
];
// Typed lyrics flowed over notes, then pinned the way the editor does before splitting or joining
// syllables: pinning must not change what is sung, and the text written back must round-trip.
for (const typed of ['I remember the singer and the canoe _ wanna go', 'hel-lo, I\'m a new vir-tual sing-er _ every hour on the hour']) {
  const c = synth('pinned: ' + typed, Array(24).fill(''));
  const flowed = await plan(scoreOf(c.notes, typed));
  const flowLabels = labelNotes(c.notes, flowed, bpm);
  c.notes.forEach(n => { n.lyric = pinnedLyric(flowLabels.get(n._id)); });
  const pinnedLabels = labelNotes(c.notes, await plan(scoreOf(c.notes, null)), bpm);
  c.pinProblems = c.notes.filter(n => flowLabels.get(n._id)?.text !== pinnedLabels.get(n._id)?.text || flowLabels.get(n._id)?.kind !== pinnedLabels.get(n._id)?.kind)
    .map(n => `pinning changed note ${n._id} from "${flowLabels.get(n._id)?.text}" to "${pinnedLabels.get(n._id)?.text}"`);
  synthetic.push(c);
}
if (extra) {
  const json = JSON.parse(readFileSync(resolve(extra), 'utf8'));
  synthetic.push({corpus: 'file', id: extra, tag: '', notes: importedNotes(json.score || json)});
}
const cases = [...synthetic, ...corpusCases];
if (!corpusCases.length) console.log(`Lyric round trip: no corpus score.json for ${tags.join(', ')}; synthetic cases only`);

const results = [];
let next = 0;
await Promise.all(Array.from({length: PARALLEL}, async () => {
  while (next < cases.length) { const c = cases[next++]; const r = await check(c); r.problems.unshift(...(c.pinProblems || [])); results.push(r); }
}));
results.sort((a, b) => (a.corpus + a.id + a.tag).localeCompare(b.corpus + b.id + b.tag));

const groups = new Map();
for (const r of results) {
  const key = `${r.corpus} ${r.tag}`;
  const g = groups.get(key) || {cases: 0, notes: 0, failed: 0, marks: 0, holds: 0};
  g.cases++; g.notes += r.notes; g.marks += r.marks; g.holds += r.holds; g.failed += r.problems.length ? 1 : 0;
  groups.set(key, g);
}
for (const [key, g] of groups) console.log(`${key.padEnd(40)} ${String(g.cases).padStart(4)} cases ${String(g.notes).padStart(6)} notes  ${g.holds} "_"  ${g.marks} "~"  ${g.failed} failed`);
for (const r of results.filter(r => r.problems.length)) {
  console.log(`\nFAIL ${r.corpus}/${r.id} ${r.tag}\n  ${r.problems.slice(0, 6).join('\n  ')}`);
}
for (const r of results.filter(r => r.corpus === 'synthetic' || r.corpus === 'file').concat(results.filter(r => r.corpus !== 'synthetic').slice(0, show)))
  console.log(`\n${r.corpus}/${r.id} ${r.tag}:\n${r.text}`);
const failed = results.filter(r => r.problems.length).length;
assert.equal(failed, 0, `${failed} of ${results.length} imports did not round-trip`);
console.log(`Lyric round trip: ${results.length} imports, ${results.reduce((s, r) => s + r.notes, 0)} notes, every note kept its lyric`);
