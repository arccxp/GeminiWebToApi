"""Entrypoints: `python -m gemini_web_to_api login` and `python -m gemini_web_to_api serve`."""

from __future__ import annotations

import argparse
import os
import sys

from .auth import browser_login, default_session_path
from .client import DEFAULT_MODEL, GeminiWebClient
from .server import create_server


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m gemini_web_to_api")
    sub = parser.add_subparsers(dest="command")

    login = sub.add_parser("login", help="Sign into Gemini in a browser and save the session")
    login.add_argument("--profile", default=os.environ.get("GEMINI_PROFILE", "default"))
    login.add_argument("--account", type=int, default=0)
    login.add_argument("--browser", default=os.environ.get("GEMINI_BROWSER", ""))
    login.add_argument("--timeout", type=float, default=300.0)

    serve = sub.add_parser("serve", help="Start local OpenAI + Anthropic-compatible bridge")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8080)
    serve.add_argument("--profile", default=os.environ.get("GEMINI_PROFILE", "default"))
    serve.add_argument("--cookie", default=os.environ.get("GEMINI_COOKIE", ""))
    serve.add_argument("--account", type=int, default=None)
    serve.add_argument("--model", default=os.environ.get("GEMINI_MODEL", DEFAULT_MODEL))
    serve.add_argument("--effort", default=os.environ.get("GEMINI_EFFORT", "medium"))
    serve.add_argument("--api-key", default=os.environ.get("BRIDGE_API_KEY", ""))
    serve.add_argument("--no-auto-login", action="store_true")

    args = parser.parse_args()
    if args.command == "login":
        print("Opening browser for Gemini sign-in... Sign in and wait for the window to close.")
        saved = browser_login(
            profile=args.profile,
            headless=False,
            timeout=args.timeout,
            browser_path=args.browser or None,
            account=args.account,
        )
        who = f" ({saved.email})" if saved.email else ""
        print(f"Saved session for profile '{saved.profile}'{who} -> {default_session_path()}")
        return 0

    if args.command == "serve":
        if args.cookie:
            acc = int(os.environ.get("GEMINI_ACCOUNT", "0")) if args.account is None else args.account
            client = GeminiWebClient(args.cookie, account=acc)
        else:
            client = GeminiWebClient.from_session(
                profile=args.profile,
                auto_login=not args.no_auto_login,
                account=args.account,
            )
        server = create_server(
            client,
            host=args.host,
            port=args.port,
            default_model=args.model,
            default_effort=args.effort,
            api_key=args.api_key or None,
        )
        print(f"GeminiWebToApi bridge listening on http://{args.host}:{args.port}")
        print(f"  OpenAI endpoint:    http://{args.host}:{args.port}/v1/chat/completions")
        print(f"  Anthropic endpoint: http://{args.host}:{args.port}/v1/messages")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
