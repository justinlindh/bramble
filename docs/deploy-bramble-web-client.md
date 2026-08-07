# Deploying bramble web client image

The unified runtime web client image is published by
`.github/workflows/webapp-build-publish.yml`. That workflow is a reusable
workflow: `release-components.yml` calls it after semantic-release cuts a
`webapp-v*` tag, so an image exists for every released webapp version and for
no other commit. It also accepts a `workflow_dispatch` with a `webapp_tag`
input, which rebuilds and republishes an existing release tag.

Publishing and deploying are two separate steps. The workflow only pushes the
image. Rolling the hosted site forward is a GitOps action, described under
[Deploy](#deploy) below.

## Registry image name

`${REGISTRY_IMAGE_REPO}`, from the `REGISTRY_IMAGE_REPO` repo variable, with
`${REGISTRY_HOST}` from the `REGISTRY_HOST` repo variable. Pushing also needs
the `REGISTRY_USERNAME` and `REGISTRY_PAT` secrets. The workflow's first step
fails the run when any of the four is unset, and checks that the image repo
lives under the registry host. There is no placeholder default: an
unconfigured publish fails loudly instead of pushing somewhere unintended.

The values are repo variables rather than literals in the workflow because
`scripts/lint/check-no-internal-refs.sh` forbids the private registry
hostname in this public tree.

## Tag rules

Every published build gets exactly two immutable tags:

- `v<version>`, the released webapp version, without the `webapp-` prefix
- `sha-<full git sha>` of the tagged commit

There are no floating tags. Nothing deploys from a moving tag, and a moving
tag would defeat the semver ordering the deploy side relies on.

## Registry media types

The registry implements the OCI distribution spec without Docker schema2
compatibility, and rejects a Docker manifest with `manifest invalid`. A plain
`docker buildx build --push` emits schema2 and always fails against it, so the
workflow pushes with `--output type=image,push=true,oci-mediatypes=true` and
exports its build cache with `oci-mediatypes=true,image-manifest=true`. A
verification step reads both tags back and fails the run unless the published
manifest is an OCI image index carrying the expected platforms.

## Docker build source

CI builds the unified runtime image from:

- Dockerfile: `webapp/Dockerfile`
- Build context: `webapp`

The workflow stages `web-flasher/` into `webapp/public/web-flasher` first,
because the build context is `webapp`. It runs no tests: `webapp-quality.yml`
already gates the same commit, and the image's own `npm run build` is
`npm run typecheck && vite build`, so a type error fails the build before
anything is pushed.

This Dockerfile starts `server/unified-server.mjs` and exposes port `8085`.

## Deploy

The hosted site runs as a Kubernetes Deployment reconciled by ArgoCD from a
private GitOps repository, with self-heal enabled. Self-heal reverts a direct
`kubectl set image`, so the image reference in the GitOps manifest is the only
thing that moves the site: the deploy is a commit there, not an action taken
by this repo's CI.

`argocd-image-updater` watches the registry for new `v<version>` tags and
writes the bump into that repository, which ArgoCD then syncs. Bramble's CI
therefore holds no credential for the cluster or the GitOps repository, and
its only publish privilege is pushing to the one registry repository.
