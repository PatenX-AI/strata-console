"use strict";

const $ = (id) => document.getElementById(id);
const state = {
  csrf: "",
  payload: null,
  models: [],
  requests: [],
  selectedRequestKey: "",
  pendingRestart: false,
  dirtyFields: new Set(),
  live: null,
  dirty: false,
  original: {},
  tab: "monitor",
  logPaused: false,
  logLines: [],
  logPath: "",
  clearKey: false,
  apiKeySet: false,
  apiKeyDisabled: false,
  lastError: "",
  pollTimer: null,
};

const stateLabels = {
  stopped: "未启动",
  starting: "启动中",
  loading: "加载中",
  running: "运行中",
  unloaded: "未加载",
  stopping: "停止中",
  restarting: "重启中",
  error: "错误",
  external: "外部服务",
};

const liveLabels = {
  idle: "空闲",
  reading: "读取提示词",
  generating: "生成中",
  unloaded: "未加载",
  queued: "排队中",
};

function esc(v) {
  return String(v ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function fmt(n, digits = 0) {
  if (n === null || n === undefined || Number.isNaN(Number(n))) return "—";
  return Number(n).toLocaleString("zh-CN", { maximumFractionDigits: digits, minimumFractionDigits: 0 });
}

function gb(v, digits = 1) {
  if (v === null || v === undefined) return "—";
  return fmt(Number(v) / 1073741824, digits);
}

function pct(part, total) {
  if (!total) return 0;
  return Math.max(0, Math.min(100, (Number(part) / Number(total)) * 100));
}

function setHtml(id, html) {
  const el = $(id);
  if (el) el.innerHTML = html;
}

function setText(id, text) {
  const el = $(id);
  if (el) el.textContent = text ?? "";
}

async function api(path, opts = {}) {
  const method = (opts.method || "GET").toUpperCase();
  const headers = { Accept: "application/json", ...(opts.headers || {}) };
  if (method !== "GET") {
    headers["Content-Type"] = "application/json";
    headers["X-CSRF-Token"] = state.csrf;
  }
  const res = await fetch(path, { ...opts, method, headers, cache: "no-store" });
  let body = null;
  try { body = await res.json(); } catch (e) { /* not JSON */ }
  if (!res.ok && res.status === 403 && body?.error?.includes("CSRF") && path !== "/api/session") {
    const s = await fetch("/api/session", { cache: "no-store" }).then((r) => r.json());
    state.csrf = s.token;
    return api(path, opts);
  }
  if (!res.ok) {
    const msg = body && (body.error || body.message) ? (body.error || body.message) : `HTTP ${res.status}`;
    throw new Error(msg);
  }
  return body;
}

const post = (path, data = {}) => api(path, { method: "POST", body: JSON.stringify(data) });

function toast(message, type = "ok", ms = 4500) {
  const box = document.createElement("div");
  box.className = `toast ${type}`;
  box.textContent = message;
  $("toasts").appendChild(box);
  setTimeout(() => box.remove(), ms);
}

function setToggle(id, on) {
  const el = $(id);
  if (el) el.setAttribute("aria-checked", on ? "true" : "false");
}

function isOn(id) {
  return $(id)?.getAttribute("aria-checked") === "true";
}

function bindTokenPreset(inputId, presetId) {
  const select = $(presetId);
  if (!select) return;
  select.addEventListener("change", () => {
    if (select.value === "") return;
    setField(inputId, select.value === "__clear__" ? "" : select.value);
    markDirty(inputId);
    select.value = "";
  });
}

function bindToggle(id, cb) {
  const el = $(id);
  if (!el) return;
  el.addEventListener("click", () => {
    if (el.disabled) return;
    const on = !isOn(id);
    setToggle(id, on);
    markDirty(id);
    cb?.(on);
  });
}

function markDirty(field = "*") {
  state.dirty = true;
  state.dirtyFields.add(field);
  const b = $("saveState");
  b.textContent = "未保存";
  b.className = "badge warn";
  updatePathState();
  updateButtons();
}

function restartRequired() {
  if (state.pendingRestart) return true;
  if (!state.dirty) return false;
  if (state.dirtyFields.has("*")) return true;
  const noRestart = new Set(["autoRestart", "lazyRestart", "openBrowser", "preventSleep", "backupCount", "startMode"]);
  for (const field of state.dirtyFields) {
    if (!noRestart.has(field)) return true;
  }
  return false;
}

function updatePathState() {
  const b = $("pathState");
  if (!(state.models || []).some((m) => m.installed)) {
    b.textContent = "未发现模型";
    b.className = "badge danger";
    return;
  }
  if (pathsChanged()) {
    b.textContent = "已切换 · 待保存";
    b.className = "badge warn";
  } else {
    b.textContent = "自动读取";
    b.className = "badge warn";
  }
}

function pathsChanged() {
  const fields = ["ggufPath", "packPath", "plePath", "tokenizerPath", "exePath", "profilePath"];
  return fields.some((id) => ($(id)?.value || "") !== (state.original[id] || ""));
}

function value(id) { return ($(id)?.value ?? "").trim(); }

function activeModelPort() {
  return Number(state.live?.process?.active_port || state.live?.config?.port || state.payload?.view?.port || numberValue("port", 8080));
}
function numberValue(id, fallback = 0) {
  const n = Number(value(id));
  return Number.isFinite(n) ? n : fallback;
}

function buildForm() {
  return {
    data_dir: value("dataDir"),
    model_name: value("modelName"),
    aliases: value("modelAliases"),
    exe: value("exePath"),
    native: value("ggufPath"),
    ple: value("plePath"),
    pack: value("packPath"),
    tokenizer: value("tokenizerPath"),
    expert_profile: value("profilePath"),
    log: value("logPath"),
    context: numberValue("contextInput", 0),
    kv: value("kvSelect"),
    vision: $("visionCheck").checked,
    vision_gpu: value("visionDevice") === "gpu",
    vision_exe: value("visionExe"),
    vision_mmproj: value("visionMmproj"),
    vision_max_tokens: numberValue("visionMaxTokens", 768),
    vision_threads: numberValue("visionThreads", 8),
    vision_elastic: isOn("visionElastic"),
    vision_elastic_idle_s: value("visionElasticIdleS"),
    vision_elastic_mib: value("visionElasticMib"),
    vision_min_tokens: value("visionMinTokens"),
    idle_unload_min: numberValue("idleUnload", 60),
    reasoning_effort: value("reasoningEffort"),
    reasoning_budget_tokens: value("reasoningBudget"),
    temperature: value("temperature"),
    top_p: value("topP"),
    top_k: value("topK"),
    min_p: value("minP"),
    presence_penalty: value("presencePenalty"),
    frequency_penalty: value("frequencyPenalty"),
    repetition_penalty: value("repetitionPenalty"),
    penalty_last_n: value("penaltyLastN"),
    seed: value("seed"),
    max_tokens: value("maxTokens"),
    fit_max_tokens: isOn("fitMaxTokens"),
    answer_room: isOn("answerRoom"),
    reasoning_close_retry: isOn("reasoningCloseRetry"),
    reasoning_loop_recovery: value("reasoningLoopRecovery"),
    repeat_stop_tokens: value("repeatStopTokens"),
    tool_call_recovery: isOn("toolCallRecovery"),
    effort_position: value("effortPosition"),
    anthropic_thinking: value("anthropicThinking"),
    codex_thread_titles: isOn("codexThreadTitles"),
    codex_compaction_cache: isOn("codexCompactionCache"),
    low_ram: value("lowRam"),
    gpu: value("gpu"),
    mtp: isOn("mtpEnabled"),
    drafting: value("draftingMode"),
    mtp_path: value("mtpPath"),
    spec: value("spec"),
    spec_min_p: value("specMinP"),
    prefill: value("prefill"),
    expert_cache: value("expertCache"),
    kv_resident: value("kvResident"),
    vram_reserve: numberValue("vramReserve", 0),
    vram_elastic: isOn("vramElastic"),
    vram_segment_mib: value("vramSegment"),
    conversation_cache_mib: value("conversationCacheMib"),
    conversation_cache_slots: value("conversationCacheSlots"),
    conversation_cache_min_free_mib: value("conversationCacheMinFree"),
    conversation_cache_min_tokens: value("conversationCacheMinTokens"),
    conversation_cache_park_gain: value("conversationCacheParkGain"),
    expert_profile_save: value("expertProfileSave"),
    expert_profile_save_every: value("expertProfileSaveEvery"),
    resident_budget_gib: value("residentBudgetGib"),
    pool_workers: value("poolWorkers"),
    ple_io: value("pleIo"),
    parallel: value("parallel"),
    max_queue: value("maxQueue"),
    queue_timeout: value("queueTimeout"),
    engine_ready_s: value("engineReadyS"),
    host: value("host"),
    port: numberValue("port", 8080),
    api_key: value("apiKey"),
    api_key_clear: state.clearKey,
    cors_origins: value("corsOrigins"),
    trusted_origins: value("trustedOrigins"),
    allowed_hosts: value("allowedHosts"),
    auto_restart: isOn("autoRestart"),
    lazy_on_restart: isOn("lazyRestart"),
    start_mode: value("startMode"),
    open_browser: isOn("openBrowser"),
    prevent_sleep: isOn("preventSleep"),
    api_monitor: isOn("apiMonitor"),
    backup_count: numberValue("backupCount", 5),
  };
}

function setField(id, v) {
  const el = $(id);
  if (!el) return;
  if (el.type === "checkbox") el.checked = !!v;
  else el.value = v ?? "";
}

function fillModels(payload) {
  const models = payload.models || [];
  state.models = models;
  const sel = $("modelSelect");
  if (!sel) return;
  sel.innerHTML = "";
  if (!models.length) {
    const o = document.createElement("option");
    o.value = "";
    o.textContent = "未发现已安装模型";
    o.disabled = true;
    sel.appendChild(o);
    $("modelHint").textContent = "未找到模型";
    renderModelSummary();
    return;
  }
  let activeId = "";
  for (const m of models) {
    const o = document.createElement("option");
    o.value = m.id || "";
    let text = m.name || m.id || "未命名模型";
    if (m.quant) text += ` · ${m.quant}`;
    if (!m.installed) text += " · 不完整";
    o.textContent = text;
    o.disabled = !m.installed;
    o.title = [m.model_name, m.pack, m.native, m.reason].filter(Boolean).join("\n");
    if (m.active) {
      o.selected = true;
      activeId = m.id;
    }
    sel.appendChild(o);
  }
  if (!activeId) {
    const ph = document.createElement("option");
    ph.value = "";
    ph.textContent = "当前配置不在该模型位置，请选择";
    ph.disabled = true;
    ph.selected = true;
    sel.prepend(ph);
    $("modelHint").textContent = "当前配置不在该位置，请选择模型";
    renderModelSummary();
    return;
  }
  const current = modelById(sel.value);
  $("modelHint").textContent = current
    ? `${models.filter((m) => m.installed).length} 个已安装模型`
    : "自动读取已安装模型";
  renderModelSummary();
}

function modelById(id) {
  return (state.models || []).find((m) => m.id === id) || null;
}

function renderModelSummary() {
  const box = $("modelSummary");
  if (!box) return;
  const m = modelById($("modelSelect")?.value);
  if (!m) {
    box.innerHTML = `<div class="row"><span>状态</span><code>未选择模型</code></div>`;
    return;
  }
  const rows = [
    ["API 模型名", m.model_name || "—"],
    ["量化", m.quant || "—"],
    ["上下文", m.context ? `${Math.round(Number(m.context) / 1024)}K` : "—"],
    ["大小", m.size_bytes ? `${gb(m.size_bytes)} GB` : "—"],
    ["Pack", m.native_only ? "GGUF 原生（无 Pack）" : (m.pack || "—")],
    ["GGUF", m.native || "—"],
  ];
  box.innerHTML = rows.map(([k, val]) =>
    `<div class="row"><span>${esc(k)}</span><code title="${esc(val)}">${esc(val)}</code></div>`
  ).join("") + (m.installed ? "" : `<div class="row"><span>状态</span><code class="danger">${esc(m.reason || "模型文件不完整")}</code></div>`);
}

function fillConfig(payload) {
  state.payload = payload;
  const v = payload.view || {};
  const configs = payload.configs || [];
  const sel = $("configSelect");
  sel.innerHTML = "";
  for (const c of configs) {
    const o = document.createElement("option");
    o.value = c.path;
    o.textContent = c.name;
    o.title = c.name + (c.model_name ? ` · ${c.model_name}` : "");
    if (c.path === payload.active) o.selected = true;
    sel.appendChild(o);
  }
  if (!configs.length) {
    const o = document.createElement("option");
    o.textContent = "未找到 strata-*.json";
    o.value = "";
    sel.appendChild(o);
  }

  fillModels(payload);
  setField("dataDir", payload.data_dir || v.data_dir || "");
  $("dataDirHint").textContent = (payload.models || []).length
    ? "自动读取 models、packs、mtp"
    : "未找到模型，请选择 Strata-data 根目录";

  setField("modelName", v.model_name);
  setField("modelAliases", v.aliases || "");
  updateUserArgsNotice(v.user_args);
  setField("ggufPath", v.native);
  setField("packPath", v.pack);
  setField("plePath", v.ple);
  setField("tokenizerPath", v.tokenizer);
  setField("exePath", v.exe);
  setField("profilePath", v.expert_profile);
  setField("logPath", v.log);
  setField("contextInput", v.context || "");
  setField("kvSelect", v.kv || "int8");
  setField("idleUnload", v.idle_unload_min ?? 60);
  setField("lowRam", v.low_ram || "off");
  updateLowRamUi();
  setField("gpu", v.gpu || "");
  $("gpu").disabled = !!v.gpu_roles_active;
  if (v.gpu_roles_active) {
    $("gpuRolesHint").textContent = `由 Strata App/Terminal 管理：${v.gpu_roles_text || "Main / Helper / Off"}`;
    $("gpuRolesHint").className = "hint ok";
  } else {
    $("gpuRolesHint").textContent = "留空=默认；支持单卡 0 或分层 0,2";
    $("gpuRolesHint").className = "hint";
  }
  setToggle("mtpEnabled", v.mtp);
  const nativeMtp = !!v.mtp_native;
  setField("draftingMode", v.drafting && v.drafting !== "off" ? v.drafting : "auto");
  $("mtpPathLabel").style.display = nativeMtp ? "none" : "";
  $("mtpControl").style.display = nativeMtp ? "none" : "";
  $("mtpPath").placeholder = "...\\mtp\\rt";
  $("browseMtp").style.display = nativeMtp ? "none" : "";
  $("browseMtp").disabled = nativeMtp;
  setField("mtpPath", nativeMtp ? "" : (v.mtp_path || v.mtp_detected_path || ""));
  $("mtpPath").disabled = !v.mtp;
  setText("specLabelText", nativeMtp ? "MTP 草稿数" : "投机窗口");
  $("specTip").dataset.tip = nativeMtp
    ? "模型自带草稿：这里写 --mtp N 的草稿数，即每一步推测解码中 MTP 头提议的 token 数。范围 1–7，常用 2–3。"
    : "对应 --spec，MTP 推测窗口，常用 4；越大不一定越快。";
  $("spec").placeholder = nativeMtp ? "2" : "4";
  $("specField").style.display = nativeMtp ? "none" : "";
  $("specMinPField").style.display = nativeMtp ? "none" : "";
  updateMtpHint();
  setField("spec", nativeMtp
    ? (v.mtp_kind === "count" ? (v.mtp_value || "") : "")
    : (v.spec || ""));
  setField("specMinP", v.spec_min_p || "");
  setField("prefill", v.prefill || "");
  setField("expertCache", v.expert_cache || "");
  setField("kvResident", v.kv_resident || "");
  setField("vramReserve", v.vram_reserve || 0);
  setToggle("vramElastic", v.vram_elastic);
  setField("vramSegment", v.vram_segment_mib ?? "");
  setField("conversationCacheMib", v.conversation_cache_mib || "");
  setField("conversationCacheSlots", v.conversation_cache_slots || "");
  setField("conversationCacheMinFree", v.conversation_cache_min_free_mib || "");
  setField("conversationCacheMinTokens", v.conversation_cache_min_tokens || "");
  setField("conversationCacheParkGain", v.conversation_cache_park_gain || "");
  setField("expertProfileSave", v.expert_profile_save || "");
  setField("expertProfileSaveEvery", v.expert_profile_save_every ?? "");
  setField("residentBudgetGib", v.low_ram === "resident" ? "" : (v.resident_budget_gib || ""));
  updateLowRamUi();
  setField("poolWorkers", v.pool_workers || "");
  setField("pleIo", v.ple_io || "");
  setField("parallel", v.parallel ?? 1);
  setField("maxQueue", v.max_queue ?? "");
  setField("queueTimeout", v.queue_timeout ?? "");
  setField("engineReadyS", v.engine_ready_s ?? "");
  setField("visionCheck", v.vision);
  setField("visionDevice", v.vision_gpu ? "gpu" : "cpu");
  setField("visionExe", v.vision_exe || "");
  {
    const desired = v.vision_gpu ? v.vision_exe_gpu : v.vision_exe_cpu;
    if (desired && (!v.vision_exe || v.vision_exe === v.vision_exe_gpu || v.vision_exe === v.vision_exe_cpu))
      setField("visionExe", desired);
    const hint = $("visionExeHint");
    if (hint) hint.textContent = v.vision_exe_gpu && v.vision_exe_cpu
      ? "自动使用 GPU / CPU 版本" : "只检测到一个视觉编码器版本";
  }
  setField("visionMmproj", v.vision_mmproj || "");
  setField("visionMaxTokens", v.vision_max_tokens || 768);
  setField("visionThreads", v.vision_threads || 8);
  setToggle("visionElastic", v.vision_elastic);
  setField("visionElasticIdleS", v.vision_elastic_idle_s ?? 120);
  setField("visionElasticMib", v.vision_elastic_mib ?? 2048);
  setField("visionMinTokens", v.vision_min_tokens ?? 0);
  setField("reasoningEffort", v.reasoning_effort || "");
  setField("reasoningBudget", v.reasoning_budget_tokens ?? "");
  setField("temperature", v.temperature ?? "");
  setField("topP", v.top_p ?? "");
  setField("topK", v.top_k ?? "");
  setField("minP", v.min_p ?? "");
  setField("presencePenalty", v.presence_penalty ?? "");
  setField("frequencyPenalty", v.frequency_penalty ?? "");
  setField("repetitionPenalty", v.repetition_penalty ?? "");
  setField("penaltyLastN", v.penalty_last_n ?? "");
  setField("seed", v.seed ?? "");
  setField("maxTokens", v.max_tokens || "");
  setToggle("fitMaxTokens", v.fit_max_tokens);
  setToggle("answerRoom", v.answer_room !== false);
  setToggle("reasoningCloseRetry", v.reasoning_close_retry);
  setField("reasoningLoopRecovery", v.reasoning_loop_recovery || "off");
  setField("repeatStopTokens", v.repeat_stop_tokens ?? 256);
  setToggle("toolCallRecovery", v.tool_call_recovery !== false);
  setField("effortPosition", v.effort_position || "start");
  setField("anthropicThinking", v.anthropic_thinking || "model");
  setToggle("codexThreadTitles", v.codex_thread_titles);
  setToggle("codexCompactionCache", v.codex_compaction_cache);
  setField("startMode", v.start_mode || "preload");
  setField("backupCount", v.backup_count || 5);
  setField("host", v.host || "127.0.0.1");
  setField("port", v.port || 8080);
  const lan = !["127.0.0.1", "localhost", "::1"].includes(v.host || "127.0.0.1");
  setField("corsOrigins", v.cors_origins || "");
  setField("trustedOrigins", v.trusted_origins || "");
  setField("allowedHosts", v.allowed_hosts || "");
  setField("apiKey", "");
  state.apiKeySet = !!v.api_key_set;
  state.apiKeyDisabled = !!v.api_key_disabled;
  const prefix = v.api_key_prefix || "sk-strata-…";
  if (state.apiKeyDisabled) {
    $("apiKey").placeholder = "已显式关闭（none）";
    $("keyState").textContent = "已关闭（none）";
  } else {
    $("apiKey").placeholder = v.api_key_set ? `已设置 ${prefix}（留空保持不变）` : "未设置";
    $("keyState").textContent = v.api_key_set ? `已设置 · ${prefix}` : "未设置";
  }
  state.clearKey = false;
  updateCorsHint();

  setToggle("autoRestart", v.auto_restart);
  setToggle("lazyRestart", v.lazy_on_restart);
  $("lazyRestart").disabled = !v.auto_restart;
  setToggle("openBrowser", v.open_browser);
  setToggle("preventSleep", v.prevent_sleep);
  setToggle("apiMonitor", v.api_monitor);
  setToggle("apiMonitorToggle", v.api_monitor);

  setToggle("lanToggle", lan);
  $("lanExtra").classList.toggle("open", lan);
  if (!Array.from($("host").options).some((o) => o.value === v.host)) {
    const o = document.createElement("option");
    o.value = v.host;
    o.textContent = v.host;
    $("host").appendChild(o);
  }
  setField("host", v.host || "127.0.0.1");
  updateLanBadge(lan);

  updateVisionNote();
  $("visionDevice").disabled = !$("visionCheck").checked;
  $("visionExe").disabled = !$("visionCheck").checked;
  $("visionMmproj").disabled = !$("visionCheck").checked;
  $("visionMaxTokens").disabled = !$("visionCheck").checked;
  $("visionMaxTokensPreset").disabled = !$("visionCheck").checked;
  $("visionThreads").disabled = !$("visionCheck").checked;
  updateVisionHint();
  updateOptionalVisibility();

  $("backupBadge").textContent = `${v.backup_count || 5} 个备份`;
  setText("manageSummary", `${v.config_name || "未选择配置"}${v.model_name ? ` · ${v.model_name}` : ""}${v.context ? ` · ${Math.round(v.context / 1024)}K` : ""}${v.kv ? ` · KV ${v.kv}` : ""}`);
  $("contextHint").textContent = v.context ? `当前 ${Math.round(v.context / 1024)}K` : "当前配置";
  $("contextNote").textContent = v.context
    ? `当前 ${Math.round(v.context / 1024)}K；可点“自动配置”查看官方建议`
    : "从配置读取，可手动修改";

  state.original = {};
  for (const id of ["ggufPath", "packPath", "plePath", "tokenizerPath", "exePath", "profilePath"]) {
    state.original[id] = $(id)?.value || "";
  }
  state.dirty = false;
  state.dirtyFields.clear();
  const b = $("saveState");
  b.textContent = "已保存";
  b.className = "badge ok";
  const modelCount = (payload.models || []).filter((m) => m.installed).length;
  $("pathState").textContent = modelCount ? "自动读取" : "未发现模型";
  $("pathState").className = "badge " + (modelCount ? "warn" : "danger");
  updateButtons();
  renderWarnings(v.warnings || []);
}

function updateUserArgsNotice(userArgs) {
  const box = $("userArgsNotice");
  if (!box) return;
  const args = Array.isArray(userArgs) ? userArgs : [];
  box.hidden = args.length === 0;
  if (args.length) {
    $("userArgsText").innerHTML = `<span class="mono">${esc(args.join(" "))}</span>`;
  }
}

function apiBaseUrl() {
  const port = numberValue("port", 8080);
  const on = isOn("lanToggle");
  const urls = state.payload?.view?.lan_urls || [];
  if (on) {
    const host = urls.length ? urls[0] : "http://本机IP";
    return `${host}:${port}/v1`;
  }
  return `http://127.0.0.1:${port}/v1`;
}

function updateVisionNote() {
  const el = $("visionNote");
  if (!el) return;
  const checked = $("visionCheck").checked;
  const ready = !!(value("visionExe") && value("visionMmproj"));
  if (!checked) {
    el.textContent = "图片输入未启用；勾选后保存并重启生效。";
    el.className = "hint warn";
  } else if (!ready) {
    el.textContent = "缺少视觉编码器；请在高级性能中指定 exe 和 mmproj。";
    el.className = "hint warn";
  } else if (value("visionDevice") === "gpu") {
    el.textContent = "将使用 GPU 编码；自动选用检测到的 CUDA 版视觉编码器，保存并重启后生效。";
    el.className = "hint ok";
  } else {
    el.textContent = "视觉组件已启用：CPU 编码（不占显存，单图较慢）";
    el.className = "hint ok";
  }
}

function updateLowRamUi() {
  const resident = value("lowRam") === "resident";
  const budget = $("residentBudgetGib");
  const hint = $("lowRamBudgetHint");
  if (budget) budget.disabled = resident;
  if (hint) {
    if (resident) hint.textContent = "resident 模式会尽量把全部专家放入 RAM，不使用预算";
    else if (value("lowRam") === "budget") hint.textContent = "预算缓存 = mmap + 只复制最热 N GiB 专家";
    else if (value("residentBudgetGib")) hint.textContent = "填写预算会自动切换到“预算缓存”模式";
    else hint.textContent = "预算缓存使用 mmap；与 resident 互斥";
  }
}

function syncVisionExe() {
  const v = state.payload?.view || {};
  const gpu = value("visionDevice") === "gpu";
  const desired = gpu ? v.vision_exe_gpu : v.vision_exe_cpu;
  const cur = value("visionExe");
  if (desired && (!cur || cur === v.vision_exe_gpu || cur === v.vision_exe_cpu))
    setField("visionExe", desired);
  const hint = $("visionExeHint");
  if (hint) hint.textContent = v.vision_exe_gpu && v.vision_exe_cpu
    ? "自动使用 GPU / CPU 版本" : "只检测到一个视觉编码器版本";
}

function updateOptionalVisibility() {
  const mtp = $("mtpOptions");
  if (mtp) mtp.classList.toggle("open", isOn("mtpEnabled"));
  const vision = $("visionOptions");
  const visionOn = !!$("visionCheck")?.checked;
  if (vision) vision.classList.toggle("open", visionOn);
  const gpu = value("visionDevice") === "gpu";
  const visionElastic = isOn("visionElastic");
  for (const id of ["visionElastic", "visionElasticIdleS", "visionElasticMib"]) {
    const el = $(id);
    if (el) el.disabled = !visionOn || !gpu || (id !== "visionElastic" && !visionElastic);
  }
  const minTokens = $("visionMinTokens");
  if (minTokens) minTokens.disabled = !visionOn;
  const vramSegment = $("vramSegment");
  if (vramSegment) vramSegment.disabled = !isOn("vramElastic");
}

function updateMtpHint() {
  const el = $("mtpHint");
  if (!el) return;
  const on = isOn("mtpEnabled");
  const path = value("mtpPath");
  const v = state.payload?.view || {};
  if (v.mtp_native) {
    const drafts = value("spec");
    el.textContent = on
      ? `模型自带草稿${/^\d+$/.test(drafts) ? ` · 每步 ${drafts} 个草稿 token` : ""}；草稿数使用下方 MTP 草稿数`
      : "模型自带草稿；启用后草稿数使用下方 MTP 草稿数";
    el.className = on ? "hint ok" : "hint";
    return;
  }
  if (!on) {
    if (v.mtp_available && v.mtp_detected_path) {
      el.textContent = `检测到 MTP runtime：${v.mtp_detected_path}；打开后保存并重启生效`;
      el.className = "hint ok";
    } else {
      el.textContent = "MTP 未启用";
      el.className = "hint";
    }
    return;
  }
  if (!path) {
    el.textContent = "请填写 MTP runtime 目录（需包含 experts.bin）";
    el.className = "hint warn";
    return;
  }
  const known = path === v.mtp_path || path === v.mtp_detected_path;
  if (known && !v.mtp_available) {
    el.textContent = "目录已配置，但缺少 experts.bin；请等 MTP 安装完成后再启用";
    el.className = "hint warn";
  } else {
    el.textContent = `保存后使用：${path}`;
    el.className = "hint ok";
  }
}

function updateVisionHint(showToast = false) {
  const el = $("visionDeviceHint");
  if (!el) return;
  const gpu = value("visionDevice") === "gpu";
  const ready = !!(value("visionExe") && value("visionMmproj"));
  const free = state.live?.status?.metrics?.engine?.vram_free_mib;
  if (!ready) {
    el.textContent = "需要视觉 exe 与 mmproj；GPU 模式会自动使用检测到的 CUDA 版视觉编码器。";
    el.className = "hint warn";
    return;
  }
  if (gpu) {
    const freeText = free != null ? `当前显存剩余约 ${fmt(free)} MiB。` : "";
    const hasGpu = !!state.payload?.view?.vision_exe_gpu;
    el.textContent = hasGpu
      ? `GPU 编码将使用 CUDA 版视觉编码器。${freeText}按需释放开启时，首图启动、空闲后卸载。`
      : `未检测到 CUDA 版视觉编码器；请先安装或编译 GPU 版 vision。${freeText}`;
    el.className = hasGpu ? "hint ok" : "hint warn";
    if (showToast) toast(hasGpu ? "GPU 视觉编码器已检测到" : "未检测到 CUDA 版视觉编码器", hasGpu ? "ok" : "warn", 7000);
  } else {
    el.textContent = "当前使用 CPU 编码，不占显存；单张图片约数秒。切换到 GPU 会自动改用 CUDA 版视觉编码器。";
    el.className = "hint ok";
  }
}

function updateCorsHint() {
  const el = $("corsHint");
  if (!el) return;
  const cors = value("corsOrigins");
  const hasKey = state.apiKeySet || !!value("apiKey");
  if (!cors) {
    el.textContent = "CORS 当前关闭。填写来源后保存并重启生效。";
    el.className = "hint";
  } else if (cors.split(",").map((x) => x.trim()).includes("*") && !hasKey) {
    el.textContent = "CORS 为 * 且未设置 API Key：任意网页都可以调用本地模型 API，风险较高。";
    el.className = "hint warn";
  } else {
    el.textContent = "CORS 已配置；保存并重启生效。";
    el.className = "hint ok";
  }
}

function updateApiBase() {
  const base = apiBaseUrl();
  const root = base.replace(/\/v1$/, "");
  const fields = {
    apiBase: base,
    chatEndpoint: `${root}/v1/chat/completions`,
    anthropicEndpoint: `${root}/v1/messages`,
  };
  for (const [id, text] of Object.entries(fields)) {
    const el = $(id);
    if (el) el.value = text;
  }
}

function updateLanBadge(on) {
  $("lanBadge").textContent = on ? "局域网" : "仅本机";
  $("lanBadge").className = "badge " + (on ? "info" : "off");
  $("lanExtra").classList.toggle("open", on);
  const port = numberValue("port", 8080);
  const urls = state.payload?.view?.lan_urls || [];
  $("lanUrl").textContent = on
    ? (urls.length ? `${urls[0]}:${port}` : `http://本机IP:${port}`)
    : "—";
  updateApiBase();
}

async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (e) {
    try {
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.appendChild(ta);
      ta.select();
      const ok = document.execCommand("copy");
      ta.remove();
      return ok;
    } catch (e2) {
      return false;
    }
  }
}

async function copyField(id, label) {
  const text = value(id);
  if (await copyText(text)) toast(`${label} 已复制`, "ok");
  else toast("复制失败，请手动选择地址", "warn");
}

async function copyApi() {
  await copyField("apiBase", "API Base");
}

async function copyKey() {
  let key = value("apiKey");
  if (!key) {
    if (state.apiKeyDisabled) {
      toast("API Key 已显式关闭（none）", "warn");
      return;
    }
    if (!state.apiKeySet) {
      toast("尚未设置 API Key", "warn");
      return;
    }
    try {
      const r = await post("/api/api-key", {});
      key = r.api_key || "";
    } catch (e) {
      toast(`读取 API Key 失败: ${e.message}`, "err", 7000);
      return;
    }
  }
  if (key && await copyText(key)) toast("API Key 已复制", "ok");
  else toast("复制失败，请手动选择", "warn");
}

function renderWarnings(list) {
  // Inline warnings are shown in the relevant hints; this is a compact toast only when needed.
  if (!list.length) return;
  // Avoid a toast on every poll: only when the set changes.
  const key = list.join("|");
  if (state.lastError === key) return;
  state.lastError = key;
  toast(list[0], "warn", 7000);
}

function stateInfo(s) {
  const p = s.process || {};
  const st = s.status || {};
  const h = st.health && typeof st.health === "object" ? st.health : {};
  const m = st.metrics && typeof st.metrics === "object" ? st.metrics : {};
  return { p, st, h, m, live: m.live || {}, hw: m.hardware || {}, hs: m.hardware_static || {}, eng: m.engine || {} };
}

function render(s) {
  state.live = s;
  const { p, st, h, m, live, hw, hs, eng } = stateInfo(s);
  const cv = s.config || {};
  const raw = p.state || "stopped";
  const stLabel = stateLabels[raw] || raw;
  $("statusPill").dataset.state = raw;
  setText("statusText", stLabel);
  setText("statusSub", `${cv.host || "127.0.0.1"}:${cv.port || 8080}`);
  const consoleHost = (location.hostname && location.hostname !== "localhost") ? location.hostname : "127.0.0.1";
  setText("consolePort", `${consoleHost}:${s.app?.ui_port || 8090}`);

  const model = h.model || eng.model || cv.model_name || "—";
  const ctx = Number(eng.max_context || cv.context || 0);
  const images = !!h.image_generation || !!h.images || !!cv.vision;
  const idleMin = Number(cv.idle_unload_min || 0);
  setText("workspaceSub", `${model} · ${ctx ? Math.round(ctx / 1024) + "K" : "上下文未知"} · 图片${images ? "开启" : "关闭"} · API Key ${cv.api_key_set ? "已启用" : "未设置"}${idleMin ? ` · 空闲 ${fmt(idleMin)} 分钟卸载` : ""}`);

  const uptime = Number(p.uptime_s || 0);
  const upText = uptime ? `${String(Math.floor(uptime / 3600)).padStart(2, "0")}:${String(Math.floor(uptime % 3600 / 60)).padStart(2, "0")}:${String(uptime % 60).padStart(2, "0")}` : "—";
  const engineVer = eng.engine || eng.version || h.version || "";
  setText("serviceState", stLabel);
  setText("serviceSub", p.owned ? `PID ${p.pid || "—"} · ${upText} · ${engineVer ? `引擎 ${engineVer}` : "引擎 —"}` : (raw === "external" ? "由外部启动，只读监控" : (p.detail || "等待启动")));
  setText("modelState", h.loaded ? "已加载" : (raw === "stopped" || raw === "error" ? "未启动" : (raw === "unloaded" ? "未加载" : "加载中")));
  setText("modelSub", `${model} · ${ctx ? Math.round(ctx / 1024) + "K" : "—"} · ${images ? "图片开启" : "图片关闭"}`);

  const last = (m.requests && m.requests[0]) || null;
  const used = live.state === "idle"
    ? Number(last?.prompt_tokens || 0) + Number(last?.output_tokens || 0)
    : Number(live.prompt_tokens || 0) + Number(live.generated || 0);
  const frac = pct(used, ctx);
  setText("ctxValue", `${used ? (used >= 1000 ? fmt(used / 1000, 1) + "K" : fmt(used)) : "0"}${ctx ? ` / ${Math.round(ctx / 1024)}K` : ""}`);
  $("ctxBar").style.width = `${frac}%`;
  setText("ctxSub", last ? `最近请求 ${fmt(last.prompt_tokens)} prompt · ${fmt(last.output_tokens)} output` : "暂无请求");

  const hasConvCache = m.conversation_cache && typeof m.conversation_cache === "object";
  const cc = hasConvCache ? m.conversation_cache : {};
  const ccEnabled = !!cc.enabled;
  const ccBudget = Number(cc.budget_mib || 0);
  const ccBytes = Number(cc.bytes || 0);
  const ccParked = Number(cc.parked || 0);
  const ccSlots = Number(cc.slots || 0);
  if (!h.loaded) {
    setText("convValue", "—");
    $("convBar").style.width = "0%";
    setText("convSub", "服务未加载");
  } else if (!hasConvCache) {
    setText("convValue", "—");
    $("convBar").style.width = "0%";
    setText("convSub", "等待指标");
  } else if (ccEnabled) {
    setText("convValue", `${fmt(ccParked)}${ccSlots ? ` / ${fmt(ccSlots)}` : ""}`);
    const ccPct = ccBudget > 0 ? pct(ccBytes / 1048576, ccBudget) : 0;
    $("convBar").style.width = `${ccPct}%`;
    tone($("convBar"), ccPct);
    const usedMib = ccBytes / 1048576;
    const budgetText = ccBudget > 0 ? `${fmt(usedMib, 0)} / ${fmt(ccBudget)} MiB` : `${fmt(usedMib, 0)} MiB`;
    setText("convSub", `${budgetText} · ${fmt(cc.restores)} 恢复`);
  } else {
    setText("convValue", "关闭");
    $("convBar").style.width = "0%";
    setText("convSub", "会话缓存未启用");
  }

  renderLive(live, last);
  renderMetrics(hw, hs, eng, m, cv);
  renderRequests(m.requests || [], m.totals || null, m);
  renderLogPreview(st.last_log || "");
  updateVisionHint();
  updateMtpHint();
  updateButtons();
}

function renderLive(live, last) {
  const s = live.state || "idle";
  const badge = $("liveState");
  badge.textContent = liveLabels[s] || s || "空闲";
  badge.className = "badge " + (s === "generating" ? "ok" : s === "reading" ? "info" : s === "queued" ? "warn" : "plain");
  let tokens = "—", speed = "—", bar = 0, meta = "等待请求", elapsed = "";
  if (s === "reading") {
    const total = Number(live.prompt_total || 0);
    const read = Number(live.prompt_read || 0);
    tokens = total ? `${fmt(read)} / ${fmt(total)} tokens` : `${fmt(live.prompt_tokens)} tokens`;
    bar = total ? pct(read, total) : 0;
    speed = live.prefill_tok_s_mean ? `${fmt(live.prefill_tok_s_mean)} tok/s` : "—";
    elapsed = live.elapsed_s ? `${fmt(live.elapsed_s, 1)}s` : "";
    meta = `预填中 · ${fmt(bar)}%`;
  } else if (s === "generating") {
    tokens = `${fmt(live.generated)} / ${fmt(live.max_tokens)} tokens`;
    bar = live.max_tokens ? pct(live.generated, live.max_tokens) : 0;
    speed = `${fmt(live.tok_s, 1)} tok/s`;
    elapsed = live.elapsed_s ? `${fmt(live.elapsed_s, 1)}s` : "";
    meta = `阶段 ${live.phase || "decode"} · 预填 ${fmt(live.prefill_tok_s_mean, 1)} tok/s · 队列 ${fmt(live.queued)}`;
  } else if (last) {
    tokens = `${fmt(last.output_tokens)} tokens`;
    speed = last.decode_tok_s ? `${fmt(last.decode_tok_s, 1)} tok/s` : "—";
    meta = `最近请求 · prompt ${fmt(last.prompt_tokens)} · 输出 ${fmt(last.output_tokens)}`;
  }
  setText("liveTokens", tokens);
  setText("liveSpeed", speed);
  setText("liveElapsed", elapsed);
  $("liveBar").style.width = `${bar}%`;
  setText("liveMeta", meta);
}

function tone(el, p) {
  if (!el) return;
  el.classList.remove("warn", "danger");
  if (p >= 92) el.classList.add("danger");
  else if (p >= 80) el.classList.add("warn");
}

function metric(id, val, unit, sub, barId, barPct) {
  $(id).innerHTML = `${val == null || val === "—" ? "—" : esc(val)}${unit && val !== "—" ? `<small>${esc(unit)}</small>` : ""}`;
  setText(id + "Sub", sub || "");
  const p = Math.max(0, Math.min(100, barPct || 0));
  const b = barId || (id.replace(/^m/, "m") + "Bar");
  if (b && $(b)) { $(b).style.width = `${p}%`; tone($(b), p); }
}

function renderMetrics(hw, hs, eng, m, cv) {
  const gpu = hw.gpu_util;
  metric("mGpu", gpu == null ? "—" : fmt(gpu), "%", hs.gpu_name || "GPU", "mGpuBar", gpu || 0);
  const vramUsed = hw.gpu_mem_used, vramTotal = hw.gpu_mem_total;
  metric("mVram", vramUsed == null ? "—" : gb(vramUsed), vramTotal ? `/ ${gb(vramTotal, 0)} GB` : "GB",
         eng.expert_slots ? `${fmt(eng.expert_slots)} 专家缓存` : "显存", "mVramBar", pct(vramUsed, vramTotal));
  const temp = hw.gpu_temp;
  metric("mTemp", temp == null ? "—" : fmt(temp), "°C", hw.gpu_power != null ? `功耗 ${fmt(hw.gpu_power)} W` : "GPU 温度", "mTempBar", pct(temp, 90));
  const power = hw.gpu_power;
  metric("mPower", power == null ? "—" : fmt(power), "W", hw.gpu_power_limit ? `限制 ${fmt(hw.gpu_power_limit)} W` : "功耗", "mPowerBar", pct(power, hw.gpu_power_limit || 250));
  metric("mCpu", hw.cpu == null ? "—" : fmt(hw.cpu), "%", hs.threads ? `${hs.cores || "?"} 核 · ${hs.threads} 线程` : "CPU", "mCpuBar", hw.cpu || 0);
  metric("mRam", hw.ram_used == null ? "—" : gb(hw.ram_used), hw.ram_total ? `/ ${gb(hw.ram_total, 0)} GB` : "GB", "系统 RAM", "mRamBar", pct(hw.ram_used, hw.ram_total));
  const disk = hw.disk_read_mb;
  metric("mDisk", disk == null ? "—" : fmt(disk, disk < 10 ? 1 : 0), "MB/s", hw.disk_write_mb != null ? `写入 ${fmt(hw.disk_write_mb, 1)} MB/s` : "磁盘读取", "mDiskBar", Math.min(100, (disk || 0) / 1024 * 100));
  const gen = hw.gpu_pcie_gen_max || hw.gpu_pcie_gen;
  metric("mPcie", gen ? `Gen${gen}` : "—", hw.gpu_pcie_width ? `x${hw.gpu_pcie_width}` : "", hw.gpu_pcie_rx_mb != null ? `接收 ${fmt(hw.gpu_pcie_rx_mb, hw.gpu_pcie_rx_mb < 10 ? 1 : 0)} MB/s` : "PCIe", "mPcieBar", Math.min(100, (hw.gpu_pcie_rx_mb || 0) / 2048 * 100));
}

function finishText(finish) {
  const map = { stop: "Done", length: "Max tokens", cancel: "Stopped", disconnect: "Closed", error: "Error" };
  return map[finish] || finish || "—";
}

function finishBadge(finish) {
  const map = {
    stop: ["ok", "Done"], length: ["ok", "Max tokens"], cancel: ["warn", "Stopped"],
    disconnect: ["warn", "Closed"], error: ["danger", "Error"],
  };
  const [cls, label] = map[finish] || ["plain", finish || "—"];
  return `<span class="badge ${cls}">${esc(label)}</span>`;
}

function requestKey(r) {
  if (r.id) return `id:${r.id}`;
  return `t:${Math.round(Number(r.time) || 0)}|${r.endpoint || ""}|${r.prompt_tokens || 0}|${r.output_tokens || 0}`;
}

function requestTimeText(r, full = false) {
  if (!r.time) return "—";
  const d = new Date(Number(r.time) * 1000);
  if (full) return d.toLocaleString("zh-CN");
  const now = new Date();
  const sameDay = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth() && d.getDate() === now.getDate();
  const time = d.toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  return sameDay ? time : `${d.getMonth() + 1}-${String(d.getDate()).padStart(2, "0")} ${time}`;
}

function draftStats(r) {
  const offered = Number(r.drafts_offered || 0);
  const accepted = Number(r.drafts_accepted || 0);
  if (!offered) return null;
  return { accepted, offered, pct: `${(accepted / offered * 100).toFixed(1)}%` };
}

function draftCell(r) {
  const d = draftStats(r);
  return d ? d.pct : "\u2014";
}

function draftRateText(t) {
  if (!t || !Number(t.drafts_offered)) return "";
  const pct = Number(t.drafts_accepted || 0) / Number(t.drafts_offered) * 100;
  return ` \u00b7 \u8349\u7a3f\u63a5\u53d7 ${pct.toFixed(1)}%`;
}

function slotLabel(r) {
  if (r.slot != null) return `slot${r.slot}`;
  const parallel = Number(state.payload?.view?.parallel ?? 1);
  return parallel <= 1 ? "slot0" : "\u2014";
}

function slotCell(r) {
  return slotLabel(r);
}

function requestRows(reqs, details) {
  if (!reqs.length) return `<tr><td colspan="${details ? 9 : 12}" class="muted">暂无请求</td></tr>`;
  return reqs.map((r) => {
    const t = requestTimeText(r);
    const title = [requestTimeText(r, true), r.model, r.endpoint].filter(Boolean).join(" · ");
    const model = esc(r.model || "—");
    if (details) {
      return `<tr data-req-key="${esc(requestKey(r))}" title="${esc(title)}"><td>${esc(t)}</td><td>${model}</td><td class="num">${slotCell(r)}</td><td class="mono">${esc(r.endpoint || "API")}</td><td>${finishBadge(r.finish)}</td><td class="num">${fmt(r.prompt_tokens)}</td><td class="num">${fmt(r.output_tokens)}</td><td class="num">${draftCell(r)}</td><td class="num">${fmt(r.duration_s, 1)}s</td></tr>`;
    }
    const fresh = r.prompt_tokens == null ? null : Math.max(0, Number(r.prompt_tokens) - Number(r.reused || 0));
    const prefill = fresh > 0 && r.prompt_ms > 0 ? fresh / (r.prompt_ms / 1000) : null;
    const hit = r.hit_rate == null ? "—" : `${(Number(r.hit_rate) * 100).toFixed(1)}%`;
    return `<tr title="${esc(title)}"><td>${esc(t)}</td><td>${model}</td><td class="num">${slotCell(r)}</td><td>${finishBadge(r.finish)}</td><td class="num">${fmt(r.prompt_tokens)}</td><td class="num">${fmt(r.reused)}</td><td class="num">${fmt(r.output_tokens)}</td><td class="num">${fmt(prefill)}</td><td class="num">${fmt(r.decode_tok_s, 1)}</td><td class="num">${esc(hit)}</td><td class="num">${draftCell(r)}</td><td class="num">${fmt(r.duration_s, 1)}s</td></tr>`;
  }).join("");
}

function renderRequestDetail(r) {
  const box = $("requestDetail");
  if (!box) return;
  if (!r) {
    box.innerHTML = `<div class="empty"><b>选择一条请求</b><br><br>显示耗时、吞吐与专家命中率等元数据。<br>请求内容需启用 api_monitor 后在官方页面查看。</div>`;
    return;
  }
  const fresh = r.prompt_tokens == null ? null : Math.max(0, Number(r.prompt_tokens) - Number(r.reused || 0));
  const prefill = fresh > 0 && r.prompt_ms > 0 ? fresh / (r.prompt_ms / 1000) : null;
  const pctText = (v) => v == null ? "—" : `${(Number(v) * 100).toFixed(1)}%`;
  const rows = [
    ["时间", requestTimeText(r, true)],
    ["模型", r.model || "—"],
    ["Slot", slotLabel(r)],
    ["端点", r.endpoint || "API"],
    ["状态", finishText(r.finish)],
    ["Prompt", fmt(r.prompt_tokens)],
    ["复用", fmt(r.reused)],
    ["输出", fmt(r.output_tokens)],
    ["首 token", r.first_engine_token_s == null ? "—" : `${fmt(r.first_engine_token_s, 2)}s`],
    ["耗时", `${fmt(r.duration_s, 1)}s`],
    ["Prefill ms", r.prompt_ms == null ? "—" : `${fmt(r.prompt_ms, 1)} ms`],
    ["Decode ms", r.decode_ms == null ? "—" : `${fmt(r.decode_ms, 1)} ms`],
    ["Prefill t/s", fmt(prefill, 1)],
    ["Decode t/s", fmt(r.decode_tok_s, 1)],
    ["专家缓存命中率", pctText(r.hit_rate)],
    ["PCIe 占比", pctText(r.pcie_share)],
    ["RAM / File blobs", `${fmt(r.ram_blobs)} / ${fmt(r.file_blobs)}`],
    ["草稿接受", `${fmt(r.drafts_accepted)} / ${fmt(r.drafts_offered)}`],
    ["草稿接受率", draftStats(r) ? draftStats(r).pct : "—"],
  ];
  box.innerHTML = `<div class="req-head" style="margin-bottom:8px"><span class="card-title">请求元数据</span><span class="badge plain">只读</span></div>
    <table class="diff"><tbody>${rows.map(([k, v]) => `<tr><th>${esc(k)}</th><td>${esc(v)}</td></tr>`).join("")}</tbody></table>
    <div class="hint" style="margin-top:8px">这里只显示元数据；启用 api_monitor 后可在官方监视页查看请求内容。</div>`;
}

function renderRequests(reqs, totals, metrics) {
  const list = reqs.slice(0, 30);
  state.requests = list;
  $("reqBody").innerHTML = requestRows(list, false);
  $("reqTabBody").innerHTML = requestRows(list, true);
  for (const row of $("reqTabBody").querySelectorAll("tr[data-req-key]"))
    row.classList.toggle("selected", row.getAttribute("data-req-key") === state.selectedRequestKey);
  const history = metrics?.history_totals || null;
  const kept = Number(metrics?.history_kept || reqs.length || 0);
  setText("reqCount", reqs.length);
  setText("reqSummary", reqs.length ? `显示 ${list.length} / ${kept} 条` : "暂无请求");
  setText("reqTabSummary", reqs.length ? `显示 ${list.length} / ${kept} 条元数据` : "暂无请求");
  if (totals && totals.requests) {
    const since = totals.since ? new Date(totals.since * 1000).toLocaleString("zh-CN") : "";
    const hist = history && history.requests ? ` · 历史 ${fmt(history.requests)}` : "";
    setText("reqTotals", `本会话 ${fmt(totals.requests)} 请求 · ${fmt(totals.output_tokens)} 输出 tokens${hist}${since ? ` · 自 ${since}` : ""}`);
  } else if (history && history.requests) {
    const since = history.since ? new Date(history.since * 1000).toLocaleString("zh-CN") : "";
    setText("reqTotals", `历史 ${fmt(history.requests)} 请求 · ${fmt(history.output_tokens)} 输出 tokens${since ? ` · 自 ${since}` : ""}`);
  } else {
    setText("reqTotals", "");
  }
  const draftSummary = draftRateText(totals) || draftRateText(history);
  if (draftSummary) setText("reqTotals", `${$("reqTotals").textContent}${draftSummary}`);
  const selected = state.requests.find((r) => requestKey(r) === state.selectedRequestKey);
  if (selected) renderRequestDetail(selected);
  else {
    state.selectedRequestKey = "";
    renderRequestDetail(null);
  }
  setText("stateUpdated", state.live?.now ? `更新于 ${state.live.now}` : "");
}

function renderLogPreview(lastLine) {
  const lines = state.logLines.length ? state.logLines.slice(-6) : (lastLine ? [lastLine] : []);
  if (!lines.length) {
    $("logPreview").textContent = "等待启动…";
    return;
  }
  $("logPreview").innerHTML = lines.map((line) => {
    const cls = /WARNING|WARN|error|failed|ERR/i.test(line) ? (/error|failed|ERR/i.test(line) ? "err" : "warn") : (/ready|session is up|loaded/i.test(line) ? "ok" : "");
    return cls ? `<span class="${cls}">${esc(line)}</span>` : esc(line);
  }).join("\n");
}

function renderLogs() {
  const out = $("consoleOutput");
  if (!state.logLines.length) {
    out.textContent = "等待启动…";
    return;
  }
  out.innerHTML = state.logLines.map((line) => {
    const cls = /WARNING|WARN/i.test(line) ? "warn" : (/error|failed|ERR/i.test(line) ? "err" : (/ready|session is up|loaded/i.test(line) ? "ok" : "dim"));
    return `<span class="${cls}">${esc(line)}</span>`;
  }).join("\n");
  if (!state.logPaused) out.scrollTop = out.scrollHeight;
  setText("logPathLabel", state.logPath || "strata / server + engine");
  setText("logUpdated", `更新于 ${new Date().toLocaleTimeString("zh-CN")}`);
}

function updateButtons() {
  const p = state.live?.process || {};
  const raw = p.state || "stopped";
  const owned = !!p.owned;
  const hasConfig = !!(state.payload?.active || state.live?.config?.config_path);
  const transitioning = ["starting", "stopping", "restarting"].includes(raw);
  const canStart = hasConfig && !["starting", "loading", "running", "unloaded", "stopping", "restarting", "external"].includes(raw);
  const pendingRestart = state.pendingRestart && ["running", "loading", "unloaded", "external"].includes(raw);
  if (state.dirty || pendingRestart) {
    $("startBtn").disabled = !hasConfig || transitioning;
    $("startBtn").textContent = "应用更改";
    $("startBtn").title = state.dirty
      ? "保存修改；运行中的服务会提示是否重启"
      : "修改已保存，需要重启服务后生效";
  } else {
    $("startBtn").disabled = !canStart;
    $("startBtn").textContent = (raw === "stopped" || raw === "error") ? "启动服务" : (raw === "external" ? "外部服务" : "运行中");
    $("startBtn").title = "启动模型服务；启动模式为预加载时会同时加载模型，延迟加载则在首个请求时加载";
  }
  $("stopBtn").disabled = !((owned && !["stopped", "stopping"].includes(raw)) || raw === "external");
  $("loadBtn").disabled = !["running", "unloaded", "external"].includes(raw);
  $("unloadBtn").disabled = raw !== "running";
  $("restartBtn").disabled = !(raw === "running" || raw === "loading" || raw === "unloaded");
  $("openWebBtn").disabled = raw === "stopped" || raw === "error";
  $("apiMonitorLabel").textContent = isOn("apiMonitor") ? "api_monitor 开启" : "api_monitor 关闭";
  $("apiMonitorState").textContent = isOn("apiMonitor") ? "已开启" : "默认关闭";
  $("apiMonitorState").className = "badge " + (isOn("apiMonitor") ? "ok" : "plain");
}

async function pollState() {
  try {
    const s = await api("/api/state");
    render(s);
    if (s.app?.init_error && !s.config?.config_path) {
      setText("workspaceSub", s.app.init_error);
    }
    // If the active config or model folder changed on disk/by another tab, refresh the form.
    if (s.config?.config_path && state.payload?.active && s.config.config_path !== state.payload.active) {
      await loadConfig(true);
    } else if (s.config?.data_dir && state.payload?.data_dir && s.config.data_dir !== state.payload.data_dir) {
      await loadConfig(true);
    }
  } catch (e) {
    $("statusPill").dataset.state = "error";
    setText("statusText", "控制台连接失败");
    setText("statusSub", e.message);
  }
}

async function pollLogs() {
  if (state.logPaused) return;
  try {
    const j = await api("/api/logs?lines=300");
    state.logLines = j.lines || [];
    state.logPath = j.path || "";
    renderLogs();
    if (state.tab !== "console") renderLogPreview("");
  } catch (e) { /* the state poll reports connection errors */ }
}

async function loadConfig(quiet = false) {
  try {
    const payload = await api("/api/config");
    fillConfig(payload);
    if (!quiet) toast("配置已刷新", "ok");
  } catch (e) {
    toast(`读取配置失败: ${e.message}`, "err", 7000);
  }
}

async function refreshModels() {
  try {
    const r = await api("/api/models");
    fillModels({ ...(state.payload || {}), models: r.models || [], data_dir: r.data_dir || "" });
    setField("dataDir", r.data_dir || "");
    toast(`已扫描到 ${(r.models || []).filter((m) => m.installed).length} 个可用模型`, "ok");
  } catch (e) {
    toast(`扫描模型失败: ${e.message}`, "err", 7000);
  }
}

async function applyDataDir(path) {
  const dir = String(path || "").trim();
  if (!dir) return;
  try {
    if (state.dirty) await saveConfig(true);
    const payload = await post("/api/data-dir", { path: dir });
    fillConfig(payload);
    const n = (payload.models || []).filter((m) => m.installed).length;
    toast(`模型位置已更新，发现 ${n} 个可用模型`, "ok");
    const raw = state.live?.process?.state || "";
    if (["running", "loading", "unloaded"].includes(raw)) {
      toast("模型位置已切换；当前服务仍使用旧路径，重启后生效", "warn", 7000);
    }
  } catch (e) {
    toast(`切换模型位置失败: ${e.message}`, "err", 8000);
    await loadConfig(true);
  }
}

async function selectModel(id) {
  if (!id) return;
  const current = modelById(id);
  if (!current || !current.installed) {
    if (current) toast(current.reason || "模型文件不完整", "warn", 7000);
    return;
  }
  try {
    if (state.dirty) await saveConfig(true);
    const payload = await post("/api/models/select", { id });
    fillConfig(payload);
    const raw = state.live?.process?.state || "";
    if (["running", "loading", "unloaded"].includes(raw)) {
      if (confirm(`已切换到 ${current.name || current.id}。\n\n现在重启模型服务以加载新模型？`)) {
        await restartServer();
      } else {
        state.pendingRestart = true;
        updateButtons();
        toast("模型配置已切换；点击“应用更改”或“重启”后生效", "warn", 8000);
      }
    } else {
      toast(`已切换到 ${current.name || current.id}`, "ok");
    }
  } catch (e) {
    toast(`切换模型失败: ${e.message}`, "err", 8000);
    await loadConfig(true);
  }
}

async function exportLlamaSwap() {
  try {
    const res = await fetch("/api/export/llama-swap");
    if (!res.ok) throw new Error((await res.text()) || `HTTP ${res.status}`);
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "llama-swap.yaml";
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    toast("llama-swap 配置已导出", "ok");
  } catch (e) {
    toast(`导出失败: ${e.message}`, "err", 8000);
  }
}

async function saveAsConfig() {
  const current = state.payload?.view?.config_name || state.payload?.active || "strata-config";
  const suggested = current.replace(/^strata-/, "").replace(/\.json$/, "") + "-copy";
  const name = prompt("将当前配置另存为：", suggested);
  if (!name) return;
  $("saveMenu").hidden = true;
  try {
    await saveConfig(true);
    const payload = await post("/api/config/save-as", { name });
    fillConfig(payload);
    toast(`已另存为 ${payload.view?.config_name || name}`, "ok");
  } catch (e) {
    toast(`另存失败: ${e.message}`, "err", 8000);
  }
}

async function saveConfig(quiet = false) {
  const data = buildForm();
  if (!data.native && !data.pack) throw new Error("请先选择已安装模型");
  const payload = await post("/api/config", data);
  fillConfig(payload);
  if (!quiet) toast("配置已保存，备份已生成", "ok");
  return payload;
}

async function applyChanges() {
  const raw = state.live?.process?.state || "stopped";
  const needsRestart = ["running", "loading", "unloaded", "external"].includes(raw);
  const hadDirty = state.dirty;
  const restart = needsRestart && restartRequired();
  try {
    if (hadDirty) await saveConfig(true);
    if (!needsRestart) {
      toast("配置已保存；启动服务后生效", "ok");
      updateButtons();
      return;
    }
    if (!restart) {
      toast("配置已保存", "ok");
      updateButtons();
      return;
    }
    state.pendingRestart = true;
    updateButtons();
    if (!confirm("当前修改需重启模型服务才能生效，是否立即重启服务？")) {
      toast("配置已保存；重启服务后生效", "warn", 7000);
      updateButtons();
      return;
    }
    if (raw === "external") {
      await post("/api/stop");
      await post("/api/start", { lazy: value("startMode") === "lazy" });
      state.pendingRestart = false;
      toast("外部服务已停止，正在以新参数启动", "ok");
      await pollState();
      return;
    }
    await post("/api/restart", { lazy: value("startMode") === "lazy" });
    state.pendingRestart = false;
    toast("服务重启中", "ok");
    await pollState();
  } catch (e) {
    if (needsRestart) {
      state.pendingRestart = true;
      updateButtons();
    }
    toast(`应用更改失败: ${e.message}`, "err", 8000);
  }
}

async function startServer() {
  try {
    if (pathsChanged()) {
      if (!confirm("检测到模型/Pack/Tokenizer/引擎路径被修改。\n\n路径不匹配可能导致启动失败或输出异常。仍要继续吗？")) return;
    }
    await saveConfig(true);
    await post("/api/start", { lazy: value("startMode") === "lazy" });
    state.pendingRestart = false;
    toast("服务启动中", "ok");
    await pollState();
  } catch (e) {
    toast(`启动失败: ${e.message}`, "err", 8000);
  }
}

async function restartServer() {
  try {
    if (pathsChanged()) {
      if (!confirm("路径已修改，重启后服务可能无法加载。仍要继续吗？")) return;
    }
    await saveConfig(true);
    await post("/api/restart", { lazy: value("startMode") === "lazy" });
    state.pendingRestart = false;
    toast("服务重启中", "ok");
    await pollState();
  } catch (e) {
    toast(`重启失败: ${e.message}`, "err", 8000);
  }
}

async function stopServer() {
  const external = (state.live?.process?.state || "") === "external";
  const msg = external
    ? "停止外部模型服务？这会终止占用模型端口的 Strata 进程。"
    : "停止模型服务？这会释放显存。";
  if (!confirm(msg)) return;
  try {
    await post("/api/stop");
    state.pendingRestart = false;
    toast("服务已停止", "ok");
    await pollState();
  } catch (e) {
    toast(`停止失败: ${e.message}`, "err", 7000);
  }
}

async function loadModel() {
  try { await post("/api/load"); toast("正在加载模型", "ok"); await pollState(); }
  catch (e) { toast(`加载失败: ${e.message}`, "err", 7000); }
}

async function unloadModel() {
  try { await post("/api/unload"); toast("模型已卸载，下一次请求会重新加载", "ok"); await pollState(); }
  catch (e) { toast(`卸载失败: ${e.message}`, "err", 7000); }
}

async function openTarget(target) {
  try { await post("/api/open", { target }); }
  catch (e) { toast(`打开失败: ${e.message}`, "err", 6000); }
}

async function browse(kind, targetId, title) {
  try {
    const r = await post("/api/browse", { kind, title, current: value(targetId) });
    if (r.path) {
      setField(targetId, r.path);
      markDirty(targetId);
    }
  } catch (e) { toast(`选择失败: ${e.message}`, "err", 6000); }
}

function showModal(title, body, buttons = []) {
  setText("modalTitle", title);
  $("modalBody").innerHTML = body;
  const foot = $("modalFoot");
  foot.innerHTML = "";
  for (const b of buttons) {
    const el = document.createElement("button");
    el.className = `st-btn ${b.primary ? "st-btn--primary" : "st-btn--secondary"}`;
    el.textContent = b.label;
    el.addEventListener("click", () => b.onClick?.());
    foot.appendChild(el);
  }
  $("modalBackdrop").hidden = false;
}

function closeModal() { $("modalBackdrop").hidden = true; }

async function autoConfig() {
  try {
    toast("正在读取硬件与 Strata 推荐…", "warn", 3000);
    const r = await post("/api/autoconfig");
    if (!r.ok) throw new Error(r.error || "检测失败");
    const hw = r.hardware || {};
    const rec = r.recommendation || {};
    const gpu = (hw.gpus && hw.gpus[0]) || {};
    const currentCtx = numberValue("contextInput", 0);
    const suggested = Number(rec.context || 0);
    const rows = [];
    rows.push(["GPU", gpu.name ? `${gpu.name} · ${gpu.vram_gb} GB` : "—"]);
    rows.push(["RAM", hw.ram_gb ? `${hw.ram_gb} GB` : "—"]);
    rows.push(["CPU", hw.cpu?.name || "—"]);
    rows.push(["官方安装推荐", rec.title ? `${rec.title} · ${suggested ? Math.round(suggested / 1024) + "K" : "—"}` : "—"]);
    rows.push(["官方上下文建议", suggested ? `${Math.round(suggested / 1024)}K` : "—"]);
    rows.push(["当前上下文", currentCtx ? `${Math.round(currentCtx / 1024)}K` : "—"]);
    const table = `<table class="diff"><tbody>${rows.map(([k, v]) => `<tr><th>${esc(k)}</th><td>${esc(v)}</td></tr>`).join("")}</tbody></table>`;
    const note = `<p class="muted small">自动配置只调整运行参数，不会更换模型或修改路径。${rec.why ? `<br>${esc(rec.why)}` : ""}</p>`;
    showModal("自动配置预览", table + note, [
      { label: "取消", onClick: closeModal },
      { label: "应用推荐", primary: true, onClick: () => {
          if (suggested > 0) setField("contextInput", suggested);
          if (!["fp16", "int8", "q4_0", "k8v4"].includes(value("kvSelect"))) setField("kvSelect", "int8");
          if (!value("visionExe") || !value("visionMmproj")) $("visionCheck").checked = false;
          markDirty("*");
          closeModal();
          toast("已应用推荐值，保存后生效", "ok");
        } },
    ]);
  } catch (e) {
    toast(`自动配置失败: ${e.message}`, "err", 8000);
  }
}

async function showBackups() {
  try {
    const j = await api("/api/backups");
    const items = j.backups || [];
    if (!items.length) {
      showModal("备份管理", `<p class="muted">还没有备份。保存配置后会生成带时间戳的备份。</p>`, [
        { label: "打开备份目录", onClick: () => { closeModal(); openTarget("backups"); } },
        { label: "关闭", primary: true, onClick: closeModal },
      ]);
      return;
    }
    const list = `<div class="modal-list">${items.map((b) => `<div class="item"><div class="meta"><b>${esc(b.name)}</b><span>${esc(b.mtime_text)} · ${esc(b.size)} · ${esc(b.model_name || "—")} · ${b.context ? Math.round(b.context / 1024) + "K" : "—"} · ${esc(b.kv || "—")}</span></div><button class="st-btn st-btn--secondary st-btn--sm" data-restore="${esc(b.path)}">恢复</button></div>`).join("")}</div>`;
    showModal("备份管理", list, [
      { label: "打开备份目录", onClick: () => { closeModal(); openTarget("backups"); } },
      { label: "关闭", primary: true, onClick: closeModal },
    ]);
    $("modalBody").querySelectorAll("[data-restore]").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const path = btn.getAttribute("data-restore");
        if (!confirm("恢复这个备份？当前配置会先自动备份。")) return;
        try {
          const payload = await post("/api/backups/restore", { path });
          fillConfig(payload);
          closeModal();
          toast("备份已恢复", "ok");
        } catch (e) { toast(`恢复失败: ${e.message}`, "err", 7000); }
      });
    });
  } catch (e) { toast(`读取备份失败: ${e.message}`, "err", 7000); }
}

