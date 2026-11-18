"""mqtt-bridge: subscribe to topics on other MQTT brokers and publish them on the
MQTT broker of the WAGO PFC (mosquitto, normally 127.0.0.1:1883).

    remote broker A ─┐
    remote broker B ─┼─ subscribe ── mqtt-bridge ── publish ──▶ mosquitto on the PFC ◀── kbus-gateway
    ...             ─┘                                                              ◀── CODESYS, Node-RED ...

Configuration: an INI file (default /config/mqtt-bridge.ini, see mqtt-bridge.ini):
    [local]            the broker on the PFC
    [remote <name>]    one section per remote broker, with its subscriptions:
        topics = <remote topic filter> -> <local topic prefix>    (one per line)
    A message on remote topic T is published locally as <local prefix> + T
    (after removing strip_prefix from the front of T, when it starts with it).

Status (retained, on the local broker):
    <status_topic>/status          online | offline   (the bridge itself, last will)
    <status_topic>/<name>/status   online | offline   (connection to each remote broker)
"""
import argparse
import configparser
import logging
import os
import signal
import socket
import ssl
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import paho.mqtt.client as mqtt

DEFAULT_CONFIG = "/config/mqtt-bridge.ini"
log = logging.getLogger("mqtt-bridge")


@dataclass
class BrokerSettings:
    name: str
    host: str
    port: int = 1883
    username: str = ""
    password: str = ""
    client_id: str = ""
    keepalive: int = 30
    protocol: str = "3.1.1"            # 3.1.1 | 5
    transport: str = "tcp"             # tcp | websockets
    ws_path: str = "/mqtt"
    tls: bool = False
    ca_file: str = ""                  # empty = the system CAs
    cert_file: str = ""                # client certificate (optional)
    key_file: str = ""
    insecure: bool = False             # True = do not check the server certificate (testing only)


@dataclass
class RemoteSettings(BrokerSettings):
    topics: List[Tuple[str, str]] = field(default_factory=list)   # (remote filter, local prefix)
    strip_prefix: str = ""
    qos: int = 1
    retain: str = "always"             # always | keep | never


@dataclass
class Settings:
    log_level: str
    status_topic: str
    stats_s: int
    local: BrokerSettings
    remotes: List[RemoteSettings]


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
def _resolve(path: str, base: str) -> str:
    return path if not path or os.path.isabs(path) else os.path.join(base, path)


def _broker(sec: configparser.SectionProxy, name: str, cls, base: str, default_id: str, **extra):
    b = cls(
        name=name,
        host=sec.get("host", "").strip(),
        port=sec.getint("port", 8883 if sec.getboolean("tls", False) else 1883),
        username=sec.get("username", "").strip(),
        password=sec.get("password", ""),
        client_id=sec.get("client_id", "").strip() or default_id,
        keepalive=sec.getint("keepalive", 30),
        protocol=sec.get("protocol", "3.1.1").strip(),
        transport=sec.get("transport", "tcp").strip().lower(),
        ws_path=sec.get("ws_path", "/mqtt").strip(),
        tls=sec.getboolean("tls", False),
        ca_file=_resolve(sec.get("ca_file", "").strip(), base),
        cert_file=_resolve(sec.get("cert_file", "").strip(), base),
        key_file=_resolve(sec.get("key_file", "").strip(), base),
        insecure=sec.getboolean("insecure", False),
        **extra,
    )
    where = f"[{sec.name}]"
    if not b.host:
        raise ValueError(f"{where} host is required")
    if b.protocol not in ("3.1.1", "5"):
        raise ValueError(f"{where} protocol must be 3.1.1 or 5")
    if b.transport not in ("tcp", "websockets"):
        raise ValueError(f"{where} transport must be tcp or websockets")
    for f in (b.ca_file, b.cert_file, b.key_file):
        if f and not os.path.isfile(f):
            raise ValueError(f"{where} file not found: {f}")
    return b


def _topics(text: str, where: str) -> List[Tuple[str, str]]:
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if "->" in line:
            remote, local = (x.strip() for x in line.split("->", 1))
        else:
            remote, local = line, ""
        if not remote:
            raise ValueError(f"{where} topics: empty remote filter in {line!r}")
        if "+" in local or "#" in local:
            raise ValueError(f"{where} topics: the local prefix must not contain wildcards: {line!r}")
        out.append((remote, local))
    if not out:
        raise ValueError(f"{where} topics: at least one '<remote filter> -> <local prefix>' line is required")
    return out


