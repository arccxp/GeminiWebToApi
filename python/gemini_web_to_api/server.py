"""Local HTTP bridge exposing Gemini Web as OpenAI (/v1/chat/completions) and Anthropic (/v1/messages) APIs.

Works with Claude Code, Aider, Cline, Roo Code, OpenCode, Continue, OpenAI SDKs, and Anthropic SDKs.
"""

from __future__ import annotations

import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
import time
from typing import Any, Dict, List, Optional, Tuple
import uuid

from .client import DEFAULT_MODEL, GeminiWebClient, GeminiWebError
from .usage import usage_summary

_TOOL_CALL_FENCE_RE = re.compile(r"```tool_call\s*\n(.*?)\n```", re.DOTALL)


def _extract_images_and_text_openai(content: Any) -> Tuple[str, List[Tuple[bytes, str, str]]]:
    if isinstance(content, str):
        return content, []
    if not isinstance(content, list):
        return str(content or ""), []

    texts: List[str] = []
    images: List[Tuple[bytes, str, str]] = []
    for idx, part in enumerate(content):
        if isinstance(part, str):
            texts.append(part)
            continue
        if not isinstance(part, dict):
            continue
        ptype = part.get("type", "")
        if ptype == "text":
            texts.append(str(part.get("text", "")))
        elif ptype == "image_url":
            url_obj = part.get("image_url", {})
            url = url_obj.get("url", "") if isinstance(url_obj, dict) else str(url_obj)
            if url.startswith("data:") and ";base64," in url:
                header, b64 = url.split(";base64,", 1)
                mime = header[5:] or "image/png"
                ext = mime.split("/")[-1].split(";")[0] or "png"
                images.append((base64.b64decode(b64), f"image_{idx}.{ext}", mime))
    return "\n".join(t for t in texts if t), images


def _extract_images_and_text_anthropic(content: Any) -> Tuple[str, List[Tuple[bytes, str, str]]]:
    if isinstance(content, str):
        return content, []
    if not isinstance(content, list):
        return str(content or ""), []

    texts: List[str] = []
    images: List[Tuple[bytes, str, str]] = []
    for idx, block in enumerate(content):
        if isinstance(block, str):
            texts.append(block)
            continue
        if not isinstance(block, dict):
            continue
        btype = block.get("type", "")
        if btype == "text":
            texts.append(str(block.get("text", "")))
        elif btype == "image":
            src = block.get("source", {})
            if isinstance(src, dict) and src.get("type") == "base64":
                mime = str(src.get("media_type", "image/png"))
                ext = mime.split("/")[-1] or "png"
                images.append((base64.b64decode(src.get("data", "")), f"image_{idx}.{ext}", mime))
        elif btype == "tool_use":
            name = block.get("name", "")
            inp = json.dumps(block.get("input", {}), separators=(",", ":"))
            texts.append(f"[Called tool {name} with arguments: {inp}]")
        elif btype == "tool_result":
            tool_id = block.get("tool_use_id", "")
            res_content = block.get("content", "")
            if isinstance(res_content, list):
                res_text = "\n".join(
                    str(b.get("text", "")) for b in res_content if isinstance(b, dict) and b.get("type") == "text"
                )
            else:
                res_text = str(res_content)
            texts.append(f"[Tool result ({tool_id}):\n{res_text}]")
    return "\n".join(t for t in texts if t), images


def _format_tools_prompt(tools: List[Dict[str, Any]], format_kind: str) -> str:
    if not tools:
        return ""
    specs = []
    for t in tools:
        if format_kind == "openai" and t.get("type") == "function" and isinstance(t.get("function"), dict):
            fn = t["function"]
            specs.append(
                {
                    "name": fn.get("name", ""),
                    "description": fn.get("description", ""),
                    "parameters": fn.get("parameters", {}),
                }
            )
        else:
            specs.append(
                {
                    "name": t.get("name", ""),
                    "description": t.get("description", ""),
                    "parameters": t.get("input_schema", t.get("parameters", {})),
                }
            )
    return (
        "You have access to these tools:\n"
        + json.dumps(specs, indent=2)
        + "\n\nWhen you need to call a tool, respond with one or more fenced blocks in this exact format:\n"
        "```tool_call\n"
        '{"name": "tool_name", "input": {"arg": "value"}}\n'
        "```\n"
        "If no tool is needed, reply normally with plain text."
    )


