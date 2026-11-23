import crypto from "node:crypto";
import { AccountTier } from "./models.js";

export const ORIGIN = "https://gemini.google.com";
export const BATCH_PATH = "/_/BardChatUi/data/batchexecute";
export const GENERATE_PATH =
  "/_/BardChatUi/data/assistant.lamda.BardFrontendService/StreamGenerate";
export const UPLOAD_URL = "https://content-push.googleapis.com/upload";
export const ROTATE_URL = "https://accounts.google.com/RotateCookies";

export const RPC_USER_STATUS = "otAQ7b";
export const RPC_QUOTA = "qpEbW";
export const RPC_USAGE_PAGE = "jSf9Qc";

export const UPLOAD_AUTHORIZATION = "Basic c2F2ZXM6cyNMdGhlNmxzd2F2b0RsN3J1d1U=";
export const DEFAULT_PUSH_ID = "feeds/mcudyrk2a4khkz";
export const UPLOAD_FORM_TYPE = "application/x-www-form-urlencoded;charset=UTF-8";

export const MODEL_HEADER = "x-goog-ext-525001261-jspb";
export const SESSION_HEADER = "x-goog-ext-525005358-jspb";
export const USER_AGENT =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " +
  "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

export function urlEncode(value) {
  return encodeURIComponent(String(value)).replace(/[!'()*]/g, (c) =>
    "%" + c.charCodeAt(0).toString(16).toUpperCase()
  );
}

export function sapisidhashHeader(sapisid, origin = ORIGIN, timestamp = Math.floor(Date.now() / 1000)) {
  const digest = crypto.createHash("sha1").update(`${timestamp} ${sapisid} ${origin}`, "utf8").digest("hex");
  return `SAPISIDHASH ${timestamp}_${digest}`;
}

export function thinkingLevel(effort) {
  const key = String(effort || "").trim().toLowerCase();
  if (key === "high" || key === "extended") return 2;
  if (key === "medium") return 4;
  return 1;
}

export function scrapeToken(html, key) {
  const quoted = `"${key}"`;
  let pos = 0;
  while (true) {
    const hit = html.indexOf(quoted, pos);
    if (hit === -1) return "";
    let cursor = hit + quoted.length;
    while (cursor < html.length && ":,] \t\n\r".includes(html[cursor])) cursor++;
    if (cursor < html.length && html[cursor] === '"') {
      const start = cursor + 1;
      let end = start;
      while (end < html.length) {
        if (html[end] === "\\") {
          end += 2;
          continue;
        }
        if (html[end] === '"') break;
        end++;
      }
      if (end < html.length && end > start) return html.slice(start, end);
    }
    pos = hit + quoted.length;
  }
}

export function tierFromShellHtml(html) {
  if (!html) return AccountTier.UNKNOWN;
  const low = html.toLowerCase();
  if (
    low.includes('"gemini_ultra"') ||
    low.includes('"gemini-ultra"') ||
    low.includes("ultra_tier_enrollment") ||
    low.includes("gemini_ultra_tier")
  ) {
    return AccountTier.ULTRA;
  }
  if (
    low.includes('"gemini_advanced"') ||
    low.includes('"gemini-advanced"') ||
    low.includes("advanced_tier_enrollment") ||
    low.includes("gemini_advanced_tier") ||
    low.includes("advancedsubscriber")
  ) {
    return AccountTier.PRO;
  }
  if (
    low.includes("upgrade_to_advanced") ||
    low.includes("upgrade_to_gemini_advanced") ||
    low.includes("subscribe_to_advanced")
  ) {
    return AccountTier.FREE;
  }
  return AccountTier.UNKNOWN;
}

export function parseSessionFromShell(html) {
  const at = scrapeToken(html, "SNlM0e");
  const bl = scrapeToken(html, "cfb2h");
  const sid = scrapeToken(html, "FdrFJe");
  const hl = scrapeToken(html, "TuX5cc") || "en";
  const push = scrapeToken(html, "qKIAYe");
  const email = scrapeToken(html, "oPEP7c");
  const tier = tierFromShellHtml(html);
  const valid = Boolean(bl && sid);
  return { at, bl, sid, hl, push, email, tier, valid, authenticated: valid && Boolean(at || email) };
}

export function buildStreamGenerateInner({
  prompt,
  locale = "en",
  chat = null,
  requestUuid,
  modelNumber,
  level,
  fileIds = [],
  fileNames = [],
}) {
  const inner = new Array(81).fill(null);
  if (fileIds && fileIds.length) {
    const files = fileIds.map((fid, i) => [[fid], fileNames[i] || ""]);
    inner[0] = [prompt, 0, null, files, null, null, 0];
  } else {
    inner[0] = [prompt];
  }
  inner[1] = [locale || "en"];
  const active = Boolean(chat && chat.conversation && chat.response && chat.candidate);
  inner[2] = [
    active ? chat.conversation : "",
    active ? chat.response : "",
    active ? chat.candidate : "",
    null,
    null,
    null,
    null,
    null,
    null,
    "",
  ];
  inner[6] = [1];
  inner[7] = 1;
  inner[10] = 1;
  inner[11] = 0;
  inner[17] = [[0]];
  inner[18] = 0;
  inner[27] = 1;
  inner[30] = [4];
  inner[41] = [1];
  inner[53] = 0;
  inner[59] = requestUuid;
  inner[61] = [];
  inner[68] = 1;
  inner[79] = modelNumber;
  inner[80] = level;
  return JSON.stringify(inner);
}

export function buildStreamGenerateForm(innerPayload) {
  const wrapper = JSON.stringify([null, innerPayload]);
  return "f.req=" + urlEncode(wrapper);
}

// Finds the end index of a complete JSON array starting at `start` (string-aware).
function findArrayEnd(text, start) {
  if (text[start] !== "[") return -1;
  let depth = 0;
  let inString = false;
  for (let i = start; i < text.length; i++) {
    const c = text[i];
    if (inString) {
      if (c === "\\") {
        i++;
      } else if (c === '"') {
        inString = false;
      }
      continue;
    }
    if (c === '"') inString = true;
    else if (c === "[") depth++;
    else if (c === "]") {
      depth--;
      if (depth === 0) return i + 1;
    }
  }
  return -1;
}

function findRecord(value, rpcId, depth = 0) {
  if (depth > 8 || !Array.isArray(value)) return null;
  if (value[0] === "wrb.fr") {
    if (value[1] === rpcId && typeof value[2] === "string" && value[2]) return value[2];
    return null;
  }
  for (const item of value) {
    const hit = findRecord(item, rpcId, depth + 1);
    if (hit !== null) return hit;
  }
  return null;
}

export function findRpcPayload(body, rpcId) {
  let cursor = body.startsWith(")]}'") ? 4 : 0;
  while (cursor < body.length) {
    const start = body.indexOf("[", cursor);
    if (start === -1) return null;
    const end = findArrayEnd(body, start);
    if (end === -1) return null;
    cursor = end;
    try {
      const frame = JSON.parse(body.slice(start, end));
      const hit = findRecord(frame, rpcId);
      if (hit !== null) return hit;
    } catch {
      cursor = start + 1;
    }
  }
  return null;
}

function collectStrings(value, parts, depth = 0) {
  if (depth > 4) return;
  if (typeof value === "string") parts.push(value);
  else if (Array.isArray(value)) {
    for (const item of value) collectStrings(item, parts, depth + 1);
  }
}

function answerFromPayload(payload) {
  const first = Array.isArray(payload[4]) ? payload[4][0] : null;
  const slot = Array.isArray(first) ? first[1] : null;
  if (typeof slot === "string") return slot;
  if (Array.isArray(slot)) return slot.filter((p) => typeof p === "string").join("");
  return "";
}

function thinkingFromPayload(payload) {
  const first = Array.isArray(payload[4]) ? payload[4][0] : null;
  const thinking = Array.isArray(first) ? first[37] : null;
  if (thinking == null) return "";
  const parts = [];
  collectStrings(thinking, parts);
  return parts.join("");
}

export function applyReplyPayload(payload, turn) {
  if (Array.isArray(payload[1])) {
    if (typeof payload[1][0] === "string" && payload[1][0]) turn.chat.conversation = payload[1][0];
    if (typeof payload[1][1] === "string" && payload[1][1]) turn.chat.response = payload[1][1];
  }
  if (Array.isArray(payload[4]) && Array.isArray(payload[4][0])) {
    if (typeof payload[4][0][0] === "string" && payload[4][0][0]) {
      turn.chat.candidate = payload[4][0][0];
    }
  }
  const thinking = thinkingFromPayload(payload);
  if (thinking.length > turn.thinking.length) turn.thinking = thinking;

  const prevLen = turn.answer.length;
  const candidate = answerFromPayload(payload);
  if (candidate && candidate.length >= prevLen) {
    turn.answer = candidate;
    if (typeof payload[42] === "string" && payload[42]) turn.servedLabel = payload[42];
    if (typeof payload[58] === "number" && payload[58] > 0) turn.servedNumber = payload[58];
  }
  return turn.answer.length > prevLen;
}

export class ReplyStreamScanner {
  constructor() {
    this._buf = "";
    this._cursor = 0;
    this.sawFrame = false;
    this.turn = {
      answer: "",
      thinking: "",
      servedLabel: "",
      servedNumber: 0,
      chat: { conversation: "", response: "", candidate: "" },
    };
  }

  feed(chunk) {
    this._buf += chunk;
    let grew = false;
    while (true) {
      if (this._cursor > 65536 && this._cursor > this._buf.length / 2) {
        this._buf = this._buf.slice(this._cursor);
        this._cursor = 0;
      }
      const start = this._buf.indexOf("[", this._cursor);
      if (start === -1) {
        this._cursor = this._buf.length;
        break;
      }
      this._cursor = start;
      const end = findArrayEnd(this._buf, start);
      if (end === -1) break;
      let frame;
      try {
        frame = JSON.parse(this._buf.slice(start, end));
      } catch {
        this._cursor = start + 1;
        continue;
      }
      this._cursor = end;
      if (!Array.isArray(frame)) continue;
      for (const record of frame) {
        if (!Array.isArray(record) || record[0] !== "wrb.fr" || typeof record[2] !== "string") continue;
        let payload;
        try {
          payload = JSON.parse(record[2]);
        } catch {
          continue;
        }
        if (Array.isArray(payload)) {
          this.sawFrame = true;
          if (applyReplyPayload(payload, this.turn)) grew = true;
        }
      }
    }
    return grew;
  }
}

export function extractReply(body) {
  const scanner = new ReplyStreamScanner();
  scanner.feed(body);
  return scanner.turn;
}

export function stripCitations(text) {
  return String(text || "")
    .replace(/\[cite:[^\]]*\]/g, "")
    .replaceAll("[cite_start]", "")
    .replaceAll("?code_reference", "")
    .replaceAll("?code_stdout", "");
}

