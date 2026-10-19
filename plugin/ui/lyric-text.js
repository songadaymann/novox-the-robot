// Notes <-> lyric text for the editor. Pure functions with no DOM, so the node tests can run the
// same code the editor runs (plugin/Tests/LyricTextTests.mjs, plugin/Tests/LyricRoundTrip.mjs).
//
// The lyric box flows text onto notes the way the engine does (engine/src/planner.cpp flowLyrics):
// whitespace separates words, "hel-lo" puts each piece on its own note, "_" holds the previous
// syllable over the next note, a word without hyphens takes one note per vowel, "wanna~" keeps a
// word on one note, and a lone "~" leaves a note without a lyric.

// A rest at least this long (in beats) starts a new line of lyric text. Line breaks are only
// whitespace to the lyric flow, so they change how the text reads, not where syllables land.
// Shorter gaps are mostly stop consonants inside a phrase, so a capitalized word (the recognizer
// capitalizes sentence starts) or a sentence mark also starts a line.
export const PHRASE_GAP_BEATS = 1;
// A line holds at least this many words before a capital letter or a sentence mark may end it.
const MIN_LINE_WORDS = 3;

// The note span (beats) within which a plan syllable counts as starting on a note.
const LABEL_TOLERANCE_BEATS = 0.04;
// The engine's vowel phonemes (engine/src/phonemes.cpp): a word with two or more of them takes that
// many notes in the lyric flow unless it is marked to stay on one.
const VOWELS = new Set(["IY", "IH", "EH", "AE", "AA", "AO", "UH", "UW", "AH", "AX", "ER", "EY", "AY", "AW", "OW", "OY"]);
const HUM = "mm";          // a lyric-less note; the engine sings it as a plain hum
const NO_LYRIC = "~";      // a lyric-less note inside a word (the engine's one-note mark, alone)
const ONE_NOTE = "~";      // appended to a word sung on one note that the flow would split

const isHold = (t) => t === "_" || t === "+";
const isExplicit = (t) => t.length >= 2 && t[0] === "{" && t[t.length - 1] === "}";
const opensWord = (t) => t.endsWith("-") && !isExplicit(t);
const phonemeList = (s) => String(s?.phonemes || "").split(/[\s|]+/).filter(Boolean);
const vowelCount = (s) => phonemeList(s).filter((p) => VOWELS.has(p.replace(/\d/g, ""))).length;
// "mm", "Mmm" or "{M}" sung as a plain hum shows as a hum, the same as a note with no lyric.
const humLike = (s) => /^\{?\s*m+\s*\}?$/i.test(s.text) && phonemeList(s).join(" ") === "M";

// What each note sings, from the engine's plan (seconds at planBpm): a Map from note _id to
// {kind: "word" | "cont" | "hold" | "hum", text, syl, phon, vowel}, or null without a plan.
// "cont" is a later note of a word the flow split by its vowels; "hold" continues the syllable
// before it ("_").
export function labelNotes(notes, plan, planBpm) {
  const labels = new Map();
  const syls = (plan?.syllables || []).map((s) => ({ ...s, b0: (s.start * planBpm) / 60, b1: (s.end * planBpm) / 60 }));
  const tol = LABEL_TOLERANCE_BEATS;
  for (const n of notes) {
    if (!plan) { labels.set(n._id, null); continue; }
    let best = -1, bestD = tol;
    for (let i = 0; i < syls.length; i++) {
      const d = Math.abs(syls[i].b0 - n.start);
      if (d < bestD) { bestD = d; best = i; }
      if (syls[i].b0 > n.start + tol) break;
    }
    if (best >= 0) {
      const s = syls[best], prev = syls[best - 1];
      const ph = String(s.phonemes || "");
      const phon = ph.replace(/\|/g, " ").replace(/\s+/g, " ").trim();
      const vowel = (ph.split("|")[1] || "").trim().split(" ")[0];
      if (s.text && !humLike(s)) labels.set(n._id, { kind: "word", text: s.text + (s.continues ? "-" : ""), syl: s, phon, vowel });
      else if (!s.text && prev && prev.continues) labels.set(n._id, { kind: "cont", text: "-" + phon.toLowerCase().replace(/ /g, ""), syl: s, phon, vowel });
      else labels.set(n._id, { kind: "hum", text: "mm", syl: s, phon: phon || "M", vowel: "M" });
      continue;
    }
    const s = syls.find((x) => x.b0 < n.start + tol && x.b1 > n.start + tol);
    if (s) {
      const vowel = (String(s.phonemes || "").split("|")[1] || "").trim().split(" ")[0];
      labels.set(n._id, { kind: "hold", text: "_", syl: s, phon: vowel, vowel });
    } else {
      labels.set(n._id, { kind: "hum", text: "mm", syl: null, phon: "M", vowel: "M" });
    }
  }
  return labels;
}

// The lyric a note keeps when the editor pins flowing lyrics onto notes (before splitting or
// joining syllables). A later note of a split word keeps no text of its own: "-" while the word
// goes on, "" on its last note, which is how the engine reads the rest of a word.
export function pinnedLyric(label) {
  if (!label) return "";
  if (label.kind === "hum") return "{M}";
  if (label.kind === "cont") return label.syl?.continues ? "-" : "";
  return String(label.text || "");
}

