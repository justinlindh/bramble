# syntax=docker/dockerfile:1

# Stage 1: Build React UI
FROM node:22-slim AS ui-build
WORKDIR /app/simulator/ui
COPY simulator/ui/package*.json .
RUN --mount=type=cache,target=/root/.npm \
    npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 2: Build Go server (with embedded C via cgo)
FROM golang:1.25-bookworm AS go-build
RUN apt-get update && apt-get install -y gcc libc6-dev libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY simulator/gosim/go.mod simulator/gosim/go.sum simulator/gosim/
RUN --mount=type=cache,target=/go/pkg/mod \
    cd simulator/gosim && go mod download
COPY components/ components/
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
