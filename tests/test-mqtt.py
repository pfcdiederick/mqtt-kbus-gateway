#!/usr/bin/env python3
"""Tests for kbus-gateway through the MQTT broker (needs: pip install paho-mqtt).

    tests/test-mqtt.py <broker> sim               simulated node (make run-sim): everything, writes included
    tests/test-mqtt.py <broker> node              real node, READ ONLY: topics, values, modules
    tests/test-mqtt.py <broker> write --do 1      real node: switches DO1 on and off (check what is connected!)
    tests/test-mqtt.py <broker> serial --gateway build/kbus-gateway
                                                  COM1 on a virtual serial port (pty, on the PC): starts the
                                                  gateway itself (simulation), several times
    tests/test-mqtt.py <broker> watchdog --gateway build/kbus-gateway
                                                  write watchdog (MQTT_WATCHDOG_S=2): starts the gateway itself

Options: --port 1883, --prefix kbus, --user, --password.
The simulation node loops DO1..8 back to DI1..8 and AO1 back to AI4.
"""
import argparse
import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time
import tty

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is missing: pip install paho-mqtt")


class Broker:
    """Subscribes to <prefix>/#, keeps the last payload of every topic."""

    def __init__(self, a):
        self.prefix = a.prefix
        self.values, self.errors, self.rx, self.responses = {}, [], [], []
        self.cond = threading.Condition()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"kbus-gateway-test-{time.time_ns()}")
        if a.user:
            self.c.username_pw_set(a.user, a.password)
        self.c.on_message = self._on_message
        self.c.connect(a.broker, a.port, 30)
        self.c.subscribe(f"{a.prefix}/#")
        self.c.loop_start()

    def _on_message(self, _c, _u, msg):
        with self.cond:
            t = msg.topic[len(self.prefix) + 1:]
            if t == "serial/com1/rx":                     # binary, one message per frame
                self.rx.append(bytes(msg.payload))
                self.cond.notify_all()
                return
            try:
                p = msg.payload.decode()
            except UnicodeDecodeError:                    # binary (module/<pos>/in)
                p = msg.payload.decode("latin-1")
            if t.endswith("/stepper/response"):
                self.responses.append(json.loads(p))
            if t == "error":
                self.errors.append(json.loads(p))
            elif p == "":
                self.values.pop(t, None)
            else:
                self.values[t] = p
            self.cond.notify_all()

    def wait(self, topic, pred=lambda v: v is not None, timeout=3.0):
        """Wait until the payload of <prefix>/<topic> fulfils pred; returns it."""
        end = time.monotonic() + timeout
        with self.cond:
            while not pred(self.values.get(topic)):
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"{topic}: got {self.values.get(topic)!r}")
                self.cond.wait(left)
            return self.values.get(topic)

    def wait_error(self, topic, timeout=3.0):
        end = time.monotonic() + timeout
        full = f"{self.prefix}/{topic}"
        with self.cond:
            while not any(e["topic"] == full for e in self.errors):
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"no error for {topic}")
                self.cond.wait(left)
            return [e for e in self.errors if e["topic"] == full][-1]["error"]

    def wait_response(self, pred, timeout=5.0):
        """Wait for a stepper response that fulfils pred; returns it."""
        end = time.monotonic() + timeout
        with self.cond:
            while True:
                for r in self.responses:
                    if pred(r):
                        return r
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"no such stepper response: {self.responses!r}")
                self.cond.wait(left)

    def state(self, topic):
        v = self.values.get(topic)
        return json.loads(v) if v else {}

    def wait_state(self, topic, pred, timeout=5.0):
        return json.loads(self.wait(topic, lambda v: v is not None and pred(json.loads(v)), timeout))

    def wait_rx(self, count, timeout=3.0):
        """Wait until <prefix>/serial/com1/rx has brought count frames; returns them."""
        end = time.monotonic() + timeout
        with self.cond:
            while len(self.rx) < count:
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"serial/com1/rx: {len(self.rx)} of {count} frames: {self.rx!r}")
                self.cond.wait(left)
            return list(self.rx)

    def set(self, topic, payload, retain=False):
        self.c.publish(f"{self.prefix}/{topic}", payload, retain=retain).wait_for_publish()


