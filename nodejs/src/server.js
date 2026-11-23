import crypto from "node:crypto";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { DEFAULT_MODEL, GeminiWebClient } from "./client.js";
import { tierLabel } from "./models.js";
import { usageSummary } from "./usage.js";

const TOOL_CALL_RE = /```tool_call\s*\n([\s\S]*?)\n```/g;

export function parseToolCalls(text) {
  const calls = [];
  const remaining = String(text || "")
    .replace(TOOL_CALL_RE, (full, inner) => {
      try {
        const obj = JSON.parse(inner.trim());
        if (obj && typeof obj === "object" && typeof obj.name === "string") {
          calls.push({
            id: `toolu_${crypto.randomBytes(10).toString("hex")}`,
            name: obj.name,
            input: obj.input ?? obj.arguments ?? {},
          });
          return "";
        }
      } catch {
        // leave unparsed block untouched
      }
      return full;
    })
    .trim();
  return { text: remaining, calls };
}

function extractOpenAiContent(content) {
  if (typeof content === "string") return { text: content, images: [] };
  if (!Array.isArray(content)) return { text: String(content || ""), images: [] };
  const texts = [];
  const images = [];
  for (let i = 0; i < content.length; i++) {
    const part = content[i];
    if (typeof part === "string") texts.push(part);
    else if (part?.type === "text") texts.push(String(part.text || ""));
    else if (part?.type === "image_url") {
      const url = typeof part.image_url === "string" ? part.image_url : part.image_url?.url || "";
      if (url.startsWith("data:") && url.includes(";base64,")) {
        const [hdr, b64] = url.split(";base64,", 2);
        const mimeType = hdr.slice(5) || "image/png";
        const ext = mimeType.split("/").pop()?.split(";")[0] || "png";
        images.push({ bytes: Buffer.from(b64, "base64"), filename: `image_${i}.${ext}`, mimeType });
      }
    }
  }
  return { text: texts.filter(Boolean).join("\n"), images };
}

function extractAnthropicContent(content) {
  if (typeof content === "string") return { text: content, images: [] };
  if (!Array.isArray(content)) return { text: String(content || ""), images: [] };
  const texts = [];
  const images = [];
  for (let i = 0; i < content.length; i++) {
    const b = content[i];
    if (typeof b === "string") texts.push(b);
    else if (b?.type === "text") texts.push(String(b.text || ""));
    else if (b?.type === "image" && b.source?.type === "base64") {
      const mimeType = b.source.media_type || "image/png";
      const ext = mimeType.split("/").pop() || "png";
      images.push({
        bytes: Buffer.from(b.source.data || "", "base64"),
        filename: `image_${i}.${ext}`,
        mimeType,
      });
    } else if (b?.type === "tool_use") {
      texts.push(`[Called tool ${b.name} with arguments: ${JSON.stringify(b.input || {})}]`);
    } else if (b?.type === "tool_result") {
      const body = Array.isArray(b.content)
        ? b.content.filter((x) => x?.type === "text").map((x) => x.text).join("\n")
        : String(b.content || "");
      texts.push(`[Tool result (${b.tool_use_id || ""}):\n${body}]`);
    }
  }
  return { text: texts.filter(Boolean).join("\n"), images };
}

function formatToolsPrompt(tools, formatKind) {
  if (!Array.isArray(tools) || !tools.length) return "";
  const specs = tools.map((t) => {
    if (formatKind === "openai" && t.type === "function" && t.function) {
      return {
        name: t.function.name || "",
        description: t.function.description || "",
        parameters: t.function.parameters || {},
      };
    }
    return {
      name: t.name || "",
      description: t.description || "",
      parameters: t.input_schema || t.parameters || {},
    };
  });
  return (
    "You have access to these tools:\n" +
    JSON.stringify(specs, null, 2) +
    "\n\nWhen you need to call a tool, respond with one or more fenced blocks in this exact format:\n" +
    "```tool_call\n" +
    '{"name": "tool_name", "input": {"arg": "value"}}\n' +
    "```\n" +
    "If no tool is needed, reply normally with plain text."
  );
}

