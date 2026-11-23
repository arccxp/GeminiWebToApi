"""Unit and HTTP bridge tests for gemini_web_to_api."""

from __future__ import annotations

import asyncio
import http.client
import json
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from gemini_web_to_api import (
    AccountTier,
    AsyncGeminiWebClient,
    GeminiWebClient,
    QuotaRow,
    canonical_model_name,
    create_server,
    is_ultra_only_model,
    model_allowed_on_plan,
    model_slug,
    name_answers_to,
    parse_cookie_header,
    parse_tool_calls,
    rotating_cookie_changed,
    strip_citations,
    usage_summary,
)
from gemini_web_to_api.auth import format_cdp_cookies, load_session, save_session
from gemini_web_to_api.client import HttpResult
from gemini_web_to_api.protocol import parse_bard_error
from gemini_web_to_api.usage import classify_quota_rows


def _make_mock_transport(answer_override: str | None = None):
    state = {"shell": 0, "models": 0, "generate": 0, "usage": 0, "upload": 0, "rotate": 0}

    def transport(method, url, headers, body, timeout, on_chunk):
        if method == "GET" and "/app" in url:
            state["shell"] += 1
            return HttpResult(
                status=200,
                body=(
                    '<html><script>window.WIZ_global_data = {'
                    '"SNlM0e":"at_123","cfb2h":"boq_build","FdrFJe":"-999",'
                    '"TuX5cc":"en","qKIAYe":"feeds/abc","oPEP7c":"user@example.com",'
                    '"tier":"gemini_advanced"};</script></html>'
                ),
                set_cookies=["__Secure-1PSIDTS=ts_rotated; Domain=.google.com; Path=/; Secure"],
            )

        if "rpcids=otAQ7b" in url:
            state["models"] += 1
            inner = json.dumps(
                [
                    None, None, None, None, None, None, None, None, None, None, None, None, None, None, None,
                    [
                        ["id_flash", "Fast", None, None, None, None, None, None, None, 1, None, "3.6 Flash", None, None, None, None, None, 1],
                        ["id_pro", "Pro", None, None, None, None, None, None, None, 3, None, "3.1 Pro", None, None, None, None, None, 3],
                    ],
                    [8],
                    [19],
                ]
            )
            return HttpResult(status=200, body=f")]}}'\n\n99\n[[\"wrb.fr\",\"otAQ7b\",{json.dumps(inner)}]]\n")

        if "StreamGenerate" in url:
            state["generate"] += 1
            final_text = answer_override or "Hello world [cite: 1]"
            f1_inner = json.dumps(
                [None, ["c_1", "r_1"], None, None, [["rc_1", [final_text[:5]], *([None] * 35), ["thinking..."]]]]
            )
            f2_inner = json.dumps(
                [
                    None,
                    ["c_1", "r_1"],
                    None,
                    None,
                    [["rc_1", [final_text], *([None] * 35), ["thinking done"]]],
                    *([None] * 37),
                    "3.6 Flash",
                    *([None] * 15),
                    1,
                ]
            )
            c1 = f")]}}'\n10\n[[\"wrb.fr\",null,{json.dumps(f1_inner)}]]\n"
            c2 = f"10\n[[\"wrb.fr\",null,{json.dumps(f2_inner)}]]\n"
            if on_chunk:
                on_chunk(c1)
                on_chunk(c2)
            return HttpResult(status=200, body=c1 + c2)

        if "rpcids=qpEbW" in url:
            state["usage"] += 1
            reset_sec = int(time.time()) + 7200
            inner = json.dumps([[[[None, 2], 1, 20, [reset_sec, 0]], [[None, 2], 2, 45, [reset_sec + 86400, 0]]]])
            return HttpResult(status=200, body=f")]}}'\n\n50\n[[\"wrb.fr\",\"qpEbW\",{json.dumps(inner)}]]\n")

        if "content-push.googleapis.com/upload" in url:
            state["upload"] += 1
            if method == "OPTIONS":
                return HttpResult(status=200, body="")
            if headers.get("X-Goog-Upload-Command") == "start":
                return HttpResult(status=200, body="", headers={"x-goog-upload-url": "https://content-push.googleapis.com/upload?id=u1"})
            return HttpResult(status=200, body="/contrib_service/ttl_1d/file_xyz")

        if "RotateCookies" in url:
            state["rotate"] += 1
            return HttpResult(
                status=200,
                body="",
                set_cookies=[
                    "__Secure-1PSIDTS=ts_from_rotate; Domain=.google.com; Path=/",
                    "__Host-GAPS=host_only_val; Path=/",
                ],
            )

        return HttpResult(status=404, body="")

    return transport, state


