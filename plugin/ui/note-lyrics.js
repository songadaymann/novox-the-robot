// A local lyric edit consumes existing notes in timeline order; it never moves,
// splits, deletes, or creates notes. Tokens come from the engine's lyricTokens
// API, not a second syllable-counting implementation in the UI.
import { pinnedLyric } from './lyric-text.js';

const hold = t => t === '_' || t === '+';
const explicit = t => t.startsWith('{') && t.endsWith('}');
const opens = t => t.endsWith('-') && !explicit(t);
const text = n => String(n.lyric ?? '').trim();
const byTime = (a, b) => a.start - b.start || a.pitch - b.pitch;

// labels must describe the current notes and lyrics, from a fresh engine plan.
// The caller checks its plan/version key before calling this helper and again
// before committing an asynchronous token request. Mirror the lyric pane using
// lyricsFromNotes AFTER the new plan arrives: the old labels cannot tell whether
// a new word needs a one-note ~ or whether a newly split word can be collapsed.
//
// Replacing part of an old word deliberately ends that word at both edit edges.
// Retained fragments are written as their already-sung explicit phonemes, so
// "remember-", "-", "" with only its middle note changed cannot turn into a
// different concatenated word. This uncommon partial-word edit shows {phonemes}
// in the lyric pane for the retained fragments; complete-word edits retain text.
// Held notes belonging to a touched word are pinned the same way at its edges.
// A dangling final hyphen in the entered text is closed at the edit edge.
export function applyNoteLyrics({ notes, noteId, tokens, labels, useNoteLyrics = false }) {
  if (!Array.isArray(notes) || !Array.isArray(tokens) || tokens.some(t => typeof t !== 'string'))
    return { error: 'The lyric edit was not valid.', code: 'invalid-edit' };
  const sorted = [...notes].sort(byTime);
  const start = sorted.findIndex(n => n._id === noteId);
  if (start < 0) return { error: 'This note is no longer in the melody.', code: 'missing-note' };
  const ids = new Set(notes.map(n => n._id));
  if (ids.size !== notes.length || notes.some(n => !labels?.get(n._id)))
    return { error: 'Wait for the current lyrics to finish assigning to notes.', code: 'missing-labels' };
  const incoming = tokens.length ? [...tokens] : ['{M}'];
  const available = sorted.length - start;
  if (incoming.length > available)
    return { error: `These lyrics need ${incoming.length} notes; only ${available} remain. Add notes or shorten the lyrics.`,
      code: 'not-enough-notes', required: incoming.length, available };
  // Do not silently join an unfinished entered word to an untouched next word.
  let lastWord = incoming.length - 1;
  while (lastWord >= 0 && hold(incoming[lastWord])) --lastWord;
  if (lastWord >= 0 && opens(incoming[lastWord])) incoming[lastWord] = incoming[lastWord].slice(0, -1) || '{M}';
  const end = start + incoming.length;
  const result = notes.map(n => ({ ...n, lyric: useNoteLyrics ? String(n.lyric ?? '') : pinnedLyric(labels.get(n._id)) }));
  const byId = new Map(result.map(n => [n._id, n]));
  const order = sorted.map(n => byId.get(n._id));

  // Group the original words, including their held notes. Even after a final
  // syllable closes a word, trailing holds still belong to that same word.
  const groups = [];
  let group = null, open = false;
  for (let i = 0; i < order.length; i++) {
    const t = text(order[i]);
    if (hold(t) && group) { group.push(i); continue; }
    if (!open) { group = []; groups.push(group); }
    group.push(i);
    open = opens(t);
  }
  const retainedFragments = [];
  for (const indices of groups) {
    if (!indices.some(i => i >= start && i < end) || !indices.some(i => i < start || i >= end)) continue;
    for (const i of indices) {
      if (i >= start && i < end) continue;
      const n = order[i], label = labels.get(n._id);
      // A hold label contains only its sustained vowel; a syllable label includes
      // onset and coda. Pinning removes the old word dependency without guessing.
      const phon = String(label.phon || label.vowel || 'M').replace(/\|/g, ' ').replace(/\s+/g, ' ').trim();
      n.lyric = `{${phon || 'M'}}`;
      retainedFragments.push(n._id);
    }
  }
  for (let i = 0; i < incoming.length; i++) order[start + i].lyric = incoming[i];
  return { notes: result, useNoteLyrics: true, changedIds: order.slice(start, end).map(n => n._id), retainedFragments };
}
