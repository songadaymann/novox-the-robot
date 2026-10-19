"""Novox import lab: measure and iterate on sung-vocal import (audio -> piano roll).

The plugin imports a recording like this (plugin/Source/AudioImport.cpp):

    decode -> mono 16 kHz (JUCE resampler) -> whisper.cpp words (DTW timestamps)
           -> novox_transcribe(audio, words, {"algorithm": "singing-v1"}) -> notes

Decoding and speech recognition do not depend on the engine and are slow, so the lab
caches both from a real run of the plugin's own import code (the NovoxPluginTests
binary) and then re-runs only the engine on the cache. Engine changes measured here
therefore predict what the plugin will put on the piano roll for the same file.

Run with the native arm64 venv (never the x86 python3 that may be first on PATH):

    tools/.venv/bin/python tools/import_lab.py cache reference/novox-tests/life.wav --id life
    tools/.venv/bin/python tools/import_lab.py run life [--options '{"onsetDb": 4}'] [--tag try1]
    tools/.venv/bin/python tools/import_lab.py corpus [--options '{"algorithm": "singing-v2"}'] --tag try1
    tools/.venv/bin/python tools/import_lab.py compare baseline2 try1
    tools/.venv/bin/python tools/import_lab.py hyphens try1        (digits / missing hyphens in note lyrics)
    tools/.venv/bin/python tools/import_lab.py corpus --split test --tag try1     (held out; confirm only)
    tools/.venv/bin/python tools/import_lab.py asr life sleep --variant greedy beam5   (ASR experiments)
    tools/.venv/bin/python tools/import_lab.py corpus --words words-beam5.json --tag try-beam5
    tools/.venv/bin/python tools/import_lab.py corpus reference/datasets/vocadito/manifest.json \
        --lab out/import-lab-vocadito --tag v1           (another dataset; see tools/datasets/README.md)
    tools/.venv/bin/python tools/import_lab.py agree reference/datasets/vocadito/manifest.json   (A2 vs A1)

Typical loop: change the engine, rebuild build/libnovox.dylib, `corpus --tag tryN` (dev split, in
parallel, no plots), read its SCORE line, `compare baseline2 tryN` to see which cases moved and
`run ID --tag tryN` for one case's plot.

Subcommands
  cache AUDIO --id ID   Runs `NovoxPluginTests --audio AUDIO` (whisper.cpp + singing-v1, the
                        exact plugin import; the binary also runs its audition/preview
                        checks). Writes LAB/ID/:
                          plugin-result.json  the plugin's import result, as written by the binary
                          input16k.wav        the 16 kHz mono audio the plugin analyzed ("original")
                          words.json          the whisper.cpp words the plugin gave the engine
                                              (see Words below)
                          gt.json             ground-truth notes, when found (see References)
                          cache.json          provenance (source hash, binary, checks, timing)
                          plugin-run.log      the binary's output
                        --reuse skips the plugin run when plugin-result.json exists (refreshes the
                        extracted files, ground truth and baselines after lab changes).
                        then does a default engine run (runs/latest/) with a reproduction check
                        and analyzes the plugin's own notes (runs/plugin/, the corpus baseline).
                        Files are replaced atomically (other runs may be reading them); an
                        unchanged gt.json is left untouched.
  run ID...             Re-runs the engine through bindings/python/novox (libnovox from build/,
                        or --lib) on input16k.wav at 16 kHz with the cached words (--words FILE in the
                        case folder instead of words.json, e.g. words-beam5.json from `asr`). --options is
                        merged onto {"algorithm": "singing-v1"} and passed through unchanged (any
                        "algorithm" the engine knows; an unknown one fails that case only);
                        --no-words gives the engine no words (melody only; run.json records "(no words)");
                        "debugFrames": true is always added to obtain the analysis frames (it never
                        changes the notes). Writes LAB/ID/runs/TAG/ (default tag "latest"; files of
                        an earlier run with the same tag are removed first):
                          score.json   engine output (without the frames)
                          frames.tsv   time, midi (0 = unvoiced), db, clarity, confidence
                          notes.tsv    start, dur, pitch, detectedPitch, pitchSpread, confidence,
                                       lyric, wordIndex, word
                          metrics.json metrics (below) + comparison with plugin-result.json
                          plot.png     pitch frames, notes + lyrics, references, words, level
                                       (1.5 inch/second); --no-plot skips it
                          run.json     options, words file, library, timing (compare scores the run
                                       with the words file it ran on); engineInvalidUtf8 (also in
                                       metrics.json) when the engine's JSON had invalid UTF-8, which the
                                       lab replaces with U+FFFD (a lyric cut inside a multi-byte character
                                       of a non-English word, e.g. 'tete' with a circumflex)
                          error.json   only when the engine failed (the message; no other files)
  corpus [MANIFEST]     Runs every cached case of --split (default dev; see Splits) listed in MANIFEST
                        (every cached case in the lab when omitted) in --jobs worker processes
                        (default min(8, CPUs); each loads its own libnovox), without plots unless
                        --plot. Prints a structure table, a table against the silver/verified
                        reference and one against gt.json, with a TOTAL row per split, then the
                        per-split AGGREGATES and one headline line per split, easy to grep:
                          SCORE dev: conpF=0.512 octErr=12 frag=30 merged=4 phantom=41 missed=9 lyricW=0.701 cases=15/15 verified=0
                        and one held-note line per split (see heldNotes; "no reference with lyrics" for vocadito):
                          HELD dev: P=0.400 R=0.200 F=0.267 ref=45 pred=22 found=9 asNew=12 asHum=1 merged=20 missed=3 ...
                        Values that differ from the --baseline tag (default "plugin"; its stored
                        notes are re-scored against today's references) show the change in
                        parentheses. A failing case is reported and left out of every total; the
                        run goes on and cases=ok/total shows it. --cache-missing caches listed but
                        uncached cases first (ground truth from MANIFEST, else the manifest next to the
                        audio). --words FILE / --no-words as for run (a case without that file fails).
                        --by FIELD... (default singer english) adds one table per manifest case field
                        (rows per split and value, against the headline reference; ordered by the mean
                        "averagePitch" when cases have one) and, for a field with at most 4 values, one
                        line per value, e.g. `SCORE dev english=yes: ...`; a field no case has is skipped.
                        Writes LAB/corpus-TAG.json (options, per-case headline
                        metrics and --by fields, tables, aggregates and baseline aggregates per split,
                        "groups" per --by field). Exits 1 after the report when a case failed.
  compare TAG_A TAG_B   Re-scores both tags' stored runs (runs/TAG/score.json and frames.tsv; no engine
                        run, nothing written) against the current references, in parallel, and
                        prints per-case values of TAG_B with the change from TAG_A in parentheses,
                        then per-split aggregates of both and their delta (B - A). Only cases that
                        have both runs count. --split as for corpus.
  hyphens TAG [MANIFEST]  Scans the stored note lyrics of TAG (runs/TAG/score.json, --split as for corpus;
                        MANIFEST only for its splits.json) against the transcript words: digits left in a
                        lyric, words whose syllables sit on several notes without hyphens ("re" "mem" "ber"),
                        and (--verbose lists them) a hyphen into the next word or into a rest. Prints the
                        digit and missing-hyphen cases and one line, e.g.
                          HYPHENS dev 's4': digit=0 missingHyphen=0 extraHyphen=0 danglingHyphen=0 cases=15 notes=627
                        Words read out as other words (numbers, &, %, the letter W) take the notes up to the
                        next word unchecked.
  asr [ID...]           Speech-recognition experiment on dev cases (every cached dev case when no ID;
                        held-out cases are refused): for each --variant runs the plugin import with
                        `NovoxPluginTests --audio SOURCE --whisper-variant VARIANT` (a comma list of
                        greedy, beam5, prompt, nst, nofallback, entropy=X, maxlen1, maxtok16; see
                        plugin/Tests/PluginTests.cpp; the plugin always uses the defaults) and writes
                        LAB/ID/words-VARIANT.json (commas become '+'; words.json is never touched),
                        one at a time (GPU). The variant "cached" scores words.json without a run;
                        --reuse scores an existing words-VARIANT.json. Prints, per case and variant,
                        recognized words against the words sung in the silver/verified reference
                        (see Word recall) and whisper_full seconds; writes LAB/asr-VARIANT.json. A case
                        without a silver/verified reference gets its words but no score.
                        --words-only only writes words-VARIANT.json (no scoring, no table, no
                        asr-VARIANT.json) and also runs held-out cases (every cached case when no ID):
                        words are engine inputs, not results.
  agree MANIFEST        Human agreement ceiling: each case's alternate reference (--alternate KEY,
                        default A2, from the case's "alternates": {KEY: {"notes": [...]}}) is scored as
                        if it were a detected result against the case's "notes" (e.g. annotator A1, what
                        gt.json holds), with the reference metrics below; per-case table with TOTAL per
                        split, the --by tables and `AGREE dev: ...` lines. --split as for corpus. Needs
                        no cache and no engine.

Words (words.json, as the plugin passes them to novox_transcribe; AudioImport.cpp)
  [{"word", "start", "end", "probability", "tokens": [{"text", "time", "probability"}], "asrEnd"}]
  word         whisper.cpp text of the word, punctuation included ("die,")
  start        the previous word's end (0 for the first word): NOT an onset
  end          DTW completion time of the word's last token (seconds); the LAST word's end is forced
               to the end of the file (a final held vowel can outlast the last token)
  probability  minimum token probability
  tokens       the word's whisper pieces in order: text as decoded (a leading space starts a word,
               " re", "member"), time = DTW completion of that piece (t_dtw, else t1; seconds,
               approximate, can be early on held vowels), probability of that piece
  asrEnd       only on the last word: its measured end before it was forced to the file end
  Caches made before tokens existed keep their previous words as words-v0.json.

Word recall (asr)
  The reference's sung words: one per note that starts a word (lyric not '_', wordSyllable 0 or not
  continuing the previous note's word); melisma notes extend the word before them. Words compare as
  lowercase letters and apostrophes; a hyphenated vocable run ("la-la-la") counts as its parts.
  recall/precision: longest common subsequence of the word texts over reference / recognized words.
  timed: the same, a match also needing the recognized word's DTW end (asrEnd for the last word) within
  ASR_TIME_TOLERANCE of the reference word's span (first note start .. last note end).
  vocables: recall over reference words in VOCABLES ("oh", "la", "ooh", ...).

Manifests are JSON {"cases": [{"id", "audio", optional "notes"}]} with audio paths relative
to the manifest (reference/novox-tests/manifest.json and reference/corpus/manifest.json). Other keys
are kept: corpus --by reads fields such as "singer", "english" and "averagePitch", and agree reads
"alternates" (dataset manifests from tools/datasets/, e.g. reference/datasets/vocadito/manifest.json).
Keep each dataset in its own --lab (e.g. out/import-lab-vocadito): corpus without a MANIFEST runs every
case cached in the lab.

References (LAB/ID/, read fresh on every run, corpus and compare)
  gt.json           human/MIDI reference, scored as "groundTruth" (life and sleep: Jonathan's
                    intended melody, quantized MIDI). `cache` stores it when --gt FILE is given, or
                    when a manifest.json next to the audio (or --manifest) has a case for this file
                    with "notes" (start, dur, pitch, and lyric/word when the manifest notes have them,
                    e.g. reference/datasets/gtsinger/manifest.json; then gt.json gets lyric metrics too).
                    A hand-written LAB/ID/gt.json also works.
  gt-verified.json  agent reference checked by an auditor; preferred over
  gt-silver.json    agent reference ("what a musician would notate": one note per sung syllable).
                    The better of the two is scored as "silver" with "silverSource": "verified" or
                    "silver"; it is the reference of the SCORE line. A new gt-verified.json is used
                    by the next command that scores the case.
  Headline reference: silver/verified when the case has one, else gt.json (a dataset with human notes
                    only); SCORE lines, compare and corpus-TAG.json use it, and add " gt=N" when N
                    cases were scored against gt.json. Lyric metrics are null ("-") for a reference
                    without lyrics.
  Format: {"notes": [...]} or a bare list of {"start", "dur" (seconds), "pitch" (MIDI), optional
  "lyric" (engine style: 'hel-', 'lo', '_' melisma, '' hum), optional "word" (the whole word the
  note's syllable belongs to)}; other keys are ignored. Notes without numeric start/dur/pitch are
  skipped and an unreadable file (it may be mid-write) is ignored, both with a warning
  (metrics.json "referenceWarnings").

Splits: reference/corpus/splits.json (--splits) lists the held-out "test" ids; every other case is
  dev. Without --splits, corpus and agree use the splits.json next to their MANIFEST when there is one
  (reference/corpus/manifest.json -> reference/corpus/splits.json, the default); other commands with
  another --lab warn that they fall back to reference/corpus/splits.json. corpus and compare default to --split dev; test cases run only with --split test or all, and
  `run` on one prints a reminder. Do not tune on test results.

Metrics (metrics.json, every run)
  notes, syllables (expected from the words via the engine lexicon), notesPerSyllable,
  shortNotes (< 120 ms), intraWordSplits (adjacent notes of one word, |pitch diff| <= 1 st,
  gap < 50 ms), excessIntraWordSplits (those beyond the word's syllable count),
  octaveJumpsInWord (adjacent notes of one word >= 9 st apart), frameOctaveJumps (events
  where voiced frames change >= 9 st within 40 ms of continuous voicing), melismaNotes
  ('_' lyrics), lyriclessNotes, unassignedWords.
  Against each reference ("groundTruth" = gt.json, "silver" = gt-verified/gt-silver.json):
    referenceNotes (also gtNotes), notesPerReferenceNote (notes / referenceNotes)
    conp            COnP precision/recall/F: one-to-one (Hungarian) matching, onset within
                    100 ms and pitch within 0.5 st; onsetOnly: the same without the pitch test
    onsetMedianAbsMs  median |onset error| of the COnP matches (conpOnsetErrorsMs lists them)
    The "best overlap" of a reference note is the detected note with the largest time overlap:
    octaveErrors    best overlap 11-13 or 23-25 st off
    pitchErrorsNonOctave  best overlap >= 1 st off and not an octave error
    pitchRightByOverlap   best overlap within 0.5 st
    octaveFragments any detected note overlapping >= 30 ms that is an octave off
    fragmented      reference notes that are the best overlap of > 1 detected note
    merged          detected notes that are the best overlap of > 1 reference note
    phantom         detected notes that overlap no reference note by >= 30 ms (phantomNotes)
    missed          reference notes that no detected note overlaps by >= 30 ms
    lyricAccuracy   share of reference notes whose best overlap has the same lyric, compared
                    lowercase, letters and apostrophes only, outer apostrophes dropped ('Hel-' =
                    'hel'); '_' and '' both mean melisma/hum and equal each other; no overlap = wrong
    lyricWordAccuracy  also right when the detected lyric's letters are contained in the reference
                    note's word ("word", else rebuilt from the reference hyphenation: 'ev-' 'ery-'
                    'thing' -> everything, '_' belongs to the word before it), i.e. the right word
                    hyphenated differently. Both are null for a reference without lyrics (e.g. a MIDI gt.json).
    heldNotes       held notes ('_': the syllable before continues onto a new pitch) against a reference
                    with lyrics (null without). A reference note is "held" ('_'), a "new" syllable (letters)
                    or unlabelled ('' hum, left out). Recall side, per reference '_' by its best overlap
                    (>= 30 ms): found (a predicted '_'), merged (the same note as the reference note before
                    it: no note of its own), asNew (a new syllable there: the lyrics drift), asHum (a
                    lyric-less note), missed (no overlap); missedOnset counts reference new syllables whose
                    best overlap is a predicted '_' (the result holds over a sung onset). Precision side,
                    per predicted '_' by its best-overlap reference note: correct when that note is a '_'
                    (predicted '_' on unlabelled reference notes are left out). precision = correct /
                    predicted, recall = found / reference, f. heldWhere lists every reference note in a
                    category other than found (start, lyric, word, category, detectedLyric); perGtNote has
                    "held" for those notes too.
    perGtNote       per reference note: its best overlap, flags and COnP onset error
  The plot draws gt.json as solid orange boxes and the silver/verified notes as dashed violet
  boxes with their lyric below.

Aggregates (corpus TOTAL rows, AGGREGATES and corpus-TAG.json, per split)
  Sums over the split's successful cases of notes, syllables, shortNotes, intraWordSplits,
  octaveJumpsInWord, frameOctaveJumps, ...; per reference kind sums of notes, referenceNotes,
  octaveErrors, pitchErrorsNonOctave, fragmented, merged, phantom, missed; micro-averaged COnP and
  onset-only P/R/F (summed matches over summed note counts); the pooled median COnP onset error;
  lyricAccuracy and lyricWordAccuracy weighted by reference notes (summed correct over summed
  reference notes); heldNotes counts summed and P/R/F micro-averaged over cases with lyrics. The SCORE
  and HELD lines report the headline reference (silver/verified, else gt.json).

Reproducibility: the plugin hands the engine float audio; the cache can only hold the
16-bit WAV the plugin stores as "original" (JUCE scales int16 by 1/32768 both ways), so the
lab re-runs on audio within 1/32768 of the plugin's. Every run compares its notes with
plugin-result.json ("plugin" in metrics.json); for default options and an unchanged
engine they should be identical (pitch, start, dur, lyric, warnings, unassigned words);
detectedPitch may move by a few thousandths of a semitone from the int16 rounding
(tolerance FRAME_MATCH), confidence and pitchSpread only by print rounding.
"""
from __future__ import annotations

