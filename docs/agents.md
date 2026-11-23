# Integrating with AI coding agents

Coding agents almost always speak one of two HTTP protocols: Anthropic's `/v1/messages` or OpenAI's `/v1/chat/completions`. The local bridge in `python/gemini_web_to_api/server.py` and `nodejs/src/server.js` speaks both at the same time so you do not have to write a custom provider plugin for each editor or CLI.

## Starting the bridge

Set your browser cookie header first, then pick either runtime:

```bash
export GEMINI_COOKIE='__Secure-1PSID=...; __Secure-1PSIDTS=...; SAPISID=...'
export GEMINI_MODEL='gemini-auto'   # optional: pin a specific model like gemini-3.1-pro
export GEMINI_EFFORT='high'         # optional: low | medium | high
```

Python:

```bash
PYTHONPATH=python python -m gemini_web_to_api serve --host 127.0.0.1 --port 8080
```

Node.js:

```bash
PORT=8080 node nodejs/src/server.js
```

Keep the server bound to `127.0.0.1`. Anyone who can reach the port can spend your Google account's Gemini quota. If you expose it on a LAN interface, set `BRIDGE_API_KEY` so every request has to send a matching `Authorization: Bearer <key>` or `x-api-key` header.

## Claude Code

Claude Code uses the Anthropic Messages API and respects `ANTHROPIC_BASE_URL` and `ANTHROPIC_AUTH_TOKEN`.

macOS and Linux:

```bash
ANTHROPIC_BASE_URL=http://127.0.0.1:8080 \
ANTHROPIC_AUTH_TOKEN=local \
claude
```

Windows PowerShell:

```powershell
$env:ANTHROPIC_BASE_URL = "http://127.0.0.1:8080"
$env:ANTHROPIC_AUTH_TOKEN = "local"
claude
```

What the bridge translates for Claude Code:

- `POST /v1/messages` (both buffered JSON and SSE `stream: true` with `message_start`, `content_block_start`, `content_block_delta`, `content_block_stop`, `message_delta`, `message_stop`).
- `POST /v1/messages/count_tokens` (returns a rough character-based estimate so preflight token checks succeed).
- Model aliases: Claude Code sends names like `claude-sonnet-4-...` or `claude-haiku-...`. The bridge maps any `claude-*` name to your configured `GEMINI_MODEL`.
- Tools: when Claude Code sends tool definitions (`Read`, `Edit`, `Write`, `Bash`, `Glob`, `Grep`), the bridge appends a short tool-call format instruction to the prompt and parses any ```` ```tool_call ```` blocks in Gemini's reply into native Anthropic `tool_use` content blocks with `stop_reason: "tool_use"`. Subsequent `tool_result` blocks from Claude Code are folded back into the conversation transcript.

Tip: set `GEMINI_EFFORT=high` and pin a Pro-tier model (`GEMINI_MODEL=gemini-3.1-pro` or whichever Pro row `list_models()` returns on your account) if you want more accurate JSON tool calls. Smaller Flash rows sometimes skip the closing fence or paraphrase tool arguments.

## Aider

Aider accepts any OpenAI-compatible server via `OPENAI_API_BASE`:

```bash
OPENAI_API_BASE=http://127.0.0.1:8080/v1 \
OPENAI_API_KEY=local \
aider --model openai/gemini-auto
```

Aider applies its own diff-based edits directly from plain text replies, so it pairs reliably with web-to-API bridges without needing JSON function-calling schemas.

## Cline and Roo Code (VS Code)

1. Open the Cline or Roo Code settings panel in VS Code.
2. Set API Provider to "OpenAI Compatible".
3. Set Base URL to `http://127.0.0.1:8080/v1`.
4. Set API Key to `local` (or your `BRIDGE_API_KEY`).
5. Set Model ID to `gemini-auto` (or a model name from `GET /v1/models`).

Cline and Roo Code already embed XML tool instructions in their system prompts, so they work without relying on JSON function-calling schemas.

## OpenCode and Continue

For OpenCode or Continue, configure a custom OpenAI-compatible provider pointing at `http://127.0.0.1:8080/v1`:

```json
{
  "models": [
    {
      "title": "Gemini Web",
      "provider": "openai",
      "model": "gemini-auto",
      "apiBase": "http://127.0.0.1:8080/v1",
      "apiKey": "local"
    }
  ]
}
```

## OpenAI and Anthropic SDKs (Python, TypeScript, Go, Rust, etc.)

Any language with an official OpenAI or Anthropic SDK works without extra bindings.

Python (`openai` SDK):

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="local")
resp = client.chat.completions.create(
    model="gemini-auto",
    messages=[{"role": "user", "content": "Write a binary search in Rust."}],
)
print(resp.choices[0].message.content)
```

Python (`anthropic` SDK):

```python
import anthropic

client = anthropic.Anthropic(base_url="http://127.0.0.1:8080", api_key="local")
msg = client.messages.create(
    model="gemini-auto",
    max_tokens=1024,
    messages=[{"role": "user", "content": "Write a binary search in Rust."}],
)
print(msg.content[0].text)
```

cURL:

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"gemini-auto","messages":[{"role":"user","content":"Hello"}]}'
```

## Practical caveats for coding loops

- Context length: coding agents resend the full conversation history on every turn. Because the bridge flattens `messages` into one prompt per request, very long sessions can hit Gemini Web's per-message input cap. Start a fresh session or compact history once a thread grows large.
- Rate limits: Gemini Web enforces a rolling 5-hour compute window and a weekly cap. Check `GET http://127.0.0.1:8080/v1/usage` to see how much of the current window is left before starting a long agent loop.
- Streaming with tools: when `tools` are present on a request, the bridge waits for the complete Gemini reply before emitting SSE chunks so it can split prose from ```` ```tool_call ```` blocks cleanly without leaking raw JSON fences into the UI.
