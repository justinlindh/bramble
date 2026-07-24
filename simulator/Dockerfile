# syntax=docker/dockerfile:1
#
# Build context is the repo root (compose sets `context: ..`); the root
# .dockerignore whitelists exactly the trees the COPY steps below read.
# Layer-cache discipline: each stage installs its dependencies (keyed on the
# lockfiles alone) before any source COPY, so a source change re-runs only
# that stage's final compile layer. CI persists these layers through the
# GitHub cache service (see docs/ci.md).

# Stage 1: Build React UI
FROM node:26-slim AS ui-build
WORKDIR /app/simulator/ui
COPY simulator/ui/package*.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 2: Build Go server (with embedded C via cgo).
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
# simulator/engine/sim_node.h includes ../../main/beacon_policy_calc.h, so the
# firmware main/ tree has to be in the build context too. Without it the cgo
# build fails at that include and the image cannot be built at all.
COPY main/ main/
COPY test/stubs/ test/stubs/
COPY simulator/engine/ simulator/engine/
COPY simulator/gosim/ simulator/gosim/
WORKDIR /app/simulator/gosim
RUN --mount=type=cache,target=/go/pkg/mod \
    --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=1 go build -o /bramble-gosim .

# Stage 3: Runtime (minimal)
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates libssl3 && rm -rf /var/lib/apt/lists/*
COPY --from=go-build /bramble-gosim /usr/local/bin/bramble-gosim
COPY --from=ui-build /app/simulator/ui/dist /ui
COPY simulator/scenarios /scenarios
EXPOSE 3000
CMD ["bramble-gosim", "--ui", "/ui", "--scenarios", "/scenarios"]