function quitConsole() {
  showModal("退出控制台后台服务", `<p>这会终止运行在 8090 的 Strata Console 后台服务，不是只关闭当前管理页面。</p><p class="muted small">仅退出后台服务时，模型 API 继续运行，但自动重启监督会停止；下次启动控制台会自动识别为外部服务。</p>`, [
    { label: "取消", onClick: closeModal },
    { label: "仅退出后台服务", onClick: async () => { try { await post("/api/quit", { stop_model: false }); closeModal(); setText("statusText", "后台服务已退出"); } catch (e) { toast(`退出失败: ${e.message}`, "err", 7000); } } },
    { label: "退出并停止模型", primary: true, onClick: async () => { try { await post("/api/quit", { stop_model: true }); closeModal(); setText("statusText", "已退出并停止"); } catch (e) { toast(`退出失败: ${e.message}`, "err", 7000); } } },
  ]);
}

async function showCommand() {
  try {
    const c = await api("/api/command");
    const engine = Array.isArray(c.engine_args) ? c.engine_args.join(" ") : "";
    showModal("启动命令预览", `<div class="muted small" style="margin-bottom:8px">服务命令</div><pre>${esc(c.server || "")}</pre><div class="muted small" style="margin:12px 0 8px">引擎参数</div><pre>${esc(engine || "（由配置提供）")}</pre>`, [
      { label: "关闭", primary: true, onClick: closeModal },
    ]);
  } catch (e) { toast(`读取命令失败: ${e.message}`, "err", 7000); }
}

