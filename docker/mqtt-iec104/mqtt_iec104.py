#!/usr/bin/env python3
"""mqtt-iec104: gateway between the MQTT broker and an IEC 60870-5-104 server.

    MQTT -> IEC 104   [points]    every topic becomes a monitoring point (single point,
                                  measured value float or scaled) of an IEC 104 server
    IEC 104 -> MQTT   [commands]  a command written by an IEC 104 client (single command,
                                  set point float or scaled) is published on a topic

The mapping is a YAML file (default /config/mqtt-iec104.yaml, see mqtt-iec104.yaml).
The IEC 104 server is c104, the Python binding of mz-automation/lib60870-C.

    python3 mqtt_iec104.py [--config FILE] [--check]
"""
import argparse
import json
import logging
import math
import os
import signal
import socket
import ssl
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import c104
import paho.mqtt.client as mqtt
import yaml

DEFAULT_CONFIG = os.environ.get("IEC104_CONFIG", "/config/mqtt-iec104.yaml")
log = logging.getLogger("mqtt-iec104")

# YAML type name -> IEC 104 type (monitoring direction: always with CP56Time2a time tag)
POINT_TYPES = {
    "single": c104.Type.M_SP_TB_1,   # single point information, true / false
    "float": c104.Type.M_ME_TF_1,    # measured value, short floating point
    "scaled": c104.Type.M_ME_TE_1,   # measured value, scaled (INT16)
}
# YAML type name -> IEC 104 command type
COMMAND_TYPES = {
    "single": c104.Type.C_SC_NA_1,   # single command, on / off
    "float": c104.Type.C_SE_NC_1,    # set point command, short floating point
    "scaled": c104.Type.C_SE_NB_1,   # set point command, scaled (INT16)
}
COMMAND_MODES = {
    "direct": c104.CommandMode.DIRECT,
    "select": c104.CommandMode.SELECT_AND_EXECUTE,
}


class ConfigError(Exception):
    pass


# ---------------------------------------------------------------------------
# configuration
# ---------------------------------------------------------------------------

@dataclass
class PointCfg:
    topic: str
    ioa: int
    type: str
    key: str = ""             # JSON payload: dotted path of the value, e.g. "position"
    scale: float = 1.0        # IEC value = MQTT value * scale + offset
    offset: float = 0.0
    report_ms: int = 0
    name: str = ""


@dataclass
class CommandCfg:
    topic: str
    ioa: int
    type: str
    scale: float = 1.0        # MQTT value = IEC value * scale + offset
    offset: float = 0.0
    integer: bool = False     # publish the result as an integer (rounded)
    min: Optional[float] = None   # reject values outside min..max (after scaling)
    max: Optional[float] = None
    on: str = "true"          # payloads of a single command
    off: str = "false"
    retain: bool = False
    name: str = ""


@dataclass
class Settings:
    log_level: str = "INFO"
    stale_after: float = 0.0
    mqtt_host: str = "127.0.0.1"
    mqtt_port: int = 1883
    mqtt_client_id: str = ""
    mqtt_username: str = ""
    mqtt_password: str = ""
    mqtt_keepalive: int = 30
    qos: int = 0
    tls: bool = False
    ca_cert: str = ""
    tls_insecure: bool = False
    availability_topic: str = ""
    availability_online: str = "online"
    status_topic: str = "iec104/status"
    iec_ip: str = "0.0.0.0"
    iec_port: int = 2404
    iec_max_connections: int = 0
    iec_common_address: int = 1
    iec_report_ms: int = 0
    iec_command_mode: str = "direct"
    points: List[PointCfg] = field(default_factory=list)
    commands: List[CommandCfg] = field(default_factory=list)


def _section(data: dict, name: str) -> dict:
    value = data.get(name) or {}
    if not isinstance(value, dict):
        raise ConfigError(f"'{name}' must be a mapping")
    return value


def _num(value: Any, where: str, kind=float, lo=None, hi=None):
    if isinstance(value, bool):
        raise ConfigError(f"{where}: expected a number, got {value!r}")
    try:
        result = kind(value)
    except (TypeError, ValueError):
        raise ConfigError(f"{where}: expected a number, got {value!r}")
    if kind is int and isinstance(value, float) and not value.is_integer():
        raise ConfigError(f"{where}: expected an integer, got {value!r}")
    if (lo is not None and result < lo) or (hi is not None and result > hi):
        raise ConfigError(f"{where}: must be between {lo} and {hi}, got {value!r}")
    return result


