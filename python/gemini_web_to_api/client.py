"""Standalone Gemini Web client (sync and async) using only the Python standard library."""

from __future__ import annotations

import asyncio
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
import http.client
import json
import mimetypes
import os
import random
import ssl
import threading
import time
from typing import AsyncIterator, Callable, Dict, Iterator, List, Optional, Tuple, Union
from urllib.parse import urlparse
import uuid
import zlib

from .cookies import CookieJar, header_cookie, parse_cookie_header
from .models import (
    AccountTier,
    ModelEntry,
    WebModel,
    build_model_header,
    fallback_model_table,
    is_ultra_only_model,
    model_allowed_on_plan,
    model_base_key,
    parse_models_from_payload,
    pick_model,
    product_model_name,
)
from .protocol import (
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
    ChatThread,
    ReplyStreamScanner,
    ReplyTurn,
    WebSession,
    build_stream_generate_form,
    build_stream_generate_inner,
    extract_reply,
    find_rpc_payload,
    is_canned_failure,
    is_site_apology,
    is_site_model_error,
    is_transient_error,
    parse_bard_error,
    parse_session_from_shell,
    sapisidhash_header,
    strip_citations,
    thinking_level,
    url_encode,
)
from .usage import (
    UsageMeter,
    classify_quota_rows,
    parse_quota_rows,
    parse_usage_page_payload,
)

DEFAULT_MODEL = "gemini-auto"
SESSION_TTL_SEC = 45 * 60
MODEL_TTL_SEC = 30 * 60


class GeminiWebError(RuntimeError):
    """Base error for Gemini Web wrapper failures."""


class AuthRejectedError(GeminiWebError):
    """Raised when Google rejects the cookie session (401/403 or signed-out shell)."""


@dataclass
class HttpResult:
    status: int
    body: str
    headers: Dict[str, str] = field(default_factory=dict)
    set_cookies: List[str] = field(default_factory=list)


@dataclass
class AskResult:
    answer: str
    thinking: str = ""
    served_label: str = ""
    served_number: int = 0
    thread: ChatThread = field(default_factory=ChatThread)
    elapsed_ms: int = 0


TransportFn = Callable[
    [str, str, Dict[str, str], Optional[bytes], float, Optional[Callable[[str], None]]],
    HttpResult,
]


def _default_transport(
    method: str,
    url: str,
    headers: Dict[str, str],
    body: Optional[bytes],
    timeout: float,
    on_chunk: Optional[Callable[[str], None]],
) -> HttpResult:
    parsed = urlparse(url)
    host = parsed.hostname or ""
    is_https = parsed.scheme.lower() == "https"
    port = parsed.port or (443 if is_https else 80)
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"

    req_headers = dict(headers)
    if "Accept-Encoding" not in req_headers and "accept-encoding" not in req_headers:
        req_headers["Accept-Encoding"] = "gzip, deflate"
    if on_chunk is not None:
        req_headers["Connection"] = "close"

    conn: http.client.HTTPConnection
    if is_https:
        ctx = ssl.create_default_context()
        conn = http.client.HTTPSConnection(host, port, timeout=timeout, context=ctx)
    else:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)

    try:
        conn.request(method, path, body=body, headers=req_headers)
        resp = conn.getresponse()
        status = resp.status
        hdr_map: Dict[str, str] = {}
        for k, v in resp.getheaders():
            hdr_map[k.lower()] = v
        set_cookies = resp.headers.get_all("Set-Cookie") or []

        encoding = hdr_map.get("content-encoding", "").lower().strip()
        decompressor = None
        if "gzip" in encoding:
            decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
        elif "deflate" in encoding:
            decompressor = zlib.decompressobj(-zlib.MAX_WBITS)

        chunks: List[bytes] = []
        if on_chunk is not None:
            while True:
                raw = resp.read1(8192) if hasattr(resp, "read1") else resp.read(8192)
                if not raw:
                    break
                data = decompressor.decompress(raw) if decompressor is not None else raw
                if data:
                    chunks.append(data)
                    on_chunk(data.decode("utf-8", errors="replace"))
            if decompressor is not None:
                tail = decompressor.flush()
                if tail:
                    chunks.append(tail)
                    on_chunk(tail.decode("utf-8", errors="replace"))
        else:
            raw = resp.read()
            data = (decompressor.decompress(raw) + decompressor.flush()) if decompressor else raw
            chunks.append(data)

        text = b"".join(chunks).decode("utf-8", errors="replace")
        return HttpResult(status=status, body=text, headers=hdr_map, set_cookies=set_cookies)
    finally:
        conn.close()


