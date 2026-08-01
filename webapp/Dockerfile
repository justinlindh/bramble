# syntax=docker/dockerfile:1
#
# Layer-cache discipline: npm ci is keyed on the lockfile COPY alone, so a
# source-only change re-runs just the `COPY . .` and build layers. CI
# persists these layers through the GitHub cache service (see docs/ci.md).
#
# `--platform=$BUILDPLATFORM` pins this stage to the builder's own
# architecture, so the multi-platform publish (linux/amd64 + linux/arm64,
# see .github/workflows/webapp-build-publish.yml) runs `npm ci` and
# `vite build` exactly once instead of once per target. That is only correct
# because the artifacts this stage produces are architecture independent:
# `dist` is bundled JS/CSS, and every runtime dependency is pure JS (the
# runtime tree is ws + @noble/hashes and their transitive deps; the
# architecture-specific packages in this project, esbuild/rollup/electron,
# are all devDependencies and are pruned below).
#
# Node major: keep in step with `.nvmrc`. scripts/lint/check-node-version.sh
# gates that and runs on every pull request.
FROM --platform=$BUILDPLATFORM node:25-slim AS build
WORKDIR /app
ARG APP_VERSION=dev
ENV APP_VERSION=${APP_VERSION}
COPY package.json package-lock.json ./
# BuildKit cache mount keeps npm tarballs between builds when the builder
# instance persists (local dev); in CI the layer cache does the work.
RUN --mount=type=cache,target=/root/.npm npm ci
COPY . .
RUN npm run build
# Reduce the tree the runtime stage copies to production dependencies only.
# `prune` reuses the install above rather than re-resolving it, and it is what
# keeps the cross-platform copy honest: without it the arm64 image would carry
# the builder's x64 esbuild/rollup binaries and an unused ~400 MB of electron
# and playwright.
RUN npm prune --omit=dev

FROM node:25-slim AS runtime
WORKDIR /app
ENV NODE_ENV=production

COPY --from=build /app/dist ./dist
COPY --from=build /app/server ./server
COPY --from=build /app/mock ./mock
COPY --from=build /app/node_modules ./node_modules

EXPOSE 8085
CMD ["node", "server/unified-server.mjs"]
