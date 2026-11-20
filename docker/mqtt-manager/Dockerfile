# mqtt-manager (optional): the KBUS Manager page at http(s)://<pfc>/kbus/ (WBM layout).
# Serves web/ + its API on 127.0.0.1:9080 and links /kbus in the WAGO web server (lighttpd);
# the page reads the broker through /kbus/ws (mosquitto 127.0.0.1:9001, read only) and
# writes through the API (login checked, published on 127.0.0.1:1883).
# Build context: the PROJECT ROOT (for web/). From the project root:
#   docker buildx build --platform linux/arm64 -f docker/mqtt-manager/Dockerfile -t mqtt-manager:arm64 --load .
# (--restart unless-stopped: a 'docker run' option, not possible in a Dockerfile; always use it)
#   docker run -d --name mqtt-manager --restart unless-stopped --network host \
#       --privileged --pid host -v /home/mqtt-manager:/data mqtt-manager:arm64
# Usually started by tools/deploy-docker.sh <pfc-ip> --manager.
# Same base image as the mqtt-bridge: the layers are shared on the controller.
FROM python:3.12-slim
LABEL org.opencontainers.image.title="mqtt-manager" \
      org.opencontainers.image.description="KBUS Manager page for kbus-gateway (MQTT), linked at /kbus in the WAGO web server"
WORKDIR /app
COPY docker/mqtt-manager/mqtt_manager.py ./
COPY web/ ./www/
ENV PYTHONUNBUFFERED=1
STOPSIGNAL SIGTERM
ENTRYPOINT ["python", "/app/mqtt_manager.py"]
