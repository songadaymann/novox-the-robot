"""Python bindings for the New Virtual Singer engine (ctypes over libnvs).

    import nvs
    singer = nvs.Singer()                       # finds build/libnvs.dylib and the dictionary
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

ROOT = Path(__file__).resolve().parents[2]
_LIB_NAMES = {"darwin": "libnvs.dylib", "win32": "nvs.dll"}


def _find_library() -> Path:
    name = _LIB_NAMES.get(sys.platform, "libnvs.so")
    for candidate in (os.environ.get("NVS_LIB"), ROOT / "build" / name):
        if candidate and Path(candidate).exists():
            return Path(candidate)
    raise FileNotFoundError(f"{name} not found; build the engine first (cmake --build build)")


class NvsError(RuntimeError):
    pass


class Singer:
    def __init__(self, sample_rate: float = 48000, dictionary: str | os.PathLike | None = None,
                 library: str | os.PathLike | None = None):
        self.sample_rate = sample_rate
        lib = ctypes.CDLL(str(library or _find_library()))
        lib.nvs_create.restype = ctypes.c_void_p
        lib.nvs_create.argtypes = [ctypes.c_double]
        lib.nvs_destroy.argtypes = [ctypes.c_void_p]
        lib.nvs_load_dictionary.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.nvs_add_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        lib.nvs_render.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                   ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                                   ctypes.POINTER(ctypes.c_size_t)]
        for fn in ("nvs_plan", "nvs_phonemize"):
            getattr(lib, fn).restype = ctypes.c_void_p
            getattr(lib, fn).argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.nvs_voice_names.restype = ctypes.c_void_p
        lib.nvs_last_error.restype = ctypes.c_char_p
        lib.nvs_last_error.argtypes = [ctypes.c_void_p]
        lib.nvs_free.argtypes = [ctypes.c_void_p]
        self._lib = lib
        self._engine = lib.nvs_create(sample_rate)
        dict_path = Path(dictionary) if dictionary else ROOT / "data" / "cmudict.dict"
        if lib.nvs_load_dictionary(self._engine, str(dict_path).encode()) < 0:
            raise NvsError(self._error())

    def __del__(self):
        if getattr(self, "_engine", None):
            self._lib.nvs_destroy(self._engine)
            self._engine = None

    def _error(self) -> str:
        return self._lib.nvs_last_error(self._engine).decode()

    def _take_string(self, ptr) -> str:
        if not ptr:
            raise NvsError(self._error())
        try:
            return ctypes.string_at(ptr).decode()
        finally:
            self._lib.nvs_free(ptr)

    @staticmethod
    def _score_text(score) -> bytes:
        return (score if isinstance(score, str) else json.dumps(score)).encode()

    def render(self, score):
        """Render a score (dict or JSON string) to mono float samples."""
        buf = ctypes.POINTER(ctypes.c_float)()
        count = ctypes.c_size_t()
        if self._lib.nvs_render(self._engine, self._score_text(score), ctypes.byref(buf), ctypes.byref(count)):
            raise NvsError(self._error())
        try:
            try:
                import numpy as np
                return np.ctypeslib.as_array(buf, shape=(count.value,)).copy()
            except ImportError:
                return buf[: count.value]
        finally:
            self._lib.nvs_free(buf)

    def plan(self, score) -> dict:
        """The syllables and phoneme segments the engine would sing."""
        return json.loads(self._take_string(self._lib.nvs_plan(self._engine, self._score_text(score))))

    def phonemize(self, text: str) -> str:
        return self._take_string(self._lib.nvs_phonemize(self._engine, text.encode()))

    def add_word(self, word: str, arpabet: str) -> None:
        if self._lib.nvs_add_word(self._engine, word.encode(), arpabet.encode()):
            raise NvsError(self._error())

    def voices(self) -> list[str]:
        return self._take_string(self._lib.nvs_voice_names()).split(",")

    def save_wav(self, path, samples) -> None:
        with open(path, "wb") as f:
            data = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, float(s))) * 32767)) for s in samples)
            rate = int(self.sample_rate)
            f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
            f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
            f.write(b"data" + struct.pack("<I", len(data)) + data)
