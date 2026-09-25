"""Deep comparison of two renders of the same score on one sustained vowel.

Looks past the long-term spectrum at things that also shape timbre:
  1. pitch-period waveforms (pulse shape / phase / peakiness)
  2. harmonic-by-harmonic amplitudes
  3. LPC formant frequencies and bandwidths (the vowel's resonances)
  4. per-period crest factor, and noise between harmonics (harmonic-to-noise ratio)

    tools/.venv/bin/python tools/deep_compare.py out/ha/ha_demo_std.wav out/compare/ours_std_44k.wav \
        --at 6.2 --name you
"""

from __future__ import annotations

import argparse
from pathlib import Path

import librosa
import matplotlib
import numpy as np
import scipy.signal as sig

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SR = 44100
ROOT = Path(__file__).resolve().parents[1]


def load(p):
    y, _ = librosa.load(p, sr=SR)
    return y / (np.max(np.abs(y)) + 1e-9)


def align(a, b):
    """Seconds to add to a time in `a` to find the same moment in `b` (onset-envelope xcorr)."""
    ea = librosa.onset.onset_strength(y=a, sr=SR, hop_length=256)
    eb = librosa.onset.onset_strength(y=b, sr=SR, hop_length=256)
    c = sig.correlate(eb - eb.mean(), ea - ea.mean(), mode="full")
    lag = np.argmax(c) - (len(ea) - 1)
    return lag * 256 / SR


def f0_at(y, t):
    seg = y[int((t - 0.1) * SR):int((t + 0.1) * SR)]
    f0, vf, _ = librosa.pyin(seg, fmin=70, fmax=1000, sr=SR, frame_length=2048)
    return float(np.nanmedian(f0))


def periods(y, t, f0, n=3):
    """n consecutive pitch periods starting at the largest negative peak near t."""
    T = int(round(SR / f0))
    start = int(t * SR)
    win = y[start:start + 2 * T]
    k = start + int(np.argmin(win))  # glottal closure: sharpest negative excursion
    return y[k - T // 4:k - T // 4 + n * T], T


def harmonics(y, t, f0, count=40):
    seg = y[int((t - 0.08) * SR):int((t + 0.08) * SR)]
    seg = seg * np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(seg, n=1 << 16))
    freqs = np.fft.rfftfreq(1 << 16, 1 / SR)
    amps, noise = [], []
    for h in range(1, count + 1):
        fh = h * f0
        if fh > SR / 2 - 200:
            break
        band = (freqs > fh - f0 * 0.25) & (freqs < fh + f0 * 0.25)
        amps.append(spec[band].max())
        between = (freqs > fh + f0 * 0.35) & (freqs < fh + f0 * 0.65)
        noise.append(np.median(spec[between]))
    amps = 20 * np.log10(np.array(amps) + 1e-12)
    noise = 20 * np.log10(np.array(noise) + 1e-12)
    ref = amps.max()
    return amps - ref, noise - ref


def lpc_formants(y, t, order=None):
    seg = y[int((t - 0.03) * SR):int((t + 0.03) * SR)]
    seg = librosa.resample(seg, orig_sr=SR, target_sr=11025)  # formants below 5.5 kHz
    seg = sig.lfilter([1, -0.63], 1, seg) * np.hamming(len(seg))
    a = librosa.lpc(seg, order=order or 12)
    roots = [r for r in np.roots(a) if np.imag(r) > 0.01]
    ang = np.angle(roots)
    freqs = ang * 11025 / (2 * np.pi)
    bws = -0.5 * 11025 / (2 * np.pi) * np.log(np.abs(roots))
    fm = sorted((f, b) for f, b in zip(freqs, bws) if 90 < f < 5000 and b < 600)
    w, h = sig.freqz(1, a, worN=2048, fs=11025)
    return fm[:5], w, 20 * np.log10(np.abs(h) + 1e-12)


