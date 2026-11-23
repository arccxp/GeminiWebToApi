"""Cookie parsing, multi-account slot detection, and live Set-Cookie rotation."""

from __future__ import annotations

from dataclasses import dataclass, field
import re
from typing import Dict, List, Optional, Tuple

ACCOUNTS_COOKIE_PREFIX = "GwaAccounts."
MAX_COOKIE_HEADER_BYTES = 32 * 1024
MAX_COOKIE_VALUE_BYTES = 4 * 1024

_NAME_RE = re.compile(r"^[A-Za-z0-9_\-.!#$%&*+^`|~]+$")


def _unwrap(text: str) -> str:
    value = text.strip()
    if value.startswith("-H "):
        value = value[3:].strip()
    while value and value[0] in ("'", '"'):
        value = value[1:]
    while value and value[-1] in ("'", '"', "\\"):
        value = value[:-1]
    if value.lower().startswith("cookie:"):
        value = value[7:].strip()
        while value and value[0] in ("'", '"'):
            value = value[1:]
        while value and value[-1] in ("'", '"'):
            value = value[:-1]
        value = value.strip()
    return value


def _cookie_value(raw: str) -> str:
    value = raw.strip()
    for sep in ("\r", "\n"):
        idx = value.find(sep)
        if idx != -1:
            value = value[:idx]
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1].strip()
    return value


def _split_pairs(header: str) -> List[Tuple[str, str]]:
    pairs: List[Tuple[str, str]] = []
    for part in header.split(";"):
        item = part.strip()
        if not item or "=" not in item:
            continue
        name, raw_val = item.split("=", 1)
        name = name.strip()
        val = _cookie_value(raw_val)
        if name and val and _NAME_RE.match(name):
            pairs.append((name, val))
    return pairs


def _join_pairs(pairs: List[Tuple[str, str]]) -> str:
    return "; ".join(f"{k}={v}" for k, v in pairs)


def header_cookie(header: str, name: str) -> str:
    for k, v in _split_pairs(header):
        if k == name:
            return v
    return ""


def _slot_session_cookie(slot: int) -> str:
    return f"__Secure-{slot + 1}PSID"


def _slot_paper_cookie(slot: int) -> str:
    return f"__Secure-{slot + 1}PAPISID"


def _has_session_cookie(header: str) -> bool:
    if header_cookie(header, "SID"):
        return True
    for slot in range(4):
        if header_cookie(header, _slot_session_cookie(slot)):
            return True
    return False


def _slot_is_login(header: str, slot: int, value: str) -> bool:
    if not value:
        return False
    if slot != 2:
        return True
    has_first = bool(header_cookie(header, _slot_session_cookie(0)))
    has_second = bool(header_cookie(header, _slot_session_cookie(1)))
    return not (has_first and not has_second)


