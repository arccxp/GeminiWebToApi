import crypto from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";

import { CookieJar, headerCookie, parseCookieHeader } from "./cookies.js";
import {
  AccountTier,
  buildModelHeader,
  fallbackModelTable,
  isUltraOnlyModel,
  modelAllowedOnPlan,
  modelBaseKey,
  parseModelsFromPayload,
  pickModel,
  productModelName,
} from "./models.js";
import {
  BATCH_PATH,
  DEFAULT_PUSH_ID,
  GENERATE_PATH,
  MODEL_HEADER,
  ORIGIN,
  ROTATE_URL,
  RPC_QUOTA,
  RPC_USAGE_PAGE,
  RPC_USER_STATUS,
  SESSION_HEADER,
  UPLOAD_AUTHORIZATION,
  UPLOAD_FORM_TYPE,
  UPLOAD_URL,
  USER_AGENT,
  ReplyStreamScanner,
  buildStreamGenerateForm,
  buildStreamGenerateInner,
  extractReply,
  findRpcPayload,
  isSiteApology,
  isSiteModelError,
  isTransientError,
  parseBardError,
  parseSessionFromShell,
  sapisidhashHeader,
  stripCitations,
  thinkingLevel,
  urlEncode,
} from "./protocol.js";
import { classifyQuotaRows, parseQuotaRows, parseUsagePagePayload } from "./usage.js";

export const DEFAULT_MODEL = "gemini-auto";
const SESSION_TTL_MS = 45 * 60 * 1000;
const MODEL_TTL_MS = 30 * 60 * 1000;

export class GeminiWebError extends Error {
  constructor(message) {
    super(message);
    this.name = "GeminiWebError";
  }
}

export class AuthRejectedError extends GeminiWebError {
  constructor(message = "Gemini Web: auth rejected (401).") {
    super(message);
    this.name = "AuthRejectedError";
  }
}

function guessMime(filename) {
  const low = String(filename || "").toLowerCase();
  if (low.endsWith(".png")) return "image/png";
  if (low.endsWith(".webp")) return "image/webp";
  if (low.endsWith(".gif")) return "image/gif";
  if (low.endsWith(".pdf")) return "application/pdf";
  return "image/jpeg";
}

async function defaultTransport({ method, url, headers, body, timeoutMs, onChunk }) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, {
      method,
      headers,
      body: method === "GET" || method === "HEAD" ? undefined : body,
      signal: controller.signal,
    });
    const setCookies =
      typeof res.headers.getSetCookie === "function"
        ? res.headers.getSetCookie()
        : res.headers.get("set-cookie")
          ? [res.headers.get("set-cookie")]
          : [];
    const headerMap = {};
    res.headers.forEach((v, k) => {
      headerMap[k.toLowerCase()] = v;
    });

    if (onChunk && res.body) {
      const reader = res.body.getReader();
      const decoder = new TextDecoder("utf-8");
      let full = "";
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        const text = decoder.decode(value, { stream: true });
        if (text) {
          full += text;
          onChunk(text);
        }
      }
      const tail = decoder.decode();
      if (tail) {
        full += tail;
        onChunk(tail);
      }
      return { status: res.status, body: full, headers: headerMap, setCookies };
    }

    const text = await res.text();
    return { status: res.status, body: text, headers: headerMap, setCookies };
  } finally {
    clearTimeout(timer);
  }
}