def crest_per_period(y, t, f0, n=10):
    p, T = periods(y, t, f0, n)
    return float(np.mean([np.max(np.abs(p[i*T:(i+1)*T])) / (np.sqrt(np.mean(p[i*T:(i+1)*T] ** 2)) + 1e-12)
                          for i in range(n)]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("ours")
    ap.add_argument("--at", type=float, nargs="+", default=[6.2], help="times (s, in the reference) inside sustained vowels")
    ap.add_argument("--name", default="deep")
    args = ap.parse_args()
    ya, yb = load(args.reference), load(args.ours)
    off = align(ya, yb)
    print(f"alignment: ours = reference + {off:.3f}s")

    rows = len(args.at)
    fig, ax = plt.subplots(rows, 3, figsize=(18, 4.2 * rows), squeeze=False)
    for r, t in enumerate(args.at):
        tb = t + off
        f0a, f0b = f0_at(ya, t), f0_at(yb, tb)
        pa, Ta = periods(ya, t, f0a)
        pb, Tb = periods(yb, tb, f0b)
        ax[r][0].plot(np.arange(len(pa)) / SR * 1000, pa / np.max(np.abs(pa)), label=f"reference (f0 {f0a:.0f})")
        ax[r][0].plot(np.arange(len(pb)) / SR * 1000, pb / np.max(np.abs(pb)), label=f"ours (f0 {f0b:.0f})", alpha=0.8)
        ax[r][0].set_title(f"t={t}s: three pitch periods (normalized)")
        ax[r][0].set_xlabel("ms")
        ax[r][0].legend(fontsize=8)

        ha, na = harmonics(ya, t, f0a)
        hb, nb = harmonics(yb, tb, f0b)
        k = min(len(ha), len(hb))
        x = np.arange(1, k + 1)
        ax[r][1].plot(x, ha[:k], "o-", ms=3, label="reference harmonics")
        ax[r][1].plot(x, hb[:k], "o-", ms=3, label="ours harmonics")
        ax[r][1].plot(x, na[:k], ":", label="reference noise between")
        ax[r][1].plot(x, nb[:k], ":", label="ours noise between")
        ax[r][1].set_ylim(-80, 3)
        ax[r][1].set_xlabel("harmonic number")
        ax[r][1].set_title("harmonic amplitudes (dB re strongest)")
        ax[r][1].legend(fontsize=8)

        fa, wa, ea = lpc_formants(ya, t)
        fb, wb, eb = lpc_formants(yb, tb)
        ax[r][2].plot(wa, ea - ea.max(), label="reference LPC envelope")
        ax[r][2].plot(wb, eb - eb.max(), label="ours LPC envelope")
        ax[r][2].set_xlabel("Hz")
        ax[r][2].set_ylim(-60, 3)
        ax[r][2].legend(fontsize=8)
        ax[r][2].set_title("vowel resonances (LPC)")

        hnr_a = float(np.mean(ha[:k][ha[:k] > -50] - na[:k][ha[:k] > -50]))
        hnr_b = float(np.mean(hb[:k][hb[:k] > -50] - nb[:k][hb[:k] > -50]))
        print(f"\n== t={t}s  f0 ref {f0a:.1f} / ours {f0b:.1f}")
        print("  formants ref : " + ", ".join(f"{f:.0f}Hz bw{b:.0f}" for f, b in fa))
        print("  formants ours: " + ", ".join(f"{f:.0f}Hz bw{b:.0f}" for f, b in fb))
        print(f"  crest/period ref {crest_per_period(ya, t, f0a):.2f}  ours {crest_per_period(yb, tb, f0b):.2f}")
        print(f"  harmonic-to-noise ref {hnr_a:.1f} dB  ours {hnr_b:.1f} dB")
        print("  harmonic dB ref : " + " ".join(f"{v:.0f}" for v in ha[:24]))
        print("  harmonic dB ours: " + " ".join(f"{v:.0f}" for v in hb[:24]))
    fig.tight_layout()
    out = ROOT / "out" / "compare" / f"deep_{args.name}.png"
    fig.savefig(out, dpi=75)
    print(f"\nwrote {out.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
