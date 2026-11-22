#!/usr/bin/env python3
"""mqtt-modbus: Modbus TCP devices <-> the MQTT broker.

    Modbus -> MQTT   every tag of every device is read cyclically (one Modbus TCP
                     connection and one poll thread per device, adjacent registers
                     in one request) and published on <device topic>/<tag> on change
    MQTT -> Modbus   a value published on <device topic>/<tag>/set is written to the
                     device (tags with 'write: true'); coils with FC5, 16-bit registers
                     with FC6, 32/64-bit values with FC16

The configuration is a YAML file (default /config/mqtt-modbus.yaml, see mqtt-modbus.yaml).

    python3 mqtt_modbus.py [--config FILE] [--check]
"""
import argparse
import json
import logging
import math
import os
import signal
import socket
import ssl
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

import paho.mqtt.client as mqtt
import yaml
from pymodbus.client import ModbusTcpClient

DEFAULT_CONFIG = os.environ.get("MODBUS_CONFIG", "/config/mqtt-modbus.yaml")
log = logging.getLogger("mqtt-modbus")

# data type -> (number of 16-bit registers, struct format)
DATA_TYPES = {
    "bool": (1, None),
    "int16": (1, "h"), "uint16": (1, "H"),
    "int32": (2, "i"), "uint32": (2, "I"), "float32": (2, "f"),
    "int64": (4, "q"), "uint64": (4, "Q"), "float64": (4, "d"),
}
TABLES = ("coil", "discrete", "holding", "input")      # FC 1, 2, 3, 4
BIT_TABLES = ("coil", "discrete")
MAX_BLOCK = {"coil": 2000, "discrete": 2000, "holding": 125, "input": 125}


class ConfigError(Exception):
    pass


# ---------------------------------------------------------------------------
# configuration
# ---------------------------------------------------------------------------

@dataclass
class Tag:
    name: str
    table: str
    address: int
    type: str
    unit_id: int
    topic: str
    bit: Optional[int] = None       # holding/input: one bit 0..15 of the register (type bool)
    scale: float = 1.0              # value = raw * scale + offset
    offset: float = 0.0
    decimals: Optional[int] = None  # round published floats
    unit: str = ""
    write: bool = False
    min: Optional[float] = None     # refuse writes outside (engineering value)
    max: Optional[float] = None

    @property
    def size(self) -> int:
        return 1 if self.table in BIT_TABLES else DATA_TYPES[self.type][0]


@dataclass
class Device:
    name: str
    host: str
    port: int = 502
    unit_id: int = 1
    timeout: float = 3.0
    interval: float = 1.0
    word_order: str = "big"
    byte_order: str = "big"
    max_gap: int = 10
    topic: str = ""
    tags: List[Tag] = field(default_factory=list)


@dataclass
class Settings:
    log_level: str = "INFO"
    payload: str = "plain"          # plain | json
    republish_s: float = 0.0
    mqtt_host: str = "127.0.0.1"
    mqtt_port: int = 1883
    mqtt_client_id: str = ""
    mqtt_username: str = ""
    mqtt_password: str = ""
    mqtt_keepalive: int = 30
    qos: int = 0
    retain: bool = True
    tls: bool = False
    ca_cert: str = ""
    tls_insecure: bool = False
    status_topic: str = "modbus/status"
    devices: List[Device] = field(default_factory=list)


def _num(value: Any, where: str, kind=float, lo=None, hi=None):
    if isinstance(value, bool) or value is None:
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


def _bool(value: Any, where: str) -> bool:
    if isinstance(value, bool):
        return value
    raise ConfigError(f"{where}: expected true or false, got {value!r}")


def _mapping(value: Any, where: str) -> dict:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ConfigError(f"{where} must be a mapping")
    return value


def _check_keys(d: dict, allowed: set, where: str):
    unknown = set(d) - allowed
    if unknown:
        raise ConfigError(f"{where}: unknown key(s) {', '.join(sorted(unknown))}")


TAG_KEYS = {"name", "table", "address", "type", "unit_id", "topic", "bit", "scale", "offset",
            "decimals", "unit", "write", "min", "max", "count"}
