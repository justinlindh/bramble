# Bramble Testing Reference

Quick guide for what to run based on what changed.

## 1) Firmware host tests (C / Unity)

Run from repo root:

```bash
bash test/run_all_tests.sh
```

Use this after firmware changes in `components/**`, `main/**`, or protocol logic.

---

## 2) Webapp tests

```bash
cd webapp
npm ci
npm test
```

Use this after changes in `webapp/**`.

---

## 3) Simulator scenarios

Run from repo root:

```bash
cd simulator
bash scripts/run-scenario.sh ideal-10-node
# or
bash scripts/run-scenario.sh all
```

Use this for routing/reliability/airtime behavior validation.

---

## 4) Hardware build/flash validation

Use board-aware wrappers (preferred):

```bash
# Heltec V3
bash scripts/flash.sh local heltec-v3 build

# Heltec V4
bash scripts/flash.sh local heltec-v4 build

# T-Deck Plus
bash scripts/flash.sh local tdeck-plus build
```

For full flashing + board checks, see [BUILDING.md](BUILDING.md) and [heltec-v4-gnss-bringup.md](heltec-v4-gnss-bringup.md).

---

## 5) Hardware E2E RPC smoke

```bash
python3 scripts/e2e-test.py <endpoint-a> <endpoint-b>
```

Examples:

```bash
python3 scripts/e2e-test.py ws://<node-a>/ws ws://<node-b>/ws
python3 scripts/e2e-test.py serial:/dev/ttyUSB0 serial:/dev/ttyUSB1
```

Use this after RPC surface or transport-impacting changes.

---

## Decision guide

| Changed area | Minimum recommended verification |
|---|---|
| `components/**`, `main/**` | `bash test/run_all_tests.sh` |
| `webapp/**` | `cd webapp && npm test` |
| Routing/reliability/airtime | Host tests + targeted simulator scenario |
| RPC methods or wire format | Host tests + E2E RPC smoke |
| Pre-release sweep | Host tests + webapp tests + simulator + selected hardware E2E |

## Related docs

- [BUILDING.md](BUILDING.md)
- [testing/network-reach-e2e-checklist.md](testing/network-reach-e2e-checklist.md)
- [runbooks/ota-publish-endpoint-runbook.md](runbooks/ota-publish-endpoint-runbook.md)
