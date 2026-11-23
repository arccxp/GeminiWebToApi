export const AccountTier = Object.freeze({
  UNKNOWN: -1,
  FREE: 0,
  PRO: 1,
  ULTRA: 2,
});

export function tierLabel(tier) {
  if (tier === AccountTier.FREE) return "Free";
  if (tier === AccountTier.PRO) return "Pro";
  if (tier === AccountTier.ULTRA) return "Ultra";
  return "Unknown";
}

export function modelSlug(value) {
  let out = "";
  for (const ch of String(value || "")) {
    if (/[a-zA-Z0-9]/.test(ch)) out += ch.toLowerCase();
    else if (out && !out.endsWith("-")) out += "-";
  }
  return out.replace(/-+$/, "");
}

export function nameAnswersTo(requested, id = "", label = "", category = "") {
  const want = String(requested || "").trim().toLowerCase();
  if (!want) return false;
  if (want === id.toLowerCase() || want === label.toLowerCase() || want === category.toLowerCase()) {
    return true;
  }
  const wantSlug = modelSlug(requested);
  if (!wantSlug) return false;
  const wantBare = wantSlug.startsWith("gemini-") ? wantSlug.slice(7) : wantSlug;
  const targets = new Set([modelSlug(label), modelSlug(category), modelSlug(id)]);
  return targets.has(wantSlug) || targets.has(wantBare);
}

export function canonicalModelName(label, number = 0) {
  let out = "";
  let pending = false;
  for (const ch of String(label || "")) {
    if (/[a-zA-Z0-9]/.test(ch)) {
      if (pending && out && !out.endsWith("-") && !out.endsWith(".")) out += "-";
      pending = false;
      out += ch.toLowerCase();
    } else if (ch === ".") {
      if (out && !out.endsWith("-") && !out.endsWith(".")) out += ".";
    } else {
      pending = true;
    }
  }
  out = out.replace(/[-.]+$/, "");
  if (!out) return number > 0 ? `gemini-${number}` : "";
  if (!out.startsWith("gemini")) return `gemini-${out}`;
  if (!out.startsWith("gemini-")) return `gemini-${out.slice(6)}`;
  return out;
}

export function productModelName(label, category, number = 0) {
  return canonicalModelName(label || category, number);
}

export function isUltraOnlyModel(id = "", label = "", name = "") {
  return `${id} ${label} ${name}`.toLowerCase().includes("carbon");
}

export function modelAllowedOnPlan(id, label, name, tier) {
  if (!isUltraOnlyModel(id, label, name)) return true;
  return tier >= AccountTier.ULTRA;
}

function hasFlag(list, flag) {
  return Array.isArray(list) && list.some((x) => typeof x === "number" && x === flag);
}

export function computeCapacity(root) {
  const tier = Array.isArray(root) ? root[16] : null;
  const caps = Array.isArray(root) ? root[17] : null;
  if (hasFlag(tier, 21)) return { capacity: 1, capacityField: 13 };
  if (hasFlag(tier, 22)) return { capacity: 2, capacityField: 13 };
  if (hasFlag(caps, 115)) return { capacity: 4, capacityField: 12 };
  if (hasFlag(tier, 16) || hasFlag(caps, 106)) return { capacity: 3, capacityField: 12 };
  if (hasFlag(tier, 8) || hasFlag(caps, 19)) return { capacity: 2, capacityField: 12 };
  return { capacity: 1, capacityField: 12 };
}

function looksLikeLabel(s) {
  if (typeof s !== "string" || s.length < 3 || s.includes("://")) return false;
  if (!/[a-zA-Z]/.test(s)) return false;
  if (/^[0-9a-fA-F]+$/.test(s)) return false;
  return true;
}

function recoverLabel(entry) {
  const stack = [entry];
  while (stack.length) {
    const node = stack.pop();
    if (typeof node === "string" && looksLikeLabel(node)) return node;
    if (Array.isArray(node)) {
      for (let i = node.length - 1; i >= 0; i--) stack.push(node[i]);
    }
  }
  return "";
}

