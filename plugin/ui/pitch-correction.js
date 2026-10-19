// Explicit editor operation. Detection and its original evidence stay separate.
export const SCALES = { major: [0,2,4,5,7,9,11], minor: [0,2,3,5,7,8,10] };
export function correctedPitch(note, settings, evidence) {
  const root = Number(settings.root), scale = SCALES[settings.scale];
  if (!Number.isInteger(root) || root < 0 || root > 11 || !scale) return note.pitch;
  const allowed = pitch => scale.includes(((pitch-root)%12+12)%12);
  if (allowed(note.pitch)) return note.pitch;
  const measured = Number.isFinite(evidence?.detectedPitch) ? evidence.detectedPitch : note.pitch;
  let best = note.pitch, distance = Infinity, tied = false;
  for (let pitch=0;pitch<=127;pitch++) {
    if (!allowed(pitch)) continue;
    const d = Math.abs(pitch-measured);
    if (d < distance-1e-6) {best=pitch;distance=d;tied=false;}
    else if (Math.abs(d-distance)<1e-6) tied=true;
  }
  // An equally close sharp/flat has no evidence for a direction.
  if (tied && settings.mode !== 'snap') return note.pitch;
  if (settings.mode === 'snap') return best;
  if (settings.mode !== 'gentle' || !evidence || distance > .65) return note.pitch;
  if (evidence.confidence >= .9 && evidence.pitchSpread <= .5) return note.pitch;
  return best;
}