function buildTranscript(systemPrompt, messages, formatKind, tools) {
  const sections = [];
  const images = [];
  const extract = formatKind === "anthropic" ? extractAnthropicContent : extractOpenAiContent;

  if (systemPrompt) sections.push(`System:\n${systemPrompt}`);
  if (tools && tools.length) sections.push(formatToolsPrompt(tools, formatKind));

  if (messages.length === 1 && !sections.length && messages[0]?.role === "user") {
    const one = extract(messages[0].content);
    return { prompt: one.text, images: one.images };
  }

  for (const msg of messages) {
    const role = String(msg?.role || "user").toLowerCase();
    const { text: extracted, images: imgs } = extract(msg?.content);
    images.push(...imgs);
    if (role === "system") {
      if (extracted) sections.unshift(`System:\n${extracted}`);
      continue;
    }
    if (role === "tool") {
      sections.push(`Tool result (${msg.tool_call_id || ""}):\n${extracted}`);
      continue;
    }
    let text = extracted;
    if (role === "assistant" && Array.isArray(msg.tool_calls)) {
      for (const tc of msg.tool_calls) {
        const fn = tc?.function || {};
        text = `${text}\n[Called tool ${fn.name || ""}(${fn.arguments || "{}"})]`.trim();
      }
    }
    if (text) sections.push(`${role === "assistant" ? "Assistant" : "User"}:\n${text}`);
  }
  return { prompt: sections.join("\n\n"), images };
}

function resolveModel(requested, defaultModel) {
  const req = String(requested || "").trim();
  if (!req) return defaultModel;
  const low = req.toLowerCase();
  if (low.startsWith("claude-") || low.startsWith("gpt-") || /^o[134]/.test(low)) {
    return defaultModel;
  }
  return req;
}

async function readJsonBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const raw = Buffer.concat(chunks).toString("utf8");
  return raw ? JSON.parse(raw) : {};
}

function sendJson(res, status, data) {
  const body = Buffer.from(JSON.stringify(data), "utf8");
  res.writeHead(status, {
    "Content-Type": "application/json",
    "Content-Length": String(body.length),
    "Access-Control-Allow-Origin": "*",
  });
  res.end(body);
}