PASSED = []


def check(name, fn):
    fn()
    PASSED.append(name)
    print(f"  ok  {name}")


def read_only(b):
    check("status online", lambda: b.wait("status", lambda v: v == "online"))
    mods = json.loads(b.wait("modules"))
    print(f"      {len(mods)} modules: " + ", ".join(m["name"] for m in mods))
    count = {k: sum(len(m["channels"].get(k, [])) for m in mods) for k in ("di", "do", "ai", "ao")}
    print(f"      channels: {count}")
    for k, n in count.items():
        if n:
            check(f"{k}/1..{n} present", lambda k=k, n=n: [b.wait(f"{k}/{i}") for i in range(1, n + 1)])
            check(f"{k}/all has {n} values", lambda k=k, n=n: b.wait(f"{k}/all", lambda v: v and len(json.loads(v)) == n))
            check(f"{k}/{n + 1} absent", lambda k=k, n=n: b.values.get(f"{k}/{n + 1}") is None or (_ for _ in ()).throw(AssertionError("present")))
    check("bus/healthy", lambda: b.wait("bus/healthy", lambda v: v in ("true", "false")))
    check("bus/cycletime", lambda: int(b.wait("bus/cycletime")))
    check("bus/configversion", lambda: int(b.wait("bus/configversion")))
    check("bus/statistics", lambda: json.loads(b.wait("bus/statistics"))["cycles"] >= 0)
    return count


def sim(b):
    read_only(b)
    for v in ("true", "false"):
        b.set("do/3/set", v)
        check(f"do/3/set {v} -> do/3, di/3 (loopback)",
              lambda v=v: (b.wait("do/3", lambda x: x == v), b.wait("di/3", lambda x: x == v)))
    for p, want in (("ON", "true"), ("0", "false"), ('"true"', "true"), ("off", "false")):
        b.set("do/2/set", p)
        check(f"do/2/set {p} -> {want}", lambda want=want: b.wait("do/2", lambda x: x == want))
    # 750-450 (simulation position 3): temperatures in degC next to the raw AI channels
    mods = json.loads(b.wait("modules"))
    rtd = next(m for m in mods if m["typeCode"] == 450)
    check("modules: 750-450 has the temperature function",
          lambda: rtd.get("functions", {}).get("temperature", {}).get("unit") == "°C" or (_ for _ in ()).throw(AssertionError(rtd)))
    pos = rtd["position"]
    for ch, want in ((1, "21.4"), (2, "-12.5"), (3, "850.0")):
        check(f"module/{pos}/temperature/{ch} = {want}", lambda ch=ch, w=want: b.wait(f"module/{pos}/temperature/{ch}", lambda v: v == w))
    b.set("ao/2/set", str(65536 - 2005))                  # AI8 = AO2: -200.5 degC (two's complement)
    check(f"module/{pos}/temperature/4 follows AO2: -200.5", lambda: b.wait(f"module/{pos}/temperature/4", lambda v: v == "-200.5"))
    check("raw AI channel stays raw", lambda: b.wait(f"ai/{rtd['channels']['ai'][3]}", lambda v: v == str(65536 - 2005)))
    b.set("ao/2/set", "0")
    registers(b, mods)
    stepper(b, mods)
    b.set("ao/1/set", "12345")
    check("ao/1/set 12345 -> ao/1, ai/4 (loopback)",
          lambda: (b.wait("ao/1", lambda x: x == "12345"), b.wait("ai/4", lambda x: x == "12345")))
    b.set("do/all/set", "[true, null, true]")
    check("do/all/set [true,null,true]",
          lambda: b.wait("do/all", lambda x: x and json.loads(x)[:3] == [True, False, True]))
    b.set("do/all/set", "[false,false,false]")
    b.wait("do/1", lambda x: x == "false")
    b.set("ao/all/set", "[0, 65535]")
    check("ao/all/set [0,65535]", lambda: b.wait("ao/all", lambda x: x and json.loads(x)[:2] == [0, 65535]))

    for topic, payload, part in (("do/1/set", "maybe", "true or false"), ("ao/1/set", "70000", "0..65535"),
                                 ("do/99/set", "true", "only"),
                                 ("do/all/set", "true", "JSON array"), ("bus/cycletime/set", "100", "1..40"),
                                 ("bus/foo/set", "1", "unknown")):
        b.set(topic, payload)
        check(f"{topic} {payload} rejected", lambda t=topic, p=part: p in b.wait_error(t) or (_ for _ in ()).throw(AssertionError(b.wait_error(t))))

    b.set("bus/cycletime/set", "8")
    check("bus/cycletime/set 8", lambda: b.wait("bus/cycletime", lambda x: x == "8"))
    b.set("bus/cycletime/set", "5")
    b.wait("bus/cycletime", lambda x: x == "5")
    ver = int(b.wait("bus/configversion"))
    b.set("bus/rescan/set", "true")
    check("bus/rescan/set -> configversion + 1", lambda: b.wait("bus/configversion", lambda x: x and int(x) == ver + 1, 5))
    # after a rescan all outputs are 0 again
    check("outputs 0 after the rescan", lambda: b.wait("do/all", lambda x: x and not any(json.loads(x))))


