# kbus-gateway: WAGO KBUS on MQTT

A small native program for WAGO PFC100 / PFC200 / **PFC300** controllers that
**owns the KBUS** and makes all of its data available on an **MQTT broker**
(mosquitto on the controller): every DI / DO / AI / AO channel, the module list
and the bus status. Outputs are switched by publishing to a `.../set` topic. The
serial interface **COM1** is forwarded transparently over MQTT as well (3.9). It
is installed as a firmware package (`.ipk`), or runs as a container, starts at
boot and runs the KBUS cycle every **5 ms** with real-time priority.

Around it, each in its own container:

- **kbus-mosquitto**: the broker (mosquitto, configuration built in);
- **mqtt-bridge**: subscribes to topics on **other MQTT brokers** and publishes
  them on the broker of the PFC, so data from elsewhere sits next to the KBUS data;
- **mqtt-manager** (optional): the **KBUS Manager** page at
  `http(s)://<pfc-ip>/kbus`, in the look of the WBM: status, modules, COM1,
  all topics live with switching of the outputs, clients, cycle time, login.

```
                                    WAGO PFC300
                 ┌──────────────────────────────────────────────────────────┐
 KBUS ◀──5 ms──▶ │ kbus-gateway ──publish kbus/#──▶ mosquitto :1883 ◀── CODESYS, Node-RED,
 I/O modules     │           ◀──kbus/do/+/set──                  ◀── containers ...
                 │                                    ▲                     │
                 │ mqtt-bridge ───publish plant/...───┘  ▲ wss, read only   │
                 │                                        │                   │
                 │ WBM web server /kbus ──▶ mqtt-manager (page) ◀── browser   │
                 └──────▲─────────────────────────────────────────────────────┘
                        │ subscribe (TLS, login)
             other brokers: plant / cloud / weather ...
```

```bash
mosquitto_sub -h 127.0.0.1 -t 'kbus/#' -v              # everything (on the controller)
mosquitto_pub -h 127.0.0.1 -t kbus/do/3/set -m true     # switch DO3 on
mosquitto_pub -h 127.0.0.1 -t kbus/ao/1/set -m 16384    # AO1 to half scale (raw)
mosquitto_pub -h 127.0.0.1 -t kbus/serial/com1/tx/set -m 'hello'   # send bytes on COM1
```

The KBUS access is based on the WAGO example *HowTo_ADI-MyKBusApplikation* in
[docs/reference/](docs/reference/HowTo_ADI-MyKBusApplikation/README.md) (same
ADI/DAL calls as `kbusdemo.c`, same module discovery as `getkbusinfo.c`).

