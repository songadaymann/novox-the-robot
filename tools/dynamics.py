"""Loudness/dynamics fingerprint of a sung render: sibilance vs vowels, loudness spread, crest factor.

    tools/.venv/bin/python tools/dynamics.py out/ha/ha_demo_std.wav out/compare/ours_std_44k.wav
"""
import sys

import librosa
import numpy as np


def stats(path):
    y, sr = librosa.load(path, sr=44100)
    y = y / (np.max(np.abs(y)) + 1e-9)
    hop, n = 441, 2048  # 10 ms hops
    S = np.abs(librosa.stft(y, n_fft=n, hop_length=hop)) ** 2
    f = librosa.fft_frequencies(sr=sr, n_fft=n)
    total = S.sum(0) + 1e-12
    hi = S[f > 4000].sum(0)
    rms_db = 10 * np.log10(total / n)
    active = rms_db > rms_db.max() - 45
    ratio = hi / total
    fric = active & (ratio > 0.5)            # noise-dominated frames: S, SH, F, bursts
    vowel = active & (ratio < 0.05)          # voiced, low-frequency-dominated frames
    sib = np.median(rms_db[fric]) - np.median(rms_db[vowel]) if fric.any() else float("nan")
    sib_peak = np.percentile(rms_db[fric], 90) - np.median(rms_db[vowel]) if fric.any() else float("nan")
    # Loudness spread of sustained vowels (compression narrows this).
    spread = np.percentile(rms_db[vowel], 90) - np.percentile(rms_db[vowel], 10)
    crest = 20 * np.log10(np.max(np.abs(y)) / np.sqrt(np.mean(y[np.abs(y) > 1e-4] ** 2)))
    return {"sibilant_minus_vowel_db": round(float(sib), 1), "sibilant_p90_minus_vowel_db": round(float(sib_peak), 1),
            "vowel_loudness_spread_db": round(float(spread), 1), "crest_factor_db": round(float(crest), 1),
            "fricative_frames": int(fric.sum())}


for p in sys.argv[1:]:
    print(p, stats(p))
