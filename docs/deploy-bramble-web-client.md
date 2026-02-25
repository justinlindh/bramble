# Deploying bramble web client image

The unified runtime web client image is published by CI from `.gitea/workflows/webapp-build-publish.yml`.

## Registry image name

- `ghcr.io/example/justinlindh/bramble-web-client`

## Tag rules

- `main` for pushes to the `main` branch
- Immutable SHA tag for every published build:
  - `sha-<full git sha>`
- Semantic release tags (`vX.Y.Z`) additionally publish:
  - `vX.Y.Z`
  - `vX.Y`
  - `vX`
  - `sha-<full git sha>`

## Docker build source

CI builds the unified runtime image from:

- Dockerfile: `webapp/Dockerfile`
- Build context: `webapp`

This Dockerfile starts `server/unified-server.mjs` and exposes port `8085`.
