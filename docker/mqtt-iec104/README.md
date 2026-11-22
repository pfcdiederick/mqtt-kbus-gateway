# mqtt-iec104: MQTT ⇄ IEC 60870-5-104 gateway

An IEC 60870-5-104 server (slave / outstation) on the PFC whose data comes from
the MQTT broker. A SCADA system or control centre that speaks IEC 104 thereby
reaches the KBUS I/O of kbus-gateway, and any other MQTT data, without a
CODESYS application in between.

```
 SCADA / control centre ── IEC 104, TCP 2404 ──▶ mqtt-iec104 ◀── MQTT ──▶ mosquitto ◀──▶ kbus-gateway ◀──▶ KBUS
                                                 (container, --network host)
   monitoring points  ◀──  kbus/di/1, kbus/ai/1, kbus/module/3/temperature/1, ...
   commands           ──▶  kbus/do/1/set, kbus/ao/1/set, ...
```

The IEC 104 protocol is handled by [c104](https://github.com/Fraunhofer-FIT-DIEN/iec104-python)
2.2.1, the Python binding of [mz-automation/lib60870-C](https://github.com/mz-automation/lib60870).
The mapping between topics and information object addresses (IOAs) is one
YAML file.

## Files

| File | |
|---|---|
| `mqtt_iec104.py` | the gateway |
| `mqtt-iec104.yaml` | example mapping for a KBUS node, all settings explained (copied to the controller at the first deployment) |
| `Dockerfile`, `.dockerignore` | image; build context is this folder |

Test: `tests/test-iec104.py` (below).

## Deploy

```bash
tools/deploy-docker.sh 192.168.1.17 --iec104 --no-gateway
```

The first deployment creates `/home/mqtt-iec104/mqtt-iec104.yaml` on the
controller (mounted as `/config`, read-only); later deployments keep it. After
a change:

```bash
ssh root@192.168.1.17 vi /home/mqtt-iec104/mqtt-iec104.yaml
ssh root@192.168.1.17 docker run --rm -v /home/mqtt-iec104:/config:ro mqtt-iec104:arm64 --check   # check, show the mapping
ssh root@192.168.1.17 docker restart mqtt-iec104
ssh root@192.168.1.17 docker logs -f mqtt-iec104
```

The SCADA connects to `<pfc-ip>:2404` with the configured common address
(default 1). The image is built for **arm64 (PFC300)**; c104 has no prebuilt
package for armv7 (PFC100/PFC200).

## The mapping (YAML)

```yaml
points:                                  # MQTT -> IEC 104
  - topic: kbus/di/{n}                   # kbus/di/1..8 -> IOA 101..108, single point
    count: 8
    ioa: 101
    type: single
  - topic: kbus/ai/{n}                   # raw 0..32767 -> 0..100 %, measured value float
    count: 4
    ioa: 301
    type: float
    scale: 0.0030518
  - topic: kbus/module/4/stepper/state   # one field of a JSON payload
    ioa: 501
    type: float
    key: position

commands:                                # IEC 104 -> MQTT
  - topic: kbus/do/{n}/set               # single command IOA 1101..1108 -> true / false
    count: 8
    ioa: 1101
    type: single
  - topic: kbus/ao/1/set                 # set point 0..100 % -> raw 0..32767
    ioa: 1201
    type: float
    scale: 327.67
    integer: true
    min: 0
    max: 32767
```

**Series.** `{n}` in the topic with `count: N` makes N entries: `{n}` runs from
`start` (default 1), the IOA from `ioa` upwards. MQTT wildcards are not
allowed: every IOA needs a fixed topic.

### Points (MQTT → IEC 104)

| `type` | IEC 104 type | Value |
|---|---|---|
| `single` | `M_SP_TB_1` (30), single point with time tag | `true`/`false`, `on`/`off`, a number (≠ 0 = on) |
| `float` | `M_ME_TF_1` (36), measured value, short float with time tag | number × `scale` + `offset` |
| `scaled` | `M_ME_TE_1` (35), measured value, scaled with time tag | number × `scale` + `offset`, rounded to INT16; outside −32768…32767: clamped, quality *Overflow* |

| Key | Default | |
|---|---|---|
| `topic`, `ioa`, `type` | – | required |
| `count`, `start` | 1, 1 | series, see above |
| `key` | – | JSON payload: dotted path of the value (`position`, `a.b`, `0`); without it a plain value or `{"value": ..}` |
| `scale`, `offset` | 1, 0 | IEC value = MQTT value × scale + offset |
| `report_ms` | `iec104.report_ms` | also send cyclically every N ms |
| `name` | – | free text |

Behaviour:

- A new MQTT value is sent **spontaneously** (COT 3) to all connected clients,
  with the receive time as time tag. A **general interrogation** returns the
  latest values (COT 20).
- **Quality**: *Invalid* until the first value arrives, while the MQTT
  connection is lost, and while the availability topic is not `online`
  (default `kbus/status`: kbus-gateway stopped or crashed). *NonTopical* when
  no new value arrives within `general.stale_after` seconds (off by default:
  kbus-gateway publishes only changes, so a constant input would become
  NonTopical). Good again as soon as the cause is gone; the last value is kept.
- One topic may feed several IOAs (e.g. different `key`s of a JSON payload).
- Unreadable payloads are logged and ignored; the point keeps its value.

### Commands (IEC 104 → MQTT)

| `type` | IEC 104 type | Published |
|---|---|---|
| `single` | `C_SC_NA_1` (45), single command | `on` / `off` payload (default `true` / `false`) |
| `float` | `C_SE_NC_1` (50), set point, short float | value × `scale` + `offset` |
| `scaled` | `C_SE_NB_1` (49), set point, scaled INT16 | value × `scale` + `offset` |

| Key | Default | |
|---|---|---|
| `topic`, `ioa`, `type`, `count`, `start`, `name` | | as for points |
| `scale`, `offset` | 1, 0 | MQTT value = IEC value × scale + offset |
| `integer` | `true` for scaled, `false` for float | publish as integer (rounded) |
| `min`, `max` | – | refuse values outside (after scaling) |
| `on`, `off` | `true`, `false` | payloads of a single command |
| `retain` | `false` | keep `false` for `kbus/.../set`: kbus-gateway ignores retained commands |

- The client gets a **positive activation confirmation** when the value has
  been published, a **negative** one when the broker is not connected, the
  availability topic is not `online`, or the value is outside `min`/`max`.
  A refused command is not published later.
- `iec104.command_mode`: `direct` (default) or `select`
  (select-before-operate).
- Feedback: map the state topic as a point (e.g. `kbus/do/{n}` next to
  `kbus/do/{n}/set`), so the SCADA sees the real output state.

### Other settings

| Section | Keys |
|---|---|
| `general` | `log_level` (`DEBUG` shows every value), `stale_after` |
| `mqtt` | `host`, `port`, `client_id`, `username`, `password`, `keepalive`, `qos`, `tls`, `ca_cert` (file in `/home/mqtt-iec104/` → `/config/<file>`), `tls_insecure`, `status_topic` (default `iec104/status`: `online`/`offline`, retained, last will), `availability` (`topic`, `online`; empty = always available) |
| `iec104` | `ip` (`0.0.0.0` = all interfaces), `port` (2404), `max_connections` (0 = unlimited), `common_address` (1), `report_ms` (0 = only spontaneous), `command_mode` |

The configuration is checked completely at start (unknown keys, double IOAs,
a command topic that is also a point topic, ranges); an error stops the
container with a clear message in `docker logs mqtt-iec104`.

## Security

IEC 104 has no login and no encryption here: **everyone who can reach port
2404 of the controller can read the points and send the configured
commands**, i.e. switch outputs. Use it on a separate, trusted network; if
the PFC firewall is enabled, allow TCP 2404 only from the SCADA system. Map
only the commands the SCADA really needs, and use `min`/`max` on set points.

## Test without a PFC

```bash
docker run -d --rm --name mosquitto-dev -p 1883:1883 eclipse-mosquitto:2.0.20 mosquitto -c /mosquitto-no-auth.conf
pip install c104==2.2.1 paho-mqtt PyYAML
tests/test-iec104.py 127.0.0.1
```

The test starts the gateway with its own configuration (topics under
`test104/`), plays kbus-gateway on MQTT and checks with an IEC 104 client:
interrogation of a retained value, spontaneous values, scale, Overflow, JSON
key, single command and set points to MQTT, refusal outside `min`/`max`,
availability offline/online, clean stop (22 checks).

For a manual test with a GUI tool (e.g. QTester104): connect to
`<pfc-ip>:2404`, common address 1, send StartDT and a general interrogation.

## Origin

Based on the standalone `docker-mqtt-104` gateway (INI file, one measured value
and one set point), rewritten for this project: YAML mapping with series,
single points and single commands for the KBUS, scaled and float values,
scale/offset, JSON keys, availability through `kbus/status`, status topic,
select-before-operate, an image that builds without emulation.
