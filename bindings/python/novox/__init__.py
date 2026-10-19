"""Python bindings for the Novox The Robot engine (ctypes over libnovox).

    import novox
    singer = novox.Singer()
    audio = singer.render({"tempo": 100, "lyrics": "hel-lo", "notes": ["C4 1", "E4 1"]})
    singer.save_wav("hello.wav", audio)

`audio` is a list-like array of float samples (numpy array if numpy is installed).
"""

from __future__ import annotations

import ctypes
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]  # repository root when running from a source checkout
_LIB_NAMES = {"darwin": "libnovox.dylib", "win32": "novox.dll"}

__version__ = "0.1.0"


def _find_library() -> Path:
    name = _LIB_NAMES.get(sys.platform, "libnovox.so")
    # Installed wheel: next to this file. Source checkout: build/. Override: $NOVOX_LIB.
    for candidate in (os.environ.get("NOVOX_LIB"), HERE / name, ROOT / "build" / name):
        if candidate and Path(candidate).exists():
            return Path(candidate)
    raise FileNotFoundError(f"{name} not found; build the engine first (cmake --build build)")


class NovoxError(RuntimeError):
    pass


class Singer:
    def __init__(self, sample_rate: float = 48000, dictionary: str | os.PathLike | None = None,
                 library: str | os.PathLike | None = None):
        self.sample_rate = sample_rate
        lib = ctypes.CDLL(str(library or _find_library()))
        lib.novox_create.restype = ctypes.c_void_p
        lib.novox_create.argtypes = [ctypes.c_double]
        lib.novox_destroy.argtypes = [ctypes.c_void_p]
        lib.novox_load_dictionary.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.novox_add_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        lib.novox_render.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                   ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                                   ctypes.POINTER(ctypes.c_size_t)]
        for fn in ("novox_plan", "novox_phonemize"):
            getattr(lib, fn).restype = ctypes.c_void_p
            getattr(lib, fn).argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.novox_voice_names.restype = ctypes.c_void_p
        lib.novox_last_error.restype = ctypes.c_char_p
        lib.novox_last_error.argtypes = [ctypes.c_void_p]
        lib.novox_free.argtypes = [ctypes.c_void_p]
        lib.novox_transcribe.restype = ctypes.c_void_p
        lib.novox_transcribe.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                                         ctypes.c_double, ctypes.c_char_p, ctypes.c_char_p]
        lib.novox_import.restype = ctypes.c_void_p
        lib.novox_import.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
        lib.novox_stream_create.restype = ctypes.c_void_p
        lib.novox_stream_create.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.novox_stream_destroy.argtypes = [ctypes.c_void_p]
        for fn in ("novox_stream_length", "novox_stream_position"):
            getattr(lib, fn).restype = ctypes.c_size_t
            getattr(lib, fn).argtypes = [ctypes.c_void_p]
        lib.novox_stream_lead.restype = ctypes.c_double
        lib.novox_stream_lead.argtypes = [ctypes.c_void_p]
        lib.novox_stream_seek.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        lib.novox_stream_process.restype = ctypes.c_size_t
        lib.novox_stream_process.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t]
        self._lib = lib
        self._engine = lib.novox_create(sample_rate)
        # The CMU dictionary is compiled in; `dictionary` adds a cmudict-format file on top.
        if dictionary and lib.novox_load_dictionary(self._engine, str(dictionary).encode()) < 0:
            raise NovoxError(self._error())

    def __del__(self):
        if getattr(self, "_engine", None):
            self._lib.novox_destroy(self._engine)
            self._engine = None

    def _error(self) -> str:
        return self._lib.novox_last_error(self._engine).decode()

    def _take_string(self, ptr) -> str:
        if not ptr:
            raise NovoxError(self._error())
        try:
            return ctypes.string_at(ptr).decode()
        finally:
            self._lib.novox_free(ptr)

    @staticmethod
    def _score_text(score) -> bytes:
        return (score if isinstance(score, str) else json.dumps(score)).encode()

    def render(self, score):
        """Render a score (dict or JSON string) to mono float samples."""
        buf = ctypes.POINTER(ctypes.c_float)()
        count = ctypes.c_size_t()
        if self._lib.novox_render(self._engine, self._score_text(score), ctypes.byref(buf), ctypes.byref(count)):
            raise NovoxError(self._error())
        try:
            try:
                import numpy as np
                return np.ctypeslib.as_array(buf, shape=(count.value,)).copy()
            except ImportError:
                return buf[: count.value]
        finally:
            self._lib.novox_free(buf)

    def plan(self, score) -> dict:
        """The syllables and phoneme segments the engine would sing."""
        return json.loads(self._take_string(self._lib.novox_plan(self._engine, self._score_text(score))))

    def phonemize(self, text: str) -> str:
        return self._take_string(self._lib.novox_phonemize(self._engine, text.encode()))

    def add_word(self, word: str, arpabet: str) -> None:
        if self._lib.novox_add_word(self._engine, word.encode(), arpabet.encode()):
            raise NovoxError(self._error())

    def import_file(self, path, lyrics: str = "", track: int = -1, voice: str = "") -> dict:
        """Read a MIDI or MusicXML file into a score dict (render it with render())."""
        data = Path(path).read_bytes()
        opts = json.dumps({"lyrics": lyrics, "track": track, **({"voice": voice} if voice else {})})
        return json.loads(self._take_string(self._lib.novox_import(self._engine, data, len(data), opts.encode())))

    def transcribe(self, samples, sample_rate: float, words=None, **options) -> dict:
        """Sung audio (mono floats) -> score dict. `words`: [{"word", "start", "end", "probability"}]
        from a speech recognizer with word timestamps (e.g. Whisper), to add lyrics; a word may
        carry "tokens": [{"text", "time", "probability"}] (sub-word pieces and when the recognizer
        finished each), which singing-v2 uses as soft anchors.
        algorithm="singing-v1" selects candidate sequence decoding for singing; "singing-v2" is
        syllable-first (lyric syllables decide note starts, octave decided per note). The API
        default is "legacy" for compatibility. detail="natural" (default) gives singing-v2 one
        note per sung syllable plus real melodic steps; detail="detailed" also makes notes of
        held approaches, grace notes and droops (as annotators who mark every pitch write
        them); vibrato and glides without a plateau stay one note in both. debugFrames=True
        adds the analysis frames.
        Returned transcription metadata retains original timestamped words and unresolved word
        indices. See novox_transcribe in engine/include/novox.h for every option."""
        try:
            import numpy as np
            if not isinstance(samples, np.ndarray):
                raise ImportError
            array = np.ascontiguousarray(samples, dtype=np.float32)  # kept alive for the call
            data = array.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        except ImportError:
            data = (ctypes.c_float * len(samples))(*[float(x) for x in samples])
        wj = json.dumps(words).encode() if words else None
        oj = json.dumps(options).encode() if options else None
        ptr = self._lib.novox_transcribe(self._engine, data, len(samples), float(sample_rate), wj, oj)
        return json.loads(self._take_string(ptr))

    def stream(self, score) -> "Stream":
        """Incremental renderer (what a plugin uses): process(n) returns the next n samples."""
        ptr = self._lib.novox_stream_create(self._engine, self._score_text(score))
        if not ptr:
            raise NovoxError(self._error())
        return Stream(self, ptr)

    def voices(self) -> list[str]:
        return self._take_string(self._lib.novox_voice_names()).split(",")

    def save_wav(self, path, samples) -> None:
        """Write mono 16-bit PCM at this singer's sample rate."""
        import array
        pcm = array.array("h", (int(max(-1.0, min(1.0, float(s))) * 32767) for s in samples))
        if sys.byteorder == "big":
            pcm.byteswap()
        data = pcm.tobytes()
        rate = int(self.sample_rate)
        with open(path, "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
            f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
            f.write(b"data" + struct.pack("<I", len(data)) + data)

class Stream:
    """Real-time style rendering of one score. Create with Singer.stream(score)."""

    def __init__(self, singer: Singer, ptr):
        self._singer, self._lib, self._ptr = singer, singer._lib, ptr

    def __del__(self):
        if getattr(self, "_ptr", None):
            self._lib.novox_stream_destroy(self._ptr)
            self._ptr = None

    @property
    def length(self) -> int:
        return self._lib.novox_stream_length(self._ptr)

    @property
    def position(self) -> int:
        return self._lib.novox_stream_position(self._ptr)

    @property
    def lead(self) -> float:
        """Seconds of pre-roll before score time 0."""
        return self._lib.novox_stream_lead(self._ptr)

    def seek(self, sample: int) -> None:
        self._lib.novox_stream_seek(self._ptr, sample)

    def process(self, frames: int):
        buf = (ctypes.c_float * frames)()
        self._lib.novox_stream_process(self._ptr, buf, frames)
        try:
            import numpy as np
            return np.frombuffer(buf, dtype=np.float32).copy()
        except ImportError:
            return list(buf)
