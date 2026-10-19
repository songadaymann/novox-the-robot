// Engine warnings that change how far to trust an import, most serious first, with the short
// form the LCD line has room for: the transcript could not be the lyrics (melody only), then
// accompaniment or a second voice under the singer. Other warnings show as written.
const SERIOUS_WARNINGS = [
  [/melody only/i, "lyrics didn't fit: melody only"],
  [/accompaniment|second voice/i, 'accompaniment heard: check pitches'],
];

// The LCD flash after an audio import ({state, line}): the note count, or the number of engine
// warnings and the most serious one (the LCD tooltip lists them all). Per-word warnings can run
// to dozens; joining them all only truncated the line. added: the import joined the song (Add to
// song) at the position `at` (text, e.g. "2.1.1") instead of replacing it.
export function importSummary(score, name, {added = false, at = ''} = {}) {
  const count = score.notes.length, verb = added ? 'ADDED' : 'IMPORTED';
  const warnings = (score.transcribeWarnings || []).map(w => String(w).trim()).filter(Boolean);
  const notes = `${count} note${count === 1 ? '' : 's'}`;
  // An added import's position comes before the file name, so a narrow LCD still shows it.
  if (!warnings.length) return {state: `${verb} ✓`, line: added && at ? `${notes} at ${at} · ${name}` : `${notes} from ${name}`};
  const rank = w => { const i = SERIOUS_WARNINGS.findIndex(([re]) => re.test(w)); return i < 0 ? SERIOUS_WARNINGS.length : i; };
  const first = warnings.reduce((best, w) => rank(w) < rank(best) ? w : best);
  return {state: `${verb} · ${warnings.length} WARNING${warnings.length === 1 ? '' : 'S'}`,
          line: SERIOUS_WARNINGS[rank(first)]?.[1] || first};
}

// Where a DAW-exported recording sat on the timeline, in beats at `bpm`, or null when it carries
// no position. result.timeReference is seconds from 00:00:00:00: a Broadcast WAV stamp, or the
// "Timestamp:" marker of a region Logic drags onto the plugin (an AIFF). Bar 1 is the host's edit
// origin (status.editOrigin), or Logic's default 01:00:00:00 (whole hours) for Logic/GarageBand
// files. The host's own seconds/beats pair at the playhead absorbs earlier tempo changes.
export function timelinePosition(result, status, bpm) {
  const stamp = +result.timeReference, st = status || {};
  if (!(stamp > 0)) return null;
  const known = (v) => typeof v === 'number' && Number.isFinite(v);
  let origin = known(st.editOrigin) ? st.editOrigin : null;
  if (origin === null && /^(Logic|GarageBand)/i.test(result.originator || '')) origin = Math.floor(stamp / 3600) * 3600;
  if (origin === null || stamp < origin) return null;
  const seconds = stamp - origin;
  const beats = st.host && known(st.seconds) ? st.beats + (seconds - st.seconds) * bpm / 60 : seconds * bpm / 60;
  return Math.max(0, Math.round(beats * 1e4) / 1e4); // drop sample-rounding dust
}
// The same, with beat 0 for a recording without a position.
export function timelineBeats(result, status, bpm) {
  return timelinePosition(result, status, bpm) ?? 0;
}
// Where an audio import's notes go, in beats: the recording's timeline position; without one, the
// playhead (status.beats: the host's in a DAW, the app's own in the standalone) when the import is
// added to the song, or beat 0 when it replaces the song.
export function importPosition(result, status, bpm, addToSong) {
  const at = timelinePosition(result, status, bpm);
  if (at !== null) return at;
  return addToSong ? Math.max(0, Math.round((+status?.beats || 0) * 1e4) / 1e4) : 0;
}

// Convert a recording, then place it directly in the existing song editor.
export function createAudioImport(B, acceptScore, notify = () => {}) {
  const dialog = document.createElement('dialog');
  dialog.className = 'audio-import-consent';
  dialog.innerHTML = `<header><h2>Import recording</h2><button data-action="cancel">Cancel</button></header>
    <p>Download the lyrics model (574 MB)? Your recording stays on this computer.</p>
    <button data-action="download">Download &amp; convert</button><button data-action="melody">Melody only</button>`;
  document.body.append(dialog);
  let upload = null, active = false, generation = 0, timer = 0;
  const report = (text) => notify('IMPORTING…', text);
  const cancel = () => {
    generation++; clearTimeout(timer); active = false; B.cancelAudioImport();
    if (dialog.open) dialog.close(); notify('IMPORT CANCELLED');
  };
  dialog.addEventListener('cancel', e => { e.preventDefault(); cancel(); });
  async function poll(token) {
    if (!active || token !== generation) return;
    try {
      const state = await B.audioImportStatus();
      if (!active || token !== generation) return;
      if (state.state === 'ready') {
        const result = state.result;
        const score = structuredClone(result.score); score.voice = upload.voice;
        if (!Array.isArray(score.notes) || !score.notes.length) throw new Error("No notes detected in this recording");
        await acceptScore(score, result.name, result, upload.status);
        active = false; return;
      }
      if (state.state === 'error' || state.state === 'cancelled') {
        active = false; notify('CAN’T IMPORT', state.message); return;
      }
      report(state.message || 'Converting recording…');
      timer = setTimeout(() => poll(token), 350);
    } catch(e) { active = false; notify('CAN’T IMPORT', String(e)); }
  }
  async function convert(download, melodyOnly) {
    if (dialog.open) dialog.close();
    const token = generation; report('Converting recording…');
    try {
      const response = await B.startAudioImport(upload.b64, upload.name, download, melodyOnly, upload.detailed);
      if (token !== generation) return;
      if (response?.error || typeof response === 'string') throw new Error(response.error || response);
      await poll(token);
    } catch(e) { active = false; notify('CAN’T IMPORT', e.message || String(e)); }
  }
  dialog.addEventListener('click', e => {
    const action = e.target.closest('[data-action]')?.dataset.action;
    if (action === 'cancel') cancel();
    if (action === 'download') convert(true,false);
    if (action === 'melody') convert(false,true);
  });
  // detailed: also split scoops, grace notes and quick pitch steps into notes (fixed at open).
  // status: the transport when the recording arrived (its playhead places a recording without a
  // timeline position), handed back to acceptScore: converting takes seconds, during which a
  // playing transport moves on.
  async function open(file, voice = 'std', detailed = false, status = null) {
    if (active) { notify('IMPORT IN PROGRESS', 'Wait for the current recording to finish'); return; }
    active = true; const token = ++generation; report('Reading recording…');
    try {
      if (file.size > 64*1024*1024) throw new Error('Choose a recording smaller than 64 MB');
      let b64 = '';
      if (file.arrayBuffer) {
        const bytes = new Uint8Array(await file.arrayBuffer()); let binary = '';
        for (let i=0;i<bytes.length;i+=32768) binary += String.fromCharCode(...bytes.subarray(i,i+32768));
        b64 = btoa(binary);
      }
      if (token !== generation) return;
      upload = {b64,name:file.name,voice,detailed:!!detailed,status};
      const state = await B.audioImportStatus();
      if (token !== generation) return;
      if (!state.modelAvailable) dialog.showModal(); else await convert(false,false);
    } catch(e) { active = false; notify('CAN’T IMPORT', e.message || String(e)); }
  }
  return {open,cancel};
}
