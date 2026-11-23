"""Minimal entrypoint: `python -m gemini_web_to_api serve`."""

from __future__ import annotations

import argparse
import os
import sys

from .client import DEFAULT_MODEL, GeminiWebClient
from .server import create_server


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m gemini_web_to_api")
    sub = parser.add_subparsers(dest="command")

    serve = sub.add_parser("serve", help="Start local OpenAI + Anthropic-compatible bridge")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8080)
    serve.add_argument("--cookie", default=os.environ.get("GEMINI_COOKIE", ""))
    serve.add_argument("--account", type=int, default=int(os.environ.get("GEMINI_ACCOUNT", "0")))
    serve.add_argument("--model", default=os.environ.get("GEMINI_MODEL", DEFAULT_MODEL))
    serve.add_argument("--effort", default=os.environ.get("GEMINI_EFFORT", "medium"))
    serve.add_argument("--api-key", default=os.environ.get("BRIDGE_API_KEY", ""))

    args = parser.parse_args()
    if args.command != "serve":
        parser.print_help()
        return 1
    if not args.cookie:
        print("Error: set GEMINI_COOKIE or pass --cookie.", file=sys.stderr)
        return 1

    client = GeminiWebClient(args.cookie, account=args.account)
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


if __name__ == "__main__":
    raise SystemExit(main())
