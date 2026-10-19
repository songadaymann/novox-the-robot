"""GTSinger (English) -> import-lab manifest and singer-level dev/test split.

GTSinger (Zhang et al., NeurIPS 2024 Datasets and Benchmarks; https://huggingface.co/datasets/GTSinger/GTSinger,
CC BY-NC-SA 4.0 plus the indemnity terms of the dataset's dataset_license.md) has professional singers
singing phrases with six techniques (mixed voice, falsetto, breathy, pharyngeal, glissando, vibrato). Each
technique folder holds songs; each song has a technique group, a Control_Group (the same phrases sung
plainly) and often a Paired_Speech_Group (the lyrics spoken). Every segment NNNN has a .wav, a .json
(word, phoneme and note annotation), a .TextGrid (word and phone tiers plus one tier per technique) and,
for sung segments, a .musicxml score. Download it yourself (it is never committed; local evaluation
only), e.g. to reference/datasets/gtsinger/, then:

    tools/.venv/bin/python -I tools/datasets/gtsinger.py reference/datasets/gtsinger

DATASET_ROOT is the folder holding the language folder English/ (or English/ itself), laid out as
English/<singer>/<technique>/<song>/<group>/<NNNN>.{wav,json,TextGrid,musicxml}. Some TextGrids are
named NNNN_TextGrid; both names are read. Writes, next to each other (default folder
reference/datasets/gtsinger/, gitignored; --out-dir to change):

  manifest.json  "cases": one case per SUNG segment, as tools/import_lab.py reads it:
                   id          gts-<singer>-<technique>-<song>-<group>-<NNNN>, short codes (SINGER_CODES,
                               TECHNIQUE_CODES, GROUP_CODES; song: its first 10 letters/digits), e.g.
                               gts-a1-vb-edelweiss-ctrl-0002
                   audio       path of the .wav, relative to the manifest
                   singer (EN-Alto-1), voice (alto/tenor), gender, technique (folder name, e.g.
                   Mixed_Voice_and_Falsetto), group (folder name, e.g. Falsetto_Group), condition
                   ("technique" or "control"), groupTechnique (the json label the group demonstrates:
                   mix, falsetto, breathy, pharyngeal, glissando, vibrato; null for control),
                   techniqueShare (share of reference notes carrying that label), lumpedShare (share of
                   notes in words GTSinger gave fewer notes than syllables), song, segment, language,
                   english, averagePitch (duration-weighted mean reference pitch, MIDI), lyrics (the words),
                   singingMethod, pace, range, emotion (the json's global labels)
                   notes       the reference, in the lab's format and the Novox convention (see Notes)
                   alternates  {"raw": {"notes": [...]}} GTSinger's own word-level notes as annotated
                               (start = note_start, lyric = the word on its first note, '_' on the rest),
                               for `import_lab.py agree MANIFEST --alternate raw`: how far the raw
                               annotation is from the converted reference
                 "speechCases": the Paired_Speech_Group segments ("speech": true, no notes; same fields
                               otherwise, condition "speech"): not scored, but cached and run to check what
                               the importer does on speech.
  manifest-speech.json  {"cases": speechCases}, so `import_lab.py corpus` can run them on their own.
  splits.json    singer-level split (SPLIT_RULE): dev = EN-Alto-1, EN-Tenor-1; test = EN-Alto-2
                 (held out). "test" lists the held-out ids (sung and speech).

Notes (one per note of the GTSinger annotation, made syllable-level)
  GTSinger annotates notes per WORD (json "note", "note_start", "note_end": the word's notes tile the word
  from its first consonant to its end; the musicxml has the same notes with the whole word as one
  "single" lyric). The Novox convention is one note per sung syllable starting at the syllable's VOWEL
  onset. Within each word, the syllables are its vowel phones (TextGrid phone tier; ARPAbet vowels) and
  each GTSinger note belongs to the first syllable whose vowel ends more than ASSIGN_MARGIN after the
  note starts (a note starting in the consonants before a vowel belongs to that vowel's syllable; one
  starting inside a vowel is a melisma note of that syllable). For each syllable:
    - head note: start = the vowel onset; pitch = its first note when that starts no later than
      LATE_ARRIVAL after the vowel onset (a late pitch arrival is a scoop, not a new note; when several
      notes start before the vowel, the last of them), else the note sounding at the vowel onset;
      lyric = the syllable's spelling with a trailing '-' unless it ends the word
    - the syllable's later notes: melisma continuations, lyric '_', start = their own note_start
    - a syllable without a note of its own (GTSinger gave the word fewer notes than syllables) still
      gets a head note, with the pitch of the note sounding at its vowel onset ("split": true)
  end = the next note's start when the next note follows without a rest (<SP>/<AP>) in between, else
  the end of the word (end of sound). pitch = the annotated MIDI note, rounded. Each note also has
  "word", "syllable" (index in the word), "scoreNote" (index of the GTSinger note it comes from),
  "split" when it is a syllable split of a longer GTSinger note, "lumped" when GTSinger gave its word
  fewer notes than syllables (some of that word's pitches are then a guess: a dev check against pyin
  found 24% of split notes more than 1 st off, against 8% of the others), "techniques" (json labels set
  on its vowel phone; for a melisma note, on the phone where it starts). A note that would come out
  shorter than MIN_DUR is dropped and reported. The syllable spelling is a lyric-sheet style guess
  (hyphenate()); the lab's lyricWordAccuracy also accepts the right word hyphenated differently.

Only the standard library is used, so it runs with `python -I` (the dataset files are data). --list
prints "id<TAB>audio path" for every case (sung, then speech) for the cache loop in
tools/datasets/README.md.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

DATASET = "gtsinger"
SOURCE = {"name": "GTSinger", "url": "https://huggingface.co/datasets/GTSinger/GTSinger",
          "license": "CC BY-NC-SA 4.0 plus the indemnity terms of the dataset's dataset_license.md; local evaluation only",
          "citation": "Y. Zhang et al., 'GTSinger: A Global Multi-Technique Singing Corpus with Realistic Music Scores "
                      "for All Singing Tasks', NeurIPS 2024 Datasets and Benchmarks"}
DEV_SINGERS = ("EN-Alto-1", "EN-Tenor-1")
TEST_SINGERS = ("EN-Alto-2",)
SPLIT_RULE = ("Singer-level split, made by tools/datasets/gtsinger.py: dev = EN-Alto-1 and EN-Tenor-1, test = "
              "EN-Alto-2 (one alto held out). All segments of a singer (sung and speech) stay together; a singer "
              "not listed goes to dev. Test singers are held out: never tune on them, never open their plots or "
              "results.")
SINGER_CODES = {"EN-Alto-1": "a1", "EN-Alto-2": "a2", "EN-Tenor-1": "t1"}
TECHNIQUE_CODES = {"Breathy": "br", "Glissando": "gl", "Mixed_Voice_and_Falsetto": "mf", "Pharyngeal": "ph",
                   "Vibrato": "vb"}
GROUP_CODES = {"Control_Group": "ctrl", "Paired_Speech_Group": "sp", "Breathy_Group": "br", "Glissando_Group": "gl",
               "Falsetto_Group": "fal", "Mixed_Voice_Group": "mix", "Pharyngeal_Group": "ph", "Vibrato_Group": "vb"}
# The json technique label each technique group demonstrates.
GROUP_TECHNIQUE = {"Breathy_Group": "breathy", "Glissando_Group": "glissando", "Falsetto_Group": "falsetto",
                   "Mixed_Voice_Group": "mix", "Pharyngeal_Group": "pharyngeal", "Vibrato_Group": "vibrato"}
TECHNIQUE_LABELS = ("mix", "falsetto", "breathy", "pharyngeal", "glissando", "vibrato")
SPEECH_GROUP = "Paired_Speech_Group"
CONTROL_GROUP = "Control_Group"
RESTS = {"<SP>", "<AP>", ""}
VOWEL_PHONES = {"AA", "AE", "AH", "AO", "AW", "AX", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"}

ASSIGN_MARGIN = 0.030   # s: a note starting this close to a vowel's end belongs to the next syllable
LATE_ARRIVAL = 0.100    # s: a syllable's first note starting this late into the vowel is still its head ...
LATE_SHARE = 0.5        # ... if that is within this share of the vowel
MIN_DUR = 0.010         # s
EPS = 1e-6


# ---------------------------------------------------------------------------
# Files

def read_textgrid(path: Path) -> dict[str, list[tuple[float, float, str]]]:
    """Interval tiers of a Praat long-format TextGrid: {tier name: [(xmin, xmax, text), ...]}."""
    tiers: dict[str, list] = {}
    current, interval = None, {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        m = re.match(r'^(name|xmin|xmax|text)\s*=\s*(.*)$', line)
        if line.startswith("item ["):
            current = None
        elif m and m.group(1) == "name" and current is None:
            current = tiers.setdefault(m.group(2).strip().strip('"'), [])
        elif line.startswith("intervals ["):
            interval = {}
        elif m and current is not None and m.group(1) in ("xmin", "xmax", "text") and not line.startswith("intervals"):
            key, value = m.group(1), m.group(2).strip()
            if key == "text":
                interval["text"] = value[1:-1].replace('""', '"') if value.startswith('"') else value
                if "xmin" in interval and "xmax" in interval:
                    current.append((float(interval["xmin"]), float(interval["xmax"]), interval["text"]))
                interval = {}
            else:
                interval[key] = value
    return tiers


def textgrid_path(json_path: Path) -> Path | None:
    for candidate in (json_path.with_suffix(".TextGrid"), json_path.with_name(json_path.stem + "_TextGrid")):
        if candidate.exists():
            return candidate
    return None


def is_vowel(phone: str) -> bool:
    return phone.rstrip("012") in VOWEL_PHONES


# ---------------------------------------------------------------------------
# Lyric-sheet hyphenation (a guess from the spelling, guided by the phone syllable count)

SUFFIXES_AFTER_GLIDE = ("er", "ers", "ing", "ings", "ed", "ered", "ering", "ery")
ONSET_BLENDS = {"bl", "br", "cl", "cr", "dr", "fl", "fr", "gl", "gr", "pl", "pr", "tr", "thr", "shr", "chr", "phr",
                "spl", "spr", "str", "scr", "squ", "qu", "wh", "sh", "ch", "th", "ph"}
DIGRAPHS = ("ch", "sh", "th", "ph", "wh", "ck", "gh", "ng", "qu")
SHORT_VOWELS = {"AE", "EH", "IH", "AH", "UH"}


def _vowel_groups(w: str) -> list[list[int]]:
    """[lo, hi) letter spans of the vowel spellings of a lowercase word."""
    n, groups, i = len(w), [], 0
    while i < n:
        c = w[i]
        vowel = c in "aeiou" or (c == "y" and i > 0 and w[i - 1] not in "aeiou'")
        if not vowel:
            i += 1
            continue
        j = i + 1
        while j < n:
            d = w[j]
            if d in "aeiou" and not (w[j - 1] == "y" and j - 1 > i):
                j += 1
            elif d in "wy" and w[j - 1] in "aeo" and (j + 1 >= n or w[j + 1] not in "aeiouy"
                                                     or w[j + 1:] in SUFFIXES_AFTER_GLIDE):
                j += 1
            else:
                break
        groups.append([i, j])
        i = j
    return groups


def _units(cluster: str) -> list[str]:
    out, i = [], 0
    while i < len(cluster):
        two = cluster[i:i + 2]
        if two in DIGRAPHS:
            out.append(two)
            i += 2
        else:
            out.append(cluster[i])
            i += 1
    return out


CONTRACTION = re.compile(r"^(.+?)(n't|'ll|'s|'re|'ve|'d|'m|'t)$")


def hyphenate(word: str, phones: list[str]) -> list[str]:
    """The word's spelling cut into as many pieces as it has vowel phones (lyric-sheet style)."""
    vowels = [p for p in phones if is_vowel(p)]
    count = max(1, len(vowels))
    if count == 1:
        return [word]
    m = CONTRACTION.match(word.lower())
    if m:  # syllabic n't / 'll (did-n't, it-'ll), else the tail rides on the last piece
        stem, tail = word[:len(m.group(1))], word[len(m.group(1)):]
        syllabic = tail.lower() in ("n't", "'ll") and len(phones) >= 2 and is_vowel(phones[-2]) and phones[-1] in ("N", "L")
        if syllabic:
            return hyphenate(stem, phones[:-2]) + [tail] if count - 1 >= 1 else [word]
        pieces = _hyphenate_letters(stem, vowels)
        pieces[-1] += tail
        return pieces
    return _hyphenate_letters(word, vowels)


