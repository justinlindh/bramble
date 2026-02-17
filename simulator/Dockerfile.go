# Stage 1: Build React UI
FROM node:22-slim AS ui-build
WORKDIR /app/simulator/ui
COPY simulator/ui/package*.json .
RUN npm ci
COPY simulator/ui/ .
RUN npm run build

# Stage 2: Build Go server (with embedded C via cgo)
FROM golang:1.24-bookworm AS go-build
RUN apt-get update && apt-get install -y gcc libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY components/ components/
COPY test/stubs/ test/stubs/
COPY simulator/engine/ simulator/engine/
COPY simulator/gosim/ simulator/gosim/
WORKDIR /app/simulator/gosim
RUN CGO_ENABLED=1 go build -o /bramble-gosim .

# Stage 3: Runtime (minimal)
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=go-build /bramble-gosim /usr/local/bin/bramble-gosim
COPY --from=ui-build /app/simulator/ui/dist /ui
COPY simulator/scenarios /scenarios
EXPOSE 3000
CMD ["bramble-gosim", "--ui", "/ui", "--scenarios", "/scenarios"]
