"""Wire protocol helpers: SAPISIDHASH, /app bootstrap, JSPB payloads, wrb.fr streaming."""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
import re
import time
from typing import Any, List, Optional, Tuple
from urllib.parse import quote

from .models import AccountTier

ORIGIN = "https://gemini.google.com"
BATCH_PATH = "/_/BardChatUi/data/batchexecute"
GENERATE_PATH = "/_/BardChatUi/data/assistant.lamda.BardFrontendService/StreamGenerate"
UPLOAD_URL = "https://content-push.googleapis.com/upload"
ROTATE_URL = "https://accounts.google.com/RotateCookies"

RPC_USER_STATUS = "otAQ7b"
RPC_QUOTA = "qpEbW"
RPC_USAGE_PAGE = "jSf9Qc"

UPLOAD_AUTHORIZATION = "Basic c2F2ZXM6cyNMdGhlNmxzd2F2b0RsN3J1d1U="
DEFAULT_PUSH_ID = "feeds/mcudyrk2a4khkz"
UPLOAD_FORM_TYPE = "application/x-www-form-urlencoded;charset=UTF-8"

MODEL_HEADER = "x-goog-ext-525001261-jspb"
SESSION_HEADER = "x-goog-ext-525005358-jspb"

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)


@dataclass
class WebSession:
    at: str = ""
    email: str = ""
    bl: str = ""
    sid: str = ""
    hl: str = "en"
    push: str = ""
    tier: AccountTier = AccountTier.UNKNOWN
    valid: bool = False

    @property
    def authenticated(self) -> bool:
        return self.valid and (bool(self.at) or bool(self.email))


@dataclass
class ChatThread:
    conversation: str = ""
    response: str = ""
    candidate: str = ""

    @property
    def active(self) -> bool:
        return bool(self.conversation and self.response and self.candidate)

    def clear(self) -> None:
        self.conversation = ""
        self.response = ""
        self.candidate = ""


@dataclass
class ReplyTurn:
    answer: str = ""
    thinking: str = ""
    served_label: str = ""
    served_number: int = 0
    chat: ChatThread = field(default_factory=ChatThread)


def url_encode(value: str) -> str:
    return quote(value, safe="-_.~")


def sapisidhash_header(sapisid: str, origin: str = ORIGIN, timestamp: Optional[int] = None) -> str:
    ts = int(time.time()) if timestamp is None else int(timestamp)
    digest = hashlib.sha1(f"{ts} {sapisid} {origin}".encode("utf-8")).hexdigest()
    return f"SAPISIDHASH {ts}_{digest}"


def thinking_level(effort: str) -> int:
    key = effort.strip().lower()
    if key in ("high", "extended"):
        return 2
    if key == "medium":
        return 4
    return 1


def scrape_token(html: str, key: str) -> str:
    quoted = f'"{key}"'
    pos = 0
    n = len(html)
    while True:
        hit = html.find(quoted, pos)
        if hit == -1:
            return ""
        cursor = hit + len(quoted)
        while cursor < n and html[cursor] in ":,] \t\n\r":
            cursor += 1
        if cursor < n and html[cursor] == '"':
            start = cursor + 1
            end = start
            while end < n:
                if html[end] == "\\":
                    end += 2
                    continue
                if html[end] == '"':
                    break
                end += 1
            if end < n and end > start:
                return html[start:end]
        pos = hit + len(quoted)


def tier_from_shell_html(html: str) -> AccountTier:
    if not html:
        return AccountTier.UNKNOWN
    low = html.lower()
    if any(
        needle in low
        for needle in ('"gemini_ultra"', '"gemini-ultra"', "ultra_tier_enrollment", "gemini_ultra_tier")
    ):
        return AccountTier.ULTRA
    if any(
        needle in low
        for needle in (
            '"gemini_advanced"',
            '"gemini-advanced"',
            "advanced_tier_enrollment",
            "gemini_advanced_tier",
            "advancedsubscriber",
        )
    ):
        return AccountTier.PRO
    if any(
        needle in low
        for needle in ("upgrade_to_advanced", "upgrade_to_gemini_advanced", "subscribe_to_advanced")
    ):
        return AccountTier.FREE
    return AccountTier.UNKNOWN


