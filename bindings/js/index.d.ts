export type Voice = string | ({ preset?: string } & Record<string, number | string | number[][]>);

export interface Score {
  tempo?: number;
  units?: "beats" | "seconds";
  voice?: Voice;
  lyrics?: string;
  dictionary?: Record<string, string>;
  notes: Array<string | { pitch: string | number; dur: number; start?: number; lyric?: string }>;
}

/** What an imported file holds; beats are quarter notes from time 0 (the start of any pickup). */
export interface ImportInfo {
  format: "midi" | "musicxml";
  part: number;
  partVoice: string;
  /** The verse sung throughout, or 0 when verse N is sung on repeat pass N. */
  verse: number;
  /** The notes' lyrics are the file's own (the `lyrics` option was not used). */
  fileLyrics: boolean;
  repeats: boolean;
  unfolded: boolean;
  pickupBeats: number;
  tempoMap: { beat: number; seconds: number; bpm: number }[];
  timeSignatures: { beat: number; seconds: number; beats: number; beatType: number }[];
  parts: {
    index: number;
    id: string;
    name: string;
    notes: number;
    lyrics: number;
    verses: number[];
    voices: { voice: string; notes: number; lyrics: number }[];
  }[];
}

export interface ImportedScore extends Score {
  import: ImportInfo;
  importWarnings?: string[];
}

export interface Plan {
  lead: number;
  length: number;
  syllables: { start: number; end: number; text: string; phonemes: string; continues: boolean }[];
  segments: { label: string; kind: string; start: number; end: number }[];
  warnings: string[];
}

export class NovoxError extends Error {}

export class Stream {
  readonly length: number;
  readonly position: number;
  readonly lead: number;
  seek(sample: number): void;
  process(frames: number): Float32Array;
  destroy(): void;
}

export class Singer {
  readonly sampleRate: number;
  render(score: Score | string): Float32Array;
  plan(score: Score | string): Plan;
  phonemize(text: string): string;
  voices(): string[];
  addWord(word: string, arpabet: string): void;
  importFile(
    bytes: Uint8Array | ArrayBuffer,
    options?: { lyrics?: string; track?: number; voice?: string; partVoice?: string; verse?: number; unfold?: boolean },
  ): ImportedScore;
  stream(score: Score | string): Stream;
  toWav(samples: Float32Array, sampleRate?: number): Uint8Array;
  destroy(): void;
}

export function createSinger(options?: { sampleRate?: number; moduleOptions?: object }): Promise<Singer>;