class TestGeminiWebPython(unittest.TestCase):
    def test_cookies(self) -> None:
        jar = parse_cookie_header(
            "-H 'cookie: SID=s1; SAPISID=k0; __Secure-1PSID=p1; __Secure-3PSID=p3; __Secure-1PSIDTS=old'"
        )
        self.assertEqual(jar.accounts(), [0])
        self.assertEqual(jar.hash_key(0), "k0")
        updated = jar.absorb_set_cookies(["__Secure-1PSIDTS=new; Domain=.google.com; Path=/"])
        self.assertIsNotNone(updated)
        assert updated is not None
        self.assertTrue(rotating_cookie_changed(jar.header, updated, 0))

        multi = parse_cookie_header("__Secure-1PSID=a; __Secure-1PAPISID=ka; __Secure-2PSID=b; __Secure-2PAPISID=kb")
        self.assertEqual(multi.accounts(), [0, 1])
        self.assertEqual(multi.hash_key(1), "kb")

    def test_models_and_usage(self) -> None:
        self.assertEqual(model_slug("3.6 Flash"), "3-6-flash")
        self.assertEqual(canonical_model_name("3.6 Flash", 1), "gemini-3.6-flash")
        self.assertTrue(name_answers_to("gemini-3.6-flash", "id1", "3.6 Flash", "Fast"))
        self.assertTrue(is_ultra_only_model("x", "4 Carbon", "gemini-4-carbon"))
        self.assertFalse(model_allowed_on_plan("x", "4 Carbon", "gemini-4-carbon", AccountTier.PRO))
        self.assertTrue(model_allowed_on_plan("x", "4 Carbon", "gemini-4-carbon", AccountTier.ULTRA))

        self.assertEqual(strip_citations("hi [cite: 1, 2] ok [cite_start]?code_stdout"), "hi  ok ")
        self.assertEqual(parse_bard_error('["BardErrorInfo",1097,"cant continue"]'), (1097, "cant continue"))

        now_ms = 1_700_000_000_000
        meter = classify_quota_rows([QuotaRow(1, 15, now_ms + 3600_000)], now_ms)
        self.assertEqual(usage_summary(meter, now_ms), "85% 1h0m")

    def test_client_flow(self) -> None:
        transport, state = _make_mock_transport()
        rotations = []
        client = GeminiWebClient(
            "__Secure-1PSID=p1; SAPISID=k0; __Secure-1PSIDTS=old",
            transport=transport,
            on_cookie_update=lambda acc, h: rotations.append((acc, h)),
        )

        models, tier, email = client.list_models()
        self.assertEqual(tier, AccountTier.PRO)
        self.assertEqual(email, "user@example.com")
        self.assertEqual([m.name for m in models], ["gemini-3.6-flash", "gemini-3.1-pro"])
        self.assertIn("__Secure-1PSIDTS=ts_rotated", client.live_cookies)
        self.assertTrue(rotations)

        res = client.ask("Hi", model="gemini-3.6-flash", effort="high", continue_chat=True)
        self.assertEqual(res.answer, "Hello world ")
        self.assertEqual(res.thinking, "thinking done")
        self.assertEqual(res.served_label, "3.6 Flash")
        self.assertTrue(client.thread.active)

        deltas = list(client.stream("Hi again", model="gemini-3.6-flash"))
        self.assertEqual("".join(deltas), "Hello world ")

        fid, fname = client.upload_image(b"\xff\xd8\xff\xe0", filename="shot.jpg", mime_type="image/jpeg")
        self.assertEqual(fid, "/contrib_service/ttl_1d/file_xyz")
        self.assertEqual(fname, "shot.jpg")

        meter = client.get_usage()
        self.assertEqual(meter.short_window.used_percent, 20)
        self.assertEqual(meter.weekly.used_percent, 45)

        self.assertTrue(client.rotate_cookies(force=True))
        self.assertIn("__Secure-1PSIDTS=ts_from_rotate", client.live_cookies)
        self.assertIn("GwaAccounts.__Host-GAPS=host_only_val", client.live_cookies)
        self.assertNotIn("__Host-GAPS", client.jar.scoped_header(for_accounts=False))
        self.assertIn("__Host-GAPS=host_only_val", client.jar.scoped_header(for_accounts=True))
        self.assertGreaterEqual(state["generate"], 2)

    def test_async_client(self) -> None:
        transport, _ = _make_mock_transport()
        aclient = AsyncGeminiWebClient("__Secure-1PSID=p1; SAPISID=k0", transport=transport)

        async def run():
            res = await aclient.ask("hello")
            chunks = [c async for c in aclient.stream("hello")]
            return res.answer, "".join(chunks)

        ans, streamed = asyncio.run(run())
        self.assertEqual(ans, "Hello world ")
        self.assertEqual(streamed, "Hello world ")

    def test_session_store_and_cdp_cookie_scoping(self) -> None:
        import os
        import tempfile

        header = format_cdp_cookies(
            [
                {"name": "__Secure-1PSID", "value": "ps1", "domain": ".google.com"},
                {"name": "SAPISID", "value": "sap1", "domain": ".google.com"},
            ],
            [
                {"name": "__Host-GAPS", "value": "gap_val", "domain": "accounts.google.com"},
                {"name": "__Secure-1PSIDTS", "value": "ts_1", "domain": ".google.com"},
            ],
        )
        self.assertIn("__Secure-1PSID=ps1", header)
        self.assertIn("GwaAccounts.__Host-GAPS=gap_val", header)

        with tempfile.TemporaryDirectory() as tmp:
            sess_file = Path(tmp) / "session.json"
            save_session(header, profile="work", email="work@example.com", account=0, path=sess_file)
            loaded = load_session("work", path=sess_file)
            self.assertIsNotNone(loaded)
            assert loaded is not None
            self.assertEqual(loaded.email, "work@example.com")
            self.assertEqual(loaded.cookies, header)

            # from_session loads session.json and auto-persists rotated Set-Cookie updates
            prev_env = os.environ.pop("GEMINI_COOKIE", None)
            try:
                transport, _ = _make_mock_transport()
                client = GeminiWebClient.from_session(
                    profile="work", auto_login=False, session_path=sess_file, transport=transport
                )
                client.list_models()
                reloaded = load_session("work", path=sess_file)
                assert reloaded is not None
                self.assertIn("__Secure-1PSIDTS=ts_rotated", reloaded.cookies)
            finally:
                if prev_env is not None:
                    os.environ["GEMINI_COOKIE"] = prev_env

    def test_tool_call_bridge_and_http_server(self) -> None:
        tool_reply = 'Checking files.\n```tool_call\n{"name": "Read", "input": {"path": "main.py"}}\n```'
        rem, calls = parse_tool_calls(tool_reply)
        self.assertEqual(rem, "Checking files.")
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "Read")

        transport, _ = _make_mock_transport(answer_override=tool_reply)
        client = GeminiWebClient("__Secure-1PSID=p1; SAPISID=k0", transport=transport)
        server = create_server(client, host="127.0.0.1", port=0)
        port = server.server_address[1]
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/v1/models")
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            data = json.loads(r.read().decode())
            self.assertEqual(data["object"], "list")
            self.assertEqual(len(data["data"]), 2)

            # OpenAI chat/completions with tool calling
            oai_req = json.dumps(
                {
                    "model": "gpt-4o",
                    "messages": [{"role": "user", "content": "Read main.py"}],
                    "tools": [{"type": "function", "function": {"name": "Read", "parameters": {}}}],
                }
            )
            conn.request("POST", "/v1/chat/completions", body=oai_req, headers={"Content-Type": "application/json"})
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            oai_body = json.loads(r.read().decode())
            self.assertEqual(oai_body["choices"][0]["finish_reason"], "tool_calls")
            self.assertEqual(oai_body["choices"][0]["message"]["tool_calls"][0]["function"]["name"], "Read")

            # Anthropic /v1/messages (Claude Code format) with tool_use block
            ant_req = json.dumps(
                {
                    "model": "claude-sonnet-4-20250514",
                    "max_tokens": 1024,
                    "messages": [{"role": "user", "content": "Read main.py"}],
                    "tools": [{"name": "Read", "input_schema": {"type": "object"}}],
                }
            )
            conn.request("POST", "/v1/messages", body=ant_req, headers={"Content-Type": "application/json"})
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            ant_body = json.loads(r.read().decode())
            self.assertEqual(ant_body["stop_reason"], "tool_use")
            self.assertEqual(ant_body["content"][1]["type"], "tool_use")
            self.assertEqual(ant_body["content"][1]["name"], "Read")

            # Anthropic SSE streaming
            ant_stream = json.dumps(
                {
                    "model": "claude-sonnet-4-20250514",
                    "stream": True,
                    "messages": [{"role": "user", "content": "Hi"}],
                }
            )
            conn.request("POST", "/v1/messages", body=ant_stream, headers={"Content-Type": "application/json"})
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            sse = r.read().decode()
            self.assertIn("event: message_start", sse)
            self.assertIn("event: content_block_delta", sse)
            self.assertIn("event: message_stop", sse)
            conn.close()
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
