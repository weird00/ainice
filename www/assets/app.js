import { getBridgeClient } from "/platform/sdk/bridge.js";
import { Locale } from "/platform/sdk/locale.js";
import { Toast } from "/platform/sdk/toast.js";

const MODULE = "ha-webhook";
const STATUS_INTERVAL = 5000;
const LOG_INTERVAL = 10000;
const LOG_LIMIT = 30;
// 与后端 TRIGGER_LIMIT 一致；页面靠它提前挡住第 33 条。
const TRIGGER_LIMIT = 32;
// 平台固定八个关注区（Zone-A…Zone-H）。板端尚未上报快照时也要能先把触发器配好，
// 所以用它给下拉框兜底，真实区域列表到了以后再补进来。
const ZONE_FALLBACK = [
  "Zone-A", "Zone-B", "Zone-C", "Zone-D",
  "Zone-E", "Zone-F", "Zone-G", "Zone-H",
];

const TEXT = {
  title: { zh: "Home Assistant 触发", en: "Home Assistant Trigger" },
  subtitle: {
    zh: "有人无人状态变化或手动触发时，向 Home Assistant 的 Webhook 发送一条命令。",
    en: "Send a command to a Home Assistant webhook on presence changes or on demand.",
  },
  connSection: { zh: "连接配置", en: "Connection" },
  connHint: {
    zh: "Home Assistant 地址与共享密钥只需配置一次，下面的触发器共用这一份设置。",
    en: "The Home Assistant URL and shared secret are set once and reused by every trigger below.",
  },
  triggerSection: { zh: "触发器", en: "Triggers" },
  triggerHint: {
    zh: "每条触发器各自带一个 Webhook ID，并各自指定监听哪个区域、在有人还是无人时投递；需要一个就增加一条。",
    en: "Each trigger has its own webhook ID and picks the zone and the state it listens to. Add one entry per webhook you need.",
  },
  triggerTitle: { zh: "触发器 {index}", en: "Trigger {index}" },
  addTrigger: { zh: "新增触发器", en: "Add trigger" },
  removeTrigger: { zh: "删除", en: "Remove" },
  testTrigger: { zh: "测试", en: "Test" },
  triggerEmpty: {
    zh: "还没有触发器，点「新增触发器」为某个区域添加一条。",
    en: "No triggers yet. Use \"Add trigger\" to create one for a zone.",
  },
  scope: { zh: "监听范围", en: "Scope" },
  scopeGlobal: { zh: "全局（所有区域）", en: "Global (all zones)" },
  event: { zh: "触发状态", en: "Trigger on" },
  eventPresent: { zh: "有人", en: "Present" },
  eventAbsent: { zh: "无人", en: "Absent" },
  eventBoth: { zh: "有人或无人", en: "Either" },
  triggerCount: { zh: "共 {count} 条 · 已就绪 {ready} 条", en: "{count} total · {ready} ready" },
  triggerLimit: { zh: "触发器最多 {limit} 条", en: "At most {limit} triggers" },
  statusSection: { zh: "运行状态", en: "Status" },
  logSection: { zh: "最近记录", en: "Recent log" },
  save: { zh: "保存配置", en: "Save" },
  trigger: { zh: "立即触发一次", en: "Trigger now" },
  test: { zh: "发送测试命令", en: "Send test" },
  enable: { zh: "启用自动触发", en: "Auto trigger" },
  haUrl: { zh: "Home Assistant 地址", en: "Home Assistant URL" },
  webhookId: { zh: "Webhook ID", en: "Webhook ID" },
  secret: { zh: "共享密钥", en: "Shared secret" },
  secretHint: {
    zh: "可选。会作为命令里的 token 字段发出，由 Home Assistant 侧条件校验。",
    en: "Optional. Sent as the token field and checked by a condition on the HA side.",
  },
  targetEmpty: { zh: "尚未配置", en: "Not configured" },
  saveOk: { zh: "配置已保存", en: "Configuration saved" },
  saveFail: { zh: "保存失败", en: "Save failed" },
  sendOk: { zh: "命令已送达 Home Assistant", en: "Command delivered to Home Assistant" },
  sendFail: { zh: "发送失败", en: "Send failed" },
  testOk: { zh: "测试命令已送达 Home Assistant", en: "Test command delivered" },
  sendConfig: { zh: "请先填写 HA 地址和 Webhook ID", en: "Fill in the HA URL and webhook ID first" },
  sendConnect: { zh: "无法连接到 Home Assistant", en: "Cannot reach Home Assistant" },
  sendHttp: { zh: "Home Assistant 返回了错误状态", en: "Home Assistant returned an error status" },
  running: { zh: "运行开关", en: "Auto trigger" },
  on: { zh: "已启用", en: "Enabled" },
  off: { zh: "已停用", en: "Disabled" },
  presence: { zh: "状态自动触发", en: "Presence source" },
  presenceReady: { zh: "可用", en: "Available" },
  presenceMissing: { zh: "板端 SDK 未提供订阅符号", en: "Not provided by the board SDK" },
  stats: { zh: "发送统计", en: "Delivery counters" },
  statsValue: { zh: "成功 {sent} · 失败 {fail} · 排队 {queued}", en: "{sent} sent · {fail} failed · {queued} queued" },
  last: { zh: "最近一次", en: "Last result" },
  lastNone: { zh: "尚无记录", en: "No records yet" },
  stOk: { zh: "成功", en: "OK" },
  stConfig: { zh: "未配置", en: "Not configured" },
  stConnect: { zh: "连接失败", en: "Connection failed" },
  stHttp: { zh: "HTTP 错误", en: "HTTP error" },
  stOther: { zh: "其他错误", en: "Other error" },
  kindSystem: { zh: "系统", en: "System" },
  kindPresence: { zh: "自动", en: "Auto" },
  kindManual: { zh: "手动", en: "Manual" },
  kindTest: { zh: "测试", en: "Test" },
  logEmpty: { zh: "暂无记录", en: "No records yet" },
  initFail: { zh: "插件后端暂时不可用，请稍后重试。", en: "Plugin backend is unavailable, please retry." },
};