def _expand(entries: Any, section: str, types: dict, known: dict) -> List[dict]:
    """Expands 'count' entries: topic 'kbus/di/{n}', ioa 101, count 8 -> 8 entries."""
    if entries is None:
        return []
    if not isinstance(entries, list):
        raise ConfigError(f"'{section}' must be a list of entries ('- topic: ...')")
    result = []
    for i, entry in enumerate(entries, 1):
        where = f"{section}[{i}]"
        if not isinstance(entry, dict):
            raise ConfigError(f"{where}: must be a mapping with topic, ioa and type")
        unknown = set(entry) - set(known) - {"topic", "ioa", "type", "count", "start"}
        if unknown:
            raise ConfigError(f"{where}: unknown key(s) {', '.join(sorted(unknown))}")
        topic = entry.get("topic")
        if not isinstance(topic, str) or not topic.strip():
            raise ConfigError(f"{where}: 'topic' is required")
        topic = topic.strip()
        if "+" in topic or "#" in topic:
            raise ConfigError(f"{where}: no MQTT wildcards in '{topic}'; use '{{n}}' with 'count'")
        if "ioa" not in entry:
            raise ConfigError(f"{where}: 'ioa' is required")
        ioa = _num(entry["ioa"], f"{where} ioa", int, 1, 16777215)
        kind = str(entry.get("type", "")).strip().lower()
        if kind not in types:
            raise ConfigError(f"{where}: 'type' must be one of {', '.join(types)}, got {entry.get('type')!r}")
        count = _num(entry.get("count", 1), f"{where} count", int, 1, 10000)
        start = _num(entry.get("start", 1), f"{where} start", int, 0, 1000000)
        if count > 1 and "{n}" not in topic:
            raise ConfigError(f"{where}: 'count' needs '{{n}}' in the topic")
        extra = {}
        for k, conv in known.items():
            if k in entry and entry[k] is not None:
                extra[k] = conv(entry[k], f"{where} {k}")
        for j in range(count):
            n = start + j
            item = dict(extra, topic=topic.replace("{n}", str(n)), ioa=ioa + j, type=kind)
            if "name" in item:
                item["name"] = item["name"].replace("{n}", str(n))
            result.append(item)
    return result


def _str(v, where):
    return str(v)


def _bool(v, where):
    if isinstance(v, bool):
        return v
    raise ConfigError(f"{where}: expected true or false, got {v!r}")


def load_settings(path: str) -> Settings:
    try:
        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
    except FileNotFoundError:
        raise ConfigError(f"configuration file not found: {path}")
    except yaml.YAMLError as exc:
        raise ConfigError(f"{path}: invalid YAML: {exc}")
    if not isinstance(data, dict):
        raise ConfigError(f"{path}: the top level must be a mapping")

    general, m, iec = _section(data, "general"), _section(data, "mqtt"), _section(data, "iec104")
    avail = m.get("availability") or {}
    if not isinstance(avail, dict):
        raise ConfigError("mqtt.availability must be a mapping (topic, online) or empty")

    s = Settings(
        log_level=str(general.get("log_level", "INFO")).upper(),
        stale_after=_num(general.get("stale_after", 0), "general.stale_after", float, 0),
        mqtt_host=str(m.get("host", "127.0.0.1")).strip(),
        mqtt_port=_num(m.get("port", 1883), "mqtt.port", int, 1, 65535),
        mqtt_client_id=str(m.get("client_id") or "").strip(),
        mqtt_username=str(m.get("username") or "").strip(),
        mqtt_password=str(m.get("password") or ""),
        mqtt_keepalive=_num(m.get("keepalive", 30), "mqtt.keepalive", int, 5, 3600),
        qos=_num(m.get("qos", 0), "mqtt.qos", int, 0, 2),
        tls=bool(m.get("tls", False)),
        ca_cert=str(m.get("ca_cert") or "").strip(),
        tls_insecure=bool(m.get("tls_insecure", False)),
        availability_topic=str(avail.get("topic") or "").strip(),
        availability_online=str(avail.get("online", "online")),
        status_topic=str(m.get("status_topic") or "").strip(),
        iec_ip=str(iec.get("ip", "0.0.0.0")).strip(),
        iec_port=_num(iec.get("port", 2404), "iec104.port", int, 1, 65535),
        iec_max_connections=_num(iec.get("max_connections", 0), "iec104.max_connections", int, 0, 1000),
        iec_common_address=_num(iec.get("common_address", 1), "iec104.common_address", int, 1, 65534),
        iec_report_ms=_num(iec.get("report_ms", 0), "iec104.report_ms", int, 0, 86400000),
        iec_command_mode=str(iec.get("command_mode", "direct")).strip().lower(),
    )
    if "status_topic" not in m:
        s.status_topic = "iec104/status"
    if not s.mqtt_host:
        raise ConfigError("mqtt.host is required")
    if s.iec_command_mode not in COMMAND_MODES:
        raise ConfigError("iec104.command_mode must be 'direct' or 'select'")
    if not s.mqtt_client_id:
        s.mqtt_client_id = f"mqtt-iec104-{socket.gethostname()}"

    num = lambda v, w: _num(v, w)
    point_keys = {"key": _str, "scale": num, "offset": num, "name": _str,
                  "report_ms": lambda v, w: _num(v, w, int, 0, 86400000)}
    command_keys = {"scale": num, "offset": num, "integer": _bool, "min": num, "max": num,
                    "on": _str, "off": _str, "retain": _bool, "name": _str}
    s.points = [PointCfg(**{"report_ms": s.iec_report_ms, **p})
                for p in _expand(data.get("points"), "points", POINT_TYPES, point_keys)]
    s.commands = [CommandCfg(**{"integer": c["type"] == "scaled", **c})
                  for c in _expand(data.get("commands"), "commands", COMMAND_TYPES, command_keys)]

    if not s.points and not s.commands:
        raise ConfigError("'points' or 'commands' must contain at least one entry")
    used: Dict[int, str] = {}
    for item in [*s.points, *s.commands]:
        if item.ioa in used:
            raise ConfigError(f"IOA {item.ioa} is used twice ({used[item.ioa]} and {item.topic})")
        used[item.ioa] = item.topic
    point_topics = {p.topic for p in s.points}
    for c in s.commands:
        if c.topic in point_topics:
            raise ConfigError(f"commands: {c.topic} is also a point topic (the gateway would read its own commands)")
    return s


