# Assets

The code in this repository is covered by [LICENSE](LICENSE) (MIT). This file covers everything that isn't code: where each asset came from, what you may do with it, and how to get the assets that aren't included.

## 1. Ours: included and open

Unless a row says otherwise, original art, music, sound, video, and 3D files made by Jonathan Mann for this project are licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Credit "Jonathan Mann (Song A Day)".

| Path | What it is | Made with | License |
| --- | --- | --- | --- |
| `assets/icons/` | App icon, robot art and singer portrait concepts | Jonathan Mann, with AI assistance (Claude) | CC BY 4.0 |

## 2. Free third-party: included under their own licenses

None yet.

## 3. Licensed (paid or restricted): not included

None.

## 4. Not ours and not licensed: never in the repo

These appear only in Jonathan's private copy, under parody or fair-use judgment, or they are personal reference material. Each one is listed so nobody wonders why it's missing. Everything under `reference/` and `out/` is gitignored.

| What | Why it's excluded |
| --- | --- |
| vocadito (`reference/datasets/vocadito/`): 40 solo a-cappella vocal tracks by 29 singers, with note, f0 and lyric annotations. Source: https://zenodo.org/records/5578259 (Bittner, Pasalo, Bosch, Meseguer-Brocal, Rubinstein, 2021). Used to evaluate the sung-vocal importer on other voices (`tools/datasets/vocadito.py`). | CC BY 4.0, so redistribution with credit is allowed, but evaluation data is never committed. Download it from Zenodo. Credit the authors when you publish results. |
| GTSinger, English part, a balanced sample (`reference/datasets/gtsinger/`): professional singers (EN-Alto-1, EN-Alto-2, EN-Tenor-1) singing phrases with breathy, glissando, mixed voice and falsetto, pharyngeal and vibrato technique, each with a plainly sung control take and a spoken take, with word, phoneme and note annotations and MusicXML scores. Source: https://huggingface.co/datasets/GTSinger/GTSinger (Zhang et al., NeurIPS 2024). Used to evaluate the sung-vocal importer on other voices and singing techniques (`tools/datasets/gtsinger.py`). | CC BY-NC-SA 4.0 plus the indemnity terms in the dataset's `dataset_license.md`, which Jonathan accepted. Local evaluation only: never commit, ship, or redistribute it or files derived from it (manifests, caches, plots). |
| Children's Song Dataset (CSD) v1.1, English half (`reference/datasets/csd/`): 50 children's songs, each sung in two keys by one trained female singer, with note annotations (onset, offset, MIDI pitch, romanised syllable labels), lyrics and MIDI. Source: https://zenodo.org/records/4916302 (Choi, Kim, Park, Yong, Nam, 2020). Used to evaluate the sung-vocal importer on whole songs by a higher female voice (`tools/datasets/csd.py`). | CC BY-NC-SA 4.0 (noncommercial, share-alike). Local evaluation only: never commit, ship, or redistribute it or files derived from it (manifests, caches, plots). Credit the authors when you publish results. |

## Never commit

Ripped songs or videos, press, stock, or Getty photos, photos or likenesses of real people, other NFT collections' art, screenshots of commercial software, the source files of paid asset packs, voice models with noncommercial terms, and personal data.
