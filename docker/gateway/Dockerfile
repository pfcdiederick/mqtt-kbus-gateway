# syntax=docker/dockerfile:1
#
# kbus-gateway (WAGO KBUS <-> MQTT) as a container - multi-platform build with docker buildx.
# Alternative to the native package (tools/build-ipk.sh); run it with
# tools/deploy-docker.sh, which knows the mounts the KBUS access needs (docs/MANUAL.md 2.8).
# The build context is the PROJECT ROOT (src/, sdk/, VERSION); ignore file:
# Dockerfile.dockerignore next to this file. From the project root:
#
#   PFC300 (arm64):
#     docker buildx build --platform linux/arm64 -f docker/gateway/Dockerfile \
#         -t kbus-gateway:3.0.0-arm64 --load .
#   PFC100 / PFC200 (armv7): --platform linux/arm/v7
#
#   Simulation image for a PC (no SDK headers needed):
#     docker buildx build --platform linux/amd64 --build-arg REQUIRE_ADI=0 \
#         -f docker/gateway/Dockerfile -t kbus-gateway:sim --load .
#
# Run on the controller (tools/deploy-docker.sh does exactly this). The restart policy is a
# 'docker run' option, it cannot be set in a Dockerfile: always start it with
# --restart unless-stopped, so it comes back after a reboot:
#     docker run -d --name kbus-gateway --restart unless-stopped --privileged --network host \
#         -v /usr/lib:/host/usr/lib:ro -v /var/run/dbus:/var/run/dbus -v /etc/specific:/etc/specific:ro \
#         -v /home/kbus-gateway:/home/kbus-gateway -v /dev/log:/dev/log -e LOG_TARGET=syslog \
#         kbus-gateway:3.0.0-arm64
#
# The build stage always runs natively on the build host and cross-compiles;
# the runtime stage has no RUN steps, so no QEMU emulation is required.

ARG DEBIAN_RELEASE=trixie

# ---------------------------------------------------------------------------
# Build stage (runs on the build host, cross-compiles for $TARGETPLATFORM)
# ---------------------------------------------------------------------------
FROM --platform=$BUILDPLATFORM debian:${DEBIAN_RELEASE} AS build
ARG TARGETARCH
ARG BUILDARCH
# 1 = fail when the WAGO SDK headers (sdk/include) are missing
ARG REQUIRE_ADI=1
SHELL ["/bin/bash", "-c"]

RUN set -eux; \
    case "${TARGETARCH}" in \
      arm)   triplet=arm-linux-gnueabihf; debarch=armhf ;; \
      arm64) triplet=aarch64-linux-gnu;   debarch=arm64 ;; \
      amd64) triplet=x86_64-linux-gnu;    debarch=amd64 ;; \
      *) echo "unsupported TARGETARCH ${TARGETARCH}"; exit 1 ;; \
    esac; \
    apt-get update; \
    if [ "${TARGETARCH}" = "${BUILDARCH}" ]; then \
        apt-get install -y --no-install-recommends make gcc libc6-dev binutils; \
    else \
        apt-get install -y --no-install-recommends make binutils-${triplet//_/-} \
            gcc-${triplet//_/-} libc6-dev-${debarch}-cross; \
    fi; \
    rm -rf /var/lib/apt/lists/*; \
    echo "${triplet}" > /triplet

WORKDIR /src
COPY Makefile VERSION ./
COPY src ./src
COPY sdk ./sdk

RUN set -eux; \
    triplet=$(cat /triplet); \
    make CC="${triplet}-gcc" REQUIRE_ADI="${REQUIRE_ADI}" CFLAGS="-O2 -g0"; \
    "${triplet}-strip" build/kbus-gateway

# ---------------------------------------------------------------------------
# Runtime stage
# ---------------------------------------------------------------------------
# glibc of this image must be >= the PFC firmware's glibc, because the WAGO
# libraries are loaded from the controller at runtime (see README).
FROM debian:${DEBIAN_RELEASE}-slim AS runtime
LABEL org.opencontainers.image.title="kbus-gateway" \
      org.opencontainers.image.description="WAGO KBUS DI/DO/AI/AO on an MQTT broker"

COPY --from=build /src/build/kbus-gateway /usr/local/bin/kbus-gateway
COPY --chmod=755 docker/gateway/entrypoint.sh /usr/local/bin/entrypoint.sh

# The same defaults as the native package (packaging/ipk/etc/kbus-gateway.conf).
# Run with --network host: the broker (mosquitto) listens on 127.0.0.1:1883 of the controller.
ENV MQTT_HOST=127.0.0.1 \
    MQTT_PORT=1883 \
    MQTT_TOPIC_PREFIX=kbus \
    MQTT_PRIORITY=30 \
    KBUS_CYCLE_MS=5 \
    KBUS_PRIORITY=40 \
    KBUS_SETTINGS_STORE=/home/kbus-gateway/kbus-settings.conf
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
