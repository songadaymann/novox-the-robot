"""Intelligibility test: sing test sentences, transcribe them with Whisper, score word error rate.

This is the automated "render -> speech-to-text -> compare" loop. It runs locally and free
(Whisper via MLX on Apple Silicon). It's a development tool for tuning phoneme targets and
catching regressions, not something the singer runs at performance time.

    tools/.venv/bin/python tools/intelligibility.py                    # std + robot + female
    tools/.venv/bin/python tools/intelligibility.py --voices robot --limit 3 -v
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))

import jiwer  # noqa: E402
import mlx_whisper  # noqa: E402
import numpy as np  # noqa: E402

import novox  # noqa: E402

# Harvard sentences (IEEE 1969, public domain): phonetically balanced test material.
SENTENCES = [
    "the birch canoe slid on the smooth planks",
    "glue the sheet to the dark blue background",
    "it's easy to tell the depth of a well",
    "these days a chicken leg is a rare dish",
    "rice is often served in round bowls",
    "the juice of lemons makes fine punch",
    "the box was thrown beside the parked truck",
    "the hogs were fed chopped corn and garbage",
    "four hours of steady work faced us",
    "a large size in stockings is hard to sell",
    # Lyric-like lines with lots of sustained vowels.
    "i will sing you a song about the morning sun",
    "every day i write another little tune",
]

# A gentle stepwise melody, cycled across syllables (MIDI note numbers relative to a root).
MELODY = [0, 2, 4, 5, 7, 5, 4, 2, 4, 7, 9, 7]
VOWELS = {"AA", "AE", "AH", "AO", "AW", "AX", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"}
ROOTS = {"bass": 45, "male": 50, "std": 50, "robot": 50, "tenor": 55, "crooner": 50,
         "female": 60, "alto": 57, "soprano": 65, "kid": 62}


def count_syllables(singer: novox.Singer, text: str) -> int:
    return sum(1 for sym in singer.phonemize(text).split() if sym in VOWELS)


def make_score(singer: novox.Singer, text: str, voice: str, beat: float) -> dict:
    root = ROOTS.get(voice, 55)
    n = count_syllables(singer, text)
    notes = [f"{root + MELODY[i % len(MELODY)]} {beat}" for i in range(n)]
    notes[-1] = f"{root} {beat * 2}"  # land on the root, longer
    return {"tempo": 100, "voice": voice, "lyrics": text, "notes": notes}


def normalize(s: str) -> str:
    s = s.lower().replace("it's", "it is")
    s = re.sub(r"[^a-z' ]+", " ", s)
    return " ".join(s.split())


def transcribe(audio: np.ndarray, sr: int, model: str) -> str:
    # Whisper wants 16 kHz mono float32.
    idx = np.arange(0, len(audio), sr / 16000.0)
    audio16 = np.interp(idx, np.arange(len(audio)), audio).astype(np.float32)
    result = mlx_whisper.transcribe(audio16, path_or_hf_repo=model, language="en",
                                    condition_on_previous_text=False, fp16=True)
    return result["text"].strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--voices", default="std,robot,female")
    ap.add_argument("--beat", type=float, default=0.5, help="beats per syllable at 100 bpm")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--model", default="mlx-community/whisper-large-v3-turbo")
    ap.add_argument("--save", action="store_true", help="also write each rendering to out/intel/")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    sr = 48000
    singer = novox.Singer(sample_rate=sr)
    sentences = SENTENCES[: args.limit] if args.limit else SENTENCES
    report = {"model": args.model, "beat": args.beat, "voices": {}}
    outdir = ROOT / "out" / "intel"
    if args.save:
        outdir.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    for voice in args.voices.split(","):
        refs, hyps, rows = [], [], []
        for i, text in enumerate(sentences):
            audio = singer.render(make_score(singer, text, voice, args.beat))
            if args.save:
                singer.save_wav(outdir / f"{voice}_{i:02d}.wav", audio)
            hyp = normalize(transcribe(audio, sr, args.model))
            ref = normalize(text)
            wer = jiwer.wer(ref, hyp) if hyp else 1.0
            refs.append(ref)
            hyps.append(hyp or "<nothing>")
            rows.append({"ref": ref, "hyp": hyp, "wer": round(wer, 3)})
            if args.verbose:
                print(f"  [{voice}] {wer:4.0%}  {ref}\n{'':13}-> {hyp}")
        total = jiwer.wer(refs, hyps)
        report["voices"][voice] = {"wer": round(total, 3), "sentences": rows}
        print(f"{voice:10s} WER {total:6.1%}   ({len(sentences)} sentences)")

    (ROOT / "out").mkdir(exist_ok=True)
    (ROOT / "out" / "intelligibility.json").write_text(json.dumps(report, indent=2))
    print(f"done in {time.time() - t0:.0f}s; details in out/intelligibility.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
