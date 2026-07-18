# Bramble Testing Reference

Quick guide for what to run based on what changed.

## 1) Firmware host tests (C / Unity)

Run from repo root:

```bash
bash test/run_all_tests.sh
```

Use this after firmware changes in `components/**`, `main/**`, or protocol logic.

The runner builds every suite registered in `test/CMakeLists.txt` and runs every produced `test_*` binary. It exits non-zero if the build fails, if any suite fails, or if zero suites are found, so a broken build can never report green. The suite count is intentionally not hardcoded anywhere; the set of `add_executable(test_...)` targets in `test/CMakeLists.txt` is the source of truth. The full run is a required CI gate (`.github/workflows/quality.yml`).

The same Quality workflow also enforces the RPC spec/firmware contract: `scripts/check-rpc-contract.sh` fails CI on any method-name drift between `api/openapi.yaml` and the registry in `main/rpc_methods.c`.

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

Use this for routing/reliability/airtime behavior validation. The simulator runs the real protocol code over a radio model with real time-on-air, collisions, capture, half-duplex, and listen-before-talk, which makes it the primary proving ground for scale and multi-hop behavior; `--no-collisions` gives an ideal-channel baseline. Current scale results live in [results/simulation-2026-07-honest-baseline.md](results/simulation-2026-07-honest-baseline.md) (which supersedes [results/simulation-2026-06.md](results/simulation-2026-06.md)).

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

For full flashing + board checks, see [BUILDING.md](BUILDING.md).

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
| RPC methods or wire format | Host tests + `bash scripts/check-rpc-contract.sh` + E2E RPC smoke |
| Pre-release sweep | Host tests + webapp tests + simulator + selected hardware E2E |

## Related docs

- [BUILDING.md](BUILDING.md)
