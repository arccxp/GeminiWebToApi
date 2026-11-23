# How to convert a web LLM app to an API for coding

When you have a web subscription to an AI product and want to wire it into scripts, editors, or coding agents like Claude Code and Aider, you need two layers:

1. A client that reproduces the website's own HTTP requests (auth headers, session tokens, RPC payloads, streaming parser, and cookie refresh).
2. A thin local HTTP adapter that translates incoming `/v1/chat/completions` and `/v1/messages` calls into those web requests and formats the response back.

Below is how this Gemini Web wrapper was built, written out as a general playbook you can follow for Gemini or adapt to other web apps.

## 1. Capture a real session in DevTools

Open the web app in a clean browser tab, open DevTools (Network tab), check "Preserve log", and run one normal prompt from start to finish:

1. Reload `/app` so you capture the initial HTML document.
2. Open the model switcher so you see any model-list or entitlement RPC.
3. Upload a small PNG and send a short prompt so you capture the upload handshake and the generation stream.
4. Send a second follow-up message in the same thread so you can diff turn 1 vs turn 2 and see which IDs keep a conversation alive.

Right-click the generation request and export it with "Copy as cURL" or save a HAR file. Never commit HAR files to git; they contain live session cookies.

## 2. Separate auth cookies from rotating cookies and derived headers

Modern web apps rarely authenticate with a single static header. On Google properties (`gemini.google.com`), auth has three moving pieces:

1. Long-lived session cookies (`SID`, `__Secure-1PSID`, `__Secure-2PSID`, etc.). These identify which Google accounts are logged in. Note one quirk: Google writes `__Secure-3PSID` alongside `__Secure-1PSID` even on a single-account login, so slot 2 only exists when `__Secure-2PSID` is present too.
2. Short-lived rotating cookies (`__Secure-1PSIDTS`, `__Secure-1PSIDCC`, `SIDCC`). Google replaces `__Secure-1PSIDTS` via `Set-Cookie` on responses and via `POST https://accounts.google.com/RotateCookies`. If you reuse a frozen cookie snapshot without absorbing `Set-Cookie` headers, requests start returning 401 within hours.
3. Derived request signature (`Authorization: SAPISIDHASH <unix_ts>_<sha1>`). Google computes `SHA-1("<unix_ts> <sapisid> https://gemini.google.com")` using `SAPISID` / `__Secure-1PAPISID` for account slot 0, or `__Secure-<slot+1>PAPISID` for slots 1 through 3, paired with `X-Goog-AuthUser: <slot>` and a matching `Referer` (`/app` or `/u/<slot>/app`).

Watch out for cookie scope when calling `RotateCookies`: cookies returned by `accounts.google.com` without `Domain=.google.com` are host-only. Keep them prefixed or stored separately (this repo prefixes them with `GwaAccounts.`) so they only get sent back to `accounts.google.com` and stay off `gemini.google.com` requests.

## 3. Scrape boot tokens from the app shell instead of running a browser

Before firing RPCs, the browser reads page-boot values embedded in the HTML of `GET /app` (inside `window.WIZ_global_data`):

| Key | Purpose |
| --- | --- |
| `SNlM0e` | XSRF `at` token (optional on newer shells; omit `at=` if absent) |
| `cfb2h` | Server build label (`bl` query parameter) |
| `FdrFJe` | Session ID (`f.sid` query parameter) |
| `TuX5cc` | Locale (`hl`, defaults to `en`) |
| `qKIAYe` | Push-ID for attachment uploads |
| `oPEP7c` | Signed-in account email |

One plain `GET /app` with the auth headers pulls all six strings in under a second and can be cached for 30 to 45 minutes. If an RPC later returns 400, 401, or 403, drop the cached shell, run cookie rotation, re-scrape `/app`, patch `bl`, `f.sid`, `at`, `Cookie`, and `SAPISIDHASH`, and replay the request once.

## 4. Map the internal RPCs

Gemini Web uses two Google RPC endpoints under `/_/BardChatUi/data/`:

### Model discovery and tier flags (`batchexecute`, rpcid `otAQ7b`)

`POST /_/BardChatUi/data/batchexecute?rpcids=otAQ7b&source-path=/app&hl=en&rt=c&bl=...&f.sid=...`

Form body: `f.req=[[["otAQ7b","[null,null,null,null,null,null,null,null,null,null]",null,"generic"]]]` (URL-encoded, plus `at=...` when present).

The response starts with Google's anti-XSSI prefix `)]}'` followed by length-prefixed JSON arrays containing `["wrb.fr", "otAQ7b", "<inner_json_string>"]`. Inside that inner JSON array:

- `root[15]` holds the account's model rows (`id` at `[0]`, display label at `[11]` or `[19]`, routing number at `[17]` or `[9]`).
- `root[16]` holds tier flags (`21` Free, `8`/`16` Pro, `22` Ultra).
- `root[17]` holds capability flags (`19`/`106` Pro, `115` Ultra / capacity 4).
- `root[24][1]` is a catalog fallback if `root[15]` is empty.

Computing the right account capacity (`1..4` and field slot `12` vs `13`) matters because sending the wrong capacity in the model header causes error `1052`.

### File and image uploads (`content-push.googleapis.com/upload`)

Attachments use a 4-step resumable protocol before generation:

1. `OPTIONS https://content-push.googleapis.com/upload` with `X-Goog-Upload-Protocol: resumable`, `Push-ID: <qKIAYe>`, `X-Tenant-Id: bard-storage`, and `X-Goog-Upload-Header-Content-Length: <bytes>`.
2. `POST https://content-push.googleapis.com/upload` with `X-Goog-Upload-Command: start`, `Size: <bytes>`, body `File name: <basename>`. Read the target URL from the `X-Goog-Upload-Url` response header.
3. `OPTIONS <upload_url>` with the same start headers.
4. `POST <upload_url>` with `X-Goog-Upload-Command: upload, finalize`, `X-Goog-Upload-Offset: 0`, `Content-Type: <mime>`, and raw file bytes as the body. The response body is the `fileId` string you place into the generation payload.

