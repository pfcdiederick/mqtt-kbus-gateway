"""mqtt-manager: the KBUS Manager page at http(s)://<pfc>/kbus/ (optional container).

  browser ── http(s)://<pfc>/kbus/   ─▶ WAGO web server (lighttpd) ─▶ mqtt-manager 127.0.0.1:9080 (page + API)
          └─ ws(s)://<pfc>/kbus/ws ─▶ WAGO web server (lighttpd) ─▶ mosquitto  127.0.0.1:9001 (MQTT, read only)

The page has the layout of the controller's Web-based Management (WBM), like the
KBUS Manager of the HTTP/wdx gateway. It READS everything live over MQTT/WebSocket;
every WRITE goes through this program (POST /api/publish), which checks the login
and publishes on the broker (MQTT_HOST:MQTT_PORT). The WebSocket listener of the
broker is read only (docker/mosquitto/acl-monitor).

Jobs:
0. Report the MQTT clients connected to the broker: /clients.json (from mosquitto's log).
1. Serve the page (web/: index.html, kbus.js, kbus.css) and its API on MANAGER_BIND:MANAGER_PORT:
     /api/settings   GET: login settings, topic prefix, read-only; POST JSON {login, user, password} (login)
     /api/login      GET with Authorization: check user + password (login dialog)
     /api/publish    POST JSON {topic, value}: write an output or a bus setting, or send bytes on COM1
     /api/info       date and time of the controller (status bar)
     /wbm-assets/... fonts, logo and favicon of the controller's own WBM (look of the page)
2. Link /kbus in the WAGO web server and keep it linked: write
   /etc/lighttpd/apps.confd/mqtt.conf on the controller, let lighttpd check the whole
   configuration (lighttpd -tt), reload it. Checked again every LINK_CHECK_S seconds, so
   the link comes back by itself (e.g. after a firmware update). A configuration that
   lighttpd rejects is removed again at once, so the WBM is never left broken.
   The old address /mqtt is redirected to /kbus.
   Needs --privileged --pid host: the commands run on the controller through nsenter.

    python mqtt_manager.py            serve + keep the link (the container's default)
    python mqtt_manager.py --unlink   remove /kbus from the web server and exit

Login (same rules as the KBUS Manager of the wdx gateway): the website is open by
default ("Login required" on Settings > Login closes it). Reset Statistics and
changing the login settings ALWAYS need the login: user and password from
MGMT_DATA/settings.json (password only as a PBKDF2 hash), default admin / wago until
a password is set. Mount MGMT_DATA (/home/mqtt-manager) so it survives a new container.

Environment: MANAGER_BIND (127.0.0.1), MANAGER_PORT (9080), MQTT_WS_PORT (9001),
WEB_PATH (/kbus), OLD_WEB_PATH (/mqtt, redirected; "" = off), LINK (1; 0 = only serve),
LINK_CHECK_S (60), MQTT_HOST (127.0.0.1), MQTT_PORT (1883), MQTT_USER / MQTT_PASSWORD
(broker login, if any), MQTT_TOPIC_PREFIX (kbus), MGMT_DATA (/data), MGMT_READONLY (0),
MGMT_DEFAULT_USER / MGMT_DEFAULT_PASSWORD (admin / wago), MGMT_WBM_URL (https://127.0.0.1/wbm).
"""
import argparse
import base64
import hashlib
import hmac
import http.server
import json
import logging
import os
import re
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.request

