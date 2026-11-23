"""One-time Chromium sign-in, shared session storage, and silent headless refresh.

Uses only the Python standard library (socket + RFC 6455 WebSocket + Chrome DevTools Protocol).
Never touches or reads the user's main browser profile: launches Chromium with an isolated
user-data directory under ~/.gemini-web-to-api/browser/<profile>.
"""

from __future__ import annotations

import base64
from dataclasses import dataclass
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

from .cookies import ACCOUNTS_COOKIE_PREFIX, CookieJar, parse_cookie_header

_PROFILE_SAFE_RE = re.compile(r"[^A-Za-z0-9._-]+")

_CHECK_SESSION_JS = r"""
(() => {
  if (location.origin !== 'https://gemini.google.com') return null;
  const m = location.pathname.match(/^\/(?:u\/([0-3])\/)?app(?:\/|$)/);
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
})()
"""


@dataclass
class SavedSession:
    cookies: str
    email: str = ""
    account: int = 0
    updated_at: int = 0
    profile: str = "default"


def _sanitize_profile(profile: str) -> str:
    cleaned = _PROFILE_SAFE_RE.sub("_", (profile or "default").strip()).strip("._")
    return cleaned or "default"


def default_data_dir() -> Path:
    override = os.environ.get("GEMINI_DATA_DIR", "").strip()
    if override:
        return Path(override).expanduser()
    return Path.home() / ".gemini-web-to-api"


def default_session_path() -> Path:
    override = os.environ.get("GEMINI_SESSION_FILE", "").strip()
    if override:
        return Path(override).expanduser()
    return default_data_dir() / "session.json"


def browser_profile_dir(profile: str = "default") -> Path:
    return default_data_dir() / "browser" / _sanitize_profile(profile)


def has_browser_profile(profile: str = "default") -> bool:
    p = browser_profile_dir(profile)
    return (p / "Default").exists() or (p / "Local State").exists()


def _chmod_private(path: Path, is_dir: bool = False) -> None:
    if os.name == "posix":
        try:
            os.chmod(path, 0o700 if is_dir else 0o600)
        except OSError:
            pass


