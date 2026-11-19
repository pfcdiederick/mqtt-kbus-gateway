// MQTT Monitor: live view of the kbus-gateway and the mqtt-bridge, read from the
// local broker of the controller only (kbus/# and bridge status), with an
// optional control mode (settings menu) to switch the outputs.
//
// A minimal MQTT 3.1.1 client over WebSocket (QoS 0, no library, works without
// internet). It reads everything; it only publishes the bus settings (cycle time,
// rescan, reset statistics). The broker's ACL refuses anything else from the page. The page connects to the WebSocket next to itself:
//   https://<pfc>/mqtt/  ->  wss://<pfc>/mqtt/ws   (lighttpd proxies both to mosquitto)
// URL options: ?prefix=kbus (topic prefix of kbus-gateway), ?ws=<websocket URL>.
"use strict";

const params = new URLSearchParams(location.search);
const PREFIX = (params.get("prefix") || "kbus").replace(/\/+$/, "");
const WS_URL = params.get("ws") ||
  (location.protocol === "https:" ? "wss://" : "ws://") + location.host +
  location.pathname.replace(/[^/]*$/, "") + "ws";
const CLASSES = ["di", "do", "ai", "ao"];

const $ = (id) => document.getElementById(id);
const enc = new TextEncoder();
const dec = new TextDecoder();

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const io = { di: [], do: [], ai: [], ao: [] };      // channel values, index = n-1
const bus = {};                                      // bus/<name>
let modules = [];
let kbusStatus = null;
const bridges = {};                                  // <name> -> online|offline, "" = the bridge itself
let layoutKey = "";
let dirty = false;

