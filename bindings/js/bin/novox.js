#!/usr/bin/env node
// npx novox say "hello world" -o hello.wav --voice robot --pitch D3
// npx novox render song.mid --lyrics "..." -o song.wav
// npx novox import song.musicxml [--track N] [--part-voice V] [--verse N] [--no-unfold]
import { readFileSync, writeFileSync } from "node:fs";
import { createSinger } from "../index.js";

const args = process.argv.slice(2);
const cmd = args.shift();
const opt = (name, def) => {
  const i = args.indexOf(name);
  if (i < 0) return def;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
};
const out = opt("-o", "out.wav");
const voice = opt("--voice", "");
const pitch = opt("--pitch", "C4");
const lyrics = opt("--lyrics", "");
const rate = Number(opt("--rate", "48000"));
const track = Number(opt("--track", "-1"));
const partVoice = opt("--part-voice", "");
const verse = Number(opt("--verse", "0"));
const flag = (name) => {
  const i = args.indexOf(name);
  if (i >= 0) args.splice(i, 1);
  return i >= 0;
};
const unfold = !flag("--no-unfold");
const input = args[0];

const singer = await createSinger({ sampleRate: rate });
const VOWELS = new Set("AA AE AH AO AW AX AY EH ER EY IH IY OW OY UH UW".split(" "));

let score;
if (cmd === "voices") {
  console.log(singer.voices().join("\n"));
  process.exit(0);
} else if (cmd === "phonemes") {
  console.log(singer.phonemize(input));
  process.exit(0);
} else if (cmd === "say") {
  const n = singer.phonemize(input).split(/\s+/).filter((s) => VOWELS.has(s)).length;
  score = { tempo: 100, lyrics: input, notes: Array(Math.max(1, n)).fill(`${pitch} 1`) };
} else if (cmd === "render" || cmd === "import") {
  const bytes = readFileSync(input);
  const isJson = bytes.toString("utf8").trimStart().startsWith("{");
  // A MIDI or MusicXML file's own lyrics win; --lyrics fills in for a file without any.
  score = isJson
    ? JSON.parse(bytes.toString("utf8").split("\n").filter((l) => !l.trim().startsWith("//")).join("\n"))
    : singer.importFile(bytes, { lyrics, track, partVoice, verse, unfold });
  if (lyrics && isJson) score.lyrics = lyrics;
  if (cmd === "import") {
    console.log(JSON.stringify(score, null, 1));
    process.exit(0);
  }
} else {
  console.error("usage: novox say|render|import|phonemes|voices ... [-o out.wav] [--voice NAME]");
  process.exit(1);
}
if (voice) score.voice = voice;
const audio = singer.render(score);
writeFileSync(out, singer.toWav(audio));
console.error(`wrote ${out} (${(audio.length / rate).toFixed(2)} s)`);