export function parseBardError(body) {
  const idx = body.indexOf("BardErrorInfo");
  if (idx === -1) return null;
  const match = /(\d+)/.exec(body.slice(idx));
  if (!match) return null;
  const code = Number(match[1]);
  const after = idx + match.index + match[0].length;
  const q1 = body.indexOf('"', after);
  let message = "";
  if (q1 !== -1) {
    const q2 = body.indexOf('"', q1 + 1);
    if (q2 > q1 + 1) message = body.slice(q1 + 1, q2);
  }
  return { code, message };
}

export function isSiteApology(text) {
  const trimmed = String(text || "").trim();
  if (!trimmed || trimmed.length > 320) return false;
  const low = trimmed.toLowerCase();
  if ((low.startsWith("sorry") || low.includes("sorry,")) && low.includes("try your request again")) {
    return true;
  }
  return low.startsWith("i encountered an error doing what you asked");
}

export function isSiteModelError(text) {
  const trimmed = String(text || "").trim();
  if (!trimmed || trimmed.length > 160) return false;
  return trimmed.toLowerCase().startsWith("i seem to be encountering an error");
}

export function isTransientError(error) {
  const low = String(error || "").toLowerCase();
  if (!low || low.includes("auth rejected (401)")) return false;
  return [
    "the site failed",
    "something went wrong",
    "try your request again",
    "code 1013",
    "code 1052",
    "http 429",
    "http 500",
    "http 502",
    "http 503",
    "http 504",
    "timed out",
    "timeout",
    "overloaded",
    "temporarily",
    "unavailable",
  ].some((needle) => low.includes(needle));
}