def _hyphenate_letters(word: str, vowels: list[str]) -> list[str]:
    count = max(1, len(vowels))
    if count == 1:
        return [word]
    w = word.lower()
    groups = _vowel_groups(w)

    def drop(pred) -> bool:
        for k in range(len(groups) - 1, 0, -1):
            lo, hi = groups[k]
            if pred(lo, hi, k):
                del groups[k]
                return True
        return False
    # Too many spellings: drop silent e's (final e, -es/-ed endings, a "magic e" between single consonants
    # as in some-thing, lone-ly), then merge (ev-ery: a syncopated e before r first).
    while len(groups) > count:
        if drop(lambda lo, hi, k: w[lo:hi] == "e" and hi == len(w) and w[lo - 1] not in "aeiou"
                and not (w[lo - 1] == "l" and lo >= 2 and w[lo - 2] not in "aeiou")):
            continue
        if drop(lambda lo, hi, k: w[lo:hi] == "e" and w[hi:] in ("s", "d") and w[lo - 1] not in "aeiou"
                and w[lo - 1] not in "dt" and w[lo - 2:lo] not in ("ch", "sh") and w[lo - 1] not in "sxz"):
            continue
        if drop(lambda lo, hi, k: w[lo:hi] == "e" and k + 1 < len(groups) and lo - groups[k - 1][1] == 1
                and groups[k + 1][0] - hi == 1 and w[hi] != "r"):
            continue
        syncope = lambda k: w[groups[k][0]:groups[k][1]] == "e" and w[groups[k][1]:groups[k + 1][0]] == "r"
        best = min(range(len(groups) - 1), key=lambda k: (not syncope(k), groups[k + 1][0] - groups[k][1], -k))
        groups[best][1] = groups[best + 1][1]
        del groups[best + 1]
    # Too few: split two-letter vowel spellings (go-ing, ac-tu-al, li-on).
    while len(groups) < count:
        k = next((k for k, (lo, hi) in enumerate(groups) if hi - lo >= 2 and w[lo:hi] not in
                  ("ee", "oo", "ea", "ou", "ai", "ay", "ey", "oy", "aw", "ow", "ew")),
                 next((k for k, (lo, hi) in enumerate(groups) if hi - lo >= 2), None))
        if k is None:
            break
        lo, hi = groups[k]
        groups[k:k + 1] = [[lo, hi - 1], [hi - 1, hi]]
    if len(groups) < count:
        n = len(word)
        return [word[n * k // count: n * (k + 1) // count] for k in range(count)]
    cuts = []
    for k in range(len(groups) - 1):
        a, b = groups[k][1], groups[k + 1][0]
        cluster = w[a:b]
        vowel = vowels[k] if k < len(vowels) else "AH0"
        base, stress = vowel.rstrip("012"), vowel[-1:] if vowel[-1:].isdigit() else "0"
        short = (base in SHORT_VOWELS or (base == "AA" and w[groups[k][0]:a] == "o")) and stress in "12"
        units = _units(cluster)
        rest = w[b:]
        if not units:
            cut = a
        elif rest in ("ing", "ings"):  # sit-ting, kid-ding; stand-ing, go-ing
            cut = b - 1 if len(cluster) >= 2 and cluster[-1] == cluster[-2] else b
        elif (rest.startswith("ed") and rest[2:] in ("", "ly", "s")) or (rest == "es" and cluster[-1:] in "sxz"):
            cut = b  # need-ed, twist-ed, prom-is-es
        elif rest == "e" and cluster.endswith("l") and len(units) >= 2:
            cut = b - 2  # lit-tle, gen-tle, bot-tle
        elif len(units) == 1:
            keep_left = short or units[0] in ("x", "ck", "gh") or (units[0] == "r" and base == "ER"
                                                                  and w[groups[k][0]:a].endswith("e"))
            cut = b if keep_left else a
        else:
            tail2 = "".join(units[-2:])
            if tail2 in ONSET_BLENDS and len(units) == 2 and not short:
                cut = b - len(tail2)
            elif units[-1] == "ng":
                cut = b
            else:
                cut = b - len(units[-1])
        cuts.append(cut)
    pieces, start = [], 0
    for cut in cuts:
        pieces.append(word[start:cut])
        start = cut
    pieces.append(word[start:])
    if any(not p for p in pieces):  # degenerate: even cut
        n = len(word)
        pieces = [word[n * k // count: n * (k + 1) // count] for k in range(count)]
    return pieces


# ---------------------------------------------------------------------------
# One segment

def segment_words(data: list[dict], tiers: dict | None, problems: list[str], label: str) -> list[dict]:
    """The json words with their phones; phone times from the TextGrid phone tier when it matches."""
    words = []
    phone_tier = (tiers or {}).get("phone") or (tiers or {}).get("phones")
    tier_phones = [(a, b, t) for a, b, t in phone_tier] if phone_tier else None
    for w in data:
        phones = [{"ph": p, "start": float(s), "end": float(e)} for p, s, e in
                  zip(w.get("ph", []), w.get("ph_start", []), w.get("ph_end", []))]
        if tier_phones is not None:
            inside = [(a, b, t) for a, b, t in tier_phones if a >= float(w["start_time"]) - 1e-4 and b <= float(w["end_time"]) + 1e-4]
            if [t for _, _, t in inside] == [p["ph"] for p in phones]:
                phones = [{"ph": t, "start": a, "end": b} for a, b, t in inside]
            elif w.get("word") not in RESTS:
                problems.append(f"{label}: '{w.get('word')}' TextGrid phones {[t for _, _, t in inside]} differ "
                                f"from json {[p['ph'] for p in phones]}; json used")
        for p, i in zip(phones, range(len(phones))):
            p["techniques"] = [t for t in TECHNIQUE_LABELS if str((w.get(t) or ["0"] * len(phones))[i]) == "1"]
        notes = [{"pitch": n, "start": float(s), "end": float(e)} for n, s, e in
                 zip(w.get("note", []), w.get("note_start", []), w.get("note_end", []))]
        words.append({"word": w.get("word", ""), "start": float(w["start_time"]), "end": float(w["end_time"]),
                      "phones": phones, "notes": notes})
    return words


def word_notes(w: dict, first_score_note: int) -> tuple[list[dict], list[dict]]:
    """Syllable-level reference notes of one sung word (ends filled in later) and its raw GTSinger notes."""
    notes = [n for n in w["notes"] if n["pitch"]]
    raw = [{"start": n["start"], "end": n["end"], "pitch": int(round(n["pitch"])),
            "lyric": w["word"] if i == 0 else "_", "word": w["word"], "scoreNote": first_score_note + i}
           for i, n in enumerate(notes)]
    if not notes:
        return [], raw
    phones = w["phones"]
    syllables = [p for p in phones if is_vowel(p["ph"])]
    if not syllables:  # no vowel phone: the word is one syllable from its start
        syllables = [{"ph": "", "start": w["start"], "end": w["end"], "techniques": phones[0]["techniques"] if phones else []}]
    pieces = hyphenate(w["word"], [p["ph"] for p in phones])
    if len(pieces) != len(syllables):
        pieces = [w["word"]] + [""] * (len(syllables) - 1)
    lumped = len(notes) < len(syllables)  # GTSinger gave the word fewer notes than syllables
    owner = []
    for n in notes:
        k = next((k for k, s in enumerate(syllables) if s["end"] > n["start"] + ASSIGN_MARGIN), len(syllables) - 1)
        owner.append(k)
    sounding = lambda t: max((i for i, n in enumerate(notes) if n["start"] <= t + EPS), default=0)
    phone_at = lambda t: next((p for p in phones if p["start"] <= t + EPS < p["end"]), phones[-1] if phones else None)
    out = []
    for k, s in enumerate(syllables):
        own = [i for i, o in enumerate(owner) if o == k]
        onset = s["start"]
        late = min(LATE_ARRIVAL, LATE_SHARE * (s["end"] - s["start"]))
        early = [i for i in own if notes[i]["start"] <= onset + late + EPS]
        head = early[-1] if early else sounding(onset)
        lyric = pieces[k] + ("-" if k + 1 < len(syllables) else "")
        out.append({"start": onset, "pitch": int(round(notes[head]["pitch"])), "lyric": lyric, "word": w["word"],
                    "syllable": k, "scoreNote": first_score_note + head, "split": head not in own, "lumped": lumped,
                    "techniques": s.get("techniques", [])})
        for i in own:
            if early and i <= early[-1]:
                continue
            p = phone_at(notes[i]["start"])
            out.append({"start": notes[i]["start"], "pitch": int(round(notes[i]["pitch"])), "lyric": "_",
                        "word": w["word"], "syllable": k, "scoreNote": first_score_note + i, "split": False,
                        "lumped": lumped, "techniques": p["techniques"] if p else []})
    return out, raw


def finish_notes(seq: list[dict], words_end: list[float], legato_to_next: list[bool]) -> list[dict]:
    """Fill in "dur": the next note's start when legato (same word or next word without a rest), else the
    word's end. seq items carry "_word" (index) for this."""
    out = []
    for i, n in enumerate(seq):
        nxt = seq[i + 1] if i + 1 < len(seq) else None
        legato = nxt is not None and (nxt["_word"] == n["_word"] or
                                      (nxt["_word"] == n["_word"] + 1 and legato_to_next[n["_word"]]))
        end = nxt["start"] if legato else words_end[n["_word"]]
        note = {k: v for k, v in n.items() if not k.startswith("_")}
        note["start"] = round(n["start"], 4)
        note["dur"] = round(end - n["start"], 4)
        out.append(note)
    return out


def convert(words: list[dict], problems: list[str], label: str) -> tuple[list[dict], list[dict]]:
    sung = [i for i, w in enumerate(words) if w["word"] not in RESTS and any(n["pitch"] for n in w["notes"])]
    seq, raw_seq, score_index = [], [], 0
    for idx in sung:
        notes, raw = word_notes(words[idx], score_index)
        score_index += len(raw)
        seq += [dict(n, _word=idx) for n in notes]
        raw_seq += [dict(n, _word=idx) for n in raw]
    words_end = [w["end"] for w in words]
    legato = [i + 1 < len(words) and words[i + 1]["word"] not in RESTS
              and abs(words[i + 1]["start"] - words[i]["end"]) < 1e-3 for i in range(len(words))]
    seq.sort(key=lambda n: n["start"])
    ref = finish_notes(seq, words_end, legato)
    kept = [n for n in ref if n["dur"] >= MIN_DUR]
    for n in ref:
        if n["dur"] < MIN_DUR:
            problems.append(f"{label}: note '{n['lyric']}' at {n['start']} came out {n['dur']} s long; dropped")
    raw = []
    for n in raw_seq:
        r = {k: v for k, v in n.items() if not k.startswith("_") and k != "end"}
        r["start"], r["dur"] = round(n["start"], 4), round(n["end"] - n["start"], 4)
        raw.append(r)
    return kept, raw


# ---------------------------------------------------------------------------
# Dataset

def slug(text: str, size: int = 10) -> str:
    return "".join(c for c in text.lower() if c.isascii() and c.isalnum())[:size] or "song"


def find_language_root(root: Path, language: str) -> Path:
    if (root / language).is_dir():
        return root / language
    if any(p.is_dir() and p.name.startswith("EN-") for p in root.iterdir()):
        return root
    raise SystemExit(f"{root}: no {language}/ folder and no singer folders; DATASET_ROOT holds {language}/")


def build(root: Path, out_dir: Path, language: str) -> tuple[dict, dict, list[str]]:
    lang_root = find_language_root(root, language)
    cases, speech, problems, ids = [], [], [], set()
    for wav in sorted(lang_root.glob("*/*/*/*/*.wav")):
        group_dir = wav.parent
        group, song, technique, singer = group_dir.name, group_dir.parent.name, group_dir.parent.parent.name, \
            group_dir.parent.parent.parent.name
        label = f"{singer}/{technique}/{song}/{group}/{wav.stem}"
        js = wav.with_suffix(".json")
        if not js.exists():
            problems.append(f"{label}: no json; skipped")
            continue
        try:
            data = json.loads(js.read_text(encoding="utf-8"))
        except ValueError as error:
            problems.append(f"{label}: unreadable json ({error}); skipped")
            continue
        tg = textgrid_path(js)
        tiers = read_textgrid(tg) if tg else None
        if not tg and group != SPEECH_GROUP:
            problems.append(f"{label}: no TextGrid; json phone times used")
        case_id = "-".join(["gts", SINGER_CODES.get(singer, slug(singer, 6)),
                            TECHNIQUE_CODES.get(technique, slug(technique, 4)), slug(song),
                            GROUP_CODES.get(group, slug(group, 6)), wav.stem])
        if case_id in ids:
            problems.append(f"{label}: duplicate id {case_id}; skipped")
            continue
        ids.add(case_id)
        voice = "tenor" if "tenor" in singer.lower() else "alto" if "alto" in singer.lower() else \
            "soprano" if "soprano" in singer.lower() else "bass" if "bass" in singer.lower() else "unknown"
        is_speech = group == SPEECH_GROUP
        first = next((w for w in data if w.get("word") not in RESTS), data[0] if data else {})
        words = segment_words(data, tiers, problems, label)
        case = {"id": case_id, "audio": os.path.relpath(wav.resolve(), out_dir.resolve()), "singer": singer,
                "voice": voice, "gender": "male" if voice in ("tenor", "bass") else "female",
                "technique": technique, "group": group,
                "condition": "speech" if is_speech else "control" if group == CONTROL_GROUP else "technique",
                "groupTechnique": GROUP_TECHNIQUE.get(group), "song": song, "segment": wav.stem,
                "language": language, "english": language == "English",
                "lyrics": " ".join(w["word"] for w in words if w["word"] not in RESTS)}
        if is_speech:
            case["speech"] = True
            speech.append(case)
            continue
        for key, name in (("singing_method", "singingMethod"), ("pace", "pace"), ("range", "range"),
                          ("emotion", "emotion")):
            if key in first:
                case[name] = first[key]
        notes, raw = convert(words, problems, label)
        if not notes:
            problems.append(f"{label}: no notes; skipped")
            continue
        total = sum(n["dur"] for n in notes)
        case["averagePitch"] = round(sum(n["pitch"] * n["dur"] for n in notes) / total, 2) if total else None
        tech = case["groupTechnique"]
        case["techniqueShare"] = round(sum(1 for n in notes if tech in n["techniques"]) / len(notes), 3) if tech else None
        case["lumpedShare"] = round(sum(1 for n in notes if n["lumped"]) / len(notes), 3)
        case["reference"] = "converted"
        case["notes"] = notes
        case["alternates"] = {"raw": {"notes": raw}}
        cases.append(case)
    manifest = {"version": 1, "dataset": DATASET, "source": SOURCE,
                "comment": "Generated by tools/datasets/gtsinger.py (do not edit; re-run it). Audio paths are relative "
                           "to this file. notes = GTSinger's word-level notes made syllable-level (vowel-onset starts, "
                           "lyric-sheet hyphenation, '_' melisma; see the adapter's docstring); alternates.raw = the "
                           "word-level notes as annotated. speechCases are not scored.",
                "reference": "converted", "cases": cases, "speechCases": speech}
    split = make_split(cases, speech)
    return manifest, split, problems


def make_split(cases: list[dict], speech: list[dict]) -> dict:
    out = {"comment": SPLIT_RULE, "dataset": DATASET, "test": [], "dev": [], "testSingers": [], "devSingers": [],
           "summary": {}}
    singers = sorted({c["singer"] for c in cases + speech})
    for s in singers:
        out["testSingers" if s in TEST_SINGERS else "devSingers"].append(s)
    for c in cases + speech:
        out["test" if c["singer"] in TEST_SINGERS else "dev"].append(c["id"])
    for name in ("dev", "test"):
        chosen = set(out[name])
        out["summary"][name] = {"singers": len(out[f"{name}Singers"]),
                                "sung": sum(1 for c in cases if c["id"] in chosen),
                                "speech": sum(1 for c in speech if c["id"] in chosen),
                                "notes": sum(len(c["notes"]) for c in cases if c["id"] in chosen)}
    return out


def list_cases(manifest_path: Path) -> None:
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    for case in data.get("cases", []) + data.get("speechCases", []):
        audio = (manifest_path.parent / case["audio"]).resolve()
        print(f"{case['id']}\t{os.path.relpath(audio)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("dataset_root", nargs="?", help="GTSinger folder holding English/")
    parser.add_argument("--language", default="English")
    parser.add_argument("--out-dir", default=str(Path(__file__).resolve().parents[2] / "reference" / "datasets" / DATASET),
                        help="where manifest.json, manifest-speech.json and splits.json go (default reference/datasets/gtsinger)")
    parser.add_argument("--list", metavar="MANIFEST", help="print id<TAB>audio for every case of MANIFEST and exit")
    args = parser.parse_args()
    if args.list:
        list_cases(Path(args.list))
        return
    if not args.dataset_root:
        parser.error("dataset_root is required (or --list MANIFEST)")
    root, out_dir = Path(args.dataset_root).resolve(), Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest, split, problems = build(root, out_dir, args.language)
    speech_manifest = {"version": 1, "dataset": DATASET, "source": SOURCE,
                       "comment": "Generated by tools/datasets/gtsinger.py: the Paired_Speech_Group segments of "
                                  "manifest.json (speechCases), not scored; no notes.",
                       "cases": manifest["speechCases"]}
    for name, value in (("manifest.json", manifest), ("manifest-speech.json", speech_manifest), ("splits.json", split)):
        (out_dir / name).write_text(json.dumps(value, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    cases = manifest["cases"]
    splits = sum(1 for c in cases for n in c["notes"] if n["split"])
    melisma = sum(1 for c in cases for n in c["notes"] if n["lyric"] == "_")
    print(f"{len(cases)} sung cases ({sum(len(c['notes']) for c in cases)} notes: {melisma} melisma, {splits} syllable "
          f"splits; {sum(len(c['alternates']['raw']['notes']) for c in cases)} raw GTSinger notes), "
          f"{len(manifest['speechCases'])} speech cases, {len({c['singer'] for c in cases})} singers")
    for name, s in split["summary"].items():
        print(f"  {name}: {s['singers']} singers, {s['sung']} sung, {s['speech']} speech, {s['notes']} notes")
    for problem in problems:
        print(f"  note: {problem}", file=sys.stderr)
    print(f"wrote {out_dir / 'manifest.json'}, {out_dir / 'manifest-speech.json'} and {out_dir / 'splits.json'}")


if __name__ == "__main__":
    main()
