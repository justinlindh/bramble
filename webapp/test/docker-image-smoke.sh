#!/usr/bin/env bash
set -euo pipefail

IMAGE_TAG="bramble-webapp-smoke:local"
CONTAINER_NAME="bramble-web-test"

cleanup() {
  docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cleanup

echo "[smoke] building image: $IMAGE_TAG"
docker build -t "$IMAGE_TAG" .

echo "[smoke] starting container: $CONTAINER_NAME"
docker run --rm -d --name "$CONTAINER_NAME" -p 18080:80 "$IMAGE_TAG" >/dev/null

for _ in $(seq 1 30); do
  if curl -fsS http://127.0.0.1:18080/ >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

echo "[smoke] checking /healthz"
curl -fS http://127.0.0.1:18080/healthz

echo "[smoke] checking SPA fallback"
curl -fS http://127.0.0.1:18080/non-existent-route | grep -qi '<!doctype html>'

echo "PASS: docker image smoke test"
