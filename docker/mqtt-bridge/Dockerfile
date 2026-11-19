# mqtt-bridge: subscribe to topics on other MQTT brokers and publish them on the
# MQTT broker of the PFC. Build context: this folder. From the project root:
#
#   docker buildx build --platform linux/arm64 -t mqtt-bridge:arm64 --load docker/mqtt-bridge
#
# (--restart unless-stopped: a 'docker run' option, not possible in a Dockerfile; always use it)
#   docker run -d --name mqtt-bridge --restart unless-stopped --network host \
#       -v /home/mqtt-bridge:/config mqtt-bridge:arm64
#
# /home/mqtt-bridge/mqtt-bridge.ini on the controller is the configuration
# (tools/deploy-docker.sh <pfc-ip> --bridge creates it from mqtt-bridge.ini).
#
# paho-mqtt is pure Python: the first stage runs on the build PC and only
# downloads it; the final image has no RUN step, so no QEMU emulation is needed.
FROM --platform=$BUILDPLATFORM python:3.12-slim AS deps
RUN pip install --no-cache-dir --target /deps paho-mqtt==2.1.0

FROM python:3.12-slim
LABEL org.opencontainers.image.title="mqtt-bridge" \
      org.opencontainers.image.description="Topics of other MQTT brokers -> the MQTT broker on the WAGO PFC"
WORKDIR /app
COPY --from=deps /deps /usr/local/lib/python3.12/site-packages
COPY mqtt_bridge.py mqtt-bridge.ini ./
ENV PYTHONUNBUFFERED=1 BRIDGE_CONFIG=/config/mqtt-bridge.ini
STOPSIGNAL SIGTERM
ENTRYPOINT ["python", "/app/mqtt_bridge.py"]