export class GeminiWebClient {
  constructor(
    cookies,
    {
      account = 0,
      timeoutMs = 120_000,
      generateTimeoutMs = 300_000,
      allowUltra = false,
      onCookieUpdate = null,
      transport = null,
    } = {}
  ) {
    this.jar = cookies instanceof CookieJar ? cookies : parseCookieHeader(cookies, account);
    if (account) this.jar.account = Math.max(0, account);
    if (this.jar.account > 0 && !this.jar.hashKey(this.jar.account)) {
      const slot = this.jar.account + 1;
      throw new Error(`Google slot ${slot} is missing __Secure-${slot}PAPISID`);
    }

    this.timeoutMs = timeoutMs;
    this.generateTimeoutMs = generateTimeoutMs;
    this.allowUltra = allowUltra;
    this.onCookieUpdate = onCookieUpdate;
    this._transport = transport || defaultTransport;

    this._session = null;
    this._sessionAt = 0;
    this._models = [];
    this._modelsAt = 0;
    this._tier = AccountTier.UNKNOWN;
    this._generationId = crypto.randomUUID().toUpperCase();
    this._reqid = 100_000 + Math.floor(Math.random() * 800_000);
    this._rotateAfter = 0;
    this.thread = { conversation: "", response: "", candidate: "" };
  }

  get liveCookies() {
    return this.jar.liveHeader;
  }

  resetChat() {
    this.thread = { conversation: "", response: "", candidate: "" };
  }

  resetSession() {
    this._session = null;
    this._sessionAt = 0;
    this._models = [];
    this._modelsAt = 0;
  }

  _accountPrefix() {
    return this.jar.account > 0 ? `/u/${this.jar.account}` : "";
  }

  _nextReqid() {
    const val = this._reqid;
    this._reqid += 100_000;
    return val;
  }

  _authHeaders() {
    const slot = Math.max(0, this.jar.account);
    const key = this.jar.hashKey(slot);
    return {
      Authorization: sapisidhashHeader(key, ORIGIN),
      "X-Goog-AuthUser": String(slot),
      Origin: ORIGIN,
      Referer: `${ORIGIN}${this._accountPrefix()}/app`,
      "User-Agent": USER_AGENT,
      "Accept-Language": "en-US,en;q=0.9",
      Cookie: this.jar.scopedHeader(false),
    };
  }

  _requestHeaders(modelHeaderVal, sessionUuid) {
    return {
      ...this._authHeaders(),
      "Content-Type": "application/x-www-form-urlencoded;charset=utf-8",
      "X-Same-Domain": "1",
      [MODEL_HEADER]: modelHeaderVal,
      [SESSION_HEADER]: `["${sessionUuid}",1]`,
      "X-Goog-Ext-73010989-jspb": "[0]",
      "X-Goog-Ext-73010990-jspb": "[0,0,0]",
    };
  }

  _absorb(res, { fromAccounts = false } = {}) {
    const updated = this.jar.absorbSetCookies(res.setCookies || [], { fromAccounts });
    if (updated && typeof this.onCookieUpdate === "function") {
      this.onCookieUpdate(this.jar.account, updated);
    }
  }

  _replaceField(text, name, value) {
    const parts = text.split("&").filter((p) => p && p.split("=", 1)[0] !== name);
    if (value) parts.push(`${name}=${urlEncode(value)}`);
    return parts.join("&");
  }

  async _fetchShellRaw(timeoutMs = 30_000) {
    const headers = { ...this._authHeaders(), Accept: "text/html,application/xhtml+xml" };
    const res = await this._transport({
      method: "GET",
      url: `${ORIGIN}${this._accountPrefix()}/app`,
      headers,
      timeoutMs,
    });
    if (res.status < 200 || res.status >= 300) return null;
    const session = parseSessionFromShell(res.body);
    if (!session.authenticated) return null;
    this._absorb(res);
    return session;
  }

