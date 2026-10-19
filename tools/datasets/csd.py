"""Children's Song Dataset (English) -> import-lab manifest and song-level dev/test split.

CSD (Choi, Kim, Park, Yong, Nam, 2020; https://zenodo.org/records/4916302, v1.1, CC BY-NC-SA 4.0) has 50
English and 50 Korean children's songs, each sung twice (keys "a" and "b", usually transposed: metadata
key_diff) by one trained female singer, with note annotations (csv: start, end, MIDI pitch and a
romanised phonetic syllable label such as 'b_ii'), the syllable labels per line (txt/), the lyrics per
line (lyric/) and MIDI. Download it yourself (it is never committed; local evaluation only), check its md5
against the Zenodo page and extract it, e.g. to reference/datasets/csd/extracted/, then:

    tools/.venv/bin/python -I tools/datasets/csd.py reference/datasets/csd/extracted/CSD/english

DATASET_ROOT is the folder holding metadata.json, wav/, csv/, txt/ and lyric/ (CSD/english). Writes,
next to each other (default folder reference/datasets/csd/, gitignored; --out-dir to change):

  manifest.json  one case per recording, as tools/import_lab.py reads it:
                   id            "csd-en0NN{a,b}"
                   audio         path of wav/en0NN{a,b}.wav, relative to the manifest
                   song (int), songName, key ("a"/"b"), keyDiff, tempo, timeSignature (metadata.json),
                   singer "csd-f1" (one singer), language "English", english true, averagePitch
                   (duration-weighted mean reference pitch, MIDI), gridShare (share of CSD note starts
                   exactly on a 16th/triplet grid of the tempo: 35 of the 100 recordings are at 0.95 or
                   more, i.e. their starts are score positions, not audio times; the singer sang to a
                   click, so they still fit the audio within the measured offsets), lyrics (the lyric lines)
                   notes         the reference (see Notes)
                   alignment     word-alignment cost of the song and its notable lines (see Words)
                   alternates    {"csd": {"notes": [...]}} the notes with CSD's own start times (no onset
                                 refinement), for `import_lab.py agree MANIFEST --alternate csd`
  splits.json    song-level split (SPLIT_RULE): odd song numbers dev, even test (held out); both keys
                 of a song stay together.

Notes (one per CSD note, in the Novox convention)
  pitch = CSD's MIDI pitch as given (the "b" keys are already transposed). Each note has "label" (CSD's
  syllable label), "word" (the lyric word it belongs to), "lyric" (engine style: the syllable's piece
  of the word with a trailing '-' unless it ends the word, '_' for a melisma continuation), "csdStart"
  (CSD's start) and "onsetShift" (start - csdStart, s).
  Start: the Novox convention is the syllable's VOWEL onset. CSD starts a note at (or early in) its
  onset consonant: on spectrograms the frication of 's', 'sh', 'f' and the closure of stops begin at CSD's
  start, and the vowel's pYIN voicing onset after an unvoiced consonant comes a median ~50 ms later (up to
  ~180 ms for 'sh'; see tools/datasets/README.md). For a syllable whose last onset consonant is unvoiced
  (s, sh, f, th, h, p, t, k, ch) the start moves to that voicing onset, found per note and limited to the
  consonant span: the first frame in [csdStart - PRE_WINDOW, csdStart + span] that follows UNVOICED_RUN
  unvoiced pYIN frames (16 kHz, FRAME hop) and starts VOICED_RUN voiced ones, whose unvoiced stretch begins
  by csdStart + CONSONANT_LATE (so a voicing break inside the vowel is never taken); span = SPAN_BASE +
  SPAN_PER_CONSONANT per extra onset consonant, at most SPAN_SHARE of the note; never earlier than
  csdStart. Voiced onset consonants (b, d, g, j, l, m, n, r, v, w, y, z, dh) keep CSD's start, which is
  then roughly 20-90 ms before the vowel: a vowel-band level-rise detector tried for them disagreed with
  the voicing onset of unvoiced syllables more (MAE ~40 ms) than a constant offset did (~27 ms), so it was
  dropped. Vowel-initial syllables and melisma continuations keep CSD's start. A legato previous note (its
  end at csdStart) is extended to the new start. --no-refine keeps every CSD start (no audio is read;
  standard library only).
  End: CSD's end.

Words (CSD has no per-note words; they are derived)
  The lyric lines and the syllable-label lines correspond one to one. Within a line, each word takes
  one or more consecutive labels by a dynamic program: a word's syllable count should be one of its
  pronunciations' vowel counts (data/cmudict.dict, the engine's dictionary; a spelling estimate for
  unknown words), its first label's onset should be the pronunciation's first phone, and a later label
  that is only a vowel (no onset consonant, e.g. 'oo' after 'g_l_oo', or 'i' after 'd_ei') may be a melisma
  continuation of the syllable before it ('_'). A word can also get no label of its own ("know it" sung
  'n_oi_t'; listed in the alignment problems), at a high cost. Lyric pieces are lyric-sheet hyphenations
  of the word as written (letter vowel groups, cut so the next piece starts with as many consonants as
  the next label's onset has); lyricWordAccuracy also accepts the right word hyphenated differently.

Only the standard library is used for the words; the onset refinement uses librosa's pYIN (run it with
the tools/.venv python and -I: the dataset files are data; it takes ~6 minutes for the 100 recordings).
--list prints "id<TAB>audio path" for every case for the cache loop in tools/datasets/README.md.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from pathlib import Path

DATASET = "csd"
SINGER = "csd-f1"
SOURCE = {"name": "Children's Song Dataset (CSD) v1.1", "url": "https://zenodo.org/records/4916302",
          "doi": "10.5281/zenodo.4916302", "license": "CC BY-NC-SA 4.0",
          "citation": "S. Choi, W. Kim, S. Park, S. Yong, J. Nam, 'Children's Song Dataset for Singing Voice "
                      "Research', ISMIR Late-Breaking Demo, 2020"}
SPLIT_RULE = ("Song-level split, made by tools/datasets/csd.py: odd song numbers (en001, en003, ...) are dev, even "
              "ones are test; both keys (a, b) of a song stay in the same split. One singer sings everything, so this "
              "split holds out songs (melodies, lyrics, tempi), not voices. Test songs are held out: never tune on "
              "them, never open their plots or results.")
ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DICT = ROOT / "data" / "cmudict.dict"

# CSD's syllable labels: phones joined by '_', exactly one vowel each.
CSD_VOWELS = {"a", "ae", "ai", "ao", "au", "e", "ei", "eo", "er", "i", "ii", "oi", "oo", "ou", "u", "uu"}
UNVOICED = {"ch", "f", "h", "k", "p", "s", "sh", "t", "th"}
ARPA_CONSONANTS = {"B": "b", "CH": "ch", "D": "d", "DH": "dh", "F": "f", "G": "g", "HH": "h", "JH": "j", "K": "k",
                   "L": "l", "M": "m", "N": "n", "NG": "ng", "P": "p", "R": "r", "S": "s", "SH": "sh", "T": "t",
                   "TH": "th", "V": "v", "W": "w", "Y": "y", "Z": "z", "ZH": "zh"}
ARPA_VOWELS = {"AA", "AE", "AH", "AO", "AW", "AX", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"}

# Word alignment costs (per word).
COST_SYLLABLE = 1.0       # per syllable more than the nearest pronunciation's count
COST_REDUCED = 0.6        # per syllable fewer (sung reductions are common: 'pia-no', 'iron', 'mer-rily')
COST_ONSET = 1.5          # first label's onset is not the word's first phone
COST_SAME_VOWEL = 0.02    # a melisma continuation repeating the syllable's vowel ('oo' after 'g_l_oo')
COST_OTHER_VOWEL = 0.3    # a continuation with another vowel (a diphthong's glide 'i' after 'd_ei')
COST_ELIDED = 2.5         # a word with no label of its own
MAX_LABELS_PER_WORD = 32
NOTABLE_LINE_COST = 2.0   # lines at or above this alignment cost are listed in the case's "alignment"

# Onset refinement (see Notes).
RATE = 16000              # Hz, the analysis rate (the plugin's)
FRAME = 0.005             # s, pYIN hop
PYIN_FRAME = 512          # samples (32 ms)
FMIN, FMAX = 100.0, 1100.0
VOICED = 0.5              # pYIN voiced probability
UNVOICED_RUN = 4          # frames (20 ms) of the consonant before the onset
VOICED_RUN = 6            # frames (30 ms) of sustained voicing after it
PRE_WINDOW = 0.030        # s searched before csdStart
SPAN_BASE = 0.200         # s: consonant span after csdStart for one onset consonant ('sh' runs to ~180 ms)
SPAN_PER_CONSONANT = 0.050
SPAN_SHARE = 0.7          # of the note's CSD duration
CONSONANT_LATE = 0.050    # s: the consonant's unvoiced stretch must begin by csdStart + this
LEGATO = 0.001            # s: a previous note ending this close to csdStart is legato


# ---------------------------------------------------------------------------
# Dictionary and words

def read_dictionary(path: Path) -> dict[str, list[list[str]]]:
    """cmudict: word -> pronunciations (ARPAbet phones without stress digits)."""
    out: dict[str, list[list[str]]] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(";;;"):
            continue
        line = line.split("#", 1)[0].strip()
        parts = line.split()
        if len(parts) < 2:
            continue
        word = re.sub(r"\(\d+\)$", "", parts[0])
        out.setdefault(word, []).append([re.sub(r"\d", "", p) for p in parts[1:]])
    return out


def clean_word(text: str) -> str:
    """The word as written: letters and apostrophes (curly ones made straight), outer apostrophes dropped."""
    text = text.replace("’", "'").replace("‘", "'")
    return "".join(c for c in text if c.isalpha() or c == "'").strip("'")


def spelling_syllables(word: str) -> int:
    w = re.sub(r"^y", "j", word.lower().replace("'", ""))
    n = len(re.findall(r"[aeiouy]+", w))
    if n > 1 and re.search(r"[^aeiouy]e$", w) and not w.endswith("le"):
        n -= 1
    if n > 1 and re.search(r"[^aeiouytd]ed$", w):
        n -= 1
    if n > 1 and re.search(r"[aeiouy][^aeiouyscxzgh]es$", w):
        n -= 1
    return max(1, n)


def word_info(word: str, dictionary: dict) -> tuple[set[int], set[str | None]]:
    """Possible syllable counts and first phones (CSD names; None = a vowel) of a word."""
    key = word.lower()
    prons = dictionary.get(key) or dictionary.get(key.replace("'", ""))
    if prons:
        counts = {sum(1 for p in pron if p in ARPA_VOWELS) or 1 for pron in prons}
        firsts = {None if pron[0] in ARPA_VOWELS else ARPA_CONSONANTS.get(pron[0], pron[0].lower()) for pron in prons}
        return counts, firsts
    firsts = spelling_onsets(key)
    return {spelling_syllables(word)}, firsts


SPELLING_ONSETS = [("wh", {"w", "h"}), ("wr", {"r"}), ("kn", {"n"}), ("gn", {"n"}), ("ph", {"f"}), ("th", {"th", "dh"}),
                   ("sh", {"sh"}), ("sch", {"s", "sh"}), ("ch", {"ch", "k", "sh"}), ("qu", {"k"}), ("x", {"z", None}),
                   ("b", {"b"}), ("c", {"k", "s", "ch"}), ("d", {"d", "j"}), ("f", {"f"}), ("g", {"g", "j"}),
                   ("h", {"h", None}), ("j", {"j", "y"}), ("k", {"k"}), ("l", {"l"}), ("m", {"m"}), ("n", {"n"}),
                   ("p", {"p"}), ("r", {"r"}), ("s", {"s", "sh", "z"}), ("t", {"t", "ch"}), ("v", {"v"}),
                   ("w", {"w"}), ("y", {"y", None}), ("z", {"z"})]


def spelling_onsets(word: str) -> set[str | None]:
    for prefix, onsets in SPELLING_ONSETS:
        if word.startswith(prefix):
            return onsets
    return {None, "y", "w"}


def label_parts(label: str) -> tuple[list[str], str, list[str]]:
    """Onset consonants, vowel, coda consonants of a CSD syllable label."""
    phones = label.split("_")
    v = next((i for i, p in enumerate(phones) if p in CSD_VOWELS), None)
    if v is None:
        return phones, "", []
    return phones[:v], phones[v], phones[v + 1:]


def continuation_cost(prev: str, label: str) -> float | None:
    """Cost of `label` being a melisma continuation of the syllable `prev` (None: it cannot be)."""
    onset, vowel, _ = label_parts(label)
    if onset or not vowel:
        return None
    return COST_SAME_VOWEL if vowel == label_parts(prev)[1] else COST_OTHER_VOWEL


def segment_cost(word_counts: set[int], word_onsets: set, labels: list[str]) -> tuple[float, list[bool]]:
    """Cost of one word taking these labels, and which of them are syllable heads (the rest are '_')."""
    first_onset = label_parts(labels[0])[0]
    onset_ok = (first_onset[0] if first_onset else None) in word_onsets
    cost = 0.0 if onset_ok else COST_ONSET
    options = [(continuation_cost(labels[j - 1], labels[j]), j) for j in range(1, len(labels))]
    optional = sorted((c, j) for c, j in options if c is not None)
    must = len(labels) - len(optional)          # heads that cannot be continuations
    best = None
    for k in range(len(optional) + 1):          # k optional labels become continuations
        heads = len(labels) - k
        syllable_cost = min((n - heads) * COST_REDUCED if heads < n else (heads - n) * COST_SYLLABLE
                            for n in word_counts)
        total = syllable_cost + sum(c for c, _ in optional[:k])
        if best is None or total < best[0] - 1e-12:
            best = (total, {j for _, j in optional[:k]})
        if heads <= must:
            break
    extra, continuations = best
    return cost + extra, [j not in continuations for j in range(len(labels))]


def align_line(words: list[str], labels: list[str], dictionary: dict) -> tuple[float, list[tuple[int, bool]], list[int]]:
    """Assigns the labels of one line to its words. Returns the cost, (word index, is head) per label and
    the indices of words that got no label."""
    infos = [word_info(w, dictionary) for w in words]
    W, T = len(words), len(labels)
    inf = float("inf")
    best = [[inf] * (T + 1) for _ in range(W + 1)]
    back: list[list[tuple | None]] = [[None] * (T + 1) for _ in range(W + 1)]
    best[0][0] = 0.0
    for wi in range(W):
        counts, onsets = infos[wi]
        for a in range(T + 1):
            if best[wi][a] == inf:
                continue
            if best[wi][a] + COST_ELIDED < best[wi + 1][a]:      # no label of its own
                best[wi + 1][a] = best[wi][a] + COST_ELIDED
                back[wi + 1][a] = (a, None)
            for b in range(a + 1, min(T, a + MAX_LABELS_PER_WORD) + 1):
                cost, heads = segment_cost(counts, onsets, labels[a:b])
                if best[wi][a] + cost < best[wi + 1][b]:
                    best[wi + 1][b] = best[wi][a] + cost
                    back[wi + 1][b] = (a, heads)
    assignment: list[tuple[int, bool]] = [(-1, True)] * T
    elided, b = [], T
    for wi in range(W, 0, -1):
        a, heads = back[wi][b]
        if heads is None:
            elided.append(wi - 1)
        else:
            for j, head in enumerate(heads):
                assignment[a + j] = (wi - 1, head)
        b = a
    return best[W][T], assignment, sorted(elided)


# ---------------------------------------------------------------------------
# Hyphenation

DIGRAPHS = ("ch", "sh", "th", "ph", "wh", "gh", "qu")
HIATUS = ("ia", "io", "iu", "ea", "eo", "oe", "ua", "uo", "ue", "oa", "ie", "ii")
NEVER_START = ("ck", "x")


def _consonant_units(cluster: str) -> list[str]:
    units, i = [], 0
    while i < len(cluster):
        two = cluster[i:i + 2].lower()
        if two in DIGRAPHS or two == "ck":
            units.append(cluster[i:i + 2])
            i += 2
        else:
            units.append(cluster[i])
            i += 1
    return units


def _nuclei(word: str) -> list[tuple[int, int]]:
    """Letter spans of the vowel groups that can carry a syllable."""
    low = word.lower()
    vowel = [c in "aeiou" or (c == "y" and i > 0 and not (i + 1 < len(low) and low[i + 1] in "aeiou"))
             for i, c in enumerate(low)]
    groups, i = [], 0
    while i < len(low):
        if vowel[i]:
            j = i
            while j < len(low) and vowel[j]:
                j += 1
            groups.append((i, j))
            i = j
        else:
            i += 1
    return groups


def hyphenate(word: str, count: int, onsets: list[int]) -> list[str]:
    """Splits `word` into `count` pieces; onsets[k] is the onset length (phones) of syllable k+1."""
    if count <= 1:
        return [word]
    letters = [i for i, c in enumerate(word) if c.isalpha()]
    if len(letters) < count:               # "W" sung as three syllables: letters first, then empty pieces
        return [word] + [""] * (count - 1)
    groups = _nuclei(word)
    low = word.lower()
    # Too many vowel groups: silent final e, then -es/-ed, then merge the closest pair.
    while len(groups) > count:
        last = groups[-1]
        tail = low[last[0]:]
        if len(groups) > 1 and (tail == "e" or tail in ("es", "ed")) and last[0] > 0 and low[last[0] - 1] not in "aeiouy":
            groups.pop()
            continue
        gaps = [(groups[k + 1][0] - groups[k][1], k) for k in range(len(groups) - 1)]
        _, k = min(gaps)
        groups[k:k + 2] = [(groups[k][0], groups[k + 1][1])]
    # Too few: split hiatus vowel pairs ("pi-a-no", "li-on").
    changed = True
    while len(groups) < count and changed:
        changed = False
        for k, (a, b) in enumerate(groups):
            for p in range(a, b - 1):
                if low[p:p + 2] in HIATUS:
                    groups[k:k + 1] = [(a, p + 1), (p + 1, b)]
                    changed = True
                    break
            if changed or len(groups) >= count:
                break
    if len(groups) < count:                # still short: whole word on the first, then empty pieces
        return [word] + [""] * (count - 1)
    cuts = []
    for k in range(count - 1):
        start, end = groups[k][1], groups[k + 1][0]
        units = _consonant_units(word[start:end])
        take = min(max(onsets[k] if k < len(onsets) else 1, 0), len(units))
        if take and units and any(units[len(units) - take].lower().startswith(n) for n in NEVER_START):
            take -= 1
        if k == count - 2 and re.search(r"[^aeiouy]l(e|es|ed)$", low) and groups[k + 1][0] == len(low) - len(
                re.search(r"(e|es|ed)$", low).group(0)) and len(units) >= 2:
            take = 2                       # consonant + "le" starts the last piece: trou-bles, whis-tle
        cuts.append(end - sum(len(u) for u in units[len(units) - take:]))
    pieces, prev = [], 0
    for cut in cuts:
        pieces.append(word[prev:cut])
        prev = cut
    pieces.append(word[prev:])
    if any(not any(c.isalpha() for c in p) for p in pieces):
        return [word] + [""] * (count - 1)
    return pieces


# ---------------------------------------------------------------------------
# Onset refinement

def voicing(audio: Path):
    """pYIN voiced probability every FRAME seconds (frame k centred at k * FRAME) of the mono 16 kHz audio."""
    import librosa  # tools/.venv
    y, _ = librosa.load(str(audio), sr=RATE, mono=True)
    hop = int(round(FRAME * RATE))
    _, _, probability = librosa.pyin(y, fmin=FMIN, fmax=FMAX, sr=RATE, frame_length=PYIN_FRAME, hop_length=hop)
    return probability > VOICED


def vowel_onset(voiced, start: float, end: float, onset_len: int) -> float | None:
    """The vowel onset of a syllable with an unvoiced onset consonant whose CSD note starts at `start`: the
    first voicing onset (UNVOICED_RUN unvoiced frames, then VOICED_RUN voiced ones) in [start - PRE_WINDOW,
    start + span] whose unvoiced stretch (the consonant) begins by start + CONSONANT_LATE (see Notes); None
    when there is none or it is not after `start`."""
    span = min(SPAN_BASE + SPAN_PER_CONSONANT * max(0, onset_len - 1), SPAN_SHARE * (end - start))
    lo = max(UNVOICED_RUN, int(round((start - PRE_WINDOW) / FRAME)))
    hi = min(len(voiced) - VOICED_RUN, int(round((start + span) / FRAME)))
    for k in range(lo, hi + 1):
        if voiced[k] and not voiced[k - UNVOICED_RUN:k].any() and voiced[k:k + VOICED_RUN].all():
            first = k - UNVOICED_RUN
            while first > 0 and not voiced[first - 1]:
                first -= 1
            if first * FRAME > start + CONSONANT_LATE:   # a voicing break inside the vowel, not the consonant
                return None
            onset = k * FRAME
            return onset if onset > start else None
    return None


# ---------------------------------------------------------------------------
# Build

def read_lines(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace").replace("﻿", "").replace("​", "")
    return [line.strip() for line in text.splitlines() if line.strip()]


def read_notes(path: Path) -> tuple[list[dict], int]:
    notes, skipped = [], 0
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                start, end, pitch = float(row["start"]), float(row["end"]), float(row["pitch"])
                label = (row.get("syllable") or "").strip()
            except (KeyError, TypeError, ValueError):
                skipped += 1
                continue
            if not (end > start >= 0) or not label:
                skipped += 1
                continue
            notes.append({"start": start, "end": end, "pitch": int(round(pitch)), "label": label})
    return notes, skipped


def grid_share(notes: list[dict], tempo: float) -> float:
    beat = 60.0 / tempo if tempo else 0
    if not beat or not notes:
        return 0.0
    on = 0
    for n in notes:
        for division in (4, 12):
            step = beat / division
            x = n.get("csdStart", n["start"]) / step
            if abs(x - round(x)) * step < 0.002:
                on += 1
                break
    return round(on / len(notes), 3)


def words_for(notes: list[dict], label_lines: list[list[str]], lyric_lines: list[list[str]], dictionary: dict,
              problems: list[str], name: str) -> dict:
    """Sets word, lyric and head on every note; returns the case's alignment summary."""
    flat = [lab for line in label_lines for lab in line]
    if flat != [n["label"] for n in notes]:
        problems.append(f"{name}: txt/ labels differ from csv/ labels; words aligned over the whole song")
        label_lines = [[n["label"] for n in notes]]
        lyric_lines = [[w for line in lyric_lines for w in line]]
    elif len(label_lines) != len(lyric_lines):
        problems.append(f"{name}: {len(label_lines)} label lines but {len(lyric_lines)} lyric lines; "
                        "words aligned over the whole song")
        label_lines = [[lab for line in label_lines for lab in line]]
        lyric_lines = [[w for line in lyric_lines for w in line]]
    summary = {"cost": 0.0, "lines": [], "elided": []}
    index = 0
    for line_no, (labels, words) in enumerate(zip(label_lines, lyric_lines)):
        cost, assignment, elided = align_line(words, labels, dictionary)
        summary["cost"] += cost
        line_notes = notes[index:index + len(labels)]
        index += len(labels)
        by_word: dict[int, list[int]] = {}
        for j, (wi, head) in enumerate(assignment):
            line_notes[j]["word"] = words[wi]
            line_notes[j]["head"] = head
            by_word.setdefault(wi, []).append(j)
        for wi, js in by_word.items():
            heads = [j for j in js if line_notes[j]["head"]]
            onsets = [len(label_parts(line_notes[j]["label"])[0]) for j in heads[1:]]
            pieces = hyphenate(words[wi], len(heads), onsets)
            for k, j in enumerate(heads):
                line_notes[j]["lyric"] = pieces[k] + ("-" if k + 1 < len(heads) and pieces[k] else "")
            for j in js:
                if not line_notes[j]["head"]:
                    line_notes[j]["lyric"] = "_"
        for wi in elided:
            summary["elided"].append(words[wi])
            # the label sung for it is the previous word's (or the next one's): note it there
            target = max((j for j, (w, _) in enumerate(assignment) if w < wi), default=0)
            line_notes[target].setdefault("elided", []).append(words[wi])
        if cost >= NOTABLE_LINE_COST or elided:
            summary["lines"].append({"line": line_no + 1, "cost": round(cost, 2), "words": " ".join(words),
                                     "labels": " ".join(labels), "elided": [words[wi] for wi in elided]})
    summary["cost"] = round(summary["cost"], 2)
    return summary