// Lyric text for notes that carry their own lyrics (audio import, MIDI import, pinned lyrics),
// written so the lyric flow gives every note back the same lyric. Hyphenated pieces stay
// together ("hel-lo"), "_" stands for each held note, a lyric-less note reads "mm" (or "~" inside
// a word), and a new line starts after a rest of phraseGap beats or more, or at a capitalized word or
// after a sentence mark once the line has a few words.
//
// labels (from labelNotes for these notes) tell which one-note words have two or more vowels:
// those get "~" so the flow keeps them on one note ("wanna~"). Without labels nothing is marked.
// A word the flow itself split by vowels and the editor then pinned ("singer-" then "") is
// written back as the plain word.
//
// emptyNote: "hum" for notes the importer left without lyrics; "hold" where a missing lyric means
// the syllable goes on (MIDI files leave melisma notes without lyric events).
// Returns null when no note has a lyric.
export function lyricsFromNotes(notes, { labels = null, phraseGap = PHRASE_GAP_BEATS, emptyNote = "hum" } = {}) {
  const sorted = [...notes].sort((a, b) => a.start - b.start || a.pitch - b.pitch);
  const toks = sorted.map((n) => String(n.lyric ?? "").trim());
  if (!toks.some((t) => t && !isHold(t))) return null;
  const label = (i) => (labels && sorted[i]._id !== undefined ? labels.get(sorted[i]._id) : null);

  // One text token per note; each must flow back onto exactly one note.
  const words = [];
  let open = false; // the last token that is not a hold left its word open ("hel-")
  for (let i = 0; i < toks.length; i++) {
    let t = toks[i];
    if (!t) t = emptyNote === "hold" ? "_" : open ? NO_LYRIC : HUM;
    else if (!open && /^\{\s*M\s*\}$/i.test(t)) t = HUM; // a pinned hum reads like the others
    else if (!isHold(t) && !isExplicit(t)) {
      // Whitespace or an inner hyphen would split one note's lyric over several notes.
      const body = t.replace(/-+$/, ""), tail = t.slice(body.length) ? "-" : "";
      if (/[\s-]/.test(body)) t = body.replace(/[\s-]+/g, ONE_NOTE) + tail;
      if (!open && tail && !/[\s-]/.test(body)) {
        const auto = autoSplitWord(sorted, toks, labels, i);
        if (auto > 0) { words.push({ i, text: body }); i += auto - 1; continue; }
      } else if (!open && !t.includes(ONE_NOTE)) {
        const l = label(i);
        if (l?.kind === "word" && l.text === t && vowelCount(l.syl) > 1) t += ONE_NOTE;
      }
    }
    words.push({ i, text: t });
    if (!isHold(t)) open = opensWord(t);
  }
  // A text that runs out leaves the remaining notes without lyrics, which is what trailing hums are.
  while (words.length && (words[words.length - 1].text === HUM || words[words.length - 1].text === NO_LYRIC)) words.pop();

  let out = "", lineWords = 0, sentenceEnd = false;
  words.forEach((w, k) => {
    const newWord = !isHold(w.text) && !(k && wordOpenBefore(words, k));
    if (!k) { out = w.text; lineWords = 1; }
    else if (opensWord(words[k - 1].text) && !isHold(w.text) && w.text !== NO_LYRIC && !isExplicit(w.text)) out += w.text; // hel-lo
    else {
      const before = sorted[w.i - 1];
      const rest = sorted[w.i].start - (before.start + before.dur);
      const capital = /^\p{Lu}/u.test(w.text) && !/^I(['’]|$)/.test(w.text);
      const breakLine = newWord && (rest >= phraseGap || (lineWords >= MIN_LINE_WORDS && (capital || sentenceEnd)));
      out += (breakLine ? "\n" : " ") + w.text;
      if (breakLine) lineWords = 0;
      if (newWord) lineWords++;
    }
    if (!isHold(w.text)) sentenceEnd = /[.!?;:]["')\]]*$/.test(w.text);
  });
  return out;
}

// Whether the word before token k is still open (looking past holds), i.e. token k is its tail.
function wordOpenBefore(words, k) {
  for (let j = k - 1; j >= 0; j--) if (!isHold(words[j].text)) return opensWord(words[j].text);
  return false;
}

// For a pinned word the flow had split by vowels ("singer-", then "-" while it goes on, then ""),
// how many notes it covers when its plain spelling flows back onto exactly those notes (each of
// its syllables has one vowel); 0 otherwise.
function autoSplitWord(notes, toks, labels, i) {
  if (!labels || !opensWord(toks[i])) return 0;
  let j = i + 1;
  while (j < toks.length && toks[j] === "-") j++;
  if (j >= toks.length || toks[j] !== "") return 0;
  const first = labels.get(notes[i]._id);
  if (first?.kind !== "word" || first.text !== toks[i]) return 0;
  for (let k = i; k <= j; k++) {
    const l = labels.get(notes[k]._id);
    if (!l || (k > i && l.kind !== "cont") || vowelCount(l.syl) !== 1) return 0;
  }
  return j - i + 1;
}
