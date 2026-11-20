#!/usr/bin/env bash
# Run kbus-gateway as a Docker container on the PFC instead of the native package
# (or switch back), and optionally the broker, the mqtt-bridge and the mqtt-manager.
#
#   tools/deploy-docker.sh <pfc-ip> [options]
#     (no option)    kbus-gateway container (replaces the native service)
#     --bridge       also the mqtt-bridge (docker/mqtt-bridge): topics of other brokers -> PFC broker
#     --mosquitto    also the broker: kbus-mosquitto (docker/mosquitto, configuration built in)
#     --manager      also the mqtt-manager (docker/mqtt-manager, optional): the KBUS Manager page at
#                    http(s)://<pfc>/kbus/, linked in the WAGO web server
#     --sim          kbus-gateway in simulation mode (KBUS_SIMULATION=1: no real KBUS access,
#                    e.g. while CODESYS still owns the KBUS)
#     --no-gateway   leave kbus-gateway as it is (native or container); e.g. only --bridge
#     --native       remove the kbus-gateway container, native service again
#   Images: docker/<gateway|mqtt-bridge|mosquitto|mqtt-manager>/Dockerfile (docker/README.md); archives: build-docker/
#   All containers run with --restart unless-stopped: they come back after a reboot.
#
# Only one program may own the KBUS: the native service is stopped and its boot
# link removed while the container runs (--native restores both).
#
# Environment:
#   PFC_USER        SSH user (default root)
#   SKIP_BUILD      1 = use the existing archives in build-docker/
#   GW_ENV          extra docker run arguments for kbus-gateway, e.g. "-e MQTT_TOPIC_PREFIX=line1/kbus"
#   BR_ENV          extra docker run arguments for the mqtt-bridge
#   MG_ENV          extra docker run arguments for the mqtt-manager, e.g. "-e MGMT_READONLY=1"
#
# Files on the controller (created at the first deployment, then kept):
#   /home/mqtt-bridge/mqtt-bridge.ini   bridge configuration (+ certificates); docker restart mqtt-bridge
#   /home/mosquitto/data/                retained values of the broker
#   /home/mqtt-manager/settings.json     login of the KBUS Manager page
# The broker configuration is built into the image: docker/mosquitto/mosquitto.conf.
set -euo pipefail

PFC=${1:?usage: $0 <pfc-ip> [--bridge] [--mosquitto] [--manager] [--sim] [--no-gateway] | --native}
shift
GW=1; BR=0; MQ=0; MG=0; NATIVE=0; SIM=0
for a in "$@"; do
    case "$a" in
        --bridge)     BR=1 ;;
        --mosquitto)  MQ=1 ;;
        --manager)    MG=1 ;;
        --no-gateway) GW=0 ;;
        --native)     NATIVE=1 ;;
        --sim)        SIM=1 ;;
        *) echo "unknown option $a" >&2; exit 1 ;;
    esac
done
PFC_USER=${PFC_USER:-root}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
SSH="ssh -o BatchMode=yes $PFC_USER@$PFC"
OUT=$ROOT/build-docker

if [[ $NATIVE == 1 ]]; then
    echo ">>> back to the native service"
    $SSH 'docker rm -f kbus-gateway >/dev/null 2>&1 || true
          [ -x /etc/init.d/kbus-gateway ] || { echo "native package not installed (tools/deploy.sh)"; exit 1; }
          ln -sf ../init.d/kbus-gateway /etc/rc.d/S99_kbus-gateway
          /etc/init.d/kbus-gateway start'
    exit 0
fi

machine=$($SSH uname -m)
case "$machine" in
    aarch64) PLATFORM=linux/arm64; ARCH=arm64 ;;
    armv7*)  PLATFORM=linux/arm/v7; ARCH=armv7 ;;
    *) echo "unknown controller architecture '$machine'" >&2; exit 1 ;;
esac
GW_IMG=kbus-gateway:$VERSION-$ARCH
BR_IMG=mqtt-bridge:$ARCH
GW_TAR=$OUT/kbus-gateway-$VERSION-$ARCH.tar.gz
BR_TAR=$OUT/mqtt-bridge-$ARCH.tar.gz
MQ_IMG=kbus-mosquitto:$ARCH
MQ_TAR=$OUT/kbus-mosquitto-$ARCH.tar.gz
MG_IMG=mqtt-manager:$ARCH
MG_TAR=$OUT/mqtt-manager-$ARCH.tar.gz
echo ">>> controller $PFC is $machine -> $PLATFORM"

if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
    echo ">>> 1/4 building images"
    mkdir -p "$OUT"
    # keep only the archive on the PC
    save() { docker save "$1" | gzip > "$2" && docker rmi "$1" >/dev/null; }
    if [[ $GW == 1 ]]; then
        docker buildx build --platform "$PLATFORM" -t "$GW_IMG" -f "$ROOT/docker/gateway/Dockerfile" --load "$ROOT"
        save "$GW_IMG" "$GW_TAR"
    fi
    if [[ $BR == 1 ]]; then
        docker buildx build --platform "$PLATFORM" -t "$BR_IMG" --load "$ROOT/docker/mqtt-bridge"
        save "$BR_IMG" "$BR_TAR"
    fi
    if [[ $MQ == 1 ]]; then
        docker buildx build --platform "$PLATFORM" -t "$MQ_IMG" --load "$ROOT/docker/mosquitto"
        save "$MQ_IMG" "$MQ_TAR"
    fi
    if [[ $MG == 1 ]]; then
        docker buildx build --platform "$PLATFORM" -t "$MG_IMG" -f "$ROOT/docker/mqtt-manager/Dockerfile" --load "$ROOT"
        save "$MG_IMG" "$MG_TAR"
    fi