function onMessage(topic, payload) {
  const text = dec.decode(payload);
  const deleted = payload.length === 0;              // empty retained message = topic removed
  if (topic === PREFIX + "/status") {
    kbusStatus = deleted ? null : text;
  } else if (topic.startsWith(PREFIX + "/")) {
    const rest = topic.slice(PREFIX.length + 1).split("/");
    if (CLASSES.includes(rest[0]) && rest.length === 2 && /^\d+$/.test(rest[1])) {
      const i = Number(rest[1]) - 1;
      if (deleted) io[rest[0]].length = Math.min(io[rest[0]].length, i);
      else io[rest[0]][i] = rest[0][0] === "d" ? text === "true" : Number(text);
    } else if (rest[0] === "bus" && rest.length === 2) {
      bus[rest[1]] = deleted ? undefined : text;
    } else if (rest[0] === "modules" && rest.length === 1) {
      try { modules = deleted ? [] : JSON.parse(text); } catch (e) { modules = []; }
    }
    else if (rest[0] === "error" && rest.length === 1 && !deleted) {
      try {
        const e = JSON.parse(text);
        if (e.topic && (e.topic.includes("/bus/") || (control && /\/(do|ao)\//.test(e.topic))))
          msg("Rejected " + e.topic.slice(PREFIX.length + 1) + ": " + e.error, true);
      }
      catch (e) { /* not JSON */ }
    }
    // <class>/all and .../set are not shown
  } else if (topic === "bridge/status") {
    bridges[""] = deleted ? undefined : text;
  } else if (/^bridge\/[^/]+\/status$/.test(topic)) {
    bridges[topic.split("/")[1]] = deleted ? undefined : text;
  }
  if (!dirty) { dirty = true; requestAnimationFrame(render); }
}

// ---------------------------------------------------------------------------
// Rendering (same layout as the web page of the wdx gateway)
// ---------------------------------------------------------------------------
const KIND = {
  di: { name: "DI", digital: true },  do: { name: "DO", digital: true },
  ai: { name: "AI", digital: false }, ao: { name: "AO", digital: false },
};

function msg(text, err) { const m = $("msg"); m.textContent = text || ""; m.className = err ? "err" : ""; }

function channelCount(cls) {
  // from the module list (exact), else from the topics seen
  let n = 0;
  for (const m of modules) n += (m.channels && m.channels[cls] ? m.channels[cls].length : 0);
  return n || io[cls].length;
}

function moduleOf(cls, n) {
  for (const m of modules) if (m.channels && m.channels[cls] && m.channels[cls].includes(n)) return m.position;
  return null;
}

function buildChannels() {
  for (const cls of CLASSES) {
    const k = KIND[cls], n = channelCount(cls), box = $(cls);
    $("sec-" + cls).classList.toggle("hidden", n === 0);
    box.textContent = "";
    for (let i = 1; i <= n; i++) {
      const el = document.createElement("div");
      el.className = "ch";
      const pos = moduleOf(cls, i);
      const mod = pos ? '<div class="m">module ' + pos + "</div>" : "";
      el.innerHTML = k.digital
        ? '<div class="n">' + k.name + i + '</div><div class="v">–</div>' + mod
        : '<div><span class="n">' + k.name + i + '</span> <span class="v">–</span></div>' +
          '<div class="bar"><i></i></div>' + mod +
          (cls === "ao" ? '<div class="aoset"><input type="number" min="0" max="65535" step="1" aria-label="AO' + i +
                          '"><button type="button">Set</button></div>' : "");
      if (cls === "do") el.addEventListener("click", () => switchDO(i, el));
      if (cls === "ao") {
        const inp = el.querySelector("input");
        el.querySelector("button").addEventListener("click", () => setAO(i, inp.value));
        inp.addEventListener("keydown", (e) => { if (e.key === "Enter") setAO(i, inp.value); });
      }
      box.appendChild(el);
    }
  }
  const tb = $("mods");
  tb.textContent = "";
  if (!modules.length) tb.innerHTML = '<tr><td colspan="4">no modules</td></tr>';
  for (const m of modules) {
    const ch = Object.entries(m.channels || {})
      .map(([c, l]) => c.toUpperCase() + l[0] + (l.length > 1 ? "–" + c.toUpperCase() + l[l.length - 1] : ""))
      .join(", ");
    const tr = document.createElement("tr");
    for (const v of [m.position, m.name, m.typeCode, ch || "no channels"]) {
      const td = document.createElement("td"); td.textContent = v; tr.appendChild(td);
    }
    tb.appendChild(tr);
  }
}

function render() {
  dirty = false;
  // header
  const online = kbusStatus === "online";
  document.body.classList.toggle("stale", !online);
  const h = bus.healthy;
  $("healthy").textContent = !online ? (kbusStatus ? "kbus-gateway " + kbusStatus : "–")
                                     : h === "true" ? "running" : h === "false" ? "ERROR" : "–";
  let st = {};
  try { st = bus.statistics ? JSON.parse(bus.statistics) : {}; } catch (e) { st = {}; }
  $("cycletime").textContent = bus.cycletime || "–";
  const ct = $("ct");
  if (bus.cycletime && document.activeElement !== ct && ct.dataset.v !== bus.cycletime) {
    ct.value = bus.cycletime; ct.dataset.v = bus.cycletime;
  }
  for (const id of ["ct-set", "reset", "rescan", "control"]) $(id).disabled = !connected || !online;
  if (control && (!connected || !online)) setControl(false);
  $("cycles").textContent = st.cycles != null ? st.cycles : "–";
  $("errors").textContent = st.errors != null ? st.errors : "–";
  $("config").textContent = bus.configversion || "–";
  $("t-last").textContent = st.cycleDuration != null ? st.cycleDuration + " µs" : "–";
  $("t-max").textContent = st.maxCycleDuration != null ? st.maxCycleDuration + " µs" : "–";
  $("t-wake").textContent = st.maxWakeupDelay != null ? st.maxWakeupDelay + " µs" : "–";
  $("t-overruns").textContent = st.overruns != null ? st.overruns : "–";
  $("t-rt").textContent = st.realtime == null ? "–" : st.realtime ? "yes" : "no";
  $("t-version").textContent = st.version ? st.version + (st.backend === "simulation" ? " (simulation)" : "") : "–";
  if (kbusStatus === "offline") msg("kbus-gateway is offline: the values below are the last ones it published.", true);
  else if (st.backend === "simulation") msg("SIMULATION: the kbus-gateway runs without the real KBUS.", true);
  else if (connected) msg("");

  // channels: rebuild when the node changed
  const key = CLASSES.map(channelCount).join(",") + "|" + bus.configversion + "|" + modules.length;
  if (key !== layoutKey) { layoutKey = key; buildChannels(); }
  for (const cls of CLASSES) {
    const els = $(cls).children;
    for (let i = 0; i < els.length; i++) {
      const v = io[cls][i], el = els[i];
      if (KIND[cls].digital) {
        el.classList.toggle("on", v === true);
        el.querySelector(".v").textContent = v === undefined ? "–" : v ? "TRUE" : "FALSE";
      } else {
        el.querySelector(".v").textContent = v === undefined ? "–" : v;
        el.querySelector(".bar i").style.width = v === undefined ? "0" : (v / 655.35).toFixed(1) + "%";
      }
    }
  }

  // MQTT clients connected directly to this broker (from the mqtt-manager: mosquitto's log)
  const gb = $("connected");
  gb.textContent = "";
  const tile = (name, state, detail) => {
    const el = document.createElement("div");
    el.className = "ch " + (state === "online" ? "on" : state ? "bad" : "");
    el.innerHTML = '<div class="n"></div><div class="v"></div><div class="m"></div>';
    el.querySelector(".n").textContent = name;
    el.querySelector(".v").textContent = state || "not seen";
    el.querySelector(".m").textContent = detail;
    gb.appendChild(el);
  };
  const kbusDetail = online ? "KBUS " + (h === "true" ? "running" : h === "false" ? "ERROR" : "–") +
                              (bus.cycletime ? " · " + bus.cycletime + " ms" : "") : "KBUS ⇄ MQTT";
  const local = (a) => a === "127.0.0.1" || a === "::1" || a === location.hostname;
  if (clientList) {
    const list = clientList.filter((c) => !c.id.startsWith("kbus-monitor-"));   // this page itself
    const gw = list.filter((c) => c.id.startsWith("kbus-gateway"));
    const br = list.filter((c) => c.id.startsWith("mqtt-bridge"));
    if (gw.length) for (const c of gw) tile("kbus-gateway", "online", kbusDetail);
    else tile("kbus-gateway", kbusStatus ? "offline" : "", "not connected");
    for (const c of br) tile("mqtt-bridge", "online", c.id);
    if (!br.length && bridges[""] !== undefined) tile("mqtt-bridge", "offline", "not connected");
    for (const c of list) {
      if (gw.includes(c) || br.includes(c)) continue;
      if (local(c.address)) tile("CODESYS", "online", c.id);
      else tile("MQTT client", "online", c.id + " · " + c.address);
    }
  } else {
    // the mqtt-manager cannot tell: what the components report themselves
    tile("kbus-gateway", kbusStatus, kbusDetail);
    if (bridges[""] !== undefined) tile("mqtt-bridge", bridges[""], "");
  }
}

// connected clients, from the mqtt-manager (every 5 s)
let clientList = null;
async function pollClients() {
  try {
    const r = await fetch("clients.json", { cache: "no-store" });
    const j = r.ok ? await r.json() : null;
    clientList = j && j.ok ? j.clients : null;
  } catch (e) { clientList = null; }
  if (!dirty) { dirty = true; requestAnimationFrame(render); }
  setTimeout(pollClients, 5000);
}

// ---------------------------------------------------------------------------
// MQTT 3.1.1 over WebSocket (subscribe only)
// ---------------------------------------------------------------------------
function varint(n) {
  const out = [];
  do { let b = n % 128; n = Math.floor(n / 128); if (n > 0) b |= 128; out.push(b); } while (n > 0);
  return out;
}
function str(s) { const b = enc.encode(s); return [b.length >> 8, b.length & 255, ...b]; }
function packet(type, body) { return new Uint8Array([type, ...varint(body.length), ...body]); }

let ws = null, rx = new Uint8Array(0), pingTimer = null, retry = 1000, connected = false;

function conn(state, text) { $("dot").className = "dot" + (state ? " " + state : ""); $("conn").textContent = text; }

function connect() {
  conn("", "connecting…");
  try { ws = new WebSocket(WS_URL, ["mqtt"]); } catch (e) { scheduleReconnect(); return; }
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    const id = "kbus-monitor-" + Math.random().toString(16).slice(2, 10);
    ws.send(packet(0x10, [...str("MQTT"), 4, 0x02, 0, 30, ...str(id)]));   // clean session, keepalive 30 s
  };
  ws.onmessage = (ev) => {
    const d = new Uint8Array(ev.data);
    const n = new Uint8Array(rx.length + d.length);
    n.set(rx); n.set(d, rx.length); rx = n;
    parse();
  };
  ws.onclose = () => {
    clearInterval(pingTimer);
    if (connected) msg("Connection to the broker lost, reconnecting …", true);
    connected = false;
    conn("bad", "offline");
    document.body.classList.add("stale");
    scheduleReconnect();
  };
  ws.onerror = () => { try { ws.close(); } catch (e) { /* closed */ } };
}

function scheduleReconnect() {
  setTimeout(connect, retry);
  retry = Math.min(retry * 2, 15000);
}

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
      if (body[1] !== 0) { conn("bad", "refused (" + body[1] + ")"); ws.close(); return; }
      conn("ok", "online");
      connected = true;
      msg("");
      retry = 1000;
      // only what the PFC's own components publish: kbus-gateway and the status of the mqtt-bridge
      ws.send(packet(0x82, [0, 1, ...str(PREFIX + "/#"), 0, ...str("bridge/status"), 0]));
      pingTimer = setInterval(() => ws.readyState === 1 && ws.send(new Uint8Array([0xc0, 0])), 20000);
    } else if (type === 3) {                             // PUBLISH
      const tl = (body[0] << 8) | body[1];
      const topic = dec.decode(body.subarray(2, 2 + tl));
      const qos = (flags >> 1) & 3;
      const off = 2 + tl + (qos ? 2 : 0);
      onMessage(topic, body.slice(off));
    }
    rx = rx.slice(i + len);
  }
}