class GeminiWebClient:
    """Wrapper around Gemini's web RPCs (batchexecute, StreamGenerate, content-push, RotateCookies)."""

    def __init__(
        self,
        cookies: Union[str, CookieJar],
        *,
        account: int = 0,
        timeout: float = 120.0,
        generate_timeout: float = 300.0,
        allow_ultra: bool = False,
        on_cookie_update: Optional[Callable[[int, str], None]] = None,
        transport: Optional[TransportFn] = None,
    ) -> None:
        if isinstance(cookies, CookieJar):
            self.jar = cookies
            if account:
                self.jar.account = max(0, account)
        else:
            self.jar = parse_cookie_header(cookies, account=account)

        if self.jar.account > 0 and not self.jar.hash_key(self.jar.account):
            slot = self.jar.account + 1
            raise ValueError(
                f"Google slot {slot} is not present in these cookies (missing __Secure-{slot}PAPISID)"
            )

        self.timeout = timeout
        self.generate_timeout = generate_timeout
        self.allow_ultra = allow_ultra
        self.on_cookie_update = on_cookie_update
        self._transport: TransportFn = transport or _default_transport

        self._lock = threading.RLock()
        self._session: Optional[WebSession] = None
        self._session_at: float = 0.0
        self._models: List[ModelEntry] = []
        self._models_at: float = 0.0
        self._tier: AccountTier = AccountTier.UNKNOWN
        self._generation_id = str(uuid.uuid4()).upper()
        self._reqid = 100_000 + random.randint(0, 799_999)
        self._rotate_after: float = 0.0
        self.thread = ChatThread()

    @property
    def live_cookies(self) -> str:
        return self.jar.live_header

    def reset_chat(self) -> None:
        self.thread.clear()

    def reset_session(self) -> None:
        with self._lock:
            self._session = None
            self._session_at = 0.0
            self._models = []
            self._models_at = 0.0

    def _account_prefix(self) -> str:
        return f"/u/{self.jar.account}" if self.jar.account > 0 else ""

    def _next_reqid(self) -> int:
        with self._lock:
            val = self._reqid
            self._reqid += 100_000
            return val

    def _auth_headers(self) -> Dict[str, str]:
        slot = max(0, self.jar.account)
        key = self.jar.hash_key(slot)
        return {
            "Authorization": sapisidhash_header(key, ORIGIN),
            "X-Goog-AuthUser": str(slot),
            "Origin": ORIGIN,
            "Referer": f"{ORIGIN}{self._account_prefix()}/app",
            "User-Agent": USER_AGENT,
            "Accept-Language": "en-US,en;q=0.9",
            "Cookie": self.jar.scoped_header(for_accounts=False),
        }

    def _request_headers(self, model_header_val: str, session_uuid: str) -> Dict[str, str]:
        headers = self._auth_headers()
        headers.update(
            {
                "Content-Type": "application/x-www-form-urlencoded;charset=utf-8",
                "X-Same-Domain": "1",
                MODEL_HEADER: model_header_val,
                SESSION_HEADER: f'["{session_uuid}",1]',
                "X-Goog-Ext-73010989-jspb": "[0]",
                "X-Goog-Ext-73010990-jspb": "[0,0,0]",
            }
        )
        return headers

    def _absorb(self, result: HttpResult, *, from_accounts: bool = False) -> None:
        updated = self.jar.absorb_set_cookies(result.set_cookies, from_accounts=from_accounts)
        if updated and self.on_cookie_update:
            self.on_cookie_update(self.jar.account, updated)

    def _replace_form_or_query_field(self, text: str, name: str, value: str) -> str:
        parts = [p for p in text.split("&") if p and p.split("=", 1)[0] != name]
        if value:
            parts.append(f"{name}={url_encode(value)}")
        return "&".join(parts)

    def _authed_request(
        self,
        method: str,
        url: str,
        headers: Dict[str, str],
        body: Optional[bytes],
        timeout: float,
        on_chunk: Optional[Callable[[str], None]] = None,
        on_reset: Optional[Callable[[], None]] = None,
    ) -> HttpResult:
        res = self._transport(method, url, headers, body, timeout, on_chunk)
        rejected = res.status in (401, 403)
        shell_get = method == "GET" and url.endswith(f"{self._account_prefix()}/app")
        missing_shell = shell_get and res.status == 200 and not parse_session_from_shell(res.body).authenticated

        if not rejected and not missing_shell:
            if 200 <= res.status < 300:
                self._absorb(res)
            return res

        # Attempt single cookie rotation + shell validation, then replay once.
        if not self.rotate_cookies(force=True):
            return res
        fresh_shell = self._fetch_shell_raw(timeout=20.0)
        if not fresh_shell or not fresh_shell.authenticated:
            return res

        with self._lock:
            self._session = fresh_shell
            self._session_at = time.monotonic()

        if shell_get:
            return HttpResult(status=200, body=res.body, headers=res.headers)

        retry_headers = dict(headers)
        retry_headers.update(
            {
                "Authorization": sapisidhash_header(self.jar.hash_key(self.jar.account), ORIGIN),
                "Cookie": self.jar.scoped_header(for_accounts=False),
            }
        )
        retry_url = url
        retry_body = body
        if method == "POST" and "?" in url:
            base, query = url.split("?", 1)
            query = self._replace_form_or_query_field(query, "bl", fresh_shell.bl)
            query = self._replace_form_or_query_field(query, "f.sid", fresh_shell.sid)
            query = self._replace_form_or_query_field(query, "at", fresh_shell.at)
            retry_url = f"{base}?{query}"
            if body is not None:
                body_text = self._replace_form_or_query_field(body.decode("utf-8"), "at", fresh_shell.at)
                retry_body = body_text.encode("utf-8")
        if on_reset:
            on_reset()
        replay = self._transport(method, retry_url, retry_headers, retry_body, timeout, on_chunk)
        if 200 <= replay.status < 300:
            self._absorb(replay)
        return replay

    def _fetch_shell_raw(self, timeout: float = 30.0) -> Optional[WebSession]:
        headers = self._auth_headers()
        headers["Accept"] = "text/html,application/xhtml+xml"
        res = self._transport("GET", f"{ORIGIN}{self._account_prefix()}/app", headers, None, timeout, None)
        if not (200 <= res.status < 300):
            return None
        session = parse_session_from_shell(res.body)
        if not session.authenticated:
            return None
        self._absorb(res)
        return session

    def get_session(self, force: bool = False) -> WebSession:
        with self._lock:
            if not force and self._session and (time.monotonic() - self._session_at) < SESSION_TTL_SEC:
                return self._session

        headers = self._auth_headers()
        headers["Accept"] = "text/html,application/xhtml+xml"
        res = self._authed_request(
            "GET", f"{ORIGIN}{self._account_prefix()}/app", headers, None, min(self.timeout, 30.0)
        )
        if res.status in (401, 403):
            raise AuthRejectedError("Gemini Web: auth rejected (401).")
        if not (200 <= res.status < 300):
            raise GeminiWebError(f"Gemini Web: /app returned HTTP {res.status}")

        with self._lock:
            if self._session and self._session.authenticated and (time.monotonic() - self._session_at) < 5.0:
                return self._session

        session = parse_session_from_shell(res.body)
        if not session.authenticated:
            raise AuthRejectedError("Gemini Web: signed-in shell unavailable (cookies expired or signed out).")
        with self._lock:
            self._session = session
            self._session_at = time.monotonic()
            if session.email:
                self.jar.email = session.email
        return session

    def rotate_cookies(self, force: bool = False) -> bool:
        now = time.monotonic()
        with self._lock:
            if not force and now < self._rotate_after:
                return False
            self._rotate_after = now + 300.0

        slot = max(0, self.jar.account)
        psid = header_cookie(self.jar.live_header, f"__Secure-{slot + 1}PSID")
        if not psid and slot == 0:
            psid = self.jar.hash_key(0)
        if not psid:
            return False

        headers = {
            "X-Goog-AuthUser": str(slot),
            "User-Agent": USER_AGENT,
            "Accept-Language": "en-US,en;q=0.9",
            "Content-Type": "application/json",
            "Origin": "https://accounts.google.com",
            "Referer": "https://accounts.google.com/",
            "Cookie": self.jar.scoped_header(for_accounts=True),
        }
        primary_body = b'[000,"-0000000000000000000"]'
        res = self._transport("POST", ROTATE_URL, headers, primary_body, 30.0, None)
        if res.status in (400, 415):
            legacy = b'["ChromiumBrowser",null,null,null,0,"https://accounts.google.com",null,null,null,0]'
            res = self._transport("POST", ROTATE_URL, headers, legacy, 30.0, None)

        with self._lock:
            self._rotate_after = time.monotonic() + (900.0 if res.status == 429 else 60.0)
        if not (200 <= res.status < 300):
            return False
        self._absorb(res, from_accounts=True)
        return True

    def _build_batch_query(self, rpc_id: str, source_path: str, session: WebSession) -> str:
        q = (
            f"{self._account_prefix()}{BATCH_PATH}?rpcids={rpc_id}"
            f"&source-path={url_encode(self._account_prefix() + source_path)}"
            f"&hl={url_encode(session.hl)}&rt=c"
        )
        if session.at:
            q += f"&at={url_encode(session.at)}"
        if session.sid:
            q += f"&f.sid={url_encode(session.sid)}"
        if session.bl:
            q += f"&bl={url_encode(session.bl)}"
        return q

    def _load_model_table(self, session: WebSession, force: bool = False) -> Tuple[List[ModelEntry], AccountTier]:
        with self._lock:
            if not force and self._models and (time.monotonic() - self._models_at) < MODEL_TTL_SEC:
                return list(self._models), self._tier

        query = self._build_batch_query(RPC_USER_STATUS, "/app", session)
        req_json = f'[[["{RPC_USER_STATUS}","[null,null,null,null,null,null,null,null,null,null]",null,"generic"]]]'
        body_str = f"f.req={url_encode(req_json)}"
        if session.at:
            body_str = f"at={url_encode(session.at)}&{body_str}"
        headers = self._request_headers("[]", str(uuid.uuid4()).upper())
        res = self._authed_request("POST", f"{ORIGIN}{query}", headers, body_str.encode("utf-8"), 15.0)
        if res.status in (401, 403):
            self.reset_session()
            raise AuthRejectedError("Gemini Web: auth rejected (401).")
        if not (200 <= res.status < 300):
            return fallback_model_table(), session.tier

        payload = find_rpc_payload(res.body, RPC_USER_STATUS)
        if not payload:
            return fallback_model_table(), session.tier
        entries, tier = parse_models_from_payload(payload)
        if not entries:
            return fallback_model_table(), session.tier
        if tier == AccountTier.UNKNOWN:
            tier = AccountTier.PRO if len(entries) > 1 else AccountTier.FREE

        with self._lock:
            self._models = entries
            self._models_at = time.monotonic()
            self._tier = tier
            if tier >= AccountTier.ULTRA:
                self.allow_ultra = True
        return list(entries), tier

    def list_models(self, force: bool = True) -> Tuple[List[WebModel], AccountTier, str]:
        session = self.get_session()
        entries, tier = self._load_model_table(session, force=force)
        if tier == AccountTier.UNKNOWN and session.tier >= AccountTier.FREE:
            tier = session.tier
        allow_ultra = self.allow_ultra or tier >= AccountTier.ULTRA

        models: List[WebModel] = []
        seen_keys: Dict[str, int] = {}
        for e in entries:
            if not allow_ultra and is_ultra_only_model(e.id, e.category, e.label):
                continue
            model_id = e.id or (str(e.number) if e.number > 0 else "")
            label = e.label or e.category
            name = product_model_name(label, e.category, e.number)
            key = model_base_key(label, e.category, e.number)
            if key in seen_keys:
                existing = models[seen_keys[key]]
                if len(label) > len(existing.label):
                    existing.label = label
                if not existing.id:
                    existing.id = model_id
                continue
            seen_keys[key] = len(models)
            models.append(WebModel(id=model_id, label=label, name=name, available=e.available, note=e.note))
        return models, tier, session.email

    def upload_image(
        self,
        source: Union[str, os.PathLike, bytes],
        *,
        filename: Optional[str] = None,
        mime_type: Optional[str] = None,
        push_id: Optional[str] = None,
    ) -> Tuple[str, str]:
        if isinstance(source, (bytes, bytearray)):
            raw_bytes = bytes(source)
            fname = filename or "image.jpg"
            mime = mime_type or mimetypes.guess_type(fname)[0] or "image/jpeg"
        else:
            path = os.fspath(source)
            with open(path, "rb") as f:
                raw_bytes = f.read()
            fname = filename or os.path.basename(path)
            mime = mime_type or mimetypes.guess_type(path)[0] or "image/jpeg"

        if not raw_bytes:
            raise GeminiWebError("Gemini Web: cannot upload empty file")

        size = len(raw_bytes)
        effective_push = push_id or (self._session.push if self._session else "") or DEFAULT_PUSH_ID

        def make_headers(command: Optional[str], content_type: str) -> Dict[str, str]:
            h = {
                "Accept": "*/*",
                "Authorization": UPLOAD_AUTHORIZATION,
                "Content-Type": content_type,
                "Origin": ORIGIN,
                "Referer": f"{ORIGIN}/",
                "Push-ID": effective_push,
                "X-Goog-Upload-Header-Content-Length": str(size),
                "X-Goog-Upload-Protocol": "resumable",
                "X-Tenant-Id": "bard-storage",
                "User-Agent": USER_AGENT,
            }
            if command:
                h["X-Goog-Upload-Command"] = command
            cookie = self.jar.scoped_header(for_accounts=False)
            if cookie:
                h["Cookie"] = cookie
            return h

        r1 = self._transport("OPTIONS", UPLOAD_URL, make_headers(None, UPLOAD_FORM_TYPE), b"", self.timeout, None)
        if not (200 <= r1.status < 300):
            raise GeminiWebError(f"Gemini Web: upload preflight failed (HTTP {r1.status})")

        start_headers = make_headers("start", UPLOAD_FORM_TYPE)
        start_headers["Size"] = str(size)
        r2 = self._transport(
            "POST", UPLOAD_URL, start_headers, f"File name: {fname}".encode("utf-8"), self.timeout, None
        )
        if not (200 <= r2.status < 300):
            raise GeminiWebError(f"Gemini Web: upload start failed (HTTP {r2.status})")

        upload_url = r2.headers.get("x-goog-upload-url", "")
        if not upload_url:
            raise GeminiWebError("Gemini Web: missing X-Goog-Upload-Url header")

        r3 = self._transport("OPTIONS", upload_url, start_headers, b"", self.timeout, None)
        if not (200 <= r3.status < 300):
            raise GeminiWebError(f"Gemini Web: upload target preflight failed (HTTP {r3.status})")

        final_headers = make_headers("upload, finalize", mime)
        final_headers["Size"] = str(size)
        final_headers["X-Goog-Upload-Offset"] = "0"
        r4 = self._transport("POST", upload_url, final_headers, raw_bytes, self.timeout, None)
        if not (200 <= r4.status < 300):
            raise GeminiWebError(f"Gemini Web: upload finalize failed (HTTP {r4.status})")

        file_id = r4.body.strip().strip("\"'")
        if not file_id:
            raise GeminiWebError("Gemini Web: empty file id returned from upload")
        return file_id, fname

    def _run_ask_once(
        self,
        prompt: str,
        model: str,
        effort: str,
        images: Optional[List[Union[str, os.PathLike, Tuple[bytes, str, str]]]],
        chat: ChatThread,
        on_partial: Optional[Callable[[str], None]],
    ) -> AskResult:
        session = self.get_session()
        table, tier = self._load_model_table(session, force=False)
        selected = pick_model(table, model) or pick_model(table, DEFAULT_MODEL)
        if selected is None:
            raise GeminiWebError("No model rows available for this account.")

        effective_tier = AccountTier.ULTRA if (self.allow_ultra or tier >= AccountTier.ULTRA) else tier
        if not model_allowed_on_plan(selected.id, selected.category, selected.label, effective_tier):
            raise GeminiWebError(f"{selected.label} requires an Ultra plan.")

        file_ids: List[str] = []
        file_names: List[str] = []
        if images:
            def _upload_one(item: Union[str, os.PathLike, Tuple[bytes, str, str]]) -> Tuple[str, str]:
                if isinstance(item, tuple):
                    raw, fname, mime = item
                    return self.upload_image(raw, filename=fname, mime_type=mime, push_id=session.push)
                return self.upload_image(item, push_id=session.push)

            with ThreadPoolExecutor(max_workers=min(len(images), 4)) as pool:
                for fid, fname in pool.map(_upload_one, images):
                    file_ids.append(fid)
                    file_names.append(fname)

        req_uuid = str(uuid.uuid4()).upper()
        level = thinking_level(effort)
        inner = build_stream_generate_inner(
            prompt=prompt,
            locale=session.hl,
            chat=chat,
            request_uuid=req_uuid,
            model_number=selected.number,
            level=level,
            file_ids=file_ids or None,
            file_names=file_names or None,
        )
        form = build_stream_generate_form(inner)
        if session.at:
            form = f"at={url_encode(session.at)}&{form}"

        query = (
            f"{self._account_prefix()}{GENERATE_PATH}?hl={url_encode(session.hl)}"
            f"&_reqid={self._next_reqid()}&rt=c"
        )
        if session.at:
            query += f"&at={url_encode(session.at)}"
        if session.bl:
            query += f"&bl={url_encode(session.bl)}"
        if session.sid:
            query += f"&f.sid={url_encode(session.sid)}"

        headers = self._request_headers(
            build_model_header(selected, level, self._generation_id), req_uuid
        )

        scanner = ReplyStreamScanner()
        emitted_len = 0

        def handle_chunk(chunk_text: str) -> None:
            nonlocal emitted_len
            if scanner.feed(chunk_text) and on_partial is not None:
                text = scanner.turn.answer
                if emitted_len == 0 or len(text) >= emitted_len + 32:
                    emitted_len = len(text)
                    on_partial(strip_citations(text))

        def handle_reset() -> None:
            nonlocal scanner, emitted_len
            scanner = ReplyStreamScanner()
            emitted_len = 0

        started = time.monotonic()
        res = self._authed_request(
            "POST",
            f"{ORIGIN}{query}",
            headers,
            form.encode("utf-8"),
            self.generate_timeout,
            on_chunk=handle_chunk,
            on_reset=handle_reset,
        )
        elapsed_ms = int((time.monotonic() - started) * 1000)

        if res.status in (401, 403):
            self.reset_session()
            raise AuthRejectedError("Gemini Web: auth rejected (401).")
        if res.status < 200 or res.status >= 300:
            if res.status == 400:
                self.reset_session()
            raise GeminiWebError(f"Gemini Web: HTTP {res.status}")

        turn: ReplyTurn = scanner.turn
        if not turn.answer and res.body:
            turn = extract_reply(res.body)

        if turn.chat.active:
            chat.conversation = turn.chat.conversation
            chat.response = turn.chat.response
            chat.candidate = turn.chat.candidate
        else:
            chat.clear()

        if turn.answer.strip() and is_site_model_error(turn.answer):
            raise GeminiWebError(f"Gemini Web: {selected.label} did not answer. Pick another model.")
        if turn.answer.strip() and is_site_apology(turn.answer):
            raise GeminiWebError("Gemini Web: the site failed the request - try again.")

        if not turn.answer.strip():
            err_info = parse_bard_error(res.body)
            if err_info:
                code, msg = err_info
                if code == 1097:
                    chat.clear()
                raise GeminiWebError(f"Gemini Web: {msg or f'request refused (code {code})'}")
            raise GeminiWebError("Could not parse the Gemini response.")

        clean = strip_citations(turn.answer)
        if on_partial is not None and len(clean) > emitted_len:
            on_partial(clean)

        return AskResult(
            answer=clean,
            thinking=turn.thinking,
            served_label=turn.served_label or selected.label,
            served_number=turn.served_number or selected.number,
            thread=ChatThread(chat.conversation, chat.response, chat.candidate),
            elapsed_ms=elapsed_ms,
        )

    def ask(
        self,
        prompt: str,
        *,
        model: str = DEFAULT_MODEL,
        effort: str = "medium",
        images: Optional[List[Union[str, os.PathLike, Tuple[bytes, str, str]]]] = None,
        continue_chat: bool = False,
        thread: Optional[ChatThread] = None,
        on_partial: Optional[Callable[[str], None]] = None,
        retries: int = 2,
    ) -> AskResult:
        active_thread = thread if thread is not None else (self.thread if continue_chat else ChatThread())
        last_err: Optional[Exception] = None
        for attempt in range(retries + 1):
            try:
                return self._run_ask_once(prompt, model, effort, images, active_thread, on_partial)
            except AuthRejectedError:
                raise
            except GeminiWebError as exc:
                last_err = exc
                if attempt >= retries or not is_transient_error(str(exc)):
                    raise
                if not continue_chat and thread is None:
                    active_thread.clear()
                self.reset_session()
                time.sleep(0.25 if attempt == 0 else 0.7)
        assert last_err is not None
        raise last_err

    def stream(
        self,
        prompt: str,
        *,
        model: str = DEFAULT_MODEL,
        effort: str = "medium",
        images: Optional[List[Union[str, os.PathLike, Tuple[bytes, str, str]]]] = None,
        continue_chat: bool = False,
        thread: Optional[ChatThread] = None,
    ) -> Iterator[str]:
        """Yields incremental text deltas as Gemini streams frames."""
        import queue

        q: queue.Queue[Optional[Tuple[str, object]]] = queue.Queue()
        sent_len = 0

        def on_partial(snapshot: str) -> None:
            nonlocal sent_len
            if len(snapshot) > sent_len:
                delta = snapshot[sent_len:]
                sent_len = len(snapshot)
                q.put(("delta", delta))

        def worker() -> None:
            try:
                res = self.ask(
                    prompt,
                    model=model,
                    effort=effort,
                    images=images,
                    continue_chat=continue_chat,
                    thread=thread,
                    on_partial=on_partial,
                )
                if len(res.answer) > sent_len:
                    q.put(("delta", res.answer[sent_len:]))
                q.put(("done", res))
            except Exception as exc:
                q.put(("error", exc))

        t = threading.Thread(target=worker, daemon=True)
        t.start()
        while True:
            kind, payload = q.get()
            if kind == "delta":
                yield str(payload)
            elif kind == "error":
                raise payload  # type: ignore[misc]
            else:
                break

    def get_usage(self) -> UsageMeter:
        session = self.get_session()
        now_ms = int(time.time() * 1000)
        query = self._build_batch_query(RPC_QUOTA, "/app", session)
        headers = self._request_headers("[]", str(uuid.uuid4()).upper())

        meter = UsageMeter()
        for shape in ("[[[1,2]]]", "[]"):
            req_json = f'[[["{RPC_QUOTA}","{shape}",null,"generic"]]]'
            body_str = f"f.req={url_encode(req_json)}"
            if session.at:
                body_str = f"at={url_encode(session.at)}&{body_str}"
            res = self._authed_request("POST", f"{ORIGIN}{query}", headers, body_str.encode("utf-8"), 30.0)
            if res.status in (401, 403):
                self.reset_session()
                raise AuthRejectedError("Gemini Web: auth rejected (401).")
            if not (200 <= res.status < 300):
                raise GeminiWebError(f"Gemini Web: usage RPC returned HTTP {res.status}")
            payload = find_rpc_payload(res.body, RPC_QUOTA)
            if payload:
                rows = parse_quota_rows(payload)
                if rows:
                    meter = classify_quota_rows(rows, now_ms)
                    if meter.short_window.valid:
                        return meter

        if not meter.short_window.valid:
            page_query = self._build_batch_query(RPC_USAGE_PAGE, "/usage", session)
            page_req = f'[[["{RPC_USAGE_PAGE}","[]",null,"generic"]]]'
            page_body = f"f.req={url_encode(page_req)}"
            if session.at:
                page_body = f"at={url_encode(session.at)}&{page_body}"
            res = self._authed_request(
                "POST", f"{ORIGIN}{page_query}", headers, page_body.encode("utf-8"), 30.0
            )
            if 200 <= res.status < 300:
                payload = find_rpc_payload(res.body, RPC_USAGE_PAGE)
                if payload:
                    rows = parse_usage_page_payload(payload)
                    if rows:
                        meter = classify_quota_rows(rows, now_ms)

        if not meter.valid:
            raise GeminiWebError("No quota rows returned by Gemini.")
        return meter


