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
- use the device description of the controller's firmware.

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
| Status of an analog input | subscribe `kbus/ai/<n>/status` (0 = no error) |
| Write watchdog (if `MQTT_WATCHDOG_S` is set) | publish `kbus/watchdog/set` (any value, without retain) at least every N/2 s; `kbus/watchdog` = `expired` means the outputs were switched to 0 |
| 750-671 stepper | publish `kbus/module/<pos>/stepper/cmd/set` = `{"cmd":"enable"}`, `{"cmd":"move","position":P,"velocity":V,"acceleration":A,"id":"1"}`, `{"cmd":"stop"}` …; subscribe `.../stepper/response` (`accepted` / `done` / `error`) and `.../stepper/state` |
| 750-495, 750-657 (raw data) | subscribe `kbus/module/<pos>/in`, publish `kbus/module/<pos>/out/set` (binary, without retain); see below |
| COM1: receive | subscribe `kbus/serial/com1/rx`: one message per frame, binary |
| COM1: send | publish `kbus/serial/com1/tx/set` = 1..512 bytes, binary, **without retain** |
| COM1: settings | publish `kbus/serial/com1/baudrate/set`, `format/set` (e.g. `8E1`), `framegap/set` (ms) only when they differ from the retained `kbus/serial/com1/baudrate`, `format`, `framegap` |
| Data from other brokers | as configured in the mqtt-bridge, e.g. `homeassistant/sensor/.../state` |

All details: [docs/MANUAL.md](../docs/MANUAL.md) (topics: section 3, module
functions: section 4, COM1: section 5).

Recommendations:

- Subscribe to the single topics (`kbus/di/+`) when the application reacts to
  changes; they need no JSON parsing (`true`/`false`, plain integers).
- Publish an output only when its value changes, not every task cycle:
  kbus-gateway keeps the value. After a restart of kbus-gateway (`kbus/status`
  `offline` → `online`) all outputs are 0: write them again.
- Values are raw: a 750-559 (0–10 V) has 32767 at 10 V.
- Check the result live at `https://<pfc-ip>/kbus` (KBUS Manager page).

## The 750-495 with the standard WAGO function block

On a normal PFC the 750-495 sits in the KBus device tree, its 24 input and 24
output bytes are mapped to variables, and the function block of the WAGO
library **WagoAppPowerMeasurement** works with those variables. Here the
CODESYS runtime has no KBus, so the same 24 bytes come and go through MQTT
instead ("virtual I/O"). The function block does not notice the difference.

```
 750-495 ◀─KBUS─▶ kbus-gateway ── kbus/module/2/in      (24 bytes) ──▶ CODESYS: copy into the FB's input bytes
                              ◀── kbus/module/2/out/set (24 bytes) ◀── CODESYS: the FB's output bytes
```

1. **Check the function block.** In the library manager, open the 750-495
   function block of WagoAppPowerMeasurement. It must take the module's
   process data as **ordinary variables** (an input and an output structure
   or byte array of 24 bytes each). If it needs a reference to the KBus
   device instead, this route does not work.
2. **Device tree:** no KBus. Add the MQTT client library you already use;
   broker `127.0.0.1:1883`.
3. **Subscribe** to `kbus/module/2/in` (`2` = position of the module in the
   node, see `kbus/modules`). Payload: binary, exactly 24 bytes from one KBUS
   cycle. Copy them into the function block's input variable when a message
   arrives.
4. **Call the function block** every task cycle, as usual.
5. **Publish** the function block's 24 output bytes on
   `kbus/module/2/out/set` when they change (binary, exactly 24 bytes,
   **retain off**, QoS 0 or 1). This is also needed when the application only
   reads measured values: the function block requests every value through
   these bytes. kbus-gateway rejects any other length (`kbus/error`).
6. **Watch** `kbus/status` (`online`) and `kbus/bus/healthy` (`true`). After a
   restart of kbus-gateway the output bytes are 0 until the application writes
   them again: reset the copy of the last sent bytes when `kbus/status`
   becomes `online`, so step 5 sends them again.

Skeleton in ST. The publish block, the 750-495 function block and their
parameters are written as comments, because they depend on the library
versions in the project:

```iecst
VAR
    oFbSubscribeIn : (* subscribe block of your MQTT library *);   // kbus/module/2/in
    oFbPublishOut  : (* publish block of the same library *);      // kbus/module/2/out/set
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

Adapt the names and parameters of the MQTT function blocks to your library,
and the 750-495 function block's in/out variables to its declaration.

- **Timing.** Every request of the function block now takes a round trip
  through MQTT (CODESYS → broker → kbus-gateway → next KBUS cycle → module →
  kbus-gateway → broker → CODESYS), roughly 10–50 ms instead of one KBUS
  cycle. Measured values every second or so are no problem. If the function
  block has a time-out for the module's answer, set it to at least 500 ms.
- **One writer.** Only this CODESYS application may publish
  `kbus/module/2/out/set`. The KBUS Manager page cannot write it.
- **Diagnosis:** `mosquitto_sub -h <pfc-ip> -t kbus/module/2/in -F "%U %x"`
  (time + 24 bytes in hex).