function bindEvents() {
  document.querySelectorAll(".settings-tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".settings-tab").forEach((x) => {
        x.classList.toggle("active", x === tab);
        x.setAttribute("aria-selected", x === tab ? "true" : "false");
      });
      document.querySelectorAll(".settings-page").forEach((page) => {
        page.classList.toggle("active", page.id === "page-" + tab.dataset.panel);
      });
      $("settingsScroll").scrollTop = 0;
    });
  });

  const collapseKey = (target) => `strata-console-collapse:${target}`;
  document.querySelectorAll(".adv-toggle").forEach((btn) => {
    const body = $(btn.dataset.target);
    if (!body) return;
    try {
      const saved = localStorage.getItem(collapseKey(btn.dataset.target));
      if (saved === "1") {
        body.classList.add("open");
        btn.classList.add("open");
      } else if (saved === "0") {
        body.classList.remove("open");
        btn.classList.remove("open");
      }
    } catch (_) {}
    btn.addEventListener("click", () => {
      const open = body.classList.toggle("open");
      btn.classList.toggle("open", open);
      try { localStorage.setItem(collapseKey(btn.dataset.target), open ? "1" : "0"); } catch (_) {}
    });
  });

  document.querySelectorAll(".st-tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".st-tab").forEach((x) => x.setAttribute("aria-selected", x === tab ? "true" : "false"));
      document.querySelectorAll(".panel").forEach((p) => p.classList.toggle("active", p.id === "panel-" + tab.dataset.tab));
      state.tab = tab.dataset.tab;
      $("panels").classList.toggle("no-scroll", state.tab === "console");
      if (state.tab === "console") renderLogs();
      if (state.tab === "requests") pollState();
    });
  });

  $("reqTabBody").addEventListener("click", (e) => {
    const tr = e.target.closest("tr[data-req-key]");
    if (!tr) return;
    const key = tr.getAttribute("data-req-key") || "";
    state.selectedRequestKey = key;
    for (const row of $("reqTabBody").querySelectorAll("tr[data-req-key]"))
      row.classList.toggle("selected", row === tr);
    renderRequestDetail(state.requests.find((r) => requestKey(r) === key) || null);
    $("requestDetail").scrollIntoView({ block: "nearest", behavior: "smooth" });
  });

  const settings = $("settingsScroll");
  const managedField = (el) => el.matches("input, select") && el.id !== "modelSelect" && el.id !== "dataDir" && !el.id.endsWith("Preset");
  settings.addEventListener("input", (e) => {
    if (managedField(e.target)) { markDirty(e.target.id); }
  });
  settings.addEventListener("change", (e) => {
    if (managedField(e.target)) { markDirty(e.target.id); }
  });
  bindTokenPreset("kvResident", "kvResidentPreset");
  bindTokenPreset("prefill", "prefillPreset");
  bindTokenPreset("visionMaxTokens", "visionMaxTokensPreset");
  bindTokenPreset("maxTokens", "maxTokensPreset");
  bindTokenPreset("reasoningBudget", "reasoningBudgetPreset");
  bindTokenPreset("conversationCacheMinTokens", "conversationCacheMinTokensPreset");
  bindTokenPreset("conversationCacheParkGain", "conversationCacheParkGainPreset");

  $("configSelect").addEventListener("change", async () => {
    try {
      const payload = await post("/api/select", { path: $("configSelect").value });
      fillConfig(payload);
      toast("已切换配置档", "ok");
      await pollState();
    } catch (e) { toast(`切换失败: ${e.message}`, "err", 7000); }
  });

  $("contextPreset").addEventListener("change", () => {
    if ($("contextPreset").value) {
      setField("contextInput", $("contextPreset").value);
      markDirty("contextInput");
    }
    $("contextPreset").value = "";
  });

  $("lanToggle").addEventListener("click", () => {
    const on = !isOn("lanToggle");
    setToggle("lanToggle", on);
    $("host").value = on ? "0.0.0.0" : "127.0.0.1";
    updateLanBadge(on);
    markDirty("host");
  });

  bindToggle("autoRestart", (on) => {
    $("lazyRestart").disabled = !on;
    if (!on) setToggle("lazyRestart", true);
  });
  bindToggle("lazyRestart");
  bindToggle("fitMaxTokens");
  bindToggle("vramElastic", updateOptionalVisibility);
  bindToggle("answerRoom");
  bindToggle("reasoningCloseRetry");
  bindToggle("toolCallRecovery");
  bindToggle("codexThreadTitles");
  bindToggle("codexCompactionCache");
  bindToggle("mtpEnabled", (on) => {
    const v = state.payload?.view || {};
    $("mtpPath").disabled = !on;
    if (on && v.mtp_native && !value("spec")) setField("spec", "2");
    if (on && !v.mtp_native && !value("mtpPath")) setField("mtpPath", v.mtp_detected_path || "");
    updateMtpHint();
    updateOptionalVisibility();
  });
  $("mtpPath").addEventListener("input", updateMtpHint);
  $("spec").addEventListener("input", updateMtpHint);
  bindToggle("openBrowser");
  bindToggle("preventSleep");
  bindToggle("apiMonitor", (on) => {
    setToggle("apiMonitorToggle", on);
    updateButtons();
  });
  bindToggle("apiMonitorToggle", (on) => {
    setToggle("apiMonitor", on);
    updateButtons();
  });

  $("visionCheck").addEventListener("change", () => {
    $("visionDevice").disabled = !$("visionCheck").checked;
    $("visionExe").disabled = !$("visionCheck").checked;
    $("visionMmproj").disabled = !$("visionCheck").checked;
    $("visionMaxTokens").disabled = !$("visionCheck").checked;
  $("visionMaxTokensPreset").disabled = !$("visionCheck").checked;
    $("visionThreads").disabled = !$("visionCheck").checked;
    updateVisionNote();
    updateVisionHint();
    updateOptionalVisibility();
    markDirty("visionCheck");
  });
  $("lowRam").addEventListener("change", () => {
    if (value("lowRam") === "resident") setField("residentBudgetGib", "");
    updateLowRamUi();
  });
  $("residentBudgetGib").addEventListener("input", () => {
    if (value("residentBudgetGib")) {
      if (value("lowRam") !== "budget") setField("lowRam", "budget");
    } else if (value("lowRam") === "budget") {
      setField("lowRam", "mmap");
    }
    updateLowRamUi();
  });
  $("visionDevice").addEventListener("change", () => {
    syncVisionExe();
    updateVisionNote();
    updateVisionHint(true);
    updateOptionalVisibility();
  });
  bindToggle("visionElastic", updateOptionalVisibility);
  $("visionExe").addEventListener("input", () => { updateVisionNote(); updateVisionHint(); });
  $("visionMmproj").addEventListener("input", () => { updateVisionNote(); updateVisionHint(); });

  $("apiKey").addEventListener("input", () => {
    state.clearKey = false;
    if (value("apiKey")) {
      state.apiKeySet = true;
      $("keyState").textContent = "保存后生效";
    }
    updateCorsHint();
  });

  $("genKey").addEventListener("click", () => {
    const a = new Uint8Array(24);
    crypto.getRandomValues(a);
    setField("apiKey", "sk-strata-" + Array.from(a, (x) => x.toString(16).padStart(2, "0")).join(""));
    state.clearKey = false;
    state.apiKeySet = true;
    $("keyState").textContent = "保存后生效（可先点“复制”）";
    updateCorsHint();
    markDirty("apiKey");
  });
  $("copyKeyBtn").addEventListener("click", copyKey);
  $("clearKey").addEventListener("click", () => {
    state.clearKey = true;
    state.apiKeySet = false;
    setField("apiKey", "");
    $("keyState").textContent = "保存后清除";
    updateCorsHint();
    markDirty("apiKey");
  });
  $("corsOrigins").addEventListener("input", updateCorsHint);

  $("exportLlamaSwapBtn").addEventListener("click", exportLlamaSwap);
  $("mergeUserArgsBtn").addEventListener("click", async () => {
    if (!confirm("将 user_args 合并到配置并删除 user_args？\n\n合并后只以配置文件为准。")) return;
    try {
      const payload = await post("/api/user-args/merge");
      fillConfig(payload);
      toast("user_args 已合并到配置文件", "ok");
    } catch (e) {
      toast(`合并失败: ${e.message}`, "err", 8000);
    }
  });
  $("saveMenuBtn").addEventListener("click", (e) => {
    e.stopPropagation();
    const menu = $("saveMenu");
    menu.hidden = !menu.hidden;
    $("saveMenuBtn").setAttribute("aria-expanded", String(!menu.hidden));
  });
  $("saveAsBtn").addEventListener("click", (e) => {
    e.stopPropagation();
    saveAsConfig();
  });
  document.addEventListener("click", () => {
    const menu = $("saveMenu");
    if (menu && !menu.hidden) {
      menu.hidden = true;
      $("saveMenuBtn").setAttribute("aria-expanded", "false");
    }
  });
  $("saveBtn").addEventListener("click", async () => {
    try {
      const raw = state.live?.process?.state || "stopped";
      const needsRestart = ["running", "loading", "unloaded", "external"].includes(raw) && restartRequired();
      await saveConfig();
      if (needsRestart) {
        state.pendingRestart = true;
        updateButtons();
        toast("配置已保存；点击“应用更改”重启后生效", "warn", 7000);
      }
      await pollState();
    } catch (e) {
      toast(`保存失败: ${e.message}`, "err", 8000);
    }
  });
  $("startBtn").addEventListener("click", () => {
    if (state.dirty || state.pendingRestart) applyChanges();
    else startServer();
  });
  $("stopBtn").addEventListener("click", stopServer);
  $("restartBtn").addEventListener("click", restartServer);
  $("loadBtn").addEventListener("click", loadModel);
  $("unloadBtn").addEventListener("click", unloadModel);
  $("openWebBtn").addEventListener("click", () => {
    window.open(`http://127.0.0.1:${activeModelPort()}/`, "_blank", "noopener");
  });
  $("autoConfigBtn").addEventListener("click", autoConfig);
  $("backupsBtn").addEventListener("click", showBackups);
  $("openBackupDirBtn").addEventListener("click", () => openTarget("backups"));
  $("commandBtn").addEventListener("click", showCommand);
  $("quitConsoleBtn").addEventListener("click", quitConsole);
  $("openOfficialMonitor").addEventListener("click", () => {
    if (!isOn("apiMonitor")) {
      toast("请先开启 api_monitor 并重启服务", "warn");
      return;
    }
    window.open(`http://127.0.0.1:${activeModelPort()}/api-monitor`, "_blank", "noopener");
  });
  $("browseDataDir").addEventListener("click", async () => {
    await browse("folder", "dataDir", "选择 Strata-data 目录");
    await applyDataDir(value("dataDir"));
  });
  $("refreshModels").addEventListener("click", refreshModels);
  $("modelSelect").addEventListener("change", () => selectModel(value("modelSelect")));
  $("dataDir").addEventListener("change", () => applyDataDir(value("dataDir")));
  $("browseMtp").addEventListener("click", async () => {
    await browse("folder", "mtpPath", "选择 MTP runtime 目录");
    updateMtpHint();
  });
  $("copyApiBtn").addEventListener("click", copyApi);
  $("copyChatBtn").addEventListener("click", () => copyField("chatEndpoint", "Chat 端点"));
  $("copyAnthropicBtn").addEventListener("click", () => copyField("anthropicEndpoint", "Anthropic 端点"));
  $("copyModelBtn").addEventListener("click", () => copyField("modelName", "模型名"));
  $("modelName").addEventListener("input", updateApiBase);
  $("port").addEventListener("input", updateApiBase);
  $("host").addEventListener("change", () => {
    const on = value("host") !== "127.0.0.1";
    setToggle("lanToggle", on);
    updateLanBadge(on);
    markDirty("host");
  });
  $("openConfigBtn").addEventListener("click", () => openTarget("config"));
  $("openConfigBtn2").addEventListener("click", () => openTarget("config"));
  $("openLogBtn").addEventListener("click", () => openTarget("log"));
  $("reloadBtn").addEventListener("click", () => loadConfig());
  $("configRefresh").addEventListener("click", () => loadConfig());
  $("modalClose").addEventListener("click", closeModal);
  $("modalBackdrop").addEventListener("click", (e) => { if (e.target === $("modalBackdrop")) closeModal(); });

  $("pauseLog").addEventListener("click", () => {
    state.logPaused = !state.logPaused;
    $("pauseLog").textContent = state.logPaused ? "继续滚动" : "暂停滚动";
    $("logFollow").textContent = state.logPaused ? "已暂停" : "实时跟随";
    $("logFollow").className = "badge " + (state.logPaused ? "warn" : "ok");
  });
  $("clearLog").addEventListener("click", () => {
    state.logLines = [];
    $("consoleOutput").textContent = "视图已清空；新日志会继续追加。";
  });

  const themeBtn = $("themeBtn");
  try {
    const saved = localStorage.getItem("strata.console.theme");
    if (saved) document.documentElement.dataset.theme = saved;
  } catch (e) {}
  syncTheme();
  themeBtn.addEventListener("click", () => {
    document.documentElement.dataset.theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
    try { localStorage.setItem("strata.console.theme", document.documentElement.dataset.theme); } catch (e) {}
    syncTheme();
  });
}

function syncTheme() {
  const dark = document.documentElement.dataset.theme === "dark";
  $("themeIcon").setAttribute("href", dark ? "#i-sun" : "#i-moon");
}

async function boot() {
  bindEvents();
  try {
    const s = await api("/api/session");
    state.csrf = s.token;
    await loadConfig(true);
    await pollState();
    await pollLogs();
    state.pollTimer = setInterval(pollState, 1500);
    setInterval(pollLogs, 1500);
  } catch (e) {
    toast(`控制台初始化失败: ${e.message}`, "err", 10000);
    setText("statusText", "初始化失败");
  }
}

document.addEventListener("DOMContentLoaded", boot);
