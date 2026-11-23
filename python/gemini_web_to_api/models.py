"""Model discovery (rpcid otAQ7b), tier detection, and routing header construction."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import json
from typing import Any, List, Optional, Tuple


class AccountTier(IntEnum):
    UNKNOWN = -1
    FREE = 0
    PRO = 1
    ULTRA = 2

    @property
    def label(self) -> str:
        return {
            AccountTier.FREE: "Free",
            AccountTier.PRO: "Pro",
            AccountTier.ULTRA: "Ultra",
        }.get(self, "Unknown")


@dataclass
class WebModel:
    id: str
    label: str
    name: str
    available: bool = True
    note: str = ""


@dataclass
class ModelEntry:
    id: str = ""
    category: str = ""
    label: str = ""
    number: int = 1
    capacity: int = 1
    capacity_field: int = 12
    in_picker: bool = False
    available: bool = True
    note: str = ""


def model_slug(value: str) -> str:
    out: List[str] = []
    for ch in value:
        if ch.isalnum():
            out.append(ch.lower())
        elif out and out[-1] != "-":
            out.append("-")
    while out and out[-1] == "-":
        out.pop()
    return "".join(out)


def name_answers_to(requested: str, model_id: str, label: str, category: str) -> bool:
    want = requested.strip().lower()
    if not want:
        return False
    if want in (model_id.lower(), label.lower(), category.lower()):
        return True
    want_slug = model_slug(requested)
    if not want_slug:
        return False
    want_bare = want_slug[7:] if want_slug.startswith("gemini-") else want_slug
    targets = {model_slug(label), model_slug(category), model_slug(model_id)}
    return want_slug in targets or want_bare in targets


def canonical_model_name(label: str, number: int = 0) -> str:
    out: List[str] = []
    pending = False
    for ch in label:
        if ch.isalnum():
            if pending and out and out[-1] not in ("-", "."):
                out.append("-")
            pending = False
            out.append(ch.lower())
        elif ch == ".":
            if out and out[-1] not in ("-", "."):
                out.append(".")
        else:
            pending = True
    while out and out[-1] in ("-", "."):
        out.pop()
    s = "".join(out)
    if not s:
        return f"gemini-{number}" if number > 0 else ""
    if not s.startswith("gemini"):
        return f"gemini-{s}"
    if not s.startswith("gemini-"):
        return f"gemini-{s[6:]}"
    return s


def product_model_name(label: str, category: str, number: int = 0) -> str:
    return canonical_model_name(label or category, number)


def is_ultra_only_model(model_id: str, label: str, name: str) -> bool:
    return "carbon" in f"{model_id} {label} {name}".lower()


def model_allowed_on_plan(model_id: str, label: str, name: str, tier: AccountTier) -> bool:
    if not is_ultra_only_model(model_id, label, name):
        return True
    return tier >= AccountTier.ULTRA


def _at(seq: Any, idx: int) -> Any:
    if isinstance(seq, list) and 0 <= idx < len(seq):
        return seq[idx]
    return None


def _as_int(val: Any, fallback: int = 0) -> int:
    if isinstance(val, bool):
        return fallback
    if isinstance(val, int):
        return val
    if isinstance(val, float):
        return int(val)
    return fallback


def _as_str(val: Any) -> Optional[str]:
    return val if isinstance(val, str) else None


def _has_flag(lst: Any, flag: int) -> bool:
    if not isinstance(lst, list):
        return False
    return any(isinstance(x, (int, float)) and not isinstance(x, bool) and int(x) == flag for x in lst)


def compute_capacity(root: list) -> Tuple[int, int]:
    tier = _at(root, 16)
    caps = _at(root, 17)
    if _has_flag(tier, 21):
        return 1, 13
    if _has_flag(tier, 22):
        return 2, 13
    if _has_flag(caps, 115):
        return 4, 12
    if _has_flag(tier, 16) or _has_flag(caps, 106):
        return 3, 12
    if _has_flag(tier, 8) or _has_flag(caps, 19):
        return 2, 12
    return 1, 12


def _looks_like_label(s: str) -> bool:
    if len(s) < 3 or "://" in s:
        return False
    has_letter = any(c.isalpha() for c in s)
    if not has_letter:
        return False
    all_hex = all(c in "0123456789abcdefABCDEF" for c in s)
    if all_hex and " " not in s:
        return False
    return True


def _recover_label(entry: Any) -> str:
    stack = [entry]
    while stack:
        node = stack.pop()
        if isinstance(node, str) and _looks_like_label(node):
            return node
        if isinstance(node, list):
            for child in reversed(node):
                stack.append(child)
    return ""


def parse_models_from_payload(payload: str) -> Tuple[List[ModelEntry], AccountTier]:
    try:
        root = json.loads(payload)
    except json.JSONDecodeError:
        return [], AccountTier.UNKNOWN
    if not isinstance(root, list):
        return [], AccountTier.UNKNOWN

    capacity, capacity_field = compute_capacity(root)
    account_list = _at(root, 15)
    tier_flags = _at(root, 16)
    cap_flags = _at(root, 17)

    ultra_flag = _has_flag(cap_flags, 115) or _has_flag(tier_flags, 22)
    pro_flag = (
        _has_flag(tier_flags, 16)
        or _has_flag(tier_flags, 8)
        or _has_flag(cap_flags, 106)
        or _has_flag(cap_flags, 19)
    )
    free_flag = _has_flag(tier_flags, 21)

    tier = AccountTier.UNKNOWN
    if ultra_flag:
        tier = AccountTier.ULTRA
    elif pro_flag:
        tier = AccountTier.PRO
    elif free_flag:
        tier = AccountTier.FREE
    elif isinstance(account_list, list) and account_list:
        tier = AccountTier.PRO if (len(account_list) > 1 or capacity > 1) else AccountTier.FREE

    models: List[ModelEntry] = []
    if isinstance(account_list, list):
        for entry in account_list:
            if not isinstance(entry, list):
                continue
            model_id = _as_str(_at(entry, 0)) or ""
            number = _as_int(_at(entry, 17), _as_int(_at(entry, 9), 0))
            if not model_id and number <= 0:
                recovered = _recover_label(entry)
                if recovered:
                    models.append(
                        ModelEntry(
                            label=recovered,
                            number=0,
                            capacity=capacity,
                            capacity_field=capacity_field,
                            in_picker=True,
                        )
                    )
                continue
            category = _as_str(_at(entry, 1)) or _as_str(_at(entry, 10)) or ""
            display = _as_str(_at(entry, 11))
            if isinstance(_at(entry, 3), list) and category:
                display = category
                category = _as_str(_at(entry, 2)) or ""
            elif display is None:
                display = _as_str(_at(entry, 19))
            label = display if display else (category or model_id)
            if label:
                models.append(
                    ModelEntry(
                        id=model_id,
                        category=category,
                        label=label,
                        number=number if number > 0 else 1,
                        capacity=capacity,
                        capacity_field=capacity_field,
                        in_picker=True,
                    )
                )
        if models:
            return models, tier

    policy = _at(root, 24)
    catalog = _at(policy, 1) if isinstance(policy, list) else None
    if isinstance(catalog, list):
        for group in catalog:
            row = _at(group, 0) if isinstance(group, list) and _at(group, 0) is not None else group
            if not isinstance(row, list):
                continue
            number = _as_int(_at(row, 1), 0)
            if number <= 0:
                continue
            category = _as_str(_at(row, 4)) or ""
            label = _as_str(_at(row, 12)) or category
            if label:
                models.append(
                    ModelEntry(
                        number=number,
                        category=category,
                        label=label,
                        capacity=capacity,
                        capacity_field=capacity_field,
                    )
                )
    return models, tier


def fallback_model_table() -> List[ModelEntry]:
    return [ModelEntry(id="cf41b0e0dd7d53e5", label="Auto", number=6)]


def pick_model(models: List[ModelEntry], requested: str) -> Optional[ModelEntry]:
    if not models:
        return None
    want = requested.strip().lower()
    if not want or want in ("gemini-auto", "auto", "default"):
        auto_row = full_flash = non_lite = None
        for m in models:
            tag = f"{m.category} {m.label}".lower()
            lite = "lite" in tag
            flash = "flash" in tag
            if auto_row is None and "auto" in tag:
                auto_row = m
            if flash and not lite and full_flash is None:
                full_flash = m
            if not lite and non_lite is None:
                non_lite = m
        return auto_row or full_flash or non_lite or models[0]

    for m in models:
        if name_answers_to(requested, m.id, m.label, m.category) or name_answers_to(
            requested, m.id, product_model_name(m.label, m.category, m.number), m.category
        ):
            return m

    if want.isdigit() and len(want) <= 2:
        num = int(want)
        for m in models:
            if m.number == num:
                return m
    if want.startswith("gemini-") and want[7:].isdigit():
        num = int(want[7:])
        for m in models:
            if m.number == num:
                return m
    return None


def model_base_key(label: str, category: str, number: int = 0) -> str:
    text = (label or category).lower()
    tokens: List[str] = []
    buf: List[str] = []
    for ch in text:
        if ch.isalnum():
            buf.append(ch)
        else:
            if buf:
                tokens.append("".join(buf))
                buf = []
    if buf:
        tokens.append("".join(buf))
    kept = [t for t in tokens if t != "gemini" and not t.isdigit()]
    if not kept and number > 0:
        return f"n{number}"
    return "-".join(kept)


def build_model_header(model: ModelEntry, thinking_level: int, generation_id: str) -> str:
    header: List[Any] = [None] * 17
    header[0] = 1
    header[4] = model.id if model.id else None
    header[7] = 0
    header[8] = [4, 5, 6, 8]
    header[11] = [None, model.capacity] if model.capacity_field == 13 else model.capacity
    header[14] = model.number
    header[15] = 2 if thinking_level >= 2 else 1
    header[16] = generation_id
    return json.dumps(header, separators=(",", ":"))
