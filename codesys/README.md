# CODESYS and the KBUS through MQTT

kbus-gateway owns the KBUS, so the CODESYS runtime runs **without KBUS** and
reaches the I/O through the MQTT broker on the controller, with any CODESYS
MQTT client library.

## The setup on the PFC300

| | |
|---|---|
| Firmware runtime | WBM → *PLC Runtime* → **None** |
| CODESYS | container `cds3` from [docker-codesys3-wagopfc](https://github.com/pfcdiederick/docker-codesys3-wagopfc) with **`KBUS=false`** (image built from the controller's firmware: `tools/build-on-pfc.sh <pfc>`, `tools/run-on-pfc.sh <pfc> false`) |
| KBUS | container `kbus-gateway` (or the native package) |
| Broker | container `mosquitto` (kbus-mosquitto), `127.0.0.1:1883`, no login |

With `KBUS=false` the CODESYS container removes its KBUS drivers (and, from
FW 31, the BACnet component that needs them). Therefore:

- the CODESYS project must **not contain a KBus** in its device tree;
- BACnet is not available in CODESYS in this setup;
- use the device description of the controller's firmware (PFC300, FW 31).

The container starts empty: download the application with the IDE
(`<pfc-ip>`, port 11740). Application and settings are kept in
`/home/cds3data` on the controller.

## Topics

| Broker | `127.0.0.1:1883`, no login |
|---|---|
| Read inputs | subscribe `kbus/di/+`, `kbus/ai/+`, or the arrays `kbus/di/all`, `kbus/ai/all` (JSON) |
| Read the outputs' state | `kbus/do/+`, `kbus/ao/+` |
| Write outputs | publish `kbus/do/<n>/set` = `true`/`false`, `kbus/ao/<n>/set` = `0..65535`, **without retain** |
| Several outputs in one KBUS cycle | publish `kbus/do/all/set` = `[true,false,null,...]` (`null` = unchanged) |
| Is the I/O current? | `kbus/status` = `online` and `kbus/bus/healthy` = `true` |
| Rejected writes | subscribe `kbus/error` |
| Temperatures of a 750-450 | subscribe `kbus/module/<pos>/temperature/<n>` (°C, e.g. `21.4`) |
| Write watchdog (if `MQTT_WATCHDOG_S` is set) | publish `kbus/watchdog/set` (any value, without retain) at least every N/2 s; `kbus/watchdog` = `expired` means the outputs were switched to 0 |
| 750-671 stepper | publish `kbus/module/<pos>/stepper/cmd/set` = `{"cmd":"enable"}`, `{"cmd":"move","position":P,"velocity":V,"acceleration":A,"id":"1"}`, `{"cmd":"stop"}` …; subscribe `kbus/module/<pos>/stepper/response` (`accepted` / `done` / `error`) and `.../stepper/state` (README 3.12) |
| COM1: receive | subscribe `kbus/serial/com1/rx`: one message per frame, binary (README 3.9) |
| COM1: send | publish `kbus/serial/com1/tx/set` = 1..512 bytes, binary, **without retain** |
| COM1: settings | publish `kbus/serial/com1/baudrate/set`, `format/set` (e.g. `8E1`), `framegap/set` (ms) only when they differ from the retained `kbus/serial/com1/baudrate`, `format`, `framegap` |
| Data from other brokers | as configured in the mqtt-bridge, e.g. `homeassistant/sensor/.../state` |

Recommendations:

- Subscribe to the single topics (`kbus/di/+`) when the application reacts to
  changes; they need no JSON parsing (`true`/`false`, plain integers).
- Publish an output only when its value changes, not every task cycle:
  kbus-gateway keeps the value. After a restart of kbus-gateway (`kbus/status`
  `offline` → `online`) all outputs are 0: write them again.
- Values are raw: a 750-559 (0–10 V) has 32767 at 10 V.
- Check the result live at `https://<pfc-ip>/kbus` (KBUS Manager page).

## The 750-495 with the standard WAGO function block

The power measurement module has no channels; kbus-gateway passes its 24 raw
input and output bytes through MQTT (`kbus/module/2/in`,
`kbus/module/2/out/set`), so the standard function block of
WagoAppPowerMeasurement can be used as "virtual I/O". Step-by-step guide,
ST skeleton, timing and checks: main [README](../README.md), section 3.8.

Current node (PFC300, 18-11-2026): module 1 = 8DO → `kbus/do/1` … `kbus/do/8`;
module 2 = 750-495 (no channels).

The CODESYS project and the ST code of the HTTP/wdx version are in the original
project `docker-wdx-kbus-interface/codesys`. ST code for MQTT is not written yet.

## Prompt for Claude in CODESYS

[PROMPT-for-Claude-in-CODESYS.md](PROMPT-for-Claude-in-CODESYS.md): ready-to-paste task for Claude in CODESYS (MCP) that builds the MQTT interface of the test node: digital outputs, 750-450 temperatures, `FB_MqttStepper` (750-671 through the gateway driver) and `FB_MqttIoLinkBridge` (750-657 with the standard WAGO IO-Link FB over the raw process image).
