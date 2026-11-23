import { execFileSync, spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { ACCOUNTS_COOKIE_PREFIX, parseCookieHeader } from "./cookies.js";

const CHECK_SESSION_JS = `(() => {
  if (location.origin !== 'https://gemini.google.com') return null;
  const m = location.pathname.match(/^\\/(?:u\\/([0-3])\\/)?app(?:\\/|$)/);
  if (!m) return null;
  const boot = window.WIZ_global_data;
  if (!boot || typeof boot !== 'object') return null;
  const tok = (k) => (typeof boot[k] === 'string' ? boot[k].trim() : '');
  const at = tok('SNlM0e');
  const bl = tok('cfb2h');
  const sid = tok('FdrFJe');
  const email = tok('oPEP7c');
  if ((!at && !email) || !bl || !sid) return null;
  return { authenticated: true, account: Number(m[1] || 0), email };
})()`;

function sanitizeProfile(profile) {
  const cleaned = String(profile || "default")
    .trim()
    .replace(/[^A-Za-z0-9._-]+/g, "_")
    .replace(/^[._]+|[._]+$/g, "");
  return cleaned || "default";
}

export function defaultDataDir() {
  const override = (process.env.GEMINI_DATA_DIR || "").trim();
  if (override) return path.resolve(override);
  return path.join(os.homedir(), ".gemini-web-to-api");
}

export function defaultSessionPath() {
  const override = (process.env.GEMINI_SESSION_FILE || "").trim();
  if (override) return path.resolve(override);
  return path.join(defaultDataDir(), "session.json");
}

export function browserProfileDir(profile = "default") {
  return path.join(defaultDataDir(), "browser", sanitizeProfile(profile));
}

export function hasBrowserProfile(profile = "default") {
  const dir = browserProfileDir(profile);
  return fs.existsSync(path.join(dir, "Default")) || fs.existsSync(path.join(dir, "Local State"));
}

function chmodPrivate(target, isDir = false) {
  if (process.platform !== "win32") {
    try {
      fs.chmodSync(target, isDir ? 0o700 : 0o600);
    } catch {
      // ignore chmod failures on restricted filesystems
    }
  }
}

export function loadSession(profile = "default", { sessionPath = null } = {}) {
  const filePath = sessionPath ? path.resolve(sessionPath) : defaultSessionPath();
  if (!fs.existsSync(filePath)) return null;
  try {
    const data = JSON.parse(fs.readFileSync(filePath, "utf8"));
    const key = sanitizeProfile(profile);
    const entry = data?.accounts?.[key];
    if (!entry || typeof entry.cookies !== "string" || !entry.cookies.trim()) return null;
    return {
      cookies: entry.cookies.trim(),
      email: String(entry.email || ""),
      account: Number(entry.account || 0),
      updatedAt: Number(entry.updated_at || 0),
      profile: key,
    };
  } catch {
    return null;
  }
}

export function saveSession(
  cookies,
  { profile = "default", email = "", account = 0, sessionPath = null } = {}
) {
  const filePath = sessionPath ? path.resolve(sessionPath) : defaultSessionPath();
  const dir = path.dirname(filePath);
  fs.mkdirSync(dir, { recursive: true });
  chmodPrivate(dir, true);

  let data = { version: 1, accounts: {} };
  if (fs.existsSync(filePath)) {
    try {
      const existing = JSON.parse(fs.readFileSync(filePath, "utf8"));
      if (existing && typeof existing.accounts === "object") data = existing;
    } catch {
      // overwrite malformed file
    }
  }

  const key = sanitizeProfile(profile);
  const prev = data.accounts[key] || {};
  data.version = 1;
  data.accounts[key] = {
    cookies,
    email: email || String(prev.email || ""),
    account: Number(account || 0),
    updated_at: Math.floor(Date.now() / 1000),
  };

  const tmpPath = `${filePath}.tmp`;
  fs.writeFileSync(tmpPath, JSON.stringify(data, null, 2) + "\n", "utf8");
  chmodPrivate(tmpPath, false);
  fs.renameSync(tmpPath, filePath);
  chmodPrivate(filePath, false);
  return filePath;
}

export function findChromiumBrowser(explicit = null) {
  for (const c of [explicit, process.env.GEMINI_BROWSER]) {
    if (c && fs.existsSync(c)) return c;
  }

  const candidates = [];
  if (process.platform === "win32") {
    const roots = [process.env.LOCALAPPDATA, process.env.PROGRAMFILES, process.env["PROGRAMFILES(X86)"]].filter(
      Boolean
    );
    const rels = [
      "Google\\Chrome\\Application\\chrome.exe",
      "Chromium\\Application\\chrome.exe",
      "Microsoft\\Edge\\Application\\msedge.exe",
      "BraveSoftware\\Brave-Browser\\Application\\brave.exe",
      "Vivaldi\\Application\\vivaldi.exe",
    ];
    for (const root of roots) {
      for (const rel of rels) candidates.push(path.join(root, rel));
    }
    for (const exe of ["chrome.exe", "msedge.exe", "brave.exe", "chromium.exe", "vivaldi.exe"]) {
      for (const hive of ["HKCU", "HKLM"]) {
        try {
          const out = execFileSync(
            "reg",
            ["query", `${hive}\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\${exe}`, "/ve"],
            { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }
          );
          const match = /REG_\w+\s+(.+)/.exec(out);
          if (match) candidates.push(match[1].trim().replace(/^"|"$/g, ""));
        } catch {
          // registry entry missing
        }
      }
    }
  } else if (process.platform === "darwin") {
    candidates.push(
      "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
      "/Applications/Chromium.app/Contents/MacOS/Chromium",
      "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
      "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
      "/Applications/Vivaldi.app/Contents/MacOS/Vivaldi"
    );
  } else {
    for (const name of [
      "google-chrome",
      "google-chrome-stable",
      "chromium",
      "chromium-browser",
      "microsoft-edge",
      "brave-browser",
      "vivaldi",
    ]) {
      try {
        const found = execFileSync("which", [name], { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim();
        if (found) candidates.push(found);
      } catch {
        // not installed
      }
    }
  }

  for (const c of candidates) {
    if (c && fs.existsSync(c)) return c;
  }
  return null;
}

function findFreePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.unref();
    srv.on("error", reject);
    srv.listen(0, "127.0.0.1", () => {
      const addr = srv.address();
      const port = typeof addr === "object" && addr ? addr.port : 0;
      srv.close(() => resolve(port));
    });
  });
}

