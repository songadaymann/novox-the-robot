# Dataset adapters

Public singing datasets turned into manifests for the import lab (`tools/import_lab.py`). The engine was
tuned on one singer (Jonathan); these sets test the sung-vocal importer on other voices.

Datasets are never committed. Download them yourself into the gitignored `reference/datasets/<name>/`
folder. Each adapter writes its `manifest.json` and `splits.json` next to the data there, and the lab
caches go in a lab folder of their own under the gitignored `out/` (not `out/import-lab`, so they never
mix into the default corpus). The adapters read the datasets' CSV and text files (`csd.py` also analyzes
the audio, with librosa from `tools/.venv`) and run with `python -I`, since the dataset files are data.

| Adapter | Dataset | License | What it writes |
| --- | --- | --- | --- |
| `vocadito.py` | [vocadito](https://zenodo.org/records/5578259): 40 solo a-cappella tracks, 29 singers, 7 languages, notes from two annotators | CC BY 4.0 (credit Bittner et al., 2021) | One case per track (`voc-<n>`): singer, language, `english`, average pitch, lyric phrases, annotator A1 notes as the reference, A2 as `alternates.A2`; a singer-level dev/test split |
| `gtsinger.py` | [GTSinger](https://huggingface.co/datasets/GTSinger/GTSinger) English sample: 3 singers, 5 techniques, each with a plainly sung control take and a spoken take; word, phoneme and note annotations | CC BY-NC-SA 4.0 plus the indemnity terms of its `dataset_license.md`; local evaluation only | One case per sung segment (`gts-<singer>-<technique>-<song>-<group>-<NNNN>`): singer, voice, technique, group, condition (technique/control), syllable-level notes with lyrics as the reference, the raw word-level notes as `alternates.raw`; speech segments listed apart (`speechCases`, also `manifest-speech.json`); dev = EN-Alto-1 + EN-Tenor-1, test = EN-Alto-2 |
| `csd.py` | [Children's Song Dataset](https://zenodo.org/records/4916302) v1.1, English half: 50 children's songs, each sung in two keys by one trained female singer (whole songs, 1-2 min); notes with romanised syllable labels, lyrics, MIDI | CC BY-NC-SA 4.0; local evaluation only | One case per recording (`csd-en0NN{a,b}`): song, key, tempo, `gridShare`, singer `csd-f1`, notes with lyric pieces and words aligned from the lyric files, starts after unvoiced consonants moved to the vowel onset (CSD's own starts as `alternates.csd`); a song-level split (odd songs dev, even test) |

## vocadito

```bash
# 1. Download vocadito.zip from https://zenodo.org/records/5578259 into reference/datasets/vocadito/,
#    check its md5 against the Zenodo page, and extract it to reference/datasets/vocadito/extracted/.
tools/.venv/bin/python -I tools/datasets/vocadito.py reference/datasets/vocadito/extracted/zenodo_data

# 2. Cache every track through the exact plugin import (sequential; whisper.cpp uses the GPU).
for n in $(seq 1 40); do
  tools/.venv/bin/python -I tools/import_lab.py cache \
    reference/datasets/vocadito/extracted/zenodo_data/Audio/vocadito_$n.wav --id voc-$n \
    --manifest reference/datasets/vocadito/manifest.json --lab out/import-lab-vocadito
done

# 3. Optional: whisper.cpp words at repetition threshold 1.5, an experiment the plugin does not ship
#    (it keeps whisper's 2.4; see docs/SINGING-V2-DESIGN.md, Evaluation). These are engine inputs, so
#    --words-only may also write them for held-out cases; it scores nothing.
tools/.venv/bin/python -I tools/import_lab.py asr --variant entropy=1.5 --words-only \
  --lab out/import-lab-vocadito --splits reference/datasets/vocadito/splits.json

# 4. Score the dev split. The split file next to the manifest is used automatically.
M=reference/datasets/vocadito/manifest.json
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-vocadito --tag v1
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-vocadito \
  --options '{"algorithm":"singing-v2"}' --tag v2
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-vocadito \
  --options '{"algorithm":"singing-v2","detail":"detailed"}' --tag v2-detailed
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-vocadito \
  --options '{"algorithm":"singing-v2"}' --no-words --tag v2-melody
tools/.venv/bin/python -I tools/import_lab.py agree $M      # A2 vs A1: the human-agreement ceiling
```

The references are notes only, so the headline `SCORE` uses `gt.json` (A1) and `lyricW` is `-`. The
corpus output also has a per-singer table and `SCORE dev english=yes|no` lines. whisper.cpp runs
forced to English, so the words for non-English tracks are junk or English translations, which is what
a user singing in another language would get.

The split is by singer. Singers are sorted by average pitch and paired, and each pair puts one singer
in dev and one in test, so both splits run from low voices to high ones. English tracks fall in both
splits. `splits.json` records the rule. Test singers are held out: never tune on them, and never open
their plots or results.

Pass `--splits reference/datasets/vocadito/splits.json` to `run`, `compare` and `asr`, which have no
manifest. Without it they fall back to Jonathan's split file and print a warning.

## GTSinger

```bash
# 1. Get the English part of GTSinger from https://huggingface.co/datasets/GTSinger/GTSinger (accept its
#    license and dataset_license.md terms) into reference/datasets/gtsinger/English/<singer>/<technique>/...
tools/.venv/bin/python -I tools/datasets/gtsinger.py reference/datasets/gtsinger

# 2. Cache every segment (sung and speech) through the exact plugin import, sequentially (optionally
#    followed by the entropy-1.5 words experiment, as for vocadito).
M=reference/datasets/gtsinger/manifest.json
tools/.venv/bin/python -I tools/datasets/gtsinger.py --list $M | while IFS=$'\t' read -r id audio; do
  tools/.venv/bin/python -I tools/import_lab.py cache "$audio" --id "$id" --manifest $M --lab out/import-lab-gtsinger
done
tools/.venv/bin/python -I tools/import_lab.py asr --variant entropy=1.5 --words-only \
  --lab out/import-lab-gtsinger --splits reference/datasets/gtsinger/splits.json

# 3. Score the dev split (sung cases only), per technique and technique vs control.
BY="--by technique condition group singer"
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-gtsinger $BY --tag v1
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-gtsinger $BY \
  --options '{"algorithm":"singing-v2"}' --tag v2
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-gtsinger $BY \
  --options '{"algorithm":"singing-v2","detail":"detailed"}' --tag v2-detailed
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-gtsinger $BY \
  --options '{"algorithm":"singing-v2"}' --no-words --tag v2-melody
tools/.venv/bin/python -I tools/import_lab.py agree $M --alternate raw   # raw word-level notes vs the reference

# 4. What the importer does on speech (no reference: structure tables only).
tools/.venv/bin/python -I tools/import_lab.py corpus reference/datasets/gtsinger/manifest-speech.json \
  --lab out/import-lab-gtsinger --options '{"algorithm":"singing-v2"}' --by technique --tag speech-v2
```

GTSinger annotates notes per word: each word's notes tile it from its first consonant, and the MusicXML
gives the whole word as one lyric. The adapter turns them into the Novox convention: one note per sung
syllable (the word's vowel phones), starting at the syllable's vowel onset from the TextGrid phone tier;
a word's extra notes inside a vowel become `_` melisma notes at their own start; a note ends at the next
note's start unless a rest follows, then at the end of the word. Lyrics are lyric-sheet hyphenations
guessed from the spelling (`hel-`, `lo`), so `lyricW` (right word, any hyphenation) is the fairer lyric
number. The cases carry `lyric` and `word`, which `cache` keeps in `gt.json`, so the headline scores
against `gt.json` with lyric metrics (`gt=N` on the SCORE line).

Known limits of the annotation (checked against pyin on the dev split): octaves agree everywhere, but
about 10% of notes are more than 1 semitone from the sung pitch. Most of those are words GTSinger gave
fewer notes than syllables (`"lumped": true` on the note; the extra syllables are `"split": true` and
copy the pitch sounding at their vowel onset), a few are phone alignments that put a vowel in the wrong
place, and a few segments miss a sung word. Treat about 0.9 as the conpF ceiling.

The split is by singer: dev = EN-Alto-1 and EN-Tenor-1, test = EN-Alto-2. The test singer is held out:
never tune on it, and never open its plots or results. The plugin test binary's audition check fails on
segments that start with more than 1.5 s of silence (`exit 1 (... 1 failures)` in `cache`); the import
itself is unaffected.

## CSD

```bash
# 1. Download CSD.zip (v1.1) from https://zenodo.org/records/4916302 into reference/datasets/csd/, check its
#    md5 against the Zenodo page and extract it to reference/datasets/csd/extracted/ (CSD/english is used).
tools/.venv/bin/python -I tools/datasets/csd.py reference/datasets/csd/extracted/CSD/english   # ~6 min (pYIN)

# 2. Cache every recording through the exact plugin import, sequentially. The asr line is the optional
#    entropy-1.5 experiment; run it one case at a time (see Known problems).
M=reference/datasets/csd/manifest.json
tools/.venv/bin/python -I tools/datasets/csd.py --list $M | while IFS=$'\t' read -r id audio; do
  tools/.venv/bin/python -I tools/import_lab.py cache "$audio" --id "$id" --manifest $M --lab out/import-lab-csd
  tools/.venv/bin/python -I tools/import_lab.py asr "$id" --variant entropy=1.5 --words-only \
    --lab out/import-lab-csd --splits reference/datasets/csd/splits.json
done

# 3. Score the dev split.
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-csd --by key --tag v1
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-csd --by key \
  --options '{"algorithm":"singing-v2"}' --tag v2
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-csd --by key \
  --options '{"algorithm":"singing-v2","detail":"detailed"}' --tag v2-detailed
tools/.venv/bin/python -I tools/import_lab.py corpus $M --lab out/import-lab-csd --by key \
  --options '{"algorithm":"singing-v2"}' --no-words --tag v2-melody
tools/.venv/bin/python -I tools/import_lab.py agree $M --alternate csd   # CSD's own starts vs the refined ones
```

The references carry `lyric` and `word`, so the headline scores against `gt.json` with lyric metrics
(`gt=N` on the SCORE line). There is one singer, so `--by singer` says nothing; `--by key` compares the two
keys of each song.

Words. CSD labels each note with a romanised syllable (`b_ii`, `s_t_r_ei_n_z`, `oo` for a melisma vowel)
and gives the lyrics per line, with the same lines as the labels. The adapter assigns labels to words with
a small dynamic program over each line (syllable counts from `data/cmudict.dict`, first phones, vowel-only
labels as possible `_` continuations). On all 100 recordings, 34 lines cost 2 or more (sung reductions
such as "pia-no" or "mer-rily", and "know it" sung as one syllable, `n_oi_t`, which leaves "it" without a
note). Lyric pieces are lyric-sheet hyphenations, so `lyricW` is the fairer lyric number.

Onsets. CSD starts a note at its onset consonant, not at the vowel. On the dev recordings the frication
of `s`, `sh` and `f` and the closure of stops begin at CSD's start. After an unvoiced consonant, the
vowel's pYIN voicing onset comes a median 50 ms later (interquartile range 27-72 ms, up to about 200 ms for
long `s` and `sh`). For those syllables the adapter moves the start to that voicing onset, note by note,
within the consonant: 86% of dev's unvoiced-onset syllables move, and visual checks agree. Syllables that
start with a voiced consonant keep CSD's start, which is roughly 20-90 ms before the vowel. A detector
based on the rise of the 300-3000 Hz band was tried for them and dropped, because on the unvoiced syllables
it was less accurate than a constant offset (MAE about 40 ms against 27 ms). This early bias of the
voiced-consonant notes remains in the reference. Scoring CSD's own starts against the refined ones gives
`AGREE dev: conpF=0.981`, so at the 100 ms COnP tolerance the convention moves about 2% of notes.

Pitch is CSD's MIDI note as given. Against pYIN on 13 dev recordings (both keys), the reference has no
octave offset (0 octave frames), the tuning is within 0.2 semitones and there is no time lag over 20 ms.
About a third of the recordings (`gridShare` ≥ 0.95) have every start exactly on the tempo grid: the
singer sang to a click and these annotations are score positions. They still fit the audio within the
offsets above.

The split is by song: odd song numbers are dev, even ones are test, and both keys of a song stay together.
There is one singer, so the split holds out melodies and lyrics, not voices. Test songs are held out:
never tune on them, and never open their plots or results.

Known problems (as of 2026-10-16):

- Fixed in today's plugin build: whisper.cpp's DTW assert crash on `en012a` (test) and `en023b` (dev),
  and the engine error `invalid timestamped word` on `en011b` and `en047a/b` (dev, and at entropy 1.5
  also `en002a/b`, `en031a`, `en033a` and `en048b`). All 100 recordings now import.
- The lab caches still predate the fixes: `en011b`, `en012a`, `en023b` and `en047a/b` have no cached
  import (`corpus` lists them as failed), and `en031a`/`en033a` have no `words-entropy=1.5.json`. Re-run
  `cache` for them to complete the corpus. Note that `cache` writes `plugin-result.json` with the current
  plugin (singing-v2), while the other cases' plugin results are singing-v1, so the `=plugin` check of a
  v1 run won't hold for those cases.
- At entropy 1.5 whisper loops on several full songs (`en002a/b`, `en048b`, `en020a`); that is why the
  plugin keeps 2.4. Long loops can still happen at 2.4 on long inputs.
- `asr` stops at the first case whose import fails (an `AttributeError` on the `null` result), which is
  why step 2 runs it one case at a time.
- Caching takes 7-35 s per recording (whisper 2-23 s) and at most 1.1 GB of memory. An engine run on a
  127 s song takes under 3 s.