def load_settings(path: str) -> Settings:
    if not os.path.isfile(path):
        raise FileNotFoundError(f"configuration file not found: {path}")
    cfg = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";",))
    cfg.optionxform = str
    cfg.read(path, encoding="utf-8-sig")
    base = os.path.dirname(os.path.abspath(path))
    host = socket.gethostname()

    general = cfg["general"] if cfg.has_section("general") else {}
    if not cfg.has_section("local"):
        raise ValueError("section [local] (the broker on the PFC) is missing")
    local = _broker(cfg["local"], "local", BrokerSettings, base, f"mqtt-bridge-{host}")

    remotes: List[RemoteSettings] = []
    for sec_name in cfg.sections():
        if not sec_name.startswith("remote"):
            continue
        name = sec_name[len("remote"):].strip()
        if not name or "/" in name or "+" in name or "#" in name:
            raise ValueError(f"[{sec_name}]: write the section as [remote <name>], name without / + #")
        sec = cfg[sec_name]
        retain = sec.get("retain", "always").strip().lower()
        if retain not in ("always", "keep", "never"):
            raise ValueError(f"[{sec_name}] retain must be always, keep or never")
        qos = sec.getint("qos", 1)
        if qos not in (0, 1, 2):
            raise ValueError(f"[{sec_name}] qos must be 0, 1 or 2")
        remotes.append(_broker(sec, name, RemoteSettings, base, f"mqtt-bridge-{name}-{host}",
                               topics=_topics(sec.get("topics", ""), f"[{sec_name}]"),
                               strip_prefix=sec.get("strip_prefix", "").strip(),
                               qos=qos, retain=retain))
    if len({r.name for r in remotes}) != len(remotes):
        raise ValueError("two [remote ...] sections have the same name")

    get = general.get if hasattr(general, "get") else (lambda k, d=None: d)
    return Settings(
        log_level=str(get("log_level", "INFO")).upper(),
        status_topic=str(get("status_topic", "bridge")).strip().strip("/") or "bridge",
        stats_s=int(get("stats_s", "300")),
        local=local,
        remotes=remotes,
    )


# ---------------------------------------------------------------------------
# MQTT
# ---------------------------------------------------------------------------
def make_client(b: BrokerSettings, clean: bool = True) -> mqtt.Client:
    proto = mqtt.MQTTv5 if b.protocol == "5" else mqtt.MQTTv311
    kw = {} if proto == mqtt.MQTTv5 else {"clean_session": clean}
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=b.client_id, protocol=proto,
                    transport=b.transport, **kw)
    if b.transport == "websockets":
        c.ws_set_options(path=b.ws_path)
    if b.username:
        c.username_pw_set(b.username, b.password or None)
    if b.tls:
        c.tls_set(ca_certs=b.ca_file or None, certfile=b.cert_file or None, keyfile=b.key_file or None,
                  cert_reqs=ssl.CERT_NONE if b.insecure else ssl.CERT_REQUIRED)
        if b.insecure:
            c.tls_insecure_set(True)
    c.reconnect_delay_set(min_delay=1, max_delay=30)
    return c


def endpoint(b: BrokerSettings) -> str:
    scheme = ("wss" if b.tls else "ws") if b.transport == "websockets" else ("mqtts" if b.tls else "mqtt")
    return f"{scheme}://{b.host}:{b.port}"