def reg_get(b, pos, payload, rid, timeout=3.0):
    """module/<pos>/reg/get, returns the answer (module/<pos>/reg/value) with this id."""
    b.set(f"module/{pos}/reg/get", payload)
    return json.loads(b.wait(f"module/{pos}/reg/value",
                             lambda v: v is not None and json.loads(v).get("id") == rid, timeout))


def registers(b, mods):
    """Register reads (simulation: register 6 of the 750-450 = 96 at >= 850.0 degC, 8 = type)."""
    rtd = next(m for m in mods if m["typeCode"] == 450)
    pos, ai = rtd["position"], rtd["channels"]["ai"]
    others = [n for m in mods if m["typeCode"] != 450 for n in m["channels"].get("ai", [])]
    check(f"ai/{ai[2]}/status = 96 (850.0 degC)", lambda: b.wait(f"ai/{ai[2]}/status", lambda v: v == "96", 5))
    for n in (ai[0], ai[1], ai[3], *others):
        check(f"ai/{n}/status = 0", lambda n=n: b.wait(f"ai/{n}/status", lambda v: v == "0", 5))
    check(f"ai/{len(ai) + len(others) + 1}/status absent",
          lambda: b.values.get(f"ai/{len(ai) + len(others) + 1}/status") is None or (_ for _ in ()).throw(AssertionError("present")))
    b.set("ao/2/set", "8500")                             # AI8 = AO2: 850.0 degC -> status 96 (read once per second)
    check(f"ai/{ai[3]}/status follows: 96", lambda: b.wait(f"ai/{ai[3]}/status", lambda v: v == "96", 4))
    b.set("ao/2/set", "0")
    check(f"ai/{ai[3]}/status back to 0", lambda: b.wait(f"ai/{ai[3]}/status", lambda v: v == "0", 4))

    r = reg_get(b, pos, '{"table":2,"register":6,"id":"x"}', "x")
    check("reg/get table 2 register 6", lambda: r == {"table": 2, "register": 6, "value": 96, "ok": True, "id": "x"} or (_ for _ in ()).throw(AssertionError(r)))
    r = reg_get(b, pos, '{"table":0,"register":8,"id":7}', 7)
    check("reg/get register 8 = 450, numeric id", lambda: r["ok"] and r["value"] == 450 or (_ for _ in ()).throw(AssertionError(r)))
    digital = next(m["position"] for m in mods if m["typeCode"] & 0x8000)
    for p_, payload, part in ((pos, '{"table":9,"register":6,"id":"e1"}', "read failed"),
                              (pos, '{"table":0,"register":64,"id":"e2"}', "register"),
                              (pos, '{"register":6,"id":"e3"}', "table"),
                              (pos, 'nonsense', "table"),
                              (digital, '{"table":0,"register":6,"id":"e4"}', "digital"),
                              (99, '{"table":0,"register":6,"id":"e5"}', "no module")):
        rid = json.loads(payload).get("id") if payload.startswith("{") else None
        if rid is None:
            b.set(f"module/{p_}/reg/get", payload)
            r = json.loads(b.wait(f"module/{p_}/reg/value", lambda v: v is not None and "id" not in json.loads(v)))
        else:
            r = reg_get(b, p_, payload, rid)
        check(f"reg/get module {p_} {payload}: ok false, {part}",
              lambda r=r, part=part: (not r["ok"] and part in r["error"]) or (_ for _ in ()).throw(AssertionError(r)))
    # many requests at once: each one answered (queued) or refused as busy, none lost
    ids = [f"q{i}" for i in range(24)]
    for i in ids:
        b.set(f"module/{pos}/reg/get", json.dumps({"table": 0, "register": 8, "id": i}))
    got = {}
    end = time.monotonic() + 5

    def collect():
        while len(got) < len(ids) and time.monotonic() < end:
            with b.cond:
                b.cond.wait(0.1)
        missing = set(ids) - set(got)
        if missing:
            raise AssertionError(f"no answer for {sorted(missing)}")
    seen = b.c.on_message

    def on(c, u, msg):
        seen(c, u, msg)
        if msg.topic.endswith(f"module/{pos}/reg/value"):
            j = json.loads(msg.payload)
            if str(j.get("id", "")).startswith("q"):
                got[j["id"]] = j
    b.c.on_message = on
    try:
        check("24 requests at once: all answered (ok or busy)", collect)
    finally:
        b.c.on_message = seen
    check("  queued ones ok, the rest busy",
          lambda: all(j["ok"] or "busy" in j["error"] for j in got.values()) and any(j["ok"] for j in got.values())
          or (_ for _ in ()).throw(AssertionError(got)))