DEVICE_KEYS = {"name", "host", "port", "unit_id", "timeout", "interval", "word_order", "byte_order",
               "max_gap", "topic", "tags"}


def load_tags(dev: Device, entries: Any, where: str) -> List[Tag]:
    if not isinstance(entries, list) or not entries:
        raise ConfigError(f"{where}.tags: a list with at least one tag is required")
    tags = []
    for i, e in enumerate(entries, 1):
        w = f"{where}.tags[{i}]"
        e = _mapping(e, w)
        _check_keys(e, TAG_KEYS, w)
        name = str(e.get("name") or "").strip()
        if not name or "/" in name or "+" in name or "#" in name:
            raise ConfigError(f"{w}: 'name' is required, without '/', '+' or '#'")
        table = str(e.get("table", "holding")).strip().lower()
        if table not in TABLES:
            raise ConfigError(f"{w}: 'table' must be one of {', '.join(TABLES)}")
        if "address" not in e:
            raise ConfigError(f"{w}: 'address' is required")
        address = _num(e["address"], f"{w} address", int, 0, 65535)
        bit = None if e.get("bit") is None else _num(e["bit"], f"{w} bit", int, 0, 15)
        default_type = "bool" if table in BIT_TABLES or bit is not None else "uint16"
        kind = str(e.get("type", default_type)).strip().lower()
        if kind not in DATA_TYPES:
            raise ConfigError(f"{w}: 'type' must be one of {', '.join(DATA_TYPES)}")
        if (table in BIT_TABLES or bit is not None) != (kind == "bool"):
            raise ConfigError(f"{w}: type bool belongs to coil/discrete or a 'bit' of a register")
        write = _bool(e.get("write", False), f"{w} write")
        if write and table in ("discrete", "input"):
            raise ConfigError(f"{w}: {table} is read only; only coil and holding can be written")
        if write and bit is not None:
            raise ConfigError(f"{w}: a single bit of a register cannot be written")
        scale = _num(e.get("scale", 1), f"{w} scale")
        if scale == 0:
            raise ConfigError(f"{w}: scale may not be 0")
        count = _num(e.get("count", 1), f"{w} count", int, 1, 1000)
        if count > 1 and "{n}" not in name:
            raise ConfigError(f"{w}: 'count' needs '{{n}}' in the name")
        topic = e.get("topic")
        if count > 1 and topic:
            raise ConfigError(f"{w}: 'topic' cannot be combined with 'count'")
        step = 1 if table in BIT_TABLES or bit is not None else DATA_TYPES[kind][0]
        for n in range(1, count + 1):
            tag = Tag(
                name=name.replace("{n}", str(n)), table=table, address=address + (n - 1) * step,
                type=kind, bit=bit,
                unit_id=_num(e.get("unit_id", dev.unit_id), f"{w} unit_id", int, 0, 255),
                topic="", scale=scale, offset=_num(e.get("offset", 0), f"{w} offset"),
                decimals=None if e.get("decimals") is None else _num(e["decimals"], f"{w} decimals", int, 0, 12),
                unit=str(e.get("unit") or ""), write=write,
                min=None if e.get("min") is None else _num(e["min"], f"{w} min"),
                max=None if e.get("max") is None else _num(e["max"], f"{w} max"),
            )
            if tag.address + tag.size - 1 > 65535:
                raise ConfigError(f"{w}: {tag.name} runs past address 65535")
            tag.topic = str(topic).strip() if topic else f"{dev.topic}/{tag.name}"
            tags.append(tag)
    return tags


