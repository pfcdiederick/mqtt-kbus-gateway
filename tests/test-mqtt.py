#!/usr/bin/env python3
"""Tests for kbus-gateway through the MQTT broker (needs: pip install paho-mqtt).

    tests/test-mqtt.py <broker> sim               simulated node (make run-sim): everything, writes included
    tests/test-mqtt.py <broker> node              real node, READ ONLY: topics, values, modules
    tests/test-mqtt.py <broker> write --do 1      real node: switches DO1 on and off (check what is connected!)

Options: --port 1883, --prefix kbus, --user, --password.
The simulation node loops DO1..8 back to DI1..8 and AO1 back to AI4.
"""
import argparse
import json
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is missing: pip install paho-mqtt")


class Broker:
    """Subscribes to <prefix>/#, keeps the last payload of every topic."""

    def __init__(self, a):
        self.prefix = a.prefix
        self.values, self.errors = {}, []
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
            p = msg.payload.decode()
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
    p.add_argument("mode", choices=("sim", "node", "write"))
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--prefix", default="kbus")
    p.add_argument("--user")
    p.add_argument("--password")
    p.add_argument("--do", type=int, default=1, help="write mode: the DO channel to switch")
    a = p.parse_args()
    b = Broker(a)
    try:
        {"sim": lambda: sim(b), "node": lambda: read_only(b), "write": lambda: write(b, a.do)}[a.mode]()
    except AssertionError as e:
        print(f"FAIL {e}")
        return 1
    print(f"{len(PASSED)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
