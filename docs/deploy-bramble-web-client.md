# Deploying bramble web client image

The unified runtime web client image is published by CI from `.gitea/workflows/webapp-build-publish.yml`.

## Registry image name

- `${REGISTRY_IMAGE_REPO}` (set via the `REGISTRY_IMAGE_REPO` repo variable in CI; defaults to `registry.example.com/bramble/bramble-web-client` when unset)

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
