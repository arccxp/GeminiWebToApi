# GeminiWebToApi

This repository is the standalone Gemini Web wrapper extracted from my private work. I originally wrote it in C++ for a private project, pulled the Gemini Web layer out on its own, and added matching zero-dependency wrappers for Python and Node.js plus a local HTTP bridge so coding tools and other languages can use it too.

It talks straight to `gemini.google.com` over HTTP using your Google session cookies. A built-in browser login command signs you in once, saves the session locally, and refreshes rotating cookies automatically so you do not have to dig through DevTools every time.

## What is inside

- `cpp/` is the original C++17 wrapper (`gwa::Client` plus a C ABI shared library in `c_api.h` for Rust, Go, C#, or C callers). Uses WinHTTP on Windows and libcurl on Linux and macOS.
- `python/` is a Python 3.9+ wrapper (`GeminiWebClient` and `AsyncGeminiWebClient`) using only the standard library, plus a built-in browser login helper and local HTTP bridge server.
- `nodejs/` is a Node.js 18+ ESM wrapper (`GeminiWebClient`) with TypeScript definitions (`index.d.ts`), browser login helper, and matching local HTTP bridge server, with zero npm dependencies.
- `docs/agents.md` shows how to point Claude Code, Aider, Cline, Roo Code, OpenCode, and SDKs at the local bridge.
- `docs/web-to-api-guide.md` walks through how this wrapper works on the wire and how to build a web-to-API wrapper for coding workflows yourself.

## Sign in once (no DevTools needed)

Run either login command once:

```bash
# Python
PYTHONPATH=python python -m gemini_web_to_api login

# Node.js
npm --prefix nodejs run login
```

This opens Chrome, Edge, Brave, or Chromium with its own isolated profile under `~/.gemini-web-to-api/browser/default` (never touching your normal browser profile). Sign into Google, wait a second for `gemini.google.com/app` to finish loading, and the window closes automatically after saving the session to `~/.gemini-web-to-api/session.json`.

Python, Node.js, C++, and the local HTTP bridge all read that same `session.json` file. When Google rotates `__Secure-1PSIDTS`, the wrapper writes the fresh cookies back to `session.json` automatically. If HTTP rotation ever returns 401, the Python and Node.js clients can also silently refresh cookies via `--headless=new` against that saved browser profile without popping open a window.

Multiple Google accounts work by passing a profile name (`--profile work` or `GEMINI_PROFILE=work`), which gives each login its own isolated browser directory and entry in `session.json`.

If your browser is in a custom location, set `GEMINI_BROWSER=/path/to/chrome`. If you are on a headless server with no browser installed, you can still set `GEMINI_COOKIE` manually by copying a `Cookie:` header from DevTools.

## Python

No `pip install` is required if you add `python/` to `PYTHONPATH`, or install it editable with `pip install -e ./python`.

```python
from gemini_web_to_api import GeminiWebClient

# Loads ~/.gemini-web-to-api/session.json (or opens browser login on first run)
client = GeminiWebClient.from_session()

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

For `asyncio` code, use `await AsyncGeminiWebClient.from_session()`.

## Node.js and TypeScript

Requires Node.js 18+ (uses built-in `fetch`, `node:crypto`, and a localhost CDP socket with no npm dependencies).

```js
import { GeminiWebClient } from "./nodejs/src/index.js";

// Loads ~/.gemini-web-to-api/session.json (or opens browser login on first run)
const client = await GeminiWebClient.fromSession();

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
    // Reads GEMINI_COOKIE or ~/.gemini-web-to-api/session.json saved by `login`
    if (!client.InitFromSession("default", error)) {
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

Other languages (Rust, Go, C#, Zig, C) can link `gemini_web_ffi` against `cpp/include/gemini_web/c_api.h` (using `gwa_client_create_from_session`), or talk to the local HTTP bridge over localhost.

## Using with Claude Code and other AI coding agents

Start the local bridge with either Python or Node.js. If you have not logged in yet, it opens the browser sign-in window automatically on startup:

```bash
# Python
PYTHONPATH=python python -m gemini_web_to_api serve --port 8080

# Node.js
PORT=8080 npm --prefix nodejs run serve
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