def refine_onsets(notes: list[dict], audio: Path) -> dict:
    voiced = voicing(audio)
    candidates = shifted = 0
    for i, n in enumerate(notes):
        onset = label_parts(n["label"])[0]
        n["onsetShift"] = 0.0
        if not onset or not n["head"] or onset[-1] not in UNVOICED:
            continue
        candidates += 1
        found = vowel_onset(voiced, n["csdStart"], n["end"], len(onset))
        if found is None:
            continue
        n["start"] = found
        n["onsetShift"] = round(found - n["csdStart"], 4)
        shifted += 1
        if i > 0 and abs(notes[i - 1]["end"] - n["csdStart"]) <= LEGATO:
            notes[i - 1]["end"] = found
    return {"unvoicedOnsets": candidates, "refined": shifted}


def build(root: Path, out_dir: Path, dictionary: dict, refine: bool) -> tuple[dict, dict, list[str]]:
    meta_path = root / "metadata.json"
    if not meta_path.exists():
        raise SystemExit(f"{meta_path} not found; DATASET_ROOT is CSD/english (metadata.json, wav/, csv/, txt/, lyric/)")
    metadata = json.loads(meta_path.read_text(encoding="utf-8"))
    cases, problems = [], []
    for name in sorted(metadata):
        m = re.fullmatch(r"en(\d{3})([ab])", name)
        if not m:
            continue
        song, key = int(m.group(1)), m.group(2)
        meta = metadata[name]
        audio = root / "wav" / f"{name}.wav"
        paths = {kind: root / kind / f"{name}.{ext}" for kind, ext in (("csv", "csv"), ("txt", "txt"), ("lyric", "txt"))}
        missing = [str(p.relative_to(root)) for p in [audio, *paths.values()] if not p.exists()]
        if missing:
            problems.append(f"{name}: missing {', '.join(missing)}; skipped")
            continue
        notes, skipped = read_notes(paths["csv"])
        if skipped:
            problems.append(f"{name}: {skipped} csv rows skipped")
        if not notes:
            problems.append(f"{name}: no notes; skipped")
            continue
        notes.sort(key=lambda n: n["start"])
        label_lines = [line.split() for line in read_lines(paths["txt"])]
        lyric_raw = read_lines(paths["lyric"])
        lyric_lines = [[w for w in (clean_word(t) for t in line.split()) if w] for line in lyric_raw]
        lyric_lines = [line for line in lyric_lines if line]
        alignment = words_for(notes, label_lines, lyric_lines, dictionary, problems, name)
        for n in notes:
            n["csdStart"] = n["start"]
        csd_notes = [{"start": round(n["start"], 4), "dur": round(n["end"] - n["start"], 4), "pitch": n["pitch"],
                      "lyric": n["lyric"], "word": n["word"], "label": n["label"]} for n in notes]
        refinement = refine_onsets(notes, audio) if refine else None
        out_notes = []
        for n in notes:
            note = {"start": round(n["start"], 4), "dur": round(n["end"] - n["start"], 4), "pitch": n["pitch"],
                    "lyric": n["lyric"], "word": n["word"], "label": n["label"], "csdStart": round(n["csdStart"], 4),
                    "onsetShift": n.get("onsetShift", 0.0)}
            if n.get("elided"):
                note["elided"] = n["elided"]
            out_notes.append(note)
        total = sum(n["dur"] for n in out_notes)
        case = {"id": f"csd-{name}", "audio": os.path.relpath(audio.resolve(), out_dir.resolve()), "song": song,
                "songName": meta.get("songname", ""), "key": key, "keyDiff": meta.get("key_diff"),
                "tempo": meta.get("tempo"), "timeSignature": meta.get("time_signature"), "singer": SINGER,
                "language": "English", "english": True,
                "averagePitch": round(sum(n["pitch"] * n["dur"] for n in out_notes) / total, 2) if total else None,
                "gridShare": grid_share(notes, meta.get("tempo") or 0), "lyrics": lyric_raw,
                "reference": "csd-refined" if refine else "csd", "notes": out_notes, "alignment": alignment}
        if refinement:
            case["onsetRefinement"] = refinement
            case["alternates"] = {"csd": {"notes": csd_notes}}
        cases.append(case)
    manifest = {"version": 1, "dataset": DATASET, "source": SOURCE,
                "comment": "Generated by tools/datasets/csd.py (do not edit; re-run it). Audio paths are relative to "
                           "this file. notes = CSD's notes, starts after an unvoiced onset consonant moved to the "
                           "vowel's voicing onset (csdStart keeps CSD's), pitch as given, words aligned from the lyric files with "
                           "lyric-sheet hyphenation and '_' melisma; alternates.csd = the same notes with CSD's starts. "
                           "See the adapter's docstring.",
                "reference": cases[0]["reference"] if cases else "csd", "cases": cases}
    return manifest, make_split(cases), problems