// ---------------------------------------------------------------------------
// Bus settings (the only writes of this page)
// ---------------------------------------------------------------------------
function publish(topic, text) {
  if (!connected || !ws || ws.readyState !== 1) { msg("Not connected to the broker.", true); return false; }
  ws.send(packet(0x30, [...str(topic), ...enc.encode(text)]));      // QoS 0, not retained
  return true;
}

function setCycle() {
  const v = Number($("ct").value);
  if (!Number.isInteger(v) || v < 1 || v > 40) { msg("Cycle time: enter an integer 1…40 ms", true); return; }
  if (publish(PREFIX + "/bus/cycletime/set", String(v))) { msg("Cycle time set to " + v + " ms."); menuOpen(false); }
}

// ---------------------------------------------------------------------------
// Control mode: switch the outputs from the page (off by default, 10 min time-out)
// ---------------------------------------------------------------------------
const CONTROL_TIMEOUT_MS = 10 * 60 * 1000;
let control = false, controlTimer = null;

function setControl(on) {
  control = on;
  const b = $("control");
  b.classList.toggle("active", on);
  b.setAttribute("aria-checked", String(on));
  document.body.classList.toggle("control", on);
  $("ctlbar").classList.toggle("hidden", !on);
  clearTimeout(controlTimer);
  if (on) controlTimer = setTimeout(() => { setControl(false); msg("Control mode switched off (10 minutes without use)."); },
                                    CONTROL_TIMEOUT_MS);
}

