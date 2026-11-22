#!/usr/bin/env python3
"""End-to-end test of docker/mqtt-iec104 (needs: pip install c104 paho-mqtt PyYAML).

    tests/test-iec104.py <broker> [--port 1883] [--iec-port 24040]

Starts the gateway itself with a test configuration (prefix test104/, so it does not
touch a real node), plays kbus-gateway on MQTT and checks with an IEC 104 client:
values and quality MQTT -> IEC 104, commands IEC 104 -> MQTT, refusals, availability.
A local broker is enough: docker run -d --rm -p 1883:1883 eclipse-mosquitto:2.0.20 \
    mosquitto -c /mosquitto-no-auth.conf
"""
import argparse
import os
import subprocess
import sys
import tempfile
import threading
import time

try:
    import c104
    import paho.mqtt.client as mqtt
except ImportError as exc:
    sys.exit(f"{exc.name} is missing: pip install c104 paho-mqtt PyYAML")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATEWAY = os.path.join(ROOT, "docker", "mqtt-iec104", "mqtt_iec104.py")
P = "test104"

CONFIG = """
general: {{log_level: DEBUG}}
mqtt:
  host: {host}
  port: {port}
  client_id: test-iec104-gateway
  status_topic: {p}/iec104/status
  availability: {{topic: {p}/status, online: online}}
iec104: {{ip: 127.0.0.1, port: {iec_port}, common_address: 7}}
points:
  - {{topic: "{p}/di/{{n}}", count: 2, ioa: 101, type: single}}
  - {{topic: "{p}/ai/1", ioa: 301, type: float, scale: 0.5}}
  - {{topic: "{p}/ai/2", ioa: 302, type: scaled}}
  - {{topic: "{p}/stepper/state", ioa: 401, type: float, key: position}}
commands:
  - {{topic: "{p}/do/{{n}}/set", count: 2, ioa: 1101, type: single}}
  - {{topic: "{p}/ao/1/set", ioa: 1201, type: float, scale: 10, integer: true, min: 0, max: 1000}}
  - {{topic: "{p}/ao/2/set", ioa: 1202, type: scaled}}
"""

failures = []


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{'' if ok else ': ' + str(detail)}")
    if not ok:
        failures.append(name)