def parse_tool_calls(text: str) -> Tuple[str, List[Dict[str, Any]]]:
    calls: List[Dict[str, Any]] = []

    def _consume(match: re.Match[str]) -> str:
        raw = match.group(1).strip()
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            return match.group(0)
        if isinstance(obj, dict) and isinstance(obj.get("name"), str):
            calls.append(
                {
                    "id": f"toolu_{uuid.uuid4().hex[:20]}",
                    "name": obj["name"],
                    "input": obj.get("input", obj.get("arguments", {})) or {},
                }
            )
            return ""
        return match.group(0)

    remaining = _TOOL_CALL_FENCE_RE.sub(_consume, text).strip()
    return remaining, calls


def _build_transcript(
    system_prompt: str,
    messages: List[Dict[str, Any]],
    *,
    format_kind: str,
    tools: Optional[List[Dict[str, Any]]] = None,
) -> Tuple[str, List[Tuple[bytes, str, str]]]:
    sections: List[str] = []
    all_images: List[Tuple[bytes, str, str]] = []

    if system_prompt:
        sections.append(f"System:\n{system_prompt}")
    if tools:
        sections.append(_format_tools_prompt(tools, format_kind))

    if len(messages) == 1 and not sections and messages[0].get("role") == "user":
        extractor = _extract_images_and_text_anthropic if format_kind == "anthropic" else _extract_images_and_text_openai
        text, imgs = extractor(messages[0].get("content", ""))
        return text, imgs

    extractor = _extract_images_and_text_anthropic if format_kind == "anthropic" else _extract_images_and_text_openai
    for msg in messages:
        role = str(msg.get("role", "user")).lower()
        if role == "system":
            text, _ = extractor(msg.get("content", ""))
            if text:
                sections.insert(0, f"System:\n{text}")
            continue
        if role == "tool":
            call_id = msg.get("tool_call_id", "")
            text, _ = extractor(msg.get("content", ""))
            sections.append(f"Tool result ({call_id}):\n{text}")
            continue
        text, imgs = extractor(msg.get("content", ""))
        all_images.extend(imgs)
        if role == "assistant" and isinstance(msg.get("tool_calls"), list):
            for tc in msg["tool_calls"]:
                fn = tc.get("function", {}) if isinstance(tc, dict) else {}
                text = (text + f"\n[Called tool {fn.get('name', '')}({fn.get('arguments', '{}')})]").strip()
        label = "Assistant" if role == "assistant" else "User"
        if text:
            sections.append(f"{label}:\n{text}")

    return "\n\n".join(sections), all_images


def _resolve_model(requested: str, default_model: str) -> str:
    req = (requested or "").strip()
    if not req:
        return default_model
    low = req.lower()
    # Map Claude/OpenAI alias names sent by coding agents directly to the configured Gemini model.
    if low.startswith(("claude-", "gpt-", "o1", "o3", "o4")):
        return default_model
    return req