WEB_DIR = os.environ.get("WEB_DIR", "/app/www")
BIND = os.environ.get("MANAGER_BIND", "127.0.0.1")
PORT = int(os.environ.get("MANAGER_PORT", "9080"))
WS_PORT = int(os.environ.get("MQTT_WS_PORT", "9001"))
WEB_PATH = "/" + os.environ.get("WEB_PATH", "/kbus").strip("/")
OLD_WEB_PATH = os.environ.get("OLD_WEB_PATH", "/mqtt").strip("/")
OLD_WEB_PATH = "/" + OLD_WEB_PATH if OLD_WEB_PATH and "/" + OLD_WEB_PATH != WEB_PATH else ""
LINK = os.environ.get("LINK", "1") == "1"
LINK_CHECK_S = int(os.environ.get("LINK_CHECK_S", "60"))
CONF_DIR = "/etc/lighttpd/apps.confd"
CONF = f"{CONF_DIR}/mqtt.conf"
MOSQUITTO = os.environ.get("MOSQUITTO_CONTAINER", "mosquitto")
MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("MQTT_USER", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC_PREFIX", "kbus").strip("/") or "kbus"
READONLY = os.environ.get("MGMT_READONLY", "0") == "1"
DATA = os.environ.get("MGMT_DATA", "/data")
SETTINGS_FILE = os.path.join(DATA, "settings.json")
DEFAULT_USER = os.environ.get("MGMT_DEFAULT_USER", "admin")
DEFAULT_PASSWORD = os.environ.get("MGMT_DEFAULT_PASSWORD", "wago")

log = logging.getLogger("mqtt-manager")
stop = threading.Event()


# ---------------------------------------------------------------------------
# Login settings (as in the KBUS Manager of the wdx gateway)
# ---------------------------------------------------------------------------
ITERATIONS = 100_000              # PBKDF2; checked once per credential, then cached
USER_RE = re.compile(r"^[A-Za-z0-9_.@-]{1,32}$")


def hash_password(pw, salt=None):
    salt = salt or os.urandom(16)
    dk = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, ITERATIONS)
    return f"pbkdf2_sha256${ITERATIONS}${salt.hex()}${dk.hex()}"


def check_password(pw, stored):
    try:
        _, it, salt, dk = stored.split("$")
        got = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), int(it))
        return hmac.compare_digest(got.hex(), dk)
    except (ValueError, AttributeError):
        return False


class Settings:
    """login on/off, user, password hash; stored in settings.json."""

    def __init__(self):
        self.lock = threading.Lock()
        self.login, self.user, self.pw_hash = False, DEFAULT_USER, ""
        self.ok_cache = set()           # sha256 of accepted Authorization headers
        self.source = "default"
        damaged = False
        try:
            with open(SETTINGS_FILE) as f:
                d = json.load(f)
            self.login, self.user, self.pw_hash = bool(d["login"]), d["user"], d.get("password", "")
            self.source = SETTINGS_FILE
        except FileNotFoundError:
            pass
        except (ValueError, KeyError, TypeError) as e:
            # damaged file: fail closed rather than open
            log.error("%s unreadable (%s): login required, but no password works; fix or delete the file",
                      SETTINGS_FILE, e)
            self.login, self.pw_hash, damaged = True, "", True
            self.source = "damaged settings file"
        if not damaged:
            if self.login and not self.pw_hash:
                self.login = False
            if not self.pw_hash:
                # no password set yet: the default login (Reset Statistics, login settings)
                self.user, self.pw_hash = DEFAULT_USER, hash_password(DEFAULT_PASSWORD)
        self.default_pw = bool(self.pw_hash) and check_password(DEFAULT_PASSWORD, self.pw_hash)

    def public(self):
        return {"login": self.login, "user": self.user, "passwordSet": bool(self.pw_hash),
                "defaultPassword": self.default_pw, "stored": os.path.isfile(SETTINGS_FILE),
                "prefix": TOPIC_PREFIX, "readonly": READONLY}

    def check(self, header):
        """Access to the website: free while the login is off."""
        return not self.login or self.verify(header)

    def verify(self, header):
        """Valid user and password (always checked: protected functions)."""
        if not header.startswith("Basic "):
            return False
        key = hashlib.sha256(header.encode()).digest()
        with self.lock:
            if key in self.ok_cache:
                return True
            user, pw_hash = self.user, self.pw_hash
        try:
            user_in, _, pw = base64.b64decode(header[6:]).decode().partition(":")
        except (ValueError, UnicodeDecodeError):
            return False
        ok = hmac.compare_digest(user_in.encode(), user.encode()) and check_password(pw, pw_hash)
        if ok:
            with self.lock:
                if self.pw_hash == pw_hash:
                    self.ok_cache.add(key)
        return ok

    def update(self, login, user, password):
        """Validate and store; returns an error text or None."""
        if not isinstance(login, bool):
            return "login must be true or false"
        user = (user or "").strip() or self.user
        if not USER_RE.match(user):
            return "user: 1-32 characters A-Z a-z 0-9 _ . @ -"
        if password is not None and not isinstance(password, str):
            return "password must be text"
        if password:
            if len(password) < 4 or len(password) > 128:
                return "password: 4-128 characters"
            new_hash = hash_password(password)
        else:
            new_hash = self.pw_hash
        if login and not new_hash:
            return "enter a password to switch the login on"
        os.makedirs(DATA, exist_ok=True)
        tmp = SETTINGS_FILE + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump({"login": login, "user": user, "password": new_hash}, f)
        os.replace(tmp, SETTINGS_FILE)
        default_pw = check_password(DEFAULT_PASSWORD, new_hash)
        with self.lock:
            self.login, self.user, self.pw_hash = login, user, new_hash
            self.default_pw = default_pw
            self.ok_cache.clear()
            self.source = SETTINGS_FILE
        return None


