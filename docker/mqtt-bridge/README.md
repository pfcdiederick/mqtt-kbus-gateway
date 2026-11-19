# mqtt-bridge: topics of other MQTT brokers on the PFC broker

Subscribes to topics on one or more remote MQTT brokers and publishes every
message on the broker of the WAGO PFC (mosquitto, `127.0.0.1:1883`). Python 3.12
with [paho-mqtt](https://pypi.org/project/paho-mqtt/) 2.1.0.

```
remote broker(s) ── subscribe ── mqtt-bridge (container, --network host) ── publish ──▶ mosquitto on the PFC
```

## Files

| File | |
|---|---|
| `mqtt_bridge.py` | the bridge |
| `mqtt-bridge.ini` | default configuration, all settings explained (copied to the controller at the first deployment) |
| `Dockerfile`, `.dockerignore` | image; build context is this folder |

## Deploy

```bash
tools/deploy-docker.sh 192.168.1.17 --bridge --no-gateway
```

The first deployment creates `/home/mqtt-bridge/mqtt-bridge.ini` on the
controller (mounted as `/config`, read-only). Later deployments keep it. Put
certificates next to it and name them relative to that folder. After a change:

```bash
ssh root@192.168.1.17 vi /home/mqtt-bridge/mqtt-bridge.ini
ssh root@192.168.1.17 docker restart mqtt-bridge
ssh root@192.168.1.17 docker logs -f mqtt-bridge
```

By hand (on the PFC, after `docker load`):

```bash
docker run -d --name mqtt-bridge --restart unless-stopped --network host \
    -v /home/mqtt-bridge:/config:ro mqtt-bridge:arm64
```

## Configuration in short

```ini
[local]                     ; the broker on the PFC
host = 127.0.0.1
port = 1883

[remote plant]              ; one section per remote broker
host = broker.plant.local
port = 8883
tls = true
ca_file = plant-ca.crt
username = pfc300-01
password = secret
qos = 1
retain = always             ; always | keep | never
strip_prefix = site1/
topics =
    site1/setpoints/#  -> plant              ; -> plant/setpoints/...
    site1/hall/temp    -> hall/temperature   ; exactly this topic
```

| Remote topic | Line | Local topic |
|---|---|---|
| `site1/setpoints/line1/speed` | `site1/setpoints/# -> plant` + `strip_prefix = site1/` | `plant/setpoints/line1/speed` |
| `factory/line1/temp` | `factory/# -> plant` | `plant/factory/line1/temp` |
| `factory/line1/temp` | `factory/line1/temp -> hall/temperature` | `hall/temperature` |
| `sensors/a` | `sensors/+` (no local part) | `sensors/a` |

Status on the local broker (retained): `bridge/status` and
`bridge/<name>/status` = `online` / `offline`. Without any `[remote ...]`
section the bridge runs idle and says so in the log.

## Test without a PFC

```bash
docker run -d --name local  --network host eclipse-mosquitto:2.0.20 mosquitto -p 18830
docker run -d --name remote --network host eclipse-mosquitto:2.0.20 mosquitto -p 18831
pip install paho-mqtt
python3 docker/mqtt-bridge/mqtt_bridge.py --config my-test.ini   # [local] port 18830, [remote x] port 18831
```
