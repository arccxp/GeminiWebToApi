export const ACCOUNTS_COOKIE_PREFIX = "GwaAccounts.";
const MAX_COOKIE_HEADER_BYTES = 32 * 1024;
const MAX_COOKIE_VALUE_BYTES = 4 * 1024;
const VALID_NAME_RE = /^[A-Za-z0-9_\-.!#$%&*+^`|~]+$/;

function unwrap(text) {
  let value = String(text || "").trim();
  if (value.startsWith("-H ")) value = value.slice(3).trim();
  while (value.length && (value[0] === "'" || value[0] === '"')) value = value.slice(1);
  while (value.length && (value.endsWith("'") || value.endsWith('"') || value.endsWith("\\"))) {
    value = value.slice(0, -1);
  }
  if (value.toLowerCase().startsWith("cookie:")) {
    value = value.slice(7).trim();
    while (value.length && (value[0] === "'" || value[0] === '"')) value = value.slice(1);
    while (value.length && (value.endsWith("'") || value.endsWith('"'))) value = value.slice(0, -1);
    value = value.trim();
  }
  return value;
}

function cookieValue(raw) {
  let value = String(raw || "").trim();
  const nl = value.search(/[\r\n]/);
  if (nl !== -1) value = value.slice(0, nl);
  if (value.length >= 2 && value.startsWith('"') && value.endsWith('"')) {
    value = value.slice(1, -1).trim();
  }
  return value;
}

function splitPairs(header) {
  const pairs = [];
  for (const part of String(header || "").split(";")) {
    const item = part.trim();
    const eq = item.indexOf("=");
    if (eq <= 0) continue;
    const name = item.slice(0, eq).trim();
    const val = cookieValue(item.slice(eq + 1));
    if (name && val && VALID_NAME_RE.test(name)) pairs.push([name, val]);
  }
  return pairs;
}

function joinPairs(pairs) {
  return pairs.map(([k, v]) => `${k}=${v}`).join("; ");
}

export function headerCookie(header, name) {
  for (const [k, v] of splitPairs(header)) {
    if (k === name) return v;
  }
  return "";
}

function slotSessionCookie(slot) {
  return `__Secure-${slot + 1}PSID`;
}

function slotPaperCookie(slot) {
  return `__Secure-${slot + 1}PAPISID`;
}

function hasSessionCookie(header) {
  if (headerCookie(header, "SID")) return true;
  for (let slot = 0; slot < 4; slot++) {
    if (headerCookie(header, slotSessionCookie(slot))) return true;
  }
  return false;
}

function slotIsLogin(header, slot, value) {
  if (!value) return false;
  if (slot !== 2) return true;
  const hasFirst = Boolean(headerCookie(header, slotSessionCookie(0)));
  const hasSecond = Boolean(headerCookie(header, slotSessionCookie(1)));
  return !(hasFirst && !hasSecond);
}

export function mergeCookieHeaders(existing, incoming) {
  const map = new Map();
  for (const [k, v] of splitPairs(existing)) map.set(k, v);
  for (const [k, v] of splitPairs(incoming)) map.set(k, v);
  return joinPairs([...map.entries()]);
}

export class CookieJar {
  constructor(header, { sapisid = "", email = "", count = 0, account = 0 } = {}) {
    this.header = header;
    this.sapisid = sapisid;
    this.email = email;
    this.count = count;
    this.account = Math.max(0, account);
    this._liveHeader = header;
  }

  get liveHeader() {
    return this._liveHeader || this.header;
  }

  setLiveHeader(nextHeader) {
    this._liveHeader = nextHeader;
  }

  scopedHeader(forAccounts = false) {
    const shared = [];
    const accounts = [];
    for (const [k, v] of splitPairs(this.liveHeader)) {
      if (k.startsWith(ACCOUNTS_COOKIE_PREFIX)) {
        if (forAccounts) accounts.push([k.slice(ACCOUNTS_COOKIE_PREFIX.length), v]);
      } else {
        shared.push([k, v]);
      }
    }
    if (!accounts.length) return joinPairs(shared);
    return mergeCookieHeaders(joinPairs(shared), joinPairs(accounts));
  }

  hashKey(account = this.account) {
    const slot = Math.max(0, account);
    const live = this.liveHeader;
    if (slot > 0) return headerCookie(live, slotPaperCookie(slot));
    for (const name of ["SAPISID", slotPaperCookie(0), "__Secure-3PAPISID"]) {
      const val = headerCookie(live, name);
      if (val) return val;
    }
    return this.sapisid;
  }

  accounts() {
    const live = this.liveHeader;
    const slots = [];
    for (let slot = 0; slot < 4; slot++) {
      const val = headerCookie(live, slotSessionCookie(slot));
      if (slotIsLogin(live, slot, val)) slots.push(slot);
    }
    if (!slots.length && this.hashKey(0)) slots.push(0);
    return slots;
  }

  absorbSetCookies(setCookieHeaders, { fromAccounts = false } = {}) {
    if (!setCookieHeaders || !setCookieHeaders.length) return null;
    const pairs = splitPairs(this.liveHeader);
    let changed = false;

    for (const raw of setCookieHeaders) {
      let line = String(raw || "").trim();
      if (line.toLowerCase().startsWith("set-cookie:")) line = line.slice(11).trim();
      const parts = line.split(";");
      if (!parts.length) continue;
      const eq = parts[0].indexOf("=");
      if (eq <= 0) continue;
      let name = parts[0].slice(0, eq).trim();
      const value = cookieValue(parts[0].slice(eq + 1));
      if (!name || !VALID_NAME_RE.test(name) || value.length > MAX_COOKIE_VALUE_BYTES) continue;

      let domain = "";
      let maxAgeExpired = false;
      for (let i = 1; i < parts.length; i++) {
        const attr = parts[i].trim();
        const aeq = attr.indexOf("=");
        if (aeq <= 0) continue;
        const ak = attr.slice(0, aeq).trim().toLowerCase();
        const av = attr.slice(aeq + 1).trim().toLowerCase();
        if (ak === "domain") domain = av;
        else if (ak === "max-age" && (av === "0" || av.startsWith("-"))) maxAgeExpired = true;
      }

      if (fromAccounts && domain !== "google.com" && domain !== ".google.com") {
        name = ACCOUNTS_COOKIE_PREFIX + name;
      }

      const deleted = !value || maxAgeExpired;
      let found = false;
      for (let i = 0; i < pairs.length; i++) {
        if (pairs[i][0] !== name) continue;
        found = true;
        if (deleted) {
          pairs[i][1] = "";
          changed = true;
        } else if (pairs[i][1] !== value) {
          pairs[i][1] = value;
          changed = true;
        }
        break;
      }
      if (!found && !deleted) {
        pairs.push([name, value]);
        changed = true;
      }
    }

    if (!changed) return null;
    const kept = pairs.filter(([, v]) => Boolean(v));
    const nextHeader = joinPairs(kept);
    if (Buffer.byteLength(nextHeader, "utf8") > MAX_COOKIE_HEADER_BYTES || !hasSessionCookie(nextHeader)) {
      return null;
    }
    this._liveHeader = nextHeader;
    return nextHeader;
  }
}

export function parseCookieHeader(text, account = 0) {
  const value = unwrap(text);
  if (!value) throw new Error("no cookies");
  if (Buffer.byteLength(value, "utf8") > MAX_COOKIE_HEADER_BYTES) {
    throw new Error("cookie header too large");
  }
  const pairs = splitPairs(value);
  if (!pairs.length) throw new Error("no cookies");
  const header = joinPairs(pairs);
  if (!hasSessionCookie(header)) {
    throw new Error("no session cookie (expected SID or __Secure-1PSID)");
  }
  return new CookieJar(header, {
    sapisid: headerCookie(header, "SAPISID"),
    count: pairs.length,
    account: Math.max(0, account),
  });
}

export function rotatingCookieChanged(previous, current, account = 0) {
  const changed = (name) => {
    const next = headerCookie(current, name);
    return Boolean(next) && next !== headerCookie(previous, name);
  };
  const name = `__Secure-${account + 1}PSIDTS`;
  if (changed(name) || changed(ACCOUNTS_COOKIE_PREFIX + name)) return true;
  if (account === 0) {
    return changed("__Secure-3PSIDTS") || changed(ACCOUNTS_COOKIE_PREFIX + "__Secure-3PSIDTS");
  }
  return false;
}
