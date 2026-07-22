# syntax=docker/dockerfile:1
#
# Zero-prerequisite path for the Bramble emulator: `docker compose up --build`
# from emulator/. Build context is the repo root (see docker-compose.yml); the
# root .dockerignore whitelists exactly the trees the COPY steps below read.
#
# Four stages: ESP-IDF linux-target node build, gosim (Go+cgo) build, React UI
# build, then a minimal runtime image. The final image lays out the same
# repo-relative paths (emulator/node/build/..., simulator/gosim/...,
# simulator/scenarios/...) that the scenario files' "firmware_nodes[].binary"
# fields expect (resolved relative to gosim's working directory), so the
# bundled emulator scenarios (emu-channel-delivery, emu-dm-desync,
# emulator-3-pagers) work unmodified in the container.
#
# Layer-cache discipline: every stage installs its toolchain and dependencies
# before any source COPY, so a source change re-runs only the final compile
# layer of each stage. CI persists these layers through the GitHub cache
# service (see docs/ci.md).

# Stage 1: ESP-IDF linux target, build the firmware node binary.
# espressif/idf:v5.4.1 is the same pinned toolchain base that
# docker/firmware-builder/Dockerfile uses: ESP-IDF and its tools arrive as
# base image layers and are never rebuilt. The previous revision git-cloned
# esp-idf and ran install.sh inside the build, which redid the clone plus the
# full tool install on every cold build and dominated this image's build time.
FROM espressif/idf:v5.4.1 AS idf-build
# Host-compiler deps for the linux preview target: the base image carries the
# cross toolchains, cmake, and ninja, but the linux target compiles with the
# host gcc and links libbsd.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config libbsd-dev \
    && rm -rf /var/lib/apt/lists/*
# Pin the ESP-IDF component registry to the canonical host. The
# idf-component-manager bundled with v5.4.1 defaults to the old
# `https://api.components.espressif.com/` subdomain, which no longer has a
# DNS record (NXDOMAIN), so resolving the managed components (libsodium,
# lvgl) fails with "Cannot establish a connection to the component registry".
# A populated emulator/node/dependencies.lock would mask this locally, but it
# is gitignored AND dockerignored, so container builds always resolve from
# this URL, exactly like CI.
ENV IDF_COMPONENT_REGISTRY_URL=https://components.espressif.com/

WORKDIR /app
COPY components/ components/
COPY main/ main/
COPY emulator/node/ emulator/node/

RUN bash -c '. "$IDF_PATH/export.sh" \
    && cd emulator/node \
    && idf.py --preview set-target linux \
    && idf.py build'

# Stage 2: gosim (Go server embedding Bramble C code via cgo).
# golang:1.26-bookworm already ships gcc, g++, libc6-dev, and pkg-config;
# only the OpenSSL headers the cgo build needs are missing.
FROM golang:1.26-bookworm AS go-build
RUN apt-get update && apt-get install -y --no-install-recommends libssl-dev \
    && rm -rf /var/lib/apt/lists/*
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
COPY simulator/ui/package*.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 4: runtime (minimal). ubuntu:24.04 to match the idf-build stage's
# base (espressif/idf:v5.4.1 is Ubuntu 24.04, glibc 2.39): the node ELF links
# against the build stage's glibc, and a debian:bookworm runtime (glibc 2.36)
# refuses to load it (`GLIBC_2.38' not found). libssl3t64 is noble's name for
# the libssl.so.3 runtime the gosim cgo build links.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libssl3t64 libbsd0 libstdc++6 \
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