def wait(cond, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("broker")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--iec-port", type=int, default=24040)
    a = ap.parse_args()

    received = {}
    lock = threading.Lock()

    def on_message(c, u, m):
        with lock:
            received[m.topic] = m.payload.decode()

    pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="test-iec104-kbus")
    pub.on_message = on_message
    pub.connect(a.broker, a.port)
    pub.loop_start()
    pub.subscribe(f"{P}/#")
    for t in ("status", "di/1", "di/2", "ai/1", "ai/2", "stepper/state", "iec104/status"):
        pub.publish(f"{P}/{t}", None, retain=True)          # clean up an earlier run
    time.sleep(0.3)
    pub.publish(f"{P}/status", "online", retain=True)
    pub.publish(f"{P}/di/1", "true", retain=True)            # retained before the gateway starts

    cfg = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
    cfg.write(CONFIG.format(host=a.broker, port=a.port, iec_port=a.iec_port, p=P))
    cfg.close()
    gw = subprocess.Popen([sys.executable, GATEWAY, "--config", cfg.name])
    try:
        print("gateway started, waiting for MQTT and the IEC 104 server")
        check("gateway publishes status online", wait(lambda: received.get(f"{P}/iec104/status") == "online", 10),
              received.get(f"{P}/iec104/status"))

        client = c104.Client()
        conn = client.add_connection(ip="127.0.0.1", port=a.iec_port, init=c104.Init.ALL)
        st = conn.add_station(common_address=7)
        di1 = st.add_point(io_address=101, type=c104.Type.M_SP_TB_1)
        di2 = st.add_point(io_address=102, type=c104.Type.M_SP_TB_1)
        ai1 = st.add_point(io_address=301, type=c104.Type.M_ME_TF_1)
        ai2 = st.add_point(io_address=302, type=c104.Type.M_ME_TE_1)
        pos = st.add_point(io_address=401, type=c104.Type.M_ME_TF_1)
        do1 = st.add_point(io_address=1101, type=c104.Type.C_SC_NA_1)
        ao1 = st.add_point(io_address=1201, type=c104.Type.C_SE_NC_1)
        ao2 = st.add_point(io_address=1202, type=c104.Type.C_SE_NB_1)
        client.start()
        check("IEC 104 client connected", wait(lambda: conn.is_connected, 10))

        print("MQTT -> IEC 104")
        check("retained value via interrogation: IOA 101 = true, good",
              wait(lambda: di1.value is True and di1.quality.is_good()), (di1.value, di1.quality))
        check("no value yet: IOA 102 Invalid", di2.quality == c104.Quality.Invalid, di2.quality)
        pub.publish(f"{P}/di/2", "on")
        check("spontaneous single point: IOA 102 = true", wait(lambda: di2.value is True and di2.quality.is_good()),
              (di2.value, di2.quality))
        pub.publish(f"{P}/ai/1", "1000")
        check("float with scale 0.5: IOA 301 = 500", wait(lambda: abs(ai1.value - 500) < 1e-6), ai1.value)
        pub.publish(f"{P}/ai/2", "40000")
        check("scaled out of range: IOA 302 = 32767, Overflow",
              wait(lambda: int(ai2.value) == 32767 and not ai2.quality.is_good()), (ai2.value, ai2.quality))
        pub.publish(f"{P}/stepper/state", '{"enabled":true,"position":2000,"velocity":0}')
        check("JSON key 'position': IOA 401 = 2000", wait(lambda: abs(pos.value - 2000) < 1e-6), pos.value)

        print("IEC 104 -> MQTT")
        do1.value = True
        check("single command accepted (ACT_CON positive)", do1.transmit(cause=c104.Cot.ACTIVATION))
        check("published do/1/set = true", wait(lambda: received.get(f"{P}/do/1/set") == "true"),
              received.get(f"{P}/do/1/set"))
        do1.value = False
        do1.transmit(cause=c104.Cot.ACTIVATION)
        check("published do/1/set = false", wait(lambda: received.get(f"{P}/do/1/set") == "false"),
              received.get(f"{P}/do/1/set"))
        ao1.value = 12.34
        check("float set point accepted", ao1.transmit(cause=c104.Cot.ACTIVATION))
        check("published ao/1/set = 123 (x10, integer)", wait(lambda: received.get(f"{P}/ao/1/set") == "123"),
              received.get(f"{P}/ao/1/set"))
        ao1.value = 500.0
        check("set point above max refused (ACT_CON negative)", not ao1.transmit(cause=c104.Cot.ACTIVATION))
        check("refused set point not published", received.get(f"{P}/ao/1/set") == "123", received.get(f"{P}/ao/1/set"))
        ao2.value = c104.Int16(-42)
        check("scaled set point accepted", ao2.transmit(cause=c104.Cot.ACTIVATION))
        check("published ao/2/set = -42", wait(lambda: received.get(f"{P}/ao/2/set") == "-42"),
              received.get(f"{P}/ao/2/set"))

        print("availability")
        pub.publish(f"{P}/status", "offline", retain=True)
        check("status offline: IOA 101 Invalid", wait(lambda: di1.quality == c104.Quality.Invalid), di1.quality)
        do1.value = True
        check("status offline: command refused", not do1.transmit(cause=c104.Cot.ACTIVATION))
        pub.publish(f"{P}/status", "online", retain=True)
        check("status online again: IOA 101 good, value kept",
              wait(lambda: di1.quality.is_good() and di1.value is True), (di1.value, di1.quality))
        check("status online: IOA 102 good again", wait(lambda: di2.quality.is_good()), di2.quality)

        client.stop()
    finally:
        gw.terminate()
        try:
            gw.wait(5)
        except subprocess.TimeoutExpired:
            gw.kill()
        os.unlink(cfg.name)
    check("gateway stops cleanly and publishes offline",
          gw.returncode == 0 and wait(lambda: received.get(f"{P}/iec104/status") == "offline"),
          (gw.returncode, received.get(f"{P}/iec104/status")))
    for t in ("status", "di/1", "iec104/status"):
        pub.publish(f"{P}/{t}", None, retain=True)
    time.sleep(0.2)
    pub.loop_stop()
    print(f"\n{'all checks passed' if not failures else str(len(failures)) + ' FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
