"""GeminiWebToApi Python wrapper."""

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
    "UsageMeter",
    "UsageWindow",
    "WebModel",
    "canonical_model_name",
    "create_server",
    "format_usage_span",
    "is_ultra_only_model",
    "merge_cookie_headers",
    "model_allowed_on_plan",
    "model_slug",
    "name_answers_to",
    "parse_cookie_header",
    "parse_tool_calls",
    "rotating_cookie_changed",
    "strip_citations",
    "usage_summary",
]
