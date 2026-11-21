# Prompt for Claude in CODESYS (MCP): MQTT interface to the KBUS node

Open the CODESYS project of the test node in the CODESYS IDE, enable
*Tools > Enable MCP Server*, and paste everything below the line into Claude.

---

## Goal

Give this CODESYS application an **MQTT interface** to the I/O node of the
controller, so that the application controls the outputs, the 750-671 stepper
controller and the 750-657 IO-Link master through MQTT instead of through the
KBUS. Build it so that the rest of the application can use it like the
standard WAGO function blocks.

Work **strictly additively**: add a folder `20 MQTT KBUS` with new POUs, DUTs
and a GVL, and one call in an existing task. Do not delete or rename existing
objects. **Build** the application to check it, but do **not** download it, go
online or change the device tree without asking me first.

## Setup (fixed, do not change)

| | |
|---|---|
| Controller | WAGO PFC300 (750-8302), firmware 04.08.09(30), test node 192.168.178.215 |
| CODESYS | runs in a Docker container (`cds3`) **without KBUS** (`KBUS=false`) |
| KBUS owner | the program `kbus-gateway` (container); it publishes the node on MQTT |
| Broker | `127.0.0.1:1883` on the controller, no login, MQTT 3.1.1, QoS 0 |
| Topic prefix | `kbus` |

The node, left to right:

| Pos | Module | Reached through |
|---|---|---|
| 1 | 8DO (digital outputs 1–8) | `kbus/do/...` |
| 2 | 750-657 IO-Link master, 24 bytes in/out | raw process image over MQTT (see C) |
| 3 | 750-450 4 RTD inputs | `kbus/module/3/temperature/<n>` in °C |
| 4 | 750-671 stepper controller, 12 bytes in/out | the gateway's stepper driver (see B) |

Because CODESYS runs without KBUS, the project must **not** contain a KBus in
its device tree. If it still has one with these modules, tell me and propose
how to remove it; don't remove it yourself.

## Before writing code: look first, then report

1. **MQTT library.** Find out which MQTT client the project already uses
   (e.g. `WagoAppCloud` with `FbPublishMQTT_2` / `FbSubscribeMQTT_2` on a
   *Connection*, or a direct MQTT client library). Read the real interfaces
   (inputs, outputs, data types, how payloads are passed) of the publish and
   subscribe blocks in the library manager before using them. Use the same
   library as the existing code. With `WagoAppCloud`, the connections are
   configured in the WBM (*Cloud Connectivity*). On this controller (checked
   21-11-2026):
   - **Connection 2** → `192.168.178.215:1883` = the broker of the KBUS node
     (Native MQTT, client ID `CODESYS`): **use Connection 2 for all `kbus/...`
     topics**;
   - Connection 1 → `192.168.178.213:1883`, another broker: leave it to the
     code that uses it now.

   After a change in *Cloud Connectivity* the controller must be restarted
   (the cloud service does not reload its configuration by itself).
2. **WAGO libraries for the modules.** Find the standard WAGO libraries and
   function blocks the project has (or can add from the installed libraries)
   for the **750-671** and the **750-657** (IO-Link). List their names,
   versions and interfaces. For the 750-671 they are the model for the
   interface of section B; for the 750-657 they are used directly (C).
3. **JSON.** Check whether a JSON library (e.g. `WagoAppJSON`) is available.
   If not, write a small parser for the flat JSON objects below (only
   strings, integers and `true`/`false`).
4. Report what you found and your plan in a few lines, then wait for my OK.

## A. Digital outputs, inputs and temperatures

| What | Topic | Payload |
|---|---|---|
| Write outputs, all in one KBUS cycle | publish `kbus/do/all/set` | JSON array, `[true,false,null,…]` (`null` = unchanged), **without retain** |
| Write one output | publish `kbus/do/<n>/set` | `true` / `false`, without retain |
| State of the outputs | subscribe `kbus/do/all` (or `kbus/do/<n>`) | `[true,false,…]` / `true` |
| Temperatures 750-450 | subscribe `kbus/module/3/temperature/<n>` | `21.4` (°C, text). `850.0` = the upper range value, normally wire break / no sensor |
| Is the I/O current? | subscribe `kbus/status`, `kbus/bus/healthy` | `online`, `true` |
| Rejected commands | subscribe `kbus/error` | `{"topic":"…","error":"…"}` |

Make a GVL `GVL_KBUS` with `axDO : ARRAY[1..8] OF BOOL` (written by the
application), `axDO_State`, `arTemperature : ARRAY[1..4] OF REAL`,
`xOnline`, `xBusHealthy`, `sLastError`. A program `prgKbusMqtt` publishes
`kbus/do/all/set` **only when `axDO` changes** (and once after a reconnect or
after `kbus/status` went from `offline` to `online`, because the gateway
starts with all outputs 0). It never publishes every cycle.