SETTINGS = None                     # created in main() (after logging is set up)


# ---------------------------------------------------------------------------
# Writes: a minimal MQTT 3.1.1 publisher (QoS 0, not retained), one connection per write
# ---------------------------------------------------------------------------
def _mqtt_str(b: bytes) -> bytes:
    return len(b).to_bytes(2, "big") + b


def _mqtt_packet(head: int, body: bytes) -> bytes:
    n, rl = len(body), bytearray()
    while True:
        b, n = n % 128, n // 128
        rl.append(b | (128 if n else 0))
        if not n:
            return bytes([head]) + bytes(rl) + body


def mqtt_publish(topic: str, payload):
    """Publish one message (payload: str = UTF-8 text, bytes = as is); raises OSError on failure."""
    if isinstance(payload, str):
        payload = payload.encode()
    flags, extra = 0x02, b""                                      # clean session
    if MQTT_USER:
        flags |= 0x80
        extra += _mqtt_str(MQTT_USER.encode())
        if MQTT_PASSWORD:
            flags |= 0x40
            extra += _mqtt_str(MQTT_PASSWORD.encode())
    cid = f"kbus-manager-{os.getpid()}-{threading.get_ident() % 100000}".encode()
    connect = _mqtt_packet(0x10, _mqtt_str(b"MQTT") + bytes([4, flags, 0, 10]) + _mqtt_str(cid) + extra)
    with socket.create_connection((MQTT_HOST, MQTT_PORT), timeout=3) as s:
        s.sendall(connect)
        ack = b""
        while len(ack) < 4:
            chunk = s.recv(4 - len(ack))
            if not chunk:
                raise OSError("broker closed the connection")
            ack += chunk
        if ack[0] != 0x20 or ack[3] != 0:
            raise OSError(f"broker refused the connection (code {ack[3]})")
        s.sendall(_mqtt_packet(0x30, _mqtt_str(topic.encode()) + payload))
        s.sendall(b"\xe0\x00")                                    # DISCONNECT


# topics the page may write (relative to the prefix): (pattern, check of the value, needs the
# login, payload); payload None = the value as JSON (true, 1234), else a function value -> bytes
def _bool(v):
    return v in (True, False)


def _com1_bytes(v):
    """COM1 test send: a string of 1..512 characters U+0000..U+00FF, one character per byte."""
    return isinstance(v, str) and 1 <= len(v) <= 512 and all(ord(c) <= 0xFF for c in v)


WRITABLE = [
    # outputs: only when logged in (the page offers them only under "Take Control")
    (re.compile(r"^do/[1-9][0-9]{0,2}/set$"), _bool, True),
    (re.compile(r"^ao/[1-9][0-9]{0,2}/set$"), lambda v: isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 65535, True),
    (re.compile(r"^bus/cycletime/set$"), lambda v: isinstance(v, int) and not isinstance(v, bool) and 1 <= v <= 40, False),
    (re.compile(r"^bus/rescan/set$"), lambda v: v is True, False),
    (re.compile(r"^bus/resetstatistics/set$"), lambda v: v is True, True),      # needs the login
    # COM1 test send: the bytes as they are (binary), not JSON. The COM1 settings are NOT
    # writable here: they are set by the client (CODESYS).
    (re.compile(r"^serial/com1/tx/set$"), _com1_bytes, False, lambda v: v.encode("latin-1")),
]