  async _authedRequest({ method, url, headers, body, timeoutMs, onChunk, onReset }) {
    const res = await this._transport({ method, url, headers, body, timeoutMs, onChunk });
    const rejected = res.status === 401 || res.status === 403;
    const shellGet = method === "GET" && url.endsWith(`${this._accountPrefix()}/app`);
    const missingShell = shellGet && res.status === 200 && !parseSessionFromShell(res.body).authenticated;

    if (!rejected && !missingShell) {
      if (res.status >= 200 && res.status < 300) this._absorb(res);
      return res;
    }

    if (!(await this.rotateCookies({ force: true }))) return res;
    const freshShell = await this._fetchShellRaw(20_000);
    if (!freshShell || !freshShell.authenticated) return res;

    this._session = freshShell;
    this._sessionAt = Date.now();
    if (shellGet) return { status: 200, body: res.body, headers: res.headers, setCookies: [] };

    const retryHeaders = {
      ...headers,
      Authorization: sapisidhashHeader(this.jar.hashKey(this.jar.account), ORIGIN),
      Cookie: this.jar.scopedHeader(false),
    };
    let retryUrl = url;
    let retryBody = body;
    if (method === "POST" && url.includes("?")) {
      const qIdx = url.indexOf("?");
      const base = url.slice(0, qIdx);
      let q = url.slice(qIdx + 1);
      q = this._replaceField(q, "bl", freshShell.bl);
      q = this._replaceField(q, "f.sid", freshShell.sid);
      q = this._replaceField(q, "at", freshShell.at);
      retryUrl = `${base}?${q}`;
      if (typeof body === "string") {
        retryBody = this._replaceField(body, "at", freshShell.at);
      }
    }
    if (onReset) onReset();
    const replay = await this._transport({
      method,
      url: retryUrl,
      headers: retryHeaders,
      body: retryBody,
      timeoutMs,
      onChunk,
    });
    if (replay.status >= 200 && replay.status < 300) this._absorb(replay);
    return replay;
  }

  async getSession({ force = false } = {}) {
    if (!force && this._session && Date.now() - this._sessionAt < SESSION_TTL_MS) {
      return this._session;
    }
    const headers = { ...this._authHeaders(), Accept: "text/html,application/xhtml+xml" };
    const res = await this._authedRequest({
      method: "GET",
      url: `${ORIGIN}${this._accountPrefix()}/app`,
      headers,
      timeoutMs: Math.min(this.timeoutMs, 30_000),
    });
    if (res.status === 401 || res.status === 403) throw new AuthRejectedError();
    if (res.status < 200 || res.status >= 300) {
      throw new GeminiWebError(`Gemini Web: /app returned HTTP ${res.status}`);
    }
    if (this._session && this._session.authenticated && Date.now() - this._sessionAt < 5000) {
      return this._session;
    }
    const session = parseSessionFromShell(res.body);
    if (!session.authenticated) {
      throw new AuthRejectedError("Gemini Web: signed-in shell unavailable (cookies expired or signed out).");
    }
    this._session = session;
    this._sessionAt = Date.now();
    if (session.email) this.jar.email = session.email;
    return session;
  }

  async rotateCookies({ force = false } = {}) {
    const now = Date.now();
    if (!force && now < this._rotateAfter) return false;
    this._rotateAfter = now + 5 * 60 * 1000;

    const slot = Math.max(0, this.jar.account);
    let psid = headerCookie(this.jar.liveHeader, `__Secure-${slot + 1}PSID`);
    if (!psid && slot === 0) psid = this.jar.hashKey(0);
    if (!psid) return false;

    const headers = {
      "X-Goog-AuthUser": String(slot),
      "User-Agent": USER_AGENT,
      "Accept-Language": "en-US,en;q=0.9",
      "Content-Type": "application/json",
      Origin: "https://accounts.google.com",
      Referer: "https://accounts.google.com/",
      Cookie: this.jar.scopedHeader(true),
    };
    let res = await this._transport({
      method: "POST",
      url: ROTATE_URL,
      headers,
      body: '[000,"-0000000000000000000"]',
      timeoutMs: 30_000,
    });
    if (res.status === 400 || res.status === 415) {
      res = await this._transport({
        method: "POST",
        url: ROTATE_URL,
        headers,
        body: '["ChromiumBrowser",null,null,null,0,"https://accounts.google.com",null,null,null,0]',
        timeoutMs: 30_000,
      });
    }
    this._rotateAfter = Date.now() + (res.status === 429 ? 15 * 60 * 1000 : 60_000);
    if (res.status < 200 || res.status >= 300) return false;
    this._absorb(res, { fromAccounts: true });
    return true;
  }

