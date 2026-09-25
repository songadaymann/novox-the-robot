"""Render a spectrogram PNG of a WAV file (for eyeballing formants, clicks and noise).

    tools/.venv/bin/python tools/spectrogram.py out/say.wav [out/say.png]
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy.io import wavfile  # noqa: E402

path = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else path.rsplit(".", 1)[0] + ".png"
sr, x = wavfile.read(path)
x = x.astype(np.float32) / 32768.0
fig, (a, b) = plt.subplots(2, 1, figsize=(14, 7), sharex=True, gridspec_kw={"height_ratios": [1, 4]})
t = np.arange(len(x)) / sr
a.plot(t, x, lw=0.4)
a.set_ylim(-1, 1)
b.specgram(x, NFFT=1024, Fs=sr, noverlap=896, cmap="magma", vmin=-120)
b.set_ylim(0, 10000)
b.set_xlabel("seconds")
b.set_ylabel("Hz")
fig.tight_layout()
fig.savefig(out, dpi=90)
print(out)