def load_session(
    profile: str = "default",
    *,
    path: Optional[os.PathLike[str] | str] = None,
) -> Optional[SavedSession]:
    file_path = Path(path) if path is not None else default_session_path()
    if not file_path.is_file():
        return None
    try:
        data = json.loads(file_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    accounts = data.get("accounts")
    if not isinstance(accounts, dict):
        return None
    key = _sanitize_profile(profile)
    entry = accounts.get(key)
    if not isinstance(entry, dict):
        return None
    cookies = str(entry.get("cookies") or "").strip()
    if not cookies:
        return None
    return SavedSession(
        cookies=cookies,
        email=str(entry.get("email") or ""),
        account=int(entry.get("account") or 0),
        updated_at=int(entry.get("updated_at") or 0),
        profile=key,
    )


def save_session(
    cookies: str,
    *,
    profile: str = "default",
    email: str = "",
    account: int = 0,
    path: Optional[os.PathLike[str] | str] = None,
) -> Path:
    file_path = Path(path) if path is not None else default_session_path()
    file_path.parent.mkdir(parents=True, exist_ok=True)
    _chmod_private(file_path.parent, is_dir=True)

    data: Dict[str, Any] = {"version": 1, "accounts": {}}
    if file_path.is_file():
        try:
            existing = json.loads(file_path.read_text(encoding="utf-8"))
            if isinstance(existing, dict) and isinstance(existing.get("accounts"), dict):
                data = existing
        except (OSError, json.JSONDecodeError):
            pass

    key = _sanitize_profile(profile)
    prev = data["accounts"].get(key) if isinstance(data["accounts"].get(key), dict) else {}
    data["version"] = 1
    data["accounts"][key] = {
        "cookies": cookies,
        "email": email or str(prev.get("email") or ""),
        "account": int(account),
        "updated_at": int(time.time()),
    }

    tmp_path = file_path.with_suffix(file_path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    _chmod_private(tmp_path, is_dir=False)
    os.replace(tmp_path, file_path)
    _chmod_private(file_path, is_dir=False)
    return file_path


def find_chromium_browser(explicit: Optional[str] = None) -> Optional[str]:
    for candidate in (explicit, os.environ.get("GEMINI_BROWSER")):
        if candidate and Path(candidate).is_file():
            return str(Path(candidate))

    candidates: List[str] = []
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA", "")
        pf = os.environ.get("PROGRAMFILES", "")
        pfx86 = os.environ.get("PROGRAMFILES(X86)", "")
        rel_paths = [
            r"Google\Chrome\Application\chrome.exe",
            r"Chromium\Application\chrome.exe",
            r"Microsoft\Edge\Application\msedge.exe",
            r"BraveSoftware\Brave-Browser\Application\brave.exe",
            r"Vivaldi\Application\vivaldi.exe",
        ]
        for root in (local, pf, pfx86):
            if not root:
                continue
            for rel in rel_paths:
                candidates.append(os.path.join(root, rel))
        try:
            import winreg

            for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
                for exe in ("chrome.exe", "msedge.exe", "brave.exe", "chromium.exe", "vivaldi.exe"):
                    try:
                        with winreg.OpenKey(
                            hive, rf"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{exe}"
                        ) as k:
                            val, _ = winreg.QueryValueEx(k, None)
                            if isinstance(val, str) and val:
                                candidates.append(val.strip('"'))
                    except OSError:
                        pass
        except ImportError:
            pass
    elif sys.platform == "darwin":
        candidates.extend(
            [
                "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                "/Applications/Chromium.app/Contents/MacOS/Chromium",
                "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
                "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
                "/Applications/Vivaldi.app/Contents/MacOS/Vivaldi",
            ]
        )
    else:
        for name in (
            "google-chrome",
            "google-chrome-stable",
            "chromium",
            "chromium-browser",
            "microsoft-edge",
            "brave-browser",
            "vivaldi",
        ):
            found = shutil.which(name)
            if found:
                candidates.append(found)

    for path in candidates:
        if path and os.path.isfile(path):
            return path
    return None


class _LocalCdpSocket:
    """Minimal RFC 6455 client for plaintext localhost Chrome DevTools Protocol."""

    def __init__(self, ws_url: str, timeout: float = 10.0) -> None:
        rest = ws_url.split("://", 1)[1]
        host_port, path = rest.split("/", 1)
        host, port_s = host_port.split(":", 1)
        self._sock = socket.create_connection((host, int(port_s)), timeout=timeout)
        self._sock.settimeout(timeout)
        self._next_id = 1
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            f"GET /{path} HTTP/1.1\r\n"
            f"Host: {host_port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self._sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("CDP WebSocket handshake closed")
            buf += chunk
        status_line = buf.split(b"\r\n", 1)[0]
        if b"101 " not in status_line:
            raise ConnectionError(f"CDP WebSocket upgrade failed: {status_line!r}")
        self._leftover = buf.split(b"\r\n\r\n", 1)[1]

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    def _recv_exact(self, n: int) -> bytes:
        out = bytearray()
        if self._leftover:
            take = min(n, len(self._leftover))
            out.extend(self._leftover[:take])
            self._leftover = self._leftover[take:]
        while len(out) < n:
            chunk = self._sock.recv(n - len(out))
            if not chunk:
                raise ConnectionError("CDP WebSocket closed")
            out.extend(chunk)
        return bytes(out)

    def _send_frame(self, payload: bytes, opcode: int = 0x1) -> None:
        mask = os.urandom(4)
        hdr = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            hdr.append(0x80 | length)
        elif length < 65536:
            hdr.append(0x80 | 126)
            hdr.extend(struct.pack("!H", length))
        else:
            hdr.append(0x80 | 127)
            hdr.extend(struct.pack("!Q", length))
        hdr.extend(mask)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self._sock.sendall(bytes(hdr) + masked)

    def _recv_text(self) -> str:
        fragments: List[bytes] = []
        while True:
            b1, b2 = self._recv_exact(2)
            fin = bool(b1 & 0x80)
            opcode = b1 & 0x0F
            masked = bool(b2 & 0x80)
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8))[0]
            mask = self._recv_exact(4) if masked else None
            payload = self._recv_exact(length)
            if mask is not None:
                payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
            if opcode == 0x9:  # ping -> pong
                self._send_frame(payload, opcode=0xA)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode == 0x8:  # close
                raise ConnectionError("CDP WebSocket closed by browser")
            fragments.append(payload)
            if fin:
                return b"".join(fragments).decode("utf-8", errors="replace")

    def call(self, method: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        req_id = self._next_id
        self._next_id += 1
        msg = json.dumps({"id": req_id, "method": method, "params": params or {}}).encode("utf-8")
        self._send_frame(msg, opcode=0x1)
        while True:
            reply = json.loads(self._recv_text())
            if reply.get("id") == req_id:
                return reply


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _list_cdp_pages(port: int) -> List[Dict[str, Any]]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2.0)
    try:
        conn.request("GET", "/json")
        resp = conn.getresponse()
        if resp.status != 200:
            return []
        data = json.loads(resp.read().decode("utf-8", errors="replace"))
        return [t for t in data if isinstance(t, dict) and t.get("type") == "page"]
    finally:
        conn.close()