## B. 750-671 stepper controller (through the gateway's driver)

The gateway runs the module's handshake itself (Enable → Ready, mode → mode
ACK, Start edge → Start_ACK). The raw process image of this module is **not**
available for writing (`kbus/module/4/out/set` is refused), so do **not**
connect a standard WAGO stepper FB to raw bytes. Instead write
`FB_MqttStepper` with an interface **modelled on the standard WAGO stepper
FB** you found in step 2 (same names and meaning where possible), that
talks to these topics:

| Direction | Topic | Payload |
|---|---|---|
| command | publish `kbus/module/4/stepper/cmd/set` (without retain) | `{"cmd":"enable","id":"7"}`, `{"cmd":"disable"}`, `{"cmd":"quit"}`, `{"cmd":"stop"}`, `{"cmd":"move","position":P,"velocity":V,"acceleration":A,"id":"8"}`, `{"cmd":"reference","direction":"neg"}` or `"pos"` |
| result | subscribe `kbus/module/4/stepper/response` | `{"cmd":"move","id":"8","result":"accepted"}` then `"done"`, or `"result":"error","error":"…"` |
| state | subscribe `kbus/module/4/stepper/state` (retained) | `{"enabled":true,"ready":true,"stopAck":true,"mode":"positioning","busy":false,"onTarget":true,"standstill":true,"referenced":true,"error":false,"reset":false,"input1":true,"input2":false,"position":2000,"velocity":0,"command":null}` |
| invalid command | subscribe `kbus/error` | `{"topic":"kbus/module/4/stepper/cmd/set","error":"…"}` |

Rules for the commands:

- `position` is absolute (−8388608 … 8388607), `velocity` 1 … 25000,
  `acceleration` 1 … 32767. A new `move` while moving changes the target on
  the fly.
- `enable` also acknowledges a module reset. It fails with "Input1 (stop
  circuit) is open" when DI1 of the module is not wired/closed.
- `disable` is refused while the drive moves: send `stop` first.
- Give every command a new `id` (a counter as text) and match the response on
  it. The FB is `xBusy` from sending until `done` or `error` (or a timeout of
  your choice, e.g. 30 s for a move), then `xDone` or `xError` with the error
  text, like a standard WAGO execute FB (rising edge on `xExecute` / per
  command input).
- Outputs of the FB from `state`: `xReady`, `xBusy`, `xOnTarget`,
  `xStandstill`, `xReferenced`, `xModuleError`, `diActualPosition`,
  `iActualVelocity`.

## C. 750-657 IO-Link master (standard WAGO FB, raw process image)

The gateway has no IO-Link driver yet, so here the standard WAGO IO-Link
function block(s) found in step 2 are used, with their process image
connected over MQTT:

| Direction | Topic | Payload |
|---|---|---|
| module → FB | subscribe `kbus/module/2/in` | **24 bytes binary**, the module's input process image, from one KBUS cycle, only when it changes (retained) |
| FB → module | publish `kbus/module/2/out/set` (without retain) | **exactly 24 bytes binary**, the module's output process image |

Write `FB_MqttIoLinkBridge` that copies the received 24 bytes into the input
data structure the WAGO FB expects, and publishes the FB's 24 output bytes
**only when they change**. Then use the WAGO FB for the ports as usual
(cyclic port data, port status, parameter read/write).

Know the limit and write it as a comment in the FB: every handshake step
(mailbox / acyclic parameter access) now takes one MQTT round trip
(typically 20–40 ms) instead of one KBUS cycle. Cyclic port data works;
acyclic calls are slower. Later the gateway gets an IO-Link driver and this
bridge is replaced by functional topics.

## D. Supervision

- The gateway can switch all outputs to a safe state when the application
  disappears (`MQTT_WATCHDOG_S`, may be off). Publish `kbus/watchdog/set`
  (payload `1`, without retain) **every 1 s** from `prgKbusMqtt`, so the
  application is ready for it. The stepper then stops with its braking ramp.
- When `kbus/status` is `offline` or `kbus/bus/healthy` is `false`, treat all
  I/O values as invalid (`xOnline := FALSE`) and don't start new stepper
  commands.

## Rules

- Never publish `.../set` topics with the retain flag.
- Publish only on change (outputs, IO-Link output bytes) or on an explicit
  command (stepper), never every task cycle; the heartbeat is the exception.
- Call `prgKbusMqtt` from the existing main task; keep the publish/subscribe
  blocks in it (one instance per topic).
- Add comments in English, short.
- At the end: build, list the build result (errors/warnings), the new objects,
  how to use `FB_MqttStepper` and `FB_MqttIoLinkBridge` from the rest of the
  application, and what I still have to do (WBM connection, device tree,
  download). Download nothing yourself.