This project is the MQTT successor of the HTTP/wdx gateway
(`docker-wdx-kbus-interface`): the KBUS part is the same, the HTTP server, the
wdx data layer, the registers, the web page and the HTTP containers are
replaced by MQTT (see [6](#6-how-it-was-built)).

---

## Contents

1. [How it works](#1-how-it-works)
2. [Installation](#2-installation)
3. [MQTT topics](#3-mqtt-topics) (3.7: the KBUS Manager page, 3.8: 750-495, 3.9: COM1, 3.10: module values, 3.11: write watchdog, 3.12: 750-671 stepper)
4. [mqtt-bridge: topics from other brokers](#4-mqtt-bridge-topics-from-other-brokers)
5. [Configuration](#5-configuration)
6. [How it was built](#6-how-it-was-built)
7. [Modifying the application](#7-modifying-the-application)
8. [Troubleshooting](#8-troubleshooting)
9. [Limitations and security](#9-limitations-and-security)

---

## 1. How it works

```
 WAGO PFC (PTXdist firmware)
 ┌────────────────────────────────────────────────────────────────────┐
 │ /usr/bin/kbus-gateway  (started by /etc/init.d/kbus-gateway)             │
 │                                                                    │
 │  MQTT thread (main)                       KBUS cycle thread        │
 │   every 20 ms: snapshot of all channels    every 5 ms, SCHED_FIFO 40│
 │   → publish what changed                   write outputs           │
 │   .../set messages → write outputs ──lock──▶ libpackbus_Push        │
 │   bus rescans                              read inputs             │
 │        │ MQTT 3.1.1, TCP                        │ dlopen() of the   │
 │        ▼                                        ▼ firmware libdal.so│
 │   mosquitto 127.0.0.1:1883                 KBUS ──▶ I/O modules     │
 └────────────────────────────────────────────────────────────────────┘
```

**Startup**

1. The WAGO libraries (`libdal.so`, `libdbuskbuscommon.so` and the DAL plugins
   in `/usr/lib/dal`) are loaded with `dlopen()` from the firmware's own
   `/usr/lib`. They are not linked into the program, so it always uses the
   libraries of the installed firmware; the build only needs their headers.
2. The KBUS device (`libpackbus`) is opened. The modules are read through the
   KBUS-info D-Bus interface: position, type, bit offsets and sizes, channels.
3. A **channel map** is built. Channels are numbered from 1, separately for
   each class, in module order from left to right (at most 250 modules and
   250 channels per class).
   - **Digital module** (type code bit 15 set): every input bit is a `DI`,
     every output bit a `DO`.
   - **Analog module** with only input data → `AI`, with only output data →
     `AO`. Channel *k* is the unsigned 16-bit word at byte offset 2·(k−1) of
     the module data, raw value.
   - **750-495** and **other modules** with data in both directions (counters,
     serial interfaces) get no channels.
4. The KBUS is switched to application state *Running*; from then on the
   program drives every bus cycle itself.
5. The MQTT thread connects to the broker and publishes everything.

**KBUS cycle thread.** Every `KBUS_CYCLE_MS` (default 5 ms) a real-time thread
(`SCHED_FIFO`, priority 40) copies the output image, triggers one cycle with
`libpackbus_Push` and stores the input image. WAGO output modules switch their
outputs off when no cycle arrives within **50 ms**, so this thread never waits
for MQTT: the images are locked only for the copy.

**MQTT thread.** Every `MQTT_PUBLISH_MS` (default 20 ms) it takes one snapshot
of all channels (one lock, so all values come from the same bus cycle),
compares it with what it published last and publishes only the differences.
An input change is therefore on the broker within about 5 + 20 ms. A
`.../set` message is written to the output image at once; the next bus cycle
(≤ 5 ms) sends it to the module. The broker connection is kept alive and
re-established after a loss (1, 2, 4 … 30 s); the KBUS keeps running in the
meantime.

**Dynamic configuration.** Modules added to or removed from the KBUS are
picked up while the program runs: after **0.5 s of failing cycles** the bus is
rescanned (stop, re-initialise, new channel map, start; under one second on
the PFC300). `kbus/bus/configversion` goes up by one, `kbus/modules` is
published again, the topics of new channels appear and the retained topics of
channels that no longer exist are deleted. If errors persist, the next
automatic attempt waits 5, 10, 20, 40, then 60 s. A rescan can also be started
by hand: `kbus/bus/rescan/set` = `true`. Outputs are 0 after a rescan until
they are written again.

**Shutdown.** `/etc/init.d/kbus-gateway stop` (also at shutdown) sends SIGTERM:
`kbus/status` becomes `offline`, all outputs go to 0, one last cycle runs and
the KBUS device is closed. If the program crashes, the broker publishes
`offline` (last will) and the output modules switch off on their own after
50 ms without a cycle.

**Package on the controller**

| File | Purpose |
|---|---|
| `/usr/bin/kbus-gateway` | the program |
| `/etc/init.d/kbus-gateway` | `start`, `stop`, `restart`, `status` |
| `/etc/rc.d/S99_kbus-gateway` | boot link, created by the package |
| `/etc/kbus-gateway.conf` | settings (section 5); kept on package updates when you changed it |
| `/home/kbus-gateway/kbus-settings.conf` | cycle time written via `kbus/bus/cycletime/set` |
| `/var/log/messages` | log (syslog, `kbus-gateway[pid]`) |

---

## 2. Installation

Commands marked *PC* run in the project directory on a Linux PC; commands
marked *PFC* run on the controller (`ssh root@<pfc-ip>`). The examples use
`192.168.1.17`.

Quick route once the PC is set up (2.3):

```bash
tools/deploy-docker.sh 192.168.1.17 --mosquitto --no-gateway   # PC: the broker
tools/deploy.sh 192.168.1.17                                   # PC: build .ipk, copy, opkg install, status
tools/deploy-docker.sh 192.168.1.17 --bridge --manager --no-gateway   # PC: mqtt-bridge (4), KBUS Manager page (3.7)
```

### 2.1 Requirements on the PC

| What | Why |
|---|---|
| Linux PC with **Docker** (buildx) | WAGO's SDK build image with the firmware toolchain; building the containers |
| The **WAGO firmware SDK** of your controller, e.g. [pfc300-firmware-sdk](https://github.com/WAGO/pfc300-firmware-sdk), with its builder image: `make builder` → `pfc300-builder:latest` | toolchain with the same GCC and glibc as the firmware |
| The **WAGO SDK headers** in `sdk/include` (2.3) | ADI/DAL and KBUS-info API; proprietary, not in this repository |
| Python 3 + `pip install paho-mqtt` | only for the test script |
| SSH/scp access to the PFC as `root` | install |

### 2.2 Prepare the PFC

**One KBUS master.** Only one program may own the KBUS. Either set WBM →
*PLC Runtime* → runtime version **None**, or run the CODESYS runtime
**without KBUS** in its project. An installed kbus-gateway 2.x (the HTTP/wdx
version) is replaced by this package as a normal upgrade (version 3).

**The MQTT broker.** kbus-gateway connects to `MQTT_HOST:MQTT_PORT` (default
`127.0.0.1:1883`). Install mosquitto in one of two ways:

- **As a container** (Docker runs on the PFC300; the firmware's `mosquitto`
  package only contains the clients `mosquitto_pub`/`mosquitto_sub`):
  `tools/deploy-docker.sh 192.168.1.17 --mosquitto --no-gateway`. It builds
  **kbus-mosquitto** ([docker/mosquitto](docker/mosquitto)): eclipse-mosquitto
  2.0.20 with the configuration [docker/mosquitto/mosquitto.conf](docker/mosquitto/mosquitto.conf)
  built in, `--network host`; retained values are kept in `/home/mosquitto/data`.
  To change the configuration, edit that file and deploy again.
- **Any other way** you prefer: then take over the listeners of
  [docker/mosquitto/mosquitto.conf](docker/mosquitto/mosquitto.conf).

The configuration has two listeners:

| Listener | Access |
|---|---|
| `1883`, all interfaces | MQTT **without login**, read and write: everyone who can reach the controller can switch outputs (section 9) |
| `127.0.0.1:9001`, WebSocket | only for the monitor page (3.7), through the WAGO web server; **read only** (`acl-monitor`) |

For a login on 1883: `allow_anonymous false`, a `password_file` and an
`acl_file`, as described at the end of the configuration file.

The order does not matter: kbus-gateway keeps running the KBUS and connects as
soon as the broker is there.

### 2.3 Get the SDK header files

The real KBUS backend is compiled against the ADI/DAL and KBUS-info **header
files** of the WAGO SDK. They carry a WAGO "proprietary rights" notice and are
therefore **not in this repository**; import them once:

```bash
# A. from the prebuilt packages of the SDK (no SDK build needed)
tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages
# B. from a completely built SDK
tools/import-sdk-headers.sh ~/wago/ptxproj
```

The DAL version of the headers must equal `libdal.so.<version>` on the PFC
(`ls -l /usr/lib/libdal.so*`). Without `sdk/include/dal` only the
**simulation** build works (`make`), which is enough to develop and test the
MQTT side (7.2). Details: [sdk/README.md](sdk/README.md).

### 2.4 Build the package

*PC*:

```bash
tools/build-ipk.sh arm64        # PFC300        -> build-ipk/kbus-gateway_<version>_arm64.ipk
tools/build-ipk.sh armhf        # PFC100/PFC200 -> build-ipk/kbus-gateway_<version>_armhf.ipk (untested)
```

The script runs `make` inside the SDK builder image with WAGO's cross compiler
and `-Werror`, strips the binary, builds the package tree (program, init
script, config file, `postinst`, `prerm`, `conffiles`, `control`; version
from [VERSION](VERSION)) and packs it like a PTXdist package (an `ar` archive
with `debian-binary`, `control.tar.gz`, `data.tar.gz`). `BUILDER_IMAGE=...`
selects another SDK image.

### 2.5 Install, update, remove

**Install** — WBM → *Software Uploads* → upload the `.ipk` → *Install*, or
`tools/deploy.sh 192.168.1.17` (build, copy, `opkg install`, status and log).
`postinst` creates the boot link and (re)starts the service.

**Update** — raise [VERSION](VERSION), install the new `.ipk` the same way.
Your `/etc/kbus-gateway.conf` is kept if you changed it.

**Operate** (*PFC*):

```bash
/etc/init.d/kbus-gateway status|stop|start|restart
grep kbus-gateway /var/log/messages | tail -20
```

**Remove** — `opkg remove kbus-gateway` (stops it, all outputs go to 0).

### 2.6 Check the start

*PFC*: `grep kbus-gateway /var/log/messages | tail -20`. A good start (simulation
node shown; on a PFC the module lines show the real node):

```
kbus-gateway[812]: WAGO KBUS MQTT interface 3.0.0 starting
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

From the PC (needs a network listener on the broker, 2.2):

```bash
tests/test-mqtt.py 192.168.1.17 node              # real node, READ ONLY: status, modules, all channel topics
tests/test-mqtt.py 192.168.1.17 write --do 1      # real node: switches DO1 on/off (check what is connected!)
#   --port 1884 --user scada --password ...       for a listener with login
```

On the PC, without a PFC (a local broker, e.g. `mosquitto -p 18830`; `make` first):

```bash
make run-sim                                                  # simulated node; then, in another terminal:
tests/test-mqtt.py 127.0.0.1 sim                              # every KBUS topic and command
tests/test-mqtt.py 127.0.0.1 serial --gateway build/kbus-gateway   # COM1 on a virtual serial port (pty);
                                                              # starts the gateway itself, run-sim not needed
```

### 2.8 Alternative: kbus-gateway as a Docker container

The same program also runs as a container ([docker/gateway](docker/gateway);
all containers: [docker/README.md](docker/README.md)). The native package is
the recommended way; the container is for installations that manage
everything with Docker.

```bash
tools/deploy-docker.sh 192.168.1.17            # build, load, stop the native service, start the container
tools/deploy-docker.sh 192.168.1.17 --native   # remove the container, native service again
```

The script stops the native service and removes its boot link while the
container runs. The container uses `--privileged --network host` (real-time
priority, KBUS hardware, broker on `127.0.0.1`), mounts the firmware's
`/usr/lib` (read-only; the entrypoint loads the WAGO libraries without the
firmware's glibc), `/var/run/dbus`, `/etc/specific`, the KBUS devices, the
serial interface `/dev/serial` (COM1, 3.9) and `/home/kbus-gateway`. It does **not** read `/etc/kbus-gateway.conf`: other settings
with `GW_ENV`, e.g. `GW_ENV="-e MQTT_TOPIC_PREFIX=line1/kbus" tools/deploy-docker.sh ...`.

---

## 3. MQTT topics

### 3.1 The topic tree

`kbus` is the prefix (`MQTT_TOPIC_PREFIX`). All value topics are **retained**
(`MQTT_RETAIN=1`): a client that subscribes gets the current value at once.

```
kbus/status                     online | offline        retained; offline = last will (crash, power loss)
kbus/di/<n>                     true | false            digital input n (1-based)
kbus/do/<n>                     true | false            digital output n (the value last written)
kbus/ai/<n>                     0..65535                analog input n, raw process value
kbus/ao/<n>                     0..65535                analog output n (the value last written)
kbus/<class>/all                [..]                    the whole class as JSON array (MQTT_ARRAYS=1)
kbus/modules                    [{..}]                  modules and their channels (JSON, 3.4)
kbus/module/<pos>/in            24 bytes (binary)       raw input data of a complex module, e.g. 750-495 (3.8)
kbus/module/<pos>/temperature/<n>  21.4                 scaled value of a module with a descriptor, e.g. 750-450 in °C (3.10)
kbus/watchdog                   ok | expired            write watchdog, only with MQTT_WATCHDOG_S (3.11)
kbus/module/<pos>/stepper/state     {..}                 750-671 state (3.12)
kbus/module/<pos>/stepper/response  {..}                 result of a stepper command, not retained
kbus/bus/healthy                true | false            KBUS cycle running without errors
kbus/bus/cycletime              1..40                   ms
kbus/bus/configversion          1, 2, ...               +1 for every bus scan: modules/channels changed
kbus/bus/statistics             {..}                    every MQTT_STATS_S (3.4)
kbus/serial/com1/open           true | false            COM1 device open (3.9)
kbus/serial/com1/baudrate       19200                   current COM1 settings
kbus/serial/com1/format         8E1
kbus/serial/com1/framegap       10                      ms
kbus/serial/com1/statistics     {..}                    bytes, frames, dropped frames, line errors
kbus/serial/com1/rx             bytes (binary)          ONE message per received frame, not retained
kbus/error                      {"topic":..,"error":..} a rejected command (not retained)

write (publish WITHOUT retain):
kbus/do/<n>/set                 true | false            also on/off, 1/0
kbus/ao/<n>/set                 0..65535
kbus/do/all/set                 [true,null,false]       several outputs in the SAME bus cycle; null = unchanged
kbus/ao/all/set                 [1000,null,32767]
kbus/bus/cycletime/set          1..40                   stored, survives a restart
kbus/bus/rescan/set             true                    rescan the KBUS now
kbus/bus/resetstatistics/set    true                    cycles, errors, max values and overruns back to 0
kbus/module/<pos>/out/set       24 bytes (binary)       raw output data of a complex module (3.8)
kbus/watchdog/set               any                     heartbeat of the controlling side (3.11)
kbus/module/<pos>/stepper/cmd/set   {"cmd":..}          750-671 command: enable, move, reference, stop, quit, disable (3.12)
kbus/serial/com1/tx/set         1..512 bytes (binary)   send on COM1 as they are (3.9)
kbus/serial/com1/baudrate/set   9600                    COM1 settings: stored, survive a restart
kbus/serial/com1/format/set     8N1
kbus/serial/com1/framegap/set   20
```

Channels are numbered per class from 1, in KBUS order (left to right), as in
the CODESYS I/O mapping of the node. Values are **raw** process-image values:
for a 750-559 (0–10 V), 32767 = 10 V. Scale them in your application.

### 3.2 Why this structure (recommendation)

- **One value per topic, plain payloads** (`true`, `1234`). Every MQTT tool
  understands it: `mosquitto_sub`, Node-RED, Home Assistant, Grafana/Telegraf,
  SCADA drivers, the CODESYS MQTT libraries. Payloads are valid JSON, so
  `true`/`false` and numbers need no extra parsing.
- **Short, lower-case, fixed levels** `<prefix>/<class>/<n>`, so wildcards work
  naturally: `kbus/di/+` = all digital inputs, `kbus/+/3` = channel 3 of every
  class, `kbus/#` = everything.
- **Commands on a separate `/set` topic**, the common MQTT convention (used by
  Zigbee2MQTT, Tasmota, Home Assistant). The state topic always shows the real
  value; subscribing to `kbus/#` never triggers a write, and an ACL can allow
  reading `kbus/#` while writing only to `kbus/do/+/set`.
- **Arrays next to single values** (`kbus/do/all`): one message with a
  consistent snapshot of a whole class, and `.../all/set` writes several
  outputs in one bus cycle.
- **Retained values with a last will**: a dashboard that connects later sees
  all values at once, and `kbus/status` tells whether they are current.
- **One prefix per controller.** On a broker shared by several controllers
  (or after bridging, section 4) use a unique prefix, e.g.
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

`kbus/bus/statistics` (retained, every `MQTT_STATS_S` seconds):

```json
{"version":"3.0.0","backend":"adi","healthy":true,"cycleTime":5,"realtime":true,
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
  applied again at every reconnect; kbus-gateway therefore **ignores** retained
  `.../set` messages and logs how to delete them
  (`mosquitto_pub -r -n -t kbus/do/4/set`).
- A rejected command writes nothing and is reported on `kbus/error` and in
  the log: unknown channel, wrong value, more array values than channels.
- An output keeps its value until it is written again. There is no
  monitoring of the writer: when the client that controls an output stops,
  the output stays as it is. Use the client's own last will or a heartbeat in
  your application where that matters. Outputs go to 0 at a stop and a
  rescan.
- With `MQTT_WRITE_ENABLE=0` no `/set` topics are subscribed (read only).
- Several writers on the same output: the last message wins.

### 3.6 Retained topics after changes

At every connection kbus-gateway reads the retained topics that are on the
broker and deletes those that do not belong to the current node: channel
topics (e.g. `kbus/di/20` from a module that was removed while the program was
stopped) and module topics (`kbus/module/<pos>/in`, `.../temperature/<n>`,
`.../stepper/state` of a module that is gone or is another type now). The
same happens after a rescan. So the broker always shows exactly the current
node, and so does the KBUS Manager page: it builds its I/O, module values and
stepper panels from these topics.

The node is scanned at every start and again by itself when it changes: after
0.5 s of failing bus cycles kbus-gateway rescans the KBUS (or on
`kbus/bus/rescan/set`). WAGO allows changing modules only with the node
switched off, so in practice the new node is found at the next start.

### 3.7 KBUS Manager page (mqtt-manager)

`http://192.168.1.17/kbus` (or `https://`) is the **KBUS Manager**: the same
page as the KBUS Manager of the HTTP/wdx gateway, in the look of the
controller's Web-based Management (WBM): tab menu, sub menu, collapsible
panels, status bar with LEDs, the WBM's own fonts and logo, a link to the WBM
(top right). The old address `/mqtt` redirects to `/kbus`.

| Tab | Page | Content |
|---|---|---|
| Status | COM1 | communication parameters (state, baud rate, format, frame gap), **read only**: they are set by the client; monitor: bytes, frames, dropped, errors, the last 10 received frames, test send (3.9) |
| | KBUS | state, kbus-gateway online/offline, cycle time, cycles, errors, configuration version, version, write watchdog; timing (exchange, max. exchange, max. wake-up delay, overruns, real-time); *Reset Statistics* (login), *Rescan KBUS* |
| | Modules | `kbus/modules`: position, name, type code, channels |
| MQTT | Topics | every topic received (`kbus/#`, `bridge/status`, `bridge/+/status`) with value and time, filter. All DI/DO/AI/AO, the 750-450 temperatures and the 750-671 state are in this list. **Changing outputs**: log in, *Take Control*, click the value of `kbus/do/<n>` (`true`/`false`) or `kbus/ao/<n>` (0–65535), type, Enter (Esc = cancel). Control is **temporary**: on *Release Control*, logout or closing the page, every output written under control goes back to `false` / `0`. Command topics (`…/set`) are not listed |
| | Clients | MQTT clients connected to the broker (from mosquitto's connection log; a client from 127.0.0.1 is shown as CODESYS), state of the mqtt-bridge and its remote brokers |
| Settings | KBUS | `kbus/bus/cycletime/set`, 1–40 ms |
| | Login | website login on/off, user, password |

Status bar LEDs: **KBUS** (running / error), **GW** (`kbus/status`), **MQTT**
(WebSocket to the broker), **WR** (writes allowed / `MGMT_READONLY=1`),
**Login**.

```
 browser ─ http(s)://<pfc>/kbus/          ─▶ WAGO web server ─▶ mqtt-manager 127.0.0.1:9080 (page + API)
         ├ ws(s)://<pfc>/kbus/ws          ─▶ WAGO web server ─▶ mosquitto    127.0.0.1:9001 (MQTT, READ ONLY)
         └ POST /kbus/api/publish ─▶ mqtt-manager: login check, topic + value check ─▶ mosquitto :1883
```

- **Reads** are live over MQTT/WebSocket (no polling). The WebSocket listener
  of the broker is read only (`docker/mosquitto/acl-monitor`).
- **Writes** go through the mqtt-manager, which checks them and publishes on
  port 1883 (`do`/`ao` only with the login): `do/<n>/set`, `ao/<n>/set`, `bus/cycletime/set`,
  `bus/rescan/set`, `bus/resetstatistics/set`, `serial/com1/tx/set` (test
  send, as bytes); nothing else (no raw module data, no COM1 settings,
  nothing outside the prefix).
- **Login** (same rules as the wdx KBUS Manager): the website is open by
  default; *Login required* on Settings > Login closes it. Writing outputs
  (*Take Control*), *Reset Statistics* and changing the login settings
  **always** need the login (gear button, top
  right): default `admin` / `wago` until a password is set. Stored in
  `/home/mqtt-manager/settings.json` on the controller (password as PBKDF2
  hash). The login protects the page; port 1883 of the broker has no login,
  so anyone on the network can still read and write over MQTT directly.
- **Not available over MQTT:** *Local Control* of the wdx gateway (blocking
  all other writers) and the *Parameters* / *Registers* pages (wdx data
  layer); the *MQTT* tab replaces them.
- Environment (`MG_ENV` of `tools/deploy-docker.sh`): `MGMT_READONLY=1` (no
  writes), `MQTT_TOPIC_PREFIX` (when kbus-gateway uses another prefix),
  `MQTT_USER` / `MQTT_PASSWORD` (broker with login), `WEB_PATH` (`/kbus`),
  `OLD_WEB_PATH` (`/mqtt`, `""` = no redirect).

```bash
tools/deploy-docker.sh 192.168.1.17 --mosquitto --manager --no-gateway
```

- **No extra port**: the page uses the WBM's own web server (port 80/443, its
  certificate; HTTP follows the WBM's web-services mode). The mqtt-manager
  adds `/etc/lighttpd/apps.confd/mqtt.conf`, WAGO's own extension point (the
  WBM, WebVisu and the parameter service use it too). It lets lighttpd check
  the whole configuration (`lighttpd -tt`) before reloading it and removes a
  rejected file at once, so the WBM is never left with a broken configuration.
  A reload restarts lighttpd: the WBM is away for about a second, only when
  the file changes.
- **Kept automatically**: the mqtt-manager checks the file every minute and puts
  it back when it is missing or different (e.g. after a firmware update). The
  container runs with `--restart unless-stopped`.
- **Remove**: `docker rm -f mqtt-manager`, then
  `docker run --rm --privileged --pid host mqtt-manager:arm64 --unlink`.
  Without the second command `/kbus` stays linked and answers 503.
- Do not deploy the KBUS Manager of the wdx gateway on the same controller:
  both use `/kbus`.

The container needs `--privileged --pid host`: it writes the file and reloads
the web server on the controller (`nsenter`). The reload is run from the
controller's own cgroups, so lighttpd never belongs to the container (otherwise
Docker would stop the WBM's web server together with the container).

### 3.8 Complex modules (750-495) through MQTT

Modules without channels (750-495 power measurement, counters, serial
interfaces) get their **raw process data** as binary messages:

| Topic | Direction | Content |
|---|---|---|
| `kbus/module/<pos>/in` | kbus-gateway → | the module's input bytes (750-495: 24), all from one bus cycle, when they change, at most once per `MQTT_RAW_MIN_MS` (100 ms) per module; after a suppressed change the latest bytes always follow |
| `kbus/module/<pos>/out/set` | → kbus-gateway | exactly the module's output bytes (750-495: 24), written in one piece for the next bus cycle; without retain |

`kbus/modules` shows them as `"raw":{"in":24,"out":24}`. kbus-gateway does
nothing with the data itself: the protocol of the module is handled by the
application, e.g. the standard WAGO function block in CODESYS
([codesys/README.md](codesys/README.md)). Only one application may write a
module's output bytes.

The 750-495 delivers measured values only after the application has
requested them through its output bytes; with output bytes 0 it only shows
status bytes (FW 31 test: `47 80 92 00 …`).

#### Reading the 750-495 in CODESYS (standard WAGO function block)

On a normal PFC the 750-495 sits in the KBus device tree, its 24 input and 24
output bytes are mapped to variables, and the function block of the WAGO
library **WagoAppPowerMeasurement** works with those variables. Here the
CODESYS runtime has no KBus (container `cds3`, `KBUS=false`), so the same 24
bytes come and go through MQTT instead ("virtual I/O"). The function block
does not notice the difference.

```
 750-495 ◀─KBUS─▶ kbus-gateway ── kbus/module/2/in      (24 bytes) ──▶ CODESYS: copy into the FB's input bytes
                            ◀── kbus/module/2/out/set (24 bytes) ◀── CODESYS: the FB's output bytes
```

1. **Check the function block.** In the library manager, open the 750-495
   function block of WagoAppPowerMeasurement. It must take the module's
   process data as **ordinary variables** (an input and an output structure
   or byte array of 24 bytes each). If it needs a reference to the KBus
   device instead, this route does not work.
2. **Device tree:** no KBus (mandatory with `KBUS=false`). Add the MQTT
   client library you already use; broker `127.0.0.1:1883`.
3. **Subscribe** to `kbus/module/2/in` (`2` = position of the module in the
   node, see `kbus/modules`). Payload: binary, exactly 24 bytes, the module's
   input bytes from one KBUS cycle. Copy them into the function block's input
   variable when a message arrives.
4. **Call the function block** every task cycle, as usual.
5. **Publish** the function block's 24 output bytes on
   `kbus/module/2/out/set` when they change (binary, exactly 24 bytes,
   **retain off**, QoS 0 or 1). Also needed when the application only reads
   measured values: the function block requests every value through these
   bytes. kbus-gateway rejects any other length (`kbus/error`).
6. **Watch** `kbus/status` (`online`) and `kbus/bus/healthy` (`true`); after a
   restart of kbus-gateway the output bytes are 0 until the application writes
   them again (step 5 does that by itself when you compare with the last sent
   bytes and reset that copy when `kbus/status` becomes `online`).

Skeleton in ST. The subscribe block is the same type as the existing
`oFbSubscribe1` of the application (inputs `xSubscribe`, `sTopic`,
`aPayloadData`, outputs `xDataReceived`, `dwRxNBytes`); the publish block,
the 750-495 function block and their parameters are written as comments,
because they depend on the library versions in the project:

```iecst
VAR
    oFbSubscribeIn : (* same type as oFbSubscribe1 *);   // kbus/module/2/in
    oFbPublishOut  : (* publish block of the same MQTT library *);   // kbus/module/2/out/set
    aRxIn          : ARRAY[0..1023] OF BYTE;       // payload buffer of the subscription
    aIn495         : ARRAY[0..23] OF BYTE;         // = the module's input bytes (normally %IB mapping)
    aOut495        : ARRAY[0..23] OF BYTE;         // = the module's output bytes (normally %QB mapping)
    aOutSent       : ARRAY[0..23] OF BYTE;         // last bytes published
    fbPM495        : (* 750-495 FB of WagoAppPowerMeasurement *);
    i              : INT;
    xChanged       : BOOL;
END_VAR

// 1. input bytes from MQTT
oFbSubscribeIn(xSubscribe := TRUE, sTopic := 'kbus/module/2/in', aPayloadData := aRxIn);
IF oFbSubscribeIn.xDataReceived AND oFbSubscribeIn.dwRxNBytes = 24 THEN
    FOR i := 0 TO 23 DO
        aIn495[i] := aRxIn[i];
    END_FOR
END_IF

// 2. the standard function block, with aIn495 / aOut495 instead of the mapped I/O
fbPM495( (* input bytes := aIn495, output bytes => aOut495, ... *) );

// 3. output bytes to MQTT, only when they changed
xChanged := FALSE;
FOR i := 0 TO 23 DO
    IF aOut495[i] <> aOutSent[i] THEN xChanged := TRUE; END_IF
END_FOR
IF xChanged (* AND the publish block is not busy *) THEN
    aOutSent := aOut495;
    (* publish aOutSent: topic 'kbus/module/2/out/set', 24 bytes, retain FALSE *)
END_IF
```

The skeleton shows the data flow; adapt the names and parameters of the MQTT
function blocks to your library, and the 750-495 function block's in/out
variables to its declaration.

**Timing.** Every request of the function block now takes a round trip
through MQTT (CODESYS → broker → kbus-gateway → next KBUS cycle → module →
kbus-gateway → broker → CODESYS), roughly 10–50 ms instead of one KBUS cycle. A
full set of measured values therefore takes longer than on the KBus; measured
values every second or so are no problem. If the function block has a
time-out for the module's answer, set it to at least 500 ms.

**One writer.** Only this CODESYS application may publish
`kbus/module/2/out/set`. The KBUS Manager page cannot write it (mqtt-manager and broker ACL).

**Raw data by hand** (diagnosis):

```bash
mosquitto_sub -h <pfc-ip> -t kbus/module/2/in -F "%U %x"     # time + 24 bytes in hex
```

### 3.9 Serial interface COM1

kbus-gateway also owns the serial interface **COM1** of the controller
(`/dev/serial`) and forwards it **transparently**: it does not interpret the
bytes. The protocol of the connected device (Modbus RTU, a barcode reader, a
meter, ...) is implemented in the client, e.g. CODESYS. The same function as
`Serial/Com1/...` of the HTTP/wdx gateway, on MQTT.

| Topic | Direction | Content |
|---|---|---|
| `kbus/serial/com1/rx` | kbus-gateway → | **one message per received frame**, binary, not retained |
| `kbus/serial/com1/tx/set` | → kbus-gateway | 1–512 bytes, sent as they are (binary); without retain |
| `kbus/serial/com1/open` | kbus-gateway → | `true` / `false`: device open (retained) |
| `kbus/serial/com1/baudrate`, `format`, `framegap` | kbus-gateway → | the current settings (retained) |
| `kbus/serial/com1/baudrate/set`, `format/set`, `framegap/set` | → kbus-gateway | new settings, applied at once, stored in `KBUS_SETTINGS_STORE`; without retain |
| `kbus/serial/com1/statistics` | kbus-gateway → | `{"txBytes":..,"rxBytes":..,"frames":..,"dropped":..,"errors":..}`, retained, when it changes (at most once per second) |

- **Frames.** A pause of `framegap` ms (default 10) after the last received
  byte ends a frame. Choose it larger than the gaps *inside* a frame of the
  device and smaller than the pause between two frames (Modbus RTU: 3.5
  characters, i.e. 4 ms at 9600 baud; 10 ms is safe up to 9600 baud). A frame
  longer than 512 bytes is split.
- **Latency.** Frames go into a queue of 64; the MQTT thread publishes all of
  them at every tick (`MQTT_PUBLISH_MS`, 20 ms). A frame reaches the broker
  about framegap + ≤ 20 ms after its last byte; none is lost between two
  ticks. When the queue is full (no broker connection), the oldest frame is
  dropped and counted in `dropped`. Frames received while there is no broker
  connection are discarded at the reconnect: they are old.
- **Settings are made on the client side**: the client publishes
  `baudrate/set` (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400),
  `format/set` (data bits 5–8, parity `N`/`E`/`O`, stop bits 1/2, e.g. `8E1`)
  and `framegap/set` (1–1000 ms), preferably only when they differ from the
  retained current values. A value written there wins over `SERIAL_BAUD`,
  `SERIAL_FORMAT`, `SERIAL_FRAME_GAP_MS` and survives a restart. Invalid values
  are answered on `kbus/error` and change nothing. The KBUS Manager page only
  shows them (*Status > COM1*).
- **RS-232 / RS-485** is chosen in the WBM (*Interfaces > Serial Interface*);
  the owner of the interface must be **None**, so that kbus-gateway can open
  it (it opens it exclusively). The RS-485 direction is switched by the driver.
- **Off**: `SERIAL_DEVICE=""` (and in simulation unless it is set): no
  serial topics; retained ones of an earlier run are deleted. With
  `MQTT_WRITE_ENABLE=0` no serial `/set` topics are subscribed.

```bash
mosquitto_sub -h <pfc-ip> -t 'kbus/serial/com1/#' -v                     # settings, statistics, frames
mosquitto_sub -h <pfc-ip> -t kbus/serial/com1/rx -F "%U %x"              # frames in hex with a time stamp
printf '\x01\x03\x00\x00\x00\x02\xc4\x0b' | mosquitto_pub -h <pfc-ip> -t kbus/serial/com1/tx/set -s   # binary
mosquitto_pub -h <pfc-ip> -t kbus/serial/com1/baudrate/set -m 9600
```

### 3.10 Module values (module descriptors)

Some module types get **functional values** next to their raw channels: one
descriptor per module type (`src/modules.c`) says which channels mean what,
in which unit and with which resolution. The topics are the same for every
type:

```
kbus/module/<pos>/<function>/<n>    value in the unit of the descriptor (retained, on change)
```

| Module | Function | Value | Raw channels |
|---|---|---|---|
| 750-450 (4 RTD inputs) | `temperature/1..4` | °C, signed, 0.1 °C per digit (Pt100: −200.0 … 850.0) | stay as `kbus/ai/<n>` (raw, unsigned) |

- `kbus/modules` lists the descriptor: `"functions":{"temperature":{"unit":"°C","resolution":0.1}}`.
- The 750-450 is set to its sensor type with WAGO-I/O-CHECK. For a setting
  with another resolution or unit (e.g. resistance), set
  `MODULE_450_RESOLUTION` and `MODULE_450_UNIT`.
- `MQTT_ANALOG_DEADBAND` applies in digits (e.g. `2` = 0.2 °C).
- The **750-671** has a driver of its own (3.12).
- Not yet: the **750-657** (IO-Link master). Its handshakes (mailbox,
  control/status byte) have to run in the gateway, not in an MQTT client: raw
  bytes over MQTT would break them through broker latency. Until its driver
  exists it only gets its raw bytes (`kbus/module/<pos>/in`, `out/set`, 3.8).
  Planned: port data and port status as state, IO-Link parameters as
  request/response with a request ID.

### 3.11 Write watchdog

With `MQTT_WATCHDOG_S=N` kbus-gateway supervises the controlling side (e.g.
CODESYS): when it has received **no write and no `kbus/watchdog/set` for N
seconds**, it switches **all outputs to 0**: every DO and AO, and the raw
output bytes of complex modules (750-495, and the control bytes of a
750-657 / 750-671, which stops a stepper). This is the safe state for the
case that the client, its network or the broker disappears.

- The watchdog starts with the first write or heartbeat (outputs are 0 after
  the start anyway). Publish `kbus/watchdog/set` (any payload, **without**
  retain) at least every N/2 s when the outputs do not change that often.
- `kbus/watchdog` (retained): `ok`, or `expired` after it switched the
  outputs off. The next write or heartbeat sets it back to `ok`; outputs are
  not restored, the client writes them again.
- Also kbus-gateway's own failure is covered: `kbus/status` = `offline` (last
  will), and the output modules switch off by themselves 50 ms after the KBUS
  cycle stops.

### 3.12 750-671 stepper controller

kbus-gateway runs the **handshake of the 750-671** itself (positioning
application, `Application_Selector = 1`, cyclic process image; manual
750-671, 2.1.2.7 and 2.1.2.11.1), at every tick (`MQTT_PUBLISH_MS`, 20 ms).
MQTT carries only commands and results, so broker latency cannot break the
handshake. There is no closed-loop control over MQTT: the module ramps and
positions itself; MQTT starts and monitors it.

| Topic | Direction | Content |
|---|---|---|
| `kbus/module/<pos>/stepper/cmd/set` | → kbus-gateway | JSON command, without retain; optional `"id"` (echoed in the response) |
| `kbus/module/<pos>/stepper/response` | kbus-gateway → | `{"cmd":..,"id":..,"result":"accepted"\|"done"\|"error","error":..}`, not retained |
| `kbus/module/<pos>/stepper/state` | kbus-gateway → | retained; flags on change, position/velocity at most every 100 ms |

| Command | What the driver does | Response |
|---|---|---|
| `{"cmd":"enable"}` | Enable + Stop2_N, acknowledges a module reset (Reset_Quit) | `done` when Ready and Stop_N_ACK; `error` e.g. "Input1 (stop circuit) is open" |
| `{"cmd":"move","position":P,"velocity":V,"acceleration":A}` | absolute position (24-bit, steps or the configured unit), V 1–25000, A 1–32767; selects the positioning mode, setpoints, Start edge | `accepted` at Start_ACK, `done` at On_Target. A new `move` during a move changes the target on the fly |
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
- **Safe state** (write watchdog, 3.11): a **controlled stop** (mode and
  Start off, braking ramp); Enable stays on, so the motor keeps its holding
  current. The running command ends with `error`.
- The raw output bytes belong to the driver: `kbus/module/<pos>/out/set` is
  refused; `kbus/module/<pos>/in` is still published (diagnosis).
- **Input1** (DI1) is the module's stop circuit in the default configuration:
  open = no start possible. Jog mode, move programs, the mailbox
  (configuration tables) and the velocity application are not supported.

---

## 4. mqtt-bridge: topics from other brokers

The container [docker/mqtt-bridge](docker/mqtt-bridge/README.md) connects to
one or more **remote brokers**, subscribes to topics there and publishes every
message on the broker of the PFC. Applications on the PFC (CODESYS, Node-RED,
containers) then find external data on their local broker, next to `kbus/#`.

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
[docker/mqtt-bridge/mqtt-bridge.ini](docker/mqtt-bridge/mqtt-bridge.ini).

The other direction (KBUS data **to** a central broker or cloud) is best done
with mosquitto's own bridge function in `mosquitto.conf` (`connection`,
`address`, `topic kbus/# out 1 "" plant1/pfc300-01/`), which also buffers
messages while the connection is down.

---

## 5. Configuration

`/etc/kbus-gateway.conf` on the PFC (environment variables for the program; apply
with `/etc/init.d/kbus-gateway restart`):

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
| `MQTT_ANALOG_DEADBAND` | `0` | AI/AO are published when the raw value changed by at least N (0 = every change). Noisy analog inputs: e.g. `50` |
| `MQTT_ARRAYS` | `1` | `1` = also `<prefix>/<class>/all` |
| `MQTT_STATS_S` | `10` | `bus/statistics` every N s, 0 = off |
| `MQTT_REPUBLISH_S` | `0` | publish all values again every N s (for clients that need a heartbeat), 0 = only changes |
| `MQTT_WRITE_ENABLE` | `1` | `0` = read only: no `/set` topics |
| `MQTT_LOG` | `1` | 0 none, 1 writes that change an output and bus commands, 2 every received message |
| `MQTT_RAW_MIN_MS` | `100` | `kbus/module/<pos>/in` at most once per N ms per module (e.g. a 750-657 whose input bytes change every cycle: 37 → 10 messages/s); the latest bytes always follow; 0 = every change |
| `MQTT_WATCHDOG_S` | `0` | write watchdog (3.11): no write and no `watchdog/set` for N s → all outputs 0; 0 = off |
| `MODULE_450_RESOLUTION`, `MODULE_450_UNIT` | `0.1`, `°C` | 750-450 value per digit and unit (3.10), for other sensor settings |
| `MQTT_PRIORITY` | `0` (package: `30`) | SCHED_FIFO priority of the MQTT thread, 0 = normal. Keep it **below** `KBUS_PRIORITY` |
| `KBUS_CYCLE_MS` | `5` | KBUS cycle, 1–40 ms (well below the 50 ms output watchdog). A value written to `bus/cycletime/set` wins and is kept in `KBUS_SETTINGS_STORE` |
| `KBUS_SETTINGS_STORE` | `/home/kbus-gateway/kbus-settings.conf` | cycle time and COM1 settings written over MQTT; delete the file to use `KBUS_CYCLE_MS` / `SERIAL_*` again; empty = not stored |
| `KBUS_PRIORITY` | `40` | SCHED_FIFO priority of the KBUS cycle thread, 0 = normal |
| `SERIAL_DEVICE` | `/dev/serial` (simulation: `""`) | COM1 device (3.9); `""` = COM1 off. Set explicitly, it is also used in simulation (e.g. a pty for tests) |
| `SERIAL_BAUD` | `19200` | 1200 … 230400 (standard rates). A value written to `serial/com1/baudrate/set` wins and is kept in `KBUS_SETTINGS_STORE` |
| `SERIAL_FORMAT` | `8E1` | data bits 5–8, parity N/E/O, stop bits 1/2; `format/set` wins |
| `SERIAL_FRAME_GAP_MS` | `10` | 1–1000: a pause this long ends a received frame; `framegap/set` wins |
| `LOG_TARGET` | stderr (package: `syslog`) | `syslog` = `/var/log/messages` |
| `KBUS_SIMULATION` | `0` | `1` = simulated node, no hardware (development) |
| `KBUS_SIM_HOTPLUG` | – | simulation only: while this file exists, a 6th module (8DI) is "plugged in" |
| `KBUS_DAL_LIB`, `KBUS_INFO_LIB` | `libdal.so:...` | `dlopen()` candidates of the WAGO libraries |
| `KBUS_DIGITAL_OFFSET_MODE` | `auto` | `auto`, `relative` or `absolute` (digital data offset in the process image) |

**Load.** With `MQTT_PUBLISH_MS=20` and no changes the MQTT thread only
compares a few hundred bytes 50 times per second. Every changed channel is one
small message (two with `MQTT_ARRAYS=1`). Fast-changing analog inputs produce
up to 50 messages per second each; use `MQTT_ANALOG_DEADBAND` or a longer
`MQTT_PUBLISH_MS` when the subscribers do not need that.

---

## 6. How it was built

1. **KBUS access** comes from the WAGO example *HowTo_ADI-MyKBusApplikation*
   (`docs/reference/`, MIT licence) via the HTTP gateway
   (`docker-wdx-kbus-interface`, versions 1.x–2.3): ADI/DAL device scan,
   `libpackbus`, one `libpackbus_Push` per cycle, module list through
   KBUS-info, real-time cycle thread, dynamic rescans, native `.ipk` built with
   the SDK toolchain, optional container. These parts (`gateway.c`,
   `backend_*.c`, packaging, entrypoint) were taken over unchanged.
2. **MQTT instead of HTTP (3.0.0).** First developed as "kbus-mqtt" 1.0.0, then
   renamed back to **kbus-gateway** with version 3.0.0, so the package replaces
   kbus-gateway 2.x (HTTP/wdx) as a normal upgrade. The HTTP server, the wdx data layer, the
   registers (data shared between containers: the broker does that now, with
   retained messages and persistence), the built-in web page, the manager /
   IEC 104 / example containers and the CODESYS HTTP client were removed.
   New: a small MQTT 3.1.1 client in C (`mqtt_client.c`, no library, so the
   package still depends only on the firmware) and the KBUS ⇄ MQTT logic
   (`mqtt_io.c`). The topic structure was designed for MQTT tools (3.2), not
   copied from the wdx paths.
3. **mqtt-bridge** (Python, paho-mqtt) for data from other brokers.
4. **kbus-mosquitto and mqtt-manager.** The broker got its own image with the
   configuration built in; the optional mqtt-manager serves a read-only
   monitor page (layout of the wdx page) at `/mqtt` of the WBM web server,
   later (2026-11-20) replaced by the KBUS Manager at `/kbus`: the same WBM-style
   page as the wdx gateway, reading over MQTT/WebSocket, writing through the
   mqtt-manager with the same login rules.
   Lesson learned: a lighttpd reload started from a container belongs to the
   container's cgroups and was stopped with the container (the WBM went down
   for a short time while the manager was replaced); the reload now runs from
   the controller's root cgroups (3.7).

**On the PFC300 (18-11-2026)**: 750-8302, FW 04.09.01(31), node 8DO + 750-495,
all as containers (`kbus-gateway`, `kbus-mosquitto`, `mqtt-bridge`,
`mqtt-manager`) next to the CODESYS runtime in a container without KBUS
(`cds3`, [docker-codesys3-wagopfc](https://github.com/pfcdiederick/docker-codesys3-wagopfc), `KBUS=false`; native runtime "None"):
KBUS opened by kbus-gateway only, 0 errors, 0 overruns at 5 ms (KBUS exchange
max 1.97 ms, wake-up delay max 82 µs), `tests/test-mqtt.py ... node` and
`write --do 1` from a PC over the network (DO1 on/off, also held for 10 s),
the simulation suite on the PFC (35 checks), mqtt-bridge from a Home Assistant
broker, the KBUS Manager page through `https://<pfc>/kbus` with live data.

Before that, with the simulation node and mosquitto 2.0.20 (`tests/test-mqtt.py sim`:
35 checks, all classes, arrays, invalid commands, cycle time, rescan), plus:
removal of stale retained topics at start, a retained command that is
ignored, broker restart (reconnect), clean stop (`offline`), `kill -9` (last
will `offline`), simulated hot-plug (new DI topics appear and are removed
again), and the chain remote broker → mqtt-bridge → kbus-gateway → DO1. The
arm64 package and all arm64 images build with the WAGO toolchain.

### 6.1 Source structure

Plain C (gnu11), no dependencies besides glibc. The KBUS cycle thread and the
MQTT thread share only the process images, behind one mutex with priority
inheritance.

| File | Role |
|---|---|
| `src/main.c` | startup order, signals, memory locking |
| `src/config.c/.h` | environment variables → `gw_config_t` |
| `src/backend_adi.c` | real KBUS: `dlopen` of `libdal`, ADI device scan, KBUS-info, cycle |
| `src/backend_sim.c` | simulated node (12 DI, 8 DO, 8 AI, 4 AO; DI1–8 follow DO1–8, AI4 follows AO1; a 750-450 with AI5–8, AI8 follows AO2; a model of the 750-671 positioning handshake) |
| `src/modules.c/.h` | module descriptors: functional values of a module type (750-450 temperatures) |
| `src/stepper.c/.h` | 750-671 driver: positioning handshake, commands, state, controlled stop |
| `src/gateway.c/.h` | channel map, process images, real-time cycle thread, rescans, bulk read/write under one lock |
| `src/mqtt_client.c/.h` | MQTT 3.1.1 client: connect with last will, QoS 0 publish (buffered), subscribe, keep-alive |
| `src/mqtt_io.c/.h` | topic tree, change detection, `/set` commands, stale topic cleanup, reconnects |
| `src/serial.c/.h` | COM1: open, settings (termios), receive thread with frame gap and frame queue, transmit |
| `src/log.c/.h` | stderr or syslog |

---

## 7. Modifying the application

### 7.1 Project layout

```
├── README.md, LICENSE, VERSION, Makefile
├── src/                       kbus-gateway in C (6.1)
├── sdk/                       SDK headers (imported, 2.3) + OsCommon.h stand-in
├── packaging/ipk/             native package: init script, config file, opkg scripts
├── docker/                    containers, one folder each (docker/README.md)
│   ├── gateway/               kbus-gateway as a container (2.8): Dockerfile, entrypoint.sh
│   ├── mosquitto/             kbus-mosquitto: the broker with its configuration (2.2)
│   ├── mqtt-bridge/           topics of other brokers -> PFC broker (section 4)
│   └── mqtt-manager/          KBUS Manager page at /kbus + API, link in the WAGO web server (3.7)
├── web/                       the KBUS Manager page: index.html, kbus.js, kbus.css
├── codesys/                   using the KBUS from CODESYS through MQTT
├── tools/
│   ├── build-ipk.sh           build the .ipk with the SDK toolchain (2.4)
│   ├── deploy.sh              native: build + copy + opkg install + status
│   ├── deploy-docker.sh       containers: kbus-gateway, mosquitto, mqtt-bridge, mqtt-manager, or back to native
│   └── import-sdk-*.sh        SDK headers (2.3)
├── tests/test-mqtt.py         simulation (everything), real node read-only / write
├── docs/
│   └── reference/             WAGO example HowTo_ADI-MyKBusApplikation (MIT), basis of the KBUS access
│
│   generated (not in git): build/, build-ipk/, build-docker/
```

### 7.2 Development loop (on the PC, no PFC needed)

```bash
docker run -d --name mosquitto-dev -p 1883:1883 eclipse-mosquitto:2.0.20 \
    mosquitto -c /mosquitto-no-auth.conf             # broker for development
make run-sim                                        # simulated node, broker 127.0.0.1:1883
pip install paho-mqtt
tests/test-mqtt.py 127.0.0.1 sim                    # all topics and commands
tools/deploy.sh 192.168.1.17                        # then: build .ipk, install on the PFC
```

Without `sdk/include/dal` the native build is simulation-only, which is
enough for all MQTT work. The VS Code tasks (*Terminal → Run Task*) cover the
same steps.

### 7.3 Common changes

**Topics.** Names and payloads are in [src/mqtt_io.c](src/mqtt_io.c):
`pub_value()`, `pub_array()`, `pub_modules()`, `pub_stats()`; commands in
`on_message()`, `on_output()`, `on_bus()`. Update `tests/test-mqtt.py` and
section 3.

**Another module type.** Module classification: `build_channel_map()` in
[src/gateway.c](src/gateway.c).

**Release.** Raise [VERSION](VERSION), `tools/build-ipk.sh arm64`, install.

### 7.4 Rules to keep

- Never do slow work (I/O, logging, allocation) in `cycle_thread()`: every
  cycle must finish well within 50 ms, or the output modules switch off.
- Keep `MQTT_PRIORITY` below `KBUS_PRIORITY`.
- Read and write channels only through the `gateway_*` functions (locking).
- `gateway_rescan()` only from the MQTT thread: it is the only reader of the
  channel map besides the cycle thread.
- Build against headers with the **same DAL version** as the PFC's
  `libdal.so`, with the toolchain of the controller's firmware SDK.

---

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `MQTT broker 127.0.0.1:1883: ... Connection refused (retrying)` | mosquitto not running or listening elsewhere: `docker ps`, `netstat -tln \| grep 1883`, `MQTT_HOST`/`MQTT_PORT` |
| `broker refused the connection: not authorized` / `bad user name or password` | the listener needs a login: `MQTT_USERNAME`, `MQTT_PASSWORD` |
| `the broker refused a subscription (ACL?)` | the ACL does not allow the user to subscribe to `kbus/+/+/set`: no writes possible |
| Values arrive, writes do nothing | `MQTT_WRITE_ENABLE=0`, wrong prefix in the topic, the topic is the state topic (`kbus/do/1`) instead of `kbus/do/1/set`, or see `kbus/error` |
| `ignoring retained command ...` | something published a `/set` with retain: delete it as the log line says |
| Connection drops every few seconds | two clients with the same `MQTT_CLIENT_ID` (e.g. native service and container): the broker drops the older one |
| `kbus/status` is `offline` while the program runs | it is reconnecting (log), or another prefix is configured |
| Old channel topics stay | retained topics of a different prefix are not cleaned; delete with `mosquitto_pub -r -n -t <topic>` |
| PC cannot connect to the broker | port 1883 blocked by the PFC firewall, or another `mosquitto.conf` with `listener 1883 127.0.0.1` |
| `https://<pfc>/kbus` gives 404 | the mqtt-manager is not running or has not linked yet: `docker logs mqtt-manager` |
| `https://<pfc>/kbus` gives 503 | the mqtt-manager was removed but the link is still there: `--unlink` (3.7), or start it again |
| Monitor page: "connecting…" / "offline" | the broker (kbus-mosquitto, WebSocket listener 127.0.0.1:9001) is not running |
| Monitor page: "kbus-gateway offline" | kbus-gateway is stopped or cannot reach the broker; the page shows its last values |
| `opening the KBUS device failed` | another program owns the KBUS: runtime not *None*, a CODESYS project with KBUS, an old gateway container, or the kbus-gateway container and the native service at once |
| `bus/statistics`: `overruns` increasing | the cycle time does not hold on this node: `kbus/bus/cycletime/set` a higher value |
| Cycle time ignores `KBUS_CYCLE_MS` | a value written via `bus/cycletime/set` is stored in `/home/kbus-gateway/kbus-settings.conf` and wins: delete it, restart |
| `could not load the ADI/DAL library` | check `ls /usr/lib/libdal*`, set `KBUS_DAL_LIB` |
| `this binary was built WITHOUT the WAGO ADI/DAL headers` | import the headers (2.3) and rebuild |
| `version 'GLIBC_2.xx' not found` | built with a newer toolchain than the firmware: use the SDK builder image |
| Channel numbers point to the wrong I/O | compare the startup module list with the node; try `KBUS_DIGITAL_OFFSET_MODE=relative` or `absolute` |
| New modules not visible | look for `KBUS rescan` in the log; `kbus/bus/rescan/set` = `true` |
| `COM1: cannot open /dev/serial` / `serial/com1/open` = `false` | the serial interface has another owner in the WBM (*Interfaces > Serial Interface*: set **None**), another program uses it, or the container has no `/dev/serial` (deploy again) |
| COM1: frames split in two, or two frames in one `rx` message | `framegap` too small / too large for the device (3.9) |
| COM1: `dropped` increasing | the broker connection was away, or the frames come faster than they can be published |
| mqtt-bridge: `[name] ... refused the connection` / TLS errors | login, `ca_file`, port 8883 vs 1883; `log_level = DEBUG` shows every message |

---

## 9. Limitations and security

- Only plain digital modules and plain analog modules (16-bit words, data in
  one direction) are mapped; complex modules and the 750-495 get no channels.
  Values are raw, without scaling.
- **Only one KBUS master**: kbus-gateway and a CODESYS application with KBUS
  access cannot run at the same time.
- kbus-gateway speaks plain MQTT over TCP, meant for the broker on the same
  controller. TLS to other brokers is the job of mosquitto (bridge) or of the
  mqtt-bridge.
- QoS 0 is used for publishing and subscribing: on the local connection
  (`127.0.0.1`) nothing gets lost; a lost connection is followed by a full
  publish of all values.
- **Whoever can publish to `kbus/do/+/set` / `kbus/ao/+/set` switches
  outputs.** The broker configuration (2.2) accepts MQTT **without login on
  every interface**: everyone who can reach port 1883 of the controller can
  read all I/O and switch outputs. That is fine for a test setup or an
  isolated machine network; otherwise add user names, passwords and an ACL
  (end of `docker/mosquitto/mosquitto.conf`), use `MQTT_WRITE_ENABLE=0` when no
  outputs need to be written, block port 1883 in the PFC firewall for other
  networks, and use TLS on networks you do not control. The KBUS Manager page
  (3.7) writes only through the mqtt-manager (topic whitelist, optional login).
- No supervision of writers: outputs keep their last value when the writing
  client disappears (3.5).
- MQTT is fast but not hard real-time; for hard real-time I/O let CODESYS
  drive the KBUS directly.
- Tested on a PFC300 with FW 04.09.01(31) as containers (section 6); the
  native `.ipk` builds but has not been installed yet, the `armhf` package for
  PFC100/PFC200 neither. Only digital outputs were tested on real hardware;
  inputs and analog modules only with the simulation node. The 750-495 gets
  no channels (its measured values are not available).
- On the test node (PFC300 FW30 with 8DO, 750-657, 750-450, 750-671): the
  750-450 temperatures, the write watchdog on a real DO, and the 750-671
  state are tested. The 750-671 commands (enable, move, reference) are tested
  against the simulation model only, not yet with a motor. The 750-657 has no
  driver yet (3.10).
- COM1 is tested with a virtual serial port (pty) on the PC, not yet on the
  controller's own interface (RS-232 / RS-485).
- On `stop` and `opkg remove` all outputs are switched to 0. If the program
  crashes, the output modules switch off on their own after 50 ms.