def parse_session_from_shell(html: str) -> WebSession:
    session = WebSession(
        at=scrape_token(html, "SNlM0e"),
        bl=scrape_token(html, "cfb2h"),
        sid=scrape_token(html, "FdrFJe"),
        hl=scrape_token(html, "TuX5cc") or "en",
        push=scrape_token(html, "qKIAYe"),
        email=scrape_token(html, "oPEP7c"),
        tier=tier_from_shell_html(html),
    )
    session.valid = bool(session.bl and session.sid)
    return session


def build_stream_generate_inner(
    prompt: str,
    locale: str,
    chat: ChatThread,
    request_uuid: str,
    model_number: int,
    level: int,
    file_ids: Optional[List[str]] = None,
    file_names: Optional[List[str]] = None,
) -> str:
    inner: List[Any] = [None] * 81
    if file_ids:
        names = file_names or [""] * len(file_ids)
        files = [[[fid], fname] for fid, fname in zip(file_ids, names)]
        inner[0] = [prompt, 0, None, files, None, None, 0]
    else:
        inner[0] = [prompt]
    inner[1] = [locale or "en"]
    conv = chat.conversation if chat.active else ""
    resp = chat.response if chat.active else ""
    cand = chat.candidate if chat.active else ""
    inner[2] = [conv, resp, cand, None, None, None, None, None, None, ""]
    inner[6] = [1]
    inner[7] = 1
    inner[10] = 1
    inner[11] = 0
    inner[17] = [[0]]
    inner[18] = 0
    inner[27] = 1
    inner[30] = [4]
    inner[41] = [1]
    inner[53] = 0
    inner[59] = request_uuid
    inner[61] = []
    inner[68] = 1
    inner[79] = model_number
    inner[80] = level
    return json.dumps(inner, separators=(",", ":"))


def build_stream_generate_form(inner_payload: str) -> str:
    wrapper = json.dumps([None, inner_payload], separators=(",", ":"))
    return "f.req=" + url_encode(wrapper)


def _find_record(value: Any, rpc_id: str, depth: int = 0) -> Optional[str]:
    if depth > 8 or not isinstance(value, list):
        return None
    if value and value[0] == "wrb.fr":
        if len(value) > 2 and value[1] == rpc_id and isinstance(value[2], str) and value[2]:
            return value[2]
        return None
    for item in value:
        hit = _find_record(item, rpc_id, depth + 1)
        if hit is not None:
            return hit
    return None


def find_rpc_payload(body: str, rpc_id: str) -> Optional[str]:
    decoder = json.JSONDecoder()
    cursor = 0
    n = len(body)
    if body.startswith(")]}'"):
        cursor = 4
    while cursor < n:
        start = body.find("[", cursor)
        if start == -1:
            return None
        try:
            frame, end = decoder.raw_decode(body, start)
        except json.JSONDecodeError:
            cursor = start + 1
            continue
        cursor = end
        hit = _find_record(frame, rpc_id)
        if hit is not None:
            return hit
    return None


def _collect_strings(value: Any, parts: List[str], depth: int = 0) -> None:
    if depth > 4:
        return
    if isinstance(value, str):
        parts.append(value)
    elif isinstance(value, list):
        for item in value:
            _collect_strings(item, parts, depth + 1)


def _answer_from_payload(payload: list) -> str:
    if len(payload) <= 4 or not isinstance(payload[4], list) or not payload[4]:
        return ""
    first = payload[4][0]
    if not isinstance(first, list) or len(first) <= 1:
        return ""
    slot = first[1]
    if isinstance(slot, str):
        return slot
    if isinstance(slot, list):
        return "".join(part for part in slot if isinstance(part, str))
    return ""


def _thinking_from_payload(payload: list) -> str:
    if len(payload) <= 4 or not isinstance(payload[4], list) or not payload[4]:
        return ""
    first = payload[4][0]
    if not isinstance(first, list) or len(first) <= 37 or first[37] is None:
        return ""
    parts: List[str] = []
    _collect_strings(first[37], parts)
    return "".join(parts)


