# Stage 1: Build C simulator
FROM debian:bookworm-slim AS builder-c
RUN apt-get update && apt-get install -y gcc make && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY components/ ./components/
COPY test/stubs/ ./test/stubs/
COPY simulator/engine/ ./simulator/engine/
RUN cd simulator/engine && make clean && make

# Stage 2: Build React UI
FROM node:22-slim AS builder-ui
WORKDIR /build
COPY simulator/ui/package*.json ./
RUN npm ci
COPY simulator/ui/ ./
RUN npm run build

# Stage 3: Runtime
FROM node:22-slim
WORKDIR /app
COPY simulator/server/package*.json ./server/
RUN cd server && npm ci --production
COPY simulator/server/ ./server/
COPY --from=builder-c /build/simulator/engine/bramble-sim ./engine/bramble-sim
COPY --from=builder-ui /build/dist ./ui/dist
COPY simulator/scenarios/ ./scenarios/

EXPOSE 3000
WORKDIR /app/server
CMD ["npx", "tsx", "relay.ts"]