def format_cdp_cookies(
    gemini_cookies: List[Dict[str, Any]],
    accounts_cookies: List[Dict[str, Any]],
) -> str:
    merged: Dict[str, str] = {}
    order: List[str] = []

    def put(name: str, value: str) -> None:
        if not name or not value:
            return
        if name not in merged:
            order.append(name)
        merged[name] = value

    for c in gemini_cookies:
        put(str(c.get("name") or ""), str(c.get("value") or ""))

    for c in accounts_cookies:
        name = str(c.get("name") or "")
        value = str(c.get("value") or "")
        domain = str(c.get("domain") or "").lower()
        if domain in ("accounts.google.com", ".accounts.google.com"):
            put(ACCOUNTS_COOKIE_PREFIX + name, value)
        elif domain in (".google.com", "google.com"):
            put(name, value)

    return "; ".join(f"{k}={merged[k]}" for k in order)


def browser_login(
    profile: str = "default",
    *,
    headless: bool = False,
    timeout: float = 300.0,
    browser_path: Optional[str] = None,
    session_path: Optional[os.PathLike[str] | str] = None,
    account: int = 0,
) -> SavedSession:
    """Opens Chromium to gemini.google.com/app, waits for sign-in, and saves the cookies."""
    exe = find_chromium_browser(browser_path)
    if not exe:
        raise RuntimeError(
            "No Chromium browser found (Chrome, Edge, Brave, or Chromium). "
            "Set GEMINI_BROWSER=/path/to/chrome or pass GEMINI_COOKIE manually."
        )

    prof_dir = browser_profile_dir(profile)
    prof_dir.mkdir(parents=True, exist_ok=True)
    _chmod_private(prof_dir, is_dir=True)

    port = _find_free_port()
    prefix = f"/u/{account}" if account > 0 else ""
    url = f"https://gemini.google.com{prefix}/app"
    args = [
        exe,
        f"--user-data-dir={prof_dir}",
        f"--remote-debugging-port={port}",
        "--no-first-run",
        "--no-default-browser-check",
        "--window-size=980,740",
    ]
    if headless:
        args.append("--headless=new")
    args.append(url)

    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("Browser closed before Gemini sign-in completed.")
            try:
                pages = _list_cdp_pages(port)
            except OSError:
                time.sleep(0.3)
                continue

            for page in pages:
                page_url = str(page.get("url") or "")
                ws_url = str(page.get("webSocketDebuggerUrl") or "")
                if not ws_url or not page_url.startswith("https://gemini.google.com/"):
                    continue
                sock: Optional[_LocalCdpSocket] = None
                try:
                    sock = _LocalCdpSocket(ws_url, timeout=5.0)
                    ev = sock.call(
                        "Runtime.evaluate",
                        {"expression": _CHECK_SESSION_JS, "returnByValue": True},
                    )
                    val = ev.get("result", {}).get("result", {}).get("value")
                    if not isinstance(val, dict) or not val.get("authenticated"):
                        continue

                    c_gem = sock.call("Network.getCookies", {"urls": ["https://gemini.google.com/"]})
                    c_acc = sock.call(
                        "Network.getCookies",
                        {"urls": ["https://accounts.google.com/RotateCookies"]},
                    )
                    gem_list = c_gem.get("result", {}).get("cookies", []) or []
                    acc_list = c_acc.get("result", {}).get("cookies", []) or []
                    header = format_cdp_cookies(gem_list, acc_list)
                    jar: CookieJar = parse_cookie_header(header, account=int(val.get("account") or 0))
                    email = str(val.get("email") or "")
                    save_session(
                        jar.header,
                        profile=profile,
                        email=email,
                        account=jar.account,
                        path=session_path,
                    )
                    try:
                        sock.call("Browser.close")
                    except Exception:
                        pass
                    return SavedSession(
                        cookies=jar.header,
                        email=email,
                        account=jar.account,
                        updated_at=int(time.time()),
                        profile=_sanitize_profile(profile),
                    )
                except Exception:
                    continue
                finally:
                    if sock is not None:
                        sock.close()
            time.sleep(1.0)
        raise TimeoutError(f"Timed out waiting for Gemini sign-in ({int(timeout)}s).")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                proc.kill()


def refresh_from_browser(
    profile: str = "default",
    *,
    timeout: float = 45.0,
    browser_path: Optional[str] = None,
    session_path: Optional[os.PathLike[str] | str] = None,
    account: int = 0,
) -> Optional[SavedSession]:
    """Silently re-harvests fresh rotating cookies from an already signed-in browser profile."""
    if not has_browser_profile(profile):
        return None
    try:
        return browser_login(
            profile=profile,
            headless=True,
            timeout=timeout,
            browser_path=browser_path,
            session_path=session_path,
            account=account,
        )
    except Exception:
        return None