def apply_reply_payload(payload: list, turn: ReplyTurn) -> bool:
    if len(payload) > 1 and isinstance(payload[1], list):
        ids = payload[1]
        if len(ids) > 0 and isinstance(ids[0], str) and ids[0]:
            turn.chat.conversation = ids[0]
        if len(ids) > 1 and isinstance(ids[1], str) and ids[1]:
            turn.chat.response = ids[1]
    if len(payload) > 4 and isinstance(payload[4], list) and payload[4]:
        first = payload[4][0]
        if isinstance(first, list) and first and isinstance(first[0], str) and first[0]:
            turn.chat.candidate = first[0]

    thinking = _thinking_from_payload(payload)
    if len(thinking) > len(turn.thinking):
        turn.thinking = thinking

    prev_len = len(turn.answer)
    candidate = _answer_from_payload(payload)
    if candidate and len(candidate) >= prev_len:
        turn.answer = candidate
        if len(payload) > 42 and isinstance(payload[42], str) and payload[42]:
            turn.served_label = payload[42]
        if len(payload) > 58 and isinstance(payload[58], (int, float)) and not isinstance(payload[58], bool):
            num = int(payload[58])
            if num > 0:
                turn.served_number = num
    return len(turn.answer) > prev_len


class ReplyStreamScanner:
    """Incremental wrb.fr JSON frame parser for live StreamGenerate chunks."""

    def __init__(self) -> None:
        self._buf = ""
        self._cursor = 0
        self._decoder = json.JSONDecoder()
        self.turn = ReplyTurn()
        self.saw_frame = False

    def feed(self, text: str) -> bool:
        self._buf += text
        grew = False
        while True:
            if self._cursor > 65536 and self._cursor > len(self._buf) // 2:
                self._buf = self._buf[self._cursor:]
                self._cursor = 0
            start = self._buf.find("[", self._cursor)
            if start == -1:
                self._cursor = len(self._buf)
                break
            self._cursor = start
            try:
                frame, end = self._decoder.raw_decode(self._buf, start)
            except json.JSONDecodeError:
                break
            self._cursor = end
            if not isinstance(frame, list):
                continue
            for record in frame:
                if not isinstance(record, list) or len(record) <= 2 or record[0] != "wrb.fr":
                    continue
                inner = record[2]
                if not isinstance(inner, str):
                    continue
                try:
                    payload = json.loads(inner)
                except json.JSONDecodeError:
                    continue
                if isinstance(payload, list):
                    self.saw_frame = True
                    if apply_reply_payload(payload, self.turn):
                        grew = True
        return grew


def extract_reply(body: str) -> ReplyTurn:
    scanner = ReplyStreamScanner()
    scanner.feed(body)
    return scanner.turn


def strip_citations(text: str) -> str:
    text = re.sub(r"\[cite:[^\]]*\]", "", text)
    text = text.replace("[cite_start]", "")
    text = text.replace("?code_reference", "")
    text = text.replace("?code_stdout", "")
    return text


def parse_bard_error(body: str) -> Optional[Tuple[int, str]]:
    idx = body.find("BardErrorInfo")
    if idx == -1:
        return None
    m = re.search(r"(\d+)", body[idx:])
    if not m:
        return None
    code = int(m.group(1))
    after = idx + m.end()
    q1 = body.find('"', after)
    msg = ""
    if q1 != -1:
        q2 = body.find('"', q1 + 1)
        if q2 != -1 and q2 > q1 + 1:
            msg = body[q1 + 1 : q2]
    return code, msg


def is_site_apology(text: str) -> bool:
    trimmed = text.strip()
    if not trimmed or len(trimmed) > 320:
        return False
    low = trimmed.lower()
    if (low.startswith("sorry") or "sorry," in low) and "try your request again" in low:
        return True
    return low.startswith("i encountered an error doing what you asked")


def is_site_model_error(text: str) -> bool:
    trimmed = text.strip()
    if not trimmed or len(trimmed) > 160:
        return False
    return trimmed.lower().startswith("i seem to be encountering an error")


def is_canned_failure(text: str) -> bool:
    return is_site_apology(text) or is_site_model_error(text)


def is_transient_error(error: str) -> bool:
    low = error.lower()
    if not low or "auth rejected (401)" in low:
        return False
    needles = (
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
    )
    return any(n in low for n in needles)
