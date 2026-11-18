"""mqtt-manager: the MQTT monitor page at https://<pfc>/mqtt/ (optional container).

  browser ── https://<pfc>/mqtt/    ─▶ WAGO web server (lighttpd) ─▶ mqtt-manager 127.0.0.1:9080 (the page)
          └─ wss://<pfc>/mqtt/ws ─▶ WAGO web server (lighttpd) ─▶ mosquitto  127.0.0.1:9001 (MQTT, read only)

Three jobs:
0. Report the MQTT clients connected to the broker: /clients.json (from mosquitto's log).
1. Serve the page (web/: index.html, monitor.js, monitor.css) on MANAGER_BIND:MANAGER_PORT.
2. Link /mqtt in the WAGO web server and keep it linked: write
   /etc/lighttpd/apps.confd/mqtt.conf on the controller, let lighttpd check the whole
   configuration (lighttpd -tt), reload it. Checked again every LINK_CHECK_S seconds, so
   the link comes back by itself (e.g. after a firmware update). A configuration that
   lighttpd rejects is removed again at once, so the WBM is never left broken.
   Needs --privileged --pid host: the commands run on the controller through nsenter.

    python mqtt_manager.py            serve + keep the link (the container's default)
    python mqtt_manager.py --unlink   remove /mqtt from the web server and exit

Environment: MANAGER_BIND (127.0.0.1), MANAGER_PORT (9080), MQTT_WS_PORT (9001),
WEB_PATH (/mqtt), LINK (1; 0 = only serve), LINK_CHECK_S (60).
"""
import argparse
import hashlib
import http.server
import json
import logging
import os
import re
import signal
import subprocess
import sys
import threading
import time

WEB_DIR = os.environ.get("WEB_DIR", "/app/www")
BIND = os.environ.get("MANAGER_BIND", "127.0.0.1")
PORT = int(os.environ.get("MANAGER_PORT", "9080"))
WS_PORT = int(os.environ.get("MQTT_WS_PORT", "9001"))
WEB_PATH = "/" + os.environ.get("WEB_PATH", "/mqtt").strip("/")
LINK = os.environ.get("LINK", "1") == "1"
LINK_CHECK_S = int(os.environ.get("LINK_CHECK_S", "60"))
CONF_DIR = "/etc/lighttpd/apps.confd"
CONF = f"{CONF_DIR}/mqtt.conf"
MOSQUITTO = os.environ.get("MOSQUITTO_CONTAINER", "mosquitto")

log = logging.getLogger("mqtt-manager")
stop = threading.Event()


# ---------------------------------------------------------------------------
# The page
# ---------------------------------------------------------------------------
class Handler(http.server.SimpleHTTPRequestHandler):
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

    def do_GET(self):
        # https://<pfc>/mqtt (no "/"): the web server passes it on unmapped -> redirect to "/",
        # which the web server maps back to /mqtt/ (proxy map-urlpath also maps Location)
        if self.path.split("?")[0] == WEB_PATH:
            self.send_response(301)
            self.send_header("Location", "/" + self.path[len(WEB_PATH):].lstrip("/"))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.split("?")[0] == "/clients.json":
            body = json.dumps(clients.snapshot()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def list_directory(self, path):          # no directory listings
        self.send_error(404)
        return None

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, fmt, *args):       # quiet: only errors
        if args and str(args[1] if len(args) > 1 else "").startswith(("4", "5")):
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
    log.info("monitor page on http://%s:%d/ (%s)", BIND, PORT, WEB_DIR)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ---------------------------------------------------------------------------
# Link in the WAGO web server
# ---------------------------------------------------------------------------
def lighttpd_conf() -> bytes:
    p = WEB_PATH
    return f"""# MQTT monitor (kbus-gateway): http(s)://<pfc>{p}/
#   {p}/    -> mqtt-manager 127.0.0.1:{PORT} (the page)
#   {p}/ws  -> mosquitto    127.0.0.1:{WS_PORT} (MQTT over WebSocket, read only)
# Written and kept in place by the container mqtt-manager (docker/mqtt-manager, version 2).
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
}}
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
    ap = argparse.ArgumentParser(description="MQTT monitor page + link in the WAGO web server")
    ap.add_argument("--unlink", action="store_true", help="remove the link from the web server and exit")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if args.unlink:
        sys.exit(unlink())

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