@dataclass
class CookieJar:
    header: str
    sapisid: str = ""
    email: str = ""
    count: int = 0
    account: int = 0
    _live_header: str = field(default="", repr=False)

    def __post_init__(self) -> None:
        if not self._live_header:
            self._live_header = self.header

    @property
    def live_header(self) -> str:
        return self._live_header or self.header

    def set_live_header(self, new_header: str) -> None:
        self._live_header = new_header

    def scoped_header(self, for_accounts: bool = False) -> str:
        shared: List[Tuple[str, str]] = []
        accounts: List[Tuple[str, str]] = []
        for k, v in _split_pairs(self.live_header):
            if k.startswith(ACCOUNTS_COOKIE_PREFIX):
                if for_accounts:
                    accounts.append((k[len(ACCOUNTS_COOKIE_PREFIX):], v))
            else:
                shared.append((k, v))
        if not accounts:
            return _join_pairs(shared)
        return merge_cookie_headers(_join_pairs(shared), _join_pairs(accounts))

    def hash_key(self, account: Optional[int] = None) -> str:
        slot = self.account if account is None else account
        slot = max(0, slot)
        live = self.live_header
        if slot > 0:
            return header_cookie(live, _slot_paper_cookie(slot))
        for name in ("SAPISID", _slot_paper_cookie(0), "__Secure-3PAPISID"):
            val = header_cookie(live, name)
            if val:
                return val
        return self.sapisid

    def accounts(self) -> List[int]:
        live = self.live_header
        slots: List[int] = []
        for slot in range(4):
            val = header_cookie(live, _slot_session_cookie(slot))
            if _slot_is_login(live, slot, val):
                slots.append(slot)
        if not slots and self.hash_key(0):
            slots.append(0)
        return slots

    def absorb_set_cookies(
        self, set_cookie_headers: List[str], *, from_accounts: bool = False
    ) -> Optional[str]:
        if not set_cookie_headers:
            return None
        pairs = _split_pairs(self.live_header)
        changed = False

        for raw in set_cookie_headers:
            line = raw.strip()
            if line.lower().startswith("set-cookie:"):
                line = line[11:].strip()
            parts = line.split(";")
            if not parts or "=" not in parts[0]:
                continue
            name, raw_val = parts[0].split("=", 1)
            name = name.strip()
            value = _cookie_value(raw_val)
            if not name or not _NAME_RE.match(name) or len(value) > MAX_COOKIE_VALUE_BYTES:
                continue

            domain = ""
            max_age_expired = False
            for attr in parts[1:]:
                attr = attr.strip()
                if "=" not in attr:
                    continue
                ak, av = attr.split("=", 1)
                ak_low = ak.strip().lower()
                av_clean = av.strip().lower()
                if ak_low == "domain":
                    domain = av_clean
                elif ak_low == "max-age":
                    if av_clean == "0" or av_clean.startswith("-"):
                        max_age_expired = True

            if from_accounts and domain not in ("google.com", ".google.com"):
                name = ACCOUNTS_COOKIE_PREFIX + name

            deleted = (not value) or max_age_expired
            found = False
            for idx, (existing_name, existing_val) in enumerate(pairs):
                if existing_name != name:
                    continue
                found = True
                if deleted:
                    pairs[idx] = (existing_name, "")
                    changed = True
                elif existing_val != value:
                    pairs[idx] = (existing_name, value)
                    changed = True
                break
            if not found and not deleted:
                pairs.append((name, value))
                changed = True

        if not changed:
            return None

        kept = [(k, v) for k, v in pairs if v]
        new_header = _join_pairs(kept)
        if len(new_header.encode("utf-8")) > MAX_COOKIE_HEADER_BYTES or not _has_session_cookie(new_header):
            return None
        self._live_header = new_header
        return new_header


def parse_cookie_header(text: str, account: int = 0) -> CookieJar:
    value = _unwrap(text)
    if not value:
        raise ValueError("no cookies")
    if len(value.encode("utf-8")) > MAX_COOKIE_HEADER_BYTES:
        raise ValueError("cookie header too large")

    pairs = _split_pairs(value)
    if not pairs:
        raise ValueError("no cookies")

    header = _join_pairs(pairs)
    if len(header.encode("utf-8")) > MAX_COOKIE_HEADER_BYTES:
        raise ValueError("cookie header too large")
    if not _has_session_cookie(header):
        raise ValueError("no session cookie (expected SID or __Secure-1PSID)")

    sapisid = header_cookie(header, "SAPISID")
    return CookieJar(header=header, sapisid=sapisid, count=len(pairs), account=max(0, account))


def merge_cookie_headers(existing: str, incoming: str) -> str:
    merged: Dict[str, str] = {}
    order: List[str] = []
    for k, v in _split_pairs(existing):
        if k not in merged:
            order.append(k)
        merged[k] = v
    for k, v in _split_pairs(incoming):
        if k not in merged:
            order.append(k)
        merged[k] = v
    return _join_pairs([(k, merged[k]) for k in order])


def rotating_cookie_changed(previous: str, current: str, account: int = 0) -> bool:
    def _changed(cookie_name: str) -> bool:
        nxt = header_cookie(current, cookie_name)
        return bool(nxt) and nxt != header_cookie(previous, cookie_name)

    name = f"__Secure-{account + 1}PSIDTS"
    if _changed(name) or _changed(ACCOUNTS_COOKIE_PREFIX + name):
        return True
    if account == 0:
        return _changed("__Secure-3PSIDTS") or _changed(ACCOUNTS_COOKIE_PREFIX + "__Secure-3PSIDTS")
    return False
