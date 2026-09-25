"""Measure vowel formants from a render of scores/vowels.json (15 h_d words, one per vowel).

Finds the 15 voiced notes by energy, then takes LPC formant estimates (F1-F4 + bandwidths)
across the steady middle of each note (and at the start/end for diphthongs), median-averaged.

    tools/.venv/bin/python tools/vowel_formants.py out/ha/ha_vowels_std.wav [--compare ours.wav] [--json out.json]
"""

from __future__ import annotations

import argparse
import json

import librosa
import numpy as np
import scipy.signal as sig

SR = 44100
LPC_SR = 11025
VOWELS = ["IY", "IH", "EH", "AE", "AA", "AO", "UH", "UW", "AH", "ER", "EY", "AY", "AW", "OW", "OY"]
DIPHTHONGS = {"EY", "AY", "AW", "OW", "OY"}


def notes(y):
    """(start, end) seconds of the 15 sustained notes, from a smoothed energy envelope."""
    hop = 441
    rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=hop)[0]
    db = 20 * np.log10(rms + 1e-9)
    on = db > db.max() - 30
    on = sig.medfilt(on.astype(float), 21) > 0.5
    regions, start = [], None
    for i, v in enumerate(on):
        if v and start is None:
            start = i
        if not v and start is not None:
            if (i - start) * hop / SR > 0.6:
                regions.append((start * hop / SR, i * hop / SR))
            start = None
    if start is not None:
        regions.append((start * hop / SR, len(on) * hop / SR))
    return regions


def formants_at(y, t, order=12):
    seg = y[int((t - 0.025) * SR):int((t + 0.025) * SR)]
    seg = librosa.resample(seg, orig_sr=SR, target_sr=LPC_SR)
    seg = sig.lfilter([1, -0.63], 1, seg) * np.hamming(len(seg))
    a = librosa.lpc(seg.astype(np.float64), order=order)
    roots = [r for r in np.roots(a) if np.imag(r) > 0.01]
    f = np.angle(roots) * LPC_SR / (2 * np.pi)
    b = -0.5 * LPC_SR / (2 * np.pi) * np.log(np.abs(roots))
    fm = sorted((ff, bb) for ff, bb in zip(f, b) if 150 < ff < 4800 and bb < 500)
    return fm


def measure(y, a, b):
    """Median F1-F4 (and bandwidths) over sample points in [a, b]."""
    rows = []
    for t in np.linspace(a, b, 9):
        fm = formants_at(y, t)
        if len(fm) >= 4:
            rows.append([x for pair in fm[:4] for x in pair])
    if not rows:
        return None
    m = np.median(np.array(rows), axis=0)
    return {"F": [round(float(m[i]), 0) for i in (0, 2, 4, 6)], "B": [round(float(m[i]), 0) for i in (1, 3, 5, 7)]}


def analyze(path):
    y, _ = librosa.load(path, sr=SR)
    y = y / (np.abs(y).max() + 1e-9)
    regs = notes(y)
    out = {}
    if len(regs) != len(VOWELS):
        print(f"warning: found {len(regs)} notes in {path}, expected {len(VOWELS)}")
    for v, (s, e) in zip(VOWELS, regs):
        d = e - s
        if v in DIPHTHONGS:
            out[v] = {"start": measure(y, s + 0.2 * d, s + 0.45 * d), "end": measure(y, s + 0.8 * d, s + 0.93 * d)}
        else:
            out[v] = {"steady": measure(y, s + 0.3 * d, s + 0.8 * d)}
    return out


def fmt(m):
    if not m:
        return "  (no estimate)                      "
    return "  ".join(f"{f:5.0f}/{b:<4.0f}" for f, b in zip(m["F"], m["B"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--compare", default="")
    ap.add_argument("--json", default="")
    args = ap.parse_args()
    ref = analyze(args.path)
    ours = analyze(args.compare) if args.compare else None
    print(f"{'':10s} {'F1/B1':>11s} {'F2/B2':>11s} {'F3/B3':>11s} {'F4/B4':>11s}")
    for v in VOWELS:
        for part, m in ref.get(v, {}).items():
            label = f"{v} {part}"
            print(f"{label:10s} ref  {fmt(m)}")
            if ours:
                print(f"{'':10s} ours {fmt(ours.get(v, {}).get(part))}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(ref, f, indent=1)


if __name__ == "__main__":
    main()