  _buildBatchQuery(rpcId, sourcePath, session) {
    let q =
      `${this._accountPrefix()}${BATCH_PATH}?rpcids=${rpcId}` +
      `&source-path=${urlEncode(this._accountPrefix() + sourcePath)}` +
      `&hl=${urlEncode(session.hl)}&rt=c`;
    if (session.at) q += `&at=${urlEncode(session.at)}`;
    if (session.sid) q += `&f.sid=${urlEncode(session.sid)}`;
    if (session.bl) q += `&bl=${urlEncode(session.bl)}`;
    return q;
  }

  async _loadModelTable(session, force = false) {
    if (!force && this._models.length && Date.now() - this._modelsAt < MODEL_TTL_MS) {
      return { entries: [...this._models], tier: this._tier };
    }
    const query = this._buildBatchQuery(RPC_USER_STATUS, "/app", session);
    const reqJson = `[[["${RPC_USER_STATUS}","[null,null,null,null,null,null,null,null,null,null]",null,"generic"]]]`;
    let body = `f.req=${urlEncode(reqJson)}`;
    if (session.at) body = `at=${urlEncode(session.at)}&${body}`;

    const headers = this._requestHeaders("[]", crypto.randomUUID().toUpperCase());
    const res = await this._authedRequest({
      method: "POST",
      url: `${ORIGIN}${query}`,
      headers,
      body,
      timeoutMs: 15_000,
    });
    if (res.status === 401 || res.status === 403) {
      this.resetSession();
      throw new AuthRejectedError();
    }
    if (res.status < 200 || res.status >= 300) {
      return { entries: fallbackModelTable(), tier: session.tier };
    }
    const payload = findRpcPayload(res.body, RPC_USER_STATUS);
    if (!payload) return { entries: fallbackModelTable(), tier: session.tier };
    let { models: entries, tier } = parseModelsFromPayload(payload);
    if (!entries.length) return { entries: fallbackModelTable(), tier: session.tier };
    if (tier === AccountTier.UNKNOWN) {
      tier = entries.length > 1 ? AccountTier.PRO : AccountTier.FREE;
    }
    this._models = entries;
    this._modelsAt = Date.now();
    this._tier = tier;
    if (tier >= AccountTier.ULTRA) this.allowUltra = true;
    return { entries: [...entries], tier };
  }

  async listModels({ force = true } = {}) {
    const session = await this.getSession();
    let { entries, tier } = await this._loadModelTable(session, force);
    if (tier === AccountTier.UNKNOWN && session.tier >= AccountTier.FREE) tier = session.tier;
    const allowUltra = this.allowUltra || tier >= AccountTier.ULTRA;

    const models = [];
    const seen = new Map();
    for (const e of entries) {
      if (!allowUltra && isUltraOnlyModel(e.id, e.category, e.label)) continue;
      const id = e.id || (e.number > 0 ? String(e.number) : "");
      const label = e.label || e.category;
      const name = productModelName(label, e.category, e.number);
      const key = modelBaseKey(label, e.category, e.number);
      if (seen.has(key)) {
        const existing = models[seen.get(key)];
        if (label.length > existing.label.length) existing.label = label;
        if (!existing.id) existing.id = id;
        continue;
      }
      seen.set(key, models.length);
      models.push({ id, label, name, available: e.available, note: e.note });
    }
    return { models, tier, email: session.email };
  }

