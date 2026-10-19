// Shaping an audio import's notes: the key they are in, and timing on the editor's grid.
// Pure functions over {pitch, start, dur} notes in beats; app.js applies them to a new import.

const MAJOR = [0, 2, 4, 5, 7, 9, 11];
// Krumhansl-Kessler key profiles (probe-tone ratings, C major and C minor).
const KK_MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88];
const KK_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17];
const pc = (pitch) => ((Math.round(pitch) % 12) + 12) % 12;

function correlation(a, profile, root) {
  const b = a.map((_, i) => profile[(i - root + 12) % 12]);
  const mean = (v) => v.reduce((s, x) => s + x, 0) / v.length;
  const ma = mean(a), mb = mean(b);
  let num = 0, da = 0, db = 0;
  for (let i = 0; i < 12; i++) { num += (a[i] - ma) * (b[i] - mb); da += (a[i] - ma) ** 2; db += (b[i] - mb) ** 2; }
  return da > 0 && db > 0 ? num / Math.sqrt(da * db) : 0;
}

// The key of a melody: {root (tonic pitch class 0-11), scale ("major"/"minor"), coverage}, or null
// for fewer than 8 notes. The seven-note scale chosen is the one holding the most sung time
// (what snapping to it would leave alone), ties broken by the Krumhansl-Kessler profiles, which
// also tell major from its relative minor. coverage: the share of sung time inside that scale.
// On the evaluation corpora it names the reference melody's own scale for 12 of Jonathan's 15 dev
// takes and 38 of 46 CSD songs.
export function detectKey(notes) {
  const hist = new Array(12).fill(0);
  let total = 0;
  for (const n of notes || []) {
    const d = Math.max(0, +n.dur || 0);
    if (!Number.isFinite(+n.pitch) || !d) continue;
    hist[pc(n.pitch)] += d;
    total += d;
  }
  if ((notes || []).length < 8 || total <= 0) return null;
  let best = null;
  for (let root = 0; root < 12; root++) {
    const coverage = Math.round((MAJOR.reduce((s, k) => s + hist[(root + k) % 12], 0) / total) * 1000) / 1000;
    const major = correlation(hist, KK_MAJOR, root), minor = correlation(hist, KK_MINOR, (root + 9) % 12);
    const fit = Math.max(major, minor);
    if (!best || coverage > best.coverage || (coverage === best.coverage && fit > best.fit))
      best = { setRoot: root, coverage, fit, minor: minor > major };
  }
  return best.minor ? { root: (best.setRoot + 9) % 12, scale: "minor", coverage: best.coverage }
                    : { root: best.setRoot, scale: "major", coverage: best.coverage };
}

// Notes moved onto a grid of `grid` beats (0.25 = 1/16): starts and ends to the nearest line, at
// least one step long, every start a step after the one before (two syllables inside one step take
// the next line rather than merge), and a note ends where the next begins when less than half a
// step separated them (rounding would only leave a sliver). Ends past `end` are cut back to it (the
// import's span). Returns new objects in time order; grid <= 0 returns plain copies.
export function quantizeNotes(notes, grid, { end = Infinity } = {}) {
  const sorted = [...(notes || [])].sort((a, b) => a.start - b.start);
  if (!(grid > 0)) return sorted.map((n) => ({ ...n }));
  const q = (x) => Math.round(x / grid + 1e-9) * grid;
  const round = (x) => Math.round(x * 1e6) / 1e6;
  const out = [];
  let floor = -Infinity;
  for (const n of sorted) {
    const start = Math.max(q(n.start), floor);
    const stop = Math.max(q(n.start + n.dur), start + grid);
    out.push({ ...n, start, dur: stop - start });
    floor = start + grid;
  }
  for (let i = 0; i < out.length; i++) {
    const next = out[i + 1];
    if (next) {
      const gap = sorted[i + 1].start - (sorted[i].start + sorted[i].dur);
      if (out[i].start + out[i].dur > next.start || gap < grid / 2) out[i].dur = next.start - out[i].start;
    }
    if (out[i].start < end && out[i].start + out[i].dur > end) out[i].dur = Math.max(grid, q(end) - out[i].start);
    out[i].start = round(out[i].start);
    out[i].dur = round(out[i].dur);
  }
  return out;
}

// The detected note behind an edited one (an import's originalNotes, in beats): the original
// overlapping it most at the same pitch, else at any pitch within two semitones (the note was
// moved onto the grid or into the key since). null when none overlaps.
export function evidenceFor(note, evidence) {
  let best = null, bestScore = 0;
  for (const e of evidence || []) {
    const overlap = Math.min(note.start + note.dur, e.start + e.dur) - Math.max(note.start, e.start);
    if (overlap <= 0 || Math.abs(Math.round(e.pitch) - note.pitch) > 2) continue;
    const score = overlap + (Math.round(e.pitch) === note.pitch ? 1e3 : 0);
    if (score > bestScore) { best = e; bestScore = score; }
  }
  return best;
}
