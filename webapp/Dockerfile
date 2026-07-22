# syntax=docker/dockerfile:1
#
# Layer-cache discipline: npm ci is keyed on the lockfile COPY alone, so a
# source-only change re-runs just the `COPY . .` and build layers. CI
# persists these layers through the GitHub cache service (see docs/ci.md).
FROM node:26-slim AS build
WORKDIR /app
ARG APP_VERSION=dev
ENV APP_VERSION=${APP_VERSION}
COPY package.json package-lock.json ./
# BuildKit cache mount keeps npm tarballs between builds when the builder
# instance persists (local dev); in CI the layer cache does the work.
RUN --mount=type=cache,target=/root/.npm npm ci
COPY . .
RUN npm run build

FROM node:26-slim AS runtime
WORKDIR /app
ENV NODE_ENV=production

COPY --from=build /app/dist ./dist
COPY --from=build /app/server ./server
COPY --from=build /app/mock ./mock
COPY --from=build /app/node_modules ./node_modules

EXPOSE 8085
CMD ["node", "server/unified-server.mjs"]
