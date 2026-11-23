# GeminiWebToApi

This repository is the standalone Gemini Web wrapper extracted from my private work. I originally wrote it in C++ for a private project, pulled the Gemini Web layer out on its own, and added matching zero-dependency wrappers for Python and Node.js plus a local HTTP bridge so coding tools and other languages can use it too.

It talks straight to `gemini.google.com` over HTTP using your browser session cookies. No browser automation is required for normal text, image, model-list, or quota calls.

## What is inside

- `cpp/` is the original C++17 wrapper (`gwa::Client` plus a C ABI shared library in `c_api.h` for Rust, Go, C#, or C callers). Uses WinHTTP on Windows and libcurl on Linux and macOS.
- `python/` is a Python 3.9+ wrapper (`GeminiWebClient` and `AsyncGeminiWebClient`) using only the standard library, plus a local HTTP bridge server.
- `nodejs/` is a Node.js 18+ ESM wrapper (`GeminiWebClient`) with TypeScript definitions (`index.d.ts`) and a matching local HTTP bridge server, with zero npm dependencies.
- `docs/agents.md` shows how to point Claude Code, Aider, Cline, Roo Code, OpenCode, and SDKs at the local bridge.
- `docs/web-to-api-guide.md` walks through how this wrapper works on the wire and how to build a web-to-API wrapper for coding workflows yourself.

## Getting your cookies

1. Open `https://gemini.google.com/app` in Chrome or Edge while signed in.
2. Open DevTools (F12), go to the Network tab, and reload the page.
3. Click any request to `gemini.google.com` and copy the `Cookie` request header (or right-click, Copy as cURL, and pass the `-H 'cookie: ...'` fragment; the parser unwraps it).
4. Put it in an environment variable so it stays out of shell history and git:

```bash
export GEMINI_COOKIE='__Secure-1PSID=...; __Secure-1PSIDTS=...; SAPISID=...'
```

PowerShell:

```powershell
$env:GEMINI_COOKIE = '__Secure-1PSID=...; __Secure-1PSIDTS=...; SAPISID=...'
```

If you are signed into multiple Google accounts in the same browser, pass `account=1` (or `GEMINI_ACCOUNT=1`) to select `/u/1/app` and sign with `__Secure-2PAPISID`.

Google rotates `__Secure-1PSIDTS` frequently. The wrapper absorbs `Set-Cookie` headers from every response and calls `accounts.google.com/RotateCookies` when a session goes stale or returns 401. Pass an `on_cookie_update` / `onCookieUpdate` / `SetCookieUpdateCallback` hook to save the updated header back to disk if you run long-lived processes.

## Python

No `pip install` is required if you add `python/` to `PYTHONPATH`, or install it editable with `pip install -e ./python`.

```python
import os
from gemini_web_to_api import GeminiWebClient

client = GeminiWebClient(os.environ["GEMINI_COOKIE"])

models, tier, email = client.list_models()
print(tier.label, email, [m.name for m in models])

# Single turn
res = client.ask("Explain why SAPISIDHASH uses SHA-1.", model="gemini-auto", effort="medium")
print(res.answer)

# Streaming deltas + multi-turn continuation
for delta in client.stream("Now show a 5-line Python implementation.", continue_chat=True):
    print(delta, end="", flush=True)
print()

# Image attachment (4-step resumable upload to content-push.googleapis.com)
res = client.ask("What error is in this screenshot?", images=["error.png"])
print(res.answer)

# Compute quota (5-hour rolling window and weekly meter)
meter = client.get_usage()
print(meter.short_window.used_percent, "% used in 5h window")
```

For `asyncio` code, import `AsyncGeminiWebClient` instead.

## Node.js and TypeScript

Requires Node.js 18+ (uses built-in `fetch` and `node:crypto`).

```js
import { GeminiWebClient } from "./nodejs/src/index.js";

const client = new GeminiWebClient(process.env.GEMINI_COOKIE);

const { models, tier, email } = await client.listModels();
console.log(tier, email, models.map((m) => m.name));

const res = await client.ask("Write a quick TCP echo server in Node.js.", {
  model: "gemini-auto",
  effort: "high",
  continueChat: true,
});
console.log(res.answer);

for await (const delta of client.stream("Add graceful SIGINT shutdown.", { continueChat: true })) {
  process.stdout.write(delta);
}
process.stdout.write("\n");
```

## C++ and C FFI

Build the static library (`gemini_web`), shared C ABI library (`gemini_web_ffi`), and unit tests with CMake:

```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
```

```cpp
#include <iostream>
#include "gemini_web/client.h"

int main() {
    gwa::Client client;
    std::string error;
    if (!client.Init(gwa::util::GetEnv("GEMINI_COOKIE"), 0, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::string answer;
    gwa::AskDetails details;
    if (!client.Ask("Summarize RFC 9110 caching rules.", answer, error,
                    "gemini-auto", "medium", {}, nullptr, true, &details)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << answer << "\n";
}
```

Other languages (Rust, Go, C#, Zig, C) can link `gemini_web_ffi` against `cpp/include/gemini_web/c_api.h`, or talk to the local HTTP bridge over localhost.

## Using with Claude Code and other AI coding agents

Start the local bridge with either Python or Node.js:

```bash
# Python
PYTHONPATH=python python -m gemini_web_to_api serve --port 8080

# Node.js
PORT=8080 node nodejs/src/server.js
```

The bridge exposes both wire formats on the same port:

- Anthropic Messages API: `POST http://127.0.0.1:8080/v1/messages` (plus `/v1/messages/count_tokens`)
- OpenAI Chat Completions API: `POST http://127.0.0.1:8080/v1/chat/completions` and `GET http://127.0.0.1:8080/v1/models`
- Quota meter: `GET http://127.0.0.1:8080/v1/usage`

Point Claude Code at the bridge:

```bash
ANTHROPIC_BASE_URL=http://127.0.0.1:8080 ANTHROPIC_AUTH_TOKEN=local claude
```

Point OpenAI-compatible coding tools (Aider, Cline, Roo Code, OpenCode, Continue, or the OpenAI SDK) at `http://127.0.0.1:8080/v1` with any non-empty API key (or set `BRIDGE_API_KEY` if you want the server to require a specific token). Model names that start with `claude-`, `gpt-`, `o1`, `o3`, or `o4` route to your configured `GEMINI_MODEL` (`gemini-auto` by default), while explicit `gemini-*` names route to that Gemini model.

See [docs/agents.md](docs/agents.md) for per-agent setup examples and tool-calling notes, and [docs/web-to-api-guide.md](docs/web-to-api-guide.md) for the full wire breakdown and how to wrap web LLM apps for coding workflows.

## Notes and limitations

- Gemini Web is an undocumented consumer endpoint. Google changes JSPB field indices, model IDs, and quota rules without notice.
- Some newer model rows only accept browser-minted `processSession` tokens and time out on direct `StreamGenerate` HTTP calls. The C++ wrapper leaves a `gwa::relay::RelayDriver` hook and reference DOM scripts (`cpp/src/relay.cpp`) if you want to plug in a headless browser fallback for those rows.
- Tool calling through the HTTP bridge works by translating tool schemas into a fenced `tool_call` prompt format and parsing tool blocks back into Anthropic `tool_use` or OpenAI `tool_calls` responses. It works well for simple read/edit/shell loops, though complex parallel tool schemas will be less reliable than a native API.

## Running the tests

All three implementations have offline test suites using mock HTTP transports:

```bash
# C++
cmake -S cpp -B cpp/build && cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure

# Python
python -m unittest discover -s python/tests -v

# Node.js
node --test nodejs/test/gemini_web.test.mjs
```

## License

GPL-3.0. See [LICENSE](LICENSE).
