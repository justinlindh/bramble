# syntax=docker/dockerfile:1
#
# Zero-prerequisite path for the Bramble emulator: `docker compose up --build`
# from emulator/. Build context is the repo root (see docker-compose.yml).
#
# Four stages: ESP-IDF linux-target node build, gosim (Go+cgo) build, React UI
# build, then a minimal runtime image. The final image lays out the same
# repo-relative paths (emulator/node/build/..., simulator/gosim/...,
# simulator/scenarios/...) that the scenario files' "firmware_nodes[].binary"
# fields expect (resolved relative to gosim's working directory), so the
# bundled emulator scenarios (emu-channel-delivery, emu-dm-desync,
# emulator-3-pagers) work unmodified in the container.

# Stage 1: ESP-IDF linux target, build the firmware node binary
FROM debian:bookworm AS idf-build
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates python3 python3-venv cmake ninja-build \
        build-essential pkg-config libusb-1.0-0 libbsd-dev \
    && rm -rf /var/lib/apt/lists/*

ARG IDF_VERSION=v5.4.1
RUN git clone --branch ${IDF_VERSION} --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/espressif/esp-idf.git /opt/esp-idf
ENV IDF_PATH=/opt/esp-idf
# Pin the ESP-IDF component registry to the canonical host. The
# idf-component-manager bundled with the v5.4.1 install defaults to the old
# `https://api.components.espressif.com/` subdomain, which no longer has a
# DNS record (NXDOMAIN), so a clean build that has to resolve the managed
# components (libsodium, lvgl) fails with "Cannot establish a connection to
# the component registry". A populated emulator/node/dependencies.lock hides
# this (it records the canonical URL and pins resolved components), but that
# file is gitignored, so a from-clean build like CI's has none and hits the
# dead default. The current manager (2.4.x, what the host build uses) already
# defaults to `https://components.espressif.com/`; setting it explicitly makes
# the build correct regardless of the installed manager version's default and
# matches the URL dependencies.lock records.
ENV IDF_COMPONENT_REGISTRY_URL=https://components.espressif.com/
# Requests the linux target only. ESP-IDF's installer still pulls the
# xtensa/riscv32 cross toolchains as part of its "core" tool set even so
# (no smaller install profile is exposed for this preview target on v5.4.1);
# only the final runtime stage's size matters, and that stage never sees
# this layer.
RUN /opt/esp-idf/install.sh linux

WORKDIR /app
COPY components/ components/
COPY main/ main/
COPY emulator/node/ emulator/node/

RUN bash -lc '. /opt/esp-idf/export.sh \
    && cd emulator/node \
    && idf.py --preview set-target linux \
    && idf.py build'

# Stage 2: gosim (Go server embedding Bramble C code via cgo)
FROM golang:1.26-bookworm AS go-build
RUN apt-get update && apt-get install -y gcc libc6-dev libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY simulator/gosim/go.mod simulator/gosim/go.sum simulator/gosim/
RUN --mount=type=cache,target=/go/pkg/mod \
    cd simulator/gosim && go mod download
COPY components/ components/
COPY main/ main/
COPY test/stubs/ test/stubs/
COPY simulator/engine/ simulator/engine/
COPY simulator/gosim/ simulator/gosim/
WORKDIR /app/simulator/gosim
RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=1 go build -o /bramble-gosim .

# Stage 3: React UI
FROM node:26-slim AS ui-build
WORKDIR /app/simulator/ui
COPY simulator/ui/package*.json .
RUN --mount=type=cache,target=/root/.npm \
    npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 4: runtime (minimal)
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libssl3 libbsd0 libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=idf-build /app/emulator/node/build/bramble-node.elf emulator/node/build/bramble-node.elf
COPY --from=idf-build /app/emulator/node/build/partition_table/partition-table.bin emulator/node/build/partition_table/partition-table.bin
COPY --from=go-build /bramble-gosim simulator/gosim/bramble-gosim
COPY --from=ui-build /app/simulator/ui/dist simulator/ui/dist
COPY simulator/scenarios simulator/scenarios
EXPOSE 3000
# gosim binds an unqualified ":3000" (all interfaces, i.e. 0.0.0.0:3000); no
# extra host/bind flag is needed here or in compose's port mapping.
CMD ["simulator/gosim/bramble-gosim", "--ui", "simulator/ui/dist", "--scenarios", "simulator/scenarios", "--port", "3000"]