import argparse
import base64
import concurrent.futures
import ctypes
import datetime as dt
import hashlib
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))
import novox  # noqa: E402

DEFAULT_LAB = ROOT / "out" / "import-lab"
DEFAULT_PLUGIN_TESTS = ROOT / "build-plugin" / "plugin" / "NovoxPluginTests_artefacts" / "Release" / "NovoxPluginTests"
DEFAULT_SPLITS = ROOT / "reference" / "corpus" / "splits.json"
DEFAULT_OPTIONS = {"algorithm": "singing-v1"}
DEFAULT_WORDS = "words.json"
NO_WORDS = "(no words)"     # --no-words: the engine gets no words (melody only); never a file name
DEFAULT_JOBS = min(8, os.cpu_count() or 1)
RATE = 16000
# Vowel phonemes of the engine (engine/src/phonemes.cpp); one vowel = one syllable, as alignLyrics counts.
VOWELS = {"IY", "IH", "EH", "AE", "AA", "AO", "UH", "UW", "AH", "AX", "ER", "EY", "AY", "AW", "OW", "OY"}
# Files of one run (runs/TAG/); a new run with the same tag removes them first.
RUN_FILES = ("score.json", "frames.tsv", "notes.tsv", "metrics.json", "plot.png", "run.json", "error.json")
SPLITS = ("dev", "test")

SHORT_NOTE = 0.120          # s
SPLIT_PITCH = 1.0           # st: adjacent same-word notes this close are likely one syllable
SPLIT_GAP = 0.050           # s
OCTAVE_JUMP = 9.0           # st
FRAME_WINDOW = 0.040        # s
GT_ONSET = 0.100            # s
GT_PITCH = 0.5              # st
GT_OVERLAP = 0.030          # s
PITCH_ERROR = 1.0           # st: best-overlap pitch this far off (and not an octave) is a pitch error
# Engine frames on the cached 16-bit audio vs the plugin's float-audio pitchCurve. int16 quantization
# nudges YIN's interpolated lag on quiet frames (typically < 0.003 st; one -57 dBFS frame moved 0.35 st)
# and can flip voicing at the silence gate (one -71 dBFS frame).
FRAME_MATCH = 0.01          # st; a frame agrees when voicing matches and midi is within this
FRAME_MATCH_SHARE = 0.99    # of the frames must agree
FRAME_MATCH_MAX = 0.5       # st, every frame voiced in both (far below OCTAVE_JUMP)
ASR_TIME_TOLERANCE = 0.5    # s: a recognized word's DTW end this far outside the sung word still matches (asr)
# Non-lexical sung syllables whisper tends to drop or merge (asr reports their recall separately).
VOCABLES = {"oh", "ohh", "ooh", "oo", "ah", "aah", "la", "na", "da", "doo", "ba", "hey", "yeah", "whoa",
            "woah", "mm", "mmm", "hmm", "uh", "ow", "ay", "ha"}


def rel(path: Path) -> str:
    """Repo-relative path for records, so cached metadata never pins a user's home folder."""
    path = Path(path).resolve()
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return path.name


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_atomic(path: Path, data: bytes) -> None:
    """Replace path in one step (temporary file in the same folder + os.replace): other lab runs read
    cached files concurrently and must never see a partial one."""
    path = Path(path)
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def write_json(path: Path, value) -> None:
    write_atomic(path, (json.dumps(value, indent=1) + "\n").encode())


def same_json(path: Path, value) -> bool:
    """True when path holds exactly this JSON value (so rewriting it can be skipped)."""
    try:
        return json.loads(Path(path).read_text()) == value
    except (OSError, ValueError):
        return False


def clean_word(text: str) -> str:
    """engine/src/transcribe.cpp cleanWord(): ASCII letters and apostrophes, no leading apostrophe."""
    return "".join(c for c in text if (c.isascii() and c.isalpha()) or c == "'").lstrip("'")


# ---------------------------------------------------------------------------
# Audio and cache

def read_wav16k(path: Path) -> np.ndarray:
    import soundfile as sf
    pcm, sr = sf.read(str(path), dtype="int16", always_2d=True)
    if sr != RATE or pcm.shape[1] != 1:
        raise SystemExit(f"{path}: expected mono {RATE} Hz, got {pcm.shape[1]} ch at {sr} Hz")
    return (pcm[:, 0].astype(np.float32) / 32768.0).astype(np.float32)  # JUCE int16 -> float scaling


def load_manifest(path: Path) -> list[dict]:
    data = json.loads(Path(path).read_text())
    cases = data.get("cases", data) if isinstance(data, dict) else data
    out = []
    for case in cases:
        case = dict(case)
        if "audio" in case:
            case["audioPath"] = (Path(path).parent / case["audio"]).resolve()
        out.append(case)
    return out


def find_ground_truth(audio: Path, case_id: str, manifest: Path | None, gt_file: Path | None):
    if gt_file:
        data = json.loads(Path(gt_file).read_text())
        notes = data["notes"] if isinstance(data, dict) else data
        return {"source": rel(gt_file), "notes": notes}
    candidates = ([manifest] if manifest else []) + [audio.parent / "manifest.json"]
    for m in candidates:
        if not m or not Path(m).exists():
            continue
        for case in load_manifest(m):
            if not case.get("notes"):
                continue
            same_file = case.get("audioPath") and Path(case["audioPath"]).resolve() == audio.resolve()
            if same_file or (manifest and m == manifest and case.get("id") == case_id):
                return {"source": f"{rel(m)}#{case.get('id', '')}", "comment": case.get("comment", ""),
                        "notes": [{"pitch": n["pitch"], "start": n["start"], "dur": n["dur"],
                                   **{k: n[k] for k in ("lyric", "word") if k in n}} for n in case["notes"]]}
    return None


def cache(args) -> None:
    audio = Path(args.audio).resolve()
    if not audio.exists():
        raise SystemExit(f"no such audio file: {args.audio}")
    binary = Path(args.plugin_tests)
    case_dir = Path(args.lab) / args.id
    case_dir.mkdir(parents=True, exist_ok=True)
    reuse = getattr(args, "reuse", False) and (case_dir / "plugin-result.json").exists()
    info = json.loads((case_dir / "cache.json").read_text()) if reuse and (case_dir / "cache.json").exists() else {}
    if not reuse:
        if not binary.exists():
            raise SystemExit(f"{binary} not found; build it: cmake --build build-plugin --target NovoxPluginTests -j 8")
        # The binary writes out/native-audio-review.json under its working directory; run it in the
        # case folder so concurrent caches never share (or clobber) the repo's out/ copy.
        produced = case_dir / "out" / "native-audio-review.json"
        produced.parent.mkdir(exist_ok=True)  # the binary does not create it
        if produced.exists():
            produced.unlink()
        print(f"[{args.id}] running plugin import on {rel(audio)} ...", flush=True)
        began = time.time()
        proc = subprocess.run([str(binary), "--audio", str(audio)], cwd=case_dir, capture_output=True, text=True)
        seconds = time.time() - began
        log = proc.stdout + ("\n--- stderr ---\n" + proc.stderr if proc.stderr else "")
        write_atomic(case_dir / "plugin-run.log", log.encode())
        if not produced.exists():
            raise SystemExit(f"[{args.id}] plugin import wrote no result (exit {proc.returncode}); see {case_dir / 'plugin-run.log'}")
        shutil.move(str(produced), case_dir / "plugin-result.json")
        try:
            produced.parent.rmdir()
        except OSError:
            pass
        stale = case_dir / "runs" / "plugin" / "frames.tsv"  # belongs to the previous result
        if stale.exists():
            stale.unlink()
        summary = next((line for line in reversed(proc.stdout.splitlines()) if line.endswith("failures")), "")
        info = {"pluginTests": rel(binary),
                "pluginTestsModified": dt.datetime.fromtimestamp(binary.stat().st_mtime).isoformat(timespec="seconds"),
                "exitCode": proc.returncode, "checks": summary, "seconds": round(seconds, 1),
                "cachedAt": dt.datetime.now().isoformat(timespec="seconds")}
    result = json.loads((case_dir / "plugin-result.json").read_text())
    if not isinstance(result, dict) or "original" not in result or "score" not in result:
        raise SystemExit(f"[{args.id}] plugin import failed; see {case_dir / 'plugin-run.log'}")
    header, _, payload = result["original"].partition(",")
    if not header.startswith("data:audio/wav;base64"):
        raise SystemExit(f"[{args.id}] unexpected original audio encoding: {header[:40]}")
    write_atomic(case_dir / "input16k.wav", base64.b64decode(payload))
    write_json(case_dir / "words.json", result.get("words", []))
    gt = find_ground_truth(audio, args.id, Path(args.manifest) if args.manifest else None,
                           Path(args.gt) if args.gt else None)
    if gt and not same_json(case_dir / "gt.json", gt):
        write_json(case_dir / "gt.json", gt)
    info.update({"id": args.id, "source": rel(audio), "sourceSha256": sha256(audio), "sourceBytes": audio.stat().st_size,
                 "duration": result.get("duration"), "words": len(result.get("words", [])),
                 "groundTruth": gt["source"] if gt else None})
    write_json(case_dir / "cache.json", info)
    print(f"[{args.id}] {'reused cached plugin result' if reuse else 'plugin import'} "
          f"{info.get('seconds', 0):.1f}s, exit {info.get('exitCode')} ({info.get('checks') or 'no check summary'}); "
          f"{len(result.get('words', []))} words, {len(result['score'].get('notes', []))} notes"
          + (f"; ground truth from {gt['source']}" if gt else ""))
    singer = novox.Singer(sample_rate=RATE, library=args.lib)
    run_case(case_dir, DEFAULT_OPTIONS, "latest", singer, args.lib, quiet=False)
    plugin_metrics = analyze_plugin(case_dir, singer)
    print(f"    plugin notes analyzed with {plugin_metrics['framesSource']} -> {rel(case_dir / 'runs' / 'plugin')}/")


# ---------------------------------------------------------------------------
# References and splits

def read_reference(path: Path) -> tuple[list[dict], int]:
    """Notes of a reference file ({"notes": [...]} or a list), in time order, and how many were skipped
    for lacking a numeric start, dur or pitch."""
    data = json.loads(Path(path).read_text())
    notes = data.get("notes") if isinstance(data, dict) else data
    if not isinstance(notes, list):
        raise ValueError("no note list")
    number = lambda v: isinstance(v, (int, float)) and not isinstance(v, bool)
    usable = [n for n in notes if isinstance(n, dict) and all(number(n.get(k)) for k in ("start", "dur", "pitch"))]
    return sorted(usable, key=lambda n: n["start"]), len(notes) - len(usable)


def load_references(case_dir: Path) -> dict:
    """{"groundTruth": gt.json, "silver": gt-verified.json or else gt-silver.json, "warnings"}; each
    present reference is {"source", "notes"} and the silver one also has "kind" ("verified"/"silver")."""
    refs = {"groundTruth": None, "silver": None, "warnings": []}
    for name, kind in (("gt.json", None), ("gt-verified.json", "verified"), ("gt-silver.json", "silver")):
        path = case_dir / name
        if not path.exists() or (kind and refs["silver"]):
            continue
        try:
            notes, skipped = read_reference(path)
        except (OSError, ValueError) as error:  # json.JSONDecodeError is a ValueError; may be mid-write
            refs["warnings"].append(f"{name} unreadable ({error}); ignored")
            continue
        if skipped:
            refs["warnings"].append(f"{name}: {skipped} notes without numeric start/dur/pitch ignored")
        if kind is None:
            refs["groundTruth"] = {"source": rel(path), "notes": notes}
        else:
            refs["silver"] = {"source": rel(path), "kind": kind, "notes": notes}
    return refs


def load_test_ids(path) -> set[str]:
    """Held-out ids from a splits file ({"test": [...]}); every other case is dev."""
    path = Path(path)
    if not path.exists():
        return set()
    return set(json.loads(path.read_text()).get("test", []))


def resolve_splits(args) -> Path:
    """--splits, else splits.json next to the corpus/agree MANIFEST when there is one, else
    reference/corpus/splits.json (warned about for another --lab: its held-out cases would count as dev)."""
    if args.splits:
        return Path(args.splits)
    manifest = getattr(args, "manifest", None) if args.command in ("corpus", "agree", "hyphens") else None
    if manifest and (Path(manifest).parent / "splits.json").exists():
        return Path(manifest).parent / "splits.json"
    if Path(args.lab).resolve() != DEFAULT_LAB.resolve() and args.command != "cache":
        print(f"warning: --lab {args.lab} without --splits uses {rel(DEFAULT_SPLITS)}; pass the lab's own split file "
              f"(e.g. --splits reference/datasets/vocadito/splits.json) or its held-out cases count as dev", file=sys.stderr)
    return DEFAULT_SPLITS


def select_cases(lab: Path, manifest: str | None, split: str, splits_file) -> tuple[list[dict], int]:
    """Cases of the manifest (or every cached case) in `split`, each with its "split"; and how many
    cases of the other split were left out."""
    if manifest:
        cases = load_manifest(Path(manifest))
    else:
        cases = [{"id": p.name} for p in sorted(lab.iterdir()) if (p / "plugin-result.json").exists()] if lab.exists() else []
    test = load_test_ids(splits_file)
    for case in cases:
        case["split"] = "test" if case["id"] in test else "dev"
    chosen = [c for c in cases if split == "all" or c["split"] == split]
    return chosen, len(cases) - len(chosen)


