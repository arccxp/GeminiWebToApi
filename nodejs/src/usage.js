const SHORT_WINDOW_MAX_MS = 6 * 60 * 60 * 1000;
const COMPUTE_ACTION = 2;

function resetMillis(val) {
  if (!Array.isArray(val) || !val.length) return 0;
  const sec = typeof val[0] === "number" ? Math.trunc(val[0]) : 0;
  if (sec <= 0) return 0;
  let nanos = typeof val[1] === "number" ? Math.trunc(val[1]) : 0;
  if (nanos < 0 || nanos > 999999999) nanos = 0;
  return sec * 1000 + Math.floor(nanos / 1_000_000);
}

export function parseQuotaRows(payload) {
  let root;
  try {
    root = JSON.parse(payload);
  } catch {
    return [];
  }
  if (!Array.isArray(root)) return [];
  const rows = [];
  function walk(node, depth) {
    if (depth > 4 || !Array.isArray(node)) return;
    if (
      node.length >= 2 &&
      Array.isArray(node[0]) &&
      typeof node[0][1] === "number" &&
      node[0][1] === COMPUTE_ACTION &&
      typeof node[1] === "number"
    ) {
      let pct = typeof node[2] === "number" ? node[2] : 0;
      pct = Math.max(0, Math.min(100, pct));
      rows.push({
        unit: Math.trunc(node[1]),
        usedPercent: Math.round(pct),
        resetsAtMs: resetMillis(node[3]),
      });
      return;
    }
    for (const item of node) walk(item, depth + 1);
  }
  walk(root, 0);
  return rows;
}

export function parseUsagePagePayload(payload) {
  let root;
  try {
    root = JSON.parse(payload);
  } catch {
    return [];
  }
  if (!Array.isArray(root)) return [];
  const rows = [];
  function walk(node, depth) {
    if (depth > 6 || !Array.isArray(node)) return;
    if (
      node.length >= 4 &&
      typeof node[1] === "number" &&
      typeof node[2] === "number" &&
      Array.isArray(node[3]) &&
      Array.isArray(node[3][0])
    ) {
      const frac = node[1];
      if (frac >= 0 && frac <= 1.5) {
        const resetsAtMs = resetMillis(node[3][0]);
        if (resetsAtMs > 0) {
          const pct = Math.max(0, Math.min(100, frac * 100));
          rows.push({
            unit: Math.trunc(node[2]),
            usedPercent: Math.round(pct),
            resetsAtMs,
          });
          return;
        }
      }
    }
    for (const item of node) walk(item, depth + 1);
  }
  walk(root, 0);
  return rows;
}

export function classifyQuotaRows(rows, nowMs = Date.now()) {
  const meter = {
    valid: false,
    shortWindow: { valid: false, usedPercent: 0, resetsAtMs: 0 },
    weekly: { valid: false, usedPercent: 0, resetsAtMs: 0 },
  };
  for (const row of rows) {
    const win = {
      valid: true,
      usedPercent: Math.max(0, Math.min(100, row.usedPercent)),
      resetsAtMs: row.resetsAtMs || 0,
    };
    if (row.unit === 1 && (win.resetsAtMs === 0 || win.resetsAtMs <= nowMs + SHORT_WINDOW_MAX_MS)) {
      meter.shortWindow = win;
    } else if (row.unit === 2) {
      meter.weekly = win;
    }
  }
  meter.valid = meter.shortWindow.valid || meter.weekly.valid;
  return meter;
}

export function formatUsageSpan(ms) {
  if (ms <= 0) return "now";
  const minutes = Math.ceil(ms / 60000);
  if (minutes < 60) return `${minutes}m`;
  const hours = Math.floor(minutes / 60);
  const rem = minutes % 60;
  if (hours < 24) return `${hours}h${rem}m`;
  return `${Math.floor(hours / 24)}d${hours % 24}h`;
}

export function usageSummary(meter, nowMs = Date.now()) {
  const w = meter.shortWindow;
  if (!w || !w.valid) return "";
  const left = 100 - Math.max(0, Math.min(100, w.usedPercent));
  if (w.resetsAtMs > 0 && nowMs >= w.resetsAtMs) return "100%";
  if (left === 100 || w.resetsAtMs <= 0) return `${left}%`;
  return `${left}% ${formatUsageSpan(w.resetsAtMs - nowMs)}`;
}