export function createBridgeServer(
  client,
  { defaultModel = DEFAULT_MODEL, defaultEffort = "medium", apiKey = null } = {}
) {
  return http.createServer(async (req, res) => {
    const urlPath = (req.url || "/").split("?", 1)[0];

    if (req.method === "OPTIONS") {
      res.writeHead(204, {
        "Access-Control-Allow-Origin": "*",
        "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
        "Access-Control-Allow-Headers":
          "Authorization, Content-Type, x-api-key, anthropic-version, anthropic-beta",
      });
      res.end();
      return;
    }

    if (req.method === "GET" && urlPath === "/health") {
      sendJson(res, 200, { status: "ok" });
      return;
    }

    if (apiKey) {
      const auth = req.headers.authorization || "";
      const xKey = req.headers["x-api-key"] || "";
      if (auth !== `Bearer ${apiKey}` && xKey !== apiKey) {
        sendJson(res, 401, { error: { message: "Invalid API key", type: "authentication_error" } });
        return;
      }
    }

    if (req.method === "GET" && (urlPath === "/v1/models" || urlPath === "/models")) {
      try {
        const { models, tier, email } = await client.listModels({ force: false });
        const now = Math.floor(Date.now() / 1000);
        sendJson(res, 200, {
          object: "list",
          tier: tierLabel(tier),
          email,
          data: models.map((m) => ({
            id: m.name || m.id,
            object: "model",
            created: now,
            owned_by: "google-gemini-web",
            label: m.label,
            available: m.available,
          })),
        });
      } catch (err) {
        sendJson(res, 502, { error: { message: err.message, type: "upstream_error" } });
      }
      return;
    }

    if (req.method === "GET" && (urlPath === "/v1/usage" || urlPath === "/usage")) {
      try {
        const meter = await client.getUsage();
        sendJson(res, 200, {
          summary: usageSummary(meter),
          short_window: {
            valid: meter.shortWindow.valid,
            used_percent: meter.shortWindow.usedPercent,
            resets_at_ms: meter.shortWindow.resetsAtMs,
          },
          weekly: {
            valid: meter.weekly.valid,
            used_percent: meter.weekly.usedPercent,
            resets_at_ms: meter.weekly.resetsAtMs,
          },
        });
      } catch (err) {
        sendJson(res, 502, { error: { message: err.message, type: "upstream_error" } });
      }
      return;
    }

    if (req.method !== "POST") {
      sendJson(res, 404, { error: { message: `Not found: ${urlPath}` } });
      return;
    }

    let payload;
    try {
      payload = await readJsonBody(req);
    } catch {
      sendJson(res, 400, { error: { message: "Invalid JSON body" } });
      return;
    }

    if (urlPath === "/v1/messages/count_tokens" || urlPath === "/messages/count_tokens") {
      sendJson(res, 200, { input_tokens: Math.max(1, Math.floor(JSON.stringify(payload).length / 4)) });
      return;
    }

    if (urlPath === "/v1/chat/completions" || urlPath === "/chat/completions") {
      const messages = payload.messages;
      if (!Array.isArray(messages) || !messages.length) {
        sendJson(res, 400, { error: { message: "messages must be a non-empty array" } });
        return;
      }
      const requestedModel = String(payload.model || defaultModel);
      const resolvedModel = resolveModel(requestedModel, defaultModel);
      const effort = String(payload.reasoning_effort || payload.effort || defaultEffort);
      const tools = Array.isArray(payload.tools) ? payload.tools : null;
      const stream = Boolean(payload.stream);
      const { prompt, images } = buildTranscript("", messages, "openai", tools);
      const completionId = `chatcmpl-${crypto.randomBytes(12).toString("hex")}`;
      const created = Math.floor(Date.now() / 1000);

      if (!stream) {
        try {
          const out = await client.ask(prompt, { model: resolvedModel, effort, images });
          let content = out.answer;
          let toolCalls = null;
          let finishReason = "stop";
          if (tools) {
            const parsed = parseToolCalls(out.answer);
            if (parsed.calls.length) {
              content = parsed.text || null;
              finishReason = "tool_calls";
              toolCalls = parsed.calls.map((c) => ({
                id: c.id,
                type: "function",
                function: { name: c.name, arguments: JSON.stringify(c.input) },
              }));
            }
          }
          const message = { role: "assistant", content };
          if (toolCalls) message.tool_calls = toolCalls;
          if (out.thinking) message.reasoning_content = out.thinking;
          sendJson(res, 200, {
            id: completionId,
            object: "chat.completion",
            created,
            model: requestedModel,
            choices: [{ index: 0, message, finish_reason: finishReason }],
            usage: {
              prompt_tokens: Math.max(1, Math.floor(prompt.length / 4)),
              completion_tokens: Math.max(1, Math.floor(out.answer.length / 4)),
              total_tokens: Math.max(2, Math.floor((prompt.length + out.answer.length) / 4)),
            },
          });
        } catch (err) {
          sendJson(res, 502, { error: { message: err.message, type: "upstream_error" } });
        }
        return;
      }

      res.writeHead(200, {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        Connection: "close",
        "Access-Control-Allow-Origin": "*",
      });
      const emit = (delta, finishReason = null) => {
        res.write(
          `data: ${JSON.stringify({
            id: completionId,
            object: "chat.completion.chunk",
            created,
            model: requestedModel,
            choices: [{ index: 0, delta, finish_reason: finishReason }],
          })}\n\n`
        );
      };
      try {
        emit({ role: "assistant" });
        if (tools) {
          const out = await client.ask(prompt, { model: resolvedModel, effort, images });
          const parsed = parseToolCalls(out.answer);
          if (parsed.text) emit({ content: parsed.text });
          if (parsed.calls.length) {
            parsed.calls.forEach((c, idx) =>
              emit({
                tool_calls: [
                  {
                    index: idx,
                    id: c.id,
                    type: "function",
                    function: { name: c.name, arguments: JSON.stringify(c.input) },
                  },
                ],
              })
            );
            emit({}, "tool_calls");
          } else {
            emit({}, "stop");
          }
        } else {
          for await (const delta of client.stream(prompt, { model: resolvedModel, effort, images })) {
            emit({ content: delta });
          }
          emit({}, "stop");
        }
        res.write("data: [DONE]\n\n");
      } catch (err) {
        res.write(`data: ${JSON.stringify({ error: { message: err.message } })}\n\n`);
      }
      res.end();
      return;
    }

    if (urlPath === "/v1/messages" || urlPath === "/messages") {
      const messages = payload.messages;
      if (!Array.isArray(messages) || !messages.length) {
        sendJson(res, 400, { type: "error", error: { type: "invalid_request_error", message: "messages required" } });
        return;
      }
      const systemText = Array.isArray(payload.system)
        ? payload.system.filter((b) => b?.type === "text").map((b) => b.text).join("\n")
        : String(payload.system || "");
      const requestedModel = String(payload.model || defaultModel);
      const resolvedModel = resolveModel(requestedModel, defaultModel);
      const effort = String(payload.effort || defaultEffort);
      const tools = Array.isArray(payload.tools) ? payload.tools : null;
      const stream = Boolean(payload.stream);
      const { prompt, images } = buildTranscript(systemText, messages, "anthropic", tools);
      const msgId = `msg_${crypto.randomBytes(12).toString("hex")}`;
      const inputTokens = Math.max(1, Math.floor(prompt.length / 4));

      if (!stream) {
        try {
          const out = await client.ask(prompt, { model: resolvedModel, effort, images });
          const content = [];
          let stopReason = "end_turn";
          if (tools) {
            const parsed = parseToolCalls(out.answer);
            if (parsed.text) content.push({ type: "text", text: parsed.text });
            if (parsed.calls.length) {
              stopReason = "tool_use";
              for (const c of parsed.calls) {
                content.push({ type: "tool_use", id: c.id, name: c.name, input: c.input });
              }
            }
          } else {
            content.push({ type: "text", text: out.answer });
          }
          if (!content.length) content.push({ type: "text", text: "" });
          sendJson(res, 200, {
            id: msgId,
            type: "message",
            role: "assistant",
            model: requestedModel,
            content,
            stop_reason: stopReason,
            stop_sequence: null,
            usage: { input_tokens: inputTokens, output_tokens: Math.max(1, Math.floor(out.answer.length / 4)) },
          });
        } catch (err) {
          sendJson(res, 502, { type: "error", error: { type: "api_error", message: err.message } });
        }
        return;
      }

      res.writeHead(200, {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        Connection: "close",
        "Access-Control-Allow-Origin": "*",
      });
      const emit = (event, data) => res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
      try {
        emit("message_start", {
          type: "message_start",
          message: {
            id: msgId,
            type: "message",
            role: "assistant",
            model: requestedModel,
            content: [],
            stop_reason: null,
            stop_sequence: null,
            usage: { input_tokens: inputTokens, output_tokens: 0 },
          },
        });
        let totalChars = 0;
        let stopReason = "end_turn";
        let blockIndex = 0;
        if (tools) {
          const out = await client.ask(prompt, { model: resolvedModel, effort, images });
          totalChars = out.answer.length;
          const parsed = parseToolCalls(out.answer);
          if (parsed.text) {
            emit("content_block_start", { type: "content_block_start", index: blockIndex, content_block: { type: "text", text: "" } });
            emit("content_block_delta", { type: "content_block_delta", index: blockIndex, delta: { type: "text_delta", text: parsed.text } });
            emit("content_block_stop", { type: "content_block_stop", index: blockIndex });
            blockIndex++;
          }
          if (parsed.calls.length) {
            stopReason = "tool_use";
            for (const c of parsed.calls) {
              emit("content_block_start", {
                type: "content_block_start",
                index: blockIndex,
                content_block: { type: "tool_use", id: c.id, name: c.name, input: {} },
              });
              emit("content_block_delta", {
                type: "content_block_delta",
                index: blockIndex,
                delta: { type: "input_json_delta", partial_json: JSON.stringify(c.input) },
              });
              emit("content_block_stop", { type: "content_block_stop", index: blockIndex });
              blockIndex++;
            }
          }
        } else {
          emit("content_block_start", { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } });
          for await (const delta of client.stream(prompt, { model: resolvedModel, effort, images })) {
            totalChars += delta.length;
            emit("content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: delta } });
          }
          emit("content_block_stop", { type: "content_block_stop", index: 0 });
        }
        emit("message_delta", {
          type: "message_delta",
          delta: { stop_reason: stopReason, stop_sequence: null },
          usage: { output_tokens: Math.max(1, Math.floor(totalChars / 4)) },
        });
        emit("message_stop", { type: "message_stop" });
      } catch (err) {
        emit("error", { type: "error", error: { type: "api_error", message: err.message } });
      }
      res.end();
      return;
    }

    sendJson(res, 404, { error: { message: `Not found: ${urlPath}` } });
  });
}