else
    echo ">>> 1/4 SKIP_BUILD=1: using $OUT"
fi

echo ">>> 2/4 loading images on the controller"
[[ $GW == 1 ]] && $SSH docker load < "$GW_TAR"
[[ $BR == 1 ]] && $SSH docker load < "$BR_TAR"
[[ $MQ == 1 ]] && $SSH docker load < "$MQ_TAR"
[[ $MG == 1 ]] && $SSH docker load < "$MG_TAR"

if [[ $MQ == 1 ]]; then
    echo ">>> starting the broker (port 1883)"
    # shellcheck disable=SC2087
    $SSH sh -s <<EOF
set -e
mkdir -p /home/mosquitto/data
chown -R 1883:1883 /home/mosquitto/data 2>/dev/null || true
docker rm -f mosquitto >/dev/null 2>&1 || true
docker run -d --name mosquitto --restart unless-stopped --network host \
    -v /home/mosquitto/data:/mosquitto/data -v /etc/localtime:/etc/localtime:ro $MQ_IMG >/dev/null
EOF
fi

if [[ $GW == 1 ]]; then
echo ">>> 3/4 stopping the native service, starting the kbus-gateway container"
# shellcheck disable=SC2087
$SSH sh -s <<EOF
set -e
for s in kbus-gateway; do                           # native service (version 2 HTTP/wdx or 3 MQTT)
    if [ -x /etc/init.d/\$s ]; then
        /etc/init.d/\$s stop
        rm -f /etc/rc.d/S99_\$s                       # not at boot while the container is used
    fi
done
docker rm -f kbus-gateway kbus-mqtt >/dev/null 2>&1 || true   # kbus-mqtt: name of 1.0.0
mkdir -p /home/kbus-gateway
OPT=""
for f in /etc/config-tools/get_typelabel_value:ro /dev/eeprom: /dev/ttyKbus: /dev/log: /etc/localtime:ro; do
    p=\${f%%:*}; ro=\${f#*:}; [ -n "\$ro" ] && ro=":\$ro"
    if [ -e "\$p" ]; then OPT="\$OPT -v \$p:\$p\$ro"; else echo "note: \$p not present, not mounted"; fi
done
docker run -d --name kbus-gateway --restart unless-stopped \
    --privileged --network host \
    -v /usr/lib:/host/usr/lib:ro \
    -v /var/run/dbus:/var/run/dbus \
    -v /etc/specific:/etc/specific:ro \
    -v /home/kbus-gateway:/home/kbus-gateway \
    \$OPT -e LOG_TARGET=syslog -e KBUS_SIMULATION=$SIM ${GW_ENV:-} \
    $GW_IMG >/dev/null
EOF
else
    echo ">>> 3/4 kbus-gateway left as it is (--no-gateway)"
fi

if [[ $BR == 1 ]]; then
    echo ">>> starting the mqtt-bridge"
    $SSH "mkdir -p /home/mqtt-bridge && chmod 700 /home/mqtt-bridge"
    if ! $SSH test -s /home/mqtt-bridge/mqtt-bridge.ini; then
        $SSH "umask 077; cat > /home/mqtt-bridge/mqtt-bridge.ini" < "$ROOT/docker/mqtt-bridge/mqtt-bridge.ini"
        echo "default bridge configuration created: /home/mqtt-bridge/mqtt-bridge.ini (add your [remote ...] sections)"
    fi
    $SSH "docker rm -f mqtt-bridge >/dev/null 2>&1 || true
          docker run -d --name mqtt-bridge --restart unless-stopped --network host ${BR_ENV:-} \
              -v /home/mqtt-bridge:/config:ro -v /etc/localtime:/etc/localtime:ro $BR_IMG >/dev/null"
fi

if [[ $MG == 1 ]]; then
    echo ">>> starting the mqtt-manager (KBUS Manager page, /kbus in the WAGO web server)"
    $SSH "mkdir -p /home/mqtt-manager && chmod 700 /home/mqtt-manager
          docker rm -f mqtt-manager >/dev/null 2>&1 || true
          docker run -d --name mqtt-manager --restart unless-stopped --network host --privileged --pid host \
              -v /home/mqtt-manager:/data -v /etc/localtime:/etc/localtime:ro ${MG_ENV:-} $MG_IMG >/dev/null"
fi

echo ">>> 4/4 status"
# shellcheck disable=SC2016
$SSH 'sleep 4; docker ps -a --filter name=kbus-gateway --filter name=mqtt-bridge --filter name=mosquitto --filter name=mqtt-manager \
          --format "{{.Names}}: {{.Status}}";
      grep "kbus-gateway\[" /var/log/messages | tail -8'
[[ $BR == 1 ]] && echo ">>> bridge: ssh $PFC_USER@$PFC docker logs -f mqtt-bridge  (configuration: /home/mqtt-bridge/mqtt-bridge.ini)"
echo ">>> topics: mosquitto_sub -h $PFC -t 'kbus/#' -t 'bridge/#' -v"
[[ $MG == 1 ]] && echo ">>> KBUS Manager: http://$PFC/kbus  (log: ssh $PFC_USER@$PFC docker logs mqtt-manager)"
[[ $GW == 1 ]] && echo ">>> done. Back to the native service: $0 $PFC --native"
exit 0
