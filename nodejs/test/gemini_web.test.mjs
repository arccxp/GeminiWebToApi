import assert from "node:assert/strict";
import test from "node:test";

import {
  AccountTier,
  GeminiWebClient,
  canonicalModelName,
  classifyQuotaRows,
  createBridgeServer,
  isUltraOnlyModel,
  modelAllowedOnPlan,
  modelSlug,
  nameAnswersTo,
  parseCookieHeader,
  parseToolCalls,
  rotatingCookieChanged,
  stripCitations,
  usageSummary,
} from "../src/index.js";

function makeMockTransport(answerOverride = null) {
  return async ({ method, url, headers, onChunk }) => {
    if (method === "GET" && url.includes("/app")) {
      return {
        status: 200,
        body:
          '<html><script>window.WIZ_global_data={"SNlM0e":"at_1","cfb2h":"boq_1","FdrFJe":"-77","TuX5cc":"en","qKIAYe":"feeds/p","oPEP7c":"dev@example.com","tier":"gemini_advanced"};</script></html>',
        headers: {},
        setCookies: ["__Secure-1PSIDTS=ts_rotated; Domain=.google.com; Path=/"],
      };
    }
    if (url.includes("rpcids=otAQ7b")) {
      const inner = JSON.stringify([
        null, null, null, null, null, null, null, null, null, null, null, null, null, null, null,
        [
          ["id_flash", "Fast", null, null, null, null, null, null, null, 1, null, "3.6 Flash", null, null, null, null, null, 1],
          ["id_pro", "Pro", null, null, null, null, null, null, null, 3, null, "3.1 Pro", null, null, null, null, null, 3],
        ],
        [8],
        [19],
      ]);
      return { status: 200, body: `)]}'\n\n99\n[["wrb.fr","otAQ7b",${JSON.stringify(inner)}]]\n`, headers: {}, setCookies: [] };
    }
    if (url.includes("StreamGenerate")) {
      const text = answerOverride || "Hello world [cite: 1]";
      const pad35 = new Array(35).fill(null);
      const pad37 = new Array(37).fill(null);
      const pad15 = new Array(15).fill(null);
      const f1 = JSON.stringify([null, ["c_1", "r_1"], null, null, [["rc_1", [text.slice(0, 5)], ...pad35, ["plan"]]]]);
      const f2 = JSON.stringify([
        null,
        ["c_1", "r_1"],
        null,
        null,
        [["rc_1", [text], ...pad35, ["plan done"]]],
        ...pad37,
        "3.6 Flash",
        ...pad15,
        1,
      ]);
      const c1 = `)]}'\n10\n[["wrb.fr",null,${JSON.stringify(f1)}]]\n`;
      const c2 = `10\n[["wrb.fr",null,${JSON.stringify(f2)}]]\n`;
      if (onChunk) {
        onChunk(c1);
        onChunk(c2);
      }
      return { status: 200, body: c1 + c2, headers: {}, setCookies: [] };
    }
    if (url.includes("rpcids=qpEbW")) {
      const resetSec = Math.floor(Date.now() / 1000) + 7200;
      const inner = JSON.stringify([[[[null, 2], 1, 20, [resetSec, 0]], [[null, 2], 2, 45, [resetSec + 86400, 0]]]]);
      return { status: 200, body: `)]}'\n\n50\n[["wrb.fr","qpEbW",${JSON.stringify(inner)}]]\n`, headers: {}, setCookies: [] };
    }
    if (url.includes("content-push.googleapis.com/upload")) {
      if (method === "OPTIONS") return { status: 200, body: "", headers: {}, setCookies: [] };
      if (headers["X-Goog-Upload-Command"] === "start") {
        return {
          status: 200,
          body: "",
          headers: { "x-goog-upload-url": "https://content-push.googleapis.com/upload?id=u1" },
          setCookies: [],
        };
      }
      return { status: 200, body: "/contrib_service/ttl_1d/file_node", headers: {}, setCookies: [] };
    }
    if (url.includes("RotateCookies")) {
      return {
        status: 200,
        body: "",
        headers: {},
        setCookies: [
          "__Secure-1PSIDTS=ts_new; Domain=.google.com; Path=/",
          "__Host-GAPS=host_val; Path=/",
        ],
      };
    }
    return { status: 404, body: "", headers: {}, setCookies: [] };
  };
}

test("cookie parsing, multi-account slots, and rotation", () => {
  const jar = parseCookieHeader("-H 'cookie: SID=s1; SAPISID=k0; __Secure-1PSID=p1; __Secure-3PSID=p3; __Secure-1PSIDTS=old'");
  assert.deepEqual(jar.accounts(), [0]);
  assert.equal(jar.hashKey(0), "k0");
  const updated = jar.absorbSetCookies(["__Secure-1PSIDTS=next; Domain=.google.com; Path=/"]);
  assert.ok(updated);
  assert.equal(rotatingCookieChanged(jar.header, updated, 0), true);

  const multi = parseCookieHeader("__Secure-1PSID=a; __Secure-1PAPISID=ka; __Secure-2PSID=b; __Secure-2PAPISID=kb");
  assert.deepEqual(multi.accounts(), [0, 1]);
  assert.equal(multi.hashKey(1), "kb");
});

