"""Round-trip test for audio-in: sing a known score, transcribe it, compare notes.

    tools/.venv/bin/python tools/transcribe_eval.py [--voices std,female] [--whisper] [-v]

Reports note precision/recall (a transcribed note matches a true note if the
pitch is equal and the onset is within 60 ms), mean onset error, and with
--whisper the lyric word accuracy after alignment.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))
import novox  # noqa: E402

SR = 44100


def load_score(p):
    return json.loads("\n".join(l for l in Path(p).read_text().splitlines() if not l.strip().startswith("//")))


def truth(singer, score):
    """True notes (seconds, audio time) with their sung syllable text."""
    plan = singer.plan(score)
    tempo = score.get("tempo", 120)
    out, cur = [], 0.0
    for n in score["notes"]:
        p, d = (n.split()[0], float(n.split()[1])) if isinstance(n, str) else (n["pitch"], n["dur"])
        if str(p).lower() != "r":
            out.append({"midi": parse_pitch(p), "start": cur * 60 / tempo + plan["lead"]})
        cur += d
    return out, plan


def parse_pitch(p):
    if isinstance(p, (int, float)):
        return round(p)
    base = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[p[0].upper()]
    i = 1
    while i < len(p) and p[i] in "#b":
        base += 1 if p[i] == "#" else -1
        i += 1
    return (int(p[i:]) + 1) * 12 + base


def match(true_notes, got):
    used, hits, errs = set(), 0, []
    for t in true_notes:
        best = None
        for j, g in enumerate(got):
            if j in used or round(g["pitch"]) != t["midi"]:
                continue
            e = abs(g["start"] - t["start"])
            if e < 0.06 and (best is None or e < best[1]):
                best = (j, e)
        if best:
            used.add(best[0])
            hits += 1
            errs.append(best[1])
    return hits, errs


def whisper_words(audio):
    import mlx_whisper
    idx = np.arange(0, len(audio), SR / 16000.0)
    a16 = np.interp(idx, np.arange(len(audio)), audio).astype(np.float32)
    r = mlx_whisper.transcribe(a16, path_or_hf_repo="mlx-community/whisper-large-v3-turbo", language="en",
                               word_timestamps=True, condition_on_previous_text=False)
    return [{"word": w["word"].strip(), "start": w["start"], "end": w["end"], "probability": w.get("probability", 1)}
            for seg in r["segments"] for w in seg.get("words", [])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voices", default="std,robot,female,kid,crooner,choir")
    ap.add_argument("--scores", default="scores/demo.json")
    ap.add_argument("--whisper", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    singer = novox.Singer(sample_rate=SR)
    for path in args.scores.split(","):
        base = load_score(ROOT / path)
        for voice in args.voices.split(","):
            score = dict(base, voice=voice)
            audio = singer.render(score)
            t, plan = truth(singer, score)
            words = whisper_words(audio) if args.whisper else None
            got = singer.transcribe(audio, SR, words=words)["notes"]
            hits, errs = match(t, got)
            prec = hits / max(1, len(got))
            rec = hits / max(1, len(t))
            line = (f"{Path(path).stem:8s} {voice:8s} notes true {len(t):3d} got {len(got):3d}  "
                    f"precision {prec:5.0%} recall {rec:5.0%}  onset err {1000 * np.mean(errs) if errs else 0:4.0f} ms")
            if args.whisper:
                sung = [s["text"].strip(",.").lower() for s in plan["syllables"]]
                heard = [n.get("lyric", "") for n in got]
                line += f"  lyric tokens {' '.join(heard)[:80]}"
            print(line)
            if args.verbose:
                for g in got:
                    print(f"    {g['start']:6.3f} {g['dur']:5.3f} {g['pitch']:5.1f} {g.get('lyric', '')}")


if __name__ == "__main__":
    main()
