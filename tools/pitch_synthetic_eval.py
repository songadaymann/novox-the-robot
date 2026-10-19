#!/usr/bin/env python3
"""Deterministic singing-v2 accuracy probe with known synthetic pitch ground truth.

Example:
  python tools/pitch_synthetic_eval.py --repo /path/to/novox \
    --baseline-lib /path/to/baseline/libnovox.dylib \
    --candidate-lib /path/to/candidate/libnovox.dylib \
    --out out/pitch-synthetic-results.json

Requires numpy and the existing Novox Python bindings. Each native library is
loaded in a separate subprocess. Audio is generated in memory; results use --out.
All frequencies, bounds and metrics here are independently defined test inputs.
Run from the repository root with its tools/.venv Python (NumPy required).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import numpy as np

RATE = 16000
HOP = 0.008
EDGE_GUARD = 0.064
GROSS_CENTS = 50.0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def cases():
    result = []
    # Fixed before evaluating the experiment: fractional periods, timbres, phases.
    for midi in (40, 48, 57, 64, 72, 80, 86, 92, 95):
        for offset in (-0.37, 0.19):
            for timbre in ("sine", "harmonic"):
                result.append(dict(id=f"steady-{midi}{offset:+.2f}-{timbre}", kind="steady",
                                   pitch=midi + offset, timbre=timbre, phase=0.17))
    for phase in (0.0, 0.31, 0.73):
        for midi in (57.41, 86.27, 94.43):
            result.append(dict(id=f"phase-{midi}-{phase}", kind="steady", pitch=midi,
                               timbre="harmonic", phase=phase))
    for midi in (48.31, 64.27, 80.19):
        for timbre in ("weak-fundamental", "strong-third"):
            result.append(dict(id=f"{timbre}-{midi}", kind="steady", pitch=midi, timbre=timbre))
        for snr in (30, 15, 5):
            result.append(dict(id=f"noise-{midi}-{snr}dB", kind="steady", pitch=midi,
                               timbre="harmonic", snrDb=snr))
    for midi in (48.31, 72.19, 86.27):
        result.append(dict(id=f"amplitude-modulation-{midi}", kind="steady", pitch=midi,
                           timbre="harmonic", amplitudeModulation=True))
    for center, depth, hz in ((57.19, 0.5, 5.5), (72.31, 1.0, 7.0), (86.27, 0.25, 4.0)):
        result.append(dict(id=f"vibrato-{center}-{depth}-{hz}", kind="vibrato", pitch=center,
                           depth=depth, modulationHz=hz, duration=2.2, end=2.05, timbre="harmonic"))
    for start, stop in ((48.19, 60.19), (72.31, 60.31), (80.19, 86.19)):
        result.append(dict(id=f"glide-{start}-{stop}", kind="glide", pitch=start, target=stop,
                           glideStart=0.60, glideEnd=0.90, duration=1.6, end=1.45, timbre="harmonic"))
    for start, stop in ((60.19, 61.19), (61.19, 60.19), (55.31, 67.31), (67.31, 55.31)):
        for step in (0.600, 0.603):
            result.append(dict(id=f"step-{start}-{stop}-{step}", kind="step", pitch=start,
                               target=stop, step=step, duration=1.4, end=1.25, timbre="harmonic"))
    result.append(dict(id="short-octave-note-120ms", kind="short-step", pitch=60.19,
                       target=72.19, step=0.603, returnTime=0.723, duration=1.3,
                       end=1.15, timbre="harmonic"))
    result.extend([dict(id="silence", kind="silence", duration=0.8),
                   dict(id="unpitched-noise", kind="noise", duration=0.8)])
    for i, case in enumerate(result):
        case.setdefault("duration", 1.2)
        case.setdefault("start", 0.12)
        case.setdefault("end", case["duration"] - 0.15)
        case.setdefault("phase", 0.0)
        case["seed"] = 7919 + 101 * i
    return result


def truth(case, t):
    t = np.asarray(t, dtype=float)
    kind = case["kind"]
    if kind in ("silence", "noise"):
        return np.zeros_like(t)
    midi = np.full_like(t, case["pitch"])
    if kind == "vibrato":
        midi += case["depth"] * np.sin(2 * np.pi * case["modulationHz"] * t)
    elif kind == "glide":
        alpha = np.clip((t - case["glideStart"]) / (case["glideEnd"] - case["glideStart"]), 0, 1)
        midi += alpha * (case["target"] - case["pitch"])
    elif kind in ("step", "short-step"):
        use_target = t >= case["step"]
        if kind == "short-step":
            use_target &= t < case["returnTime"]
        midi[use_target] = case["target"]
    return np.where((t >= case["start"]) & (t < case["end"]), midi, 0)


def synthesize(case):
    t = np.arange(round(case["duration"] * RATE)) / RATE
    rng = np.random.default_rng(case["seed"])
    if case["kind"] == "silence":
        return np.zeros(len(t), dtype=np.float32)
    if case["kind"] == "noise":
        return (0.03 * rng.standard_normal(len(t))).astype(np.float32)
    midi = truth(case, t)
    voiced = midi > 0
    frequency = 440 * 2 ** ((np.where(voiced, midi, case["pitch"]) - 69) / 12)
    # Trapezoidal frequency integration; phase is continuous across pitch steps.
    phase = np.r_[0.0, np.cumsum((frequency[:-1] + frequency[1:]) / (2 * RATE))] + case["phase"]
    harmonics = {"sine": [1.0], "harmonic": [1, .65, .4, .28, .2, .13, .09, .06],
                 "weak-fundamental": [.04, 1, .7, .45, .3, .18, .1],
                 "strong-third": [.3, .35, 1, .2, .15, .1]}[case["timbre"]]
    x = np.zeros_like(t)
    for index, amplitude in enumerate(harmonics, 1):
        # Never alias a harmonic, even at the top of a glide/vibrato.
        if index * float(frequency.max()) >= 0.45 * RATE:
            continue
        x += amplitude * np.sin(2 * np.pi * index * phase)
    fade = np.clip(np.minimum((t - case["start"]) / .02, (case["end"] - t) / .02), 0, 1)
    x *= .15 * fade
    if case.get("amplitudeModulation"):
        x *= .65 + .35 * np.cos(2 * np.pi * 6.3 * t)
    if "snrDb" in case:
        rms = np.sqrt(np.mean(x[voiced] ** 2))
        x += rng.standard_normal(len(t)) * rms * 10 ** (-case["snrDb"] / 20)
    return np.asarray(x, dtype=np.float32)


def region_masks(case, t):
    interior = (t >= case["start"] + EDGE_GUARD) & (t <= case["end"] - EDGE_GUARD)
    transition = np.zeros(len(t), dtype=bool)
    dynamic = np.zeros(len(t), dtype=bool)
    if case["kind"] == "vibrato":
        dynamic = interior.copy()
    elif case["kind"] == "glide":
        dynamic = interior & (t >= case["glideStart"]) & (t <= case["glideEnd"])
        for event in (case["glideStart"], case["glideEnd"]):
            transition |= np.abs(t - event) <= EDGE_GUARD
    elif case["kind"] in ("step", "short-step"):
        for event in [case["step"]] + ([case["returnTime"]] if case["kind"] == "short-step" else []):
            transition |= np.abs(t - event) <= EDGE_GUARD
    return {"steady": interior & ~dynamic & ~transition,
            "dynamic": interior & dynamic & ~transition,
            "transition": interior & transition,
            "voicedInterior": interior}


def metrics(expected, observed, mask):
    ref = mask & (expected > 0)
    matched = ref & (observed > 0)
    error = 100 * (observed[matched] - expected[matched])
    ae = np.abs(error)
    fine = ae[ae <= GROSS_CENTS]
    count, n = int(ref.sum()), len(ae)
    octave = (ae >= 600) & (np.abs(ae - 1200 * np.rint(ae / 1200)) <= GROSS_CENTS)
    return dict(referenceVoicedFrames=count, detectedVoicedFrames=n, missedVoicedFrames=count - n,
                voicedRecall=n / count if count else None,
                meanAbsoluteCents=float(ae.mean()) if n else None,
                medianAbsoluteCents=float(np.median(ae)) if n else None,
                p95AbsoluteCents=float(np.quantile(ae, .95)) if n else None,
                signedBiasCents=float(error.mean()) if n else None,
                meanAbsoluteCentsWithin50=float(fine.mean()) if len(fine) else None,
                p95AbsoluteCentsWithin50=float(np.quantile(fine, .95)) if len(fine) else None,
                grossPitchErrors=int((ae > GROSS_CENTS).sum()),
                grossPitchErrorShareAmongDetected=float(np.mean(ae > GROSS_CENTS)) if n else None,
                octaveErrors=int(octave.sum()), octaveErrorShare=int(octave.sum()) / count if count else None,
                correctWithin50CentsShare=int((ae <= GROSS_CENTS).sum()) / count if count else None)


def transition_details(case, t, observed):
    if case["kind"] not in ("step", "short-step"):
        return []
    transitions = [(case["step"], case["pitch"], case["target"])]
    if case["kind"] == "short-step":
        transitions.append((case["returnTime"], case["target"], case["pitch"]))
    result = []
    for q, (at, before, after) in enumerate(transitions):
        end = min(at + .12, transitions[q + 1][0] if q + 1 < len(transitions) else case["end"])
        direction = 1 if after > before else -1
        near = np.flatnonzero((t >= at - .064) & (t < end))
        crossed = [i for i in near if observed[i] > 0 and direction * (observed[i] - (before + after) / 2) >= 0]
        settled = []
        for i in near:
            if t[i] < at or i + 2 >= len(t) or t[i + 2] >= end:
                continue
            if np.all(observed[i:i + 3] > 0) and np.all(np.abs(observed[i:i + 3] - after) <= .25):
                settled.append(i)
        valid = (t >= at) & (t < end) & (observed > 0)
        overshoot = direction * (observed[valid] - after) * 100
        result.append(dict(time=at, fromMidi=before, toMidi=after,
                           midpointDelayMs=1000 * (t[crossed[0]] - at) if crossed else None,
                           settlingDelayMs=1000 * (t[settled[0]] - at) if settled else None,
                           maximumOvershootCents=max(0.0, float(overshoot.max())) if len(overshoot) else None,
                           missedFrames=int(((t >= at) & (t < end) & (observed <= 0)).sum()),
                           observationEnd=end))
    return result


def vibrato_details(case, t, observed):
    if case["kind"] != "vibrato":
        return None
    keep = region_masks(case, t)["dynamic"] & (observed > 0)
    if keep.sum() < 10:
        return None
    omega = 2 * np.pi * case["modulationHz"]
    design = np.column_stack([np.ones(int(keep.sum())), np.sin(omega * t[keep]), np.cos(omega * t[keep])])
    coef, _, _, _ = np.linalg.lstsq(design, observed[keep], rcond=None)
    phase = math.atan2(coef[2], coef[1])
    return dict(fittedCenterMidi=float(coef[0]), amplitudeRatio=float(np.hypot(coef[1], coef[2]) / case["depth"]),
                fittedDelayMs=-1000 * phase / omega, note="Sinusoidal fit on voiced interior frames; positive delay means later.")


def summarize(records):
    summary = {}
    for region in ("steady", "dynamic", "transition", "voicedInterior"):
        truth_arrays, observed_arrays, masks = [], [], []
        for r in records:
            rows = np.asarray(r["comparisonFrames"])
            t, expected, observed = rows[:, 0], rows[:, 1], rows[:, 2]
            truth_arrays.append(expected)
            observed_arrays.append(observed)
            masks.append(region_masks(r["case"], t)[region])
        summary[region] = metrics(np.concatenate(truth_arrays), np.concatenate(observed_arrays), np.concatenate(masks))
    summary["runtimeSeconds"] = sum(r["runtimeSeconds"] for r in records)
    summary["audioSeconds"] = sum(r["case"]["duration"] for r in records)
    summary["realtimeFactor"] = summary["runtimeSeconds"] / summary["audioSeconds"]
    tp, fp, fn = [sum(r["voicing"][key] for r in records)
                  for key in ("truePositive", "falsePositive", "falseNegative")]
    summary["voicing"] = dict(truePositive=tp, falsePositive=fp, falseNegative=fn,
                              recall=tp / (tp + fn) if tp + fn else None,
                              precision=tp / (tp + fp) if tp + fp else None)
    return summary


def run_worker(args):
    sys.path.insert(0, str(args.repo / "bindings" / "python"))
    from novox import Singer
    library = args.baseline_lib.resolve()
    options = dict(algorithm="singing-v2", debugFrames=True, roundPitches=False, detail="natural")
    options.update(json.loads(args.baseline_options))
    if options.get("algorithm") != "singing-v2" or options.get("debugFrames") is not True:
        raise ValueError("the probe requires singing-v2 and debugFrames=true")
    singer = Singer(sample_rate=RATE, library=library)
    # Library loading and warmup are deliberately outside the reported timings.
    singer.transcribe(np.zeros(RATE // 4, dtype=np.float32), RATE, **options)
    records = []
    selected = [c for c in cases() if not args.case_filter or args.case_filter in c["id"]]
    if not selected:
        raise ValueError("no matching cases")
    for case in selected:
        samples = synthesize(case)
        before = time.perf_counter()
        score = singer.transcribe(samples, RATE, **options)
        elapsed = time.perf_counter() - before
        frames = np.asarray(score.get("frames", []), dtype=float)
        if frames.ndim != 2 or frames.shape[1] < 5 or not len(frames):
            raise ValueError("missing/invalid debug frames")
        t, observed = frames[:, 0], frames[:, 1]
        expected = truth(case, t)
        regions = {key: metrics(expected, observed, mask) for key, mask in region_masks(case, t).items()}
        # Ignore fade-adjacent windows for voicing; score silent/noise cases throughout.
        voicing_mask = np.ones(len(t), dtype=bool)
        if case["kind"] not in ("silence", "noise"):
            voicing_mask &= (np.abs(t - case["start"]) >= EDGE_GUARD) & (np.abs(t - case["end"]) >= EDGE_GUARD)
        ref, got = (expected > 0) & voicing_mask, (observed > 0) & voicing_mask
        tp, fp, fn = int((ref & got).sum()), int((~ref & got).sum()), int((ref & ~got).sum())
        records.append(dict(case=case, audioSha256=hashlib.sha256(samples.tobytes()).hexdigest(),
                            runtimeSeconds=elapsed, regions=regions,
                            voicing=dict(truePositive=tp, falsePositive=fp, falseNegative=fn,
                                         recall=tp / (tp + fn) if tp + fn else None,
                                         precision=tp / (tp + fp) if tp + fp else None),
                            transitions=transition_details(case, t, observed),
                            vibrato=vibrato_details(case, t, observed),
                            noteCount=len(score.get("notes", [])), metadata=score.get("transcription", {}),
                            comparisonFrames=np.column_stack([t, expected, observed, frames[:, 3], frames[:, 4]]).tolist()))
    grouped = {kind: summarize([r for r in records if r["case"]["kind"] == kind])
               for kind in sorted({r["case"]["kind"] for r in records})}
    payload = dict(library=str(library), librarySha256=sha256(library), options=options,
                   summary=summarize(records), byKind=grouped, cases=records)
    args.out.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n")


def compare(baseline, candidate):
    result = []
    by_id = {r["case"]["id"]: r for r in candidate["cases"]}
    for a in baseline["cases"]:
        b = by_id[a["case"]["id"]]
        af, bf = np.asarray(a["comparisonFrames"]), np.asarray(b["comparisonFrames"])
        if a["audioSha256"] != b["audioSha256"] or af.shape != bf.shape or not np.array_equal(af[:, :2], bf[:, :2]):
            raise ValueError("A/B audio or frame timeline differs")
        valid = (af[:, 2] > 0) & (bf[:, 2] > 0)
        delta = 100 * (bf[valid, 2] - af[valid, 2])
        result.append(dict(id=a["case"]["id"], voicedMaskChanges=int(((af[:, 2] > 0) != (bf[:, 2] > 0)).sum()),
                           confidenceChanges=int((af[:, 4] != bf[:, 4]).sum()),
                           clarityChanges=int((af[:, 3] != bf[:, 3]).sum()),
                           maximumPitchChangeCents=float(np.abs(delta).max()) if len(delta) else None,
                           noteCountChange=b["noteCount"] - a["noteCount"]))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", type=Path, default=Path.cwd(),
                    help="Novox repository root containing bindings/python (default: current directory)")
    ap.add_argument("--baseline-lib", type=Path, required=True)
    ap.add_argument("--candidate-lib", type=Path)
    ap.add_argument("--baseline-options", default="{}", help="JSON transcription-option overrides")
    ap.add_argument("--candidate-options", default="{}", help="JSON transcription-option overrides")
    ap.add_argument("--out", type=Path, default=Path("novox-synthetic-results.json"))
    ap.add_argument("--case-filter", default="", help="optional substring for a smoke run")
    ap.add_argument("--include-frames", action="store_true")
    ap.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    args.repo = args.repo.resolve()
    args.out = args.out.resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.worker:
        run_worker(args)
        return
    for value in (args.baseline_options, args.candidate_options):
        if not isinstance(json.loads(value), dict):
            raise ValueError("option overrides must be JSON objects")
    result = dict(version=1, sampleRate=RATE, hopSeconds=HOP, edgeGuardSeconds=EDGE_GUARD,
                  grossErrorThresholdCents=GROSS_CENTS,
                  notes=["Pitch errors use reported frame time and known instantaneous synthetic F0.",
                         "Absolute pitch statistics use jointly voiced frames; recall and correctWithin50CentsShare also penalize misses.",
                         "Steady statistics exclude 64 ms on each side of pitch boundaries and voiced edges.",
                         "MIDI debug JSON has a 0.1-cent quantization step.",
                         "Runtime is warmed end-to-end transcription including JSON, not pitch-kernel time.",
                         "Synthetic evidence alone does not establish improvement on real singing."],
                  scriptSha256=sha256(Path(__file__)), variants={})
    with tempfile.TemporaryDirectory(prefix="novox-synthetic-", dir=args.out.parent) as temp:
        for name, lib, options in (("baseline", args.baseline_lib, args.baseline_options),
                                   ("candidate", args.candidate_lib, args.candidate_options)):
            if lib is None:
                continue
            dest = Path(temp) / (name + ".json")
            command = [sys.executable, str(Path(__file__).resolve()), "--worker", "--repo", str(args.repo),
                       "--baseline-lib", str(lib.resolve()), "--baseline-options", options,
                       "--out", str(dest), "--case-filter", args.case_filter]
            subprocess.run(command, check=True)
            result["variants"][name] = json.loads(dest.read_text())
    if "candidate" in result["variants"]:
        result["pairedChanges"] = compare(result["variants"]["baseline"], result["variants"]["candidate"])
    if not args.include_frames:
        for variant in result["variants"].values():
            for case in variant["cases"]:
                del case["comparisonFrames"]
    args.out.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    for name, variant in result["variants"].items():
        s = variant["summary"]
        print(name, json.dumps(s, sort_keys=True))
    print(f"Saved {args.out}")


if __name__ == "__main__":
    main()
