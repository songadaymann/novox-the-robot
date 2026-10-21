// What the editor says and does differently by platform. The plugin's status names the platform
// (status.platform: "mac", "windows" or "linux") and the files it imports (status.importTypes,
// plugin/Source/PluginEditor.cpp); a status without them is taken as a Mac's, as the text was
// written for macOS.

export const platformOf = (status) => (status?.platform === "windows" || status?.platform === "linux" ? status.platform : "mac");

// Mac key names in a text as this platform names them: "⌘Z undoes" → "Ctrl+Z undoes",
// "⌘ + scroll" → "Ctrl + scroll", "⌥-drag" → "Alt-drag", "⌫ deletes" → "Del deletes".
export function keyNames(text, platform) {
  if (platform === "mac") return text;
  return String(text).replace(/⌘ \+ /g, "Ctrl + ").replace(/⌘/g, "Ctrl+").replace(/⌥-/g, "Alt-").replace(/⌥/g, "Alt+")
    .replace(/⌫/g, "Del");
}

// A text that names Logic for any DAW ("drag into Logic", "Start playback in Logic"), with the DAW
// left unnamed away from the Mac. Text about Logic's own MMC setting stays as it is: it never
// goes through here.
export function dawNames(text, platform) {
  return platform === "mac" ? text : String(text).replace(/\b(in|into) Logic\b/g, "$1 your DAW");
}

// Static page text (menus, tooltips, help) as this platform says it.
export const platformText = (text, platform) => dawNames(keyNames(text, platform), platform);

// The files the plugin imports, by extension: audio it decodes and scores the engine reads.
// macOS's Core Audio also decodes m4a (AAC/ALAC); JUCE's own readers on Windows and Linux do not.
const MAC_AUDIO = ["wav", "mp3", "aif", "aiff", "m4a", "flac", "ogg"];
const SCORES = ["mid", "midi", "xml", "musicxml", "mxl"];
// Audio files people may try that the plugin cannot read away from the Mac.
const OTHER_AUDIO = ["m4a", "aac", "alac", "caf", "wma"];
export function importTypes(status) {
  const t = status?.importTypes, list = (v, fallback) => (Array.isArray(v) && v.length ? v.map((e) => String(e).toLowerCase()) : fallback);
  return { audio: list(t?.audio, MAC_AUDIO), score: list(t?.score, SCORES) };
}
// The file picker's accept list. On a Mac it also offers audio/* (Core Audio reads most of it);
// elsewhere audio/* would offer m4a and WMA files, which the plugin cannot read there.
export function importAccept(status) {
  const { audio, score } = importTypes(status);
  return [...audio, ...score].map((e) => "." + e).join(",") + (platformOf(status) === "mac" ? ",audio/*" : "");
}
const PLATFORM_NAMES = { windows: "Windows", linux: "Linux" };
// How the editor takes a file, by its name: "audio" (AudioImport), "file" (the engine's importer,
// which says if it cannot read it), or "refuse" with a message for an audio file the plugin cannot
// read on this platform.
export function importRoute(name, status) {
  const ext = /\.([^./\\]+)$/.exec(String(name))?.[1]?.toLowerCase() || "";
  const { audio } = importTypes(status), platform = platformOf(status);
  if (audio.includes(ext)) return { kind: "audio" };
  if (platform !== "mac" && OTHER_AUDIO.includes(ext)) {
    const formats = audio.filter((e) => e !== "aif").map((e) => (e === "ogg" ? "Ogg" : e.toUpperCase()));
    return { kind: "refuse", line: `.${ext} files can't be read on ${PLATFORM_NAMES[platform]} · use ${formats.slice(0, -1).join(", ")} or ${formats.at(-1)}` };
  }
  return { kind: "file" };
}

// What a ruler click in a DAW says instead of moving the DAW's playhead over MMC, or null when it
// can: GarageBand has no MMC input (every click says so, as it always has), and without Novox's
// MIDI port (status.mmc.port false: always on Windows, where a plugin cannot open one) a hint
// shown once.
export function playheadNotice(status) {
  if (platformOf(status) === "mac" && /garageband/i.test(status?.hostName || "")) return { line: "GarageBand's playhead can't be moved from a plugin" };
  if (status?.mmc?.port === false) return { line: "Move the playhead in your DAW; Novox follows it", once: true };
  return null;
}

// Space after the editor could not hand it to the DAW (hostSpace false): whether the page leaves
// the key press to the web view. A Mac's web view passes a key the page doesn't use on to the
// DAW's window. WebView2 (Windows) keeps it, so there letting it through would only do what a
// browser does with Space: press the focused button, tick a checkbox or scroll the piano roll. On a
// Mac too, only a press with nothing focused (the target is the page itself) goes through.
export function spaceToWebView(platform, target) {
  return platform === "mac" && (!target || target.tagName === "BODY" || target.tagName === "HTML");
}

// A key the web view would act on as a browser does: reload (which would lose Undo), print, find
// and page zoom. WebView2 (Windows) acts on them; macOS's web view does not. The editor uses none
// of these keys. Ctrl+Alt is AltGr on Windows, which types characters: those keys pass.
const BROWSER_KEYS = new Set(["r", "p", "f", "g", "+", "=", "-", "_", "0"]);
const BROWSER_CODES = new Set(["KeyR", "KeyP", "KeyF", "KeyG", "Equal", "Minus", "Digit0", "NumpadAdd", "NumpadSubtract", "Numpad0"]);
export function isBrowserKey(e, platform) {
  if (platform === "mac") return false;
  if (e.key === "F5" || e.key === "F3" || e.key === "BrowserRefresh") return true;
  if (!e.ctrlKey || e.altKey || e.metaKey) return false;
  return BROWSER_KEYS.has(String(e.key).toLowerCase()) || BROWSER_CODES.has(e.code);
}

// The line under the singing button. state: status.microphone; peak: the loudest level of this
// take so far. Windows lets an app open a microphone its privacy settings block, and it then
// records silence: after a few silent seconds the line says where the setting is.
export function singingMessage(state, peak, platform) {
  if (state.pending) return "Allow microphone access to record your singing.";
  const seconds = Math.floor(state.seconds || 0), time = `${seconds}s / 60s`;
  if (platform === "windows" && state.recording && seconds >= 3 && !(peak > 0))
    return `No sound from the microphone · check Settings > Privacy & security > Microphone · ${time}`;
  return `Sing, then click the singing icon to stop and convert · ${time}`;
}