# ---------------------------------------------------------------------------
# Lyrics -> words

def syllable_counts(singer: novox.Singer, words: list[dict]) -> list[int]:
    """Dictionary syllables per word, exactly as alignLyrics counts them (vowels in the lexicon's pronunciation)."""
    counts = []
    for w in words:
        text = clean_word(str(w.get("word", w.get("text", ""))))
        if not text:
            counts.append(0)
            continue
        counts.append(max(1, sum(1 for symbol in singer.phonemize(text).split() if symbol in VOWELS)))
    return counts


def note_words(notes: list[dict], words: list[dict], unassigned: set[int]):
    """Word index of each note, recovered from the lyric text the engine wrote.

    alignLyrics gives each word a run of consecutive notes: its spelling parts ('hel-', 'lo')
    followed by '_' holds; a '_' always continues the note before it. Runs are matched in
    order to the words that were not reported unassigned, checking the spelling."""
    runs, owner, continues = [], [None] * len(notes), False
    for i, n in enumerate(notes):
        lyric = n.get("lyric", "") or ""
        if lyric == "_":
            if i and owner[i - 1] is not None:
                owner[i] = owner[i - 1]
            continues = False
            continue
        if not lyric:
            continues = False
            continue
        if continues and i and owner[i - 1] is not None:
            owner[i] = owner[i - 1]
            runs[-1]["text"] += lyric.rstrip("-")
        else:
            runs.append({"text": lyric.rstrip("-"), "first": i})
            owner[i] = len(runs) - 1
        continues = lyric.endswith("-")
    assigned = [k for k, w in enumerate(words) if k not in unassigned and clean_word(str(w.get("word", w.get("text", ""))))]
    run_word, mismatches, at = [], 0, 0
    for run in runs:
        match = None
        for k in range(at, min(len(assigned), at + 4)):  # tolerate a skipped word
            if clean_word(str(words[assigned[k]].get("word", ""))) == run["text"]:
                match = k
                break
        if match is None:
            mismatches += 1
            match = at if at < len(assigned) else None
        run_word.append(assigned[match] if match is not None else None)
        if match is not None:
            at = match + 1
    return [run_word[o] if o is not None else None for o in owner], mismatches


def norm_lyric(text) -> str:
    """Lyric comparison key: lowercase ASCII letters and apostrophes, outer apostrophes dropped
    ('Hel-' -> 'hel', "Don't" -> "don't"); '_' (melisma), '' (hum) and '-' all become ''."""
    text = str(text or "").lower().replace("’", "'")
    return "".join(c for c in text if "a" <= c <= "z" or c == "'").strip("'")


def reference_words(ref: list[dict]) -> list[str]:
    """Normalized word of each reference note: its "word" field, else rebuilt from the hyphenation
    (a syllable ending in '-' continues into the next one; '_' belongs to the word before it; a hum
    has no word)."""
    groups, group_of, continues = [], [], False
    for n in ref:
        raw = str(n.get("lyric", "") or "").strip()
        key = norm_lyric(raw)
        if not key:
            inherit = raw.startswith("_") and group_of and group_of[-1] is not None
            group_of.append(group_of[-1] if inherit else None)
            continues = continues if inherit else False
            continue
        if continues and group_of and group_of[-1] is not None:
            groups[group_of[-1]] += key
        else:
            groups.append(key)
        group_of.append(len(groups) - 1)
        continues = raw.endswith("-")
    return [norm_lyric(n["word"]) if n.get("word") else (groups[g] if g is not None else "")
            for n, g in zip(ref, group_of)]


# ---------------------------------------------------------------------------
# Metrics

def frame_octave_jumps(times: np.ndarray, midi: np.ndarray, contiguous: bool = True) -> list[dict]:
    """Events where voiced pitch moves >= OCTAVE_JUMP st within FRAME_WINDOW without a voicing gap."""
    flagged = []
    for i in range(len(times)):
        if midi[i] <= 0:
            continue
        for j in range(i + 1, len(times)):
            if times[j] - times[i] > FRAME_WINDOW + 1e-9 or (contiguous and midi[j] <= 0):
                break
            if midi[j] > 0 and abs(midi[j] - midi[i]) >= OCTAVE_JUMP:
                flagged.append((i, j))
                break
    events = []
    for i, j in flagged:
        size = float(midi[j] - midi[i])
        if events and times[i] - events[-1]["_last"] <= FRAME_WINDOW + 1e-9:
            events[-1]["_last"] = float(times[j])
            if abs(size) > abs(events[-1]["semitones"]):
                events[-1]["semitones"] = round(size, 2)
            continue
        step = max(range(i + 1, j + 1), key=lambda k: abs(midi[k] - midi[k - 1]))  # where the jump happens
        events.append({"time": round(float(times[step]), 3), "semitones": round(size, 2), "_last": float(times[j])})
    for e in events:
        e.pop("_last")
    return events


def overlap(a0, a1, b0, b1) -> float:
    return max(0.0, min(a1, b1) - max(a0, b0))


def is_octave(diff: float) -> bool:
    k = round(abs(diff) / 12)
    return k >= 1 and abs(abs(diff) - 12 * k) <= 1.0


def prf(hits: int, predicted: int, reference: int) -> dict:
    p = hits / predicted if predicted else 0.0
    r = hits / reference if reference else 0.0
    return {"matched": hits, "precision": round(p, 3), "recall": round(r, 3),
            "f": round(2 * p * r / (p + r), 3) if p + r else 0.0}


def is_held(lyric) -> bool:
    """A held note: '_' continues the previous note's syllable on a new pitch (see heldNotes)."""
    return str(lyric or "").strip().startswith("_")


def held_prf(found: int, correct: int, predicted: int, reference: int) -> dict:
    """Precision from the predicted side (correct of predicted), recall from the reference side (found of
    reference): each side counts its own notes once, by best overlap. None where a side has no held notes
    (f: when neither has any)."""
    p = correct / predicted if predicted else None
    r = found / reference if reference else None
    f = (2 * p * r / (p + r) if p and r else 0.0) if predicted or reference else None
    return {"precision": None if p is None else round(p, 3), "recall": None if r is None else round(r, 3),
            "f": None if f is None else round(f, 3)}


def held_note_metrics(notes: list[dict], gt: list[dict], ov: np.ndarray, best_of_gt: list, owners: list) -> tuple[dict, list]:
    """Held notes ('_') against a reference with lyrics (see heldNotes in the module docstring).
    Returns the counts and, per reference note, its category ('found', 'asNew', 'asHum', 'merged',
    'missed' for a reference '_'; 'missedOnset' for a new syllable whose best overlap is a predicted '_')."""
    kind = ["held" if is_held(g.get("lyric")) else "new" if norm_lyric(g.get("lyric")) else None for g in gt]
    counts = {"reference": kind.count("held"), "found": 0, "asNew": 0, "asHum": 0, "merged": 0, "missed": 0,
              "newSyllables": kind.count("new"), "missedOnset": 0, "predicted": 0, "correct": 0}
    category = [None] * len(gt)
    for i, k in enumerate(kind):
        best = best_of_gt[i]
        reached = best is not None and bool(ov[i, best] >= GT_OVERLAP - 1e-9)
        if k == "held":
            if not reached:
                c = "missed"
            elif is_held(notes[best].get("lyric")):
                c = "found"
            elif i > 0 and best == best_of_gt[i - 1]:
                c = "merged"         # the note before sings on through it: no note of its own
            elif not norm_lyric(notes[best].get("lyric")):
                c = "asHum"          # a lyric-less note where the singer held the syllable
            else:
                c = "asNew"          # a new syllable where the singer held one: the lyrics drift
            counts[c] += 1
            category[i] = c
        elif k == "new" and reached and is_held(notes[best].get("lyric")):
            counts["missedOnset"] += 1  # the syllable is sung where the result holds the previous one
            category[i] = "missedOnset"
    for j, n in enumerate(notes):
        if not is_held(n.get("lyric")):
            continue
        owner = owners[j]
        if owner is not None and bool(ov[owner, j] >= GT_OVERLAP - 1e-9) and kind[owner] is None:
            continue  # on an unlabelled reference note (no lyric): neither right nor wrong
        counts["predicted"] += 1
        counts["correct"] += int(owner is not None and bool(ov[owner, j] >= GT_OVERLAP - 1e-9) and kind[owner] == "held")
    counts.update(held_prf(counts["found"], counts["correct"], counts["predicted"], counts["reference"]))
    return counts, category


def ground_truth_metrics(notes: list[dict], gt: list[dict]) -> dict:
    """Detected notes vs one reference note list (see Metrics in the module docstring)."""
    from scipy.optimize import linear_sum_assignment

    def one_to_one(valid) -> list[tuple[int, int]]:
        """Most (reference, detected) pairs passing `valid`, then the least total onset error."""
        if not notes or not gt:
            return []
        big = 1e6
        cost = np.full((len(gt), len(notes)), big)
        for i, g in enumerate(gt):
            for j, n in enumerate(notes):
                if valid(g, n):
                    cost[i, j] = abs(g["start"] - n["start"])
        rows, cols = linear_sum_assignment(cost)
        return [(int(r), int(c)) for r, c in zip(rows, cols) if cost[r, c] < big]

    onset = lambda g, n: abs(g["start"] - n["start"]) <= GT_ONSET + 1e-9
    conp_pairs = one_to_one(lambda g, n: onset(g, n) and abs(g["pitch"] - n["pitch"]) <= GT_PITCH + 1e-9)
    onset_pairs = one_to_one(onset)
    conp_error = {i: round(abs(gt[i]["start"] - notes[j]["start"]) * 1000, 1) for i, j in conp_pairs}
    span = lambda n: (n["start"], n["start"] + n["dur"])
    ov = np.array([[overlap(*span(g), *span(n)) for n in notes] for g in gt], dtype=float).reshape(len(gt), len(notes))
    best_of_gt = [int(np.argmax(ov[i])) if ov[i].max(initial=0) > 0 else None for i in range(len(gt))]
    owners = [int(np.argmax(ov[:, j])) if ov[:, j].max(initial=0) > 0 else None for j in range(len(notes))]
    phantom = [j for j in range(len(notes)) if ov[:, j].max(initial=0) < GT_OVERLAP - 1e-9]
    missed = {i for i in range(len(gt)) if ov[i].max(initial=0) < GT_OVERLAP - 1e-9}
    has_lyrics = any("lyric" in g for g in gt)
    words = reference_words(gt) if has_lyrics else []
    held, held_category = held_note_metrics(notes, gt, ov, best_of_gt, owners) if has_lyrics else (None, [None] * len(gt))
    octave_errors = octave_fragments = pitch_right = pitch_errors = lyric_ok = word_ok = 0
    details = []
    for i, g in enumerate(gt):
        best = best_of_gt[i]
        d = {"start": g["start"], "dur": g["dur"], "pitch": g["pitch"]}
        if has_lyrics:
            d.update(lyric=g.get("lyric", ""), word=words[i])
        d.update(missed=i in missed, fragments=owners.count(i), conp=i in conp_error, onsetErrorMs=conp_error.get(i))
        if held_category[i]:
            d["held"] = held_category[i]
        if best is None:
            d["detected"] = None
            details.append(d)
            continue
        n = notes[best]
        diff = n["pitch"] - g["pitch"]
        octave, pitch_error = is_octave(diff), abs(diff) >= PITCH_ERROR - 1e-9 and not is_octave(diff)
        octave_errors += octave
        pitch_errors += pitch_error
        pitch_right += abs(diff) <= GT_PITCH + 1e-9
        octave_fragments += sum(1 for m, o in zip(notes, ov[i]) if o >= GT_OVERLAP and is_octave(m["pitch"] - g["pitch"]))
        d.update(detected=n["pitch"], detectedIndex=best, detectedStart=round(n["start"], 4),
                 overlap=round(float(ov[i, best]), 4), octave=octave, pitchError=pitch_error)
        if has_lyrics:
            said, wanted = norm_lyric(n.get("lyric", "")), norm_lyric(g.get("lyric", ""))
            same, in_word = said == wanted, bool(said) and said in words[i]
            lyric_ok += same
            word_ok += same or in_word
            d.update(detectedLyric=n.get("lyric", ""), lyricOK=same, wordOK=same or in_word)
        details.append(d)
    errors = sorted(conp_error.values())
    lyric_share = lambda count: round(count / len(gt), 3) if has_lyrics and gt else None
    return {"referenceNotes": len(gt), "gtNotes": len(gt), "notes": len(notes),
            "notesPerReferenceNote": round(len(notes) / len(gt), 3) if gt else None,
            "conp": prf(len(conp_pairs), len(notes), len(gt)), "onsetOnly": prf(len(onset_pairs), len(notes), len(gt)),
            "onsetMedianAbsMs": round(float(np.median(errors)), 1) if errors else None, "conpOnsetErrorsMs": errors,
            "octaveErrors": octave_errors, "pitchErrorsNonOctave": pitch_errors, "octaveFragments": octave_fragments,
            "pitchRightByOverlap": pitch_right, "fragmented": sum(1 for i in range(len(gt)) if owners.count(i) > 1),
            "merged": sum(1 for j in range(len(notes)) if best_of_gt.count(j) > 1),
            "phantom": len(phantom), "missed": len(missed),
            "lyricCorrect": lyric_ok if has_lyrics else None, "lyricWordCorrect": word_ok if has_lyrics else None,
            "lyricAccuracy": lyric_share(lyric_ok), "lyricWordAccuracy": lyric_share(word_ok),
            "heldNotes": held,
            "heldWhere": [{"start": round(g["start"], 3), "lyric": g.get("lyric", ""), "word": words[i],
                           "held": held_category[i],
                           "detectedLyric": notes[best_of_gt[i]].get("lyric", "") if best_of_gt[i] is not None else None}
                          for i, g in enumerate(gt) if held_category[i] not in (None, "found")],
            "perGtNote": details,
            "phantomNotes": [{"index": j, "start": notes[j]["start"], "dur": notes[j]["dur"], "pitch": notes[j]["pitch"],
                              "lyric": notes[j].get("lyric", "")} for j in phantom]}


