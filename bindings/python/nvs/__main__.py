"""Command line: `nvs` (installed with pip) or `python -m nvs`.

    nvs say "hello my name is hugh" -o hello.wav --pitch D3 --voice robot
    nvs render song.json|song.mid|song.musicxml -o song.wav [--voice female] [--lyrics "..."]
    nvs import song.mid --lyrics "hap-py birth-day" -o song.json
    nvs plan song.json
    nvs phonemes "some words"
    nvs voices
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import NvsError, Singer

VOWELS = {"AA", "AE", "AH", "AO", "AW", "AX", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"}


def _load_score(singer: Singer, path: str, lyrics: str, track: int) -> dict:
    data = Path(path).read_bytes()
    if data.lstrip()[:1] == b"{":
        text = "\n".join(l for l in data.decode("utf-8").splitlines() if not l.strip().startswith("//"))
        score = json.loads(text)
        if lyrics:
            score["lyrics"] = lyrics
        return score
    score = singer.import_file(path, lyrics=lyrics, track=track)
    for w in score.pop("importWarnings", []):
        print(f"note: {w}", file=sys.stderr)
    return score


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="nvs", description="New Virtual Singer: formant singing synthesizer")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--voice", default="", help="voice preset (see `nvs voices`)")
        p.add_argument("--rate", type=int, default=48000, help="sample rate")
        p.add_argument("--dict", default="", help="extra cmudict-format dictionary")

    p = sub.add_parser("render", help="sing a score (JSON, MIDI or MusicXML) to a WAV file")
    p.add_argument("input")
    p.add_argument("-o", "--out", default="out.wav")
    p.add_argument("--lyrics", default="")
    p.add_argument("--track", type=int, default=-1)
    common(p)

    p = sub.add_parser("say", help="sing words on one pitch (quick check)")
    p.add_argument("text")
    p.add_argument("-o", "--out", default="out.wav")
    p.add_argument("--pitch", default="C4")
    p.add_argument("--beats", type=float, default=1.0)
    p.add_argument("--tempo", type=float, default=100)
    common(p)

    p = sub.add_parser("import", help="convert MIDI / MusicXML to a JSON score")
    p.add_argument("input")
    p.add_argument("-o", "--out", default="")
    p.add_argument("--lyrics", default="")
    p.add_argument("--track", type=int, default=-1)
    common(p)

    p = sub.add_parser("plan", help="print the syllable/phoneme timeline as JSON")
    p.add_argument("input")
    p.add_argument("--lyrics", default="")
    p.add_argument("--track", type=int, default=-1)
    common(p)

    p = sub.add_parser("phonemes", help="show how words are pronounced")
    p.add_argument("text")
    common(p)

    p = sub.add_parser("voices", help="list voice presets")
    common(p)

    args = ap.parse_args(argv)
    singer = Singer(sample_rate=args.rate, dictionary=args.dict or None)
    try:
        if args.cmd == "voices":
            print("\n".join(singer.voices()))
        elif args.cmd == "phonemes":
            print(singer.phonemize(args.text))
        elif args.cmd == "say":
            n = sum(1 for s in singer.phonemize(args.text).split() if s in VOWELS)
            score = {"tempo": args.tempo, "lyrics": args.text, "notes": [f"{args.pitch} {args.beats}"] * max(1, n)}
            if args.voice:
                score["voice"] = args.voice
            audio = singer.render(score)
            singer.save_wav(args.out, audio)
            print(f"wrote {args.out} ({len(audio) / args.rate:.2f} s)", file=sys.stderr)
        else:
            score = _load_score(singer, args.input, args.lyrics, args.track)
            if args.voice:
                score["voice"] = args.voice
            if args.cmd == "import":
                text = json.dumps(score, indent=1)
                if args.out:
                    Path(args.out).write_text(text)
                    print(f"wrote {args.out}", file=sys.stderr)
                else:
                    print(text)
            elif args.cmd == "plan":
                plan = singer.plan(score)
                for w in plan.get("warnings", []):
                    print(f"note: {w}", file=sys.stderr)
                print(json.dumps(plan, indent=1))
            else:
                audio = singer.render(score)
                singer.save_wav(args.out, audio)
                print(f"wrote {args.out} ({len(audio) / args.rate:.2f} s)", file=sys.stderr)
    except (NvsError, OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
