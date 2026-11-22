# kbus-gateway manual

Complete manual of kbus-gateway: installation, MQTT topics, module functions,
COM1, the KBUS Manager page, the mqtt-bridge, configuration, development and
troubleshooting. For a short introduction see the [README](../README.md).

The examples use the controller address `192.168.1.17` and the default topic
prefix `kbus`. Commands marked *PC* run in the project directory on a Linux
PC; commands marked *PFC* run on the controller (`ssh root@<pfc-ip>`).

## Contents

1. [How it works](#1-how-it-works)
2. [Installation](#2-installation)
3. [MQTT topics](#3-mqtt-topics)
4. [Module functions](#4-module-functions): complex modules (750-495), module values (750-450), 750-671 stepper, register reads, write watchdog
5. [Serial interface COM1](#5-serial-interface-com1)
6. [KBUS Manager page](#6-kbus-manager-page)
7. [Other systems: mqtt-bridge, IEC 104, Modbus TCP](#7-other-systems-mqtt-bridge-iec-104-modbus-tcp)
8. [Configuration](#8-configuration)
9. [Development](#9-development)
10. [Troubleshooting](#10-troubleshooting)
11. [Limitations and security](#11-limitations-and-security)
12. [Test status and history](#12-test-status-and-history)

---

## 1. How it works

```
 WAGO PFC (PTXdist firmware)
 ┌─────────────────────────────────────────────────────────────────────┐
 │ /usr/bin/kbus-gateway  (started by /etc/init.d/kbus-gateway)        │
 │                                                                     │
 │  MQTT thread (main)                       KBUS cycle thread         │
 │   every 20 ms: snapshot of all channels    every 5 ms, SCHED_FIFO 40│
 │   → publish what changed                   write outputs            │
 │   .../set messages → write outputs ──lock──▶ libpackbus_Push        │
 │   bus rescans, 750-671 driver              read inputs              │
 │        │ MQTT 3.1.1, TCP                        │ dlopen() of the   │
 │        ▼                                        ▼ firmware libdal.so│
 │   mosquitto 127.0.0.1:1883                 KBUS ──▶ I/O modules     │
 └─────────────────────────────────────────────────────────────────────┘
```

### 1.1 Startup

1. The WAGO libraries (`libdal.so`, `libdbuskbuscommon.so` and the DAL plugins
   in `/usr/lib/dal`) are loaded with `dlopen()` from the firmware's own
   `/usr/lib`. They are not linked into the program, so it always uses the
   libraries of the installed firmware; the build only needs their headers.
2. The KBUS device (`libpackbus`) is opened. The modules are read through the
   KBUS-info D-Bus interface: position, type, bit offsets and sizes, channels.
3. A **channel map** is built. Channels are numbered from 1, separately for
   each class, in module order from left to right (at most 250 modules and
   250 channels per class), the same as the CODESYS I/O mapping of the node.
   - **Digital module** (type code bit 15 set): every input bit is a `DI`,
     every output bit a `DO`.
   - **Analog module** with only input data → `AI`, with only output data →
     `AO`. Channel *k* is the unsigned 16-bit word at byte offset 2·(k−1) of
     the module data, raw value.
   - **Complex modules** with data in both directions (750-495, 750-657,
     750-671, counters, serial interfaces) get no channels; they are reached
     through their raw bytes or a driver (section 4).
4. The KBUS is switched to application state *Running*; from then on the
   program drives every bus cycle itself.
5. The MQTT thread connects to the broker and publishes everything.

### 1.2 KBUS cycle thread

Every `KBUS_CYCLE_MS` (default 5 ms) a real-time thread (`SCHED_FIFO`,
priority 40) copies the output image, triggers one cycle with
`libpackbus_Push` and stores the input image. WAGO output modules switch their
outputs off when no cycle arrives within **50 ms**, so this thread never waits
for MQTT: the images are locked only for the copy.

### 1.3 MQTT thread

Every `MQTT_PUBLISH_MS` (default 20 ms) it takes one snapshot of all channels
(one lock, so all values come from the same bus cycle), compares it with what
it published last and publishes only the differences. An input change is
therefore on the broker within about 5 + 20 ms. A `.../set` message is written
to the output image at once; the next bus cycle (≤ 5 ms) sends it to the
module. The broker connection is kept alive and re-established after a loss
(1, 2, 4 … 30 s); the KBUS keeps running in the meantime.

### 1.4 Changes of the node

Modules added to or removed from the KBUS are picked up while the program
runs: after **0.5 s of failing cycles** the bus is rescanned (stop,
re-initialise, new channel map, start; under one second on the PFC300).
`kbus/bus/configversion` goes up by one, `kbus/modules` is published again,
the topics of new channels appear and the retained topics of channels that no
longer exist are deleted. If errors persist, the next automatic attempt waits
5, 10, 20, 40, then 60 s. A rescan can also be started by hand:
`kbus/bus/rescan/set` = `true`. Outputs are 0 after a rescan until they are
written again.

WAGO allows changing modules only with the node switched off, so in practice
a new node is found at the next start.

### 1.5 Shutdown

`/etc/init.d/kbus-gateway stop` (also at shutdown) sends SIGTERM:
`kbus/status` becomes `offline`, all outputs go to 0, one last cycle runs and
the KBUS device is closed. If the program crashes, the broker publishes
`offline` (last will) and the output modules switch off on their own after
50 ms without a cycle.

### 1.6 Files on the controller

| File | Purpose |
|---|---|
| `/usr/bin/kbus-gateway` | the program |
| `/etc/init.d/kbus-gateway` | `start`, `stop`, `restart`, `status` |
| `/etc/rc.d/S99_kbus-gateway` | boot link, created by the package |
| `/etc/kbus-gateway.conf` | settings (section 8); kept on package updates when you changed it |
| `/home/kbus-gateway/kbus-settings.conf` | cycle time and COM1 settings written over MQTT |
| `/var/log/messages` | log (syslog, `kbus-gateway[pid]`) |
| `/home/mosquitto/data/` | retained values of the broker (kbus-mosquitto container) |
| `/home/mqtt-bridge/mqtt-bridge.ini` | configuration of the mqtt-bridge (+ certificates) |
| `/home/mqtt-manager/settings.json` | login of the KBUS Manager page |

---

## 2. Installation

Quick route once the PC is set up (2.1–2.3):

```bash
tools/deploy-docker.sh 192.168.1.17 --mosquitto --no-gateway           # PC: the broker
tools/deploy.sh 192.168.1.17                                           # PC: build .ipk, copy, opkg install, status
tools/deploy-docker.sh 192.168.1.17 --bridge --manager --no-gateway    # PC: optional: mqtt-bridge, KBUS Manager page
```

### 2.1 Requirements on the PC

| What | Why |
|---|---|
| Linux PC with **Docker** (buildx) | WAGO's SDK build image with the firmware toolchain; building the containers |
| The **WAGO firmware SDK** of your controller, e.g. [pfc300-firmware-sdk](https://github.com/WAGO/pfc300-firmware-sdk), with its builder image: `make builder` → `pfc300-builder:latest` | toolchain with the same GCC and glibc as the firmware |
| The **WAGO SDK headers** in `sdk/include` (2.3) | ADI/DAL, KBUS-info and register API; proprietary, not in this repository |
| Python 3 + `pip install paho-mqtt` | only for the test script |
| SSH/scp access to the PFC as `root` | installation |

### 2.2 Prepare the PFC

**One KBUS master.** Only one program may own the KBUS. Either set WBM →
*PLC Runtime* → runtime version **None**, or run the CODESYS runtime
**without KBUS** (see [codesys/README.md](../codesys/README.md)). An installed
kbus-gateway 2.x (the HTTP/wdx version) is replaced by this package as a
normal upgrade.

**The MQTT broker.** kbus-gateway connects to `MQTT_HOST:MQTT_PORT` (default
`127.0.0.1:1883`). The firmware's `mosquitto` package only contains the
clients (`mosquitto_pub`, `mosquitto_sub`), so install the broker in one of
two ways:

- **As a container** (recommended):
  `tools/deploy-docker.sh 192.168.1.17 --mosquitto --no-gateway`. It builds
  **kbus-mosquitto** ([docker/mosquitto](../docker/mosquitto)):
  eclipse-mosquitto 2.0.20 with
  [docker/mosquitto/mosquitto.conf](../docker/mosquitto/mosquitto.conf) built
  in, `--network host`; retained values are kept in `/home/mosquitto/data`.
  To change the configuration, edit that file and deploy again.
- **Any other way**: then take over the listeners of that configuration file.

| Listener | Access |
|---|---|
| `1883`, all interfaces | MQTT **without login**, read and write: everyone who can reach the controller can switch outputs (section 11) |
| `127.0.0.1:9001`, WebSocket | only for the KBUS Manager page (section 6), through the WAGO web server; **read only** (`acl-monitor`) |

For a login on 1883: `allow_anonymous false`, a `password_file` and an
`acl_file`, as described at the end of the configuration file.

The order of installation does not matter: kbus-gateway keeps running the
KBUS and connects as soon as the broker is there.

### 2.3 Get the SDK header files

The real KBUS backend is compiled against the ADI/DAL, KBUS-info and KBUS
register-communication **header files** of the WAGO SDK. They carry a WAGO
"proprietary rights" notice and are therefore **not in this repository**;
import them once:

```bash
# A. from the prebuilt packages of the SDK (no SDK build needed)
tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages
# B. from a completely built SDK
tools/import-sdk-headers.sh ~/wago/ptxproj
```

- The DAL version of the headers must equal `libdal.so.<version>` on the PFC
  (`ls -l /usr/lib/libdal.so*`).
- Without `sdk/include/dal` only the **simulation** build works (`make`),
  which is enough to develop and test the MQTT side (9.3).
- Without `sdk/include/ldkc_kbus_register_communication.h` (imported since
  3.4.0; import again after updating) the gateway builds without register
  access: no `kbus/ai/<n>/status`, and `reg/get` answers "register access not
  available" (4.4).

Details: [sdk/README.md](../sdk/README.md).

### 2.4 Build the package

*PC*:

```bash
tools/build-ipk.sh arm64        # PFC300        -> build-ipk/kbus-gateway_<version>_arm64.ipk
tools/build-ipk.sh armhf        # PFC100/PFC200 -> build-ipk/kbus-gateway_<version>_armhf.ipk (untested)
```

The script runs `make` inside the SDK builder image with WAGO's cross compiler
and `-Werror`, strips the binary, builds the package tree (program, init
script, config file, `postinst`, `prerm`, `conffiles`, `control`; version
from [VERSION](../VERSION)) and packs it like a PTXdist package (an `ar`
archive with `debian-binary`, `control.tar.gz`, `data.tar.gz`).
`BUILDER_IMAGE=...` selects another SDK image.

### 2.5 Install, update, remove

**Install**: WBM → *Software Uploads* → upload the `.ipk` → *Install*, or
`tools/deploy.sh 192.168.1.17` (build, copy, `opkg install`, status and log).
`postinst` creates the boot link and (re)starts the service.

**Update**: raise [VERSION](../VERSION), install the new `.ipk` the same way.
Your `/etc/kbus-gateway.conf` is kept if you changed it.

**Operate** (*PFC*):

```bash
/etc/init.d/kbus-gateway status|stop|start|restart
grep kbus-gateway /var/log/messages | tail -20
```

**Remove**: `opkg remove kbus-gateway` (stops it, all outputs go to 0).

### 2.6 Check the start

*PFC*: `grep kbus-gateway /var/log/messages | tail -20`. A good start looks
like this (the module lines show your node):

```
kbus-gateway[812]: WAGO KBUS MQTT interface 3.4.0 starting
kbus-gateway[812]: config: broker=127.0.0.1:1883 client-id=kbus-gateway-PFC300-4C2B3A user=(anonymous) prefix=kbus retain=1 publish=20ms ...
kbus-gateway[812]: module  1: digital 8DO    in   0 bits @0     out   8 bits @0
kbus-gateway[812]: channels: 0 DI, 8 DO, 0 AI, 0 AO; 0 x 750-495 (configuration 1)
kbus-gateway[812]: KBUS cycle thread running: SCHED_FIFO priority 40, every 5 ms
kbus-gateway[812]: MQTT thread: SCHED_FIFO priority 30
kbus-gateway[812]: connected to MQTT broker 127.0.0.1:1883 as kbus-gateway-PFC300-4C2B3A, topics kbus/#
```

`MQTT broker 127.0.0.1:1883: ... Connection refused (retrying)` means that
mosquitto is not running (yet) or listens elsewhere (2.2).

### 2.7 Test

On the controller (mosquitto's clients are in the container):

```bash
docker exec mosquitto mosquitto_sub -t 'kbus/#' -v
```

From the PC (needs the network listener of the broker, 2.2):

```bash
tests/test-mqtt.py 192.168.1.17 node              # real node, READ ONLY: status, modules, all channel topics
tests/test-mqtt.py 192.168.1.17 write --do 1      # real node: switches DO1 on/off (check what is connected!)
#   --port 1884 --user scada --password ...       for a listener with login
```

On the PC without a PFC: see 9.3.

### 2.8 Alternative: kbus-gateway as a Docker container

The same program also runs as a container ([docker/gateway](../docker/gateway);
all containers: [docker/README.md](../docker/README.md)). The native package
is the recommended way; the container is for installations that manage
everything with Docker.

```bash
tools/deploy-docker.sh 192.168.1.17            # build, load, stop the native service, start the container
tools/deploy-docker.sh 192.168.1.17 --sim      # the same in simulation mode (no KBUS access)
tools/deploy-docker.sh 192.168.1.17 --native   # remove the container, native service again
```

The script stops the native service and removes its boot link while the
container runs (`--native` restores both). The container uses `--privileged
--network host` (real-time priority, KBUS hardware, broker on `127.0.0.1`),
mounts the firmware's `/usr/lib` read-only (the entrypoint loads the WAGO
libraries without the firmware's glibc), `/var/run/dbus`, `/etc/specific`,
the KBUS devices, the serial interface `/dev/serial` (COM1) and
`/home/kbus-gateway`.

The container does **not** read `/etc/kbus-gateway.conf`; pass settings with
`GW_ENV`, e.g. `GW_ENV="-e MQTT_TOPIC_PREFIX=line1/kbus" tools/deploy-docker.sh ...`.
Other options of the script: `SKIP_BUILD=1` (reuse the archives in
`build-docker/`), `PFC_USER`, `BR_ENV`, `MG_ENV`.

---

## 3. MQTT topics

### 3.1 The topic tree

`kbus` is the prefix (`MQTT_TOPIC_PREFIX`). All value topics are **retained**
(`MQTT_RETAIN=1`): a client that subscribes gets the current value at once.

```
read (published by kbus-gateway):
kbus/status                         online | offline       offline = last will (crash, power loss)
kbus/di/<n>                         true | false           digital input n (1-based)
kbus/do/<n>                         true | false           digital output n (the value last written)
kbus/ai/<n>                         0..65535               analog input n, raw process value
kbus/ai/<n>/status                  0, 96, ...             register 6 of the channel (status), at most 1x/s (4.4)
kbus/ao/<n>                         0..65535               analog output n (the value last written)
kbus/<class>/all                    [..]                   the whole class as JSON array (MQTT_ARRAYS=1)
kbus/modules                        [{..}]                 modules and their channels (JSON, 3.4)
kbus/module/<pos>/in                bytes (binary)         raw input data of a complex module (4.1)
kbus/module/<pos>/temperature/<n>   21.4                   scaled value, e.g. 750-450 in °C (4.2)
kbus/module/<pos>/stepper/state     {..}                   750-671 state (4.3)
kbus/module/<pos>/stepper/response  {..}                   result of a stepper command, not retained
kbus/module/<pos>/reg/value         {..,"ok":true}         answer to reg/get, not retained (4.4)
kbus/watchdog                       ok | expired           write watchdog, only with MQTT_WATCHDOG_S (4.5)
kbus/bus/healthy                    true | false           KBUS cycle running without errors
kbus/bus/cycletime                  1..40                  ms
kbus/bus/configversion              1, 2, ...              +1 for every bus scan: modules/channels changed
kbus/bus/statistics                 {..}                   every MQTT_STATS_S (3.4)
kbus/serial/com1/open               true | false           COM1 device open (5)
kbus/serial/com1/baudrate           19200                  current COM1 settings
kbus/serial/com1/format             8E1
kbus/serial/com1/framegap           10                     ms
kbus/serial/com1/statistics         {..}                   bytes, frames, dropped frames, line errors
kbus/serial/com1/rx                 bytes (binary)         ONE message per received frame, not retained
kbus/error                          {"topic":..,"error":..}  a rejected command, not retained

write (publish WITHOUT retain):
kbus/do/<n>/set                     true | false           also on/off, 1/0
kbus/ao/<n>/set                     0..65535
kbus/do/all/set                     [true,null,false]      several outputs in the SAME bus cycle; null = unchanged
kbus/ao/all/set                     [1000,null,32767]
kbus/module/<pos>/out/set           bytes (binary)         raw output data of a complex module (4.1)
kbus/module/<pos>/stepper/cmd/set   {"cmd":..}             750-671 command (4.3)
kbus/module/<pos>/reg/get           {"table":0,"register":6}  read ONE register; also with MQTT_WRITE_ENABLE=0 (4.4)
kbus/watchdog/set                   any                    heartbeat of the controlling side (4.5)
kbus/bus/cycletime/set              1..40                  stored, survives a restart
kbus/bus/rescan/set                 true                   rescan the KBUS now
kbus/bus/resetstatistics/set        true                   cycles, errors, max values and overruns back to 0
kbus/serial/com1/tx/set             1..512 bytes (binary)  send on COM1 as they are (5)
kbus/serial/com1/baudrate/set       9600                   COM1 settings: stored, survive a restart
kbus/serial/com1/format/set         8N1
kbus/serial/com1/framegap/set       20
```

Values are **raw** process-image values: for a 750-559 (0–10 V), 32767 =
10 V. Scale them in your application (exception: module values, 4.2).

### 3.2 Why this structure

- **One value per topic, plain payloads** (`true`, `1234`). Every MQTT tool
  understands it: `mosquitto_sub`, Node-RED, Home Assistant,
  Grafana/Telegraf, SCADA drivers, the CODESYS MQTT libraries. Payloads are
  valid JSON, so `true`/`false` and numbers need no extra parsing.
- **Short, lower-case, fixed levels** `<prefix>/<class>/<n>`, so wildcards
  work naturally: `kbus/di/+` = all digital inputs, `kbus/+/3` = channel 3 of
  every class, `kbus/#` = everything.
- **Commands on a separate `/set` topic**, the common MQTT convention
  (Zigbee2MQTT, Tasmota, Home Assistant). The state topic always shows the
  real value; subscribing to `kbus/#` never triggers a write, and an ACL can
  allow reading `kbus/#` while writing only to `kbus/do/+/set`.
- **Arrays next to single values** (`kbus/do/all`): one message with a
  consistent snapshot of a whole class, and `.../all/set` writes several
  outputs in one bus cycle.
- **Retained values with a last will**: a dashboard that connects later sees
  all values at once, and `kbus/status` tells whether they are current.
- **One prefix per controller.** On a broker shared by several controllers
  (or after bridging) use a unique prefix, e.g.
  `MQTT_TOPIC_PREFIX=plant1/pfc300-01/kbus`. The rest of the tree stays the
  same.

### 3.3 Examples

```bash
B="-h 127.0.0.1"                                   # on the controller
mosquitto_sub $B -t 'kbus/#' -v                    # everything
mosquitto_sub $B -t 'kbus/di/+' -v                 # all digital inputs
mosquitto_pub $B -t kbus/do/1/set -m true          # DO1 on
mosquitto_pub $B -t kbus/do/all/set -m '[true,null,true]'   # DO1 and DO3 on, DO2 unchanged, same cycle
mosquitto_pub $B -t kbus/ao/2/set -m 8000
mosquitto_pub $B -t kbus/bus/rescan/set -m true
mosquitto_sub $B -t kbus/error -v                  # rejected commands
```

Python (paho-mqtt):

```python
import paho.mqtt.client as mqtt
c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.on_message = lambda c, u, m: print(m.topic, m.payload.decode())
c.connect("127.0.0.1", 1883)
c.subscribe("kbus/di/+")
c.publish("kbus/do/1/set", "true")
c.loop_forever()
```

### 3.4 Modules and statistics

`kbus/modules` (retained), e.g. on the simulation node:

```json
[{"position":1,"name":"75x-455","typeCode":455,"channels":{"ai":[1,2,3,4]}},
 {"position":3,"name":"digital 8DI","typeCode":34817,"channels":{"di":[1,2,3,4,5,6,7,8]}},
 {"position":4,"name":"digital 8DO","typeCode":34818,"channels":{"do":[1,2,3,4,5,6,7,8]}}]
```

Complex modules show `"raw":{"in":24,"out":24}`, modules with a descriptor
`"functions":{...}` (4.2).

`kbus/bus/statistics` (retained, every `MQTT_STATS_S` seconds):

```json
{"version":"3.4.0","backend":"adi","healthy":true,"cycleTime":5,"realtime":true,
 "cycles":120000,"errors":0,"overruns":0,"cycleDuration":410,"maxCycleDuration":592,"maxWakeupDelay":46}
```

| Value | Fine at 5 ms | When exceeded |
|---|---|---|
| `maxCycleDuration` (µs) | well below 5000 | the KBUS exchange is too long for the period: raise the cycle time |
| `maxWakeupDelay` (µs) | a few 100 | other real-time threads (CODESYS) delay the cycle |
| `overruns` | 0 | cycles started late |
| `errors` | not increasing | KBUS errors |

### 3.5 Rules for writing

- Publish commands **without the retain flag**. A retained command would be
  applied again at every reconnect; kbus-gateway therefore **ignores**
  retained `.../set` messages and logs how to delete them
  (`mosquitto_pub -r -n -t kbus/do/4/set`).
- A rejected command writes nothing and is reported on `kbus/error` and in
  the log: unknown channel, wrong value, more array values than channels.
- An output keeps its value until it is written again. When the controlling
  client must be supervised, enable the **write watchdog** (4.5); without it
  outputs stay as they are when the client disappears. Outputs go to 0 at a
  stop and a rescan.
- With `MQTT_WRITE_ENABLE=0` no `/set` topics are subscribed (read only).
- Several writers on the same output: the last message wins.

### 3.6 Retained topics after changes

At every connection kbus-gateway reads the retained topics on the broker and
deletes those that do not belong to the current node: channel topics (e.g.
`kbus/di/20` of a module that was removed while the program was stopped),
`kbus/ai/<n>/status` of channels that are gone or have no register access,
and module topics (`kbus/module/<pos>/in`, `.../temperature/<n>`,
`.../stepper/state` of a module that is gone or is another type now). The
same happens after a rescan. So the broker always shows exactly the current
node, and so does the KBUS Manager page, which builds its panels from these
topics. Retained topics under a *different* prefix are not touched.

---

## 4. Module functions

### 4.1 Complex modules: raw process data (750-495 and others)

Modules without channels (750-495 power measurement, counters, serial
interfaces) get their **raw process data** as binary messages:

| Topic | Direction | Content |
|---|---|---|
| `kbus/module/<pos>/in` | kbus-gateway → | the module's input bytes (750-495: 24), all from one bus cycle, when they change, at most once per `MQTT_RAW_MIN_MS` (100 ms) per module; after a suppressed change the latest bytes always follow |
| `kbus/module/<pos>/out/set` | → kbus-gateway | exactly the module's output bytes (750-495: 24), written in one piece for the next bus cycle; without retain; any other length is rejected |

kbus-gateway does nothing with the data itself: the protocol of the module
runs in the application, e.g. the standard WAGO function block in CODESYS
used as "virtual I/O". Step-by-step guide for the 750-495 with
WagoAppPowerMeasurement: [codesys/README.md](../codesys/README.md).

- Only one application may write a module's output bytes. The KBUS Manager
  page cannot write them.
- The 750-495 delivers measured values only after the application has
  requested them through its output bytes; with output bytes 0 it only shows
  status bytes.
- Modules with a driver of their own (750-671, 4.3) refuse `out/set`; their
  `in` bytes are still published for diagnosis.

```bash
mosquitto_sub -h <pfc-ip> -t kbus/module/2/in -F "%U %x"     # time + bytes in hex
```

### 4.2 Module values (module descriptors)

Some module types get **functional values** next to their raw channels: one
descriptor per module type (`src/modules.c`) says which channels mean what,
in which unit and with which resolution:

```
kbus/module/<pos>/<function>/<n>    value in the unit of the descriptor (retained, on change)
```

| Module | Function | Value | Raw channels |
|---|---|---|---|
| 750-450 (4 RTD inputs) | `temperature/1..4` | °C, signed, 0.1 °C per digit (Pt100: −200.0 … 850.0) | stay as `kbus/ai/<n>` (raw, unsigned) |

- `kbus/modules` lists the descriptor:
  `"functions":{"temperature":{"unit":"°C","resolution":0.1}}`.
- The 750-450 is set to its sensor type with WAGO-I/O-CHECK. For a setting
  with another resolution or unit (e.g. resistance), set
  `MODULE_450_RESOLUTION` and `MODULE_450_UNIT`.
- `MQTT_ANALOG_DEADBAND` applies in digits (e.g. `2` = 0.2 °C).
- **750-657 (IO-Link master)**: no driver yet. Its handshakes (mailbox,
  control/status byte) have to run in the gateway, because broker latency
  would break them; until then it only gets its raw bytes (4.1). Planned:
  port data and port status as state, IO-Link parameters as request/response.

### 4.3 750-671 stepper controller

kbus-gateway runs the **handshake of the 750-671** itself (positioning
application, `Application_Selector = 1`, cyclic process image; manual 750-671,
2.1.2.7 and 2.1.2.11.1) at every tick (`MQTT_PUBLISH_MS`, 20 ms). MQTT carries
only commands and results, so broker latency cannot break the handshake.
There is no closed-loop control over MQTT: the module ramps and positions
itself; MQTT starts and monitors it.

| Topic | Direction | Content |
|---|---|---|
| `kbus/module/<pos>/stepper/cmd/set` | → kbus-gateway | JSON command, without retain; optional `"id"` (echoed in the response) |
| `kbus/module/<pos>/stepper/response` | kbus-gateway → | `{"cmd":..,"id":..,"result":"accepted"\|"done"\|"error","error":..}`, not retained |
| `kbus/module/<pos>/stepper/state` | kbus-gateway → | retained; flags on change, position/velocity at most every 100 ms |

| Command | What the driver does | Response |
|---|---|---|
| `{"cmd":"enable"}` | Enable + Stop2_N, acknowledges a module reset (Reset_Quit) | `done` when Ready and Stop_N_ACK; `error` e.g. "Input1 (stop circuit) is open" |
| `{"cmd":"move","position":P,"velocity":V,"acceleration":A}` | absolute position (24-bit, steps or the configured unit), V 1–25000, A 1–32767; positioning mode, setpoints, Start edge | `accepted` at Start_ACK, `done` at On_Target. A new `move` during a move changes the target on the fly |
| `{"cmd":"reference","direction":"neg"\|"pos"}` | reference run at the setup speed (Reference_Mode from the module configuration) | `done` at Reference_OK |
| `{"cmd":"stop"}` | ends the mode: the module brakes with Acceleration_Stop_Fast | `done` at standstill |
| `{"cmd":"quit"}` | Error_Quit + Reset_Quit | `done` / `error` "error still present" |
| `{"cmd":"disable"}` | Enable off; refused while the drive moves (Enable 0 stops without a ramp) | `done` when Ready is off |

```
kbus/module/4/stepper/state = {"enabled":true,"ready":true,"stopAck":true,"mode":"positioning",
  "busy":false,"onTarget":true,"standstill":true,"referenced":true,"error":false,"reset":false,
  "input1":true,"input2":false,"position":2000,"velocity":0,"command":null}
```

- Invalid commands are answered on `kbus/error` and change nothing; a
  command that fails at the module gets `result: "error"` with the reason.
- **Safe state** (write watchdog, 4.5): a **controlled stop** (mode and Start
  off, braking ramp); Enable stays on, so the motor keeps its holding current.
  The running command ends with `error`.
- **Input1** (DI1) is the module's stop circuit in the default configuration:
  open = no start possible.
- Not supported: jog mode, move programs, the mailbox (configuration tables)
  and the velocity application.

### 4.4 Register reads (status of analog inputs)

kbus-gateway reads module registers through the **register communication of
the firmware** (`ldkc_KbusRegisterRequest_Read`; libpackbus serves it over
D-Bus inside the gateway process, so it works only while kbus-gateway owns
the KBUS). **Read only**: writing registers is deliberately not built (it
changes the module configuration, e.g. the sensor type in the 750-450's
EEPROM; use WAGO-I/O-CHECK for that).

Why not in the process image: on the PFC the KBUS process image of a 750-450
holds only the 4 data words (8 bytes in, 0 out); the status/control bytes of
a fieldbus coupler image do not exist there. The register table of each
channel does contain the status.

| Topic | Direction | Content |
|---|---|---|
| `kbus/ai/<n>/status` | kbus-gateway → | register 6 of AI channel n as an integer; retained, on change and after every (re)connection |
| `kbus/module/<pos>/reg/get` | → kbus-gateway | `{"table":0,"register":6,"id":"x"}`, without retain; `id` optional (string or integer, echoed) |
| `kbus/module/<pos>/reg/value` | kbus-gateway → | `{"table":0,"register":6,"value":96,"ok":true,"id":"x"}`, not retained; on error `{...,"ok":false,"error":"read failed","id":"x"}` |

- **Table** = channel on the module − 1 (0…3 for a 4-channel module),
  **register** 0…63. One register per request.
- `ai/<n>/status`: register 6 of every AI channel is read **at most once per
  second per channel** (`MQTT_AI_STATUS_MS`, minimum 1000, 0 = off). A module
  whose first read fails (digital modules, modules without register access)
  gets no status topics until the next bus scan. A later failed read keeps
  the last value (logged once).
- Measured on the test node (750-450, Pt100 setting): `0` on a channel with a
  valid temperature, `96` (0x60, bits 5 and 6) on open inputs that show
  850.0 / 2500.0 °C. **The meaning of the bits is not yet confirmed against
  the 750-450 manual**: treat 0 as "no error" and anything else as "see the
  module documentation".
- Errors: `no module at this position`, `digital module: no registers`,
  `expected "table": 0..255`, `expected "register": 0..63`, `busy: too many
  requests` (more than 16 waiting), `read failed` (e.g. a table the module
  does not have), `bus not running` (during a rescan), `register access not
  available` (firmware library or SDK header without register communication).
- **Timing**: a read takes about 20 ms. Reads run in a thread of their own
  with normal priority (not in the KBUS cycle, not in the MQTT thread);
  requests go before the status reads; a rescan waits for a running read.
  The process data (`kbus/ai/<n>`, temperatures) is not affected.

```bash
mosquitto_sub -h <pfc> -t 'kbus/ai/+/status' -v
mosquitto_sub -h <pfc> -t 'kbus/module/3/reg/value' &
mosquitto_pub -h <pfc> -t kbus/module/3/reg/get -m '{"table":1,"register":6,"id":"a"}'
```

### 4.5 Write watchdog

With `MQTT_WATCHDOG_S=N` kbus-gateway supervises the controlling side (e.g.
CODESYS): when it has received **no write and no `kbus/watchdog/set` for N
seconds**, it switches **all outputs to 0**: every DO and AO, the raw output
bytes of complex modules (750-495, 750-657) and a controlled stop of a
750-671. This is the safe state for when the client, its network or the
broker disappears.

- The watchdog starts with the first write or heartbeat (outputs are 0 after
  the start anyway). Publish `kbus/watchdog/set` (any payload, **without**
  retain) at least every N/2 s when the outputs do not change that often.
- `kbus/watchdog` (retained): `ok`, or `expired` after it switched the
  outputs off. The next write or heartbeat sets it back to `ok`; outputs are
  not restored, the client writes them again.
- kbus-gateway's own failure is covered as well: `kbus/status` = `offline`
  (last will), and the output modules switch off by themselves 50 ms after
  the KBUS cycle stops.

---

## 5. Serial interface COM1

kbus-gateway also owns the serial interface **COM1** of the controller
(`/dev/serial`) and forwards it **transparently**: it does not interpret the
bytes. The protocol of the connected device (Modbus RTU, a barcode reader, a
meter, ...) is implemented in the client, e.g. CODESYS.

| Topic | Direction | Content |
|---|---|---|
| `kbus/serial/com1/rx` | kbus-gateway → | **one message per received frame**, binary, not retained |
| `kbus/serial/com1/tx/set` | → kbus-gateway | 1–512 bytes, sent as they are (binary); without retain |
| `kbus/serial/com1/open` | kbus-gateway → | `true` / `false`: device open (retained) |
| `kbus/serial/com1/baudrate`, `format`, `framegap` | kbus-gateway → | the current settings (retained) |
| `kbus/serial/com1/baudrate/set`, `format/set`, `framegap/set` | → kbus-gateway | new settings, applied at once, stored in `KBUS_SETTINGS_STORE`; without retain |
| `kbus/serial/com1/statistics` | kbus-gateway → | `{"txBytes":..,"rxBytes":..,"frames":..,"dropped":..,"errors":..}`, retained, on change (at most once per second) |

- **Frames.** A pause of `framegap` ms (default 10) after the last received
  byte ends a frame. Choose it larger than the gaps *inside* a frame of the
  device and smaller than the pause between two frames (Modbus RTU: 3.5
  characters, i.e. 4 ms at 9600 baud; 10 ms is safe up to 9600 baud). A frame
  longer than 512 bytes is split.
- **Latency.** Frames go into a queue of 64; the MQTT thread publishes all of
  them at every tick (20 ms). A frame reaches the broker about framegap +
  ≤ 20 ms after its last byte. When the queue is full (no broker connection)
  the oldest frame is dropped and counted in `dropped`. Frames received while
  there is no broker connection are discarded at the reconnect.
- **Settings are made by the client**: it publishes `baudrate/set` (1200,
  2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400), `format/set` (data
  bits 5–8, parity `N`/`E`/`O`, stop bits 1/2, e.g. `8E1`) and `framegap/set`
  (1–1000 ms), preferably only when they differ from the retained current
  values. A value written there wins over `SERIAL_BAUD`, `SERIAL_FORMAT`,
  `SERIAL_FRAME_GAP_MS` and survives a restart. Invalid values are answered
  on `kbus/error`. The KBUS Manager page only shows them.
- **RS-232 / RS-485** is chosen in the WBM (*Interfaces > Serial Interface*);
  the owner of the interface must be **None**, so that kbus-gateway can open
  it (exclusively). The RS-485 direction is switched by the driver.
- **Off**: `SERIAL_DEVICE=""` (default in simulation): no serial topics;
  retained ones of an earlier run are deleted. With `MQTT_WRITE_ENABLE=0` no
  serial `/set` topics are subscribed.

```bash
mosquitto_sub -h <pfc-ip> -t 'kbus/serial/com1/#' -v                     # settings, statistics, frames
mosquitto_sub -h <pfc-ip> -t kbus/serial/com1/rx -F "%U %x"              # frames in hex with a time stamp
printf '\x01\x03\x00\x00\x00\x02\xc4\x0b' | mosquitto_pub -h <pfc-ip> -t kbus/serial/com1/tx/set -s   # binary
mosquitto_pub -h <pfc-ip> -t kbus/serial/com1/baudrate/set -m 9600
```

---

## 6. KBUS Manager page

`http(s)://192.168.1.17/kbus` is the **KBUS Manager**, served by the optional
**mqtt-manager** container, in the look of the controller's Web-based
Management (WBM): tab menu, sub menu, collapsible panels, status bar with
LEDs, the WBM's fonts and logo, a link to the WBM (top right). The old address
`/mqtt` redirects to `/kbus`.

```bash
tools/deploy-docker.sh 192.168.1.17 --mosquitto --manager --no-gateway
```

### 6.1 Pages

| Tab | Page | Content |
|---|---|---|
| Status | COM1 | communication parameters (state, baud rate, format, frame gap), **read only** (set by the client); monitor: bytes, frames, dropped, errors, the last 10 received frames, test send |
| | KBUS | state, kbus-gateway online/offline, cycle time, cycles, errors, configuration version, version, write watchdog; timing (exchange, max. exchange, max. wake-up delay, overruns, real-time); *Reset Statistics* (login), *Rescan KBUS* |
| | Modules | `kbus/modules`: position, name, type code, channels |
| MQTT | Topics | every topic received (`kbus/#`, `bridge/status`, `bridge/+/status`) with value and time, filter; all DI/DO/AI/AO, the 750-450 temperatures and the 750-671 state. Command topics (`…/set`) are not listed |
| | Clients | MQTT clients connected to the broker (from mosquitto's connection log; a client from 127.0.0.1 is shown as CODESYS), state of the mqtt-bridge and its remote brokers |
| Settings | KBUS | cycle time, 1–40 ms |
| | Login | website login on/off, user, password |

Status bar LEDs: **KBUS** (running / error), **GW** (`kbus/status`), **MQTT**
(WebSocket to the broker), **WR** (writes allowed / `MGMT_READONLY=1`),
**Login**.

**Switching outputs**: log in, *Take Control* on *MQTT > Topics*, click the
value of `kbus/do/<n>` (`true`/`false`) or `kbus/ao/<n>` (0–65535), type,
Enter (Esc = cancel). Control is **temporary**: on *Release Control*, logout
or closing the page, every output written under control goes back to
`false` / `0`.

### 6.2 How it is connected

```
 browser ─ http(s)://<pfc>/kbus/          ─▶ WAGO web server ─▶ mqtt-manager 127.0.0.1:9080 (page + API)
         ├ ws(s)://<pfc>/kbus/ws          ─▶ WAGO web server ─▶ mosquitto    127.0.0.1:9001 (MQTT, READ ONLY)
         └ POST /kbus/api/publish ─▶ mqtt-manager: login check, topic + value check ─▶ mosquitto :1883
```

- **Reads** are live over MQTT/WebSocket (no polling). The WebSocket listener
  of the broker is read only (`docker/mosquitto/acl-monitor`).
- **Writes** go through the mqtt-manager, which checks them and publishes on
  port 1883: `do/<n>/set`, `ao/<n>/set` (only with login), `bus/cycletime/set`,
  `bus/rescan/set`, `bus/resetstatistics/set`, `serial/com1/tx/set` (test
  send); nothing else (no raw module data, no COM1 settings, nothing outside
  the prefix).
- **No extra port**: the page uses the WBM's own web server (port 80/443, its
  certificate; HTTP follows the WBM's web-services mode). The mqtt-manager
  adds `/etc/lighttpd/apps.confd/mqtt.conf`, WAGO's own extension point. It
  lets lighttpd check the whole configuration (`lighttpd -tt`) before
  reloading and removes a rejected file at once, so the WBM is never left
  with a broken configuration. A reload restarts lighttpd (the WBM is away
  for about a second, only when the file changes).
- **Kept automatically**: the mqtt-manager checks the file every minute and
  puts it back when it is missing or different (e.g. after a firmware
  update). The container runs with `--restart unless-stopped`.
- The container needs `--privileged --pid host`: it writes the file and
  reloads the web server on the controller (`nsenter`). The reload runs in the
  controller's own cgroups, so lighttpd never belongs to the container
  (otherwise Docker would stop the WBM's web server together with the
  container).

### 6.3 Login

- The website is open by default; *Login required* on *Settings > Login*
  closes it.
- Writing outputs (*Take Control*), *Reset Statistics* and changing the login
  settings **always** need the login (gear button, top right): default
  `admin` / `wago` until a password is set. **Change it.**
- Stored in `/home/mqtt-manager/settings.json` on the controller (password as
  PBKDF2 hash).
- The login protects the page only: port 1883 of the broker has no login, so
  anyone on the network can still read and write over MQTT directly
  (section 11).

### 6.4 Options and removal

Environment (`MG_ENV` of `tools/deploy-docker.sh`, e.g.
`MG_ENV="-e MGMT_READONLY=1"`):

| Variable | Default | |
|---|---|---|
| `MGMT_READONLY` | `0` | `1` = no writes at all |
| `MQTT_TOPIC_PREFIX` | `kbus` | when kbus-gateway uses another prefix |
| `MQTT_USER`, `MQTT_PASSWORD` | – | broker with login |
| `WEB_PATH` | `/kbus` | address of the page |
| `OLD_WEB_PATH` | `/mqtt` | redirected to `WEB_PATH`; `""` = no redirect |

**Remove**: `docker rm -f mqtt-manager`, then
`docker run --rm --privileged --pid host mqtt-manager:arm64 --unlink`.
Without the second command `/kbus` stays linked and answers 503.

Do not deploy the KBUS Manager of the HTTP/wdx gateway on the same
controller: both use `/kbus`. Not available over MQTT compared with that
version: *Local Control* (blocking all other writers) and the *Parameters* /
*Registers* pages; the *MQTT* tab replaces them.

---

## 7. Other systems: mqtt-bridge, IEC 104, Modbus TCP

Three optional containers connect the broker of the PFC to other systems.
They only talk MQTT, so they work with or without kbus-gateway.

### 7.1 mqtt-bridge: topics from other brokers

The container [docker/mqtt-bridge](../docker/mqtt-bridge/README.md) connects
to one or more **remote brokers**, subscribes to topics there and publishes
every message on the broker of the PFC. Applications on the PFC (CODESYS,
Node-RED, containers) then find external data on their local broker, next to
`kbus/#`.

```
 broker.plant.local ──site1/setpoints/#──┐
 cloud (TLS, cert) ──weather/+──────────┼─▶ mqtt-bridge ──▶ mosquitto on the PFC: plant/..., cloud/...
 192.168.1.50 ─────sensors/#────────────┘                    bridge/<name>/status online|offline
```

```bash
tools/deploy-docker.sh 192.168.1.17 --bridge --no-gateway
ssh root@192.168.1.17 vi /home/mqtt-bridge/mqtt-bridge.ini    # add [remote ...] sections
ssh root@192.168.1.17 docker restart mqtt-bridge
ssh root@192.168.1.17 docker logs -f mqtt-bridge
```

One section per remote broker:

```ini
[remote plant]
host = broker.plant.local
port = 8883
tls = true
ca_file = plant-ca.crt          ; in /home/mqtt-bridge/ on the controller
username = pfc300-01
password = secret
qos = 1
strip_prefix = site1/
topics =
    site1/setpoints/#  -> plant          ; site1/setpoints/line1/speed -> plant/setpoints/line1/speed
    site1/weather/+    -> plant
    site1/hall/temp    -> hall/temperature   ; no wildcard: exactly this local topic
```

- Remote topic `T` becomes `<local>/T` (after removing `strip_prefix`); a
  filter without wildcards and a local topic without `/` at the end is
  renamed to exactly that topic.
- `retain = always` (default): every forwarded message is retained locally,
  so a local subscriber always gets the last value. `keep` passes the remote
  retain flag, `never` retains nothing (use it for commands).
- TLS with CA file or system CAs, client certificates (`cert_file`,
  `key_file`), user/password, MQTT 3.1.1 or 5, TCP or WebSockets.
- Connection loss: automatic reconnect (1–30 s) per broker; messages for the
  local broker are queued while it is not reachable (QoS 1/2).
- Status on the local broker (retained): `bridge/status` (the bridge itself,
  last will) and `bridge/<name>/status` (each remote broker): `online` /
  `offline`.

A remote topic mapped into the kbus tree (e.g. `... -> kbus/do/1/set`)
switches an output: this is how a remote system can control the PFC's I/O.
Do it on purpose only, with `retain = never`.

All settings: the comments in
[docker/mqtt-bridge/mqtt-bridge.ini](../docker/mqtt-bridge/mqtt-bridge.ini).

The other direction (KBUS data **to** a central broker or cloud) is best done
with mosquitto's own bridge function in `mosquitto.conf` (`connection`,
`address`, `topic kbus/# out 1 "" plant1/pfc300-01/`), which also buffers
messages while the connection is down.

### 7.2 mqtt-iec104: IEC 60870-5-104 gateway

The optional container [docker/mqtt-iec104](../docker/mqtt-iec104/README.md)
is an **IEC 104 server** on the PFC (TCP 2404) whose data comes from MQTT. A
SCADA system that speaks IEC 104 reaches the KBUS I/O directly:

```
 SCADA ── IEC 104 :2404 ──▶ mqtt-iec104 ◀── MQTT ──▶ mosquitto ◀──▶ kbus-gateway ◀──▶ KBUS
   points    ◀── kbus/di/<n>, kbus/do/<n>, kbus/ai/<n>, kbus/module/<pos>/temperature/<n>, ...
   commands  ──▶ kbus/do/<n>/set, kbus/ao/<n>/set, ...
```

```bash
tools/deploy-docker.sh 192.168.1.17 --iec104 --no-gateway
ssh root@192.168.1.17 vi /home/mqtt-iec104/mqtt-iec104.yaml     # the mapping
ssh root@192.168.1.17 docker restart mqtt-iec104
```

The mapping is a YAML file; a series of channels is one entry:

```yaml
points:                          # MQTT -> IEC 104
  - topic: kbus/di/{n}           # kbus/di/1..8 -> IOA 101..108, M_SP_TB_1
    count: 8
    ioa: 101
    type: single                 # single | float | scaled
commands:                        # IEC 104 -> MQTT
  - topic: kbus/do/{n}/set       # C_SC_NA_1 on IOA 1101..1108 -> true / false
    count: 8
    ioa: 1101
    type: single                 # single | float | scaled
```

- Points are sent spontaneously on every change and answered on a general
  interrogation; quality *Invalid* until the first value, while MQTT is
  disconnected and while `kbus/status` is not `online`.
- Commands are confirmed positively when published; negatively when the
  broker or kbus-gateway is not available or a set point is outside
  `min`/`max`.
- Scale and offset per entry (raw ⇄ engineering units), JSON fields (`key`),
  select-before-operate, status topic `iec104/status`.
- arm64 (PFC300) only. **No login on port 2404**: whoever reaches it can send
  the configured commands (section 11).

All options, behaviour and the test: [docker/mqtt-iec104/README.md](../docker/mqtt-iec104/README.md).

### 7.3 mqtt-modbus: Modbus TCP devices

The optional container [docker/mqtt-modbus](../docker/mqtt-modbus/README.md)
is a **Modbus TCP client** that polls devices (energy meters, inverters,
drives, other controllers) and publishes their values on the broker; values
published on `/set` topics are written to the device.

```
 Modbus TCP devices ◀── poll ── mqtt-modbus ──▶ mosquitto: modbus/<device>/<tag>, .../status
                    ◀── write ──              ◀── modbus/<device>/<tag>/set
```

```bash
tools/deploy-docker.sh 192.168.1.17 --modbus --no-gateway
ssh root@192.168.1.17 vi /home/mqtt-modbus/mqtt-modbus.yaml     # devices and tags
ssh root@192.168.1.17 docker restart mqtt-modbus
```

```yaml
devices:
  - name: meter
    host: 192.168.1.50
    interval: 1
    tags:
      - name: power                 # modbus/meter/power
        address: 12                 # holding register 12..13
        type: int32
        scale: 0.001
      - name: relay{n}              # modbus/meter/relay1..4, write with .../set
        count: 4
        table: coil
        address: 0
        write: true
```

- Several devices in parallel, one connection and poll interval each;
  adjacent registers are read in one request.
- All common data types (16/32/64-bit integers, float32/64, bits), word and
  byte order, scale/offset; values published on change.
- Writes with FC5 (coils), FC6 (16-bit) and FC16 (32/64-bit), range checks,
  errors on `modbus/error`; retained commands are ignored.
- `<topic>/status` per device, `modbus/status` for the bridge (last will).

All options, behaviour and the test: [docker/mqtt-modbus/README.md](../docker/mqtt-modbus/README.md).

---

## 8. Configuration

`/etc/kbus-gateway.conf` on the PFC (environment variables for the program;
apply with `/etc/init.d/kbus-gateway restart`). The container takes the same
variables through `GW_ENV` (2.8).

**MQTT**

| Variable | Default | Description |
|---|---|---|
| `MQTT_HOST` | `127.0.0.1` | broker address |
| `MQTT_PORT` | `1883` | broker port (plain TCP) |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | – | login; empty = anonymous |
| `MQTT_CLIENT_ID` | `kbus-gateway-<hostname>` | must be unique on the broker |
| `MQTT_KEEPALIVE_S` | `30` | keep-alive, 5–3600 s; a dead connection is noticed after about 1.5× this |
| `MQTT_TOPIC_PREFIX` | `kbus` | first topic level(s), e.g. `plant1/pfc300-01/kbus` (no wildcards) |
| `MQTT_RETAIN` | `1` | `1` = value topics are retained (`status`, `modules`, `bus/*` always are) |
| `MQTT_PUBLISH_MS` | `20` | changes are looked for every N ms, 5–10000 |
| `MQTT_ANALOG_DEADBAND` | `0` | AI/AO are published when the raw value changed by at least N (0 = every change); noisy inputs: e.g. `50` |
| `MQTT_ARRAYS` | `1` | `1` = also `<prefix>/<class>/all` |
| `MQTT_STATS_S` | `10` | `bus/statistics` every N s, 0 = off |
| `MQTT_REPUBLISH_S` | `0` | publish all values again every N s (for clients that need a heartbeat), 0 = only changes |
| `MQTT_WRITE_ENABLE` | `1` | `0` = read only: no `/set` topics |
| `MQTT_LOG` | `1` | 0 none, 1 writes that change an output and bus commands, 2 every received message |
| `MQTT_RAW_MIN_MS` | `100` | `module/<pos>/in` at most once per N ms per module (4.1); 0 = every change |
| `MQTT_AI_STATUS_MS` | `1000` | `ai/<n>/status` (4.4): register 6 of each AI channel at most once per N ms; minimum 1000, 0 = off (`reg/get` keeps working) |
| `MQTT_WATCHDOG_S` | `0` | write watchdog (4.5): no write and no `watchdog/set` for N s → all outputs 0; 0 = off |
| `MQTT_PRIORITY` | `0` (package: `30`) | SCHED_FIFO priority of the MQTT thread, 0 = normal. Keep it **below** `KBUS_PRIORITY` |

**KBUS and modules**

| Variable | Default | Description |
|---|---|---|
| `KBUS_CYCLE_MS` | `5` | KBUS cycle, 1–40 ms (well below the 50 ms output watchdog). A value written to `bus/cycletime/set` wins |
| `KBUS_PRIORITY` | `40` | SCHED_FIFO priority of the KBUS cycle thread, 0 = normal |
| `KBUS_SETTINGS_STORE` | `/home/kbus-gateway/kbus-settings.conf` | cycle time and COM1 settings written over MQTT; delete the file to use `KBUS_CYCLE_MS` / `SERIAL_*` again; empty = not stored |
| `MODULE_450_RESOLUTION`, `MODULE_450_UNIT` | `0.1`, `°C` | 750-450 value per digit and unit (4.2), for other sensor settings |
| `KBUS_DAL_LIB`, `KBUS_INFO_LIB` | `libdal.so:...` | `dlopen()` candidates of the WAGO libraries |
| `KBUS_DIGITAL_OFFSET_MODE` | `auto` | `auto`, `relative` or `absolute` (digital data offset in the process image) |

**COM1**

| Variable | Default | Description |
|---|---|---|
| `SERIAL_DEVICE` | `/dev/serial` (simulation: `""`) | COM1 device; `""` = COM1 off. When set explicitly it is also used in simulation (e.g. a pty for tests) |
| `SERIAL_BAUD` | `19200` | 1200 … 230400 (standard rates); `baudrate/set` wins |
| `SERIAL_FORMAT` | `8E1` | data bits 5–8, parity N/E/O, stop bits 1/2; `format/set` wins |
| `SERIAL_FRAME_GAP_MS` | `10` | 1–1000: a pause this long ends a received frame; `framegap/set` wins |

**Other**

| Variable | Default | Description |
|---|---|---|
| `LOG_TARGET` | stderr (package: `syslog`) | `syslog` = `/var/log/messages` |
| `KBUS_SIMULATION` | `0` | `1` = simulated node, no hardware (development) |
| `KBUS_SIM_HOTPLUG` | – | simulation only: while this file exists, an extra module (750-430 8DI) is "plugged in" |

**Load.** With `MQTT_PUBLISH_MS=20` and no changes the MQTT thread only
compares a few hundred bytes 50 times per second. Every changed channel is one
small message (two with `MQTT_ARRAYS=1`). Fast-changing analog inputs produce
up to 50 messages per second each; use `MQTT_ANALOG_DEADBAND` or a longer
`MQTT_PUBLISH_MS` when the subscribers do not need that.

---

## 9. Development

### 9.1 Project layout

```
├── README.md, LICENSE, VERSION, Makefile
├── src/                       kbus-gateway in C (9.2)
├── sdk/                       SDK headers (imported, 2.3) + OsCommon.h stand-in
├── packaging/ipk/             native package: init script, config file, opkg scripts
├── docker/                    containers, one folder each (docker/README.md)
│   ├── gateway/               kbus-gateway as a container (2.8)
│   ├── mosquitto/             kbus-mosquitto: the broker with its configuration (2.2)
│   ├── mqtt-bridge/           topics of other brokers -> PFC broker (7.1)
│   ├── mqtt-iec104/           IEC 60870-5-104 server <-> MQTT (7.2)
│   ├── mqtt-modbus/           Modbus TCP devices <-> MQTT (7.3)
│   └── mqtt-manager/          KBUS Manager page at /kbus + API (6)
├── web/                       the KBUS Manager page: index.html, kbus.js, kbus.css
├── codesys/                   using the KBUS from CODESYS through MQTT
├── tools/
│   ├── build-ipk.sh           build the .ipk with the SDK toolchain (2.4)
│   ├── deploy.sh              native: build + copy + opkg install + status
│   ├── deploy-docker.sh       containers: kbus-gateway, mosquitto, mqtt-bridge, mqtt-manager, mqtt-iec104, mqtt-modbus, or back to native
│   └── import-sdk-*.sh        SDK headers (2.3)
├── tests/
│   ├── test-mqtt.py           simulation, COM1, watchdog; real node read-only / write
│   ├── test-iec104.py         mqtt-iec104 end to end (gateway + IEC 104 client)
│   └── test-modbus.py         mqtt-modbus end to end (bridge + Modbus TCP server)
├── docs/
│   └── MANUAL.md              this manual
│
│   generated (not in git): build/, build-ipk/, build-docker/
```

### 9.2 Source structure

Plain C (gnu11), no dependencies besides glibc. The KBUS cycle thread and the
MQTT thread share only the process images, behind one mutex with priority
inheritance.

| File | Role |
|---|---|
| `src/main.c` | startup order, signals, memory locking |
| `src/config.c/.h` | environment variables → `gw_config_t` |
| `src/backend_adi.c` | real KBUS: `dlopen` of `libdal`, ADI device scan, KBUS-info, cycle |
| `src/backend_sim.c` | simulated node (12 DI, 8 DO, 8 AI, 4 AO; DI1–8 follow DO1–8, AI4 follows AO1; a 750-450 with AI5–8, AI8 follows AO2; a model of the 750-671 positioning handshake) |
| `src/gateway.c/.h` | channel map, process images, real-time cycle thread, rescans, bulk read/write under one lock |
| `src/mqtt_client.c/.h` | MQTT 3.1.1 client: connect with last will, QoS 0 publish (buffered), subscribe, keep-alive |
| `src/mqtt_io.c/.h` | topic tree, change detection, `/set` commands, stale topic cleanup, reconnects |
| `src/modules.c/.h` | module descriptors: functional values of a module type (750-450 temperatures) |
| `src/stepper.c/.h` | 750-671 driver: positioning handshake, commands, state, controlled stop |
| `src/regcom.c/.h` | register reads (read only) in a thread of their own: AI status register 6, `reg/get` queue |
| `src/serial.c/.h` | COM1: open, settings (termios), receive thread with frame gap and frame queue, transmit |
| `src/json.c/.h` | minimal JSON for commands (`"key":"text"`, `"key":number`) |
| `src/log.c/.h` | stderr or syslog |

### 9.3 Development loop (on the PC, no PFC needed)

```bash
docker run -d --name mosquitto-dev -p 1883:1883 eclipse-mosquitto:2.0.20 \
    mosquitto -c /mosquitto-no-auth.conf                       # broker for development
make run-sim                                                   # simulated node, broker 127.0.0.1:1883
pip install paho-mqtt
tests/test-mqtt.py 127.0.0.1 sim                               # every KBUS topic and command
tests/test-mqtt.py 127.0.0.1 serial   --gateway build/kbus-gateway   # COM1 on a virtual serial port (starts the gateway itself)
tests/test-mqtt.py 127.0.0.1 watchdog --gateway build/kbus-gateway   # write watchdog (starts the gateway itself)
tools/deploy.sh 192.168.1.17                                   # then: build .ipk, install on the PFC
```

Without `sdk/include/dal` the native build is simulation-only, which is
enough for all MQTT work. The VS Code tasks (*Terminal → Run Task*) cover the
same steps.

### 9.4 Common changes

- **Topics.** Names and payloads are in [src/mqtt_io.c](../src/mqtt_io.c):
  `pub_value()`, `pub_array()`, `pub_modules()`, `pub_stats()`; commands in
  `on_message()`, `on_output()`, `on_bus()`. Update `tests/test-mqtt.py` and
  section 3 of this manual.
- **Another module type.** Module classification: `build_channel_map()` in
  [src/gateway.c](../src/gateway.c); functional values: a descriptor in
  [src/modules.c](../src/modules.c).
- **The KBUS Manager page.** `web/` and `docker/mqtt-manager/`; redeploy with
  `tools/deploy-docker.sh <pfc> --manager --no-gateway`.
- **Release.** Raise [VERSION](../VERSION), `tools/build-ipk.sh arm64` (or
  `tools/deploy-docker.sh`), install. Old archives in `build-docker/` and
  `build-ipk/` can be deleted.

### 9.5 Rules to keep

- Never do slow work (I/O, logging, allocation) in `cycle_thread()`: every
  cycle must finish well within 50 ms, or the output modules switch off.
- Keep `MQTT_PRIORITY` below `KBUS_PRIORITY`.
- Read and write channels only through the `gateway_*` functions (locking).
- `gateway_rescan()` only from the MQTT thread: it is the only reader of the
  channel map besides the cycle thread.
- Handshakes of complex modules belong in the gateway (like the 750-671
  driver), never in an MQTT client: broker latency breaks them.
- Build against headers with the **same DAL version** as the PFC's
  `libdal.so`, with the toolchain of the controller's firmware SDK.

---

## 10. Troubleshooting

**Broker and MQTT**

| Symptom | Cause / fix |
|---|---|
| `MQTT broker 127.0.0.1:1883: ... Connection refused (retrying)` | mosquitto not running or listening elsewhere: `docker ps`, `netstat -tln \| grep 1883`, `MQTT_HOST`/`MQTT_PORT` |
| `broker refused the connection: not authorized` / `bad user name or password` | the listener needs a login: `MQTT_USERNAME`, `MQTT_PASSWORD` |
| `the broker refused a subscription (ACL?)` | the ACL does not allow the user to subscribe to `kbus/+/+/set`: no writes possible |
| Values arrive, writes do nothing | `MQTT_WRITE_ENABLE=0`, wrong prefix, the state topic (`kbus/do/1`) used instead of `kbus/do/1/set`, or see `kbus/error` |
| `ignoring retained command ...` | something published a `/set` with retain: delete it as the log line says |
| Connection drops every few seconds | two clients with the same `MQTT_CLIENT_ID` (e.g. native service and container): the broker drops the older one |
| `kbus/status` is `offline` while the program runs | it is reconnecting (log), or another prefix is configured |
| Old channel topics stay | retained topics of a different prefix are not cleaned; delete with `mosquitto_pub -r -n -t <topic>` |
| PC cannot connect to the broker | port 1883 blocked by the PFC firewall, or a `mosquitto.conf` with `listener 1883 127.0.0.1` |
| `kbus/watchdog` = `expired` | the controlling client sent no write or heartbeat for `MQTT_WATCHDOG_S` s (4.5) |

**KBUS**

| Symptom | Cause / fix |
|---|---|
| `opening the KBUS device failed` | another program owns the KBUS: runtime not *None*, a CODESYS project with KBUS, an old gateway container, or the container and the native service at once |
| `bus/statistics`: `overruns` increasing | the cycle time does not hold on this node: write a higher value to `kbus/bus/cycletime/set` |
| Cycle time ignores `KBUS_CYCLE_MS` | a value written via `bus/cycletime/set` is stored in `/home/kbus-gateway/kbus-settings.conf` and wins: delete it, restart |
| `could not load the ADI/DAL library` | check `ls /usr/lib/libdal*`, set `KBUS_DAL_LIB` |
| `this binary was built WITHOUT the WAGO ADI/DAL headers` | import the headers (2.3) and rebuild |
| `version 'GLIBC_2.xx' not found` | built with a newer toolchain than the firmware: use the SDK builder image |
| Channel numbers point to the wrong I/O | compare the startup module list with the node; try `KBUS_DIGITAL_OFFSET_MODE=relative` or `absolute` |
| New modules not visible | look for `KBUS rescan` in the log; `kbus/bus/rescan/set` = `true` |
| `reg/value`: `register access not available` | the SDK header `ldkc_kbus_register_communication.h` was missing at build time (2.3), or the firmware library has no register communication |

**COM1**

| Symptom | Cause / fix |
|---|---|
| `COM1: cannot open /dev/serial` / `serial/com1/open` = `false` | the serial interface has another owner in the WBM (*Interfaces > Serial Interface*: set **None**), another program uses it, or the container has no `/dev/serial` (deploy again) |
| Frames split in two, or two frames in one `rx` message | `framegap` too small / too large for the device (5) |
| `dropped` increasing | the broker connection was away, or the frames come faster than they can be published |

**KBUS Manager page and mqtt-bridge**

| Symptom | Cause / fix |
|---|---|
| `https://<pfc>/kbus` gives 404 | the mqtt-manager is not running or has not linked yet: `docker logs mqtt-manager` |
| `https://<pfc>/kbus` gives 503 | the mqtt-manager was removed but the link is still there: `--unlink` (6.4), or start it again |
| Page: "connecting…" / "offline" | the broker (kbus-mosquitto, WebSocket listener 127.0.0.1:9001) is not running |
| Page: "kbus-gateway offline" | kbus-gateway is stopped or cannot reach the broker; the page shows its last values |
| mqtt-bridge: `[name] ... refused the connection` / TLS errors | login, `ca_file`, port 8883 vs 1883; `log_level = DEBUG` shows every message |
| mqtt-iec104 container stops at once | configuration error: `docker logs mqtt-iec104` names it; check with `--check` (7.2) |
| IEC 104: all points *Invalid* | no value received yet, broker not connected, or `kbus/status` is not `online` (kbus-gateway stopped) |
| IEC 104: commands get a negative confirmation | broker or kbus-gateway not available, or the set point is outside `min`/`max`: `docker logs mqtt-iec104` |
| IEC 104: SCADA cannot connect | container not running, PFC firewall blocks TCP 2404, wrong common address |
| mqtt-modbus: `<device>: offline (cannot connect ...)` | wrong `host`/`port`, device not reachable from the PFC, or it accepts fewer TCP connections |
| mqtt-modbus: `reading holding 0..30 failed: ... illegal data address` | a gap in a block read does not exist on the device: lower `max_gap` (e.g. 0) |
| mqtt-modbus: wrong values for 32/64-bit tags | `word_order` / `byte_order` of the device (7.3) |

---

## 11. Limitations and security

**Security**

- **Whoever can publish to `kbus/do/+/set` / `kbus/ao/+/set` switches
  outputs.** The supplied broker configuration accepts MQTT **without login
  on every interface**: everyone who can reach port 1883 of the controller
  can read all I/O and switch outputs. That is fine for a test setup or an
  isolated machine network. Otherwise:
  - add user names, passwords and an ACL (end of
    `docker/mosquitto/mosquitto.conf`);
  - use `MQTT_WRITE_ENABLE=0` when no outputs need to be written;
  - block port 1883 in the PFC firewall for other networks;
  - use TLS on networks you do not control (mosquitto bridge or mqtt-bridge).
- **IEC 104 (mqtt-iec104) has no login either**: whoever reaches TCP 2404
  can send the configured commands. Allow 2404 only from the SCADA system and
  map only the commands it needs.
- **Modbus TCP has no security**: mqtt-modbus writes whatever arrives on a
  `/set` topic of a tag with `write: true`. Enable `write` only where needed
  and use `min`/`max`.
- The KBUS Manager page writes only through the mqtt-manager (topic
  whitelist, login). Change the default login `admin` / `wago`.

**Limitations**

- **Only one KBUS master**: kbus-gateway and a CODESYS application with KBUS
  access cannot run at the same time.
- Only plain digital modules and plain analog modules (16-bit words, data in
  one direction) are mapped to channels, with raw values. Other modules are
  reached through their raw bytes (4.1), a module descriptor (4.2) or a
  driver (4.3). The 750-657 has no driver yet.
- kbus-gateway speaks plain MQTT 3.1.1 over TCP with QoS 0, meant for the
  broker on the same controller: on `127.0.0.1` nothing gets lost, and a lost
  connection is followed by a full publish of all values. TLS to other
  brokers is the job of mosquitto (bridge) or the mqtt-bridge.
- Without the write watchdog (4.5), outputs keep their last value when the
  writing client disappears.
- MQTT is fast but not hard real-time; for hard real-time I/O let CODESYS
  drive the KBUS directly.
- On `stop` and `opkg remove` all outputs are switched to 0. If the program
  crashes, the output modules switch off on their own after 50 ms.

---

## 12. Test status and history

### 12.1 Tested

| Where | What |
|---|---|
| PC, simulation node, mosquitto 2.0.20 | `tests/test-mqtt.py sim` (all classes, arrays, invalid commands, cycle time, rescan), removal of stale retained topics, ignored retained command, broker restart (reconnect), clean stop and `kill -9` (last will), simulated hot-plug, chain remote broker → mqtt-bridge → kbus-gateway → DO1; COM1 with a virtual serial port (pty); write watchdog |
| PFC300 750-8302, FW 04.09.01(31), node 8DO + 750-495 (18-11-2026) | all as containers next to CODESYS without KBUS: 0 errors, 0 overruns at 5 ms (exchange max 1.97 ms, wake-up delay max 82 µs); `test-mqtt.py node` and `write --do 1` over the network; the simulation suite on the PFC; mqtt-bridge from a Home Assistant broker; KBUS Manager page with live data |
| PFC300, FW 04.08.09(30), node 8DO + 750-657 + 750-450 + 750-671 | 750-450 temperatures; write watchdog on a real DO; 750-671 state; register reads (`ai/<n>/status`, `reg/get` on 750-450 and 750-657, refusals, rescan while reading) with unchanged KBUS statistics (0 errors, 0 overruns, longest cycle 2.7 ms) |
| Build | the arm64 package and all arm64 images build with the WAGO toolchain |
| PC, mqtt-iec104 (2026-11-22) | `tests/test-iec104.py`: 22 checks with a c104 IEC 104 client against mosquitto 2.0.20; image builds for arm64 and amd64 |
| PC, mqtt-modbus (2026-11-22) | `tests/test-modbus.py`: 25 checks against a pymodbus TCP server and mosquitto 2.0.20; image builds for arm64, armv7 and amd64 |

### 12.2 Not tested yet

- Installation of the native `.ipk` on a controller (only the containers
  were used); the `armhf` package for PFC100/PFC200.
- Digital and analog **inputs** and analog outputs on real hardware (only the
  simulation node).
- 750-671 commands (enable, move, reference) with a motor (only against the
  simulation model).
- COM1 on the controller's own interface (RS-232 / RS-485).
- mqtt-iec104 on a controller and with a real SCADA system.
- mqtt-modbus on a controller and with real Modbus devices.

### 12.3 History

1. **KBUS access** comes from the WAGO example *HowTo_ADI-MyKBusApplikation*
   (MIT licence, see [LICENSE](../LICENSE)) via the HTTP gateway
   `docker-wdx-kbus-interface` (versions 1.x–2.3): ADI/DAL device scan,
   `libpackbus`, one `libpackbus_Push` per cycle, module list through
   KBUS-info, real-time cycle thread, dynamic rescans, native `.ipk`,
   optional container.
2. **3.0.0: MQTT instead of HTTP.** The HTTP server, the wdx data layer, the
   registers, the built-in web page, the extra containers and the CODESYS
   HTTP client were removed. New: a small MQTT 3.1.1 client in C (no library,
   so the package still depends only on the firmware) and the KBUS ⇄ MQTT
   logic, with a topic structure designed for MQTT tools. The version number
   continues from 2.x, so the package replaces the HTTP gateway as a normal
   upgrade.
3. **mqtt-bridge, kbus-mosquitto, mqtt-manager**: data from other brokers,
   the broker as an image, and the KBUS Manager page at `/kbus` (since
   2026-11-20; before that a read-only monitor at `/mqtt`).
4. **3.3: COM1**, module descriptors (750-450), the 750-671 driver and the
   write watchdog. **3.4: register reads** (`ai/<n>/status`, `reg/get`).
5. **mqtt-iec104**: IEC 104 gateway with a YAML mapping, based on the
   standalone `docker-mqtt-104` project; **mqtt-modbus**: Modbus TCP
   bridge with a YAML configuration, based on `docker-mqtt-modbus`.

Lesson learned: a lighttpd reload started from a container belongs to the
container's cgroups and was stopped together with it (the WBM went down for a
moment while the manager was replaced); the reload now runs in the
controller's root cgroups (6.2).