class Gateway:
    """The gateway in simulation with COM1 on the slave side of a pty (serial mode)."""

    def __init__(self, a, store):
        self.a, self.store, self.p = a, store, None
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)                            # no echo before the gateway sets raw itself
        self.device = os.ttyname(self.slave)

    def start(self, device=None, extra=None):
        env = dict(os.environ, KBUS_SIMULATION="1", KBUS_PRIORITY="0", KBUS_SETTINGS_STORE=self.store,
                   SERIAL_DEVICE=self.device if device is None else device, MQTT_HOST=self.a.broker,
                   MQTT_PORT=str(self.a.port), MQTT_TOPIC_PREFIX=self.a.prefix,
                   MQTT_CLIENT_ID=f"kbus-gateway-serialtest-{os.getpid()}", **(extra or {}))
        self.p = subprocess.Popen([self.a.gateway], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def stop(self):
        if self.p:
            self.p.send_signal(signal.SIGTERM)
            self.p.wait(5)
            self.p = None

    def read(self, n, timeout=2.0):
        """Bytes the gateway sent on COM1 (master side of the pty)."""
        out, end = b"", time.monotonic() + timeout
        while len(out) < n and time.monotonic() < end:
            if select.select([self.master], [], [], 0.1)[0]:
                out += os.read(self.master, 4096)
        return out

    def send(self, data):
        """Bytes the device sends to the gateway."""
        os.write(self.master, data)


SERIAL_TOPICS = ("open", "baudrate", "format", "framegap", "statistics")


def serial(b, a):
    fd, store = tempfile.mkstemp(prefix="kbus-settings-")
    os.close(fd)
    os.unlink(store)                                      # nothing stored at the start
    g = Gateway(a, store)
    try:
        g.start()
        check("status online", lambda: b.wait("status", lambda v: v == "online", 5))
        check("serial/com1/open true", lambda: b.wait("serial/com1/open", lambda v: v == "true"))
        for t, want in (("baudrate", "19200"), ("format", "8E1"), ("framegap", "10")):
            check(f"serial/com1/{t} {want}", lambda t=t, w=want: b.wait(f"serial/com1/{t}", lambda v: v == w))

        data = b"\x00\x01AB\xff\r\n\x7f\x80"
        b.set("serial/com1/tx/set", data)
        check("tx/set binary arrives byte for byte", lambda: g.read(len(data)) == data or (_ for _ in ()).throw(AssertionError("bytes differ")))

        b.rx.clear()
        g.send(b"\x01\x03\x00\x00")
        time.sleep(0.1)
        g.send(b"\xff\x00second")
        check("two bursts with a pause -> two rx messages",
              lambda: b.wait_rx(2) == [b"\x01\x03\x00\x00", b"\xff\x00second"] or (_ for _ in ()).throw(AssertionError(b.rx)))

        b.rx.clear()
        frames = [bytes([i]) + f"frame {i:02d}".encode() for i in range(20)]
        for f in frames:
            g.send(f)
            time.sleep(0.025)                              # > frame gap 10 ms, several frames per 20 ms tick
        check("20 frames in quick succession all arrive", lambda: b.wait_rx(20) == frames or (_ for _ in ()).throw(AssertionError(b.rx)))
        time.sleep(1.2)                                   # statistics: at most once per second
        check("statistics count bytes and frames", lambda: b.wait("serial/com1/statistics",
              lambda v: v and (lambda j: j["txBytes"] == len(data) and j["frames"] == 22 and j["dropped"] == 0)(json.loads(v))))

        for t, payload, want in (("baudrate", "9600", "9600"), ("format", "8n1", "8N1"), ("framegap", "20", "20")):
            b.set(f"serial/com1/{t}/set", payload)
            check(f"{t}/set {payload} -> {want}", lambda t=t, w=want: b.wait(f"serial/com1/{t}", lambda v: v == w))
        check("settings stored", lambda: all(l in open(store).read().split("\n")
              for l in ("SERIAL_BAUD=9600", "SERIAL_FORMAT=8N1", "SERIAL_FRAME_GAP_MS=20")) or (_ for _ in ()).throw(AssertionError(open(store).read())))

        for t, payload, part in (("baudrate", "1234", "baud rate"), ("baudrate", "fast", "baud rate"),
                                 ("format", "9X3", "format"), ("format", "8E", "format"),
                                 ("framegap", "0", "1..1000"), ("framegap", "1001", "1..1000"),
                                 ("tx", b"", "1..512"), ("tx", b"x" * 513, "1..512"), ("foo", "1", "unknown")):
            b.errors.clear()
            b.set(f"serial/com1/{t}/set", payload)
            check(f"{t}/set {str(payload)[:12]} rejected",
                  lambda t=t, p=part: p in b.wait_error(f"serial/com1/{t}/set") or (_ for _ in ()).throw(AssertionError(b.errors)))
        check("rejected values changed nothing", lambda: (b.values.get("serial/com1/baudrate"), b.values.get("serial/com1/format"),
              b.values.get("serial/com1/framegap")) == ("9600", "8N1", "20") or (_ for _ in ()).throw(AssertionError(b.values)))

        # restart: the stored settings apply (retained topics deleted first, so they must be published
        # anew); a retained baudrate/set waiting on the broker is ignored
        g.stop()
        for t in SERIAL_TOPICS:
            b.set(f"serial/com1/{t}", "", retain=True)
        b.wait("serial/com1/baudrate", lambda v: v is None)
        b.set("serial/com1/baudrate/set", "4800", retain=True)
        g.start()
        for t, want in (("baudrate", "9600"), ("format", "8N1"), ("framegap", "20")):
            check(f"after a restart: {t} {want}", lambda t=t, w=want: b.wait(f"serial/com1/{t}", lambda v: v == w, 5))
        time.sleep(0.5)
        check("retained baudrate/set ignored", lambda: b.values.get("serial/com1/baudrate") == "9600" or (_ for _ in ()).throw(AssertionError(b.values.get("serial/com1/baudrate"))))
        b.set("serial/com1/baudrate/set", "", retain=True)  # delete the retained command again

        # COM1 off: the retained serial topics are deleted, tx/set is not subscribed
        g.stop()
        g.start(device="")
        b.wait("status", lambda v: v == "online", 5)
        check("SERIAL_DEVICE='': no serial topics", lambda: [b.wait(f"serial/com1/{t}", lambda v: v is None, 5) for t in SERIAL_TOPICS])
        b.errors.clear()
        b.set("serial/com1/tx/set", b"x")
        time.sleep(0.5)
        check("SERIAL_DEVICE='': tx/set not handled", lambda: not b.errors or (_ for _ in ()).throw(AssertionError(b.errors)))
    finally:
        g.stop()
        if os.path.exists(store):
            os.unlink(store)


def stepper(b, mods):
    """750-671 driver against the simulated stepper (simulation position 4)."""
    m = next(x for x in mods if x["typeCode"] == 671)
    check("modules: 750-671 has the stepper driver", lambda: m.get("driver") == "stepper" or (_ for _ in ()).throw(AssertionError(m)))
    pos = m["position"]
    st, cmd = f"module/{pos}/stepper/state", f"module/{pos}/stepper/cmd/set"
    check("stepper state retained", lambda: b.wait_state(st, lambda j: "ready" in j and "position" in j))
    b.set(f"module/{pos}/out/set", bytes(12))
    check("raw out/set refused (driver)", lambda: "driver" in b.wait_error(f"module/{pos}/out/set"))
    b.responses.clear()
    b.set(cmd, '{"cmd":"enable","id":"e1"}')
    check("enable -> done, ready, reset acknowledged",
          lambda: (b.wait_response(lambda r: r.get("id") == "e1" and r["result"] == "done"),
                   b.wait_state(st, lambda j: j["ready"] and j["stopAck"] and not j["reset"])))
    for payload, part in (('{"cmd":"move","position":100,"velocity":0,"acceleration":10}', "velocity"),
                          ('{"cmd":"move","position":100,"velocity":10,"acceleration":0}', "acceleration"),
                          ('{"cmd":"move","velocity":10,"acceleration":10}', "position"),
                          ('{"cmd":"reference","direction":"up"}', "direction"),
                          ('{"cmd":"fly"}', "unknown"), ('enable', "JSON")):
        b.errors.clear()
        b.set(cmd, payload)
        check(f"stepper {payload[:40]} rejected", lambda p=part: p in b.wait_error(cmd) or (_ for _ in ()).throw(AssertionError(b.errors)))
    b.set(cmd, '{"cmd":"move","position":2000,"velocity":10000,"acceleration":1000,"id":"m1"}')
    check("move 2000 -> accepted, then done",
          lambda: (b.wait_response(lambda r: r.get("id") == "m1" and r["result"] == "accepted"),
                   b.wait_response(lambda r: r.get("id") == "m1" and r["result"] == "done")))
    check("state: position 2000, on target, positioning",
          lambda: b.wait_state(st, lambda j: j["position"] == 2000 and j["onTarget"] and j["mode"] == "positioning"))
    b.set(cmd, '{"cmd":"move","position":-30000,"velocity":2000,"acceleration":1000,"id":"m2"}')
    b.wait_response(lambda r: r.get("id") == "m2" and r["result"] == "accepted")
    check("moving: busy", lambda: b.wait_state(st, lambda j: j["busy"]))
    b.set(cmd, '{"cmd":"stop","id":"s1"}')
    check("stop interrupts the move: move error, stop done, standstill",
          lambda: (b.wait_response(lambda r: r.get("id") == "m2" and r["result"] == "error"),
                   b.wait_response(lambda r: r.get("id") == "s1" and r["result"] == "done"),
                   b.wait_state(st, lambda j: j["standstill"] and j["mode"] == "none" and -30000 < j["position"] < 2000)))
    b.set(cmd, '{"cmd":"reference","direction":"neg","id":"r1"}')
    check("reference -> done, referenced, position 0",
          lambda: (b.wait_response(lambda r: r.get("id") == "r1" and r["result"] == "done"),
                   b.wait_state(st, lambda j: j["referenced"] and j["position"] == 0)))
    b.set(cmd, '{"cmd":"move","position":500,"velocity":0,"acceleration":5}')     # rejected before the module
    b.set(cmd, '{"cmd":"disable","id":"d1"}')
    check("disable -> done, not ready", lambda: (b.wait_response(lambda r: r.get("id") == "d1" and r["result"] == "done"),
                                                 b.wait_state(st, lambda j: not j["ready"] and not j["enabled"])))
    b.errors.clear()
    b.set(cmd, '{"cmd":"move","position":5,"velocity":5,"acceleration":5}')
    check("move while disabled rejected", lambda: "enable" in b.wait_error(cmd))


def watchdog(b, a):
    # retained topics of an earlier, different node: removed at the start, real ones kept
    stale = ("module/9/temperature/1", "module/9/stepper/state", "module/9/in", "module/3/temperature/7", "module/1/in",
             "ai/9/status")
    for t in stale + ("ai/7/status",):
        b.set(t, "old", retain=True)
    b.wait("module/9/in", lambda v: v == "old")
    g = Gateway(a, "")
    g.start(device="", extra={"MQTT_WATCHDOG_S": "2"})
    try:
        check("status online", lambda: b.wait("status", lambda v: v == "online", 5))
        check("old module topics of another node removed", lambda: [b.wait(t, lambda v: v is None, 5) for t in stale])
        check("ai/7/status of the earlier run replaced", lambda: b.wait("ai/7/status", lambda v: v == "96", 5))
        check("current module topics kept", lambda: (b.wait("module/3/temperature/1", lambda v: v == "21.4"),
                                                      b.wait_state("module/4/stepper/state", lambda j: "ready" in j)))
        check("watchdog ok before the first write", lambda: b.wait("watchdog", lambda v: v == "ok"))
        time.sleep(3)
        check("not armed without a write: still ok", lambda: b.values.get("watchdog") == "ok" or (_ for _ in ()).throw(AssertionError(b.values.get("watchdog"))))
        b.set("do/1/set", "true")
        b.set("ao/1/set", "1000")
        check("do/1 on, ao/1 1000", lambda: (b.wait("do/1", lambda v: v == "true"), b.wait("ao/1", lambda v: v == "1000")))
        for _ in range(4):                                # heartbeats keep it alive
            time.sleep(1)
            b.set("watchdog/set", "1")
        check("heartbeats: outputs kept", lambda: b.values.get("do/1") == "true" and b.values.get("watchdog") == "ok" or (_ for _ in ()).throw(AssertionError(b.values)))
        check("no heartbeat for 2 s: expired", lambda: b.wait("watchdog", lambda v: v == "expired", 4))
        check("outputs 0 (safe state)", lambda: (b.wait("do/1", lambda v: v == "false"), b.wait("ao/1", lambda v: v == "0")))
        b.set("do/2/set", "true")
        check("a write: ok again and written", lambda: (b.wait("watchdog", lambda v: v == "ok"), b.wait("do/2", lambda v: v == "true")))
        # stepper (simulation position 4): the watchdog stops it with a ramp, Enable stays on
        st, cmd = "module/4/stepper/state", "module/4/stepper/cmd/set"
        b.responses.clear()
        b.set(cmd, '{"cmd":"enable","id":"e"}')
        b.wait_response(lambda r: r.get("id") == "e" and r["result"] == "done")
        b.set(cmd, '{"cmd":"move","position":-300000,"velocity":1000,"acceleration":100,"id":"m"}')
        check("stepper moving", lambda: b.wait_state(st, lambda j: j["busy"]))
        check("watchdog: move ends with an error", lambda: b.wait_response(lambda r: r.get("id") == "m" and r["result"] == "error", 5))
        check("stepper: controlled stop, still enabled", lambda: b.wait_state(st, lambda j: j["standstill"] and j["mode"] == "none" and j["enabled"]))
    finally:
        g.stop()


def write(b, ch):
    read_only(b)
    for v in ("true", "false"):
        print(f"      DO{ch} = {v}")
        b.set(f"do/{ch}/set", v)
        check(f"do/{ch}/set {v}", lambda v=v: b.wait(f"do/{ch}", lambda x: x == v))
        time.sleep(2)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("broker")
    p.add_argument("mode", choices=("sim", "node", "write", "serial", "watchdog"))
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--prefix", default="kbus")
    p.add_argument("--user")
    p.add_argument("--password")
    p.add_argument("--do", type=int, default=1, help="write mode: the DO channel to switch")
    p.add_argument("--gateway", default="build/kbus-gateway", help="serial/watchdog mode: the gateway program to start")
    a = p.parse_args()
    b = Broker(a)
    try:
        {"sim": lambda: sim(b), "node": lambda: read_only(b), "write": lambda: write(b, a.do),
         "serial": lambda: serial(b, a), "watchdog": lambda: watchdog(b, a)}[a.mode]()
    except AssertionError as e:
        print(f"FAIL {e}")
        return 1
    print(f"{len(PASSED)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