function touchControl() { if (control) setControl(true); }    // restart the time-out

function switchDO(n, el) {
  if (!control) return;
  const v = el.classList.contains("on") ? "false" : "true";
  if (publish(PREFIX + "/do/" + n + "/set", v)) { msg("DO" + n + " → " + v.toUpperCase()); touchControl(); }
}

function setAO(n, text) {
  if (!control) return;
  const v = Number(text);
  if (!Number.isInteger(v) || v < 0 || v > 65535) { msg("AO" + n + ": enter an integer 0…65535", true); return; }
  if (publish(PREFIX + "/ao/" + n + "/set", String(v))) { msg("AO" + n + " → " + v); touchControl(); }
}

function menuOpen(open) {
  $("menu").classList.toggle("hidden", !open);
  $("gear").setAttribute("aria-expanded", String(open));
  if (open) { const ct = $("ct"); ct.dataset.v = ""; render(); ct.focus(); ct.select(); }
}

document.addEventListener("DOMContentLoaded", () => {
  $("gear").addEventListener("click", (e) => { e.stopPropagation(); menuOpen($("menu").classList.contains("hidden")); });
  $("menu").addEventListener("click", (e) => e.stopPropagation());
  document.addEventListener("click", () => menuOpen(false));
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") menuOpen(false); });
  $("ct-set").addEventListener("click", setCycle);
  $("control").addEventListener("click", () => {
    setControl(!control);
    msg(control ? "" : "Control mode off.");
    menuOpen(false);
  });
  $("ctl-off").addEventListener("click", () => { setControl(false); msg("Control mode off."); });
  $("ct").addEventListener("keydown", (e) => { if (e.key === "Enter") setCycle(); });
  $("reset").addEventListener("click", () => {
    if (publish(PREFIX + "/bus/resetstatistics/set", "true")) { msg("Statistics reset."); menuOpen(false); }
  });
  $("rescan").addEventListener("click", () => {
    if (!confirm("Rescan the KBUS? All outputs go to 0 until they are written again.")) return;
    if (publish(PREFIX + "/bus/rescan/set", "true")) { msg("Rescanning the KBUS …"); menuOpen(false); }
  });
  $("host").textContent = location.hostname;
  $("prefix").textContent = PREFIX;
  render();
  connect();
  pollClients();
});
