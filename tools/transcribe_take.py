"""Real singing evaluation; outputs scores, plugin-importable MIDI and listening comparisons.

tools/.venv/bin/python tools/transcribe_take.py reference/novox-test.wav --out out/novox-test
Uses local MLX Whisper; caches words.json so pitch experiments do not rerun ASR.
The 1.5-semitone setting is an experiment, not an engine default.
"""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings/python"))
import novox
from transcribe_eval import whisper_words, parse_pitch


def vlq(n):
    result = [n & 127]
    while n >> 7:
        n >>= 7
        result.insert(0, (n & 127) | 128)
    return bytes(result)


def save_midi(score, path):
    # At 120 BPM and 960 PPQ there are 1920 ticks per second; preserve free timing.
    events = [(0, 0, b"\xff\x51\x03\x07\xa1\x20")]
    for n in score["notes"]:
        start = round(n["start"] * 1920)
        end = max(start + 1, round((n["start"] + n["dur"]) * 1920))
        pitch = max(0, min(127, round(n["pitch"])))
        if n.get("lyric"):
            lyric = n["lyric"].encode("utf-8")
            events.append((start, 1, b"\xff\x05" + vlq(len(lyric)) + lyric))
        events.extend([(start, 2, bytes([0x90, pitch, 100])), (end, 0, bytes([0x80, pitch, 0]))])
    track, previous = bytearray(), 0
    for tick, _, data in sorted(events):
        track.extend(vlq(tick - previous) + data)
        previous = tick
    track.extend(b"\x00\xff\x2f\x00")
    path.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 960) + b"MTrk" + struct.pack(">I", len(track)) + track)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.out / "source.wav"
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(args.input),
                    "-ac", "1", "-ar", "44100", str(source)], check=True)
    sr, pcm = wavfile.read(source)
    audio = pcm.astype(np.float32) / 32768
    singer = novox.Singer(sr)
    words_path = args.out / "words.json"
    words = json.loads(words_path.read_text()) if words_path.exists() else whisper_words(audio)
    words_path.write_text(json.dumps(words, indent=2))
    rows = ["variant\tmelody_notes\taligned_notes\tshort_notes_under_200ms\twarnings"]
    plots = []
    for name, options in [("baseline", {}), ("stabilized", {"stabilizePitch": True}),
                          ("sustain-experiment", {"splitSemitones": 1.5, "stabilizePitch": True})]:
        melody = singer.transcribe(audio, sr, **options)
        score = singer.transcribe(audio, sr, words=words, **options)
        for suffix, data in [("melody", melody), ("score", score)]:
            (args.out / f"{name}-{suffix}.json").write_text(json.dumps(data, indent=2))
        save_midi(score, args.out / f"{name}.mid")
        # Check the export using the same engine importer used by the plugin.
        imported = singer.import_file(args.out / f"{name}.mid")
        assert len(imported["notes"]) == len(score["notes"])
        for original, restored in zip(score["notes"], imported["notes"]):
            assert original["pitch"] == parse_pitch(restored["pitch"])
            assert abs(original["start"] - restored["start"]) < 0.001
            assert abs(original["dur"] - restored["dur"]) < 0.001
            assert original.get("lyric", "") == restored.get("lyric", "")
        rendered = np.asarray(singer.render(dict(score, voice="std")))
        singer.save_wav(str(args.out / f"{name}-novox.wav"), rendered)
        lead = round(singer.plan(score)["lead"] * sr)
        aligned = rendered[lead:lead + len(audio)]
        aligned = np.pad(aligned, (0, max(0, len(audio) - len(aligned))))
        # Source, pause, Novox; peak match solely for comfortable comparison.
        peak = max(float(np.max(np.abs(aligned))), 1e-6)
        ab = np.concatenate([audio, np.zeros(sr), aligned * np.max(np.abs(audio)) / peak])
        singer.save_wav(str(args.out / f"{name}-AB.wav"), ab)
        rows.append(f"{name}\t{len(melody['notes'])}\t{len(score['notes'])}\t"
                    f"{sum(n['dur'] < .2 for n in score['notes'])}\t{score.get('transcribeWarnings', [])}")
        (args.out / f"{name}-notes.tsv").write_text("start\tduration\tpitch\tlyric\tconfidence\n" +
            "\n".join(f"{n['start']}\t{n['dur']}\t{n['pitch']}\t{n.get('lyric', '')}\t{n['confidence']}"
                      for n in score["notes"]))
        plots.append((name, score))
    (args.out / "summary.tsv").write_text("\n".join(rows) + "\n")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1 + len(plots), 1, figsize=(15, 9), sharex=True)
    axes[0].plot(np.arange(len(audio))[::128] / sr, audio[::128], linewidth=.5)
    axes[0].set_ylabel("Recording")
    for ax, (name, score) in zip(axes[1:], plots):
        for n in score["notes"]:
            ax.plot([n["start"], n["start"] + n["dur"]], [n["pitch"]] * 2, linewidth=4)
            ax.text(n["start"], n["pitch"] + .35, n.get("lyric", ""), fontsize=8)
        ax.set_ylabel(name + "\nMIDI pitch")
        ax.set_ylim(46, 61)
        ax.grid(alpha=.2)
    axes[-1].set_xlabel("Seconds (unquantized)")
    fig.suptitle("Novox real-voice test: note segmentation and lyric alignment")
    fig.tight_layout()
    fig.savefig(args.out / "comparison.png", dpi=150)
    print("\n".join(rows))


if __name__ == "__main__":
    main()
