# OTA Publish Endpoint Runbook

Last verified: 2026-03-01

## Purpose
Verify the endpoint-driven OTA publish flow works end-to-end and provide a fast rollback procedure.

## Preconditions
- `ota-publisher` service is running on the OTA host.
- CI secret `OTA_PUBLISH_KEY` is set for firmware build workflow.
- Build artifacts are produced by `scripts/ci-build-firmware.sh`.

---

## 1) Trigger firmware build CI and confirm publish step

### Manual trigger
- In Gitea Actions, run **Firmware Build** (`.gitea/workflows/firmware-build.yml`) via `workflow_dispatch`.

### Expected result
- Build step passes.
- `Publish OTA via authenticated endpoint` step passes.
- `Verify published release completeness in index` step passes.

### Optional CLI checks
```bash
# On CI/runner logs: confirm publish + verify steps are green.
# On host, check publisher logs:
docker logs --tail 200 ota-publisher
```

---

## 2) Verify OTA outputs

### Confirm newest release in index
```bash
curl -fsSL https://bramblemesh.org/ota/index.json | jq '.releases[0]'
```

### Confirm expected release exists
```bash
VERSION=<expected-version>
CHANNEL=dev
curl -fsSL https://bramblemesh.org/ota/index.json \
  | jq -e --arg v "$VERSION" --arg c "$CHANNEL" 'any(.releases[]; .version==$v and .channel==$c)'
```

### Confirm canonical + tagged artifacts exist (all CI boards)
```bash
for BOARD in heltec-v3 tdeck-plus heltec-v4; do
  BASE="https://bramblemesh.org/ota/$CHANNEL/$VERSION/$BOARD"
  for f in \
    bootloader.bin partition-table.bin bramble.bin \
    bootloader-${VERSION#v}.bin partition-table-${VERSION#v}.bin bramble-${VERSION#v}.bin
  do
    curl -fsI "$BASE/$f" >/dev/null && echo "OK $BOARD/$f"
  done
done
```

---

## 3) Verify web flasher integration

1. Open web flasher.
2. Refresh release list.
3. Confirm newest release appears first.
4. Select each target board (`heltec-v3`, `tdeck-plus`, `heltec-v4`) and verify it resolves canonical files:
   - `bootloader.bin`
   - `partition-table.bin`
   - `bramble.bin`

If release list is stale, hard refresh and re-check `/ota/index.json` directly.

---

## 4) Rollback procedure

### A) Disable publish step in CI (fastest containment)
- In `.gitea/workflows/firmware-build.yml`, comment or gate:
  - `Publish OTA via authenticated endpoint`
  - `Verify published release completeness in index`

### B) Stop publisher service
```bash
cd /home/justin/src/dockers
docker compose stop ota-publisher
```

### C) Restore previous index
```bash
cd /home/justin/src/dockers/ota
cp index.json index.json.bad.$(date +%Y%m%d-%H%M%S)
cp <known-good-index-backup>.json index.json
```

### D) Validate rollback state
```bash
curl -fsSL https://bramblemesh.org/ota/index.json | jq '.releases[0:3]'
```

---

## 5) Recovery / re-enable checklist
- Root cause identified and fixed.
- `ota-publisher` healthy (`/healthz`).
- One manual publish test succeeds.
- Index + artifact verification pass.
- Re-enable CI publish steps.