# ---------------------------------------------------------------------------
# Look of the WBM: fonts and logo come from the controller's own WBM
# (not part of this project; fetched once, kept in memory; without them the page
# uses system fonts and a text logo)
# ---------------------------------------------------------------------------
WBM_URL = os.environ.get("MGMT_WBM_URL", "https://127.0.0.1/wbm").rstrip("/")   # --network host
WBM_ASSETS = {
    "images/wago-logo.svg": "image/svg+xml",
    "images/favicon.png": "image/png",
    **{f"fonts/open-sans-latin-{w}.woff2": "font/woff2" for w in ("300", "400", "400italic", "600", "700")},
}
_asset_cache = {}                       # path -> (time, bytes or None)


def wbm_asset(path):
    t, data = _asset_cache.get(path, (0, None))
    if data is not None or time.time() - t < 60:          # failures are retried after 60 s
        return data
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE   # the controller's self-signed certificate
    try:
        with urllib.request.urlopen(f"{WBM_URL}/{path}", context=ctx, timeout=3) as r:
            data = r.read(2_000_000) if r.status == 200 else None
    except (OSError, ValueError):
        data = None
    _asset_cache[path] = (time.time(), data)
    return data


# ---------------------------------------------------------------------------
# The page and its API
# ---------------------------------------------------------------------------
class Handler(http.server.SimpleHTTPRequestHandler):
    server_version = "kbus-manager"
    extensions_map = {
        ".html": "text/html; charset=utf-8",
        ".js": "text/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
        ".ico": "image/x-icon",
        "": "application/octet-stream",
    }

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=WEB_DIR, **kw)

    def send(self, status, ctype, body, extra=None, cache=False):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self._cache = cache
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, status, obj, extra=None):
        self.send(status, "application/json", json.dumps(obj).encode(), extra)

    def redirect(self, location):
        self.send(301, "text/plain", b"", {"Location": location})

    def auth_header(self):
        return self.headers.get("Authorization", "")

    def deny(self):
        time.sleep(0.5)                                   # slows down password guessing
        self.send(401, "text/plain", b"login required\n",
                  {"WWW-Authenticate": 'Basic realm="KBUS Manager", charset="UTF-8"'})

    def login_required(self, what):
        """401 WITHOUT WWW-Authenticate: the page shows its own login dialog."""
        time.sleep(0.5)
        self.send_json(401, {"error": f"{what}: log in first"}, {"X-Login": "required"})

    def read_json(self, limit=4096):
        # JSON only: a cross-site form cannot send it without a CORS preflight (CSRF)
        if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
            raise ValueError("Content-Type must be application/json")
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n > limit:
            raise ValueError("too large")
        d = json.loads(self.rfile.read(n) or b"{}")
        if not isinstance(d, dict):
            raise ValueError("JSON object expected")
        return d

    def do_GET(self):
        path = self.path.split("?")[0]
        # http(s)://<pfc>/kbus (no "/"): the web server passes it on unmapped -> redirect to "/",
        # which the web server maps back to /kbus/ (proxy map-urlpath also maps Location)
        if path == WEB_PATH:
            return self.redirect("/" + self.path[len(WEB_PATH):].lstrip("/"))
        # old address /mqtt/...: forwarded unmapped -> /kbus/...
        if OLD_WEB_PATH and (path == OLD_WEB_PATH or path.startswith(OLD_WEB_PATH + "/")):
            return self.redirect(WEB_PATH + "/" + self.path[len(OLD_WEB_PATH):].lstrip("/"))
        if path.startswith("/wbm-assets/"):               # look of the page: free (also on the login prompt)
            name = path[len("/wbm-assets/"):]
            data = wbm_asset(name) if name in WBM_ASSETS else None
            if data is None:
                return self.send(404, "text/plain", b"not found\n")
            return self.send(200, WBM_ASSETS[name], data, cache=True)
        if not SETTINGS.check(self.auth_header()):
            return self.deny()
        if path == "/clients.json":
            return self.send_json(200, clients.snapshot())
        if path == "/api/settings":
            return self.send_json(200, SETTINGS.public())
        if path == "/api/login":                          # check user + password (login dialog)
            if not SETTINGS.verify(self.auth_header()):
                return self.login_required("login")
            return self.send_json(200, {"user": SETTINGS.user, "defaultPassword": SETTINGS.default_pw})
        if path == "/api/info":
            return self.send_json(200, {"date": time.strftime("%d.%m.%Y"), "time": time.strftime("%H:%M:%S")})
        if path.startswith("/api/"):
            return self.send(404, "text/plain", b"not found\n")
        super().do_GET()

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        if not SETTINGS.check(self.auth_header()):
            return self.deny()
        path = self.path.split("?")[0]
        if path == "/api/publish":
            return self.publish()
        if path == "/api/settings":
            return self.change_settings()
        self.send(405, "text/plain", b"use GET\n")

    def publish(self):
        try:
            d = self.read_json()
        except ValueError as e:
            return self.send_json(400, {"error": str(e)})
        topic, value = d.get("topic"), d.get("value")
        if READONLY:
            return self.send_json(403, {"error": "the manager is read-only (MGMT_READONLY=1)"})
        rule = None
        if isinstance(topic, str):
            rule = next((r for r in WRITABLE if r[0].match(topic)), None)
        if not rule:
            return self.send_json(403, {"error": f"topic not writable from this page: {topic}"})
        if not rule[1](value):
            return self.send_json(400, {"error": f"invalid value for {topic}: {json.dumps(value)}"})
        if rule[2] and not SETTINGS.verify(self.auth_header()):
            return self.login_required("writing outputs" if topic[:3] in ("do/", "ao/") else "Reset Statistics")
        full = f"{TOPIC_PREFIX}/{topic}"
        payload = rule[3](value) if len(rule) > 3 else json.dumps(value)
        try:
            mqtt_publish(full, payload)
        except OSError as e:
            return self.send_json(502, {"error": f"broker {MQTT_HOST}:{MQTT_PORT} not reachable: {e}"})
        log.info("%s published %s = %s", self.address_string(), full, json.dumps(value))
        self.send_json(200, {"topic": full, "value": value})

    def change_settings(self):
        if not SETTINGS.verify(self.auth_header()):      # else anyone could set a new password
            return self.login_required("changing the login settings")
        try:
            d = self.read_json()
            err = SETTINGS.update(d.get("login"), d.get("user"), d.get("password"))
        except ValueError as e:
            err = str(e)
        except OSError as e:
            err = f"cannot store the settings: {e}"
        if err:
            return self.send_json(400, {"error": err})
        log.info("settings changed: login %s", "on, user " + SETTINGS.user if SETTINGS.login else "off")
        self.send_json(200, SETTINGS.public())

    def list_directory(self, path):          # no directory listings
        self.send_error(404)
        return None

    def end_headers(self):
        self.send_header("Cache-Control", "max-age=86400" if getattr(self, "_cache", False) else "no-cache")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def log_message(self, fmt, *args):       # quiet: only errors
        if len(args) > 1 and str(args[1]).startswith(("4", "5")):
            log.info("%s %s", self.address_string(), fmt % args)


