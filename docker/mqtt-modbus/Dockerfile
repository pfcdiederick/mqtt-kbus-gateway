# mqtt-modbus: Modbus TCP devices <-> the MQTT broker of the PFC. Build context:
# this folder. From the project root:
#
#   docker buildx build --platform linux/arm64 -t mqtt-modbus:arm64 --load docker/mqtt-modbus
#
# (--restart unless-stopped: a 'docker run' option, not possible in a Dockerfile; always use it)
#   docker run -d --name mqtt-modbus --restart unless-stopped --network host \
#       -v /home/mqtt-modbus:/config:ro mqtt-modbus:arm64
#
# /home/mqtt-modbus/mqtt-modbus.yaml on the controller is the configuration
# (tools/deploy-docker.sh <pfc-ip> --modbus creates it from mqtt-modbus.yaml).
#
# The first stage runs on the build PC and downloads the packages for the target
# platform (pymodbus and paho-mqtt are pure Python, PyYAML has wheels for arm64 and
# amd64; for armv7 its pure-Python part is used); the final image has no RUN step,
# so no QEMU emulation is needed.
FROM --platform=$BUILDPLATFORM python:3.12-slim AS deps
ARG TARGETARCH
RUN pip install --no-cache-dir --target /deps pymodbus==3.6.9 paho-mqtt==2.1.0 && \
    case "$TARGETARCH" in \
        arm64) pip install --no-cache-dir --target /deps --only-binary=:all: --platform manylinux2014_aarch64 \
                   --python-version 3.12 --implementation cp PyYAML==6.0.2 ;; \
        amd64) pip install --no-cache-dir --target /deps --only-binary=:all: --platform manylinux2014_x86_64 \
                   --python-version 3.12 --implementation cp PyYAML==6.0.2 ;; \
        *)     pip install --no-cache-dir --target /deps PyYAML==6.0.2 && rm -f /deps/yaml/*.so ;; \
    esac

FROM python:3.12-slim
LABEL org.opencontainers.image.title="mqtt-modbus" \
      org.opencontainers.image.description="Modbus TCP devices <-> MQTT on the WAGO PFC"
WORKDIR /app
COPY --from=deps /deps /usr/local/lib/python3.12/site-packages
COPY mqtt_modbus.py mqtt-modbus.yaml ./
ENV PYTHONUNBUFFERED=1 MODBUS_CONFIG=/config/mqtt-modbus.yaml
STOPSIGNAL SIGTERM
ENTRYPOINT ["python", "/app/mqtt_modbus.py"]
