"""Measure a sung recording's voice character, and compare our engine against a reference.

    # stats for any recording
    tools/.venv/bin/python tools/voice_compare.py analyze reference/myriad/regina.wav

    # extract the reference melody, sing it with one of our voices, compare side by side
    tools/.venv/bin/python tools/voice_compare.py copy reference/myriad/regina.wav --voice tenor \
        --lyrics "sal-vay ray-jee-nah mah-tair mee-zeh-ree-kor-dee-ay"

Measurements: pitch contour (vibrato rate/depth/delay, scoop into notes, glide time, wander),
long-term average spectrum by band, spectral tilt, and upper-band noise.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import librosa
import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))

SR = 44100
HOP = 256
BANDS = [(0, 500), (500, 1000), (1000, 2000), (2000, 3000), (3000, 4000), (4000, 5000), (5000, 8000)]


def load(path: str) -> np.ndarray:
    y, _ = librosa.load(path, sr=SR, mono=True)
    return y / (np.max(np.abs(y)) + 1e-9)


def pitch_track(y: np.ndarray):
    f0, voiced, _ = librosa.pyin(y, fmin=65, fmax=1100, sr=SR, frame_length=2048, hop_length=HOP)
    midi = librosa.hz_to_midi(f0)
    t = librosa.times_like(f0, sr=SR, hop_length=HOP)
    return t, midi, voiced & np.isfinite(midi)


def segment_notes(t, midi, voiced, min_dur=0.12):
    """Split the voiced pitch contour into notes at jumps and gaps."""
    notes, cur = [], []

    def close():
        if len(cur) * HOP / SR >= min_dur:
            seg = np.array([midi[i] for i in cur])
            notes.append({"start": float(t[cur[0]]), "end": float(t[cur[-1]] + HOP / SR),
                          "midi": float(np.median(seg)), "frames": list(cur)})

    for i in range(len(midi)):
        if not voiced[i]:
            if cur and (i - cur[-1]) * HOP / SR > 0.05:
                close()
                cur = []
            continue
        if cur:
            ref = np.median([midi[j] for j in cur[-12:]])
            if abs(midi[i] - ref) > 0.8 and len(cur) > 6:
                close()
                cur = []
        cur.append(i)
    if cur:
        close()
    return notes


def vibrato_stats(t, midi, notes):
    rates, depths, delays = [], [], []
    for n in notes:
        if n["end"] - n["start"] < 0.6:
            continue
        idx = n["frames"]
        dev = np.array([midi[i] for i in idx]) - n["midi"]
        body = dev[int(len(dev) * 0.15):]
        if len(body) >= 40:  # remove slow drift with a linear fit (keeps the vibrato intact)
            x = np.arange(len(body))
            body = body - np.polyval(np.polyfit(x, body, 1), x)
        if len(body) < 40:
            continue
        spec = np.abs(np.fft.rfft(body * np.hanning(len(body)), n=16384))
        freqs = np.fft.rfftfreq(16384, d=HOP / SR)
        band = (freqs > 3) & (freqs < 9)
        if not band.any():
            continue
        rates.append(float(freqs[band][np.argmax(spec[band])]))
        depths.append(float((np.percentile(body, 95) - np.percentile(body, 5)) / 2))
        env = np.abs(dev)
        env = np.convolve(env, np.ones(20) / 20, mode="same")
        above = np.nonzero(env > 0.5 * np.max(env))[0]
        if len(above):
            delays.append(float(above[0] * HOP / SR))
    med = lambda v: round(float(np.median(v)), 3) if v else None  # noqa: E731
    return {"vibrato_rate_hz": med(rates), "vibrato_depth_semitones": med(depths),
            "vibrato_delay_s": med(delays), "notes_measured": len(rates)}


def onset_stats(midi, notes):
    """How far below/above the note pitch the first 40 ms sits, and how long until settled."""
    scoops, settle = [], []
    for n in notes:
        idx = n["frames"]
        if len(idx) < 20:
            continue
        seg = np.array([midi[i] for i in idx]) - n["midi"]
        scoops.append(float(np.mean(seg[:7])))
        inside = np.nonzero(np.abs(seg) < 0.25)[0]
        if len(inside):
            settle.append(float(inside[0] * HOP / SR))
    return {"attack_offset_semitones": round(float(np.median(scoops)), 3) if scoops else None,
            "attack_settle_s": round(float(np.median(settle)), 3) if settle else None}


def spectrum_stats(y, voiced_mask_frames):
    S = np.abs(librosa.stft(y, n_fft=4096, hop_length=HOP)) ** 2
    n = min(S.shape[1], len(voiced_mask_frames))
    S = S[:, :n][:, voiced_mask_frames[:n]]
    ltas = S.mean(axis=1)
    freqs = librosa.fft_frequencies(sr=SR, n_fft=4096)
    total = ltas[(freqs > 80) & (freqs < 8000)].sum()
    bands = {}
    for lo, hi in BANDS:
        e = ltas[(freqs >= lo) & (freqs < hi)].sum()
        bands[f"{lo}-{hi}"] = round(float(10 * np.log10(e / total + 1e-12)), 1)
    # Tilt: slope of smoothed LTAS (dB) vs octaves between 300 Hz and 4 kHz.
    sel = (freqs > 300) & (freqs < 4000)
    db = 10 * np.log10(np.convolve(ltas, np.ones(15) / 15, mode="same")[sel] + 1e-12)
    slope = np.polyfit(np.log2(freqs[sel]), db, 1)[0]
    return {"band_energy_db": bands, "tilt_db_per_octave": round(float(slope), 2)}, freqs, ltas


def analyze(path: str) -> dict:
    y = load(path)
    t, midi, voiced = pitch_track(y)
    notes = segment_notes(t, midi, voiced)
    stats = {"file": str(path), "notes": len(notes)}
    stats.update(vibrato_stats(t, midi, notes))
    stats.update(onset_stats(midi, notes))
    wander = []
    for n in notes:
        seg = np.array([midi[i] for i in n["frames"]])
        if len(seg) > 40:
            resid = seg - np.convolve(seg, np.ones(9) / 9, mode="same")
            wander.append(np.std(resid[8:-8]) * 100)  # trim filter edges
    stats["fast_pitch_wander_cents"] = round(float(np.median(wander)), 2) if wander else None
    spec, freqs, ltas = spectrum_stats(y, voiced)
    stats.update(spec)
    return {"stats": stats, "y": y, "t": t, "midi": midi, "voiced": voiced, "notes": notes,
            "freqs": freqs, "ltas": ltas}


def notes_to_score(notes, lyrics: str, voice: str) -> dict:
    """Seconds-based score from extracted notes, preserving rests between them."""
    out, cursor = [], notes[0]["start"] if notes else 0.0
    for n in notes:
        if n["start"] - cursor > 0.03:
            out.append({"pitch": "r", "start": cursor, "dur": n["start"] - cursor})
        out.append({"pitch": round(n["midi"]), "start": n["start"], "dur": max(0.08, n["end"] - n["start"])})
        cursor = n["end"]
    return {"units": "seconds", "voice": voice, "lyrics": lyrics, "notes": out}


def plot_pair(a: dict, b: dict, labels, out: Path):
    fig, axes = plt.subplots(3, 2, figsize=(16, 10), gridspec_kw={"height_ratios": [3, 1.3, 1.6]})
    for col, (r, label) in enumerate(zip((a, b), labels)):
        ax = axes[0][col]
        ax.specgram(r["y"], NFFT=1024, Fs=SR, noverlap=896, cmap="magma", vmin=-110)
        ax.set_ylim(0, 8000)
        ax.set_title(label)
        ax = axes[1][col]
        ax.plot(r["t"], np.where(r["voiced"], r["midi"], np.nan), lw=0.8)
        ax.set_ylabel("MIDI")
    for r, label in zip((a, b), labels):
        db = 10 * np.log10(np.convolve(r["ltas"], np.ones(9) / 9, mode="same") + 1e-12)
        db -= db[(r["freqs"] > 100) & (r["freqs"] < 8000)].max()
        axes[2][0].plot(r["freqs"], db, lw=0.9, label=label)
    axes[2][0].set_xlim(0, 8000)
    axes[2][0].set_ylim(-70, 3)
    axes[2][0].set_title("long-term average spectrum (voiced frames)")
    axes[2][0].legend()
    axes[2][1].axis("off")
    rows = []
    sa, sb = a["stats"], b["stats"]
    for k in ("vibrato_rate_hz", "vibrato_depth_semitones", "vibrato_delay_s", "attack_offset_semitones",
              "attack_settle_s", "fast_pitch_wander_cents", "tilt_db_per_octave"):
        rows.append([k, sa.get(k), sb.get(k)])
    for band in sa["band_energy_db"]:
        rows.append([f"band {band} Hz (dB)", sa["band_energy_db"][band], sb["band_energy_db"][band]])
    table = axes[2][1].table(cellText=rows, colLabels=["", labels[0], labels[1]], loc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(8)
    fig.tight_layout()
    fig.savefig(out, dpi=80)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["analyze", "copy", "pair"])
    ap.add_argument("path")
    ap.add_argument("other", nargs="?", default="", help="pair mode: our rendering of the same score")
    ap.add_argument("--voice", default="std")
    ap.add_argument("--voice-json", default="", help="voice object as JSON (overrides --voice)")
    ap.add_argument("--lyrics", default="")
    ap.add_argument("--name", default="")
    args = ap.parse_args()

    ref = analyze(args.path)
    if args.mode == "analyze":
        print(json.dumps(ref["stats"], indent=2))
        return 0

    if args.mode == "pair":
        ours = analyze(args.other)
        name = args.name or f"pair_{Path(args.path).stem}_vs_{Path(args.other).stem}"
        out = ROOT / "out" / "compare" / f"{name}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        plot_pair(ref, ours, ("reference", "ours"), out)
        for k in ref["stats"]:
            if k != "file":
                print(f"{k:28s} ref {json.dumps(ref['stats'][k])}\n{'':28s} us  {json.dumps(ours['stats'][k])}")
        print(f"wrote {out.relative_to(ROOT)}")
        return 0

    import novox

    singer = novox.Singer(sample_rate=SR)
    voice = json.loads(args.voice_json) if args.voice_json else args.voice
    score = notes_to_score(ref["notes"], args.lyrics, voice)
    audio = singer.render(score)
    name = args.name or f"{Path(args.path).stem}_{args.voice}"
    out_wav = ROOT / "out" / "compare" / f"{name}.wav"
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    singer.save_wav(out_wav, audio)
    ours = analyze(str(out_wav))
    (ROOT / "out" / "compare" / f"{name}_score.json").write_text(json.dumps(score, indent=1))
    plot_pair(ref, ours, ("reference", f"ours ({name})"), ROOT / "out" / "compare" / f"{name}.png")
    for k in ref["stats"]:
        if k in ("file",):
            continue
        print(f"{k:28s} ref {json.dumps(ref['stats'][k]):>60s}\n{'':28s} us  {json.dumps(ours['stats'][k]):>60s}")
    print(f"wrote out/compare/{name}.png and .wav")
    return 0


if __name__ == "__main__":
    sys.exit(main())