const isMain = (() => {
  if (!process.argv[1]) return false;
  const selfPath = path.resolve(fileURLToPath(import.meta.url));
  const argPath = path.resolve(process.argv[1]);
  return process.platform === "win32"
    ? selfPath.toLowerCase() === argPath.toLowerCase()
    : selfPath === argPath;
})();

if (isMain) {
  const host = process.env.HOST || "127.0.0.1";
  const port = Number(process.env.PORT || 8080);
  const profile = process.env.GEMINI_PROFILE || "default";
  GeminiWebClient.fromSession({
    profile,
    autoLogin: process.env.GEMINI_NO_AUTO_LOGIN !== "1",
  })
    .then((client) => {
      const server = createBridgeServer(client, {
        defaultModel: process.env.GEMINI_MODEL || DEFAULT_MODEL,
        defaultEffort: process.env.GEMINI_EFFORT || "medium",
        apiKey: process.env.BRIDGE_API_KEY || null,
      });
      server.listen(port, host, () => {
        console.log(`GeminiWebToApi bridge listening on http://${host}:${port}`);
        console.log(`  OpenAI endpoint:    http://${host}:${port}/v1/chat/completions`);
        console.log(`  Anthropic endpoint: http://${host}:${port}/v1/messages`);
      });
    })
    .catch((err) => {
      console.error(`Failed to start bridge: ${err.message}`);
      process.exit(1);
    });
}