# ---------------------------------------------------------------------------
# Directly connected MQTT clients (from the connection messages of mosquitto)
# ---------------------------------------------------------------------------
class Clients:
    """Follows 'docker logs -f <mosquitto>' on the controller and keeps the set of
    connected clients: {client id: {address, since}}. mosquitto writes a line for every
    connect and disconnect (connection_messages true). Restarts of mosquitto or of this
    container are handled: the whole log of the running broker is read again."""

    CONNECT = re.compile(r"New client connected from (\S+) as (\S+) \(")
    GONE = re.compile(r"Client (\S+) (?:closed its connection|disconnected|has exceeded timeout"
                      r"|already connected, closing old connection)")
    SOCKET = re.compile(r"Socket error on client (\S+),")
    START = re.compile(r"mosquitto version \S+ (?:starting|running)")

    def __init__(self):
        self.lock = threading.Lock()
        self.connected = {}
        self.ok = False          # False while the log cannot be read

    def snapshot(self):
        with self.lock:
            return {"source": "mosquitto log", "ok": self.ok,
                    "clients": [dict(id=k, **v) for k, v in sorted(self.connected.items())]}

    def line(self, text: str):
        m = self.CONNECT.search(text)
        with self.lock:
            if m:
                addr = m.group(1).rsplit(":", 1)[0]
                self.connected[m.group(2)] = {"address": addr, "since": int(time.time())}
                return
            m = self.GONE.search(text) or self.SOCKET.search(text)
            if m:
                self.connected.pop(m.group(1), None)
            elif self.START.search(text):
                self.connected.clear()

    def follow(self):
        while not stop.is_set():
            with self.lock:
                self.connected.clear()
            try:
                p = subprocess.Popen(["nsenter", "-t", "1", "-m", "-u", "-i", "-n", "-p", "-C", "--",
                                      "docker", "logs", "-f", MOSQUITTO],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.ok = True
                for raw in p.stdout:
                    self.line(raw.decode(errors="replace"))
                    if stop.is_set():
                        break
                p.kill()
            except OSError as e:
                log.error("cannot read the log of %s: %s", MOSQUITTO, e)
            self.ok = False
            stop.wait(5)       # broker container restarted or not there: read again


clients = Clients()


def serve():
    srv = http.server.ThreadingHTTPServer((BIND, PORT), Handler)
    srv.daemon_threads = True
    log.info("KBUS Manager on http://%s:%d/ (%s), writes via %s:%d, prefix %s, %s", BIND, PORT, WEB_DIR,
             MQTT_HOST, MQTT_PORT, TOPIC_PREFIX, "read-only" if READONLY else "writes allowed")
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ---------------------------------------------------------------------------
# Link in the WAGO web server
# ---------------------------------------------------------------------------
def lighttpd_conf() -> bytes:
    p, o = WEB_PATH, OLD_WEB_PATH
    old = f"""    # old address {o}: the mqtt-manager redirects it to {p}/ (forwarded unmapped)
    $HTTP["url"] =~ "^{o}(/|$)" {{
        proxy.server = ( "" => (( "host" => "127.0.0.1", "port" => {PORT} )) )
    }}
""" if o else ""
    return f"""# KBUS Manager (kbus-gateway, MQTT): http(s)://<pfc>{p}/
#   {p}/    -> mqtt-manager 127.0.0.1:{PORT} (the page and its API)
#   {p}/ws  -> mosquitto    127.0.0.1:{WS_PORT} (MQTT over WebSocket, read only)
# Written and kept in place by the container mqtt-manager (docker/mqtt-manager, version 3).
# Remove: docker rm -f mqtt-manager, then
#   docker run --rm --privileged --pid host --entrypoint python mqtt-manager:<arch> /app/mqtt_manager.py --unlink
$SERVER["socket"] =~ default_sockets {{
    # {p} without "/" also goes to the mqtt-manager, which redirects it to {p}/
    # (a url.redirect here would be overridden by the WBM's own redirect rules)
    $HTTP["url"] =~ "^{p}(/|$)" {{
        proxy.server = ( "" => (( "host" => "127.0.0.1", "port" => {PORT} )) )
        proxy.header = ( "map-urlpath" => ( "{p}/" => "/" ) )
    }}
    $HTTP["url"] == "{p}/ws" {{
        proxy.server = ( "" => (( "host" => "127.0.0.1", "port" => {WS_PORT} )) )
        proxy.header = ( "map-urlpath" => ( "{p}/ws" => "/ws" ), "upgrade" => "enable" )
    }}
{old}}}
""".encode()


def on_host(cmd, data: bytes = None) -> subprocess.CompletedProcess:
    """Run a command on the controller (its mount/pid/... namespaces)."""
    return subprocess.run(["nsenter", "-t", "1", "-m", "-u", "-i", "-n", "-p", "-C", "--"] + cmd,
                          input=data, capture_output=True, timeout=60)


def reload_webserver() -> bool:
    # The reload stops and STARTS lighttpd. Started from this container, the new lighttpd
    # would belong to the container's cgroups, and Docker kills everything in them when the
    # container is removed or replaced: the WBM would go down. So the shell first moves
    # itself into the root cgroups of the controller, then runs the reload.
    r = on_host(["sh", "-c",
                 'for f in /sys/fs/cgroup/*/cgroup.procs /sys/fs/cgroup/cgroup.procs; do '
                 '[ -w "$f" ] && echo $$ > "$f" 2>/dev/null; done; '
                 'exec /etc/init.d/lighttpd reload'])
    if r.returncode != 0:
        log.error("web server reload failed: %s", (r.stdout + r.stderr).decode(errors="replace").strip())
        return False
    # check: lighttpd must run outside the container
    c = on_host(["sh", "-c", 'p=$(pidof lighttpd | cut -d" " -f1); [ -n "$p" ] && cat /proc/$p/cgroup'])
    cg = c.stdout.decode(errors="replace")
    if c.returncode != 0:
        log.error("lighttpd is not running after the reload")
        return False
    if "docker" in cg:
        log.error("lighttpd runs in a container cgroup; restart it on the controller: /etc/init.d/lighttpd restart")
    return True


def install_link(conf: bytes) -> bool:
    """Write the configuration, let lighttpd check it, reload. False = rejected (nothing changed)."""
    on_host(["sh", "-c", f"cat > {CONF}.new && chmod 600 {CONF}.new"], conf)
    test = on_host(["sh", "-c", f"cp -p {CONF} {CONF}.old 2>/dev/null; mv {CONF}.new {CONF} && "
                                f"cd /etc/lighttpd && lighttpd -tt -f /etc/lighttpd/lighttpd.conf"])
    if test.returncode != 0:
        # never leave a configuration behind that the web server (WBM) cannot load
        on_host(["sh", "-c", f"if [ -e {CONF}.old ]; then mv {CONF}.old {CONF}; else rm -f {CONF}; fi"])
        log.error("lighttpd rejected the configuration, %s not linked: %s", WEB_PATH,
                  (test.stdout + test.stderr).decode(errors="replace").strip())
        return False
    on_host(["rm", "-f", f"{CONF}.old"])
    return reload_webserver()


def keep_linked():
    conf = lighttpd_conf()
    digest = hashlib.sha256(conf).hexdigest()
    failed = None                             # digest of a configuration that was rejected
    warned = False
    while not stop.is_set():
        try:
            if on_host(["test", "-d", CONF_DIR]).returncode != 0:
                if not warned:
                    log.error("%s not found on this controller: %s cannot be linked", CONF_DIR, WEB_PATH)
                warned = True
            elif on_host(["cat", CONF]).stdout != conf and failed != digest:
                log.info("linking %s in the WAGO web server (%s)", WEB_PATH, CONF)
                if install_link(conf):
                    log.info("%s linked: https://<pfc>%s/", WEB_PATH, WEB_PATH)
                    failed = None
                else:
                    failed = digest
        except (OSError, subprocess.SubprocessError) as e:
            log.error("cannot check the link (container needs --privileged --pid host): %s", e)
        stop.wait(LINK_CHECK_S)


def unlink() -> int:
    if on_host(["test", "-e", CONF]).returncode != 0:
        print(f"{CONF} not present: nothing to do")
        return 0
    on_host(["rm", "-f", CONF])
    ok = reload_webserver()
    print(f"{WEB_PATH} removed from the WAGO web server" + ("" if ok else " (reload FAILED)"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="KBUS Manager page + link in the WAGO web server")
    ap.add_argument("--unlink", action="store_true", help="remove the link from the web server and exit")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if args.unlink:
        sys.exit(unlink())
    global SETTINGS
    SETTINGS = Settings()
    log.info("login: %s (from %s)%s", "on, user " + SETTINGS.user if SETTINGS.login else "off", SETTINGS.source,
             ", DEFAULT password (change it on Settings > Login)" if SETTINGS.default_pw else "")

    def on_signal(*_):
        stop.set()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    srv = serve()
    if LINK:
        threading.Thread(target=keep_linked, daemon=True).start()
    else:
        log.info("LINK=0: %s is not linked in the WAGO web server", WEB_PATH)
    threading.Thread(target=clients.follow, daemon=True).start()
    stop.wait()
    log.info("stopping (the link stays; remove it with --unlink)")
    srv.shutdown()


if __name__ == "__main__":
    main()