def load_settings(path: str) -> Settings:
    try:
        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
    except FileNotFoundError:
        raise ConfigError(f"configuration file not found: {path}")
    except yaml.YAMLError as exc:
        raise ConfigError(f"{path}: invalid YAML: {exc}")
    data = _mapping(data, "top level")
    _check_keys(data, {"general", "mqtt", "devices"}, "top level")
    g = _mapping(data.get("general"), "general")
    m = _mapping(data.get("mqtt"), "mqtt")
    s = Settings(
        log_level=str(g.get("log_level", "INFO")).upper(),
        payload=str(g.get("payload", "plain")).strip().lower(),
        republish_s=_num(g.get("republish_s", 0), "general.republish_s", float, 0),
        mqtt_host=str(m.get("host", "127.0.0.1")).strip(),
        mqtt_port=_num(m.get("port", 1883), "mqtt.port", int, 1, 65535),
        mqtt_client_id=str(m.get("client_id") or "").strip() or f"mqtt-modbus-{socket.gethostname()}",
        mqtt_username=str(m.get("username") or "").strip(),
        mqtt_password=str(m.get("password") or ""),
        mqtt_keepalive=_num(m.get("keepalive", 30), "mqtt.keepalive", int, 5, 3600),
        qos=_num(m.get("qos", 0), "mqtt.qos", int, 0, 2),
        retain=_bool(m.get("retain", True), "mqtt.retain"),
        tls=_bool(m.get("tls", False), "mqtt.tls"),
        ca_cert=str(m.get("ca_cert") or "").strip(),
        tls_insecure=_bool(m.get("tls_insecure", False), "mqtt.tls_insecure"),
        status_topic=str(m.get("status_topic", "modbus/status") or "").strip(),
    )
    if s.payload not in ("plain", "json"):
        raise ConfigError("general.payload must be 'plain' or 'json'")

    devices = data.get("devices")
    if not isinstance(devices, list) or not devices:
        raise ConfigError("'devices' must be a list with at least one device")
    names, topics = set(), {}
    for i, d in enumerate(devices, 1):
        w = f"devices[{i}]"
        d = _mapping(d, w)
        _check_keys(d, DEVICE_KEYS, w)
        name = str(d.get("name") or "").strip()
        if not name or "/" in name or name in names:
            raise ConfigError(f"{w}: a unique 'name' without '/' is required")
        names.add(name)
        if not str(d.get("host") or "").strip():
            raise ConfigError(f"{w} ({name}): 'host' is required")
        dev = Device(
            name=name, host=str(d["host"]).strip(),
            port=_num(d.get("port", 502), f"{w} port", int, 1, 65535),
            unit_id=_num(d.get("unit_id", 1), f"{w} unit_id", int, 0, 255),
            timeout=_num(d.get("timeout", 3), f"{w} timeout", float, 0.1, 60),
            interval=_num(d.get("interval", 1), f"{w} interval", float, 0.05, 86400),
            word_order=str(d.get("word_order", "big")).lower(),
            byte_order=str(d.get("byte_order", "big")).lower(),
            max_gap=_num(d.get("max_gap", 10), f"{w} max_gap", int, 0, 100),
            topic=str(d.get("topic") or f"modbus/{name}").strip().rstrip("/"),
        )
        if dev.word_order not in ("big", "little") or dev.byte_order not in ("big", "little"):
            raise ConfigError(f"{w}: word_order and byte_order must be 'big' or 'little'")
        if any(c in dev.topic for c in "+#"):
            raise ConfigError(f"{w}: no wildcards in 'topic'")
        dev.tags = load_tags(dev, d.get("tags"), w)
        seen = set()
        for t in dev.tags:
            if t.name in seen:
                raise ConfigError(f"{w}: tag name '{t.name}' is used twice")
            seen.add(t.name)
            for top in (t.topic, t.topic + "/set") if t.write else (t.topic,):
                if top in topics:
                    raise ConfigError(f"topic {top} is used twice ({topics[top]} and {name}/{t.name})")
                topics[top] = f"{name}/{t.name}"
        s.devices.append(dev)
    return s


# ---------------------------------------------------------------------------
# register conversion
# ---------------------------------------------------------------------------

def _swap(raw: bytes) -> bytes:
    return b"".join(raw[i + 1:i + 2] + raw[i:i + 1] for i in range(0, len(raw), 2))


def decode(words: List[int], kind: str, word_order: str, byte_order: str):
    words = list(words)
    if word_order == "little":
        words.reverse()
    raw = b"".join(struct.pack(">H", w) for w in words)
    if byte_order == "little":
        raw = _swap(raw)
    return struct.unpack(">" + DATA_TYPES[kind][1], raw)[0]


def encode(value, kind: str, word_order: str, byte_order: str) -> List[int]:
    raw = struct.pack(">" + DATA_TYPES[kind][1], value)
    if byte_order == "little":
        raw = _swap(raw)
    words = [struct.unpack(">H", raw[i:i + 2])[0] for i in range(0, len(raw), 2)]
    if word_order == "little":
        words.reverse()
    return words