class Bridge:
    def __init__(self, s: Settings):
        self.s = s
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.counts = {r.name: [0, 0] for r in s.remotes}    # received, forwarded

        # Local broker: messages are queued while it is not reachable (QoS 1/2).
        self.local = make_client(s.local)
        self.local.max_queued_messages_set(10000)
        self.local.will_set(f"{s.status_topic}/status", "offline", qos=1, retain=True)
        self.local.on_connect = self._local_connect
        self.local.on_disconnect = self._local_disconnect
        self.local_online = False
        self.remote_state = {r.name: "offline" for r in s.remotes}

        self.remotes = []
        for r in s.remotes:
            c = make_client(r)
            c.user_data_set(r)
            c.on_connect = self._remote_connect
            c.on_disconnect = self._remote_disconnect
            c.on_message = self._remote_message
            self.remotes.append((r, c))

    # ---- local --------------------------------------------------------------
    def _local_connect(self, client, _u, _flags, reason, _props):
        if reason.is_failure:
            log.error("local broker %s refused the connection: %s", endpoint(self.s.local), reason)
            return
        log.info("connected to the local broker %s", endpoint(self.s.local))
        self.local_online = True
        client.publish(f"{self.s.status_topic}/status", "online", qos=1, retain=True)
        for name, state in self.remote_state.items():
            client.publish(f"{self.s.status_topic}/{name}/status", state, qos=1, retain=True)

    def _local_disconnect(self, _c, _u, _flags, reason, _props):
        if self.local_online:
            log.error("local broker %s lost: %s (reconnecting; messages are queued)", endpoint(self.s.local), reason)
        self.local_online = False

    def _remote_status(self, r: RemoteSettings, state: str):
        self.remote_state[r.name] = state
        self.local.publish(f"{self.s.status_topic}/{r.name}/status", state, qos=1, retain=True)

    # ---- remote -------------------------------------------------------------
    def _remote_connect(self, client, r: RemoteSettings, _flags, reason, _props):
        if reason.is_failure:
            log.error("[%s] %s refused the connection: %s", r.name, endpoint(r), reason)
            return
        log.info("[%s] connected to %s", r.name, endpoint(r))
        if r.protocol == "5":
            # retainAsPublished: the retain flag of live messages is passed on (MQTT 5 only)
            opts = mqtt.SubscribeOptions(qos=r.qos, retainAsPublished=True)
            client.subscribe([(f, opts) for f, _ in r.topics])
        else:
            client.subscribe([(f, r.qos) for f, _ in r.topics])
        for f, local in r.topics:
            if local and "+" not in f and "#" not in f and not local.endswith("/"):
                target = local
            else:
                target = (local.rstrip("/") + "/" if local else "") + (
                    f"<topic without {r.strip_prefix}>" if r.strip_prefix else "<topic>")
            log.info("[%s] subscribed %s -> %s", r.name, f, target)
        self._remote_status(r, "online")

    def _remote_disconnect(self, _c, r: RemoteSettings, _flags, reason, _props):
        if self.remote_state.get(r.name) == "online":
            log.error("[%s] connection to %s lost: %s (reconnecting)", r.name, endpoint(r), reason)
        self._remote_status(r, "offline")

    @staticmethod
    def map_topic(r: RemoteSettings, topic: str) -> Optional[str]:
        """Local topic for a remote topic (first matching subscription line).

        filter without wildcards and local without '/' at the end: exactly that local topic
        otherwise: local prefix (a '/' is added when missing) + topic without strip_prefix
        """
        for f, local in r.topics:
            if not mqtt.topic_matches_sub(f, topic):
                continue
            if local and "+" not in f and "#" not in f and not local.endswith("/"):
                return local
            t = topic[len(r.strip_prefix):] if r.strip_prefix and topic.startswith(r.strip_prefix) else topic
            if local and not local.endswith("/"):
                local += "/"
            return (local + t) if t else None
        return None

    def _remote_message(self, _c, r: RemoteSettings, msg):
        with self.lock:
            self.counts[r.name][0] += 1
        t = self.map_topic(r, msg.topic)
        if t is None:
            return
        retain = r.retain == "always" or (r.retain == "keep" and msg.retain)
        if r.retain == "always" and not msg.payload:
            retain = True        # an empty retained message deletes the local value as well
        self.local.publish(t, msg.payload, qos=min(msg.qos, r.qos), retain=retain)
        with self.lock:
            self.counts[r.name][1] += 1
        if log.isEnabledFor(logging.DEBUG):
            log.debug("[%s] %s -> %s (%d bytes%s)", r.name, msg.topic, t, len(msg.payload),
                      ", retained" if retain else "")

    # ---- run ----------------------------------------------------------------
    def run(self):
        s = self.s
        self.local.connect_async(s.local.host, s.local.port, s.local.keepalive)
        self.local.loop_start()
        for r, c in self.remotes:
            log.info("[%s] connecting to %s (%s)", r.name, endpoint(r), ", ".join(f for f, _ in r.topics))
            c.connect_async(r.host, r.port, r.keepalive)
            c.loop_start()
        next_stats = time.monotonic() + s.stats_s
        while not self.stop_event.wait(1.0):
            if s.stats_s > 0 and time.monotonic() >= next_stats:
                next_stats += s.stats_s
                with self.lock:
                    parts = [f"{n}: {rx} received, {fw} forwarded" for n, (rx, fw) in self.counts.items()]
                log.info("messages since start: %s", "; ".join(parts))
        log.info("stopping")
        for _r, c in self.remotes:
            c.disconnect()
            c.loop_stop()
        self.local.publish(f"{s.status_topic}/status", "offline", qos=1, retain=True).wait_for_publish(2)
        for r in s.remotes:
            self.local.publish(f"{s.status_topic}/{r.name}/status", "offline", qos=1, retain=True)
        self.local_online = False                          # a planned stop, not a lost connection
        self.local.disconnect()
        self.local.loop_stop()


def main():
    ap = argparse.ArgumentParser(description="Subscribe on other MQTT brokers, publish on the PFC broker")
    ap.add_argument("--config", default=os.environ.get("BRIDGE_CONFIG", DEFAULT_CONFIG))
    args = ap.parse_args()
    try:
        s = load_settings(args.config)
    except (ValueError, FileNotFoundError, configparser.Error) as e:
        logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s")
        log.error("%s", e)
        sys.exit(2)
    logging.basicConfig(level=getattr(logging, s.log_level, logging.INFO),
                        format="%(asctime)s %(levelname)s %(message)s")
    log.info("local broker %s, %d remote broker(s), status topics %s/...", endpoint(s.local),
             len(s.remotes), s.status_topic)

    if not s.remotes:
        log.warning("no [remote <name>] section in %s: nothing to bridge yet", args.config)
    bridge = Bridge(s)

    def on_signal(*_):
        # docker stop sends SIGTERM; force the exit when the clean stop takes longer than 5 s
        bridge.stop_event.set()
        t = threading.Timer(5.0, lambda: os._exit(1))
        t.daemon = True
        t.start()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    bridge.run()


if __name__ == "__main__":
    main()