# ---------------------------------------------------------------------------
# payloads
# ---------------------------------------------------------------------------

def parse_payload(payload: bytes, key: str) -> Any:
    """Plain value (42, 21.4, true, on) or JSON; 'key' selects a field of a JSON object."""
    text = payload.decode("utf-8").strip()
    try:
        data = json.loads(text)
    except ValueError:
        low = text.lower()
        if low in ("on", "true"):
            return True
        if low in ("off", "false"):
            return False
        raise ValueError(f"not a number or boolean: {text[:40]!r}")
    if key:
        for part in key.split("."):
            if isinstance(data, dict) and part in data:
                data = data[part]
            elif isinstance(data, list) and part.isdigit() and int(part) < len(data):
                data = data[int(part)]
            else:
                raise ValueError(f"no field '{key}' in the payload")
    elif isinstance(data, dict):
        if "value" not in data:
            raise ValueError("JSON object without a 'value' field (set 'key' in the configuration)")
        data = data["value"]
    if data is None:
        raise ValueError("value is null")
    if not isinstance(data, (bool, int, float)):
        raise ValueError(f"not a number or boolean: {data!r}")
    return data


# ---------------------------------------------------------------------------
# gateway
# ---------------------------------------------------------------------------

class Gateway:
    def __init__(self, s: Settings):
        self.s = s
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.available = not s.availability_topic   # without an availability topic: always
        self.mqtt_connected_at = 0.0

        self.server = c104.Server(ip=s.iec_ip, port=s.iec_port, max_connections=s.iec_max_connections)
        self.station = self.server.add_station(common_address=s.iec_common_address)

        # topic -> monitoring points (one topic may feed several IOAs, e.g. different JSON keys)
        self.points: Dict[str, List[tuple]] = {}
        self.has_value: Dict[int, bool] = {}
        self.last_update: Dict[int, float] = {}
        for p in s.points:
            point = self.station.add_point(io_address=p.ioa, type=POINT_TYPES[p.type], report_ms=p.report_ms)
            point.info = self.make_info(p, None, c104.Quality.Invalid)
            self.points.setdefault(p.topic, []).append((p, point))
            self.has_value[p.ioa] = False

        self.commands: Dict[int, CommandCfg] = {}
        for c in s.commands:
            point = self.station.add_point(io_address=c.ioa, type=COMMAND_TYPES[c.type],
                                           command_mode=COMMAND_MODES[s.iec_command_mode])
            point.on_receive(callable=self.on_command)
            self.commands[c.ioa] = c

        self.server.on_connect(callable=self.on_iec_connect)
        self.mqtt = self.create_mqtt()

    # --- IEC 104 information objects ---

    @staticmethod
    def now() -> datetime:
        return datetime.now(timezone.utc)

    def make_info(self, p: PointCfg, value: Any, quality: c104.Quality):
        if p.type == "single":
            return c104.SingleInfo(on=bool(value), quality=quality, recorded_at=self.now())
        number = 0.0 if value is None else float(value)
        if p.type == "float":
            return c104.ShortInfo(actual=number, quality=quality, recorded_at=self.now())
        if not -32768 <= number <= 32767:
            quality = quality | c104.Quality.Overflow
            number = max(-32768.0, min(32767.0, number))
        return c104.ScaledInfo(actual=c104.Int16(int(round(number))), quality=quality, recorded_at=self.now())

    def convert(self, p: PointCfg, raw: Any) -> Any:
        if p.type == "single":
            return bool(raw)
        value = float(raw) * p.scale + p.offset
        if math.isnan(value) or math.isinf(value):
            raise ValueError(f"result is {value}")
        return value

    def transmit(self, point: c104.Point):
        if self.server.has_active_connections:
            point.transmit(cause=c104.Cot.SPONTANEOUS)

    def set_value(self, p: PointCfg, point: c104.Point, value: Any):
        quality = c104.Quality() if self.available else c104.Quality.Invalid
        with self.lock:
            point.info = self.make_info(p, value, quality)
            self.has_value[p.ioa] = True
            self.last_update[p.ioa] = time.monotonic()
            self.transmit(point)
        log.debug("%s = %s -> IOA %d", p.topic, value, p.ioa)

    def set_quality_all(self, quality: c104.Quality, only_with_value=False):
        with self.lock:
            for entries in self.points.values():
                for p, point in entries:
                    if only_with_value and not self.has_value[p.ioa]:
                        continue
                    if point.quality == quality:
                        continue
                    value = point.value if self.has_value[p.ioa] else None
                    if p.type == "scaled" and value is not None:
                        value = int(value)
                    point.info = self.make_info(p, value, quality)
                    self.transmit(point)

    def check_stale(self):
        if self.s.stale_after <= 0:
            return
        now = time.monotonic()
        with self.lock:
            for entries in self.points.values():
                for p, point in entries:
                    last = self.last_update.get(p.ioa)
                    if last is not None and now - last > self.s.stale_after and point.quality.is_good():
                        value = int(point.value) if p.type == "scaled" else point.value
                        point.info = self.make_info(p, value, c104.Quality.NonTopical)
                        self.transmit(point)
                        log.warning("IOA %d (%s): no new value for %.0f s -> NonTopical", p.ioa, p.topic, self.s.stale_after)

    def on_iec_connect(self, server: c104.Server, ip: str) -> bool:
        log.info("IEC 104 client %s connects", ip)
        return True

    def on_command(self, point: c104.Point, previous_info: c104.Information, message: c104.IncomingMessage) -> c104.ResponseState:
        c = self.commands[point.io_address]
        raw = point.value
        if c.type == "single":
            payload = c.on if bool(raw) else c.off
            shown = bool(raw)
        else:
            value = float(raw) * c.scale + c.offset
            if (c.min is not None and value < c.min) or (c.max is not None and value > c.max):
                log.error("IOA %d: %s outside %s..%s, not published to %s", c.ioa, value, c.min, c.max, c.topic)
                return c104.ResponseState.FAILURE
            payload = str(int(round(value))) if c.integer else repr(float(value))
            shown = payload
        if not self.mqtt.is_connected():
            log.error("IOA %d = %s not published to %s: MQTT broker not connected", c.ioa, shown, c.topic)
            return c104.ResponseState.FAILURE
        if not self.available:
            log.error("IOA %d = %s not published to %s: %s is not '%s'", c.ioa, shown, c.topic,
                      self.s.availability_topic, self.s.availability_online)
            return c104.ResponseState.FAILURE
        info = self.mqtt.publish(c.topic, payload, qos=self.s.qos, retain=c.retain)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            log.error("IOA %d = %s not published to %s: %s", c.ioa, shown, c.topic, mqtt.error_string(info.rc))
            return c104.ResponseState.FAILURE
        log.info("IEC 104 command IOA %d = %s -> %s %s", c.ioa, raw, c.topic, payload)
        return c104.ResponseState.SUCCESS

    # --- MQTT ---

    def create_mqtt(self) -> mqtt.Client:
        s = self.s
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=s.mqtt_client_id)
        if s.mqtt_username:
            client.username_pw_set(s.mqtt_username, s.mqtt_password or None)
        if s.tls:
            client.tls_set(ca_certs=s.ca_cert or None, cert_reqs=ssl.CERT_REQUIRED)
            client.tls_insecure_set(s.tls_insecure)
        if s.status_topic:
            client.will_set(s.status_topic, "offline", qos=1, retain=True)
        client.reconnect_delay_set(min_delay=1, max_delay=30)
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        client.on_message = self.on_message
        return client

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT broker refused the connection: %s", reason_code)
            return
        self.mqtt_connected_at = time.time()
        log.info("connected to MQTT broker %s:%d as %s", self.s.mqtt_host, self.s.mqtt_port, self.s.mqtt_client_id)
        topics = list(self.points)
        if self.s.availability_topic:
            topics.append(self.s.availability_topic)
        for t in topics:
            client.subscribe(t, qos=self.s.qos)
        log.info("subscribed to %d topic(s)", len(topics))
        if self.s.status_topic:
            client.publish(self.s.status_topic, "online", qos=1, retain=True)

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        if self.stop_event.is_set():
            return
        log.warning("MQTT connection lost (%s): all values Invalid", reason_code)
        if reason_code.is_failure and time.time() - self.mqtt_connected_at < 5:
            log.warning("the broker closed the connection right after connecting: another client with "
                        "client_id '%s', or the ACL refuses this client", self.s.mqtt_client_id)
        self.set_quality_all(c104.Quality.Invalid)

    def on_message(self, client, userdata, msg):
        if self.s.availability_topic and msg.topic == self.s.availability_topic:
            online = msg.payload.decode("utf-8", "replace").strip() == self.s.availability_online
            if online != self.available:
                self.available = online
                if online:
                    log.info("%s is '%s': values valid", msg.topic, self.s.availability_online)
                    self.set_quality_all(c104.Quality(), only_with_value=True)
                else:
                    log.warning("%s is '%s': all values Invalid, commands refused",
                                msg.topic, msg.payload.decode("utf-8", "replace"))
                    self.set_quality_all(c104.Quality.Invalid)
            if msg.topic not in self.points:
                return
        if not msg.payload:          # a deleted retained topic
            return
        for p, point in self.points.get(msg.topic, []):
            try:
                value = self.convert(p, parse_payload(msg.payload, p.key))
            except (ValueError, UnicodeDecodeError) as exc:
                log.error("%s (IOA %d): %s", msg.topic, p.ioa, exc)
                continue
            self.set_value(p, point, value)

    # --- main loop ---

    def run(self):
        s = self.s
        self.server.start()
        log.info("IEC 104 server on %s:%d, common address %d, %d point(s), %d command(s), command mode %s",
                 s.iec_ip, s.iec_port, s.iec_common_address, len(s.points), len(s.commands), s.iec_command_mode)
        self.mqtt.connect_async(s.mqtt_host, s.mqtt_port, keepalive=s.mqtt_keepalive)
        self.mqtt.loop_start()
        try:
            while not self.stop_event.wait(1.0):
                self.check_stale()
        finally:
            log.info("stopping")
            self.stop_event.set()
            if s.status_topic and self.mqtt.is_connected():
                self.mqtt.publish(s.status_topic, "offline", qos=1, retain=True).wait_for_publish(2)
            self.mqtt.disconnect()
            self.mqtt.loop_stop()
            self.server.stop()


def print_mapping(s: Settings):
    log.info("MQTT broker %s:%d, IEC 104 server %s:%d, common address %d",
             s.mqtt_host, s.mqtt_port, s.iec_ip, s.iec_port, s.iec_common_address)
    for p in s.points:
        log.info("  point   IOA %-6d %-6s <- %s%s", p.ioa, p.type, p.topic,
                 f" [{p.key}]" if p.key else "")
    for c in s.commands:
        log.info("  command IOA %-6d %-6s -> %s", c.ioa, c.type, c.topic)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=DEFAULT_CONFIG, help=f"YAML file (default {DEFAULT_CONFIG})")
    ap.add_argument("--check", action="store_true", help="check the configuration, show the mapping and exit")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        s = load_settings(args.config)
    except ConfigError as exc:
        log.error("%s", exc)
        sys.exit(2)
    logging.getLogger().setLevel(getattr(logging, s.log_level, logging.INFO))
    print_mapping(s)
    if args.check:
        return
    gateway = Gateway(s)
    signal.signal(signal.SIGTERM, lambda *_: gateway.stop_event.set())
    signal.signal(signal.SIGINT, lambda *_: gateway.stop_event.set())
    gateway.run()


if __name__ == "__main__":
    main()