def int_range(kind: str):
    bits = {"int16": 16, "uint16": 16, "int32": 32, "uint32": 32, "int64": 64, "uint64": 64}[kind]
    return (0, 2 ** bits - 1) if kind.startswith("u") else (-(2 ** (bits - 1)), 2 ** (bits - 1) - 1)


def parse_payload(payload: bytes):
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
    if isinstance(data, dict):
        if "value" not in data:
            raise ValueError("JSON object without a 'value' field")
        data = data["value"]
    if not isinstance(data, (bool, int, float)) or data is None:
        raise ValueError(f"not a number or boolean: {data!r}")
    return data


# ---------------------------------------------------------------------------
# bridge
# ---------------------------------------------------------------------------

@dataclass
class Block:
    unit_id: int
    table: str
    start: int
    count: int
    tags: List[Tag]


def make_blocks(dev: Device) -> List[Block]:
    """Groups tags into as few read requests as possible."""
    blocks: List[Block] = []
    for t in sorted(dev.tags, key=lambda t: (t.unit_id, t.table, t.address)):
        b = blocks[-1] if blocks else None
        end = t.address + t.size
        if (b and b.unit_id == t.unit_id and b.table == t.table
                and t.address <= b.start + b.count + dev.max_gap
                and max(end, b.start + b.count) - b.start <= MAX_BLOCK[t.table]):
            b.count = max(end, b.start + b.count) - b.start
            b.tags.append(t)
        else:
            blocks.append(Block(t.unit_id, t.table, t.address, t.size, [t]))
    return blocks


