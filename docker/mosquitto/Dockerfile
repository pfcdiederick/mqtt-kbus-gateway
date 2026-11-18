# kbus-mosquitto: the MQTT broker for the PFC, with its configuration built in.
#   mosquitto.conf: MQTT on port 1883 (no login), MQTT over WebSocket on 127.0.0.1:9001
#                   (read only, for the monitor page of the mqtt-manager)
# Build context: this folder.
#   docker buildx build --platform linux/arm64 -t kbus-mosquitto:arm64 --load docker/mosquitto
# Run on the controller (tools/deploy-docker.sh --mosquitto does this). The restart policy is a
# 'docker run' option, it cannot be set in a Dockerfile: always use --restart unless-stopped.
#   docker run -d --name mosquitto --restart unless-stopped --network host \
#       -v /home/mosquitto/data:/mosquitto/data kbus-mosquitto:arm64
FROM eclipse-mosquitto:2.0.20
LABEL org.opencontainers.image.title="kbus-mosquitto" \
      org.opencontainers.image.description="mosquitto for kbus-gateway (WAGO PFC)"
COPY mosquitto.conf acl-monitor /mosquitto/config/
