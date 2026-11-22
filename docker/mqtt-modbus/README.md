# mqtt-modbus: Modbus TCP devices ⇄ MQTT

A Modbus TCP **client** (master) on the PFC that reads energy meters,
inverters, drives, other controllers … cyclically and publishes their values
on the MQTT broker, next to the KBUS data of kbus-gateway. Values published on
a `/set` topic are written to the device.

```
 Modbus TCP devices ◀── poll (one connection per device) ── mqtt-modbus ──▶ mosquitto on the PFC
   meter  192.168.1.50:502                                     modbus/meter/power        12.4
   drive  192.168.1.60:502                                     modbus/drive/speed/set ──▶ write
```

Python 3.12 with [pymodbus](https://github.com/pymodbus-dev/pymodbus) 3.6.9
and paho-mqtt 2.1.0. Built for the **PFC300 (arm64)**; the image also builds
for armv7 (PFC200), which has not been tested.

## Files

| File | |
|---|---|
| `mqtt_modbus.py` | the bridge |
| `mqtt-modbus.yaml` | example configuration (energy meter), all settings explained (copied to the controller at the first deployment) |
| `Dockerfile`, `.dockerignore` | image; build context is this folder |

Test: `tests/test-modbus.py` (below).

## Deploy

```bash
tools/deploy-docker.sh 192.168.1.17 --modbus --no-gateway
```

The first deployment creates `/home/mqtt-modbus/mqtt-modbus.yaml` on the
controller (mounted as `/config`, read-only); later deployments keep it. Enter
your devices, then:

```bash
ssh root@192.168.1.17 docker run --rm -v /home/mqtt-modbus:/config:ro mqtt-modbus:arm64 --check   # check, show the tags
ssh root@192.168.1.17 docker restart mqtt-modbus
ssh root@192.168.1.17 docker logs -f mqtt-modbus
mosquitto_sub -h 192.168.1.17 -t 'modbus/#' -v
```

## Topics

| Topic | Content |
|---|---|
| `<topic>/<tag>` | value, published **on change** (retained); `<topic>` = `modbus/<device name>` unless set |
| `<topic>/<tag>/set` | write (tags with `write: true`); publish **without** retain |
| `<topic>/status` | `online` / `offline`: device reachable (retained) |
| `modbus/status` | the bridge itself: `online` / `offline` (retained, last will) |
| `modbus/error` | `{"topic":..,"error":..}`: rejected or failed write (not retained) |

Payload: plain (`12.4`, `true`) by default, the same style as kbus-gateway;
`general.payload: json` gives `{"value": 12.4, "unit": "kW", "timestamp": "..."}`.

## Configuration (YAML)

```yaml
devices:
  - name: meter
    host: 192.168.1.50
    unit_id: 1
    interval: 1                       # s
    tags:
      - name: voltage_l{n}            # voltage_l1..l3 = holding 0, 2, 4 (float32 = 2 registers each)
        count: 3
        address: 0
        type: float32
        decimals: 1
      - name: power
        address: 12
        type: int32
        scale: 0.001                  # W -> kW
      - name: alarm
        address: 20
        bit: 3                        # one bit of a register
      - name: setpoint                # modbus/meter/setpoint/set = 21.5 -> raw 215 with FC6
        address: 30
        type: int16
        scale: 0.1
        write: true
        min: 0
        max: 100
      - name: relay{n}                # coils 0..3; modbus/meter/relay1/set = true -> FC5
        count: 4
        table: coil
        address: 0
        write: true
```

**Device**

| Key | Default | |
|---|---|---|
| `name` | – | unique; default topic `modbus/<name>` |
| `host`, `port` | –, 502 | Modbus TCP server |
| `unit_id` | 1 | default unit / slave id of the tags |
| `interval` | 1 | poll interval in s (0.05 … 86400) |
| `timeout` | 3 | response timeout in s |
| `word_order` | `big` | 32/64-bit values: `big` = high word first (ABCD), `little` = low word first (CDAB) |
| `byte_order` | `big` | inside a register: `big` = standard (AB), `little` = swapped (BA) |
| `max_gap` | 10 | tags at most this many registers apart are read in **one** request (max. 125 registers / 2000 bits); 0 = only adjacent ones. Lower it when the device answers "illegal data address" for the gaps |
| `topic` | `modbus/<name>` | base topic |

**Tag**

| Key | Default | |
|---|---|---|
| `name` | – | topic level; with `count`, `{n}` = 1, 2, … |
| `table` | `holding` | `coil` (FC1, write FC5), `discrete` (FC2), `holding` (FC3, write FC6 / FC16), `input` (FC4) |
| `address` | – | 0-based; with `count` the next tags follow (by the size of the type) |
| `type` | `uint16` (`bool` for coil/discrete/bit) | `bool`, `int16`, `uint16`, `int32`, `uint32`, `float32`, `int64`, `uint64`, `float64` |
| `bit` | – | 0…15: one bit of a holding/input register (read only) |
| `scale`, `offset` | 1, 0 | value = raw × scale + offset; writing: raw = (value − offset) / scale |
| `decimals` | – | round the published value |
| `unit` | – | only in the json payload |
| `unit_id` | device | other unit id (e.g. several meters behind one Modbus TCP gateway) |
| `write` | `false` | subscribe to `<topic>/<name>/set` (coil and holding only) |
| `min`, `max` | – | refuse writes outside this range |
| `topic` | `<topic>/<name>` | own topic (not with `count`) |
| `count` | 1 | a series of equal tags |

**General and MQTT**: `general.log_level` (`DEBUG` shows every value),
`general.payload` (`plain` / `json`), `general.republish_s` (also publish
unchanged values every N s, 0 = only changes); `mqtt.host`, `port`,
`client_id`, `username`, `password`, `keepalive`, `qos`, `retain` (default
`true`), `tls`, `ca_cert`, `tls_insecure`, `status_topic` (default
`modbus/status`; the error topic is next to it).

The whole file is checked at start (unknown keys, ranges, types per table,
double names and topics); an error stops the container with a clear message.

## Behaviour

- **Polling**: one thread and one TCP connection per device; adjacent tags of
  the same unit and table are read in one request (the `--check` output shows
  how many requests a poll takes). A slow or dead device does not delay the
  others.
- **Change only**: a value is published when it differs from the last
  published one (and every `republish_s` if set). After an MQTT reconnect
  everything is published again.
- **Device offline**: `<topic>/status` = `offline`, reconnect every interval
  (at least 1 s, at most 10 s). The last values stay retained on the broker;
  check the status topic before trusting them.
- **Writing**: `true`/`false`/`on`/`off`, a number or `{"value": ..}`. Coils
  with FC5; 16-bit registers with FC6; 32/64-bit values with FC16 in one
  request. Range checks: `min`/`max` and the range of the data type. A retained
  `/set` message is ignored (it would be written again at every reconnect);
  delete it with `mosquitto_pub -r -n -t <topic>/set`. After a write the value
  is read back and published at the next poll.
- **Connecting to the KBUS**: the bridge only talks MQTT. Coupling Modbus
  values to KBUS outputs (or the other way round) is done by the application,
  e.g. CODESYS or Node-RED, which subscribes to one and publishes to the other.

## Test without a PFC

```bash
docker run -d --rm --name mosquitto-dev -p 1883:1883 eclipse-mosquitto:2.0.20 mosquitto -c /mosquitto-no-auth.conf
pip install pymodbus==3.6.9 paho-mqtt PyYAML
tests/test-modbus.py 127.0.0.1
```

The test starts a Modbus TCP server (pymodbus) and the bridge with its own
configuration (topics under `test-modbus/`) and checks all data types, word
order, bit, scale, series, change-only publishing, an unreachable device,
writes with FC5, FC6 and FC16, refused and invalid writes, a retained command
at start and a clean stop (25 checks).

## Origin

Based on the standalone `docker-mqtt-modbus` bridge (INI file + CSV tag list,
one device), rewritten for this project: YAML configuration, several devices
in parallel, block reads, writes with FC5/FC16 next to FC6, `/set` topics
(the old version listened on the value topic itself), change-only publishing,
device status, protection against retained commands, an image that builds
without emulation.
