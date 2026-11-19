# mqtt-manager (optional): the MQTT monitor page at https://<pfc>/mqtt/.
# Serves web/ on 127.0.0.1:9080 and links /mqtt in the WAGO web server (lighttpd);
# the page reads the broker through /mqtt/ws (mosquitto 127.0.0.1:9001, read only).
# Build context: the PROJECT ROOT (for web/). From the project root:
#   docker buildx build --platform linux/arm64 -f docker/mqtt-manager/Dockerfile -t mqtt-manager:arm64 --load .
# (--restart unless-stopped: a 'docker run' option, not possible in a Dockerfile; always use it)
#   docker run -d --name mqtt-manager --restart unless-stopped --network host \
#       --privileged --pid host mqtt-manager:arm64
# Usually started by tools/deploy-docker.sh <pfc-ip> --manager.
# Same base image as the mqtt-bridge: the layers are shared on the controller.
FROM python:3.12-slim
LABEL org.opencontainers.image.title="mqtt-manager" \
      org.opencontainers.image.description="MQTT monitor page for kbus-gateway, linked at /mqtt in the WAGO web server"
WORKDIR /app
COPY docker/mqtt-manager/mqtt_manager.py ./
COPY web/ ./www/
ENV PYTHONUNBUFFERED=1
STOPSIGNAL SIGTERM
ENTRYPOINT ["python", "/app/mqtt_manager.py"]
