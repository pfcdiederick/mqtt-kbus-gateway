// KBUS Manager (MQTT): the page of the mqtt-manager at http(s)://<pfc>/kbus/, in the layout of
// the controller's Web-based Management, like the KBUS Manager of the HTTP/wdx gateway.
//
// Reads: a minimal MQTT 3.1.1 client over WebSocket (QoS 0, no library, works without
// internet), connected to the WebSocket next to the page:
//   http(s)://<pfc>/kbus/  ->  ws(s)://<pfc>/kbus/ws   (lighttpd proxies it to mosquitto, read only)
// Subscribes to <prefix>/# (kbus-gateway) and bridge/status, bridge/+/status (mqtt-bridge).
// Writes: POST api/publish {topic, value} to the mqtt-manager, which checks the login and
// publishes on the broker. Reset Statistics and the login settings always need the login.
"use strict";

const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const enc = new TextEncoder();
const dec = new TextDecoder("utf-8", { fatal: true });
const WS_URL = (location.protocol === "https:" ? "wss://" : "ws://") + location.host +
  location.pathname.replace(/[^/]*$/, "") + "ws";
const CLASSES = ["di", "do", "ai", "ao"];

let PREFIX = "kbus", READONLY = false;

// ---- navigation: main menu (tabs) and sub menu, like the WBM ---------------
const MENU = [
  { id: "status", title: "Status", pages: [["kbus", "KBUS Status"], ["modules", "Modules"]] },
  { id: "io", title: "I/O", pages: [["process", "Process Image"]] },
  { id: "mqtt", title: "MQTT", pages: [["topics", "Topics"], ["clients", "Clients"]] },
  { id: "settings", title: "Settings", pages: [["cycle", "KBUS Cycle"], ["login", "Login"]] },
];
let tab = "status", page = "kbus";
function route() {
  let [t, p] = location.hash.replace(/^#\/?/, "").split("/");
  const m = MENU.find(x => x.id === t) || MENU[0];
  if (!m.pages.some(x => x[0] === p)) p = m.pages[0][0];
  tab = m.id; page = p;
  document.querySelectorAll("#main-menu .tab-item").forEach(li => li.classList.toggle("active", li.dataset.tab === tab));
  $("sub-menu").innerHTML = m.pages.map(([id, title]) =>
    `<li class="${id === page ? "active" : ""}"><a href="#/${tab}/${id}">${esc(title)}</a></li>`).join("");
  document.querySelectorAll(".sub-content .wbm-page").forEach(d => d.classList.toggle("active", d.id === `page-${tab}-${page}`));
  msg("");
  if (page === "topics") renderTopics(true);
  if (page === "clients") pollClients();
  schedule();
}

function msg(text, ok) {
  const m = $("msg"); m.textContent = text || ""; m.className = text ? (ok ? "success" : "error") : "";
  if (text && ok) setTimeout(() => { if (m.textContent === text) msg(""); }, 3000);
}
function led(id, color, title) {
  const it = $(id); it.title = title; it.firstElementChild.className = "led " + (color || "");
}

// ---- state (from MQTT) ----------------------------------------------------------
const io = { di: [], do: [], ai: [], ao: [] };      // channel values, index = n-1
const bus = {};                                      // <prefix>/bus/<name>
let stats = {}, modules = [], gwStatus = null;
const bridges = {};                                  // <name> -> online|offline, "" = the bridge itself
const topics = new Map();                            // topic -> {text, time}
let layoutKey = "", dirty = false, topicsDirty = false;

function payloadText(p) {
  try { return dec.decode(p); }
  catch (e) { return [...p].map(b => b.toString(16).padStart(2, "0")).join(" "); }   // binary (750-495 ...)
}

function onMessage(topic, payload) {
  const deleted = payload.length === 0;              // empty retained message = topic removed
  const text = payloadText(payload);
  if (deleted) topics.delete(topic); else topics.set(topic, { text, time: Date.now() });
  topicsDirty = true;
  if (topic === PREFIX + "/status") {
    gwStatus = deleted ? null : text;
  } else if (topic.startsWith(PREFIX + "/")) {
    const rest = topic.slice(PREFIX.length + 1).split("/");
    if (CLASSES.includes(rest[0]) && rest.length === 2 && /^\d+$/.test(rest[1])) {
      const i = Number(rest[1]) - 1;
      if (deleted) io[rest[0]].length = Math.min(io[rest[0]].length, i);
      else io[rest[0]][i] = rest[0][0] === "d" ? text === "true" : Number(text);
    } else if (rest[0] === "bus" && rest.length === 2) {
      bus[rest[1]] = deleted ? undefined : text;
      if (rest[1] === "statistics") { try { stats = deleted ? {} : JSON.parse(text); } catch (e) { stats = {}; } }
    } else if (rest[0] === "modules" && rest.length === 1) {
      try { modules = deleted ? [] : JSON.parse(text); } catch (e) { modules = []; }
    } else if (rest[0] === "error" && rest.length === 1 && !deleted) {
      try {
        const e = JSON.parse(text);
        if (e.topic && Date.now() - lastWrite < 3000) msg("Rejected " + e.topic.slice(PREFIX.length + 1) + ": " + e.error);
      } catch (e) { /* not JSON */ }
    }
  } else if (topic === "bridge/status") {
    bridges[""] = deleted ? undefined : text;
  } else if (/^bridge\/[^/]+\/status$/.test(topic)) {
    bridges[topic.split("/")[1]] = deleted ? undefined : text;
  }
  schedule();
}
function schedule() { if (!dirty) { dirty = true; requestAnimationFrame(render); } }

// ---- rendering -------------------------------------------------------------------
const us = v => v != null ? v + " µs" : "–";
function render() {
  dirty = false;
  const online = gwStatus === "online", h = bus.healthy;
  document.body.classList.toggle("stale", !online || !connected);
  const st = $("st-healthy");
  if (!online || h === undefined) { st.className = "control"; st.textContent = "–"; }
  else { st.className = "control " + (h === "true" ? "ok-text" : "bad-text"); st.textContent = h === "true" ? "running" : "error"; }
  const g = $("st-gateway");
  g.className = "control " + (online ? "ok-text" : gwStatus ? "bad-text" : "");
  g.textContent = online ? "online" : gwStatus ? gwStatus + ": the values are the last ones it published" : "not seen on the broker";
  $("st-cycletime").textContent = bus.cycletime ? bus.cycletime + " ms" : "–";
  $("st-cycles").textContent = stats.cycles ?? "–";
  $("st-errors").textContent = stats.errors ?? "–";
  $("st-configversion").textContent = bus.configversion ?? "–";
  $("st-version").textContent = stats.version ? stats.version + (stats.backend === "simulation" ? " (SIMULATION, no real KBUS)" : "") : "–";
  for (const k of ["cycleDuration", "maxCycleDuration", "maxWakeupDelay"]) $("st-" + k).textContent = us(stats[k]);
  $("st-overruns").textContent = stats.overruns ?? "–";
  $("st-realtime").textContent = stats.realtime == null ? "–" : stats.realtime ? "yes" : "no";
  $("cyc-now").textContent = bus.cycletime ? bus.cycletime + " ms" : "–";
  if (bus.cycletime && document.activeElement !== $("cyc") && $("cyc").value === "") $("cyc").value = bus.cycletime;

  led("led-kbus", !online ? "" : h === "true" ? "green" : "red", !online ? "unknown" : h === "true" ? "KBUS running" : "KBUS error");
  led("led-gw", online ? "green" : gwStatus ? "red" : "", online ? "kbus-gateway online" : "kbus-gateway " + (gwStatus || "not seen"));
  $("status-text").textContent = online
    ? `Cycle ${bus.cycletime ?? "–"} ms · ${stats.cycles ?? "–"} cycles · ${stats.errors ?? "–"} errors`
    : connected ? "kbus-gateway " + (gwStatus || "not seen") : "";

  const key = CLASSES.map(channelCount).join(",") + "|" + bus.configversion + "|" + JSON.stringify(modules);
  if (key !== layoutKey) { layoutKey = key; renderModules(); buildIO(); }
  renderIO();
  if (page === "topics" && topicsDirty) renderTopics(false);
}

function channelCount(cls) {
  let n = 0;
  for (const m of modules) n += (m.channels && m.channels[cls] ? m.channels[cls].length : 0);
  return n || io[cls].length;
}
function where(cls, n) {
  for (const m of modules) {
    const l = m.channels && m.channels[cls];
    if (l && l.includes(n)) return `module ${m.position}, channel ${l.indexOf(n) + 1}`;
  }
  return "";
}
function renderModules() {
  $("modules").innerHTML = modules.map(m => {
    const ch = Object.entries(m.channels || {})
      .map(([c, l]) => c.toUpperCase() + l[0] + (l.length > 1 ? "–" + c.toUpperCase() + l[l.length - 1] : "")).join(", ");
    return `<tr><td>${esc(m.position)}</td><td>${esc(m.name)}</td><td>${esc(m.typeCode)}</td><td>${esc(ch || "no channels")}</td></tr>`;
  }).join("") || `<tr><td colspan="4">${gwStatus ? "No modules on the KBUS." : "Waiting for kbus-gateway …"}</td></tr>`;
}
function buildIO() {
  const n = c => channelCount(c), none = '<span class="none">none</span>';
  $("io-di").innerHTML = [...Array(n("di"))].map((_, i) =>
    `<div class="io-item" title="${where("di", i + 1)}">DI${i + 1}<div class="lamp" id="di-${i}"></div></div>`).join("") || none;
  $("io-do").innerHTML = [...Array(n("do"))].map((_, i) =>
    `<div class="io-item" title="${where("do", i + 1)}">DO${i + 1}<div class="lamp" id="do-${i}"></div>`
    + `<button class="w" data-do="${i + 1}" id="dob-${i}">OFF</button></div>`).join("") || none;
  $("io-ai").innerHTML = [...Array(n("ai"))].map((_, i) =>
    `<span title="${where("ai", i + 1)}">AI${i + 1}</span><div class="bar"><div id="aib-${i}"></div></div><span class="num" id="ai-${i}">–</span>`).join("") || none;
  $("io-ao").innerHTML = [...Array(n("ao"))].map((_, i) =>
    `<span title="${where("ao", i + 1)}">AO${i + 1}</span><div class="aorow"><div class="bar"><div id="aob-${i}"></div></div>`
    + `<input type="number" min="0" max="65535" id="aoi-${i}"><button class="w small" data-ao="${i + 1}">Set</button></div>`
    + `<span class="num" id="ao-${i}">–</span>`).join("") || none;
  applyWrite();
}
function renderIO() {
  io.di.forEach((x, i) => { const l = $("di-" + i); if (l) l.classList.toggle("on", x); });
  io.do.forEach((x, i) => {
    const l = $("do-" + i), b = $("dob-" + i);
    if (l) l.classList.toggle("on", x);
    if (b) { b.textContent = x ? "ON" : "OFF"; b.classList.toggle("action", x); b.dataset.state = x ? "1" : "0"; }
  });
  for (const c of ["ai", "ao"]) io[c].forEach((x, i) => {
    const n = $(`${c}-${i}`), b = $(`${c}b-${i}`);
    if (n) n.textContent = x;
    if (b) b.style.width = (x / 655.35).toFixed(1) + "%";
  });
}

function renderTopics(full) {
  topicsDirty = false;
  const f = $("tfilter").value.trim().toLowerCase();
  const rows = [...topics.keys()].sort().filter(t => !f || t.toLowerCase().includes(f));
  $("tcount").textContent = `${rows.length} of ${topics.size} topics`;
  const tb = $("ttable");
  const existing = new Map([...tb.rows].map(r => [r.dataset.t, r]));
  if (full || existing.size !== rows.length || rows.some((t, i) => tb.rows[i].dataset.t !== t)) {
    tb.innerHTML = rows.map(t => `<tr data-t="${esc(t)}"><td class="mono">${esc(t)}</td><td class="val mono"></td><td></td></tr>`).join("");
  }
  for (const r of tb.rows) {
    const e = topics.get(r.dataset.t);
    if (!e) continue;
    const v = r.cells[1], when = new Date(e.time).toLocaleTimeString();
    if (v.textContent !== e.text) {
      const first = v.textContent === "";
      v.textContent = e.text;
      if (!first) { v.classList.remove("flash"); void v.offsetWidth; v.classList.add("flash"); }
    }
    r.cells[2].textContent = when;
  }
}

// ---- connected clients (mqtt-manager: from the broker's log) -------------------------
let clientList = null, clientTimer = null;
async function pollClients() {
  clearTimeout(clientTimer);
  try {
    const r = await fetch("clients.json", { cache: "no-store", headers: authHeaders({}) });
    const j = r.ok ? await r.json() : null;
    clientList = j && j.ok ? j.clients : null;
  } catch (e) { clientList = null; }
  renderClients();
  if (page === "clients") clientTimer = setTimeout(pollClients, 5000);
}
function renderClients() {
  const local = a => a === "127.0.0.1" || a === "::1" || a === location.hostname;
  const name = c => c.id.startsWith("kbus-gateway") ? "kbus-gateway" : c.id.startsWith("mqtt-bridge") ? "mqtt-bridge"
    : c.id.startsWith("kbus-manager") ? "KBUS Manager" : local(c.address) ? "CODESYS" : "MQTT client";
  const list = (clientList || []).filter(c => !c.id.startsWith("kbus-monitor-") && !c.id.startsWith("kbus-manager"));
  $("ctable").innerHTML = clientList === null
    ? `<tr><td colspan="4">The mqtt-manager cannot read the broker's log.</td></tr>`
    : list.map(c => `<tr><td>${esc(name(c))}</td><td class="mono">${esc(c.id)}</td><td>${esc(c.address)}</td>`
      + `<td>${esc(new Date(c.since * 1000).toLocaleString())}</td></tr>`).join("")
      || `<tr><td colspan="4">No clients.</td></tr>`;
  const b = Object.entries(bridges).filter(([, v]) => v !== undefined).sort();
  $("btable").innerHTML = b.map(([k, v]) =>
    `<tr><td>${k ? "remote broker " + esc(k) : "mqtt-bridge (local broker)"}</td>`
    + `<td><span class="pill ${v === "online" ? "on" : "off"}">${esc(v)}</span></td></tr>`).join("")
    || `<tr><td colspan="2">No mqtt-bridge seen on the broker.</td></tr>`;
}

// ---- MQTT 3.1.1 over WebSocket (read only) --------------------------------------------
function varint(n) {
  const out = [];
  do { let b = n % 128; n = Math.floor(n / 128); if (n > 0) b |= 128; out.push(b); } while (n > 0);
  return out;
}
function str(s) { const b = enc.encode(s); return [b.length >> 8, b.length & 255, ...b]; }
function packet(type, body) { return new Uint8Array([type, ...varint(body.length), ...body]); }

let ws = null, rx = new Uint8Array(0), pingTimer = null, retry = 1000, connected = false;

function connect() {
  led("led-mqtt", "yellow", "Connecting to the MQTT broker …");
  try { ws = new WebSocket(WS_URL, ["mqtt"]); } catch (e) { scheduleReconnect(); return; }
  ws.binaryType = "arraybuffer";
  rx = new Uint8Array(0);
  ws.onopen = () => {
    const id = "kbus-monitor-" + Math.random().toString(16).slice(2, 10);
    ws.send(packet(0x10, [...str("MQTT"), 4, 0x02, 0, 30, ...str(id)]));   // clean session, keepalive 30 s
  };
  ws.onmessage = ev => {
    const d = new Uint8Array(ev.data), n = new Uint8Array(rx.length + d.length);
    n.set(rx); n.set(d, rx.length); rx = n;
    parse();
  };
  ws.onclose = () => {
    clearInterval(pingTimer);
    if (connected) msg("Connection to the MQTT broker lost, reconnecting …");
    connected = false;
    led("led-mqtt", "red", "MQTT broker not reachable (" + WS_URL + ")");
    schedule();
    scheduleReconnect();
  };
  ws.onerror = () => { try { ws.close(); } catch (e) { /* closed */ } };
}
function scheduleReconnect() { setTimeout(connect, retry); retry = Math.min(retry * 2, 15000); }

function parse() {
  for (;;) {
    let len = 0, mul = 1, i = 1;
    for (;;) {
      if (i >= rx.length) return;
      const b = rx[i++];
      len += (b & 127) * mul;
      if (!(b & 128)) break;
      mul *= 128;
      if (i > 4) { ws.close(); return; }
    }
    if (rx.length < i + len) return;
    const type = rx[0] >> 4, flags = rx[0] & 15, body = rx.subarray(i, i + len);
    if (type === 2) {                                    // CONNACK
      if (body[1] !== 0) { led("led-mqtt", "red", "MQTT broker refused the connection (" + body[1] + ")"); ws.close(); return; }
      connected = true; retry = 1000;
      led("led-mqtt", "green", "MQTT broker connected (" + WS_URL + ")");
      if ($("msg").textContent.startsWith("Connection to the MQTT broker")) msg("");
      for (const k in bridges) delete bridges[k];
      topics.clear();
      ws.send(packet(0x82, [0, 1, ...str(PREFIX + "/#"), 0, ...str("bridge/status"), 0, ...str("bridge/+/status"), 0]));
      pingTimer = setInterval(() => ws.readyState === 1 && ws.send(new Uint8Array([0xc0, 0])), 20000);
    } else if (type === 3) {                             // PUBLISH
      const tl = (body[0] << 8) | body[1];
      const topic = new TextDecoder().decode(body.subarray(2, 2 + tl));
      const qos = (flags >> 1) & 3;
      onMessage(topic, body.slice(2 + tl + (qos ? 2 : 0)));
    }
    rx = rx.slice(i + len);
  }
}

// ---- login for the protected functions (Reset Statistics, login settings) ------------------
// The credentials are kept for this browser tab only (sessionStorage) and sent as HTTP Basic.
let auth = null, authUser = "", settings = null;
try { auth = sessionStorage.getItem("kbusAuth"); authUser = sessionStorage.getItem("kbusUser") || ""; } catch (e) { /* no storage */ }
function setAuth(a, user) {
  auth = a; authUser = user || "";
  try {
    if (a) { sessionStorage.setItem("kbusAuth", a); sessionStorage.setItem("kbusUser", authUser); }
    else { sessionStorage.removeItem("kbusAuth"); sessionStorage.removeItem("kbusUser"); }
  } catch (e) { /* ignore */ }
  showLogin();
}
function authHeaders(h) { if (auth) h.Authorization = auth; return h; }
class LoginRequired extends Error {}

let loginWaiters = [];
function openLogin(why) {
  $("login-why").textContent = why || "Reset Statistics and the login settings need the login.";
  $("login-err").textContent = "";
  $("login-user").value = authUser || (settings && settings.user) || "admin";
  $("login-pw").value = "";
  $("login-dlg").classList.add("open");
  setTimeout(() => $("login-pw").focus(), 0);
  return new Promise(res => loginWaiters.push(res));
}
function closeLogin(ok) {
  $("login-dlg").classList.remove("open");
  const w = loginWaiters; loginWaiters = []; w.forEach(f => f(ok));
}
async function ensureLogin(why) { return auth ? true : openLogin(why); }

function showLogin() {
  document.body.classList.toggle("logged-in", !!auth);
  led("led-login", auth ? "green" : "yellow", auth ? `Logged in as ${authUser}` : "Not logged in: Reset Statistics is hidden");
  $("login-label").textContent = auth ? "Logout" : "Login";
  $("login-btn").classList.toggle("on", !!auth);
  $("login-btn").title = auth ? `Logged in as ${authUser}: click to log out` : "Log in to show Reset Statistics and the login settings";
}
function loginClick() {
  if (auth) { setAuth(null); msg("Logged out.", true); } else openLogin("Log in to show Reset Statistics and the login settings.");
}

async function loadSettings() {
  const r = await fetch("api/settings", { headers: authHeaders({}) });
  if (!r.ok) return;
  settings = await r.json();
  PREFIX = settings.prefix || "kbus"; READONLY = !!settings.readonly;
  $("sub-list").textContent = `${PREFIX}/#, bridge/status, bridge/+/status`;
  $("slogin").checked = settings.login;
  $("suser").value = settings.user;
  $("spw").value = $("spw2").value = "";
  $("sstate").textContent = settings.login
    ? `The login is on for user "${settings.user}".`
    : "The login is off: everyone on the network can open this website.";
  if (settings.defaultPassword)
    $("sstate").textContent += ` Reset Statistics uses the DEFAULT login (${settings.user} / wago): set a password below.`;
  applyWrite();
  showLogin();
}

// ---- writes: through the mqtt-manager ------------------------------------------------------
let lastWrite = 0;
async function publish(topic, value, what, quiet) {
  if (READONLY) return msg("The manager is read-only (MGMT_READONLY=1).");
  for (let i = 0; i < 2; i++) {
    try {
      const r = await fetch("api/publish", { method: "POST", headers: authHeaders({ "Content-Type": "application/json" }),
        body: JSON.stringify({ topic, value }) });
      const d = await r.json().catch(() => ({}));
      if (r.status === 401 && r.headers.get("X-Login")) { setAuth(null); throw new LoginRequired(); }
      if (!r.ok) throw new Error(d.error || `HTTP ${r.status}`);
      lastWrite = Date.now();
      if (!quiet) msg(`${what || topic}: done.`, true);
      return true;
    } catch (e) {
      if (!(e instanceof LoginRequired)) { msg(e.message); return false; }
      if (!await ensureLogin(`${what || topic} needs the login.`)) { msg("Cancelled: not logged in."); return false; }
    }
  }
  return false;
}
function applyWrite() {
  document.querySelectorAll("button.w").forEach(b => b.disabled = READONLY);
  led("led-wr", READONLY ? "yellow" : "green", READONLY ? "Read-only (MGMT_READONLY=1)" : "Writes allowed");
}

// ---- status area: clock ------------------------------------------------------------------------
async function clock() {
  try {
    const d = await (await fetch("api/info", { headers: authHeaders({}) })).json();
    $("clock").innerHTML = `${esc(d.date)}<br>${esc(d.time)}`;
  } catch (e) { /* keep the last time */ }
}

// ---- start -------------------------------------------------------------------------------------
document.addEventListener("DOMContentLoaded", async () => {
  $("main-menu").innerHTML = MENU.map(m =>
    `<li class="tab-item" data-tab="${m.id}"><a href="#/${m.id}">${esc(m.title)}</a></li>`).join("");
  $("logo-img").addEventListener("error", () => $("logo").classList.add("noimg"));
  $("wbm-link").href = `${location.protocol}//${location.host}/wbm/`;
  window.addEventListener("hashchange", route);
  document.addEventListener("click", e => {
    const t = e.target.closest(".form > span.title");             // collapsible panels
    if (t) return t.parentNode.classList.toggle("collapsed");
    const d = e.target.closest("[data-do]");
    if (d) return publish(`do/${d.dataset.do}/set`, d.dataset.state !== "1", "", true);
    const a = e.target.closest("[data-ao]");
    if (a) {
      const v = Number($("aoi-" + (a.dataset.ao - 1)).value);
      if (!Number.isInteger(v) || v < 0 || v > 65535) return msg("Analog output: whole number 0..65535");
      publish(`ao/${a.dataset.ao}/set`, v, `AO${a.dataset.ao} = ${v}`);
    }
  });
  $("login-btn").addEventListener("click", loginClick);
  $("login-btn").addEventListener("keydown", e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); loginClick(); } });
  $("login-cancel").addEventListener("click", () => closeLogin(false));
  $("login-form").addEventListener("submit", async e => {
    e.preventDefault();
    const user = $("login-user").value.trim(), pw = $("login-pw").value;
    const a = "Basic " + btoa(unescape(encodeURIComponent(user + ":" + pw)));
    const r = await fetch("api/login", { headers: { Authorization: a } }).catch(() => null);
    if (!r || !r.ok) { $("login-err").textContent = r ? "User or password wrong." : "Not reachable."; return; }
    const d = await r.json();
    setAuth(a, user);
    if (d.defaultPassword) msg("You are logged in with the default password: change it on Settings > Login.");
    closeLogin(true);
  });
  $("tfilter").addEventListener("input", () => renderTopics(true));
  $("cycset").addEventListener("click", () => {
    const v = Number($("cyc").value);
    if (!Number.isInteger(v) || v < 1 || v > 40) return msg("Cycle time: whole number 1..40 ms.");
    publish("bus/cycletime/set", v, `Cycle time ${v} ms`).then(ok => { if (ok) $("cyc").value = ""; });
  });
  $("reset").addEventListener("click", () => publish("bus/resetstatistics/set", true, "Reset Statistics"));
  $("rescan").addEventListener("click", () => {
    if (confirm("Rescan the KBUS? All outputs go to 0 until they are written again.")) publish("bus/rescan/set", true, "Rescan KBUS");
  });
  $("sform").addEventListener("submit", async e => {
    e.preventDefault();
    const login = $("slogin").checked, user = $("suser").value.trim(), pw = $("spw").value;
    if (pw !== $("spw2").value) return msg("The two passwords are not the same.");
    if (login && !pw && !(settings && settings.passwordSet)) return msg("Enter a password to switch the login on.");
    const turningOn = login && !(settings && settings.login);
    try {
      if (!await ensureLogin("Changing the login settings needs the login.")) return msg("Cancelled: not logged in.");
      const r = await fetch("api/settings", { method: "POST", headers: authHeaders({ "Content-Type": "application/json" }),
        body: JSON.stringify({ login, user, password: pw || null }) });
      const d = await r.json().catch(() => ({}));
      if (r.status === 401) { setAuth(null); throw new Error("Login refused: log in again."); }
      if (!r.ok) throw new Error(d.error || `HTTP ${r.status}`);
      if (pw || user !== authUser) setAuth(null);          // new credentials: log in again with them
      msg("Settings saved.", true);
      if (turningOn || (login && (pw || user !== settings.user))) {
        $("snote").textContent = "Login active: reloading…";
        setTimeout(() => location.reload(), 1200);
      }
      await loadSettings();
    } catch (err) { msg(err.message); }
  });

  showLogin();
  await loadSettings().catch(() => {});
  clock(); setInterval(clock, 1000);
  route();
  connect();
  setInterval(() => { if (page === "topics") schedule(); }, 1000);
});