class AsyncGeminiWebClient:
    """Async wrapper over GeminiWebClient for asyncio apps."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        self._sync = GeminiWebClient(*args, **kwargs)  # type: ignore[arg-type]

    @property
    def live_cookies(self) -> str:
        return self._sync.live_cookies

    def reset_chat(self) -> None:
        self._sync.reset_chat()

    async def list_models(self, force: bool = True) -> Tuple[List[WebModel], AccountTier, str]:
        return await asyncio.to_thread(self._sync.list_models, force)

    async def ask(self, prompt: str, **kwargs: object) -> AskResult:
        return await asyncio.to_thread(lambda: self._sync.ask(prompt, **kwargs))  # type: ignore[arg-type]

    async def get_usage(self) -> UsageMeter:
        return await asyncio.to_thread(self._sync.get_usage)

    async def rotate_cookies(self, force: bool = False) -> bool:
        return await asyncio.to_thread(self._sync.rotate_cookies, force)

    async def stream(self, prompt: str, **kwargs: object) -> AsyncIterator[str]:
        loop = asyncio.get_running_loop()
        q: asyncio.Queue[Tuple[str, object]] = asyncio.Queue()

        def producer() -> None:
            try:
                for delta in self._sync.stream(prompt, **kwargs):  # type: ignore[arg-type]
                    loop.call_soon_threadsafe(q.put_nowait, ("delta", delta))
                loop.call_soon_threadsafe(q.put_nowait, ("done", None))
            except Exception as exc:
                loop.call_soon_threadsafe(q.put_nowait, ("error", exc))

        threading.Thread(target=producer, daemon=True).start()
        while True:
            kind, payload = await q.get()
            if kind == "delta":
                yield str(payload)
            elif kind == "error":
                raise payload  # type: ignore[misc]
            else:
                break