def compute_metrics(score: dict, words: list[dict], syllables: list[int], frames, gt, silver=None) -> tuple[dict, list]:
    """Metrics of one result. gt: gt.json reference ({"notes"}) or None; silver: the silver/verified
    reference ({"notes", "kind", "source"}, see load_references) or None."""
    notes = score.get("notes", [])
    transcription = score.get("transcription", {}) or {}
    unassigned = {u["wordIndex"] for u in transcription.get("unassignedWords", [])}
    owner, mismatches = note_words(notes, words, unassigned)
    m = {"notes": len(notes), "words": len(words), "syllables": int(sum(syllables))}
    m["notesPerSyllable"] = round(len(notes) / m["syllables"], 3) if m["syllables"] else None
    m["shortNotes"] = sum(1 for n in notes if n["dur"] < SHORT_NOTE)
    splits, octave_in_word, excess_splits, split_times, octave_times = 0, 0, 0, [], []
    per_word: dict[int, list[int]] = {}
    for i, w in enumerate(owner):
        if w is not None:
            per_word.setdefault(w, []).append(i)
    for w, members in per_word.items():
        word_splits = 0
        for a, b in zip(members, members[1:]):
            if b != a + 1:
                continue
            n, o = notes[a], notes[b]
            gap = o["start"] - (n["start"] + n["dur"])
            diff = abs(o["pitch"] - n["pitch"])
            if diff <= SPLIT_PITCH + 1e-9 and gap < SPLIT_GAP:
                word_splits += 1
                split_times.append(round(o["start"], 3))
            if diff >= OCTAVE_JUMP:
                octave_in_word += 1
                octave_times.append(round(o["start"], 3))
        splits += word_splits
        excess_splits += min(word_splits, max(0, len(members) - max(1, syllables[w] if w < len(syllables) else 1)))
    m["intraWordSplits"] = splits
    m["excessIntraWordSplits"] = excess_splits
    m["excessNotes"] = sum(max(0, len(members) - max(1, syllables[w])) for w, members in per_word.items() if w < len(syllables))
    m["octaveJumpsInWord"] = octave_in_word
    events = frame_octave_jumps(frames["time"], frames["midi"]) if frames is not None else []
    m["frameOctaveJumps"] = len(events)
    m["frameOctaveJumpsInNotes"] = sum(1 for e in events if any(n["start"] <= e["time"] < n["start"] + n["dur"] for n in notes))
    m["framesSource"] = frames["source"] if frames is not None else None
    m["melismaNotes"] = sum(1 for n in notes if n.get("lyric") == "_")
    m["lyriclessNotes"] = sum(1 for n in notes if not n.get("lyric"))
    m["unassignedWords"] = len(unassigned)
    m["wordsWithoutNotes"] = sum(1 for k in range(len(words)) if k not in per_word)
    m["wordMapMismatches"] = mismatches
    m["warnings"] = len(score.get("transcribeWarnings", []))
    m["where"] = {"intraWordSplits": split_times, "octaveJumpsInWord": octave_times, "frameOctaveJumps": events}
    if gt:
        m["groundTruth"] = dict(ground_truth_metrics(notes, gt["notes"]), source=gt.get("source"))
    if silver:
        m["silverSource"] = silver.get("kind", "silver")
        m["silver"] = dict(ground_truth_metrics(notes, silver["notes"]), source=silver.get("source"),
                           silverSource=m["silverSource"])
    return m, owner


def compare_with_plugin(plugin_score: dict, score: dict) -> dict:
    a, b = plugin_score.get("notes", []), score.get("notes", [])
    exact, worst, differing = 0, {"detectedPitch": 0.0, "confidence": 0.0, "pitchSpread": 0.0}, []
    for i, (p, q) in enumerate(zip(a, b)):
        same = (abs(p["pitch"] - q["pitch"]) < 1e-9 and abs(p["start"] - q["start"]) < 1e-9 and
                abs(p["dur"] - q["dur"]) < 1e-9 and p.get("lyric", "") == q.get("lyric", ""))
        for key in worst:
            if key in p and key in q:
                worst[key] = max(worst[key], abs(p[key] - q[key]))
        if same:
            exact += 1
        elif len(differing) < 8:
            differing.append({"index": i, "plugin": [p["start"], p["dur"], p["pitch"], p.get("lyric", "")],
                              "run": [q["start"], q["dur"], q["pitch"], q.get("lyric", "")]})
    words = lambda s: [(u["wordIndex"]) for u in (s.get("transcription", {}) or {}).get("unassignedWords", [])]
    identical = (len(a) == len(b) == exact and words(plugin_score) == words(score) and
                 plugin_score.get("transcribeWarnings", []) == score.get("transcribeWarnings", []) and
                 worst["detectedPitch"] <= FRAME_MATCH and worst["confidence"] <= 0.0101 and worst["pitchSpread"] <= 0.0101)
    return {"identical": identical, "pluginNotes": len(a), "runNotes": len(b), "sameNotes": exact,
            "maxDiff": {k: round(v, 6) for k, v in worst.items()}, "firstDifferences": differing,
            "sameWarnings": plugin_score.get("transcribeWarnings", []) == score.get("transcribeWarnings", []),
            "sameUnassigned": words(plugin_score) == words(score)}


# ---------------------------------------------------------------------------
# Aggregates

STRUCTURE_SUMS = ("notes", "syllables", "shortNotes", "intraWordSplits", "excessIntraWordSplits", "octaveJumpsInWord",
                  "frameOctaveJumps", "frameOctaveJumpsInNotes", "melismaNotes", "lyriclessNotes", "unassignedWords",
                  "warnings")
REFERENCE_SUMS = ("notes", "referenceNotes", "octaveErrors", "pitchErrorsNonOctave", "octaveFragments",
                  "pitchRightByOverlap", "fragmented", "merged", "phantom", "missed")
REFERENCE_KINDS = (("silver", "silver/verified"), ("groundTruth", "gt.json"))


def aggregate(metrics_list: list[dict]) -> dict:
    """Sums and micro averages over cases (see Aggregates in the module docstring)."""
    agg = {"cases": len(metrics_list)}
    for key in STRUCTURE_SUMS:
        agg[key] = sum(m.get(key, 0) for m in metrics_list)
    agg["notesPerSyllable"] = round(agg["notes"] / agg["syllables"], 3) if agg["syllables"] else None
    for kind, _ in REFERENCE_KINDS:
        agg[kind] = aggregate_reference([m[kind] for m in metrics_list if m.get(kind)])
        if kind == "silver" and agg[kind]:
            agg[kind]["verified"] = sum(1 for m in metrics_list if m.get("silver") and m.get("silverSource") == "verified")
    # The headline reference of each case: silver/verified when it has one, else gt.json (a dataset
    # with human notes only). Equal to the silver aggregate when every case has a silver reference.
    primary = [(m, headline_reference(m)) for m in metrics_list]
    agg["headline"] = aggregate_reference([m[kind] for m, kind in primary if kind])
    if agg["headline"]:
        agg["headline"]["verified"] = sum(1 for m, kind in primary if kind == "silver" and m.get("silverSource") == "verified")
        agg["headline"]["groundTruthCases"] = sum(1 for _, kind in primary if kind == "groundTruth")
    return agg


def headline_reference(m: dict) -> str | None:
    """The reference kind a case's headline uses: "silver" (silver/verified), else "groundTruth", else None."""
    return "silver" if m.get("silver") else "groundTruth" if m.get("groundTruth") else None


def aggregate_reference(refs: list[dict]) -> dict | None:
    """Sums and micro averages of per-case metrics against one kind of reference; None without cases."""
    if not refs:
        return None
    a = {"cases": len(refs)}
    for key in REFERENCE_SUMS:
        a[key] = sum(r.get(key, 0) for r in refs)
    a["notesPerReferenceNote"] = round(a["notes"] / a["referenceNotes"], 3) if a["referenceNotes"] else None
    for key in ("conp", "onsetOnly"):
        a[key] = prf(sum(r[key]["matched"] for r in refs), a["notes"], a["referenceNotes"])
    errors = [e for r in refs for e in r.get("conpOnsetErrorsMs", [])]
    a["onsetMedianAbsMs"] = round(float(np.median(errors)), 1) if errors else None
    with_lyrics = [r for r in refs if r.get("lyricAccuracy") is not None]
    total = sum(r["referenceNotes"] for r in with_lyrics)
    a["lyricCases"], a["lyricReferenceNotes"] = len(with_lyrics), total
    a["lyricAccuracy"] = round(sum(r["lyricCorrect"] for r in with_lyrics) / total, 3) if total else None
    a["lyricWordAccuracy"] = round(sum(r["lyricWordCorrect"] for r in with_lyrics) / total, 3) if total else None
    a["heldNotes"] = aggregate_held([r["heldNotes"] for r in refs if r.get("heldNotes")])
    return a


HELD_SUMS = ("reference", "found", "asNew", "asHum", "merged", "missed", "newSyllables", "missedOnset", "predicted",
             "correct")


def aggregate_held(helds: list[dict]) -> dict | None:
    """Summed held-note counts and micro-averaged P/R/F; None without a reference with lyrics."""
    if not helds:
        return None
    a = {"cases": len(helds), **{key: sum(h.get(key, 0) for h in helds) for key in HELD_SUMS}}
    a.update(held_prf(a["found"], a["correct"], a["predicted"], a["reference"]))
    return a


def held_text(agg: dict) -> str:
    """The HELD headline: held-note P/R/F and where reference holds went (headline reference)."""
    h = (headline_aggregate(agg) or {}).get("heldNotes") if agg else None
    if not h:
        return "no reference with lyrics"
    return (f"P={number(h['precision'], 3)} R={number(h['recall'], 3)} F={number(h['f'], 3)} ref={h['reference']} "
            f"pred={h['predicted']} "
            f"found={h['found']} asNew={h['asNew']} asHum={h['asHum']} merged={h['merged']} missed={h['missed']} "
            f"missedOnset={h['missedOnset']} lyricCases={h['cases']}")


# Headline metrics of one case or aggregate, against the silver/verified reference.
HEADLINE = (("conpF", "conp.f", 3), ("octErr", "octaveErrors", 0), ("frag", "fragmented", 0),
            ("merged", "merged", 0), ("phantom", "phantom", 0), ("missed", "missed", 0),
            ("lyricW", "lyricWordAccuracy", 3))


def pick(d, path: str):
    for key in path.split("."):
        if not isinstance(d, dict):
            return None
        d = d.get(key)
    return d


def headline(m: dict) -> dict:
    """Headline numbers of a case's metrics (silver/verified reference, else gt.json) plus structure."""
    kind = headline_reference(m)
    r = m.get(kind) or {} if kind else {}
    out = {name: pick(r, path) for name, path, _ in HEADLINE}
    out.update(notes=m.get("notes"), referenceNotes=r.get("referenceNotes"), onsetF=pick(r, "onsetOnly.f"),
               pitchErr=r.get("pitchErrorsNonOctave"), lyric=r.get("lyricAccuracy"),
               onsetMedianAbsMs=r.get("onsetMedianAbsMs"), intraWordSplits=m.get("intraWordSplits"),
               octaveJumpsInWord=m.get("octaveJumpsInWord"), frameOctaveJumps=m.get("frameOctaveJumps"),
               silverSource=m.get("silverSource"), reference=kind, heldF=pick(r, "heldNotes.f"))
    return out


def headline_aggregate(agg: dict) -> dict | None:
    """The headline reference aggregate (older aggregates without one: silver)."""
    return agg.get("headline", agg.get("silver")) if agg else None


def number(value, digits: int) -> str:
    if value is None:
        return "-"
    return f"{value:.{digits}f}" if digits else str(int(round(value)))


def headline_text(agg: dict) -> str:
    if not agg or not agg.get("cases"):
        return "no successful case"
    if not headline_aggregate(agg):
        return "no reference"
    return " ".join(f"{name}={number(pick(headline_aggregate(agg), path), digits)}" for name, path, digits in HEADLINE)


def headline_suffix(agg: dict) -> str:
    """' gt=N' when N cases of the headline were scored against gt.json (no silver/verified reference)."""
    count = (headline_aggregate(agg) or {}).get("groundTruthCases", 0)
    return f" gt={count}" if count else ""


def headline_delta(a: dict, b: dict) -> str:
    parts = []
    for name, path, digits in HEADLINE:
        x, y = pick(headline_aggregate(a), path), pick(headline_aggregate(b), path)
        parts.append(f"{name}=" + ("-" if x is None or y is None else
                                   (f"{y - x:+.{digits}f}" if digits else f"{int(round(y - x)):+d}")))
    return " ".join(parts)


# ---------------------------------------------------------------------------
# Output files

def write_notes_tsv(path: Path, notes: list[dict], owner: list, words: list[dict]) -> None:
    lines = ["index\tstart\tdur\tend\tpitch\tdetectedPitch\tpitchSpread\tconfidence\tlyric\twordIndex\tword"]
    for i, (n, w) in enumerate(zip(notes, owner)):
        lines.append("\t".join([
            str(i), f"{n['start']:.4f}", f"{n['dur']:.4f}", f"{n['start'] + n['dur']:.4f}", f"{n['pitch']:g}",
            f"{n['detectedPitch']:.3f}" if "detectedPitch" in n else "", f"{n.get('pitchSpread', 0):.2f}",
            f"{n.get('confidence', 0):.2f}", n.get("lyric", ""), "" if w is None else str(w),
            "" if w is None else str(words[w].get("word", "")).strip()]))
    write_atomic(path, ("\n".join(lines) + "\n").encode())


def write_frames_tsv(path: Path, frames: list) -> None:
    lines = ["time\tmidi\tdb\tclarity\tconfidence"]
    lines += ["\t".join(f"{v:g}" for v in f) for f in frames]
    write_atomic(path, ("\n".join(lines) + "\n").encode())


def read_frames_tsv(path: Path, source: str) -> dict:
    table = np.loadtxt(path, skiprows=1, ndmin=2).reshape(-1, 5) if path.stat().st_size > 40 else np.zeros((0, 5))
    return {"time": table[:, 0], "midi": table[:, 1], "db": table[:, 2], "clarity": table[:, 3],
            "confidence": table[:, 4], "source": source}