// Minimal RFC 6455 localhost client for Node 18+ without external packages.
class LocalCdpSocket {
  constructor(socket, initialBuffer) {
    this._sock = socket;
    this._buf = initialBuffer;
    this._waiters = [];
    this._closed = false;
    this._nextId = 1;
    this._pending = new Map();
    this._fragments = [];

    socket.on("data", (chunk) => {
      this._buf = Buffer.concat([this._buf, chunk]);
      this._drain();
    });
    socket.on("close", () => this._failAll(new Error("CDP WebSocket closed")));
    socket.on("error", (err) => this._failAll(err));
    if (this._buf.length) this._drain();
  }

  static connect(wsUrl, timeoutMs = 5000) {
    const rest = wsUrl.split("://", 2)[1];
    const slash = rest.indexOf("/");
    const hostPort = rest.slice(0, slash);
    const urlPath = rest.slice(slash);
    const [host, portStr] = hostPort.split(":");
    return new Promise((resolve, reject) => {
      const sock = net.createConnection({ host, port: Number(portStr) });
      const timer = setTimeout(() => {
        sock.destroy();
        reject(new Error("CDP handshake timed out"));
      }, timeoutMs);

      let handshake = Buffer.alloc(0);
      const onData = (chunk) => {
        handshake = Buffer.concat([handshake, chunk]);
        const idx = handshake.indexOf("\r\n\r\n");
        if (idx === -1) return;
        clearTimeout(timer);
        sock.off("data", onData);
        const head = handshake.subarray(0, idx).toString("ascii");
        if (!head.split("\r\n", 1)[0].includes("101 ")) {
          sock.destroy();
          reject(new Error(`CDP upgrade failed: ${head.split("\r\n", 1)[0]}`));
          return;
        }
        resolve(new LocalCdpSocket(sock, handshake.subarray(idx + 4)));
      };

      sock.on("data", onData);
      sock.on("error", (err) => {
        clearTimeout(timer);
        reject(err);
      });
      sock.on("connect", () => {
        const key = crypto.randomBytes(16).toString("base64");
        sock.write(
          `GET ${urlPath} HTTP/1.1\r\n` +
            `Host: ${hostPort}\r\n` +
            `Upgrade: websocket\r\n` +
            `Connection: Upgrade\r\n` +
            `Sec-WebSocket-Key: ${key}\r\n` +
            `Sec-WebSocket-Version: 13\r\n\r\n`
        );
      });
    });
  }

