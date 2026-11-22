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

let PREFIX = "kbus", READONLY = false;

// ---- navigation: main menu (tabs) and sub menu, like the WBM ---------------
const MENU = [
  { id: "status", title: "Status", pages: [["serial", "COM1"], ["kbus", "KBUS"], ["modules", "Modules"]] },
  { id: "mqtt", title: "MQTT", pages: [["topics", "Topics"], ["clients", "Clients"]] },
  { id: "settings", title: "Settings", pages: [["cycle", "KBUS"], ["login", "Login"]] },
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
const bus = {};                                      // <prefix>/bus/<name>
let stats = {}, modules = [], gwStatus = null;
const bridges = {};                                  // <name> -> online|offline, "" = the bridge itself
const ser = {};                                      // <prefix>/serial/com1/<name> (open, baudrate, ...)
let serStats = {}, serRx = [];                       // statistics; last received frames [{time, bytes}]
const topics = new Map();                            // topic -> {text, time}
let layoutKey = "", dirty = false, topicsDirty = false;

function payloadText(p) {
  try { return dec.decode(p); }
  catch (e) { return [...p].map(b => b.toString(16).padStart(2, "0")).join(" "); }   // binary (750-495 ...)
}

function onMessage(topic, payload) {
  const deleted = payload.length === 0;              // empty retained message = topic removed
  const text = payloadText(payload);
  if (topic.endsWith("/set")) return;                  // a command (write), not a state: not listed
  if (deleted) topics.delete(topic); else topics.set(topic, { text, time: Date.now() });
  topicsDirty = true;
  if (topic === PREFIX + "/status") {
    gwStatus = deleted ? null : text;
  } else if (topic.startsWith(PREFIX + "/")) {
    const rest = topic.slice(PREFIX.length + 1).split("/");
    if (rest[0] === "watchdog" && rest.length === 1) {
      bus.watchdog = deleted ? undefined : text;
    } else if (rest[0] === "bus" && rest.length === 2) {
      bus[rest[1]] = deleted ? undefined : text;
      if (rest[1] === "statistics") { try { stats = deleted ? {} : JSON.parse(text); } catch (e) { stats = {}; } }
    } else if (rest[0] === "serial" && rest[1] === "com1" && rest.length === 3) {
      if (rest[2] === "rx") {
        if (!deleted) { serRx.unshift({ time: Date.now(), bytes: payload }); serRx.length = Math.min(serRx.length, 10); }
      } else {
        ser[rest[2]] = deleted ? undefined : text;
        if (rest[2] === "statistics") { try { serStats = deleted ? {} : JSON.parse(text); } catch (e) { serStats = {}; } }
      }
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

  const key = bus.configversion + "|" + JSON.stringify(modules);
  if (key !== layoutKey) { layoutKey = key; renderModules(); }
  const wd = $("st-watchdog"), w = bus.watchdog;
  wd.className = "control " + (w === "ok" ? "ok-text" : w === "expired" ? "bad-text" : "");
  wd.textContent = w === undefined ? "off" : w === "ok" ? "ok" : "expired: no write for MQTT_WATCHDOG_S, outputs are 0";
  renderSerial();
  if (page === "topics" && topicsDirty) renderTopics(false);
}

// ---- COM1 ----------------------------------------------------------------------------
// bytes -> readable: printable ASCII as is, every other byte as <XX>
const showBytes = b => [...b].map(x => x >= 0x20 && x < 0x7f ? String.fromCharCode(x)
  : "<" + x.toString(16).toUpperCase().padStart(2, "0") + ">").join("");
function renderSerial() {
  const on = ser.open !== undefined;
  $("ser-off").style.display = on ? "none" : "";
  $("ser-form").style.display = $("ser-mon").style.display = on ? "" : "none";
  if (!on) return;
  const open = ser.open === "true";
  $("ser-open").className = "control " + (open ? "ok-text" : "bad-text");
  $("ser-open").textContent = open ? "open" : "not open (device missing, or used by another program)";
  const f = ser.format || "", parity = { N: "none", E: "even", O: "odd" }[f[1]] || "?";
  $("ser-baud").textContent = ser.baudrate ? ser.baudrate + " baud" : "–";
  $("ser-format").textContent = f ? `${f}  (${f[0]} data bits, parity ${parity}, ${f[2]} stop bit${f[2] === "2" ? "s" : ""})` : "–";
  $("ser-gap").textContent = ser.framegap ? ser.framegap + " ms" : "–";
  const s = serStats;
  $("ser-counts").textContent = s.txBytes != null ? `${s.txBytes} / ${s.rxBytes} bytes` : "–";
  $("ser-frames").textContent = s.frames != null ? `${s.frames} / ${s.dropped} / ${s.errors}` : "–";
  $("ser-rx").innerHTML = serRx.map(r => `<tr><td>${esc(new Date(r.time).toLocaleTimeString())}</td><td>${r.bytes.length}</td>`
    + `<td class="mono bytes">${esc(showBytes(r.bytes))}</td></tr>`).join("")
    || `<tr><td colspan="3">Nothing received since the page was opened.</td></tr>`;
}

function renderModules() {
  $("modules").innerHTML = modules.map(m => {
    const ch = Object.entries(m.channels || {})
      .map(([c, l]) => c.toUpperCase() + l[0] + (l.length > 1 ? "–" + c.toUpperCase() + l[l.length - 1] : "")).join(", ");
    return `<tr><td>${esc(m.position)}</td><td>${esc(m.name)}</td><td>${esc(m.typeCode)}</td><td>${esc(ch || "no channels")}</td></tr>`;
  }).join("") || `<tr><td colspan="4">${gwStatus ? "No modules on the KBUS." : "Waiting for kbus-gateway …"}</td></tr>`;
}
function renderTopics(full) {
  topicsDirty = false;
  const f = $("tfilter").value.trim().toLowerCase();
  const rows = [...topics.keys()].sort().filter(t => !f || t.toLowerCase().includes(f));
  $("tcount").textContent = `${rows.length} of ${topics.size} topics`;
  const tb = $("ttable");
  const existing = new Map([...tb.rows].map(r => [r.dataset.t, r]));
  if (full || existing.size !== rows.length || rows.some((t, i) => tb.rows[i].dataset.t !== t)) {
    tb.innerHTML = rows.map(t => {
      const w = control && output(t);
      return `<tr data-t="${esc(t)}"><td class="mono">${esc(t)}</td>`
        + `<td class="val mono${w ? " edit" : ""}"${w ? ` contenteditable="plaintext-only" spellcheck="false" title="type a new value, Enter = write, Esc = cancel"` : ""}></td><td></td></tr>`;
    }).join("");
  }
  for (const r of tb.rows) {
    const e = topics.get(r.dataset.t);
    if (!e) continue;
    const v = r.cells[1], when = new Date(e.time).toLocaleTimeString();
    if (v.textContent !== e.text && document.activeElement !== v) v.textContent = e.text;   // not while typing
    r.cells[2].textContent = when;
  }
}

// ---- take control: the output values in the topics table become editable --------------------
// Only when logged in (the mqtt-manager also requires the login for do/ao writes) and after
// "Take Control"; ends on logout. kbus/do/<n> -> do/<n>/set, kbus/ao/<n> -> ao/<n>/set.
let control = false;
const forced = new Map();                              // "do/1" -> written under control (reset on release)
function output(t) {
  const m = t.startsWith(PREFIX + "/") && t.slice(PREFIX.length + 1).match(/^(do|ao)\/(\d+)$/);
  return m ? { cls: m[1], n: Number(m[2]) } : null;
}
async function setControl(on) {
  if (!on && forced.size) await releaseOutputs();
  control = on && !!auth && !READONLY;
  $("takectl").textContent = control ? "Release Control" : "Take Control";
  $("takectl").classList.toggle("action", control);
  $("ctlnote").textContent = control ? "Click a value of kbus/do/… or kbus/ao/… to change it." : "";
  if (page === "topics") renderTopics(true);
}
function writeValue(cell) {
  const t = cell.parentNode.dataset.t, o = output(t), s = cell.textContent.trim().toLowerCase();
  let v = null;
  if (o.cls === "do") v = ["true", "on", "1"].includes(s) ? true : ["false", "off", "0"].includes(s) ? false : null;
  else if (/^\d+$/.test(s) && Number(s) <= 65535) v = Number(s);
  cell.textContent = topics.get(t) ? topics.get(t).text : "";
  if (v === null) return msg(o.cls === "do" ? "DO: true or false." : "AO: whole number 0..65535.");
  publish(`${o.cls}/${o.n}/set`, v, `${o.cls.toUpperCase()}${o.n} = ${v}`)
    .then(ok => { if (ok) forced.set(`${o.cls}/${o.n}`, o.cls); });
}

// Release: every output written under control goes back to false / 0 (temporary control).
async function releaseOutputs() {
  const list = [...forced.entries()];
  forced.clear();
  for (const [k, cls] of list) await publish(`${k}/set`, cls === "do" ? false : 0, "", true);
  if (list.length) msg(`Control released: ${list.map(([k]) => k.replace("/", "").toUpperCase()).join(", ")} reset.`, true);
}
// Closing the page under control: reset as well (best effort, the browser may not wait).
window.addEventListener("pagehide", () => {
  if (!forced.size || !auth) return;
  for (const [k, cls] of forced)
    fetch("api/publish", { method: "POST", keepalive: true, headers: authHeaders({ "Content-Type": "application/json" }),
      body: JSON.stringify({ topic: `${k}/set`, value: cls === "do" ? false : 0 }) });
  forced.clear();
});

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
  if (!a && control) { setControl(false).then(() => setAuth(null)); return; }   // reset the outputs first, then log out
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
  $("takectl").addEventListener("click", async () => {
    if (!control && !await ensureLogin("Taking control of the outputs needs the login.")) return;
    await setControl(!control);
  });
  $("ttable").addEventListener("keydown", e => {
    const c = e.target.closest("td.edit");
    if (!c) return;
    if (e.key === "Enter") { e.preventDefault(); writeValue(c); c.blur(); }
    else if (e.key === "Escape") { const d = topics.get(c.parentNode.dataset.t); c.textContent = d ? d.text : ""; c.blur(); }
  });
  $("ttable").addEventListener("focusout", e => {     // leaving the cell without Enter: back to the value
    const c = e.target.closest("td.edit"), d = c && topics.get(c.parentNode.dataset.t);
    if (d) c.textContent = d.text;
  });
  $("cycset").addEventListener("click", () => {
    const v = Number($("cyc").value);
    if (!Number.isInteger(v) || v < 1 || v > 40) return msg("Cycle time: whole number 1..40 ms.");
    publish("bus/cycletime/set", v, `Cycle time ${v} ms`).then(ok => { if (ok) $("cyc").value = ""; });
  });
  $("reset").addEventListener("click", () => publish("bus/resetstatistics/set", true, "Reset Statistics"));
  $("ser-send").addEventListener("click", () => {
    const t = $("ser-tx").value.replace(/\\r/g, "\r").replace(/\\n/g, "\n")
      .replace(/\\x([0-9a-fA-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16)));
    if (!t) return msg("COM1: enter something to send.");
    if (/[^\x00-\xff]/.test(t)) return msg("COM1: only characters U+0000..U+00FF (one byte each); use \\xNN for other bytes.");
    if (t.length > 512) return msg("COM1: at most 512 bytes.");
    publish("serial/com1/tx/set", t, `COM1: ${t.length} bytes sent`);
  });
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