def create_server(
    client: GeminiWebClient,
    *,
    host: str = "127.0.0.1",
    port: int = 8080,
    default_model: str = DEFAULT_MODEL,
    default_effort: str = "medium",
    api_key: Optional[str] = None,
) -> ThreadingHTTPServer:
    """Creates a local OpenAI + Anthropic-compatible HTTP bridge server."""

    class BridgeHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt: str, *args: object) -> None:
            return

        def _send_json(self, status: int, payload: Any) -> None:
            raw = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(raw)

        def _check_auth(self) -> bool:
            if not api_key:
                return True
            auth = self.headers.get("Authorization", "")
            x_key = self.headers.get("x-api-key", "")
            if auth == f"Bearer {api_key}" or x_key == api_key:
                return True
            self._send_json(401, {"error": {"message": "Invalid API key", "type": "authentication_error"}})
            return False

        def _read_json(self) -> Optional[Dict[str, Any]]:
            length = int(self.headers.get("Content-Length", "0") or "0")
            raw = self.rfile.read(length) if length > 0 else b"{}"
            try:
                data = json.loads(raw.decode("utf-8"))
                return data if isinstance(data, dict) else None
            except json.JSONDecodeError:
                return None

        def do_OPTIONS(self) -> None:
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header(
                "Access-Control-Allow-Headers",
                "Authorization, Content-Type, x-api-key, anthropic-version, anthropic-beta",
            )
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self) -> None:
            path = self.path.split("?", 1)[0]
            if path == "/health":
                self._send_json(200, {"status": "ok"})
                return
            if not self._check_auth():
                return
            if path in ("/v1/models", "/models"):
                try:
                    models, tier, email = client.list_models(force=False)
                except GeminiWebError as exc:
                    self._send_json(502, {"error": {"message": str(exc), "type": "upstream_error"}})
                    return
                now = int(time.time())
                self._send_json(
                    200,
                    {
                        "object": "list",
                        "tier": tier.label,
                        "email": email,
                        "data": [
                            {
                                "id": m.name or m.id,
                                "object": "model",
                                "created": now,
                                "owned_by": "google-gemini-web",
                                "label": m.label,
                                "available": m.available,
                            }
                            for m in models
                        ],
                    },
                )
                return
            if path in ("/v1/usage", "/usage"):
                try:
                    meter = client.get_usage()
                except GeminiWebError as exc:
                    self._send_json(502, {"error": {"message": str(exc), "type": "upstream_error"}})
                    return
                now_ms = int(time.time() * 1000)
                self._send_json(
                    200,
                    {
                        "summary": usage_summary(meter, now_ms),
                        "short_window": {
                            "valid": meter.short_window.valid,
                            "used_percent": meter.short_window.used_percent,
                            "resets_at_ms": meter.short_window.resets_at_ms,
                        },
                        "weekly": {
                            "valid": meter.weekly.valid,
                            "used_percent": meter.weekly.used_percent,
                            "resets_at_ms": meter.weekly.resets_at_ms,
                        },
                    },
                )
                return
            self._send_json(404, {"error": {"message": f"Not found: {path}"}})

        def do_POST(self) -> None:
            if not self._check_auth():
                return
            path = self.path.split("?", 1)[0]
            payload = self._read_json()
            if payload is None:
                self._send_json(400, {"error": {"message": "Invalid JSON body"}})
                return

            if path in ("/v1/chat/completions", "/chat/completions"):
                self._handle_openai_chat(payload)
            elif path in ("/v1/messages", "/messages"):
                self._handle_anthropic_messages(payload)
            elif path in ("/v1/messages/count_tokens", "/messages/count_tokens"):
                raw_len = len(json.dumps(payload))
                self._send_json(200, {"input_tokens": max(1, raw_len // 4)})
            else:
                self._send_json(404, {"error": {"message": f"Not found: {path}"}})

        def _handle_openai_chat(self, payload: Dict[str, Any]) -> None:
            messages = payload.get("messages", [])
            if not isinstance(messages, list) or not messages:
                self._send_json(400, {"error": {"message": "messages must be a non-empty list"}})
                return
            requested_model = str(payload.get("model", default_model))
            resolved_model = _resolve_model(requested_model, default_model)
            effort = str(payload.get("reasoning_effort", payload.get("effort", default_effort)))
            tools = payload.get("tools") if isinstance(payload.get("tools"), list) else None
            stream = bool(payload.get("stream", False))

            prompt, images = _build_transcript("", messages, format_kind="openai", tools=tools)
            completion_id = f"chatcmpl-{uuid.uuid4().hex[:24]}"
            created = int(time.time())

            if not stream:
                try:
                    res = client.ask(prompt, model=resolved_model, effort=effort, images=images or None)
                except GeminiWebError as exc:
                    self._send_json(502, {"error": {"message": str(exc), "type": "upstream_error"}})
                    return

                text_body = res.answer
                tool_calls_out = None
                finish_reason = "stop"
                if tools:
                    remaining, parsed_calls = parse_tool_calls(res.answer)
                    if parsed_calls:
                        text_body = remaining or None  # type: ignore[assignment]
                        finish_reason = "tool_calls"
                        tool_calls_out = [
                            {
                                "id": c["id"],
                                "type": "function",
                                "function": {
                                    "name": c["name"],
                                    "arguments": json.dumps(c["input"]),
                                },
                            }
                            for c in parsed_calls
                        ]

                msg_obj: Dict[str, Any] = {"role": "assistant", "content": text_body}
                if tool_calls_out:
                    msg_obj["tool_calls"] = tool_calls_out
                if res.thinking:
                    msg_obj["reasoning_content"] = res.thinking

                self._send_json(
                    200,
                    {
                        "id": completion_id,
                        "object": "chat.completion",
                        "created": created,
                        "model": requested_model,
                        "choices": [{"index": 0, "message": msg_obj, "finish_reason": finish_reason}],
                        "usage": {
                            "prompt_tokens": max(1, len(prompt) // 4),
                            "completion_tokens": max(1, len(res.answer) // 4),
                            "total_tokens": max(2, (len(prompt) + len(res.answer)) // 4),
                        },
                    },
                )
                return

            # Streaming SSE (OpenAI chat.completion.chunk)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            def send_chunk(delta: Dict[str, Any], finish: Optional[str] = None) -> None:
                chunk = {
                    "id": completion_id,
                    "object": "chat.completion.chunk",
                    "created": created,
                    "model": requested_model,
                    "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
                }
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode("utf-8"))
                self.wfile.flush()

            try:
                send_chunk({"role": "assistant"})
                if tools:
                    res = client.ask(prompt, model=resolved_model, effort=effort, images=images or None)
                    remaining, parsed_calls = parse_tool_calls(res.answer)
                    if remaining:
                        send_chunk({"content": remaining})
                    if parsed_calls:
                        for i, c in enumerate(parsed_calls):
                            send_chunk(
                                {
                                    "tool_calls": [
                                        {
                                            "index": i,
                                            "id": c["id"],
                                            "type": "function",
                                            "function": {
                                                "name": c["name"],
                                                "arguments": json.dumps(c["input"]),
                                            },
                                        }
                                    ]
                                }
                            )
                        send_chunk({}, finish="tool_calls")
                    else:
                        send_chunk({}, finish="stop")
                else:
                    for delta in client.stream(prompt, model=resolved_model, effort=effort, images=images or None):
                        send_chunk({"content": delta})
                    send_chunk({}, finish="stop")
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
            except GeminiWebError as exc:
                err_chunk = {"error": {"message": str(exc), "type": "upstream_error"}}
                self.wfile.write(f"data: {json.dumps(err_chunk)}\n\n".encode("utf-8"))
                self.wfile.flush()

        def _handle_anthropic_messages(self, payload: Dict[str, Any]) -> None:
            messages = payload.get("messages", [])
            if not isinstance(messages, list) or not messages:
                self._send_json(400, {"type": "error", "error": {"type": "invalid_request_error", "message": "messages required"}})
                return

            system_raw = payload.get("system", "")
            if isinstance(system_raw, list):
                system_text = "\n".join(
                    str(b.get("text", "")) for b in system_raw if isinstance(b, dict) and b.get("type") == "text"
                )
            else:
                system_text = str(system_raw or "")

            requested_model = str(payload.get("model", default_model))
            resolved_model = _resolve_model(requested_model, default_model)
            effort = str(payload.get("effort", default_effort))
            tools = payload.get("tools") if isinstance(payload.get("tools"), list) else None
            stream = bool(payload.get("stream", False))

            prompt, images = _build_transcript(system_text, messages, format_kind="anthropic", tools=tools)
            msg_id = f"msg_{uuid.uuid4().hex[:24]}"
            input_tokens = max(1, len(prompt) // 4)

            if not stream:
                try:
                    res = client.ask(prompt, model=resolved_model, effort=effort, images=images or None)
                except GeminiWebError as exc:
                    self._send_json(502, {"type": "error", "error": {"type": "api_error", "message": str(exc)}})
                    return

                content_blocks: List[Dict[str, Any]] = []
                stop_reason = "end_turn"
                if tools:
                    remaining, parsed_calls = parse_tool_calls(res.answer)
                    if remaining:
                        content_blocks.append({"type": "text", "text": remaining})
                    if parsed_calls:
                        stop_reason = "tool_use"
                        for c in parsed_calls:
                            content_blocks.append(
                                {"type": "tool_use", "id": c["id"], "name": c["name"], "input": c["input"]}
                            )
                else:
                    content_blocks.append({"type": "text", "text": res.answer})

                if not content_blocks:
                    content_blocks.append({"type": "text", "text": ""})

                self._send_json(
                    200,
                    {
                        "id": msg_id,
                        "type": "message",
                        "role": "assistant",
                        "model": requested_model,
                        "content": content_blocks,
                        "stop_reason": stop_reason,
                        "stop_sequence": None,
                        "usage": {
                            "input_tokens": input_tokens,
                            "output_tokens": max(1, len(res.answer) // 4),
                        },
                    },
                )
                return

            # Streaming SSE (Anthropic Messages API format used by Claude Code)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            def emit(event_name: str, data: Dict[str, Any]) -> None:
                self.wfile.write(f"event: {event_name}\ndata: {json.dumps(data)}\n\n".encode("utf-8"))
                self.wfile.flush()

            try:
                emit(
                    "message_start",
                    {
                        "type": "message_start",
                        "message": {
                            "id": msg_id,
                            "type": "message",
                            "role": "assistant",
                            "model": requested_model,
                            "content": [],
                            "stop_reason": None,
                            "stop_sequence": None,
                            "usage": {"input_tokens": input_tokens, "output_tokens": 0},
                        },
                    },
                )

                total_chars = 0
                stop_reason = "end_turn"
                block_index = 0

                if tools:
                    res = client.ask(prompt, model=resolved_model, effort=effort, images=images or None)
                    total_chars = len(res.answer)
                    remaining, parsed_calls = parse_tool_calls(res.answer)
                    if remaining:
                        emit(
                            "content_block_start",
                            {"type": "content_block_start", "index": block_index, "content_block": {"type": "text", "text": ""}},
                        )
                        emit(
                            "content_block_delta",
                            {"type": "content_block_delta", "index": block_index, "delta": {"type": "text_delta", "text": remaining}},
                        )
                        emit("content_block_stop", {"type": "content_block_stop", "index": block_index})
                        block_index += 1
                    if parsed_calls:
                        stop_reason = "tool_use"
                        for c in parsed_calls:
                            emit(
                                "content_block_start",
                                {
                                    "type": "content_block_start",
                                    "index": block_index,
                                    "content_block": {"type": "tool_use", "id": c["id"], "name": c["name"], "input": {}},
                                },
                            )
                            emit(
                                "content_block_delta",
                                {
                                    "type": "content_block_delta",
                                    "index": block_index,
                                    "delta": {"type": "input_json_delta", "partial_json": json.dumps(c["input"])},
                                },
                            )
                            emit("content_block_stop", {"type": "content_block_stop", "index": block_index})
                            block_index += 1
                else:
                    emit(
                        "content_block_start",
                        {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
                    )
                    for delta in client.stream(prompt, model=resolved_model, effort=effort, images=images or None):
                        total_chars += len(delta)
                        emit(
                            "content_block_delta",
                            {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": delta}},
                        )
                    emit("content_block_stop", {"type": "content_block_stop", "index": 0})

                emit(
                    "message_delta",
                    {
                        "type": "message_delta",
                        "delta": {"stop_reason": stop_reason, "stop_sequence": None},
                        "usage": {"output_tokens": max(1, total_chars // 4)},
                    },
                )
                emit("message_stop", {"type": "message_stop"})
            except GeminiWebError as exc:
                emit("error", {"type": "error", "error": {"type": "api_error", "message": str(exc)}})

    return ThreadingHTTPServer((host, port), BridgeHandler)