  _failAll(err) {
    this._closed = true;
    for (const { reject } of this._pending.values()) reject(err);
    this._pending.clear();
  }

  _sendFrame(payload, opcode = 0x1) {
    const mask = crypto.randomBytes(4);
    const len = payload.length;
    let header;
    if (len < 126) {
      header = Buffer.alloc(6);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | len;
      mask.copy(header, 2);
    } else if (len < 65536) {
      header = Buffer.alloc(8);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 126;
      header.writeUInt16BE(len, 2);
      mask.copy(header, 4);
    } else {
      header = Buffer.alloc(14);
      header[0] = 0x80 | opcode;
      header[1] = 0x80 | 127;
      header.writeBigUInt64BE(BigInt(len), 2);
      mask.copy(header, 10);
    }
    const masked = Buffer.allocUnsafe(len);
    for (let i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i & 3];
    this._sock.write(Buffer.concat([header, masked]));
  }

  _drain() {
    while (this._buf.length >= 2) {
      const b1 = this._buf[0];
      const b2 = this._buf[1];
      const fin = Boolean(b1 & 0x80);
      const opcode = b1 & 0x0f;
      const masked = Boolean(b2 & 0x80);
      let len = b2 & 0x7f;
      let offset = 2;
      if (len === 126) {
        if (this._buf.length < 4) return;
        len = this._buf.readUInt16BE(2);
        offset = 4;
      } else if (len === 127) {
        if (this._buf.length < 10) return;
        len = Number(this._buf.readBigUInt64BE(2));
        offset = 10;
      }
      const maskLen = masked ? 4 : 0;
      if (this._buf.length < offset + maskLen + len) return;
      const mask = masked ? this._buf.subarray(offset, offset + 4) : null;
      let payload = this._buf.subarray(offset + maskLen, offset + maskLen + len);
      this._buf = this._buf.subarray(offset + maskLen + len);
      if (mask) {
        payload = Buffer.from(payload);
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      }
      if (opcode === 0x9) {
        this._sendFrame(payload, 0xa);
        continue;
      }
      if (opcode === 0xa) continue;
      if (opcode === 0x8) {
        this._sock.end();
        return;
      }
      this._fragments.push(payload);
      if (fin) {
        const text = Buffer.concat(this._fragments).toString("utf8");
        this._fragments = [];
        try {
          const msg = JSON.parse(text);
          if (msg.id && this._pending.has(msg.id)) {
            const { resolve } = this._pending.get(msg.id);
            this._pending.delete(msg.id);
            resolve(msg);
          }
        } catch {
          // ignore malformed frames
        }
      }
    }
  }

  call(method, params = {}, timeoutMs = 5000) {
    if (this._closed) return Promise.reject(new Error("CDP WebSocket closed"));
    const id = this._nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._pending.delete(id);
        reject(new Error(`CDP call timed out: ${method}`));
      }, timeoutMs);
      this._pending.set(id, {
        resolve: (msg) => {
          clearTimeout(timer);
          resolve(msg);
        },
        reject: (err) => {
          clearTimeout(timer);
          reject(err);
        },
      });
      this._sendFrame(Buffer.from(JSON.stringify({ id, method, params }), "utf8"), 0x1);
    });
  }

  close() {
    this._sock.destroy();
  }
}

export function formatCdpCookies(geminiCookies, accountsCookies) {
  const merged = new Map();
  const put = (name, value) => {
    if (name && value) merged.set(name, value);
  };
  for (const c of geminiCookies || []) put(c.name, c.value);
  for (const c of accountsCookies || []) {
    const domain = String(c.domain || "").toLowerCase();
    if (domain === "accounts.google.com" || domain === ".accounts.google.com") {
      put(ACCOUNTS_COOKIE_PREFIX + c.name, c.value);
    } else if (domain === ".google.com" || domain === "google.com") {
      put(c.name, c.value);
    }
  }
  return [...merged.entries()].map(([k, v]) => `${k}=${v}`).join("; ");
}