export function parseModelsFromPayload(payload) {
  let root;
  try {
    root = JSON.parse(payload);
  } catch {
    return { models: [], tier: AccountTier.UNKNOWN };
  }
  if (!Array.isArray(root)) return { models: [], tier: AccountTier.UNKNOWN };

  const { capacity, capacityField } = computeCapacity(root);
  const accountList = root[15];
  const tierFlags = root[16];
  const capFlags = root[17];

  const ultraFlag = hasFlag(capFlags, 115) || hasFlag(tierFlags, 22);
  const proFlag =
    hasFlag(tierFlags, 16) || hasFlag(tierFlags, 8) || hasFlag(capFlags, 106) || hasFlag(capFlags, 19);
  const freeFlag = hasFlag(tierFlags, 21);

  let tier = AccountTier.UNKNOWN;
  if (ultraFlag) tier = AccountTier.ULTRA;
  else if (proFlag) tier = AccountTier.PRO;
  else if (freeFlag) tier = AccountTier.FREE;
  else if (Array.isArray(accountList) && accountList.length) {
    tier = accountList.length > 1 || capacity > 1 ? AccountTier.PRO : AccountTier.FREE;
  }

  const models = [];
  if (Array.isArray(accountList)) {
    for (const entry of accountList) {
      if (!Array.isArray(entry)) continue;
      const id = typeof entry[0] === "string" ? entry[0] : "";
      const number =
        typeof entry[17] === "number" ? entry[17] : typeof entry[9] === "number" ? entry[9] : 0;
      if (!id && number <= 0) {
        const recovered = recoverLabel(entry);
        if (recovered) {
          models.push({
            id: "",
            category: "",
            label: recovered,
            number: 0,
            capacity,
            capacityField,
            inPicker: true,
            available: true,
            note: "",
          });
        }
        continue;
      }
      let category =
        typeof entry[1] === "string" ? entry[1] : typeof entry[10] === "string" ? entry[10] : "";
      let display = typeof entry[11] === "string" ? entry[11] : null;
      if (Array.isArray(entry[3]) && category) {
        display = category;
        category = typeof entry[2] === "string" ? entry[2] : "";
      } else if (display === null) {
        display = typeof entry[19] === "string" ? entry[19] : null;
      }
      const label = display || category || id;
      if (label) {
        models.push({
          id,
          category,
          label,
          number: number > 0 ? number : 1,
          capacity,
          capacityField,
          inPicker: true,
          available: true,
          note: "",
        });
      }
    }
    if (models.length) return { models, tier };
  }

  const catalog = Array.isArray(root[24]) ? root[24][1] : null;
  if (Array.isArray(catalog)) {
    for (const group of catalog) {
      const row = Array.isArray(group) && group[0] != null ? group[0] : group;
      if (!Array.isArray(row)) continue;
      const number = typeof row[1] === "number" ? row[1] : 0;
      if (number <= 0) continue;
      const category = typeof row[4] === "string" ? row[4] : "";
      const label = (typeof row[12] === "string" && row[12]) || category;
      if (label) {
        models.push({
          id: "",
          category,
          label,
          number,
          capacity,
          capacityField,
          inPicker: false,
          available: true,
          note: "",
        });
      }
    }
  }
  return { models, tier };
}

export function fallbackModelTable() {
  return [
    {
      id: "cf41b0e0dd7d53e5",
      category: "",
      label: "Auto",
      number: 6,
      capacity: 1,
      capacityField: 12,
      inPicker: true,
      available: true,
      note: "",
    },
  ];
}

export function pickModel(models, requested) {
  if (!models || !models.length) return null;
  const want = String(requested || "").trim().toLowerCase();
  if (!want || want === "gemini-auto" || want === "auto" || want === "default") {
    let autoRow = null;
    let fullFlash = null;
    let nonLite = null;
    for (const m of models) {
      const tag = `${m.category} ${m.label}`.toLowerCase();
      const lite = tag.includes("lite");
      const flash = tag.includes("flash");
      if (!autoRow && tag.includes("auto")) autoRow = m;
      if (flash && !lite && !fullFlash) fullFlash = m;
      if (!lite && !nonLite) nonLite = m;
    }
    return autoRow || fullFlash || nonLite || models[0];
  }
  for (const m of models) {
    if (
      nameAnswersTo(requested, m.id, m.label, m.category) ||
      nameAnswersTo(requested, m.id, productModelName(m.label, m.category, m.number), m.category)
    ) {
      return m;
    }
  }
  if (/^\d{1,2}$/.test(want)) {
    const num = Number(want);
    return models.find((m) => m.number === num) || null;
  }
  if (/^gemini-\d+$/.test(want)) {
    const num = Number(want.slice(7));
    return models.find((m) => m.number === num) || null;
  }
  return null;
}

export function modelBaseKey(label, category, number = 0) {
  const tokens = String(label || category || "")
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((t) => t && t !== "gemini" && !/^\d+$/.test(t));
  if (!tokens.length && number > 0) return `n${number}`;
  return tokens.join("-");
}

export function buildModelHeader(model, level, generationId) {
  const header = new Array(17).fill(null);
  header[0] = 1;
  header[4] = model.id || null;
  header[7] = 0;
  header[8] = [4, 5, 6, 8];
  header[11] = model.capacityField === 13 ? [null, model.capacity] : model.capacity;
  header[14] = model.number;
  header[15] = level >= 2 ? 2 : 1;
  header[16] = generationId;
  return JSON.stringify(header);
}
