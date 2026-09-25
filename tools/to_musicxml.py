"""Write a score (our JSON format) as MusicXML with lyrics, so other singers (Harmony Assistant's
Virtual Singer, notation apps) can sing exactly the same notes and syllables.

Syllables come from our own planner, so lyric flowing/hyphenation match what our engine sings.
Melisma notes get no lyric (they extend the previous syllable).

    tools/.venv/bin/python tools/to_musicxml.py scores/demo.json out/demo.xml
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))

import nvs  # noqa: E402

DIVISIONS = 12  # per quarter note: allows 16ths and triplet 8ths
STEPS = [("C", 0), ("C", 1), ("D", 0), ("D", 1), ("E", 0), ("F", 0), ("F", 1), ("G", 0), ("G", 1), ("A", 0),
         ("A", 1), ("B", 0)]
TYPES = {48: "whole", 36: "half", 24: "half", 18: "quarter", 12: "quarter", 9: "eighth", 6: "eighth",
         3: "16th"}


def note_type(dur: int) -> tuple[str, bool]:
    for d, name in sorted(TYPES.items(), reverse=True):
        if dur >= d:
            return name, d in (36, 18, 9)
    return "16th", False


def pitch_xml(midi: int) -> str:
    step, alter = STEPS[midi % 12]
    octave = midi // 12 - 1
    alt = "<alter>1</alter>" if alter else ""
    return f"<pitch><step>{step}</step>{alt}<octave>{octave}</octave></pitch>"


def convert(score: dict, singer: nvs.Singer) -> str:
    tempo = float(score.get("tempo", 120))
    if score.get("units") == "seconds":
        raise SystemExit("seconds-based scores aren't supported; use beats")
    plan = singer.plan(score)
    beat_sec = 60.0 / tempo
    # Syllable texts keyed by start time (in beats); plan times are relative to score time 0.
    syl_at = {round(s["start"] / beat_sec, 3): s for s in plan["syllables"]}

    # Expand notes with explicit starts (like the engine does).
    events, cursor = [], 0.0
    for n in score["notes"]:
        if isinstance(n, str):
            parts = n.split(maxsplit=2)
            pitch, dur = parts[0], float(parts[1])
            start = cursor
        else:
            pitch, dur, start = n.get("pitch", "r"), float(n["dur"]), float(n.get("start", cursor))
        if start > cursor + 1e-6:
            events.append(("r", cursor, start - cursor))
        events.append((pitch, start, dur))
        cursor = start + dur

    body, measure, pos, number = [], [], 0, 1
    per_measure = 4 * DIVISIONS

    def flush():
        nonlocal measure, number
        attrs = ""
        if number == 1:
            attrs = (f"<attributes><divisions>{DIVISIONS}</divisions><key><fifths>0</fifths></key>"
                     "<time><beats>4</beats><beat-type>4</beat-type></time>"
                     "<clef><sign>G</sign><line>2</line></clef></attributes>"
                     f"<direction placement=\"above\"><sound tempo=\"{tempo:g}\"/></direction>")
        body.append(f"<measure number=\"{number}\">{attrs}{''.join(measure)}</measure>")
        measure, number = [], number + 1

    def midi_of(p):
        if isinstance(p, (int, float)):
            return int(round(p))
        from_name = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
        semis, i = from_name[p[0].upper()], 1
        while i < len(p) and p[i] in "#b":
            semis += 1 if p[i] == "#" else -1
            i += 1
        return (int(p[i:]) + 1) * 12 + semis

    for pitch, start, dur in events:
        remaining = int(round(dur * DIVISIONS))
        first = True
        syl = syl_at.get(round(start, 3))
        while remaining > 0:
            chunk = min(remaining, per_measure - pos)
            name, dotted = note_type(chunk)
            is_rest = str(pitch).lower() in ("r", "rest")
            tie_start = remaining - chunk > 0 and not is_rest
            tie_stop = not first and not is_rest
            xml = "<note>"
            xml += "<rest/>" if is_rest else pitch_xml(midi_of(pitch))
            xml += f"<duration>{chunk}</duration>"
            if tie_stop:
                xml += "<tie type=\"stop\"/>"
            if tie_start:
                xml += "<tie type=\"start\"/>"
            xml += f"<type>{name}</type>" + ("<dot/>" if dotted else "")
            if tie_start or tie_stop:
                xml += "<notations>" + ("<tied type=\"stop\"/>" if tie_stop else "") + \
                       ("<tied type=\"start\"/>" if tie_start else "") + "</notations>"
            if first and syl and not is_rest and syl["text"]:
                text = syl["text"].strip(",.;:!?\"")
                xml += f"<lyric number=\"1\"><syllabic>{syl_kind(syl, plan)}</syllabic><text>{escape(text)}</text></lyric>"
            xml += "</note>"
            measure.append(xml)
            remaining -= chunk
            pos += chunk
            first = False
            if pos >= per_measure:
                flush()
                pos = 0
    if measure:
        measure.append(f"<note><rest/><duration>{per_measure - pos}</duration></note>")
        flush()

    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 3.1 Partwise//EN\" "
            "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
            "<score-partwise version=\"3.1\"><part-list><score-part id=\"P1\"><part-name>Voice</part-name>"
            "</score-part></part-list><part id=\"P1\">" + "".join(body) + "</part></score-partwise>\n")


def syl_kind(syl: dict, plan: dict) -> str:
    """begin/middle/end/single, from whether neighbors are parts of the same word."""
    sylls = plan["syllables"]
    i = sylls.index(syl)
    cont_prev = i > 0 and sylls[i - 1].get("continues", False)
    cont_next = syl.get("continues", False)
    return {(False, False): "single", (False, True): "begin", (True, True): "middle",
            (True, False): "end"}[(cont_prev, cont_next)]


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    text = Path(src).read_text() if src.endswith(".json") else src
    # Our scores allow // comments; strip them for Python's JSON parser.
    text = "\n".join(line for line in text.splitlines() if not line.strip().startswith("//"))
    score = json.loads(text)
    Path(dst).write_text(convert(score, nvs.Singer()))
    print(dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
