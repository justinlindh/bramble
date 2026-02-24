#!/usr/bin/env bash
set -euo pipefail

# Host-side publisher: extracts artifacts from latest successful firmware-build action run
# and publishes them into OTA layout + index.

GITEA_DB_CONTAINER=${GITEA_DB_CONTAINER:-gitea-db}
GITEA_DB_USER=${GITEA_DB_USER:-gitea}
GITEA_DB_NAME=${GITEA_DB_NAME:-gitea}
GITEA_DATA_ROOT=${GITEA_DATA_ROOT:-/home/user/src/dockers/gitea/data/gitea/actions_artifacts}
OTA_ROOT=${OTA_ROOT:-/home/user/src/dockers/ota}
CHANNEL=${CHANNEL:-dev}
BOARD=${BOARD:-heltec-v3}

run_sql() {
  docker exec "$GITEA_DB_CONTAINER" psql -U "$GITEA_DB_USER" -d "$GITEA_DB_NAME" -Atc "$1"
}

RUN_ID=${1:-${RUN_ID:-}}
if [[ -z "${RUN_ID}" ]]; then
  RUN_ID=$(run_sql "select r.id from action_run r where r.workflow_id='firmware-build.yml' and r.status=1 and exists (select 1 from action_artifact a where a.run_id=r.id) order by r.id desc limit 1;")
fi
[[ -n "$RUN_ID" ]] || { echo "No successful firmware-build run with artifacts found" >&2; exit 1; }

VERSION="ci-run-${RUN_ID}"
[[ -f "$OTA_ROOT/index.json" ]] || { echo "OTA root not mounted/initialized at $OTA_ROOT" >&2; exit 1; }
DEST="$OTA_ROOT/$CHANNEL/$VERSION/$BOARD"
mkdir -p "$DEST"

for name in bootloader.bin partition-table.bin bramble.bin; do
  SP=$(run_sql "select storage_path from action_artifact where run_id=$RUN_ID and artifact_path='$name' limit 1;")
  [[ -n "$SP" ]] || { echo "Missing artifact $name for run $RUN_ID" >&2; exit 1; }

  docker run --rm \
    -v "$GITEA_DATA_ROOT:/artifacts:ro" \
    -v "$OTA_ROOT:/ota" \
    alpine:3.20 sh -lc '
      set -eu
      src="/artifacts/'"$SP"'"
      out="/ota/'"$CHANNEL/$VERSION/$BOARD/$name"'"
      [ -f "$src" ] || { echo "missing artifact blob: $src" >&2; exit 1; }
      gzip -dc "$src" > "$out"
      sha=$(sha256sum "$out" | awk "{print \$1}")
      size=$(wc -c < "$out" | tr -d " ")
      printf "{\"sha256\":\"%s\",\"size\":%s}\\n" "$sha" "$size" > "$out.meta.json"
    '
done

published_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
printf '{"published_at":"%s"}\n' "$published_at" > "$OTA_ROOT/$CHANNEL/$VERSION/release-meta.json"

node "$(dirname "$0")/build-firmware-index.js" "$OTA_ROOT" "$OTA_ROOT/index.json"
node "$(dirname "$0")/validate-firmware-index.js" "$OTA_ROOT/index.json"

echo "Published run $RUN_ID as $CHANNEL/$VERSION/$BOARD"
