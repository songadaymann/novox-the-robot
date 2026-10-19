# Audio import delivery

- [x] Native WAV/MP3 decode, background pitch + local whisper.cpp lyrics.
- [x] First-use model download consent; error/cancellation and bounded inputs.
- [x] Convert directly into existing song editor with per-note lyrics, current tempo, Undo and LCD progress.
- [x] Shared Finder drop/picker flow in standalone and AU; native file-drop fallback.
- [x] Build, engine/plugin tests, actual recording through native worker, review UI verification.
- [ ] Logic timeline region drops: host test needed; do not promise trim/bounce semantics.

No Python or ffmpeg runtime dependency. English lyrics model is cached in Application Support/Novox.
Existing MIDI/MusicXML imports remain. The song changes when conversion completes; Undo restores the previous song.
