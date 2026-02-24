#!/usr/bin/env bash
set -euo pipefail

# Host-side publisher: extracts artifacts from a successful firmware-build action run
# and publishes them into OTA layout + index.

GITEA_DB_CONTAINER=${GITEA_DB_CONTAINER:-gitea-db}
GITEA_DB_USER=${GITEA_DB_USER:-gitea}
GITEA_DB_NAME=${GITEA_DB_NAME:-gitea}
GITEA_DATA_ROOT=${GITEA_DATA_ROOT:-/home/justin/src/dockers/gitea/data/gitea/actions_artifacts}
OTA_ROOT=${OTA_ROOT:-/home/justin/src/dockers/ota}
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

# Prefer explicit VERSION, then git semver-ish describe, then fallback.
VERSION=${VERSION:-}
if [[ -z "$VERSION" ]]; then
  if command -v git >/dev/null 2>&1; then
    VERSION=$(git describe --tags --always --dirty 2>/dev/null || true)
  fi
fi
[[ -n "$VERSION" ]] || VERSION="ci-run-${RUN_ID}"

# File-safe variant for filenames
VERSION_FILE_SAFE=$(echo "$VERSION" | tr '/ ' '__')
DEST_REL="$CHANNEL/$VERSION/$BOARD"

# Ensure OTA root exists from host perspective via docker daemon mount
if ! docker run --rm -v "$OTA_ROOT:/ota" alpine:3.20 sh -lc 'test -d /ota'; then
  echo "ERROR: OTA root not accessible via docker mount: $OTA_ROOT" >&2
  exit 1
fi

for name in bootloader.bin partition-table.bin bramble.bin; do
  SP=$(run_sql "select storage_path from action_artifact where run_id=$RUN_ID and artifact_path='$name' limit 1;")
  [[ -n "$SP" ]] || { echo "Missing artifact $name for run $RUN_ID" >&2; exit 1; }

  # Keep canonical filename for flasher compatibility + create semver-marked copy.
  docker run --rm \
    -v "$GITEA_DATA_ROOT:/artifacts:ro" \
    -v "$OTA_ROOT:/ota" \
    alpine:3.20 sh -lc '
      set -eu
      src="/artifacts/'"$SP"'"
      out="/ota/'"$DEST_REL/$name"'"
      tagged="/ota/'"$DEST_REL/${name%.bin}-$VERSION_FILE_SAFE.bin"'"
      mkdir -p "$(dirname "$out")"
      [ -f "$src" ] || { echo "missing artifact blob: $src" >&2; exit 1; }
      gzip -dc "$src" > "$out"
      cp "$out" "$tagged"

      for f in "$out" "$tagged"; do
        sha=$(sha256sum "$f" | awk "{print \$1}")
        size=$(wc -c < "$f" | tr -d " ")
        printf "{\"sha256\":\"%s\",\"size\":%s}\\n" "$sha" "$size" > "$f.meta.json"
      done
    '
done

published_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
docker run --rm -v "$OTA_ROOT:/ota" alpine:3.20 sh -lc '
  set -eu
  mkdir -p "/ota/'"$CHANNEL/$VERSION"'"
  printf "{\"published_at\":\"%s\"}\\n" '"$published_at"' > "/ota/'"$CHANNEL/$VERSION"'/release-meta.json"
'

# Build index.json in container with /ota mount.
docker run --rm -v "$OTA_ROOT:/ota" python:3.12-alpine python - <<'PY'
import json, pathlib, re
root=pathlib.Path('/ota')
releases=[]
for ch in [p for p in root.iterdir() if p.is_dir()]:
    for ver in [p for p in ch.iterdir() if p.is_dir()]:
        meta=ver/'release-meta.json'
        if not meta.exists():
            continue
        published_at=json.loads(meta.read_text()).get('published_at')
        artifacts=[]
        for board in [p for p in ver.iterdir() if p.is_dir()]:
            for f in board.iterdir():
                if (not f.is_file()) or f.name.endswith('.meta.json'):
                    continue
                mpath=pathlib.Path(str(f)+'.meta.json')
                m=json.loads(mpath.read_text()) if mpath.exists() else {}
                artifacts.append({
                    'board': board.name,
                    'file': f'/ota/{ch.name}/{ver.name}/{board.name}/{f.name}',
                    'sha256': m.get('sha256',''),
                    'size': m.get('size', f.stat().st_size),
                })
        releases.append({
            'version': ver.name,
            'published_at': published_at,
            'channel': ch.name,
            'artifacts': artifacts,
        })

releases.sort(key=lambda r:(r['published_at'], r['version']), reverse=True)
out={'releases': releases}
(root/'index.json').write_text(json.dumps(out, indent=2))

# Inline validator (keeps workflow independent of host path assumptions)
errs=[]
if not isinstance(out.get('releases'), list):
    errs.append('Top-level releases must be array')
for i,rel in enumerate(out['releases']):
    p=f'releases[{i}]'
    for k in ('version','published_at','channel','artifacts'):
        if k not in rel:
            errs.append(f'{p}.{k} missing')
    if not isinstance(rel.get('artifacts'), list):
        errs.append(f'{p}.artifacts must be array')
    for j,a in enumerate(rel.get('artifacts',[])):
        ap=f'{p}.artifacts[{j}]'
        for k in ('board','file','sha256','size'):
            if k not in a:
                errs.append(f'{ap}.{k} missing')
        sha=str(a.get('sha256',''))
        if sha and not re.fullmatch(r'[0-9a-fA-F]{64}', sha):
            errs.append(f'{ap}.sha256 invalid')
        size=a.get('size')
        if not isinstance(size, int) or size <= 0:
            errs.append(f'{ap}.size invalid')
if errs:
    print('Validation failed:')
    for e in errs:
        print('-', e)
    raise SystemExit(1)

print(f'wrote index with {len(releases)} release(s)')
PY

echo "Published run $RUN_ID as $CHANNEL/$VERSION/$BOARD"