const STATUS_TEXT = {
  ok: "stOk",
  config: "stConfig",
  connect: "stConnect",
  http: "stHttp",
  other: "stOther",
};

const STATUS_CLASS = {
  ok: "ok",
  config: "warn",
  connect: "bad",
  http: "bad",
  other: "bad",
};

const KIND_TEXT = {
  system: "kindSystem",
  presence: "kindPresence",
  manual: "kindManual",
  test: "kindTest",
};

let language = "zh";
let config = null;
let lastState = null;
let logItems = [];
let timers = [];
let lifecycleDone = false;
let busy = false;

const bridge = getBridgeClient();

function t(key) {
  const entry = TEXT[key];
  if (!entry) {
    return key;
  }
  return entry[language] || entry.zh;
}

function fill(key, values) {
  return t(key).replace(/\{(\w+)\}/g, (match, name) =>
    Object.prototype.hasOwnProperty.call(values, name) ? String(values[name]) : match
  );
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined && text !== null) {
    node.textContent = text;
  }
  return node;
}

function clear(node) {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

function fieldRow(labelText, control, stacked) {
  const row = el("div", stacked ? "field stacked" : "field");
  row.appendChild(el("label", null, labelText));
  const wrap = el("div", "control");
  wrap.appendChild(control);
  row.appendChild(wrap);
  return row;
}

function textInput(value, type, placeholder) {
  const input = document.createElement("input");
  input.type = type || "text";
  input.value = value === undefined || value === null ? "" : String(value);
  input.spellcheck = false;
  input.autocomplete = "off";
  if (placeholder) {
    input.placeholder = placeholder;
  }
  return input;
}

function checkbox(checked) {
  const input = document.createElement("input");
  input.type = "checkbox";
  input.checked = !!checked;
  return input;
}

/* options 为 [{value, text}]；value 不在候选里时补一个同名选项，避免把已存的值改掉。 */
function selectInput(options, value, onChange) {
  const select = document.createElement("select");
  options.forEach((item) => {
    const option = document.createElement("option");
    option.value = item.value;
    option.textContent = item.text;
    select.appendChild(option);
  });
  select.value = value;
  if (select.value !== value) {
    const option = document.createElement("option");
    option.value = value;
    option.textContent = value;
    select.appendChild(option);
    select.value = value;
  }
  select.addEventListener("change", () => onChange(select.value));
  return select;
}

function infoRow(labelText, valueText, valueClass, mono) {
  const row = el("div", "row");
  row.appendChild(el("span", "label", labelText));
  const value = el("span", mono ? "value mono" : "value", valueText);
  if (valueClass) {
    value.classList.add(valueClass);
  }
  row.appendChild(value);
  return row;
}

function renderStatic() {
  document.querySelectorAll("[data-i18n]").forEach((node) => {
    node.textContent = t(node.getAttribute("data-i18n"));
  });
  const toggle = document.getElementById("lang-toggle");
  toggle.textContent = language === "zh" ? "EN" : "中文";
}

function renderConfig() {
  const box = document.getElementById("config-fields");
  clear(box);
  if (!config) {
    return;
  }

  const enabled = checkbox(config.enabled);
  enabled.addEventListener("change", () => {
    config.enabled = enabled.checked;
  });
  box.appendChild(fieldRow(t("enable"), enabled));

  const url = textInput(config.ha_url, "text", "http://192.168.1.10:8123");
  url.addEventListener("input", () => {
    config.ha_url = url.value.trim();
  });
  box.appendChild(fieldRow(t("haUrl"), url, true));

  const secret = textInput(config.secret, "password");
  secret.addEventListener("input", () => {
    config.secret = secret.value;
  });
  box.appendChild(fieldRow(t("secret"), secret, true));
  box.appendChild(el("p", "hint", t("secretHint")));
}

/* 监听范围候选：板端已上报的区域在前，兜底区域在后，已有的取值一个都不丢。 */
function scopeOptions() {
  const values = [];
  const push = (value) => {
    if (value && !values.includes(value)) {
      values.push(value);
    }
  };
  ((lastState && lastState.zones) || []).forEach(push);
  ((config && config.triggers) || []).forEach((item) => push(item.scope));
  ZONE_FALLBACK.forEach(push);
  return values
    .filter((value) => value !== "global")
    .map((value) => ({ value, text: value }));
}

const EVENT_OPTIONS = [
  { value: "present", textKey: "eventPresent" },
  { value: "absent", textKey: "eventAbsent" },
  { value: "both", textKey: "eventBoth" },
];

function triggerCard(trigger, index) {
  const card = el("div", "trigger");

  const head = el("div", "trigger-head");
  head.appendChild(el("span", "trigger-title", fill("triggerTitle", { index: index + 1 })));
  const actions = el("div", "trigger-actions");

  const test = el("button", "ghost", t("testTrigger"));
  test.type = "button";
  test.addEventListener("click", () => testTrigger(trigger.id));
  actions.appendChild(test);

  const remove = el("button", "ghost danger", t("removeTrigger"));
  remove.type = "button";
  remove.addEventListener("click", () => removeTrigger(trigger.id));
  actions.appendChild(remove);

  head.appendChild(actions);
  card.appendChild(head);

  const webhook = textInput(trigger.webhook_id, "text", "ha_trigger_ainice");
  webhook.addEventListener("input", () => {
    trigger.webhook_id = webhook.value.trim();
  });
  card.appendChild(fieldRow(t("webhookId"), webhook, true));

  const scopeItems = [{ value: "global", text: t("scopeGlobal") }].concat(scopeOptions());
  card.appendChild(
    fieldRow(
      t("scope"),
      selectInput(scopeItems, trigger.scope, (value) => {
        trigger.scope = value;
      })
    )
  );

  const eventItems = EVENT_OPTIONS.map((item) => ({ value: item.value, text: t(item.textKey) }));
  card.appendChild(
    fieldRow(
      t("event"),
      selectInput(eventItems, trigger.event, (value) => {
        trigger.event = value;
      })
    )
  );

  return card;
}

function renderTriggers() {
  const box = document.getElementById("trigger-list");
  const add = document.getElementById("add-trigger");
  clear(box);
  if (!config) {
    add.disabled = true;
    return;
  }
  const triggers = config.triggers || [];
  if (!triggers.length) {
    box.appendChild(el("p", "hint", t("triggerEmpty")));
  }
  triggers.forEach((trigger, index) => box.appendChild(triggerCard(trigger, index)));
  add.disabled = triggers.length >= TRIGGER_LIMIT;
}

function nextTriggerId() {
  const used = new Set((config.triggers || []).map((item) => item.id));
  let index = 1;
  while (used.has(`t${index}`)) {
    index += 1;
  }
  return `t${index}`;
}

function addTrigger() {
  if (!config) {
    return;
  }
  if ((config.triggers || []).length >= TRIGGER_LIMIT) {
    Toast.error(fill("triggerLimit", { limit: TRIGGER_LIMIT }));
    return;
  }
  config.triggers.push({
    id: nextTriggerId(),
    webhook_id: "",
    scope: "global",
    event: "both",
  });
  renderTriggers();
}

function removeTrigger(id) {
  if (!config) {
    return;
  }
  config.triggers = (config.triggers || []).filter((item) => item.id !== id);
  renderTriggers();
}

function renderStatus(state) {
  const box = document.getElementById("live-status");
  clear(box);
  if (!state) {
    return;
  }

  box.appendChild(
    infoRow(t("running"), state.enabled ? t("on") : t("off"), state.enabled ? "ok" : "warn")
  );
  box.appendChild(
    infoRow(t("haUrl"), state.ha_url || t("targetEmpty"), state.ha_url ? null : "warn", true)
  );
  box.appendChild(
    infoRow(
      t("triggerSection"),
      fill("triggerCount", {
        count: state.trigger_count || 0,
        ready: state.ready_count || 0,
      })
    )
  );
  box.appendChild(
    infoRow(
      t("presence"),
      state.presence_available ? t("presenceReady") : t("presenceMissing"),
      state.presence_available ? null : "warn"
    )
  );
  box.appendChild(
    infoRow(
      t("stats"),
      fill("statsValue", {
        sent: state.sent_total || 0,
        fail: state.fail_total || 0,
        queued: state.queued || 0,
      })
    )
  );

  const last = state.last || {};
  if (!last.time) {
    box.appendChild(infoRow(t("last"), t("lastNone")));
    return;
  }
  const statusKey = STATUS_TEXT[last.status] || "stOther";
  const parts = [`${t(statusKey)} · ${last.time}`];
  if (last.http_code && last.http_code > 0) {
    parts.push(`HTTP ${last.http_code}`);
  }
  if (last.detail) {
    parts.push(last.detail);
  }
  box.appendChild(infoRow(t("last"), parts.join(" · "), STATUS_CLASS[last.status] || "bad"));
}

function kindLabel(kind) {
  const key = KIND_TEXT[kind];
  return key ? t(key) : kind;
}

function isBadEntry(item) {
  return /失败|丢弃|不可用|停用/.test(item.text || "");
}

function renderLog() {
  const box = document.getElementById("log");
  clear(box);
  if (!logItems.length) {
    box.appendChild(el("p", "hint", t("logEmpty")));
    return;
  }
  logItems.forEach((item) => {
    const row = el("div", "log-row");
    row.appendChild(el("span", "log-time", item.time || ""));
    const kind = el("span", "log-kind", kindLabel(item.kind || ""));
    if (isBadEntry(item)) {
      kind.classList.add("bad");
    }
    row.appendChild(kind);
    row.appendChild(el("span", "log-text", item.text || ""));
    box.appendChild(row);
  });
}

async function request(method, params) {
  return bridge.request({ module: MODULE, method, params: params || {} });
}

async function refreshStatus() {
  try {
    const result = await request("state.get");
    if (result && result.ok) {
      lastState = result;
      renderStatus(result);
    }
  } catch (error) {
    // 轮询失败保留上一次显示，不打断页面
  }
}

async function refreshLog() {
  try {
    const result = await request("log.get", { limit: LOG_LIMIT });
    if (result && result.ok) {
      logItems = result.items || [];
      renderLog();
    }
  } catch (error) {
    // 同上
  }
}

async function save() {
  try {
    const result = await request("config.save", {
      enabled: !!config.enabled,
      ha_url: config.ha_url || "",
      secret: config.secret || "",
      triggers: config.triggers || [],
    });
    if (result && result.ok) {
      config = result.config;
      renderConfig();
      renderTriggers();
      Toast.success(t("saveOk"));
      refreshStatus();
    } else {
      Toast.error((result && result.error) || t("saveFail"));
    }
  } catch (error) {
    Toast.error(t("saveFail"));
  }
}

function sendFailureText(result) {
  const status = result && result.status;
  if (status === "config") {
    return t("sendConfig");
  }
  if (status === "connect") {
    return t("sendConnect");
  }
  if (status === "http") {
    return `${t("sendHttp")} (HTTP ${result.http_code})`;
  }
  return (result && result.detail) || t("sendFail");
}

function setActionsDisabled(disabled) {
  ["trigger", "test"].forEach((id) => {
    const node = document.getElementById(id);
    if (node) {
      node.disabled = disabled;
    }
  });
}

/* 手动触发：params 为空时发给所有触发器，带 trigger_id 时只发那一条。 */
async function sendTrigger(method, params, okText) {
  if (busy) {
    return;
  }
  busy = true;
  setActionsDisabled(true);
  try {
    const result = await request(method, params);
    if (result && result.ok) {
      Toast.success(okText);
    } else {
      Toast.error(sendFailureText(result));
    }
  } catch (error) {
    Toast.error(t("sendFail"));
  } finally {
    busy = false;
    setActionsDisabled(false);
    refreshStatus();
    refreshLog();
  }
}

function testTrigger(triggerId) {
  return sendTrigger("trigger.test", { trigger_id: triggerId }, t("testOk"));
}

function finishLifecycle(failed) {
  const ui = window.AIniceUI;
  if (!ui || lifecycleDone) {
    return;
  }
  lifecycleDone = true;
  if (failed) {
    if (typeof ui.fail === "function") {
      ui.fail();
    }
    return;
  }
  if (typeof ui.ready === "function") {
    ui.ready();
  }
}

function stopTimers() {
  timers.forEach((timer) => window.clearInterval(timer));
  timers = [];
}

function renderAll() {
  renderStatic();
  renderConfig();
  renderTriggers();
  renderStatus(lastState);
  renderLog();
}

async function init() {
  language = Locale.getLanguage() === "en" ? "en" : "zh";
  renderStatic();

  const result = await request("config.get");
  if (!result || !result.ok) {
    throw new Error((result && result.error) || "config.get failed");
  }
  config = result.config;

  renderAll();
  finishLifecycle(false);

  refreshStatus();
  refreshLog();
  timers.push(window.setInterval(refreshStatus, STATUS_INTERVAL));
  timers.push(window.setInterval(refreshLog, LOG_INTERVAL));
}

document.getElementById("save").addEventListener("click", save);
document.getElementById("add-trigger").addEventListener("click", addTrigger);
document
  .getElementById("trigger")
  .addEventListener("click", () => sendTrigger("trigger.send", {}, t("sendOk")));
document
  .getElementById("test")
  .addEventListener("click", () => sendTrigger("trigger.test", {}, t("testOk")));
document.getElementById("lang-toggle").addEventListener("click", () => {
  Locale.setLanguage(language === "zh" ? "en" : "zh");
});

Locale.addListener(() => {
  language = Locale.getLanguage() === "en" ? "en" : "zh";
  renderAll();
});

window.addEventListener("pagehide", stopTimers);

init().catch((error) => {
  const box = document.getElementById("config-fields");
  clear(box);
  box.appendChild(el("p", "hint", t("initFail")));
  finishLifecycle(true);
  console.error(error);
});
