# Evidence: Webapp Build/Publish Workflow (Task 4)

## Failing check note (pre-change)

- Command:
  - `grep -n "name: Webapp Build Publish" /home/justin/src/bramble/.gitea/workflows/webapp-build-publish.yml`
- Result:
  - `grep: /home/justin/src/bramble/.gitea/workflows/webapp-build-publish.yml: No such file or directory`
- Status: ✅ expected failure observed before workflow creation.

## Implemented workflow

Created `.gitea/workflows/webapp-build-publish.yml` with:
- Triggers: push to `main` (`webapp/**` + workflow path), push tags `v*`, `workflow_dispatch`.
- Build gates: `npm ci`, `npm test`, `npm run build` in `webapp`.
- Publish target: `registry.idiotica.org/bramble/web-client`.
- Tagging:
  - `main` + `sha-<shortsha>` on `main`
  - `vX.Y.Z` + `vX.Y` + `vX` on semver tags.

## Validation

### 1) Static YAML parse check

```bash
python3 - <<'PY'
import yaml,sys
p='/home/justin/src/bramble/.gitea/workflows/webapp-build-publish.yml'
print('ok' if yaml.safe_load(open(p)) else 'bad')
PY
```

- `ok`
- Command actually run (same validation intent, with temporary venv because system python lacked `yaml`):
  - `python3 -m venv /tmp/task4-yaml-venv && /tmp/task4-yaml-venv/bin/pip install pyyaml && /tmp/task4-yaml-venv/bin/python - <<'PY' ...`

### 2) Local webapp build/test verification

```bash
cd /home/justin/src/bramble/webapp && npm ci && npm test && npm run build
```

- `npm ci` completed.
- `npm test` failed on an existing test unrelated to this workflow change:
  - `FAIL test/map/mapHelpers.test.ts > Map helpers > formats node addresses in uppercase hex`
  - Expected: `0x1A2B`
  - Received: `0x00001A2B`
- Because `npm test` failed, `npm run build` did not execute in the chained command.

## Notes

- Remote registry push and credentials are validated in CI runtime using repository secrets `REGISTRY_USERNAME` and `REGISTRY_PAT`.
