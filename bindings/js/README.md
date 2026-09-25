# new-virtual-singer

A formant singing synthesizer in WebAssembly: give it lyrics and a melody and it sings, in the
robotic-charming spirit of Myriad's classic Virtual Singer. Works in browsers, workers and Node.

```js
import { createSinger } from "new-virtual-singer";

const singer = await createSinger({ sampleRate: 48000 });
const audio = singer.render({
  tempo: 96,
  voice: "robot",                       // or { preset: "female", vibratoDepth: 0.5 }
  lyrics: "hel-lo, my name is Hugh",     // words flow onto notes by syllable
  notes: ["E4 0.5", "G4 1", "r 0.5", "G4 0.5", "A4 0.5", "G4 0.5", "E4 1.5"],
});
// audio is a Float32Array (mono, peak-normalized)

const wav = singer.toWav(audio);                         // Uint8Array
const score = singer.importFile(midiBytes, { lyrics: "hap-py birth-day" });  // MIDI / MusicXML
const stream = singer.stream(score);                     // incremental rendering
const chunk = stream.process(128);                       // e.g. inside an AudioWorklet
```

Command line:

```bash
npx new-virtual-singer say "hello world" -o hello.wav --voice robot --pitch D3
npx new-virtual-singer render song.mid --lyrics "..." -o song.wav
```

Lyric notation: `hel-lo` splits syllables, `_` holds the previous vowel over the next note,
`{HH EH L OW}` spells phonemes (ARPAbet) directly, and a note without a lyric hums.

MIT licensed. The CMU Pronouncing Dictionary is compiled in (BSD-style license).
