import assert from 'node:assert/strict';
import {correctedPitch, SCALES} from '../ui/pitch-correction.js';
const key={root:0,scale:'major',mode:'gentle'};
assert.equal(correctedPitch({pitch:63},key,{detectedPitch:63.4,confidence:.8,pitchSpread:.7}),64);
assert.equal(correctedPitch({pitch:63},key,{detectedPitch:63.1,confidence:.99,pitchSpread:.1}),63);
assert.equal(correctedPitch({pitch:63},key),63);
assert.equal(correctedPitch({pitch:63},{...key,mode:'snap'}),62);
assert.equal(correctedPitch({pitch:60},{...key,mode:'snap'}),60);
assert.equal(correctedPitch({pitch:63},{...key,root:-1,mode:'snap'}),63);
assert.equal(correctedPitch({pitch:127},{root:1,scale:'major',mode:'snap'}),126);
for(let root=0;root<12;root++) for(const scale of Object.keys(SCALES)) for(let pitch=0;pitch<128;pitch++) {
  const result=correctedPitch({pitch},{root,scale,mode:'snap'});
  assert.ok(result>=0 && result<=127);
  assert.ok(SCALES[scale].includes(((result-root)%12+12)%12));
}
console.log('Key correction: uncertain drift, chromatic preservation, ties, missing evidence, disabled mode and all MIDI pitches/scales passed');