### Text generation and streaming (`StreamGenerate`)

`POST /_/BardChatUi/data/assistant.lamda.BardFrontendService/StreamGenerate?hl=en&_reqid=<seq>&rt=c&bl=...&f.sid=...`

Important details discovered during testing:

- `_reqid` must be monotonic per process (for example, start at a random 6-digit seed and add `100000` each call). Reusing a random `_reqid` inside the same `f.sid` can hit Google's server-side dedupe cache and hang.
- `x-goog-ext-525001261-jspb` is a 17-element sparse JSPB array carrying the model ID (`[4]`), account capacity (`[11]`), model routing number (`[14]`), thinking toggle (`[15]`, `1` standard or `2` extended), and a process-stable client UUID (`[16]`). Stop the array at index 16 so extra elements do not spill into trailing fields.
- `f.req` wraps `[null, "<inner_81_slot_json>"]`. Inside the 81-slot array:
  - `[0]` holds `[prompt]` (or `[prompt, 0, null, [[[fileId], fileName], ...], null, null, 0]` when images are attached).
  - `[1]` holds `[locale]`.
  - `[2]` holds the 10-slot conversation metadata block `[conversation_id, response_id, candidate_id, null, ..., ""]`. Send all three IDs (`c_...`, `r_...`, `rc_...`) to continue a thread; leave them empty for a new chat.
  - `[59]` holds the per-request uppercase UUID (matching `x-goog-ext-525005358-jspb: ["<uuid>",1]`).
  - `[79]` holds the model routing number.
  - `[80]` holds the thinking effort level (`low=1`, `medium=4`, `high=2`).

### Parsing `StreamGenerate` chunks

Responses stream as newline-delimited length-prefixed JSON arrays. Each `["wrb.fr", null, "<payload>"]` frame carries the full answer snapshot generated so far:

- `payload[1][0]` and `payload[1][1]` carry the conversation ID (`c_...`) and response ID (`r_...`).
- `payload[4][0][0]` carries the candidate ID (`rc_...`).
- `payload[4][0][1]` carries the visible answer text (a string or array of strings). To emit deltas for SSE, track how many characters you already sent and slice `snapshot[sent_len:]`.
- `payload[4][0][37]` carries the reasoning/thinking stream when enabled.
- `payload[42]` and `payload[58]` report the model label and routing number that actually served the turn.
- Refusals include `BardErrorInfo` plus a numeric code (`1097` means the thread IDs are stale and must be cleared; `1013` and `1052` are transient and safe to retry after refreshing the shell).
- Strip web-only markup (`[cite: ...]`, `[cite_start]`, `?code_reference`, `?code_stdout`) before handing text to coding tools so citations do not corrupt code blocks or diffs.

### Usage / quota checks (`qpEbW` and `jSf9Qc`)

Calling `batchexecute` with `rpcids=qpEbW` (trying body shapes `[[[1,2]]]` then `[]`) returns rows containing `[[mode, 2], unit, percentUsed, [resetSec, nanos]]` where `unit=1` is the rolling 5-hour window and `unit=2` is the weekly window. If `qpEbW` omits the 5-hour row, falling back to `rpcids=jSf9Qc` with `source-path=/usage` reads the same meter shown on `gemini.google.com/usage`.

## 5. Translate to standard API shapes for coding agents

Once you have `ask()`, `stream()`, and `list_models()` working, wrap them in a local HTTP server that speaks the schemas coding tools already expect:

1. Flatten multi-message `messages` arrays (including `system`, `user`, `assistant`, and `tool_result` entries) into a clean labeled transcript.
2. Convert base64 `image_url` (OpenAI) or `image` (Anthropic) blocks into raw bytes and upload them through the attachment endpoint.
3. Map foreign model aliases (`claude-*`, `gpt-*`) to a default model so tools like Claude Code do not fail on unknown model names.
4. Bridge tool calls if the agent sends a `tools` array: inject the JSON schemas with a fenced output convention (such as ```` ```tool_call ````), parse completed replies for those fences, and emit native `tool_use` / `tool_calls` JSON objects.
5. Translate growing snapshots into SSE frames (`data: {"choices":[{"delta":{"content":"..."}}]}` for OpenAI, and `event: content_block_delta` for Anthropic).

## 6. Knowing when pure HTTP is not enough

Some web features mint short-lived tokens inside obfuscated client JavaScript (for example, Gemini rows served over `processSession` that hang on `StreamGenerate`, or per-model disabled quota banners that only render in the mode picker DOM).

When you run into an endpoint like that, you have two choices:

- Stick to the models that answer directly on HTTP (lower latency, works headless on servers, zero browser RAM).
- Add an optional browser relay (WebView2 on Windows or Playwright/Chromium cross-platform) that keeps one signed-in page open, selects the model via DOM attributes (`data-test-id="bard-mode-option-<id>"`), fills the composer, and streams `model-response` text back over IPC. The C++ side of this repo keeps the `gwa::relay::RelayDriver` interface and the reference DOM scripts in `cpp/src/relay.cpp` so you can see how that fallback plugs in.
