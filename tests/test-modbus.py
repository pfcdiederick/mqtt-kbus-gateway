#!/usr/bin/env python3
"""End-to-end test of docker/mqtt-modbus (needs: pip install pymodbus==3.6.9 paho-mqtt PyYAML).

    tests/test-modbus.py <broker> [--port 1883] [--modbus-port 15020]

Starts a Modbus TCP server (pymodbus) in this process and the bridge with a test
configuration (topics under test-modbus/), and checks: all data types, word order,
bit, scale, count series, change-only publishing, writes (FC5, FC6, FC16), refused
writes, a retained command at start, an unreachable device, clean stop.
A local broker is enough: docker run -d --rm -p 1883:1883 eclipse-mosquitto:2.0.20 \
    mosquitto -c /mosquitto-no-auth.conf
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time

try:
    import paho.mqtt.client as mqtt
    from pymodbus.datastore import ModbusSequentialDataBlock, ModbusServerContext, ModbusSlaveContext
    from pymodbus.server import StartTcpServer
except ImportError as exc:
    sys.exit(f"{exc.name} is missing: pip install pymodbus==3.6.9 paho-mqtt PyYAML")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(ROOT, "docker", "mqtt-modbus", "mqtt_modbus.py")
P = "test-modbus"

CONFIG = """
general: {{log_level: DEBUG}}
mqtt: {{host: {host}, port: {port}, client_id: test-modbus-bridge, status_topic: {p}/status}}
devices:
  - name: dev
    host: 127.0.0.1
    port: {mb_port}
    interval: 0.2
    topic: {p}/dev
    tags:
      - {{name: u16, address: 0}}
      - {{name: i16, address: 1, type: int16}}
      - {{name: f32, address: 2, type: float32}}
      - {{name: f32cdab, address: 4, type: float32, topic: {p}/own/topic}}
      - {{name: i32, address: 6, type: int32, scale: 0.001, decimals: 3}}
      - {{name: f64, address: 8, type: float64}}
      - {{name: bit3, address: 12, bit: 3}}
      - {{name: sp, address: 20, type: int16, scale: 0.1, write: true, min: 0, max: 100}}
      - {{name: big, address: 22, type: uint32, write: true}}
      - {{name: "in{{n}}", count: 2, table: input, address: 0, type: int16}}
      - {{name: "coil{{n}}", count: 2, table: coil, address: 0, write: true}}
      - {{name: di1, table: discrete, address: 5}}
  - name: dead
    host: 127.0.0.1
    port: {dead_port}
    timeout: 0.5
    topic: {p}/dead
    tags:
      - {{name: x, address: 0}}
