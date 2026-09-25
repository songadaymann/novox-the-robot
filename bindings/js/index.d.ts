export type Voice = string | ({ preset?: string } & Record<string, number | string | number[][]>);

export interface Score {
  tempo?: number;
  units?: "beats" | "seconds";
  voice?: Voice;
  lyrics?: string;
  dictionary?: Record<string, string>;
  notes: Array<string | { pitch: string | number; dur: number; start?: number; lyric?: string }>;
}

export interface Plan {
  lead: number;
  length: number;
  syllables: { start: number; end: number; text: string; phonemes: string; continues: boolean }[];
  segments: { label: string; kind: string; start: number; end: number }[];
  warnings: string[];
}

export class NvsError extends Error {}

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
  importFile(bytes: Uint8Array | ArrayBuffer, options?: { lyrics?: string; track?: number; voice?: string }): Score;
  stream(score: Score | string): Stream;
  toWav(samples: Float32Array, sampleRate?: number): Uint8Array;
  destroy(): void;
}

export function createSinger(options?: { sampleRate?: number; moduleOptions?: object }): Promise<Singer>;
