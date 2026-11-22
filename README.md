# kbus-gateway: WAGO KBUS on MQTT

kbus-gateway is a small native program for WAGO PFC100, PFC200 and **PFC300**
controllers. It **owns the KBUS** and puts all of its I/O on the **MQTT
broker** of the controller:

- every DI / DO / AI / AO channel, the module list and the bus status;
- outputs are switched by publishing to a `.../set` topic;
- complex modules: raw process data (750-495), scaled values (750-450 in °C),
  a driver for the 750-671 stepper controller, register reads;
- the serial interface **COM1**, forwarded transparently;
- an optional write watchdog that switches all outputs off when the
  controlling client goes silent.

It runs the KBUS cycle every **5 ms** with real-time priority, starts at boot,
and is installed as a firmware package (`.ipk`) or as a container.

```
                                    WAGO PFC300
                 ┌───────────────────────────────────────────────────────────┐
 KBUS ◀──5 ms──▶ │ kbus-gateway ──publish kbus/#──▶ mosquitto :1883 ◀── CODESYS, Node-RED,
 I/O modules     │      ▲      ◀──kbus/.../set───        ▲  ▲          containers ...
 COM1 ◀────────▶ │──────┘                               │  │ ws, read only        │
                 │ mqtt-bridge ───publish plant/...─────┘  │                      │
                 │ WBM web server /kbus ──▶ mqtt-manager (page) ◀── browser       │
                 └──────▲────────────────────────────────────────────────────────┘
                        │ subscribe (TLS, login)
             other brokers: plant / cloud / weather ...
```

| Component | What it does | Needed? |
|---|---|---|
| **kbus-gateway** | owns the KBUS and COM1, publishes them on MQTT | yes (native package or container) |
| **kbus-mosquitto** | the MQTT broker, configuration built in | yes, unless you run mosquitto another way |
| **mqtt-bridge** | brings topics from **other brokers** onto the broker of the PFC | optional |
| **mqtt-iec104** | an **IEC 60870-5-104 server** for SCADA: KBUS points and commands, mapping in a YAML file | optional |
| **mqtt-modbus** | polls **Modbus TCP devices** (meters, inverters, drives) and publishes their values; writes from MQTT | optional |
| **mqtt-manager** | the **KBUS Manager** page at `http(s)://<pfc-ip>/kbus`, in the look of the WBM: status, modules, COM1, live topics, switching outputs, login | optional |

## Quick start