"""

failures = []


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else ': ' + str(detail)}")
    if not ok:
        failures.append(name)


def wait(cond, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def words(fmt, value, swap_words=False):
    raw = struct.pack(">" + fmt, value)
    w = [struct.unpack(">H", raw[i:i + 2])[0] for i in range(0, len(raw), 2)]
    return w[::-1] if swap_words else w


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("broker")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--modbus-port", type=int, default=15020)
    a = ap.parse_args()

    store = ModbusSlaveContext(di=ModbusSequentialDataBlock(0, [0] * 100), co=ModbusSequentialDataBlock(0, [0] * 100),
                               hr=ModbusSequentialDataBlock(0, [0] * 100), ir=ModbusSequentialDataBlock(0, [0] * 100),
                               zero_mode=True)
    hr = lambda addr, vals: store.setValues(3, addr, vals)
    hr(0, [65535])
    hr(1, words("h", -5))
    hr(2, words("f", 21.5))
    hr(4, words("f", -1.25, swap_words=False))
    hr(6, words("i", -123456))
    hr(8, words("d", 3.14159))
    hr(12, [0b1000])
    store.setValues(4, 0, words("h", 7) + words("h", -8))
    store.setValues(2, 5, [1])
    ctx = ModbusServerContext(slaves=store, single=True)
    threading.Thread(target=StartTcpServer, kwargs={"context": ctx, "address": ("127.0.0.1", a.modbus_port)},
                     daemon=True).start()
    time.sleep(0.5)

    received, counts = {}, {}
    lock = threading.Lock()

    def on_message(c, u, m):
        with lock:
            received[m.topic] = m.payload.decode()
            counts[m.topic] = counts.get(m.topic, 0) + 1

    pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="test-modbus-checker")
    pub.on_message = on_message
    pub.connect(a.broker, a.port)
    pub.loop_start()
    pub.subscribe(f"{P}/#")
    time.sleep(0.3)
    with lock:                                          # forget retained values of an earlier run
        received.clear()
        counts.clear()

    cfg = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
    cfg.write(CONFIG.format(host=a.broker, port=a.port, mb_port=a.modbus_port, dead_port=a.modbus_port + 1, p=P))
    cfg.close()
    pub.publish(f"{P}/dev/sp/set", "50", retain=True)      # a retained command waiting on the broker
    time.sleep(0.2)
    br = subprocess.Popen([sys.executable, BRIDGE, "--config", cfg.name])
    get = lambda t: received.get(f"{P}/{t}")
    try:
        print("Modbus -> MQTT")
        check("bridge status online", wait(lambda: get("status") == "online", 10), get("status"))
        check("device status online", wait(lambda: get("dev/status") == "online"), get("dev/status"))
        check("uint16 = 65535", wait(lambda: get("dev/u16") == "65535"), get("dev/u16"))
        check("int16 = -5", get("dev/i16") == "-5", get("dev/i16"))
        check("float32 = 21.5", get("dev/f32") == "21.5", get("dev/f32"))
        check("own topic: float32 = -1.25", get("own/topic") == "-1.25", get("own/topic"))
        check("int32 x 0.001 = -123.456", get("dev/i32") == "-123.456", get("dev/i32"))
        check("float64 = 3.14159", get("dev/f64") == "3.14159", get("dev/f64"))
        check("bit 3 of register 12 = true", get("dev/bit3") == "true", get("dev/bit3"))
        check("series input in1/in2 = 7/-8", get("dev/in1") == "7" and get("dev/in2") == "-8",
              (get("dev/in1"), get("dev/in2")))
        check("coil1 = false", get("dev/coil1") == "false", get("dev/coil1"))
        check("discrete input = true", get("dev/di1") == "true", get("dev/di1"))
        n = counts.get(f"{P}/dev/u16", 0)
        time.sleep(1.0)
        check("unchanged value not published again (5 polls)", counts.get(f"{P}/dev/u16", 0) == n,
              counts.get(f"{P}/dev/u16"))
        hr(0, [42])
        check("changed register published: u16 = 42", wait(lambda: get("dev/u16") == "42"), get("dev/u16"))
        check("retained command on the broker at start ignored", store.getValues(3, 20, 1) == [0],
              store.getValues(3, 20, 1))
        pub.publish(f"{P}/dev/sp/set", None, retain=True)
        check("unreachable device: status offline", wait(lambda: get("dead/status") == "offline", 6), get("dead/status"))

        print("MQTT -> Modbus")
        pub.publish(f"{P}/dev/sp/set", "21.5")
        check("FC6: sp 21.5 -> raw 215", wait(lambda: store.getValues(3, 20, 1) == [215]), store.getValues(3, 20, 1))
        check("read back: sp = 21.5", wait(lambda: get("dev/sp") == "21.5"), get("dev/sp"))
        pub.publish(f"{P}/dev/big/set", '{"value": 100000}')
        check("FC16: uint32 100000 (JSON payload)", wait(lambda: store.getValues(3, 22, 2) == words("I", 100000)),
              store.getValues(3, 22, 2))
        pub.publish(f"{P}/dev/coil2/set", "on")
        check("FC5: coil2 on", wait(lambda: store.getValues(1, 1, 1) == [True]), store.getValues(1, 1, 1))
        check("read back: coil2 = true", wait(lambda: get("dev/coil2") == "true"), get("dev/coil2"))
        pub.publish(f"{P}/dev/sp/set", "150")
        check("write above max refused (modbus/error)", wait(lambda: "outside" in (get("error") or "")), get("error"))
        check("refused write not executed", store.getValues(3, 20, 1) == [215], store.getValues(3, 20, 1))
        pub.publish(f"{P}/dev/sp/set", "abc")
        check("invalid payload reported", wait(lambda: "not a number" in (get("error") or "")), get("error"))
    finally:
        br.terminate()
        try:
            br.wait(8)
        except subprocess.TimeoutExpired:
            br.kill()
        os.unlink(cfg.name)
    check("bridge stops cleanly and publishes offline",
          br.returncode == 0 and wait(lambda: get("status") == "offline"), (br.returncode, get("status")))
    for t in list(received):                            # remove the retained test topics
        pub.publish(t, None, retain=True)
    time.sleep(0.3)
    pub.loop_stop()
    print(f"\n{'all checks passed' if not failures else str(len(failures)) + ' FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