class DevicePoller:
    def __init__(self, bridge: "Bridge", dev: Device):
        self.bridge, self.dev = bridge, dev
        self.client = ModbusTcpClient(dev.host, port=dev.port, timeout=dev.timeout, retries=1)
        self.lock = threading.Lock()                 # poll thread and MQTT thread share the connection
        self.blocks = make_blocks(dev)
        self.last: Dict[str, Any] = {}
        self.last_sent: Dict[str, float] = {}
        self.online: Optional[bool] = None
        self.writes = {t.topic + "/set": t for t in dev.tags if t.write}
        self.thread = threading.Thread(target=self.run, name=dev.name, daemon=True)

    # --- connection ---

    def connect(self) -> bool:
        if self.client.connected:
            return True
        if self.client.connect():
            log.info("%s: connected to %s:%d", self.dev.name, self.dev.host, self.dev.port)
            return True
        return False

    def set_online(self, online: bool, reason: str = ""):
        if online == self.online:
            return
        self.online = online
        if online:
            log.info("%s: online", self.dev.name)
        else:
            log.warning("%s: offline (%s)", self.dev.name, reason)
        self.bridge.publish(f"{self.dev.topic}/status", "online" if online else "offline", retain=True)

    # --- reading ---

    def read_block(self, b: Block):
        c, kw = self.client, {"slave": b.unit_id}
        fn = {"coil": c.read_coils, "discrete": c.read_discrete_inputs,
              "holding": c.read_holding_registers, "input": c.read_input_registers}[b.table]
        r = fn(b.start, b.count, **kw)
        if r.isError():
            raise IOError(str(r))
        return r.bits[:b.count] if b.table in BIT_TABLES else r.registers

    def value_of(self, t: Tag, b: Block, data) -> Any:
        i = t.address - b.start
        if t.table in BIT_TABLES:
            return bool(data[i])
        if t.bit is not None:
            return bool((data[i] >> t.bit) & 1)
        raw = decode(data[i:i + t.size], t.type, self.dev.word_order, self.dev.byte_order)
        if t.scale == 1 and t.offset == 0 and not t.type.startswith("float"):
            return raw
        value = raw * t.scale + t.offset
        if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
            return None
        return round(value, t.decimals) if t.decimals is not None else round(value, 6)

    def poll(self):
        with self.lock:
            if not self.connect():
                raise ConnectionError(f"cannot connect to {self.dev.host}:{self.dev.port}")
            results = []
            for b in self.blocks:
                try:
                    data = self.read_block(b)
                except Exception as exc:
                    if not self.client.connected:
                        raise ConnectionError(str(exc))
                    names = ", ".join(t.name for t in b.tags)
                    log.warning("%s: reading %s %d..%d (unit %d) failed: %s [%s]", self.dev.name, b.table,
                                b.start, b.start + b.count - 1, b.unit_id, exc, names)
                    continue
                results.append((b, data))
        now = time.monotonic()
        for b, data in results:
            for t in b.tags:
                value = self.value_of(t, b, data)
                if value is None:
                    continue
                rep = self.bridge.s.republish_s
                if self.last.get(t.name, object()) == value and not (rep and now - self.last_sent.get(t.name, 0) >= rep):
                    continue
                self.last[t.name] = value
                self.last_sent[t.name] = now
                self.bridge.publish_value(t, value)

    def run(self):
        while not self.bridge.stop_event.is_set():
            start = time.monotonic()
            try:
                self.poll()
                self.set_online(True)
            except Exception as exc:
                self.set_online(False, str(exc))
                with self.lock:
                    self.client.close()
                self.bridge.stop_event.wait(min(max(self.dev.interval, 1), 10))
                continue
            self.bridge.stop_event.wait(max(0.0, self.dev.interval - (time.monotonic() - start)))
        with self.lock:
            self.client.close()

    # --- writing ---

    def write(self, t: Tag, payload: bytes):
        try:
            value = parse_payload(payload)
        except (ValueError, UnicodeDecodeError) as exc:
            return self.bridge.error(t, str(exc))
        try:
            with self.lock:
                if not self.connect():
                    raise ConnectionError(f"{self.dev.name} not reachable")
                if t.table == "coil":
                    if not isinstance(value, bool) and value not in (0, 1):
                        return self.bridge.error(t, f"expected true/false, got {value!r}")
                    r = self.client.write_coil(t.address, bool(value), slave=t.unit_id)
                    shown = bool(value)
                else:
                    if isinstance(value, bool):
                        value = int(value)
                    if (t.min is not None and value < t.min) or (t.max is not None and value > t.max):
                        return self.bridge.error(t, f"{value} outside {t.min}..{t.max}")
                    raw = (value - t.offset) / t.scale
                    if not t.type.startswith("float"):
                        raw = int(round(raw))
                        lo, hi = int_range(t.type)
                        if not lo <= raw <= hi:
                            return self.bridge.error(t, f"raw value {raw} outside {lo}..{hi} ({t.type})")
                    words = encode(raw, t.type, self.dev.word_order, self.dev.byte_order)
                    if len(words) == 1:
                        r = self.client.write_register(t.address, words[0], slave=t.unit_id)
                    else:
                        r = self.client.write_registers(t.address, words, slave=t.unit_id)
                    shown = f"{value} (raw {raw})"
                if r.isError():
                    raise IOError(str(r))
        except Exception as exc:
            with self.lock:
                if not self.client.connected:
                    self.client.close()
            return self.bridge.error(t, f"write failed: {exc}")
        log.info("%s: %s = %s -> %s %d (unit %d)", self.dev.name, t.name, shown, t.table, t.address, t.unit_id)
        self.last.pop(t.name, None)          # publish the read-back value at the next poll