def level_curve(audio: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """RMS level in dBFS over 32 ms windows every 8 ms (the analysis hop)."""
    hop, window = 128, 512
    padded = np.pad(audio.astype(np.float64), (window // 2, window // 2))
    count = 1 + (len(audio) - 1) // hop
    power = np.array([np.mean(padded[k * hop:k * hop + window] ** 2) for k in range(count)])
    return np.arange(count) * hop / RATE, 10 * np.log10(power + 1e-12)


NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
INK, INK_2, INK_3, GRID, SURFACE = "#0b0b0b", "#52514e", "#8a8984", "#e4e3df", "#fcfcfb"
BLUE, ORANGE, AQUA, YELLOW, RED, VIOLET = "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e34948", "#7a52c7"


def plot(path: Path, title: str, duration: float, notes, owner, words, frames, level, gt, reference, where,
         silver=None, silver_label: str = "silver") -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    from matplotlib.ticker import MultipleLocator

    margin_left, margin_right = 0.9, 0.3  # inches; the time axis itself is 1.5 inches per second
    width = min(320.0, max(10.0, 1.5 * duration + margin_left + margin_right))
    fig, (ax, lane, lev) = plt.subplots(3, 1, figsize=(width, 8.6), sharex=True, facecolor=SURFACE,
                                        gridspec_kw={"height_ratios": [6, 1.0, 1.3], "hspace": 0.06})
    fig.subplots_adjust(left=margin_left / width, right=1 - margin_right / width, top=0.86, bottom=0.07)
    for a in (ax, lane, lev):
        a.set_facecolor(SURFACE)
        for side in ("top", "right"):
            a.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            a.spines[side].set_color(INK_3)
        a.tick_params(colors=INK_2, labelsize=8)
        a.xaxis.set_major_locator(MultipleLocator(0.5))
        a.xaxis.set_minor_locator(MultipleLocator(0.1))
        a.grid(axis="x", which="major", color=GRID, linewidth=0.8)
        a.grid(axis="x", which="minor", color=GRID, linewidth=0.3)
    pitches = [n["pitch"] for n in notes] + [g["pitch"] for g in (gt or [])] + [g["pitch"] for g in (silver or [])]
    if frames is not None:
        voiced = frames["midi"] > 0
        if voiced.any():
            pitches += list(np.percentile(frames["midi"][voiced], [2, 98]))
    lo, hi = (min(pitches) - 3, max(pitches) + 4) if pitches else (48, 72)
    for p in range(int(np.floor(lo)), int(np.ceil(hi)) + 1):
        ax.axhline(p, color=GRID, linewidth=0.9 if p % 12 == 0 else 0.35, zorder=0)
    ax.set_yticks(range(int(np.ceil(lo)), int(np.floor(hi)) + 1))
    ax.set_yticklabels([f"{NAMES[p % 12]}{p // 12 - 1}" for p in range(int(np.ceil(lo)), int(np.floor(hi)) + 1)], fontsize=7)
    ax.set_ylim(lo, hi)
    if frames is not None:
        t, m, c = frames["time"], frames["midi"], frames["confidence"]
        v = m > 0
        strong = v & (c >= 0.6)
        ax.scatter(t[v & ~strong], m[v & ~strong], s=3, color=INK_3, alpha=0.45, linewidths=0, zorder=2,
                   label="frames (confidence < 0.6)")
        ax.scatter(t[strong], m[strong], s=4, color=INK_2, linewidths=0, zorder=2, label="frames")
    if reference:
        for n in reference:
            ax.add_patch(Rectangle((n["start"], n["pitch"] - 0.45), n["dur"], 0.9, fill=False, edgecolor=AQUA,
                                   linewidth=1.2, linestyle=(0, (2, 1.5)), zorder=3))
        ax.plot([], [], color=AQUA, linestyle="--", label="plugin notes (differ from this run)")
    for g in gt or []:
        ax.add_patch(Rectangle((g["start"], g["pitch"] - 0.5), g["dur"], 1.0, fill=False, edgecolor=ORANGE,
                               linewidth=1.6, zorder=3))
    if gt:
        ax.plot([], [], color=ORANGE, linewidth=1.6, label="ground truth (gt.json)")
    for g in silver or []:
        ax.add_patch(Rectangle((g["start"], g["pitch"] - 0.42), g["dur"], 0.84, fill=False, edgecolor=VIOLET,
                               linewidth=1.3, linestyle=(0, (3, 1.5)), zorder=3))
        if g.get("lyric"):
            ax.text(g["start"], g["pitch"] - 0.55, str(g["lyric"]), fontsize=7, color=VIOLET, zorder=5,
                    va="top", ha="left")
    if silver:
        ax.plot([], [], color=VIOLET, linestyle="--", linewidth=1.3, label=f"{silver_label} reference (lyric below)")
    for i, n in enumerate(notes):
        ax.add_patch(Rectangle((n["start"], n["pitch"] - 0.38), n["dur"], 0.76, facecolor=BLUE, alpha=0.35,
                               edgecolor=BLUE, linewidth=1.0, zorder=4))
        lift = 0.55 + (0.75 if i % 2 else 0)
        ax.text(n["start"], n["pitch"] + lift, n.get("lyric", "") or "·", fontsize=8, color=INK, zorder=5,
                va="bottom", ha="left")
    ax.plot([], [], color=BLUE, linewidth=6, alpha=0.5, label="notes (lyric above)")
    for t0 in where.get("intraWordSplits", []):
        ax.axvline(t0, color=YELLOW, linewidth=1.4, zorder=1)
    for t0 in where.get("octaveJumpsInWord", []):
        ax.axvline(t0, color=RED, linewidth=2.0, zorder=1)
    for e in where.get("frameOctaveJumps", []):
        ax.plot([e["time"]], [hi - 0.6], marker="v", color=RED, markersize=7, zorder=6)
    ax.plot([], [], color=YELLOW, linewidth=1.4, label="intra-word split (<=1 st, gap < 50 ms)")
    ax.plot([], [], color=RED, linewidth=2.0, label="octave jump within word")
    ax.plot([], [], color=RED, marker="v", linestyle="none", label="frame octave jump")
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), fontsize=8, frameon=False, ncol=9, labelcolor=INK_2)
    ax.set_ylabel("pitch", color=INK_2, fontsize=9)
    ax.set_title(title, loc="left", fontsize=10, color=INK, pad=24)
    for k, w in enumerate(words):
        row = k % 2
        lane.add_patch(Rectangle((w["start"], row + 0.12), max(0.005, w["end"] - w["start"]), 0.76,
                                 facecolor="#d5d4cf" if row else "#e8e7e3", edgecolor=INK_3, linewidth=0.6))
        note_count = sum(1 for o in owner if o == k)
        lane.text(w["start"] + 0.01, row + 0.5, f"{str(w.get('word', '')).strip()} ({note_count})", fontsize=8,
                  color=INK, va="center", ha="left", clip_on=True)
    lane.set_ylim(0, 2)
    lane.set_yticks([])
    lane.set_ylabel("words\n(notes)", color=INK_2, fontsize=8)
    times, db = level
    lev.plot(times, db, color=INK_2, linewidth=0.8)
    lev.axhline(np.max(db) - 45, color=INK_3, linewidth=0.6, linestyle=":")
    lev.set_ylim(max(-90, np.max(db) - 70), np.max(db) + 3)
    lev.set_ylabel("level dB", color=INK_2, fontsize=8)
    lev.set_xlabel("seconds", color=INK_2, fontsize=9)
    lev.set_xlim(0, duration)
    fig.savefig(path, dpi=100, bbox_inches="tight", facecolor=SURFACE)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Runs

def load_case(case_dir: Path, words_file: str = DEFAULT_WORDS):
    """(plugin-result.json, words (words.json or words_file in the case folder), references) of a cached case."""
    if not (case_dir / "plugin-result.json").exists():
        raise SystemExit(f"{case_dir.name} is not cached; run: import_lab.py cache <audio> --id {case_dir.name}")
    plugin = json.loads((case_dir / "plugin-result.json").read_text())
    return plugin, read_words(case_dir, words_file), load_references(case_dir)


def read_words(case_dir: Path, words_file: str) -> list[dict]:
    """The words of a case: words_file in its folder, or none for NO_WORDS (--no-words, melody only)."""
    if words_file == NO_WORDS:
        return []
    if not (case_dir / words_file).exists():
        raise SystemExit(f"{case_dir.name} has no {words_file}")
    return json.loads((case_dir / words_file).read_text())


def reference_lines(metrics: dict) -> list[str]:
    """One summary line per reference the metrics were scored against (silver/verified first)."""
    parts = []
    for kind, label in (("silver", metrics.get("silverSource", "silver")), ("groundTruth", "gt.json")):
        r = metrics.get(kind)
        if not r:
            continue
        lyric = f", lyric {r['lyricAccuracy']:.2f}/word {r['lyricWordAccuracy']:.2f}" if r["lyricAccuracy"] is not None else ""
        parts.append(f"{label} ({r['referenceNotes']} notes): COnP P/R/F {r['conp']['precision']:.2f}/{r['conp']['recall']:.2f}/"
                     f"{r['conp']['f']:.2f}, octave errors {r['octaveErrors']}, pitch errors {r['pitchErrorsNonOctave']}, "
                     f"fragmented {r['fragmented']}, merged {r['merged']}, phantom {r['phantom']}, missed {r['missed']}{lyric}")
    return parts


def finish(case_dir: Path, out: Path, tag: str, score: dict, frames, words, refs, singer, plugin, compare: bool,
           audio: np.ndarray, caption: str, make_plot: bool = True, extra: dict | None = None) -> dict:
    syllables = syllable_counts(singer, words)
    metrics, owner = compute_metrics(score, words, syllables, frames, refs["groundTruth"], refs["silver"])
    metrics["syllablesPerWord"] = syllables
    metrics.update(extra or {})
    if refs["warnings"]:
        metrics["referenceWarnings"] = refs["warnings"]
    if compare:
        metrics["plugin"] = compare_with_plugin(plugin["score"], score)
    write_json(out / "metrics.json", metrics)
    write_notes_tsv(out / "notes.tsv", score.get("notes", []), owner, words)
    if not make_plot:
        return metrics
    reference = None
    if compare and not metrics["plugin"]["identical"]:
        reference = plugin["score"].get("notes", [])
    title = (f"{case_dir.name} [{tag}] {caption}\n{metrics['notes']} notes / {metrics['syllables']} syllables"
             f" | short {metrics['shortNotes']} | intra-word splits {metrics['intraWordSplits']}"
             f" | octave in word {metrics['octaveJumpsInWord']} | frame octave jumps {metrics['frameOctaveJumps']}"
             f" | unassigned words {metrics['unassignedWords']}")
    title += "".join("\n" + line for line in reference_lines(metrics))
    silver = refs["silver"]
    plot(out / "plot.png", title, len(audio) / RATE, score.get("notes", []), owner, words, frames, level_curve(audio),
         refs["groundTruth"]["notes"] if refs["groundTruth"] else None, reference, metrics["where"],
         silver=silver["notes"] if silver else None, silver_label=silver["kind"] if silver else "silver")
    return metrics


def plugin_frames(case_dir: Path, plugin: dict) -> dict:
    """The plugin's analysis frames. The result only carries every 3rd frame (pitchCurve), so use the
    engine's full-rate frames from the cached audio when they agree with it (same times; voicing
    and midi within FRAME_MATCH on FRAME_MATCH_SHARE of the frames; midi within FRAME_MATCH_MAX
    wherever both are voiced); they are then stored as runs/plugin/frames.tsv."""
    curve = np.array(plugin.get("pitchCurve", []), dtype=float).reshape(-1, 2)
    stored = case_dir / "runs" / "plugin" / "frames.tsv"
    for candidate in (stored, case_dir / "runs" / "latest" / "frames.tsv"):
        if not candidate.exists():
            continue
        table = np.loadtxt(candidate, skiprows=1, ndmin=2).reshape(-1, 5)
        sub = table[::3]
        if len(sub) != len(curve) or not np.allclose(sub[:, 0], curve[:, 0], atol=1e-4):
            continue
        same_voicing = (sub[:, 1] > 0) == (curve[:, 1] > 0)
        both = same_voicing & (curve[:, 1] > 0)
        diff = np.where(both, np.abs(sub[:, 1] - curve[:, 1]), 0.0)
        if diff.size and (diff.max() >= FRAME_MATCH_MAX or
                          np.mean(same_voicing & (diff < FRAME_MATCH)) < FRAME_MATCH_SHARE):
            continue
        if candidate != stored:
            stored.parent.mkdir(parents=True, exist_ok=True)
            write_atomic(stored, candidate.read_bytes())
        return {"time": table[:, 0], "midi": table[:, 1], "db": table[:, 2], "clarity": table[:, 3],
                "confidence": table[:, 4],
                "source": f"engine frames on the cache (verified against plugin pitchCurve: max diff "
                          f"{diff.max(initial=0):.4f} st, {int(np.sum(~same_voicing))} voicing flips)"}
    return {"time": curve[:, 0], "midi": curve[:, 1], "confidence": np.ones(len(curve)),
            "source": "plugin pitchCurve (every 3rd analysis frame)"}


def analyze_plugin(case_dir: Path, singer) -> dict:
    """Metrics and plot for the notes the plugin itself produced (tag 'plugin')."""
    plugin, words, refs = load_case(case_dir)
    out = case_dir / "runs" / "plugin"
    out.mkdir(parents=True, exist_ok=True)
    score = plugin["score"]
    write_json(out / "score.json", score)
    frames = plugin_frames(case_dir, plugin)
    audio = read_wav16k(case_dir / "input16k.wav")
    return finish(case_dir, out, "plugin", score, frames, words, refs, singer, plugin, False, audio,
                  "plugin import result")


def run_case(case_dir: Path, options: dict, tag: str, singer, lib, quiet: bool = False, make_plot: bool = True,
             words_file: str = DEFAULT_WORDS) -> dict:
    plugin, words, refs = load_case(case_dir, words_file)
    out = case_dir / "runs" / tag
    out.mkdir(parents=True, exist_ok=True)
    for name in RUN_FILES:  # a failed or plot-less run must not leave an earlier run's files behind
        (out / name).unlink(missing_ok=True)
    audio = read_wav16k(case_dir / "input16k.wav")
    library = Path(lib) if lib else novox._find_library()
    info = {"id": case_dir.name, "tag": tag, "options": options, "words": words_file, "library": rel(library),
            "libraryModified": dt.datetime.fromtimestamp(library.stat().st_mtime).isoformat(timespec="seconds"),
            "ranAt": dt.datetime.now().isoformat(timespec="seconds")}
    began = time.time()
    try:
        score, invalid_utf8 = transcribe(singer, audio, words, dict(options, debugFrames=True))
    except Exception as error:
        write_json(out / "error.json", dict(info, error=f"{type(error).__name__}: {error}"))
        raise
    seconds = time.time() - began
    raw_frames = score.pop("frames", [])
    write_json(out / "score.json", score)
    write_frames_tsv(out / "frames.tsv", raw_frames)
    table = np.array(raw_frames, dtype=float).reshape(-1, 5)
    frames = {"time": table[:, 0], "midi": table[:, 1], "db": table[:, 2], "clarity": table[:, 3],
              "confidence": table[:, 4], "source": "engine debugFrames"}
    extra = {"engineInvalidUtf8": invalid_utf8} if invalid_utf8 else {}
    write_json(out / "run.json", dict(info, seconds=round(seconds, 3), **extra))
    caption = json.dumps(options, separators=(",", ":")) + ("" if words_file == DEFAULT_WORDS else f" words {words_file}")
    metrics = finish(case_dir, out, tag, score, frames, words, refs, singer, plugin, True, audio, caption, make_plot,
                     extra)
    if invalid_utf8 and not quiet:
        print(f"[{case_dir.name}] warning: the engine's JSON had {invalid_utf8} invalid UTF-8 sequences (a lyric cut "
              f"inside a multi-byte character); replaced with U+FFFD")
    if not quiet:
        p = metrics["plugin"]
        repro = "identical to plugin-result.json" if p["identical"] else (
            f"DIFFERS from plugin-result.json: {p['sameNotes']}/{p['pluginNotes']} plugin notes reproduced, "
            f"{p['runNotes']} run notes, max diffs {p['maxDiff']}")
        print(f"[{case_dir.name}] run '{tag}' {seconds:.2f}s: {summary_line(metrics)}; {repro}")
        for line in reference_lines(metrics):
            print(f"    {line}")
        for warning in refs["warnings"]:
            print(f"    reference warning: {warning}")
        print(f"    {rel(out)}/  ({'plot.png, ' if make_plot else ''}notes.tsv, frames.tsv, metrics.json, score.json)")
    return metrics


def transcribe(singer, audio: np.ndarray, words: list[dict], options: dict) -> tuple[dict, int]:
    """singer.transcribe(), tolerating invalid UTF-8 in the engine's JSON: the engine can cut a lyric inside
    a multi-byte character of a non-English word ('tête' -> b't\\xc3t'), which the binding's strict decode
    rejects. Such bytes become U+FFFD here; returns (score, number of replaced sequences)."""
    replaced = [0]
    strict = singer._take_string

    def tolerant(ptr) -> str:
        if not ptr:
            return strict(ptr)  # raises the engine's error
        try:
            data = ctypes.string_at(ptr)
        finally:
            singer._lib.novox_free(ptr)
        try:
            return data.decode()
        except UnicodeDecodeError:
            text = data.decode("utf-8", "replace")
            replaced[0] = text.count("\ufffd") - data.decode("utf-8", "ignore").count("\ufffd")
            return text

    singer._take_string = tolerant  # instance attribute shadows the method for this call only
    try:
        return singer.transcribe(audio, RATE, words=words, **options), replaced[0]
    finally:
        del singer._take_string


def score_tag(case_dir: Path, tag: str, singer) -> dict | None:
    """Metrics of a stored run (runs/TAG/score.json and frames.tsv) against the current references,
    without running the engine or writing anything (except runs/plugin/frames.tsv, see plugin_frames).
    None when the tag has no score.json for this case."""
    out = case_dir / "runs" / tag
    if not (out / "score.json").exists():
        return None
    score = json.loads((out / "score.json").read_text())
    run_info = json.loads((out / "run.json").read_text()) if (out / "run.json").exists() else {}
    words = read_words(case_dir, run_info.get("words", DEFAULT_WORDS))  # the words the run used
    if (out / "frames.tsv").exists():
        frames = read_frames_tsv(out / "frames.tsv", f"runs/{tag}/frames.tsv")
    elif tag == "plugin":
        frames = plugin_frames(case_dir, json.loads((case_dir / "plugin-result.json").read_text()))
    else:
        frames = None
    refs = load_references(case_dir)
    metrics, _ = compute_metrics(score, words, syllable_counts(singer, words), frames, refs["groundTruth"], refs["silver"])
    if refs["warnings"]:
        metrics["referenceWarnings"] = refs["warnings"]
    return metrics


def summary_line(m: dict) -> str:
    text = (f"{m['notes']} notes/{m['syllables']} syl, short {m['shortNotes']}, intra-word splits {m['intraWordSplits']}"
            f" (excess {m['excessIntraWordSplits']}), octave-in-word {m['octaveJumpsInWord']},"
            f" frame octave jumps {m['frameOctaveJumps']}, melisma {m['melismaNotes']}, unassigned {m['unassignedWords']}")
    return text


def parse_options(text: str | None) -> dict:
    options = dict(DEFAULT_OPTIONS)
    if text:
        extra = json.loads(text)
        if not isinstance(extra, dict):
            raise SystemExit("--options must be a JSON object")
        options.update(extra)
    options.pop("debugFrames", None)
    return options


def run(args) -> None:
    singer = novox.Singer(sample_rate=RATE, library=args.lib)
    test = load_test_ids(args.splits)
    failures = 0
    for case_id in args.ids:
        if case_id in test:
            print(f"[{case_id}] note: held-out TEST case ({rel(Path(args.splits))}); measure only, do not tune on it")
        try:
            run_case(Path(args.lab) / case_id, parse_options(args.options), args.tag, singer, args.lib,
                     make_plot=args.plot is not False, words_file=args.words)
        except Exception as error:  # e.g. an unknown algorithm; the other ids still run
            failures += 1
            print(f"[{case_id}] FAILED: {type(error).__name__}: {error}")
    if failures:
        raise SystemExit(1)


# ---------------------------------------------------------------------------
# Speech-recognition experiments (asr)

def sung_words(ref: list[dict]) -> list[dict]:
    """Words sung in a reference, in time order: {"word" (compared form), "start", "end"} (see Word recall)."""
    words, continues = [], False
    for n, word in zip(ref, reference_words(ref)):
        raw = str(n.get("lyric", "") or "").strip()
        end = n["start"] + n["dur"]
        if raw.startswith("_"):  # melisma: the word before it goes on
            if words:
                words[-1]["end"] = max(words[-1]["end"], end)
            continue
        if not word:  # hum
            continues = False
            continue
        syllable = n.get("wordSyllable")
        later = syllable > 0 if isinstance(syllable, int) else continues
        if later and words and words[-1]["word"] == word:
            words[-1]["end"] = max(words[-1]["end"], end)
        else:
            words.append({"word": word, "start": n["start"], "end": end})
        continues = raw.endswith("-")
    return words


def recognized_words(words: list[dict]) -> list[dict]:
    """Recognized words in compared form with the time used for timed matching: the DTW end ("asrEnd" for a
    last word whose end was forced to the file end). A hyphenated vocable run ("la-la-la") counts as its parts."""
    out = []
    for w in words:
        text = str(w.get("word", w.get("text", "")))
        when = w.get("asrEnd", w.get("end", 0))
        parts = [norm_lyric(part) for part in text.split("-")]
        if len(parts) > 1 and all(part in VOCABLES for part in parts):
            out += [{"word": part, "time": when} for part in parts]
        elif norm_lyric(text):
            out.append({"word": norm_lyric(text), "time": when})
    return out


def common_words(hyp: list[dict], ref: list[dict], timed: bool) -> list[tuple[int, int]]:
    """Index pairs of a longest common subsequence of recognized and sung words (same text; when timed, the
    recognized time within ASR_TIME_TOLERANCE of the sung word)."""
    def same(i, j):
        return hyp[i]["word"] == ref[j]["word"] and (not timed or ref[j]["start"] - ASR_TIME_TOLERANCE <= hyp[i]["time"]
                                                     <= ref[j]["end"] + ASR_TIME_TOLERANCE)
    n, m = len(hyp), len(ref)
    table = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            table[i][j] = max(table[i + 1][j], table[i][j + 1], table[i + 1][j + 1] + 1 if same(i, j) else 0)
    pairs, i, j = [], 0, 0
    while i < n and j < m:
        if same(i, j) and table[i][j] == table[i + 1][j + 1] + 1:
            pairs.append((i, j))
            i, j = i + 1, j + 1
        elif table[i + 1][j] >= table[i][j + 1]:
            i += 1
        else:
            j += 1
    return pairs


def word_recall(words: list[dict], ref: list[dict]) -> dict:
    """Recognized words against the words sung in a reference (see Word recall in the module docstring)."""
    hyp, sung = recognized_words(words), sung_words(ref)
    text, timed = common_words(hyp, sung, False), common_words(hyp, sung, True)
    ratio = lambda a, b: round(a / b, 3) if b else None
    hit_ref, hit_hyp = {j for _, j in timed}, {i for i, _ in timed}
    vocables = [j for j, w in enumerate(sung) if w["word"] in VOCABLES]
    spans = [w.get("asrEnd", w.get("end", 0)) - w.get("start", 0) for w in words]
    return {"referenceWords": len(sung), "recognizedWords": len(hyp),
            "matched": len(text), "recall": ratio(len(text), len(sung)), "precision": ratio(len(text), len(hyp)),
            "timedMatched": len(timed), "timedRecall": ratio(len(timed), len(sung)),
            "timedPrecision": ratio(len(timed), len(hyp)),
            "vocables": len(vocables), "vocablesMatched": sum(1 for j in vocables if j in hit_ref),
            "recognizedVocables": sum(1 for h in hyp if h["word"] in VOCABLES),
            "longestWordSeconds": round(max(spans), 2) if spans else None,
            "missed": [f"{w['word']}@{w['start']:.2f}" for j, w in enumerate(sung) if j not in hit_ref],
            "unmatched": [f"{h['word']}@{h['time']:.2f}" for i, h in enumerate(hyp) if i not in hit_hyp]}


def recognize(binary: Path, audio: Path, variant: str, case_dir: Path) -> tuple[list, dict]:
    """The plugin import of audio with a whisper.cpp variant (NovoxPluginTests --whisper-variant), run in a
    temporary folder of the case; (words, run info)."""
    work = Path(tempfile.mkdtemp(dir=case_dir, prefix=".asr-"))
    produced = work / "out" / "native-audio-review.json"
    produced.parent.mkdir()
    began = time.time()
    try:
        proc = subprocess.run([str(binary), "--audio", str(audio), "--whisper-variant", variant], cwd=work,
                              capture_output=True, text=True)
        seconds = time.time() - began
        if not produced.exists():
            raise SystemExit(f"[{case_dir.name}] variant {variant}: no import result (exit {proc.returncode}): "
                             f"{proc.stderr.strip()[-300:]}")
        result = json.loads(produced.read_text())
    finally:
        produced.unlink(missing_ok=True)
        for folder in (work / "out", work):
            try:
                folder.rmdir()
            except OSError:
                pass
    decode = re.search(r"^whisper_full ([0-9.]+) s", proc.stdout, re.M)
    checks = next((line for line in reversed(proc.stdout.splitlines()) if line.endswith("failures")), "")
    return result.get("words", []), {"seconds": round(seconds, 2), "whisperSeconds": float(decode.group(1)) if decode else None,
                                     "exitCode": proc.returncode, "checks": checks,
                                     "notes": len(result.get("score", {}).get("notes", []))}


def asr(args) -> None:
    lab, binary = Path(args.lab), Path(args.plugin_tests)
    test = load_test_ids(args.splits)
    words_only = args.words_only
    ids = args.ids or [c["id"] for c in select_cases(lab, None, "all" if words_only else "dev", args.splits)[0]]
    names = {variant: variant.replace(",", "+") for variant in args.variant}
    summaries = {}
    for variant, name in names.items():
        path = lab / f"asr-{name}.json"
        summaries[variant] = json.loads(path.read_text()) if path.exists() else {"variant": variant, "cases": {}}
    header = ["id", "variant", "words", "ref", "recall", "prec", "tRecall", "tPrec", "vocables", "longest", "whisper s"]
    rows, totals = [], {variant: [] for variant in names}
    for case_id in ids:
        case_dir = lab / case_id
        if case_id in test and not words_only:
            print(f"[{case_id}] held-out TEST case: skipped (ASR experiments tune the import; --words-only "
                  f"only writes its words)")
            continue
        cache_info = json.loads((case_dir / "cache.json").read_text()) if (case_dir / "cache.json").exists() else {}
        refs = load_references(case_dir)
        if not cache_info.get("source"):
            print(f"[{case_id}] not cached; skipped")
            continue
        scored = not words_only and refs["silver"]
        if not words_only and not refs["silver"]:
            print(f"[{case_id}] no silver/verified reference: words written, not scored")
        for variant, name in names.items():
            target = case_dir / (DEFAULT_WORDS if variant == "cached" else f"words-{name}.json")
            info = summaries[variant]["cases"].get(case_id, {}).get("run", {})
            if variant == "cached" or (args.reuse and target.exists()):
                words = json.loads(target.read_text())
            else:
                if not binary.exists():
                    raise SystemExit(f"{binary} not found; build it: cmake --build build-plugin --target NovoxPluginTests -j 8")
                print(f"[{case_id}] whisper variant {variant} ...", flush=True)
                words, info = recognize(binary, ROOT / cache_info["source"], variant, case_dir)
                write_json(target, words)
                if words_only:
                    print(f"    {len(words)} words -> {rel(target)} ({info.get('whisperSeconds')} s whisper)")
            if not scored:
                continue
            metrics = word_recall(words, refs["silver"]["notes"])
            summaries[variant]["cases"][case_id] = {"words": rel(target), "run": info, "metrics": metrics}
            totals[variant].append(dict(metrics, whisperSeconds=info.get("whisperSeconds")))
            rows.append([case_id, variant, str(metrics["recognizedWords"]), str(metrics["referenceWords"]),
                         number(metrics["recall"], 3), number(metrics["precision"], 3), number(metrics["timedRecall"], 3),
                         number(metrics["timedPrecision"], 3), f"{metrics['vocablesMatched']}/{metrics['vocables']}",
                         number(metrics["longestWordSeconds"], 1), number(info.get("whisperSeconds"), 2)])
    if words_only:
        return  # nothing scored; the asr-VARIANT.json summaries stay as they were
    for variant, name in names.items():
        summaries[variant].update(binary=rel(binary), updated=dt.datetime.now().isoformat(timespec="seconds"))
        write_json(lab / f"asr-{name}.json", summaries[variant])
        cases = totals[variant]
        if not cases:
            continue
        add = lambda key: sum(c[key] for c in cases)
        ratio = lambda a, b: round(a / b, 3) if b else None
        seconds = [c["whisperSeconds"] for c in cases]
        rows.append([f"TOTAL ({len(cases)})", variant, str(add("recognizedWords")), str(add("referenceWords")),
                     number(ratio(add("matched"), add("referenceWords")), 3),
                     number(ratio(add("matched"), add("recognizedWords")), 3),
                     number(ratio(add("timedMatched"), add("referenceWords")), 3),
                     number(ratio(add("timedMatched"), add("recognizedWords")), 3),
                     f"{add('vocablesMatched')}/{add('vocables')}", "",
                     number(sum(x for x in seconds if x) if all(seconds) else None, 1)])
    print_table("recognized words vs words sung in the silver/verified reference (t = timed: DTW end within "
                f"{ASR_TIME_TOLERANCE} s of the sung word; vocables = timed matches)", header, rows)


# ---------------------------------------------------------------------------
# Parallel corpus and compare

_SINGER = None  # one engine per worker process


def _init_worker(lib) -> None:
    global _SINGER
    _SINGER = novox.Singer(sample_rate=RATE, library=lib)


def _corpus_job(job: dict) -> dict:
    """One corpus case: engine run + metrics (+ plot), and the baseline tag re-scored."""
    case_dir = Path(job["caseDir"])
    began = time.time()
    result = {"id": case_dir.name}
    try:
        result["metrics"] = run_case(case_dir, job["options"], job["tag"], _SINGER, job["lib"], quiet=True,
                                     make_plot=job["plot"], words_file=job["words"])
    except (Exception, SystemExit) as error:
        result["error"] = f"{type(error).__name__}: {error}"
    if job.get("baseline") and job["baseline"] != job["tag"]:
        try:
            result["base"] = score_tag(case_dir, job["baseline"], _SINGER)
        except (Exception, SystemExit) as error:
            result["baseError"] = f"{type(error).__name__}: {error}"
    result["seconds"] = round(time.time() - began, 2)
    return result


def _score_job(job: dict) -> dict:
    """Re-score the stored runs of one case for each tag (compare)."""
    case_dir = Path(job["caseDir"])
    result = {"id": case_dir.name, "metrics": {}, "errors": {}}
    for tag in job["tags"]:
        try:
            if (case_dir / "runs" / tag / "error.json").exists():
                result["errors"][tag] = json.loads((case_dir / "runs" / tag / "error.json").read_text()).get("error", "failed")
                continue
            result["metrics"][tag] = score_tag(case_dir, tag, _SINGER)
        except (Exception, SystemExit) as error:
            result["errors"][tag] = f"{type(error).__name__}: {error}"
    return result


def parallel(fn, jobs: list[dict], workers: int, lib, progress=None) -> dict[str, dict]:
    """fn(job) for every job, in `workers` spawned processes (in this process when 1); results by id."""
    results = {}
    if workers <= 1 or len(jobs) <= 1:
        _init_worker(lib)
        for job in jobs:
            result = fn(job)
            results[result["id"]] = result
            if progress:
                progress(result, len(results), len(jobs))
        return results
    context = multiprocessing.get_context("spawn")  # never fork a process that holds libnovox/Accelerate
    with concurrent.futures.ProcessPoolExecutor(max_workers=min(workers, len(jobs)), mp_context=context,
                                                initializer=_init_worker, initargs=(lib,)) as pool:
        for future in concurrent.futures.as_completed([pool.submit(fn, job) for job in jobs]):
            result = future.result()
            results[result["id"]] = result
            if progress:
                progress(result, len(results), len(jobs))
    return results


# ---------------------------------------------------------------------------
# Tables

STRUCTURE_COLUMNS = [("notes", "notes", 0), ("syl", "syllables", 0), ("n/syl", "notesPerSyllable", 2),
                     ("<120ms", "shortNotes", 0), ("split", "intraWordSplits", 0), ("xsplit", "excessIntraWordSplits", 0),
                     ("octW", "octaveJumpsInWord", 0), ("octF", "frameOctaveJumps", 0), ("_", "melismaNotes", 0),
                     ("unasg", "unassignedWords", 0)]
REFERENCE_COLUMNS = [("ref", "referenceNotes", 0), ("n/ref", "notesPerReferenceNote", 2), ("F", "conp.f", 2),
                     ("onF", "onsetOnly.f", 2), ("onMs", "onsetMedianAbsMs", 0), ("octE", "octaveErrors", 0),
                     ("pErr", "pitchErrorsNonOctave", 0), ("frag", "fragmented", 0), ("mrg", "merged", 0),
                     ("phan", "phantom", 0), ("miss", "missed", 0), ("lyr", "lyricAccuracy", 2),
                     ("lyrW", "lyricWordAccuracy", 2), ("hF", "heldNotes.f", 2)]
LEGEND = ("split = intra-word splits (<=1 st, gap<50 ms), xsplit = those beyond the word's syllables, octW = octave jumps"
          " within a word, octF = frame octave jumps, _ = melisma notes, unasg = unassigned words, =plugin = reproduces"
          " plugin-result.json; src = v(erified)/s(ilver); ref = reference notes, n/ref = notes per reference note,"
          " F = COnP F (100 ms, 0.5 st), onF = onset-only F, onMs = median COnP onset error, octE = octave errors,"
          " pErr = non-octave pitch errors (>= 1 st), frag = fragmented, mrg = merged, phan = phantom notes,"
          " miss = missed reference notes, lyr/lyrW = lyric / lyric-word accuracy, hF = held-note ('_') F."
          " TOTAL rows: sums, micro averages.")


def cell(value, base, digits: int = 2) -> str:
    if value is None:
        return "-"
    text = f"{value:.{digits}f}" if isinstance(value, float) else str(value)
    if base is not None and value != base:
        delta = value - base
        if isinstance(value, float) or isinstance(base, float):
            if round(delta, digits) != 0:
                text += f"({delta:+.{digits}f})"
        else:
            text += f"({delta:+d})"
    return text


def table_rows(columns, entries) -> list[list[str]]:
    """entries: (label, values dict or None, base dict or None, extra leading cells, extra trailing cells)."""
    rows = []
    for label, values, base, lead, trail in entries:
        if values is None:
            continue
        rows.append([label] + lead + [cell(pick(values, path), pick(base, path) if base else None, digits)
                                      for _, path, digits in columns] + trail)
    return rows


def print_table(title: str, header: list[str], rows: list[list[str]]) -> None:
    widths = [max(len(str(r[i])) for r in [header] + rows) for i in range(len(header))]
    print(f"\n{title}")
    for r in [header] + rows:
        print("  ".join(str(v).rjust(w) for v, w in zip(r, widths)))


def by_split(cases: list[dict]) -> dict[str, list[dict]]:
    return {s: [c for c in cases if c["split"] == s] for s in SPLITS if any(c["split"] == s for c in cases)}


GROUP_COLUMNS = [("ref", "referenceNotes", 0), ("n/ref", "notesPerReferenceNote", 2), ("F", "conp.f", 3),
                 ("onF", "onsetOnly.f", 2), ("onMs", "onsetMedianAbsMs", 0), ("octE", "octaveErrors", 0),
                 ("pErr", "pitchErrorsNonOctave", 0), ("frag", "fragmented", 0), ("mrg", "merged", 0),
                 ("phan", "phantom", 0), ("miss", "missed", 0), ("lyrW", "lyricWordAccuracy", 2)]
GROUP_SCORE_VALUES = 4      # a --by field with at most this many values also gets one SCORE line per value


def group_value(value) -> str:
    return "-" if value is None else ("yes" if value else "no") if isinstance(value, bool) else str(value)


def group_report(fields: list[str], groups: dict[str, list[dict]]) -> dict:
    """Per-value aggregates of the manifest fields `fields` (e.g. "singer") over each split's successful
    cases, printed as one table per field (headline reference: silver/verified, else gt.json; rows by
    mean manifest averagePitch when the cases have one). {field: {split: {value: aggregate}}}."""
    out = {}
    for field in fields:
        if not any(field in c for members in groups.values() for c in members):
            continue
        out[field], entries = {}, []
        pitched = any("averagePitch" in c for members in groups.values() for c in members)
        for split, members in groups.items():
            values: dict[str, list[dict]] = {}
            for c in members:
                values.setdefault(group_value(c.get(field)), []).append(c)
            mean_pitch = lambda cs: (sum(c["averagePitch"] for c in cs if "averagePitch" in c) /
                                     max(1, sum(1 for c in cs if "averagePitch" in c)))
            order = sorted(values, key=lambda v: (mean_pitch(values[v]), v) if pitched else (0, v))
            out[field][split] = {}
            for value in order:
                good = [c["metrics"] for c in values[value] if "metrics" in c]
                agg = aggregate(good)
                agg["casesTotal"] = len(values[value])
                out[field][split][value] = agg
                lead = [split, f"{len(good)}/{len(values[value])}"] + ([f"{mean_pitch(values[value]):.1f}"] if pitched else [])
                entries.append((value, headline_aggregate(agg) or {}, None, lead, []))
        header = [field, "split", "cases"] + (["avgPitch"] if pitched else []) + [name for name, _, _ in GROUP_COLUMNS]
        print_table(f"by {field} (headline reference: silver/verified, else gt.json"
                    + ("; avgPitch = mean manifest averagePitch, MIDI)" if pitched else ")"),
                    header, table_rows(GROUP_COLUMNS, entries))
    return out


def print_group_scores(prefix: str, group_aggregates: dict, suffix: bool = True) -> None:
    """One headline line per value of the --by fields that have few values (e.g. english=yes/no)."""
    for field, splits in group_aggregates.items():
        for split, values in splits.items():
            if len(values) > GROUP_SCORE_VALUES:
                continue
            for value, agg in values.items():
                print(f"{prefix} {split} {field}={value}: {headline_text(agg)} "
                      f"cases={agg['cases']}/{agg['casesTotal']}{headline_suffix(agg) if suffix else ''}")


def corpus(args) -> None:
    lab = Path(args.lab)
    cases, left_out = select_cases(lab, args.manifest, args.split, args.splits)
    options = parse_options(args.options)
    ready = []
    for case in cases:
        case_dir = lab / case["id"]
        if not (case_dir / "plugin-result.json").exists():
            if args.cache_missing and case.get("audioPath"):
                try:
                    cache(argparse.Namespace(audio=str(case["audioPath"]), id=case["id"], lab=args.lab, lib=args.lib,
                                             plugin_tests=args.plugin_tests, manifest=args.manifest, gt=None,
                                             reuse=False))
                except SystemExit as error:  # one failed import should not end the corpus run
                    print(f"[{case['id']}] caching failed: {error}; skipped")
                    continue
            else:
                print(f"[{case['id']}] not cached (use --cache-missing or `cache`); skipped")
                continue
        ready.append(case)
    note = f"; {left_out} cases of the other split left out" if left_out and args.split != "all" else ""
    words_note = "" if args.words == DEFAULT_WORDS else f", words {args.words}"
    print(f"corpus run '{args.tag}' on split {args.split}: {len(ready)} cases, {args.jobs} workers, "
          f"options {json.dumps(options, separators=(',', ':'))}{words_note}{note}", flush=True)
    jobs = [{"caseDir": str(lab / c["id"]), "options": options, "tag": args.tag, "lib": args.lib, "plot": bool(args.plot),
             "baseline": args.baseline, "words": args.words} for c in ready]

    def progress(result, done, total):
        status = f"FAILED: {result['error']}" if "error" in result else f"{result['metrics']['notes']} notes"
        print(f"  [{done}/{total}] {result['id']} {result['seconds']:.1f}s {status}", flush=True)

    began = time.time()
    results = parallel(_corpus_job, jobs, args.jobs, args.lib, progress)
    seconds = time.time() - began
    for case in ready:
        case.update(results[case["id"]])
    report(args, options, ready, seconds, lab)


def report(args, options: dict, cases: list[dict], seconds: float, lab: Path) -> None:
    ok = [c for c in cases if "metrics" in c]
    groups = by_split(cases)
    aggregates, base_aggregates = {}, {}
    for split, members in groups.items():
        good = [c for c in members if "metrics" in c]
        aggregates[split] = aggregate([c["metrics"] for c in good])
        aggregates[split]["casesTotal"] = len(members)
        has_base = good and all(c.get("base") for c in good)
        base_aggregates[split] = aggregate([c["base"] for c in good]) if has_base else None
    base_of = lambda c: c.get("base") if args.baseline != args.tag else None
    tables = {}
    # Structure
    entries = []
    for split, members in groups.items():
        for c in members:
            if "metrics" in c:
                p = c["metrics"]["plugin"]
                entries.append((c["id"], c["metrics"], base_of(c), [],
                                ["yes" if p["identical"] else f"no({p['sameNotes']}/{p['pluginNotes']})"]))
        entries.append((f"TOTAL {split}", aggregates[split], base_aggregates[split], [], [""]))
    header = ["id"] + [name for name, _, _ in STRUCTURE_COLUMNS] + ["=plugin"]
    tables["structure"] = {"header": header, "rows": table_rows(STRUCTURE_COLUMNS, entries)}
    # References
    for kind, label in REFERENCE_KINDS:
        entries = []
        for split, members in groups.items():
            for c in members:
                if "metrics" in c and c["metrics"].get(kind):
                    lead = [{"verified": "v", "silver": "s"}.get(c["metrics"].get("silverSource"), "?")] if kind == "silver" else []
                    entries.append((c["id"], c["metrics"][kind], (base_of(c) or {}).get(kind), lead, []))
            if aggregates[split].get(kind):
                lead = [f"{aggregates[split][kind]['verified']}v"] if kind == "silver" else []
                entries.append((f"TOTAL {split}", aggregates[split][kind], (base_aggregates[split] or {}).get(kind), lead, []))
        if entries:
            header = ["id"] + (["src"] if kind == "silver" else []) + [name for name, _, _ in REFERENCE_COLUMNS]
            tables[kind] = {"header": header, "rows": table_rows(REFERENCE_COLUMNS, entries)}
    words_note = "" if args.words == DEFAULT_WORDS else f", words {args.words}"
    print(f"\ncorpus run '{args.tag}' options {json.dumps(options, separators=(',', ':'))}{words_note}, split {args.split},"
          f" {seconds:.1f}s; change vs '{args.baseline}' (re-scored) in parentheses")
    titles = {"structure": "structure", "silver": "vs silver/verified reference (gt-verified.json, else gt-silver.json)",
              "groundTruth": "vs gt.json (human/MIDI intended melody)"}
    for name, t in tables.items():
        print_table(titles[name], t["header"], t["rows"])
    print("\n" + LEGEND)
    failed = [c for c in cases if "error" in c]
    if failed:
        print("\nFAILED (left out of every total):")
        for c in failed:
            print(f"  {c['id']}: {c['error']}")
    warnings = [(c["id"], w) for c in ok for w in c["metrics"].get("referenceWarnings", [])]
    for case_id, warning in warnings:
        print(f"  reference warning [{case_id}]: {warning}")
    print("\nAGGREGATES")
    for split, agg in aggregates.items():
        print(f"  {split}: {agg['cases']} of {agg['casesTotal']} cases ok; notes {agg['notes']}, syllables "
              f"{agg['syllables']}, intraWordSplits {agg['intraWordSplits']}, octaveJumpsInWord {agg['octaveJumpsInWord']}, "
              f"frameOctaveJumps {agg['frameOctaveJumps']}, shortNotes {agg['shortNotes']}, unassignedWords {agg['unassignedWords']}")
        for kind, label in REFERENCE_KINDS:
            a = agg.get(kind)
            if not a:
                continue
            source = f"{a['verified']} verified, {a['cases'] - a['verified']} silver" if kind == "silver" else f"{a['cases']} cases"
            print(f"    {label} ({source}): notes {a['notes']} / reference {a['referenceNotes']}, COnP P/R/F "
                  f"{a['conp']['precision']:.3f}/{a['conp']['recall']:.3f}/{a['conp']['f']:.3f}, onset-only F "
                  f"{a['onsetOnly']['f']:.3f}, onset median {number(a['onsetMedianAbsMs'], 1)} ms, octaveErrors "
                  f"{a['octaveErrors']}, pitchErrorsNonOctave {a['pitchErrorsNonOctave']}, fragmented {a['fragmented']}, "
                  f"merged {a['merged']}, phantom {a['phantom']}, missed {a['missed']}, lyric "
                  f"{number(a['lyricAccuracy'], 3)}, lyricWord {number(a['lyricWordAccuracy'], 3)}"
                  + (f", held P/R/F {number(a['heldNotes']['precision'], 3)}/{number(a['heldNotes']['recall'], 3)}/"
                     f"{number(a['heldNotes']['f'], 3)} (reference {a['heldNotes']['reference']})" if a.get("heldNotes") else ""))
    group_aggregates = group_report(args.by, groups)
    print()
    for split, agg in aggregates.items():
        verified = agg["silver"]["verified"] if agg.get("silver") else 0
        print(f"SCORE {split}: {headline_text(agg)} cases={agg['cases']}/{agg['casesTotal']} verified={verified}"
              f"{headline_suffix(agg)}")
        if base_aggregates.get(split) and args.baseline != args.tag:
            print(f"  vs '{args.baseline}' {split}: {headline_delta(base_aggregates[split], agg)}")
    for split, agg in aggregates.items():
        print(f"HELD {split}: {held_text(agg)}")
    print_group_scores("SCORE", group_aggregates)
    library = Path(args.lib) if args.lib else novox._find_library()
    write_json(lab / f"corpus-{args.tag}.json", {
        "tag": args.tag, "options": options, "words": args.words, "baseline": args.baseline, "split": args.split,
        "splits": rel(Path(args.splits)), "library": rel(library),
        "libraryModified": dt.datetime.fromtimestamp(library.stat().st_mtime).isoformat(timespec="seconds"),
        "ranAt": dt.datetime.now().isoformat(timespec="seconds"), "seconds": round(seconds, 1),
        "cases": [{"id": c["id"], "split": c["split"], "seconds": c.get("seconds"), "error": c.get("error"),
                   "headline": headline(c["metrics"]) if "metrics" in c else None,
                   "groundTruthF": pick(c.get("metrics"), "groundTruth.conp.f"),
                   **{field: c[field] for field in args.by if field in c}} for c in cases],
        "aggregates": aggregates, "baselineAggregates": base_aggregates, "tables": tables,
        **({"groups": group_aggregates} if group_aggregates else {})})
    if failed:
        raise SystemExit(1)  # after the report: the other cases' numbers stand


def compare(args) -> None:
    lab = Path(args.lab)
    cases, left_out = select_cases(lab, None, args.split, args.splits)
    a_tag, b_tag = args.tag_a, args.tag_b
    jobs = [{"caseDir": str(lab / c["id"]), "tags": [a_tag, b_tag]} for c in cases]
    results = parallel(_score_job, jobs, args.jobs, args.lib)
    for tag in (a_tag, b_tag):
        info = next((lab / c["id"] / "runs" / tag / name for c in cases for name in ("run.json", "error.json")
                     if (lab / c["id"] / "runs" / tag / name).exists()), None)
        if info:
            run_info = json.loads(info.read_text())
            words_note = "" if run_info.get("words", DEFAULT_WORDS) == DEFAULT_WORDS else f", words {run_info['words']}"
            print(f"'{tag}': options {json.dumps(run_info.get('options'), separators=(',', ':'))}{words_note}, library "
                  f"modified {run_info.get('libraryModified')}, ran {run_info.get('ranAt')}")
        else:
            print(f"'{tag}': no run.json (plugin result or older run)")
    groups, both, missing = by_split(cases), {}, []
    for c in cases:
        r = results[c["id"]]
        a, b = r["metrics"].get(a_tag), r["metrics"].get(b_tag)
        if a and b:
            for m in (a, b):
                m["headline"] = m.get(headline_reference(m)) if headline_reference(m) else None
            both[c["id"]] = (a, b)
        else:
            why = "; ".join(f"{t}: {r['errors'][t]}" if t in r["errors"] else f"{t}: no run"
                            for t in (a_tag, b_tag) if not r["metrics"].get(t))
            missing.append(f"{c['id']} ({why})")
    columns = [("notes", "notes", 0)] + [(name, "headline." + path, digits) for name, path, digits in REFERENCE_COLUMNS
                                         if name not in ("n/ref",)] + [("gtF", "groundTruth.conp.f", 2)]
    entries, summary = [], {}
    for split, members in groups.items():
        pairs = [both[c["id"]] for c in members if c["id"] in both]
        for c in members:
            if c["id"] in both:
                a, b = both[c["id"]]
                src = {"verified": "v", "silver": "s"}.get(b.get("silverSource"), "g" if b.get("groundTruth") else "-")
                entries.append((c["id"], b, a, [src], []))
        if pairs:
            agg_a, agg_b = aggregate([p[0] for p in pairs]), aggregate([p[1] for p in pairs])
            summary[split] = (agg_a, agg_b, len(pairs), len(members))
            entries.append((f"TOTAL {split}", agg_b, agg_a, [f"{agg_b['silver']['verified']}v" if agg_b.get('silver') else "-"], []))
    note = f"; {left_out} cases of the other split left out" if left_out and args.split != "all" else ""
    print_table(f"compare '{a_tag}' -> '{b_tag}' (split {args.split}{note}): values of '{b_tag}', change from "
                f"'{a_tag}' in parentheses; re-scored against the current references",
                ["id", "src"] + [name for name, _, _ in columns], table_rows(columns, entries))
    print("\n(columns against the headline reference: silver/verified, else gt.json (src g), as in corpus; "
          "gtF = COnP F vs gt.json)")
    if missing:
        print("not compared: " + ", ".join(missing))
    print()
    for split, (agg_a, agg_b, count, total) in summary.items():
        print(f"{a_tag} {split}: {headline_text(agg_a)} cases={count}/{total}{headline_suffix(agg_a)}")
        print(f"{b_tag} {split}: {headline_text(agg_b)} cases={count}/{total}{headline_suffix(agg_b)}")
        print(f"delta {split}: {headline_delta(agg_a, agg_b)}")
        print(f"HELD {a_tag} {split}: {held_text(agg_a)}")
        print(f"HELD {b_tag} {split}: {held_text(agg_b)}")


def form_letters(text) -> str:
    """Letters (any alphabet, lowercase) and inner apostrophes of a lyric or word, for lyric_form."""
    text = str(text or "").lower().replace("’", "'")
    return "".join(c for c in text if c.isalpha() or c == "'").strip("'")


def read_out(word: str) -> bool:
    """A transcript word sung as other words than its letters (engine syllabify): a number or symbol
    ("100", "49er", "&", "50%") or the letter W alone ("dou-ble-u")."""
    return any(c.isdigit() or c in "&%" for c in word) or form_letters(word) == "w"


def lyric_form(notes: list[dict], words: list[dict], unassigned: set[int]) -> list[dict]:
    """How a result writes its lyrics, checked against the transcript words: a digit in a lyric, a word
    whose syllables sit on several notes without hyphens ('missingHyphen': "re" "mem" "ber"), a hyphen
    that runs into the next word ('extraHyphen') or into a rest or the end ('danglingHyphen'). Notes
    form hyphen groups ('re-' 'mem-' 'ber'; '_' continues the note before it); groups are matched in
    order to the words that were not reported unassigned by their letters. A word read out (read_out)
    takes the groups up to the next word; a word that matches no group (no notes) is skipped when the
    group belongs to one of the next words, else the group is skipped."""
    problems, groups, open_group = [], [], False
    for n in notes:
        lyric = str(n.get("lyric", "") or "")
        if any(c.isdigit() for c in lyric):
            problems.append({"kind": "digit", "start": round(n["start"], 3), "lyric": lyric})
        if is_held(lyric):
            continue
        if not lyric.strip():
            if open_group:
                problems.append({"kind": "danglingHyphen", "start": groups[-1]["start"], "lyric": groups[-1]["text"]})
            open_group = False
            continue
        if open_group:
            groups[-1]["text"] += lyric
        else:
            groups.append({"start": round(n["start"], 3), "text": lyric})
        open_group = lyric.endswith("-")
    if open_group:
        problems.append({"kind": "danglingHyphen", "start": groups[-1]["start"], "lyric": groups[-1]["text"]})
    groups = [dict(g, letters=form_letters(g["text"].replace("-", ""))) for g in groups]
    groups = [g for g in groups if g["letters"]]
    sung = [str(w.get("word", "")) for k, w in enumerate(words)
            if k not in unassigned and (form_letters(w.get("word")) or read_out(str(w.get("word", ""))))]
    wi = gi = 0
    while wi < len(sung) and gi < len(groups):
        text = sung[wi]
        if read_out(text):
            following = next((form_letters(x) for x in sung[wi + 1:] if not read_out(x)), None)
            while gi < len(groups) and not (following and following.startswith(groups[gi]["letters"])):
                gi += 1
            wi += 1
            continue
        want, group = form_letters(text), groups[gi]
        got, used = group["letters"], 1
        while len(got) < len(want) and want.startswith(got) and gi + used < len(groups):
            got += groups[gi + used]["letters"]
            used += 1
        if got == want:
            if used > 1:
                problems.append({"kind": "missingHyphen", "start": group["start"], "word": text,
                                 "lyric": " ".join(g["text"] for g in groups[gi:gi + used])})
            gi, wi = gi + used, wi + 1
        elif group["letters"].startswith(want):
            problems.append({"kind": "extraHyphen", "start": group["start"], "word": text, "lyric": group["text"]})
            rest, wi, gi = group["letters"][len(want):], wi + 1, gi + 1
            while rest and wi < len(sung) and form_letters(sung[wi]) and rest.startswith(form_letters(sung[wi])):
                rest, wi = rest[len(form_letters(sung[wi])):], wi + 1
        elif any(form_letters(x).startswith(group["letters"]) for x in sung[wi + 1:wi + 5]):
            wi += 1  # this word has no notes
        else:
            gi += 1  # a group no nearby word spells
    return problems


def hyphens(args) -> None:
    """Lyric form of one tag's stored runs (see lyric_form): digits and hyphenation, per case and in total."""
    lab = Path(args.lab)
    cases, left_out = select_cases(lab, args.manifest, args.split, args.splits)
    totals, scanned, notes_total = {}, 0, 0
    for case in cases:
        run = lab / case["id"] / "runs" / args.tag / "score.json"
        if not run.exists():
            continue
        score = json.loads(run.read_text())
        transcription = score.get("transcription", {}) or {}
        words = transcription.get("transcriptWords", [])
        unassigned = {u["wordIndex"] for u in transcription.get("unassignedWords", [])}
        notes = score.get("notes", [])
        problems = lyric_form(notes, words, unassigned)
        scanned += 1
        notes_total += len(notes)
        for p in problems:
            totals[p["kind"]] = totals.get(p["kind"], 0) + 1
            if args.verbose or p["kind"] in ("digit", "missingHyphen"):
                print(f"  [{case['id']}] {p['kind']} at {p['start']}s: {p.get('word', '')!r} -> {p['lyric']!r}")
    note = f"; {left_out} cases of the other split left out" if left_out and args.split != "all" else ""
    summary = " ".join(f"{k}={totals.get(k, 0)}" for k in ("digit", "missingHyphen", "extraHyphen", "danglingHyphen"))
    print(f"HYPHENS {args.split} '{args.tag}': {summary} cases={scanned} notes={notes_total}{note}")


def agree(args) -> None:
    """Human agreement ceiling: each case's alternate reference (manifest "alternates", e.g. annotator A2)
    scored as if it were a detected result against the case's notes (e.g. annotator A1, what gt.json
    holds), with the reference metrics of corpus. No engine, no cache needed."""
    cases, left_out = select_cases(Path(args.lab), args.manifest, args.split, args.splits)
    keep = lambda n: {k: n[k] for k in ("start", "dur", "pitch", "lyric") if k in n}
    usable = lambda notes: sorted((keep(n) for n in notes if isinstance(n, dict) and all(
        isinstance(n.get(k), (int, float)) and not isinstance(n.get(k), bool) for k in ("start", "dur", "pitch"))),
        key=lambda n: n["start"])
    for case in cases:
        alternate = ((case.get("alternates") or {}).get(args.alternate) or {}).get("notes")
        if not case.get("notes") or not alternate:
            case["error"] = f"no notes or no alternates.{args.alternate}.notes"
            continue
        reference, other = usable(case["notes"]), usable(alternate)
        case["metrics"] = {"notes": len(other), "groundTruth": ground_truth_metrics(other, reference)}
    groups = by_split(cases)
    entries, aggregates = [], {}
    for split, members in groups.items():
        good = [c for c in members if "metrics" in c]
        for c in good:
            entries.append((c["id"], c["metrics"]["groundTruth"], None, [], []))
        aggregates[split] = aggregate([c["metrics"] for c in good])
        aggregates[split]["casesTotal"] = len(members)
        if aggregates[split].get("groundTruth"):
            entries.append((f"TOTAL {split}", aggregates[split]["groundTruth"], None, [], []))
    note = f"; {left_out} cases of the other split left out" if left_out and args.split != "all" else ""
    print_table(f"agreement: alternates.{args.alternate} scored against the notes of {rel(Path(args.manifest))} "
                f"(split {args.split}{note})", ["id"] + [name for name, _, _ in REFERENCE_COLUMNS],
                table_rows(REFERENCE_COLUMNS, entries))
    for c in cases:
        if "error" in c:
            print(f"  {c['id']}: {c['error']}; left out")
    group_aggregates = group_report(args.by, groups)
    print()
    for split, agg in aggregates.items():
        print(f"AGREE {split}: {headline_text(agg)} cases={agg['cases']}/{agg['casesTotal']}")
    print_group_scores("AGREE", group_aggregates, suffix=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--lab", default=str(DEFAULT_LAB), help="cache/output folder (default out/import-lab)")
    common.add_argument("--lib", default=None, help="libnovox to load (default build/libnovox.dylib or $NOVOX_LIB)")
    common.add_argument("--plugin-tests", default=os.environ.get("NOVOX_PLUGIN_TESTS", str(DEFAULT_PLUGIN_TESTS)),
                        help="NovoxPluginTests binary (default build-plugin/... or $NOVOX_PLUGIN_TESTS)")
    common.add_argument("--splits", default=None,
                        help="dev/test split file (default: splits.json next to a corpus/agree MANIFEST, else "
                             "reference/corpus/splits.json)")
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("cache", parents=[common], help="run the exact plugin import on AUDIO and cache it under --id")
    c.add_argument("audio")
    c.add_argument("--id", required=True)
    c.add_argument("--manifest", help="manifest with ground truth for this file (default: manifest.json next to it)")
    c.add_argument("--gt", help="ground-truth notes JSON (seconds)")
    c.add_argument("--reuse", action="store_true",
                   help="keep an existing plugin-result.json (no plugin run); re-extract and re-analyze it")
    c.set_defaults(func=cache)
    r = sub.add_parser("run", parents=[common], help="re-run the engine on cached ids")
    r.add_argument("ids", nargs="+")
    r.add_argument("--options", help="JSON merged onto {\"algorithm\": \"singing-v1\"}")
    r.add_argument("--tag", default="latest")
    r.add_argument("--words", default=DEFAULT_WORDS, help="words file in the case folder (default words.json)")
    r.add_argument("--no-words", dest="words", action="store_const", const=NO_WORDS,
                   help="give the engine no words (melody only)")
    r.add_argument("--plot", action=argparse.BooleanOptionalAction, default=None, help="write plot.png (default on)")
    r.set_defaults(func=run)
    k = sub.add_parser("corpus", parents=[common], help="run every cached case of a split in parallel and score it")
    k.add_argument("manifest", nargs="?")
    k.add_argument("--options", help="JSON merged onto {\"algorithm\": \"singing-v1\"}")
    k.add_argument("--tag", default="latest")
    k.add_argument("--words", default=DEFAULT_WORDS, help="words file in the case folder (default words.json)")
    k.add_argument("--no-words", dest="words", action="store_const", const=NO_WORDS,
                   help="give the engine no words (melody only)")
    k.add_argument("--by", nargs="+", default=["singer", "english"], metavar="FIELD",
                   help="manifest case fields to break the scores down by (default singer english; a field no "
                        "case has is skipped)")
    k.add_argument("--baseline", default="plugin", help="tag to compare against (default plugin)")
    k.add_argument("--split", choices=("dev", "test", "all"), default="dev",
                   help="cases to run (default dev; test is held out and runs only when asked for)")
    k.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help=f"worker processes (default {DEFAULT_JOBS})")
    k.add_argument("--plot", action=argparse.BooleanOptionalAction, default=False, help="write plot.png (default off)")
    k.add_argument("--cache-missing", action="store_true")
    k.set_defaults(func=corpus)
    m = sub.add_parser("compare", parents=[common], help="per-case and aggregate deltas between two tags")
    m.add_argument("tag_a", metavar="TAG_A")
    m.add_argument("tag_b", metavar="TAG_B")
    m.add_argument("--split", choices=("dev", "test", "all"), default="dev")
    m.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help=f"worker processes (default {DEFAULT_JOBS})")
    m.set_defaults(func=compare)
    a = sub.add_parser("asr", parents=[common], help="whisper.cpp variants on dev cases: words-VARIANT.json + word recall")
    a.add_argument("ids", nargs="*", help="dev case ids (default every cached dev case)")
    a.add_argument("--variant", nargs="+", default=["greedy"],
                   help="NovoxPluginTests --whisper-variant values (comma lists combine); 'cached' scores words.json")
    a.add_argument("--reuse", action="store_true", help="score an existing words-VARIANT.json instead of running")
    a.add_argument("--words-only", action="store_true",
                   help="only write words-VARIANT.json (no scoring, no table); allowed for held-out cases too, since "
                        "words are engine inputs, not results")
    a.set_defaults(func=asr)
    h = sub.add_parser("hyphens", parents=[common],
                       help="scan one tag's stored note lyrics for digits and words split over notes without hyphens")
    h.add_argument("tag", metavar="TAG")
    h.add_argument("manifest", nargs="?", help="manifest (for its splits.json; default every cached case)")
    h.add_argument("--split", choices=("dev", "test", "all"), default="dev")
    h.add_argument("--verbose", action="store_true", help="also list extra and dangling hyphens")
    h.set_defaults(func=hyphens)

    g = sub.add_parser("agree", parents=[common],
                       help="score a manifest's alternate reference (e.g. annotator A2) against its notes (A1)")
    g.add_argument("manifest", metavar="MANIFEST")
    g.add_argument("--alternate", default="A2", help="key of the cases' \"alternates\" to score (default A2)")
    g.add_argument("--split", choices=("dev", "test", "all"), default="dev")
    g.add_argument("--by", nargs="+", default=["singer", "english"], metavar="FIELD",
                   help="manifest case fields to break the scores down by (default singer english)")
    g.set_defaults(func=agree)
    args = parser.parse_args()
    args.splits = str(resolve_splits(args))
    args.func(args)


if __name__ == "__main__":
    main()
