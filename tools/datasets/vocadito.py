"""vocadito -> import-lab manifest and singer-level dev/test split.

vocadito (Bittner et al., 2021; https://zenodo.org/records/5578259, CC BY 4.0) is 40 short solo
a-cappella recordings by 29 singers in 7 languages, with note annotations by two independent
annotators (A1, A2), f0 and phrase-level lyrics. Download and extract it yourself (it is never
committed), e.g. to reference/datasets/vocadito/extracted/, then:

    tools/.venv/bin/python -I tools/datasets/vocadito.py reference/datasets/vocadito/extracted/zenodo_data

DATASET_ROOT is the folder holding vocadito_metadata.csv, Audio/ and Annotations/. Writes, next to
each other (default folder reference/datasets/vocadito/, gitignored; --out-dir to change):

  manifest.json  one case per track, as tools/import_lab.py reads it:
                   id            "voc-<track>"
                   audio         path of Audio/vocadito_<track>.wav, relative to the manifest
                   track, singer, language (as in vocadito_metadata.csv), english (language is
                   exactly "English"; whisper.cpp runs forced to English, so other languages give
                   junk words), averagePitch (metadata, MIDI)
                   lyrics        the lyric phrases (one per line of the lyrics file; vocadito has
                                 no per-note lyrics, so the notes carry none)
                   notes         reference = annotator A1: start, dur (s), pitch (MIDI rounded to
                                 the nearest semitone, what the lab scores), midi (unrounded),
                                 pitchHz (as annotated)
                   alternates    {"A2": {"notes": [...]}} annotator A2 in the same form (for
                                 `import_lab.py agree`: the human-agreement ceiling)
  splits.json    singer-level split (see SPLIT_RULE); "test" lists the held-out case ids.

Only the standard library is used, so it runs with `python -I` (the dataset files are data).
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from pathlib import Path

DATASET = "vocadito"
SOURCE = {"name": "vocadito", "url": "https://zenodo.org/records/5578259", "doi": "10.5281/zenodo.5578259",
          "license": "CC BY 4.0", "citation": "R. M. Bittner, K. Pasalo, J. J. Bosch, G. Meseguer-Brocal, D. Rubinstein, "
          "'vocadito: A dataset of solo vocals with f0, note, and lyric annotations', 2021"}
SPLIT_RULE = ("Singer-level split, made by tools/datasets/vocadito.py: singers sorted by the mean averagePitch of their "
              "tracks (ties by singer number) are taken in adjacent pairs, low to high; one singer of each pair goes to "
              "dev and the other to test, so both splits span low to high voices; within a pair the singer with more "
              "English tracks goes to the split that has fewer English tracks so far (ties: the lower singer to dev); "
              "an unpaired last singer goes to dev. All tracks of a singer stay together. Test singers are held out: "
              "never tune on them, never open their plots or results.")


def midi_of(hz: float) -> float:
    return 69.0 + 12.0 * math.log2(hz / 440.0)


def read_notes(path: Path) -> tuple[list[dict], int]:
    """Notes of one annotation CSV (start s, pitch Hz, duration s), in time order; and how many rows
    were skipped (not three numbers, or a non-positive pitch or duration)."""
    notes, skipped = [], 0
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row or not "".join(row).strip():
                continue
            try:
                start, hz, dur = (float(v) for v in row[:3])
            except ValueError:
                skipped += 1
                continue
            if len(row) < 3 or not (hz > 0 and dur > 0 and start >= 0) or not all(map(math.isfinite, (start, hz, dur))):
                skipped += 1
                continue
            midi = midi_of(hz)
            notes.append({"start": round(start, 6), "dur": round(dur, 6), "pitch": int(round(midi)),
                          "midi": round(midi, 3), "pitchHz": hz})
    notes.sort(key=lambda n: n["start"])
    return notes, skipped


def read_metadata(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    tracks = []
    for row in rows:
        tracks.append({"track": int(row["track_id"]), "singer": row["singer_id"].strip(),
                       "averagePitch": float(row["average_pitch"]), "language": row["language"].strip()})
    return sorted(tracks, key=lambda t: t["track"])


def singer_number(singer: str) -> int:
    digits = "".join(c for c in singer if c.isdigit())
    return int(digits) if digits else 0


def make_split(cases: list[dict]) -> dict:
    singers: dict[str, dict] = {}
    for c in cases:
        s = singers.setdefault(c["singer"], {"singer": c["singer"], "pitches": [], "english": 0, "ids": []})
        s["pitches"].append(c["averagePitch"])
        s["english"] += c["english"]
        s["ids"].append(c["id"])
    order = sorted(singers.values(), key=lambda s: (sum(s["pitches"]) / len(s["pitches"]), singer_number(s["singer"])))
    english = {"dev": 0, "test": 0}
    assign: dict[str, str] = {}
    for i in range(0, len(order), 2):
        pair = order[i:i + 2]
        if len(pair) == 1:
            assign[pair[0]["singer"]] = "dev"
            english["dev"] += pair[0]["english"]
            continue
        low, high = pair
        first = "dev"  # the lower singer's split
        if low["english"] != high["english"] and english["dev"] != english["test"]:
            more = low if low["english"] > high["english"] else high
            fewer_split = "dev" if english["dev"] < english["test"] else "test"
            first = fewer_split if more is low else ("test" if fewer_split == "dev" else "dev")
        second = "test" if first == "dev" else "dev"
        assign[low["singer"]], assign[high["singer"]] = first, second
        english[first] += low["english"]
        english[second] += high["english"]
    out = {"comment": SPLIT_RULE, "dataset": DATASET, "test": [], "dev": [], "testSingers": [], "devSingers": [],
           "singers": []}
    for s in order:
        split = assign[s["singer"]]
        out[f"{split}Singers"].append(s["singer"])
        out[split].extend(s["ids"])
        out["singers"].append({"singer": s["singer"], "split": split,
                               "averagePitch": round(sum(s["pitches"]) / len(s["pitches"]), 2),
                               "tracks": len(s["ids"]), "englishTracks": s["english"]})
    key = lambda case_id: int(case_id.split("-")[1])
    out["test"].sort(key=key)
    out["dev"].sort(key=key)
    tracks = {c["id"]: c for c in cases}
    out["summary"] = {split: {"singers": len(out[f"{split}Singers"]), "tracks": len(out[split]),
                              "englishTracks": sum(tracks[i]["english"] for i in out[split])} for split in ("dev", "test")}
    return out


def build(root: Path, out_dir: Path) -> tuple[dict, dict, list[str]]:
    meta = root / "vocadito_metadata.csv"
    if not meta.exists():
        raise SystemExit(f"{meta} not found; DATASET_ROOT is the folder with vocadito_metadata.csv, Audio/, Annotations/")
    notes_dir, lyrics_dir = root / "Annotations" / "Notes", root / "Annotations" / "Lyrics"
    cases, problems = [], []
    for t in read_metadata(meta):
        n = t["track"]
        audio = root / "Audio" / f"vocadito_{n}.wav"
        if not audio.exists():
            problems.append(f"track {n}: no audio; skipped")
            continue
        refs = {}
        for annotator in ("A1", "A2"):
            path = notes_dir / f"vocadito_{n}_notes{annotator}.csv"
            if not path.exists():
                refs[annotator] = None
                problems.append(f"track {n}: no {annotator} notes")
                continue
            refs[annotator], skipped = read_notes(path)
            if skipped:
                problems.append(f"track {n} {annotator}: {skipped} rows skipped")
        if not refs["A1"]:
            problems.append(f"track {n}: no usable A1 notes; skipped")
            continue
        lyrics_path = lyrics_dir / f"vocadito_{n}_lyrics.txt"
        lyrics = [line.strip() for line in lyrics_path.read_text(encoding="utf-8", errors="replace").splitlines()
                  if line.strip()] if lyrics_path.exists() else []
        case = {"id": f"voc-{n}", "audio": os.path.relpath(audio.resolve(), out_dir.resolve()), "track": n,
                "singer": t["singer"], "language": t["language"], "english": t["language"] == "English",
                "averagePitch": t["averagePitch"], "lyrics": lyrics, "reference": "A1", "notes": refs["A1"]}
        if refs["A2"]:
            case["alternates"] = {"A2": {"notes": refs["A2"]}}
        cases.append(case)
    manifest = {"version": 1, "dataset": DATASET, "source": SOURCE,
                "comment": "Generated by tools/datasets/vocadito.py (do not edit; re-run it). Audio paths are relative "
                           "to this file. notes = annotator A1 (pitch: nearest semitone of 69+12*log2(Hz/440); midi: "
                           "unrounded; pitchHz: as annotated), alternates.A2 = annotator A2. No per-note lyrics.",
                "reference": "A1", "cases": cases}
    return manifest, make_split(cases), problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("dataset_root", help="extracted vocadito folder (with vocadito_metadata.csv)")
    parser.add_argument("--out-dir", default=str(Path(__file__).resolve().parents[2] / "reference" / "datasets" / DATASET),
                        help="where manifest.json and splits.json go (default reference/datasets/vocadito)")
    args = parser.parse_args()
    root, out_dir = Path(args.dataset_root).resolve(), Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest, split, problems = build(root, out_dir)
    for name, value in (("manifest.json", manifest), ("splits.json", split)):
        (out_dir / name).write_text(json.dumps(value, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    cases = manifest["cases"]
    print(f"{len(cases)} cases, {len({c['singer'] for c in cases})} singers, "
          f"{sum(len(c['notes']) for c in cases)} A1 notes, "
          f"{sum(len(c.get('alternates', {}).get('A2', {}).get('notes', [])) for c in cases)} A2 notes")
    for split_name, s in split["summary"].items():
        print(f"  {split_name}: {s['singers']} singers, {s['tracks']} tracks, {s['englishTracks']} English")
    for problem in problems:
        print(f"  note: {problem}", file=sys.stderr)
    print(f"wrote {out_dir / 'manifest.json'} and {out_dir / 'splits.json'}")


if __name__ == "__main__":
    main()
