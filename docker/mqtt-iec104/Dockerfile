# mqtt-iec104: gateway between the MQTT broker of the PFC and an IEC 60870-5-104
# server (c104 = Python binding of mz-automation/lib60870-C). Build context: this
# folder. From the project root:
#
#   docker buildx build --platform linux/arm64 -t mqtt-iec104:arm64 --load docker/mqtt-iec104
#
# (--restart unless-stopped: a 'docker run' option, not possible in a Dockerfile; always use it)
#   docker run -d --name mqtt-iec104 --restart unless-stopped --network host \
#       -v /home/mqtt-iec104:/config:ro mqtt-iec104:arm64
#
# /home/mqtt-iec104/mqtt-iec104.yaml on the controller is the configuration
# (tools/deploy-docker.sh <pfc-ip> --iec104 creates it from mqtt-iec104.yaml).
#
# The first stage runs on the build PC and downloads the prebuilt wheels for the
# target platform; the final image has no RUN step, so no QEMU emulation is needed.
# c104 has wheels for linux/amd64 and linux/arm64 (PFC300), not for linux/arm/v7.
FROM --platform=$BUILDPLATFORM python:3.12-slim AS deps
ARG TARGETARCH
RUN case "$TARGETARCH" in \
        arm64) a=aarch64 ;; \
        amd64) a=x86_64 ;; \
        *) echo "c104 has no wheel for $TARGETARCH" >&2; exit 1 ;; \
    esac && \
    pip install --no-cache-dir --target /deps --only-binary=:all: \
        --platform manylinux2014_$a --platform manylinux_2_28_$a --python-version 3.12 --implementation cp \
        c104==2.2.1 paho-mqtt==2.1.0 PyYAML==6.0.2

FROM python:3.12-slim
LABEL org.opencontainers.image.title="mqtt-iec104" \
      org.opencontainers.image.description="MQTT <-> IEC 60870-5-104 gateway for the WAGO PFC"
WORKDIR /app
COPY --from=deps /deps /usr/local/lib/python3.12/site-packages
COPY mqtt_iec104.py mqtt-iec104.yaml ./
ENV PYTHONUNBUFFERED=1 IEC104_CONFIG=/config/mqtt-iec104.yaml
EXPOSE 2404
STOPSIGNAL SIGTERM
ENTRYPOINT ["python", "/app/mqtt_iec104.py"]