def make_split(cases: list[dict]) -> dict:
    out = {"comment": SPLIT_RULE, "dataset": DATASET, "test": [], "dev": [], "testSongs": [], "devSongs": [],
           "summary": {}}
    for c in cases:
        split = "dev" if c["song"] % 2 else "test"
        out[split].append(c["id"])
        songs = out[f"{split}Songs"]
        if c["song"] not in songs:
            songs.append(c["song"])
    for name in ("dev", "test"):
        chosen = set(out[name])
        out["summary"][name] = {"songs": len(out[f"{name}Songs"]), "recordings": len(chosen),
                                "notes": sum(len(c["notes"]) for c in cases if c["id"] in chosen)}
    return out


def list_cases(manifest_path: Path) -> None:
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    for case in data.get("cases", []):
        audio = (manifest_path.parent / case["audio"]).resolve()
        print(f"{case['id']}\t{os.path.relpath(audio)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("dataset_root", nargs="?", help="CSD/english folder (metadata.json, wav/, csv/, txt/, lyric/)")
    parser.add_argument("--out-dir", default=str(ROOT / "reference" / "datasets" / DATASET),
                        help="where manifest.json and splits.json go (default reference/datasets/csd)")
    parser.add_argument("--dictionary", default=str(DEFAULT_DICT), help="pronunciation dictionary (default data/cmudict.dict)")
    parser.add_argument("--no-refine", action="store_true", help="keep CSD's start times (no audio analysis, standard library only)")
    parser.add_argument("--list", metavar="MANIFEST", help="print id<TAB>audio for every case of MANIFEST and exit")
    args = parser.parse_args()
    if args.list:
        list_cases(Path(args.list))
        return
    if not args.dataset_root:
        parser.error("dataset_root is required (or --list MANIFEST)")
    root, out_dir = Path(args.dataset_root).resolve(), Path(args.out_dir).resolve()
    dictionary = read_dictionary(Path(args.dictionary))
    if not dictionary:
        print(f"note: no dictionary at {args.dictionary}; syllable counts from spelling only", file=sys.stderr)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest, split, problems = build(root, out_dir, dictionary, not args.no_refine)
    for name, value in (("manifest.json", manifest), ("splits.json", split)):
        (out_dir / name).write_text(json.dumps(value, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    cases = manifest["cases"]
    notes = [n for c in cases for n in c["notes"]]
    melisma = sum(1 for n in notes if n["lyric"] == "_")
    shifted = [n["onsetShift"] for n in notes if n["onsetShift"]]
    lines = sum(len(c["alignment"]["lines"]) for c in cases)
    print(f"{len(cases)} recordings, {len({c['song'] for c in cases})} songs, {len(notes)} notes ({melisma} melisma), "
          f"{lines} notable word-alignment lines, {sum(len(c['alignment']['elided']) for c in cases)} elided words")
    if shifted:
        shifted.sort()
        print(f"  onset refinement: {len(shifted)} starts moved to the vowel onset, median "
              f"{1000 * shifted[len(shifted) // 2]:.0f} ms")
    for name, s in split["summary"].items():
        print(f"  {name}: {s['songs']} songs, {s['recordings']} recordings, {s['notes']} notes")
    for problem in problems:
        print(f"  note: {problem}", file=sys.stderr)
    print(f"wrote {out_dir / 'manifest.json'} and {out_dir / 'splits.json'}")


if __name__ == "__main__":
    main()