Requirements on the PC: Linux with Docker (buildx), the WAGO firmware SDK of
your controller with its builder image, and SSH access to the PFC as `root`.
Details: [manual, section 2](docs/MANUAL.md#2-installation).

1. **Free the KBUS.** WBM → *PLC Runtime* → **None**, or run CODESYS without
   KBUS ([codesys/README.md](codesys/README.md)). Only one program can own the
   KBUS.
2. **Import the WAGO SDK headers** once (proprietary, not in this repository):

   ```bash
   tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages
   ```

3. **Deploy** (example controller `192.168.1.17`):

   ```bash
   tools/deploy-docker.sh 192.168.1.17 --mosquitto --no-gateway          # the broker
   tools/deploy.sh 192.168.1.17                                          # kbus-gateway: build .ipk, install, start
   tools/deploy-docker.sh 192.168.1.17 --bridge --manager --no-gateway   # optional: mqtt-bridge, KBUS Manager page
   tools/deploy-docker.sh 192.168.1.17 --iec104 --no-gateway             # optional: IEC 104 gateway (port 2404)
   tools/deploy-docker.sh 192.168.1.17 --modbus --no-gateway             # optional: Modbus TCP devices
   ```

4. **Check**:

   ```bash
   ssh root@192.168.1.17 docker exec mosquitto mosquitto_sub -t 'kbus/#' -v
   tests/test-mqtt.py 192.168.1.17 node        # from the PC, read only
   ```

   or open `https://192.168.1.17/kbus` (default login `admin` / `wago`;
   change it under *Settings > Login*).

## Topics at a glance

```bash
mosquitto_sub -h <pfc-ip> -t 'kbus/#' -v                       # everything
mosquitto_pub -h <pfc-ip> -t kbus/do/3/set -m true              # switch DO3 on
mosquitto_pub -h <pfc-ip> -t kbus/ao/1/set -m 16384             # AO1 to half scale (raw)
mosquitto_pub -h <pfc-ip> -t kbus/do/all/set -m '[true,null,false]'   # several DOs in one bus cycle
mosquitto_pub -h <pfc-ip> -t kbus/serial/com1/tx/set -m 'hello'       # send bytes on COM1
```

| Topic | Content |
|---|---|
| `kbus/status` | `online` / `offline` (last will) |
| `kbus/di/<n>`, `kbus/do/<n>` | `true` / `false` |
| `kbus/ai/<n>`, `kbus/ao/<n>` | raw value 0–65535 |
| `kbus/do/<n>/set`, `kbus/ao/<n>/set` | write an output (publish **without** retain) |
| `kbus/modules` | module list with channels (JSON) |
| `kbus/module/<pos>/...` | raw bytes, temperatures, stepper, registers |
| `kbus/bus/...` | healthy, cycle time, statistics, rescan |
| `kbus/serial/com1/...` | COM1: received frames, send, settings |
| `kbus/error` | rejected commands |

Channels are numbered per class from 1, left to right, as in the CODESYS I/O
mapping. The complete topic tree is in [manual, section 3](docs/MANUAL.md#3-mqtt-topics).

> **Security:** the supplied broker accepts MQTT without login on port 1883.
> Anyone who can reach that port can read all I/O and switch outputs. Fine
> for an isolated machine network; otherwise see
> [manual, section 11](docs/MANUAL.md#11-limitations-and-security). The same
> applies to the IEC 104 port 2404 of mqtt-iec104.

## Documentation

| Document | Content |
|---|---|
| [docs/MANUAL.md](docs/MANUAL.md) | **the manual**: how it works, installation, all topics, module functions, COM1, KBUS Manager page, mqtt-bridge, configuration, development, troubleshooting, test status |
| [codesys/README.md](codesys/README.md) | using the I/O from CODESYS through MQTT, incl. the 750-495 with the standard WAGO function block |
| [docker/README.md](docker/README.md) | the containers and how to build and deploy them |
| [docker/mqtt-bridge/README.md](docker/mqtt-bridge/README.md) | the mqtt-bridge in detail |
| [docker/mqtt-iec104/README.md](docker/mqtt-iec104/README.md) | the IEC 104 gateway: YAML mapping, behaviour, test |
| [docker/mqtt-modbus/README.md](docker/mqtt-modbus/README.md) | the Modbus TCP bridge: devices, tags, behaviour, test |
| [sdk/README.md](sdk/README.md) | the WAGO SDK headers |

## Project layout

```
├── src/              kbus-gateway in C (no dependencies besides glibc)
├── sdk/              WAGO SDK headers (imported) + OsCommon.h stand-in
├── packaging/ipk/    native package: init script, config file, opkg scripts
├── docker/           containers: gateway, mosquitto, mqtt-bridge, mqtt-manager, mqtt-iec104, mqtt-modbus
├── web/              the KBUS Manager page
├── codesys/          CODESYS guide
├── tools/            build-ipk.sh, deploy.sh, deploy-docker.sh, import-sdk-*.sh
├── tests/            test-mqtt.py, test-iec104.py, test-modbus.py
└── docs/             MANUAL.md
```

Development without a controller (simulated node):

```bash
docker run -d --name mosquitto-dev -p 1883:1883 eclipse-mosquitto:2.0.20 mosquitto -c /mosquitto-no-auth.conf
make run-sim
tests/test-mqtt.py 127.0.0.1 sim
```

## Status

Version: see [VERSION](VERSION). Tested on a PFC300 (FW 30 and 31) as
containers, next to CODESYS without KBUS. Not tested yet: the native `.ipk`
on a controller, PFC100/PFC200, inputs and analog modules on real hardware
(simulation only), 750-671 moves with a motor, COM1 on the real interface,
mqtt-iec104 and mqtt-modbus on a controller (tested on the PC).
Details: [manual, section 12](docs/MANUAL.md#12-test-status-and-history).

kbus-gateway 3.x is the MQTT successor of the HTTP/wdx gateway
(`docker-wdx-kbus-interface`, 1.x–2.x) and replaces it as a normal package
upgrade.

## License

MIT, see [LICENSE](LICENSE). The KBUS access follows WAGO's MIT-licensed
example *HowTo_ADI-MyKBusApplikation*. The WAGO SDK headers are proprietary
and not included.