  async uploadImage(source, { filename = null, mimeType = null, pushId = null } = {}) {
    let rawBytes;
    let fname;
    if (Buffer.isBuffer(source) || source instanceof Uint8Array) {
      rawBytes = Buffer.from(source);
      fname = filename || "image.jpg";
    } else {
      const filePath = String(source);
      rawBytes = await fs.readFile(filePath);
      fname = filename || path.basename(filePath);
    }
    if (!rawBytes.length) throw new GeminiWebError("Cannot upload empty file");
    const mime = mimeType || guessMime(fname);
    const size = rawBytes.length;
    const effectivePush = pushId || this._session?.push || DEFAULT_PUSH_ID;

    const makeHeaders = (command, contentType) => {
      const h = {
        Accept: "*/*",
        Authorization: UPLOAD_AUTHORIZATION,
        "Content-Type": contentType,
        Origin: ORIGIN,
        Referer: `${ORIGIN}/`,
        "Push-ID": effectivePush,
        "X-Goog-Upload-Header-Content-Length": String(size),
        "X-Goog-Upload-Protocol": "resumable",
        "X-Tenant-Id": "bard-storage",
        "User-Agent": USER_AGENT,
      };
      if (command) h["X-Goog-Upload-Command"] = command;
      const cookie = this.jar.scopedHeader(false);
      if (cookie) h.Cookie = cookie;
      return h;
    };

    const r1 = await this._transport({
      method: "OPTIONS",
      url: UPLOAD_URL,
      headers: makeHeaders(null, UPLOAD_FORM_TYPE),
      body: "",
      timeoutMs: this.timeoutMs,
    });
    if (r1.status < 200 || r1.status >= 300) {
      throw new GeminiWebError(`Upload preflight failed (HTTP ${r1.status})`);
    }

    const startHeaders = { ...makeHeaders("start", UPLOAD_FORM_TYPE), Size: String(size) };
    const r2 = await this._transport({
      method: "POST",
      url: UPLOAD_URL,
      headers: startHeaders,
      body: `File name: ${fname}`,
      timeoutMs: this.timeoutMs,
    });
    if (r2.status < 200 || r2.status >= 300) {
      throw new GeminiWebError(`Upload start failed (HTTP ${r2.status})`);
    }
    const uploadUrl = r2.headers["x-goog-upload-url"];
    if (!uploadUrl) throw new GeminiWebError("Missing X-Goog-Upload-Url header");

    const r3 = await this._transport({
      method: "OPTIONS",
      url: uploadUrl,
      headers: startHeaders,
      body: "",
      timeoutMs: this.timeoutMs,
    });
    if (r3.status < 200 || r3.status >= 300) {
      throw new GeminiWebError(`Upload target preflight failed (HTTP ${r3.status})`);
    }

    const finalHeaders = {
      ...makeHeaders("upload, finalize", mime),
      Size: String(size),
      "X-Goog-Upload-Offset": "0",
    };
    const r4 = await this._transport({
      method: "POST",
      url: uploadUrl,
      headers: finalHeaders,
      body: rawBytes,
      timeoutMs: this.timeoutMs,
    });
    if (r4.status < 200 || r4.status >= 300) {
      throw new GeminiWebError(`Upload finalize failed (HTTP ${r4.status})`);
    }
    const fileId = r4.body.trim().replace(/^['"]|['"]$/g, "");
    if (!fileId) throw new GeminiWebError("Empty file id returned from upload");
    return { fileId, fileName: fname };
  }

  async _askOnce({ prompt, model, effort, images, chat, onPartial }) {
    const session = await this.getSession();
    const { entries: table, tier } = await this._loadModelTable(session, false);
    const selected = pickModel(table, model) || pickModel(table, DEFAULT_MODEL);
    if (!selected) throw new GeminiWebError("No model rows available for this account.");

    const effectiveTier = this.allowUltra || tier >= AccountTier.ULTRA ? AccountTier.ULTRA : tier;
    if (!modelAllowedOnPlan(selected.id, selected.category, selected.label, effectiveTier)) {
      throw new GeminiWebError(`${selected.label} requires an Ultra plan.`);
    }

    const fileIds = [];
    const fileNames = [];
    if (images && images.length) {
      const uploaded = await Promise.all(
        images.map((item) => {
          if (typeof item === "string") return this.uploadImage(item, { pushId: session.push });
          return this.uploadImage(item.bytes, {
            filename: item.filename,
            mimeType: item.mimeType,
            pushId: session.push,
          });
        })
      );
      for (const u of uploaded) {
        fileIds.push(u.fileId);
        fileNames.push(u.fileName);
      }
    }

    const reqUuid = crypto.randomUUID().toUpperCase();
    const level = thinkingLevel(effort);
    const inner = buildStreamGenerateInner({
      prompt,
      locale: session.hl,
      chat,
      requestUuid: reqUuid,
      modelNumber: selected.number,
      level,
      fileIds,
      fileNames,
    });
    let form = buildStreamGenerateForm(inner);
    if (session.at) form = `at=${urlEncode(session.at)}&${form}`;

    let query = `${this._accountPrefix()}${GENERATE_PATH}?hl=${urlEncode(session.hl)}&_reqid=${this._nextReqid()}&rt=c`;
    if (session.at) query += `&at=${urlEncode(session.at)}`;
    if (session.bl) query += `&bl=${urlEncode(session.bl)}`;
    if (session.sid) query += `&f.sid=${urlEncode(session.sid)}`;

    const headers = this._requestHeaders(buildModelHeader(selected, level, this._generationId), reqUuid);

    let scanner = new ReplyStreamScanner();
    let emitted = 0;

    const started = Date.now();
    const res = await this._authedRequest({
      method: "POST",
      url: `${ORIGIN}${query}`,
      headers,
      body: form,
      timeoutMs: this.generateTimeoutMs,
      onChunk: (chunk) => {
        if (scanner.feed(chunk) && onPartial) {
          const text = scanner.turn.answer;
          if (emitted === 0 || text.length >= emitted + 32) {
            emitted = text.length;
            onPartial(stripCitations(text));
          }
        }
      },
      onReset: () => {
        scanner = new ReplyStreamScanner();
        emitted = 0;
      },
    });
    const elapsedMs = Date.now() - started;

    if (res.status === 401 || res.status === 403) {
      this.resetSession();
      throw new AuthRejectedError();
    }
    if (res.status < 200 || res.status >= 300) {
      if (res.status === 400) this.resetSession();
      throw new GeminiWebError(`Gemini Web: HTTP ${res.status}`);
    }

    let turn = scanner.turn;
    if (!turn.answer && res.body) turn = extractReply(res.body);

    if (turn.chat.conversation && turn.chat.response && turn.chat.candidate) {
      chat.conversation = turn.chat.conversation;
      chat.response = turn.chat.response;
      chat.candidate = turn.chat.candidate;
    } else {
      chat.conversation = "";
      chat.response = "";
      chat.candidate = "";
    }

    if (turn.answer.trim() && isSiteModelError(turn.answer)) {
      throw new GeminiWebError(`Gemini Web: ${selected.label} did not answer. Pick another model.`);
    }
    if (turn.answer.trim() && isSiteApology(turn.answer)) {
      throw new GeminiWebError("Gemini Web: the site failed the request - try again.");
    }
    if (!turn.answer.trim()) {
      const err = parseBardError(res.body);
      if (err) {
        if (err.code === 1097) {
          chat.conversation = "";
          chat.response = "";
          chat.candidate = "";
        }
        throw new GeminiWebError(`Gemini Web: ${err.message || `request refused (code ${err.code})`}`);
      }
      throw new GeminiWebError("Could not parse the Gemini response.");
    }

    const answer = stripCitations(turn.answer);
    if (onPartial && answer.length > emitted) onPartial(answer);

    return {
      answer,
      thinking: turn.thinking,
      servedLabel: turn.servedLabel || selected.label,
      servedNumber: turn.servedNumber || selected.number,
      thread: { ...chat },
      elapsedMs,
    };
  }

  async ask(
    prompt,
    {
      model = DEFAULT_MODEL,
      effort = "medium",
      images = [],
      continueChat = false,
      thread = null,
      onPartial = null,
      retries = 2,
    } = {}
  ) {
    const active = thread || (continueChat ? this.thread : { conversation: "", response: "", candidate: "" });
    let lastErr;
    for (let attempt = 0; attempt <= retries; attempt++) {
      try {
        return await this._askOnce({ prompt, model, effort, images, chat: active, onPartial });
      } catch (err) {
        if (err instanceof AuthRejectedError) throw err;
        lastErr = err;
        if (attempt >= retries || !isTransientError(err.message)) throw err;
        if (!continueChat && !thread) {
          active.conversation = "";
          active.response = "";
          active.candidate = "";
        }
        this.resetSession();
        await new Promise((r) => setTimeout(r, attempt === 0 ? 250 : 700));
      }
    }
    throw lastErr;
  }

  async *stream(prompt, options = {}) {
    const queue = [];
    let notify = null;
    let done = false;
    let failure = null;
    let sentLen = 0;

    const push = (delta) => {
      queue.push(delta);
      if (notify) {
        const wake = notify;
        notify = null;
        wake();
      }
    };

    const run = this.ask(prompt, {
      ...options,
      onPartial: (snapshot) => {
        if (snapshot.length > sentLen) {
          const delta = snapshot.slice(sentLen);
          sentLen = snapshot.length;
          push(delta);
        }
      },
    })
      .then((res) => {
        if (res.answer.length > sentLen) push(res.answer.slice(sentLen));
        done = true;
        if (notify) notify();
      })
      .catch((err) => {
        failure = err;
        done = true;
        if (notify) notify();
      });

    while (!done || queue.length) {
      if (queue.length) {
        yield queue.shift();
      } else {
        await new Promise((resolve) => {
          notify = resolve;
        });
      }
    }
    await run;
    if (failure) throw failure;
  }

  async getUsage() {
    const session = await this.getSession();
    const nowMs = Date.now();
    const query = this._buildBatchQuery(RPC_QUOTA, "/app", session);
    const headers = this._requestHeaders("[]", crypto.randomUUID().toUpperCase());

    let meter = { valid: false, shortWindow: { valid: false }, weekly: { valid: false } };
    for (const shape of ["[[[1,2]]]", "[]"]) {
      const reqJson = `[[["${RPC_QUOTA}","${shape}",null,"generic"]]]`;
      let body = `f.req=${urlEncode(reqJson)}`;
      if (session.at) body = `at=${urlEncode(session.at)}&${body}`;
      const res = await this._authedRequest({
        method: "POST",
        url: `${ORIGIN}${query}`,
        headers,
        body,
        timeoutMs: 30_000,
      });
      if (res.status === 401 || res.status === 403) {
        this.resetSession();
        throw new AuthRejectedError();
      }
      if (res.status < 200 || res.status >= 300) {
        throw new GeminiWebError(`Usage RPC returned HTTP ${res.status}`);
      }
      const payload = findRpcPayload(res.body, RPC_QUOTA);
      if (payload) {
        const rows = parseQuotaRows(payload);
        if (rows.length) {
          meter = classifyQuotaRows(rows, nowMs);
          if (meter.shortWindow.valid) return meter;
        }
      }
    }

    if (!meter.shortWindow.valid) {
      const pageQuery = this._buildBatchQuery(RPC_USAGE_PAGE, "/usage", session);
      const pageReq = `[[["${RPC_USAGE_PAGE}","[]",null,"generic"]]]`;
      let pageBody = `f.req=${urlEncode(pageReq)}`;
      if (session.at) pageBody = `at=${urlEncode(session.at)}&${pageBody}`;
      const res = await this._authedRequest({
        method: "POST",
        url: `${ORIGIN}${pageQuery}`,
        headers,
        body: pageBody,
        timeoutMs: 30_000,
      });
      if (res.status >= 200 && res.status < 300) {
        const payload = findRpcPayload(res.body, RPC_USAGE_PAGE);
        if (payload) {
          const rows = parseUsagePagePayload(payload);
          if (rows.length) meter = classifyQuotaRows(rows, nowMs);
        }
      }
    }

    if (!meter.valid) throw new GeminiWebError("No quota rows returned by Gemini.");
    return meter;
  }
}
