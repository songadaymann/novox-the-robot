# Feature roadmap

Work through these in order, keeping standalone and plugin behavior together.

- [x] Original-audio comparison: aligned waveform, Original/Robot listening, and preserved source recording across reopening.
- [x] Phrase looping: audition selected notes repeatedly while editing, with original/robot comparison.
- [ ] Drag finished audio and MIDI out into Logic; export corrected MIDI and current sung audio.
- [x] Syllable editing: split words into syllables, join fragments, and move lyrics between notes without shifting later words.
- [x] Standalone song files: Save, Save As, Open; WAV and MIDI export.
- [ ] Record into the song: capture from the playhead or replace a selected phrase while preserving the rest.
- [ ] Per-note expression: volume, breath and vibrato with clear visual editing.
- [ ] Multiple singer parts: independent voices and harmonies.
- [ ] Adjustable piano-roll height: let the user make the piano-roll area taller or shorter within the app, independently of note zoom and overall window size; keep standalone and plugin behavior consistent.

## Current work

- [x] Aligned source waveform and Original/Robot/Stop controls for a fresh import.
- [x] Native phrase audition and looping using selected notes.
- [x] Preserve source audio across editor/project reopening.
- [x] Region-by-region import (Add to song): each recording joins the song at its position and replaces only the notes within its length; every import's source audio is kept.
- [ ] Verify the comparison and editing flow in live Logic.

Implemented comparison, looping, syllable editing and song files. Source audio is
kept separately in the processor and included only in saved state / .novox files;
normal note edits carry just its ID. Older plain-song project states remain readable.
Every source the song's imports refer to is embedded when saving, each once; imports
that Undo took back (kept for session Redo) are not embedded in the project.

Export WAV/MIDI and native Drag WAV/Drag MIDI controls are implemented. MIDI includes
tempo, note timing and per-note lyrics. WAV is 24-bit mono, aligned to timeline zero
by removing renderer preroll (a consonant extending before zero is therefore trimmed).
Dragged exports stay at stable paths in Application Support/Novox/Exports because
Logic may reference the file. The drag-out parent remains unchecked pending live Logic
verification; normal file exports provide the fallback.

Syllable Split divides one selected note into equal segments, each with its own token;
resize their edges for timing. Join notes merges adjacent selected notes of one pitch,
joining hyphenated syllables while retaining spaces between whole words. Lyric arrows
swap a token with its neighboring note. Undo restores all these operations.

Next: record at the playhead / replace a phrase, then per-note expression and singer parts.
Live Logic drag/drop and file-dialog end-to-end checks remain separate verification work.