export async function browserLogin({
  profile = "default",
  headless = false,
  timeoutMs = 300_000,
  browserPath = null,
  sessionPath = null,
  account = 0,
} = {}) {
  const exe = findChromiumBrowser(browserPath);
  if (!exe) {
    throw new Error(
      "No Chromium browser found (Chrome, Edge, Brave, or Chromium). " +
        "Set GEMINI_BROWSER=/path/to/chrome or pass GEMINI_COOKIE manually."
    );
  }

  const profDir = browserProfileDir(profile);
  fs.mkdirSync(profDir, { recursive: true });
  chmodPrivate(profDir, true);

  const port = await findFreePort();
  const prefix = account > 0 ? `/u/${account}` : "";
  const url = `https://gemini.google.com${prefix}/app`;
  const args = [
    `--user-data-dir=${profDir}`,
    `--remote-debugging-port=${port}`,
    "--no-first-run",
    "--no-default-browser-check",
    "--window-size=980,740",
  ];
  if (headless) args.push("--headless=new");
  args.push(url);

  const proc = spawn(exe, args, { stdio: "ignore" });
  let exited = false;
  proc.on("exit", () => {
    exited = true;
  });

  const deadline = Date.now() + timeoutMs;
  try {
    while (Date.now() < deadline) {
      if (exited) throw new Error("Browser closed before Gemini sign-in completed.");
      let pages = [];
      try {
        const res = await fetch(`http://127.0.0.1:${port}/json`, { signal: AbortSignal.timeout(2000) });
        if (res.ok) {
          const all = await res.json();
          pages = Array.isArray(all) ? all.filter((t) => t?.type === "page") : [];
        }
      } catch {
        await new Promise((r) => setTimeout(r, 300));
        continue;
      }

      for (const page of pages) {
        const pageUrl = String(page.url || "");
        const wsUrl = String(page.webSocketDebuggerUrl || "");
        if (!wsUrl || !pageUrl.startsWith("https://gemini.google.com/")) continue;
        let sock = null;
        try {
          sock = await LocalCdpSocket.connect(wsUrl, 5000);
          const ev = await sock.call("Runtime.evaluate", {
            expression: CHECK_SESSION_JS,
            returnByValue: true,
          });
          const val = ev?.result?.result?.value;
          if (!val || !val.authenticated) continue;

          const cGem = await sock.call("Network.getCookies", { urls: ["https://gemini.google.com/"] });
          const cAcc = await sock.call("Network.getCookies", {
            urls: ["https://accounts.google.com/RotateCookies"],
          });
          const header = formatCdpCookies(cGem?.result?.cookies, cAcc?.result?.cookies);
          const jar = parseCookieHeader(header, Number(val.account || 0));
          const email = String(val.email || "");
          saveSession(jar.header, { profile, email, account: jar.account, sessionPath });
          try {
            await sock.call("Browser.close");
          } catch {
            // browser may close immediately
          }
          return {
            cookies: jar.header,
            email,
            account: jar.account,
            updatedAt: Math.floor(Date.now() / 1000),
            profile: sanitizeProfile(profile),
          };
        } catch {
          // page still loading
        } finally {
          if (sock) sock.close();
        }
      }
      await new Promise((r) => setTimeout(r, 1000));
    }
    throw new Error(`Timed out waiting for Gemini sign-in (${Math.round(timeoutMs / 1000)}s).`);
  } finally {
    if (!exited) {
      proc.kill();
    }
  }
}

export async function refreshFromBrowser({
  profile = "default",
  timeoutMs = 45_000,
  browserPath = null,
  sessionPath = null,
  account = 0,
} = {}) {
  if (!hasBrowserProfile(profile)) return null;
  try {
    return await browserLogin({ profile, headless: true, timeoutMs, browserPath, sessionPath, account });
  } catch {
    return null;
  }
}

const isMain = (() => {
  if (!process.argv[1]) return false;
  const selfPath = path.resolve(fileURLToPath(import.meta.url));
  const argPath = path.resolve(process.argv[1]);
  return process.platform === "win32"
    ? selfPath.toLowerCase() === argPath.toLowerCase()
    : selfPath === argPath;
})();

if (isMain) {
  const profile = process.env.GEMINI_PROFILE || "default";
  const account = Number(process.env.GEMINI_ACCOUNT || 0);
  console.log("Opening browser for Gemini sign-in... Sign in and wait for the window to close.");
  browserLogin({ profile, account })
    .then((saved) => {
      const who = saved.email ? ` (${saved.email})` : "";
      console.log(`Saved session for profile '${saved.profile}'${who} -> ${defaultSessionPath()}`);
    })
    .catch((err) => {
      console.error(`Sign-in failed: ${err.message}`);
      process.exit(1);
    });
}
