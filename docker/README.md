# Containers

One folder per function. They only talk to each other through the MQTT broker
on the controller, all with `--network host` (broker on `127.0.0.1:1883`).
All run with `--restart unless-stopped`: they come back after a reboot (Docker
starts at boot, `/etc/rc.d/S99_docker`). The restart policy is an option of
`docker run`, not of a Dockerfile: `tools/deploy-docker.sh` always sets it,
and every Dockerfile shows its `docker run` line with it. `docker stop <name>`
keeps a container stopped, also after a reboot; `docker start <name>` brings
it back.

```
 other brokers ──▶ mqtt-bridge ──┐
                                 ├──▶ kbus-mosquitto :1883 ◀── CODESYS, Node-RED, your containers
 KBUS ◀─────────▶ kbus-gateway ─────┘         :9001 (WebSocket, read only, 127.0.0.1)
                  (gateway/: container,        ▲
                   or the native .ipk)         │
 browser ─ https://<pfc>/mqtt ─ WBM web server ┴─ mqtt-manager (page, 127.0.0.1:9080)
```

| Folder | Image | Role | Needed? |
|---|---|---|---|
| [gateway/](gateway/) | `kbus-gateway:<version>-<arch>` | owns the KBUS, publishes it on MQTT, writes outputs from `.../set` | one kbus-gateway: this container **or** the native package (`tools/build-ipk.sh`) |
| [mosquitto/](mosquitto/) | `kbus-mosquitto:<arch>` | the broker, configuration built in ([mosquitto.conf](mosquitto/mosquitto.conf)) | when mosquitto is not installed otherwise |
| [mqtt-bridge/](mqtt-bridge/README.md) | `mqtt-bridge:<arch>` | subscribes on other brokers, publishes on the PFC broker | optional |
| [mqtt-manager/](mqtt-manager/) | `mqtt-manager:<arch>` | monitor page at `https://<pfc>/mqtt` (read only), keeps `/mqtt` linked in the WAGO web server | optional |

**Build context:** `gateway/` and `mqtt-manager/` build from the project root
(`src/` + `sdk/`, `web/`; ignore files `<folder>/Dockerfile.dockerignore`),
`mosquitto/` and `mqtt-bridge/` from their own folder.

## Build and deploy

`tools/deploy-docker.sh` builds the images for the controller's architecture on
the PC (no emulation needed), stores them in `build-docker/`, loads them on the
controller and starts them:

```bash
tools/deploy-docker.sh 192.168.1.17                               # kbus-gateway container (stops the native service)
tools/deploy-docker.sh 192.168.1.17 --sim                         # the same in simulation mode (no KBUS access)
tools/deploy-docker.sh 192.168.1.17 --mosquitto --no-gateway      # only the broker
tools/deploy-docker.sh 192.168.1.17 --bridge --no-gateway         # only the mqtt-bridge
tools/deploy-docker.sh 192.168.1.17 --manager --no-gateway        # only the monitor page
tools/deploy-docker.sh 192.168.1.17 --mosquitto --bridge --manager   # everything
SKIP_BUILD=1 tools/deploy-docker.sh ...                            # reuse the archives in build-docker/
tools/deploy-docker.sh 192.168.1.17 --native                      # kbus-gateway container -> native service again
```

By hand, from the project root:

```bash
docker buildx build --platform linux/arm64 -f docker/gateway/Dockerfile -t kbus-gateway:3.0.0-arm64 --load .
docker buildx build --platform linux/arm64 -t kbus-mosquitto:arm64 --load docker/mosquitto
docker buildx build --platform linux/arm64 -t mqtt-bridge:arm64 --load docker/mqtt-bridge
docker buildx build --platform linux/arm64 -f docker/mqtt-manager/Dockerfile -t mqtt-manager:arm64 --load .
```

Details: main [README](../README.md) 2.2 (broker), 2.8 (kbus-gateway container),
3.7 (monitor page), 4 (mqtt-bridge).