class Bridge:
    def __init__(self, s: Settings):
        self.s = s
        self.stop_event = threading.Event()
        self.pollers = [DevicePoller(self, d) for d in s.devices]
        self.writes: Dict[str, DevicePoller] = {}
        for p in self.pollers:
            for topic in p.writes:
                self.writes[topic] = p
        self.mqtt = self.create_mqtt()

    def create_mqtt(self) -> mqtt.Client:
        s = self.s
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=s.mqtt_client_id)
        if s.mqtt_username:
            c.username_pw_set(s.mqtt_username, s.mqtt_password or None)
        if s.tls:
            c.tls_set(ca_certs=s.ca_cert or None, cert_reqs=ssl.CERT_REQUIRED)
            c.tls_insecure_set(s.tls_insecure)
        if s.status_topic:
            c.will_set(s.status_topic, "offline", qos=1, retain=True)
        c.reconnect_delay_set(1, 30)
        c.on_connect = self.on_connect
        c.on_message = self.on_message
        c.on_disconnect = lambda *a: None if self.stop_event.is_set() else log.warning("MQTT connection lost: %s", a[3])
        return c

    def publish(self, topic: str, payload: str, retain: bool):
        self.mqtt.publish(topic, payload, qos=self.s.qos, retain=retain)

    def publish_value(self, t: Tag, value):
        if self.s.payload == "json":
            body = {"value": value, "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
            if t.unit:
                body["unit"] = t.unit
            payload = json.dumps(body, ensure_ascii=False)
        else:
            payload = json.dumps(value)
        self.publish(t.topic, payload, retain=self.s.retain)
        log.debug("%s = %s", t.topic, payload)

    def error(self, t: Tag, text: str):
        log.error("%s/set: %s", t.topic, text)
        if self.s.status_topic:
            base = self.s.status_topic.rsplit("/", 1)[0] if "/" in self.s.status_topic else "modbus"
            self.publish(f"{base}/error", json.dumps({"topic": t.topic + "/set", "error": text}), retain=False)

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT broker refused the connection: %s", reason_code)
            return
        log.info("connected to MQTT broker %s:%d as %s", self.s.mqtt_host, self.s.mqtt_port, self.s.mqtt_client_id)
        if self.s.status_topic:
            client.publish(self.s.status_topic, "online", qos=1, retain=True)
        for topic in self.writes:
            client.subscribe(topic, qos=self.s.qos)
        for p in self.pollers:                       # after a reconnect: publish everything again
            p.last.clear()
            if p.online is not None:
                self.publish(f"{p.dev.topic}/status", "online" if p.online else "offline", retain=True)

    def on_message(self, client, userdata, msg):
        p = self.writes.get(msg.topic)
        if p is None or not msg.payload:
            return
        t = p.writes[msg.topic]
        if msg.retain:
            log.warning("ignoring retained command on %s (delete it: mosquitto_pub -r -n -t %s)", msg.topic, msg.topic)
            return
        threading.Thread(target=p.write, args=(t, msg.payload), daemon=True).start()

    def run(self):
        self.mqtt.connect_async(self.s.mqtt_host, self.s.mqtt_port, keepalive=self.s.mqtt_keepalive)
        self.mqtt.loop_start()
        for p in self.pollers:
            p.thread.start()
        try:
            while not self.stop_event.wait(1.0):
                pass
        finally:
            log.info("stopping")
            for p in self.pollers:
                p.thread.join(5)
            if self.s.status_topic and self.mqtt.is_connected():
                self.mqtt.publish(self.s.status_topic, "offline", qos=1, retain=True).wait_for_publish(2)
            self.mqtt.disconnect()
            self.mqtt.loop_stop()


def print_mapping(s: Settings):
    log.info("MQTT broker %s:%d, %d device(s)", s.mqtt_host, s.mqtt_port, len(s.devices))
    for d in s.devices:
        blocks = make_blocks(d)
        log.info("%s: %s:%d, unit %d, every %g s, %d tag(s) in %d request(s), topic %s",
                 d.name, d.host, d.port, d.unit_id, d.interval, len(d.tags), len(blocks), d.topic)
        for t in d.tags:
            where = f"{t.table} {t.address}" + (f".{t.bit}" if t.bit is not None else "")
            log.info("  %-24s %-16s %-8s unit %-3d %s%s", t.name, where, t.type, t.unit_id, t.topic,
                     "  (+ /set)" if t.write else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=DEFAULT_CONFIG, help=f"YAML file (default {DEFAULT_CONFIG})")
    ap.add_argument("--check", action="store_true", help="check the configuration, show the tags and exit")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("pymodbus").setLevel(logging.CRITICAL)
    try:
        s = load_settings(args.config)
    except ConfigError as exc:
        log.error("%s", exc)
        sys.exit(2)
    logging.getLogger().setLevel(getattr(logging, s.log_level, logging.INFO))
    print_mapping(s)
    if args.check:
        return
    bridge = Bridge(s)
    signal.signal(signal.SIGTERM, lambda *_: bridge.stop_event.set())
    signal.signal(signal.SIGINT, lambda *_: bridge.stop_event.set())
    bridge.run()


if __name__ == "__main__":
    main()
