"""Compute quota parsing (rpcid qpEbW and /usage fallback jSf9Qc)."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
from typing import Any, List

SHORT_WINDOW_MAX_MS = 6 * 60 * 60 * 1000
COMPUTE_ACTION = 2


@dataclass
class UsageWindow:
    valid: bool = False
    used_percent: int = 0
    resets_at_ms: int = 0


@dataclass
class UsageMeter:
    valid: bool = False
    short_window: UsageWindow = field(default_factory=UsageWindow)
    weekly: UsageWindow = field(default_factory=UsageWindow)


@dataclass
class QuotaRow:
    unit: int = 0
    used_percent: int = 0
    resets_at_ms: int = 0


def _reset_millis(val: Any) -> int:
    if not isinstance(val, list) or not val:
        return 0
    sec = val[0]
    if not isinstance(sec, (int, float)) or isinstance(sec, bool) or int(sec) <= 0:
        return 0
    nanos = val[1] if len(val) > 1 and isinstance(val[1], (int, float)) else 0
    nanos_int = int(nanos)
    if nanos_int < 0 or nanos_int > 999_999_999:
        nanos_int = 0
    return int(sec) * 1000 + nanos_int // 1_000_000


def parse_quota_rows(payload: str) -> List[QuotaRow]:
    try:
        root = json.loads(payload)
    except json.JSONDecodeError:
        return []
    if not isinstance(root, list):
        return []
    rows: List[QuotaRow] = []

    def walk(node: Any, depth: int) -> None:
        if depth > 4 or not isinstance(node, list):
            return
        if (
            len(node) >= 2
            and isinstance(node[0], list)
            and len(node[0]) >= 2
            and isinstance(node[0][1], (int, float))
            and not isinstance(node[0][1], bool)
            and int(node[0][1]) == COMPUTE_ACTION
            and isinstance(node[1], (int, float))
            and not isinstance(node[1], bool)
        ):
            pct = float(node[2]) if len(node) > 2 and isinstance(node[2], (int, float)) else 0.0
            pct = max(0.0, min(100.0, pct))
            reset_ms = _reset_millis(node[3]) if len(node) > 3 else 0
            rows.append(QuotaRow(unit=int(node[1]), used_percent=int(pct + 0.5), resets_at_ms=reset_ms))
            return
        for item in node:
            walk(item, depth + 1)

    walk(root, 0)
    return rows


def parse_usage_page_payload(payload: str) -> List[QuotaRow]:
    try:
        root = json.loads(payload)
    except json.JSONDecodeError:
        return []
    if not isinstance(root, list):
        return []
    rows: List[QuotaRow] = []

    def walk(node: Any, depth: int) -> None:
        if depth > 6 or not isinstance(node, list):
            return
        if (
            len(node) >= 4
            and isinstance(node[1], (int, float))
            and not isinstance(node[1], bool)
            and isinstance(node[2], (int, float))
            and not isinstance(node[2], bool)
            and isinstance(node[3], list)
            and node[3]
            and isinstance(node[3][0], list)
        ):
            frac = float(node[1])
            if 0.0 <= frac <= 1.5:
                reset_ms = _reset_millis(node[3][0])
                if reset_ms > 0:
                    pct = max(0.0, min(100.0, frac * 100.0))
                    rows.append(
                        QuotaRow(unit=int(node[2]), used_percent=int(pct + 0.5), resets_at_ms=reset_ms)
                    )
                    return
        for item in node:
            walk(item, depth + 1)

    walk(root, 0)
    return rows


def classify_quota_rows(rows: List[QuotaRow], now_ms: int) -> UsageMeter:
    meter = UsageMeter()
    for row in rows:
        window = UsageWindow(
            valid=True,
            used_percent=max(0, min(100, row.used_percent)),
            resets_at_ms=row.resets_at_ms,
        )
        if row.unit == 1 and (row.resets_at_ms == 0 or row.resets_at_ms <= now_ms + SHORT_WINDOW_MAX_MS):
            meter.short_window = window
        elif row.unit == 2:
            meter.weekly = window
    meter.valid = meter.short_window.valid or meter.weekly.valid
    return meter


def format_usage_span(milliseconds: int) -> str:
    if milliseconds <= 0:
        return "now"
    minutes = (milliseconds + 59_999) // 60_000
    if minutes < 60:
        return f"{minutes}m"
    hours, rem = divmod(minutes, 60)
    if hours < 24:
        return f"{hours}h{rem}m"
    days, rem_h = divmod(hours, 24)
    return f"{days}d{rem_h}h"


def usage_summary(meter: UsageMeter, now_ms: int) -> str:
    w = meter.short_window
    if not w.valid:
        return ""
    left = 100 - max(0, min(100, w.used_percent))
    if w.resets_at_ms > 0 and now_ms >= w.resets_at_ms:
        return "100%"
    if left == 100 or w.resets_at_ms <= 0:
        return f"{left}%"
    return f"{left}% {format_usage_span(w.resets_at_ms - now_ms)}"
