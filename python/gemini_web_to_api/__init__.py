"""GeminiWebToApi Python wrapper."""

from .auth import (
    SavedSession,
    browser_login,
    default_session_path,
    find_chromium_browser,
    load_session,
    refresh_from_browser,
    save_session,
)
from .client import (
    DEFAULT_MODEL,
    AskResult,
    AsyncGeminiWebClient,
    AuthRejectedError,
    GeminiWebClient,
    GeminiWebError,
)
from .cookies import CookieJar, merge_cookie_headers, parse_cookie_header, rotating_cookie_changed
from .models import (
    AccountTier,
    ModelEntry,
    WebModel,
    canonical_model_name,
    is_ultra_only_model,
    model_allowed_on_plan,
    model_slug,
    name_answers_to,
)
from .protocol import ChatThread, ReplyStreamScanner, strip_citations
from .server import create_server, parse_tool_calls
from .usage import QuotaRow, UsageMeter, UsageWindow, format_usage_span, usage_summary

__all__ = [
    "AccountTier",
    "AskResult",
    "AsyncGeminiWebClient",
    "AuthRejectedError",
    "ChatThread",
    "CookieJar",
    "DEFAULT_MODEL",
    "GeminiWebClient",
    "GeminiWebError",
    "ModelEntry",
    "QuotaRow",
    "ReplyStreamScanner",
    "SavedSession",
    "UsageMeter",
    "UsageWindow",
    "WebModel",
    "browser_login",
    "canonical_model_name",
    "create_server",
    "default_session_path",
    "find_chromium_browser",
    "format_usage_span",
    "is_ultra_only_model",
    "load_session",
    "merge_cookie_headers",
    "model_allowed_on_plan",
    "model_slug",
    "name_answers_to",
    "parse_cookie_header",
    "parse_tool_calls",
    "refresh_from_browser",
    "rotating_cookie_changed",
    "save_session",
    "strip_citations",
    "usage_summary",
]
