"""Compare legacy and singing-v1 against human annotated recordings.
Usage: tools/.venv/bin/python tools/transcribe_benchmark.py reference/manifest.json -o out/benchmark.json
Requires ffmpeg for decoding; no speech recognition is run. See docs/SINGING-TRANSCRIPTION-SPEC.md.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'bindings/python'))
from novox import Singer


def match(truth, detected, predicate):
    """Min-cost maximum-cardinality bipartite flow; timing breaks match ties."""
    n, m = len(truth), len(detected)
    size, source, sink = n + m + 2, n + m, n + m + 1
    graph = [[] for _ in range(size)]
    def edge(a, b, cost):
        graph[a].append([b, len(graph[b]), 1, cost])
        graph[b].append([a, len(graph[a])-1, 0, -cost])
    for i in range(n): edge(source, i, 0)
    for j in range(m): edge(n+j, sink, 0)
    for i, t in enumerate(truth):
        for j, g in enumerate(detected):
            if predicate(t, g): edge(i, n+j, abs(t['start']-g['start']))
    while True:
        dist, prev = [math.inf]*size, [None]*size
        dist[source] = 0
        for _ in range(size-1):
            changed = False
            for a in range(size):
                for k, (b, _, capacity, cost) in enumerate(graph[a]):
                    if capacity and dist[a] + cost < dist[b]-1e-12:
                        dist[b], prev[b], changed = dist[a]+cost, (a,k), True
            if not changed: break
        if prev[sink] is None: break
        b = sink
        while b != source:
            a,k = prev[b]; e = graph[a][k]
            e[2] -= 1; graph[b][e[1]][2] += 1; b = a
    return [(i,e[0]-n) for i in range(n) for e in graph[i] if n <= e[0] < n+m and e[2] == 0]


def percentile(values, fraction):
    if not values: return None
    values = sorted(values); at = (len(values)-1)*fraction
    low = int(at); high = min(low+1,len(values)-1)
    return values[low]*(high-at)+values[high]*(at-low) if high != low else values[low]


def evaluate(truth, got):
    onset = lambda t,g: abs(t['start']-g['start']) <= .0600001
    exact = lambda t,g: onset(t,g) and round(t['pitch']) == round(g['pitch'])
    pairs = match(truth,got,exact)
    strict = match(truth,got,lambda t,g: exact(t,g) and abs(t['start']+t['dur']-g['start']-g['dur']) <= max(.08,.2*t['dur']))
    associated = match(truth,got,lambda t,g: min(t['start']+t['dur'],g['start']+g['dur'])-max(t['start'],g['start']) > 0 and (onset(t,g) or abs(t['start']-g['start']) < .2))
    octave = sum(abs(round(truth[i]['pitch'])-round(got[j]['pitch'])) in (12,24,36) for i,j in associated)
    other = sum(round(truth[i]['pitch']) != round(got[j]['pitch']) and abs(round(truth[i]['pitch'])-round(got[j]['pitch'])) not in (12,24,36) for i,j in associated)
    reference_duration = sum(t['dur'] for t in truth)
    correct_time = octave_time = 0.0
    for t in truth:
        for g in got:
            overlap = max(0.0, min(t['start']+t['dur'], g['start']+g['dur'])-max(t['start'],g['start']))
            delta = abs(round(t['pitch'])-round(g['pitch']))
            if delta == 0: correct_time += overlap
            if delta in (12,24,36): octave_time += overlap
    starts = [1000*abs(truth[i]['start']-got[j]['start']) for i,j in pairs]
    ends = [1000*abs(truth[i]['start']+truth[i]['dur']-got[j]['start']-got[j]['dur']) for i,j in pairs]
    mergedRepeats = sum(max(0,sum(round(g["pitch"])==round(t["pitch"]) and min(t["start"]+t["dur"],g["start"]+g["dur"]) > max(t["start"],g["start"])+.016 for t in truth)-1) for g in got)
    fragments = sum(max(0,sum(round(g['pitch'])==round(t['pitch']) and min(t['start']+t['dur'],g['start']+g['dur']) > max(t['start'],g['start'])+.016 for g in got)-1) for t in truth)
    return dict(referenceTimePitchAccuracy=correct_time/reference_duration if reference_duration else None, referenceTimeOctaveError=octave_time/reference_duration if reference_duration else None, referenceNotes=len(truth),detectedNotes=len(got),matched=len(pairs),strictEvents=len(strict),precision=len(pairs)/len(got) if got else None,recall=len(pairs)/len(truth) if truth else None,missed=len(truth)-len(pairs),extra=len(got)-len(pairs),octaveSubstitutions=octave,otherPitchSubstitutions=other,fragmentation=fragments,mergedRepeatedNotes=mergedRepeats,onsetMedianMs=percentile(starts,.5),onsetP90Ms=percentile(starts,.9),endMedianMs=percentile(ends,.5),endP90Ms=percentile(ends,.9))


def main():
    ap=argparse.ArgumentParser();ap.add_argument('manifest',type=Path);ap.add_argument('-o','--out',type=Path,default=ROOT/'out/benchmark.json');args=ap.parse_args()
    manifest=json.loads(args.manifest.read_text())
    if manifest.get('version') != 1: raise ValueError('manifest version must be 1')
    singer=Singer()
    reports=[]
    for case in manifest['cases']:
        raw=subprocess.run(['ffmpeg','-v','error','-i',str(args.manifest.parent/case['audio']),'-f','f32le','-ac','1','-ar','16000','pipe:1'],check=True,capture_output=True).stdout
        samples=struct.unpack('<'+'f'*(len(raw)//4),raw)
        ignored=lambda note:any(min(note['start']+note['dur'],r['end'])>max(note['start'],r['start']) for r in case.get('ignore',[]))
        truth=[t for t in case['notes'] if not ignored(t)]
        for algorithm in ('legacy','singing-v1'):
            before=time.perf_counter();score=singer.transcribe(samples,16000,words=case.get('words'),algorithm=algorithm);elapsed=time.perf_counter()-before
            metrics=evaluate(truth,[g for g in score['notes'] if not ignored(g)])
            metadata=score.get('transcription',{})
            metrics.update(id=case['id'],algorithm=algorithm,tags=case.get('tags',[]),audioSeconds=len(samples)/16000,runtimeSeconds=elapsed,wordsProvided=len(case.get('words',[])),wordsRetained=len(metadata.get('transcriptWords',[])),unassignedWords=len(metadata.get('unassignedWords',[])),score=score)
            reports.append(metrics);print(f"{case['id']} {algorithm}: {metrics['matched']}/{len(truth)} pitch/onset matches; {metrics['octaveSubstitutions']} octave errors; {metrics['fragmentation']} fragments")
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(dict(version=1,cases=reports),indent=2)+'\n')
if __name__=='__main__': main()