test("model naming, matching, citations, and usage", () => {
  assert.equal(modelSlug("3.6 Flash"), "3-6-flash");
  assert.equal(canonicalModelName("3.6 Flash", 1), "gemini-3.6-flash");
  assert.equal(nameAnswersTo("gemini-3.6-flash", "id1", "3.6 Flash", "Fast"), true);
  assert.equal(isUltraOnlyModel("x", "4 Carbon", "gemini-4-carbon"), true);
  assert.equal(modelAllowedOnPlan("x", "4 Carbon", "gemini-4-carbon", AccountTier.PRO), false);
  assert.equal(modelAllowedOnPlan("x", "4 Carbon", "gemini-4-carbon", AccountTier.ULTRA), true);
  assert.equal(stripCitations("hi [cite: 1] ok?code_stdout"), "hi  ok");

  const nowMs = 1_700_000_000_000;
  const meter = classifyQuotaRows([{ unit: 1, usedPercent: 15, resetsAtMs: nowMs + 3600_000 }], nowMs);
  assert.equal(usageSummary(meter, nowMs), "85% 1h0m");
});

test("client ask, stream, upload, usage, and cookie rotation", async () => {
  const client = new GeminiWebClient("__Secure-1PSID=p1; SAPISID=k0; __Secure-1PSIDTS=old", {
    transport: makeMockTransport(),
  });
  const { models, tier, email } = await client.listModels();
  assert.equal(tier, AccountTier.PRO);
  assert.equal(email, "dev@example.com");
  assert.deepEqual(models.map((m) => m.name), ["gemini-3.6-flash", "gemini-3.1-pro"]);
  assert.match(client.liveCookies, /__Secure-1PSIDTS=ts_rotated/);

  const res = await client.ask("Hello", { model: "gemini-3.6-flash", effort: "high", continueChat: true });
  assert.equal(res.answer, "Hello world ");
  assert.equal(res.thinking, "plan done");
  assert.equal(client.thread.conversation, "c_1");

  let streamed = "";
  for await (const delta of client.stream("Hello", { model: "gemini-3.6-flash" })) {
    streamed += delta;
  }
  assert.equal(streamed, "Hello world ");

  const up = await client.uploadImage(Buffer.from([0xff, 0xd8, 0xff]), { filename: "a.jpg" });
  assert.equal(up.fileId, "/contrib_service/ttl_1d/file_node");

  const meter = await client.getUsage();
  assert.equal(meter.shortWindow.usedPercent, 20);
  assert.equal(meter.weekly.usedPercent, 45);

  assert.equal(await client.rotateCookies({ force: true }), true);
  assert.match(client.liveCookies, /GwaAccounts\.__Host-GAPS=host_val/);
});

test("bridge server supports OpenAI chat completions and Anthropic /v1/messages", async () => {
  const reply = 'Reading file.\n```tool_call\n{"name":"Read","input":{"path":"app.js"}}\n```';
  const parsed = parseToolCalls(reply);
  assert.equal(parsed.text, "Reading file.");
  assert.equal(parsed.calls[0].name, "Read");

  const client = new GeminiWebClient("__Secure-1PSID=p1; SAPISID=k0", {
    transport: makeMockTransport(reply),
  });
  const server = createBridgeServer(client);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const base = `http://127.0.0.1:${port}`;
  try {
    const modelsRes = await fetch(`${base}/v1/models`);
    assert.equal(modelsRes.status, 200);
    const modelsJson = await modelsRes.json();
    assert.equal(modelsJson.data.length, 2);

    const oaiRes = await fetch(`${base}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "gpt-4o",
        messages: [{ role: "user", content: "Read app.js" }],
        tools: [{ type: "function", function: { name: "Read", parameters: {} } }],
      }),
    });
    assert.equal(oaiRes.status, 200);
    const oaiJson = await oaiRes.json();
    assert.equal(oaiJson.choices[0].finish_reason, "tool_calls");
    assert.equal(oaiJson.choices[0].message.tool_calls[0].function.name, "Read");

    const antRes = await fetch(`${base}/v1/messages`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "claude-sonnet-4-20250514",
        max_tokens: 1024,
        messages: [{ role: "user", content: "Read app.js" }],
        tools: [{ name: "Read", input_schema: { type: "object" } }],
      }),
    });
    assert.equal(antRes.status, 200);
    const antJson = await antRes.json();
    assert.equal(antJson.stop_reason, "tool_use");
    assert.equal(antJson.content[1].type, "tool_use");
    assert.equal(antJson.content[1].name, "Read");

    const streamRes = await fetch(`${base}/v1/messages`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: "claude-sonnet-4-20250514",
        stream: true,
        messages: [{ role: "user", content: "Hello" }],
      }),
    });
    const sse = await streamRes.text();
    assert.match(sse, /event: message_start/);
    assert.match(sse, /event: content_block_delta/);
    assert.match(sse, /event: message_stop/);
  } finally {
    server.close();
  }
});
