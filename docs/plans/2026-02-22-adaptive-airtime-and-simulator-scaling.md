# Adaptive Airtime + Simulator Scaling Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Add adaptive airtime control (starting with beacon policy) and simulator scenarios to validate behavior from small meshes to ~200 nodes without regressions.

**Architecture:** Introduce a runtime adaptive beacon controller in firmware that adjusts beacon interval by local mesh conditions (neighbor count, churn, and optional congestion signals). Expose controller config/status via RPC/OpenAPI, then mirror the policy in simulator scenarios and metrics pipelines for scale testing. Keep feature-flag defaults conservative so small meshes preserve current UX.

**Tech Stack:** ESP-IDF C firmware (`main/mesh_task.c`, `main/rpc_methods.c`), OpenAPI (`api/openapi.yaml`), simulator C/Go (`simulator/engine`, `simulator/gosim`), shell/python analysis scripts.

---

### Task 1: Baseline + policy shape (docs-first, no behavior change)

**Files:**
- Create: `docs/adaptive-airtime-policy.md`
- Modify: `docs/plans/2026-02-22-adaptive-airtime-and-simulator-scaling.md`

**Step 1: Write failing acceptance checks in plan notes**
- Define expected mode transitions:
  - `small/stable` mesh → 60s baseline interval
  - `dense` mesh → increased interval/backoff
  - `churn` detected → temporary fast beacon burst

**Step 2: Run validation command (docs lint/basic)**
Run: `test -s docs/adaptive-airtime-policy.md && echo OK`
Expected: `OK`

**Step 3: Write minimal policy spec**
- Include explicit thresholds, cooldown windows, and hysteresis.

**Step 4: Verify policy is unambiguous**
Run: `grep -n "threshold\|hysteresis\|cooldown\|mode" docs/adaptive-airtime-policy.md`
Expected: non-empty output

**Step 5: Commit**
```bash
git add docs/adaptive-airtime-policy.md docs/plans/2026-02-22-adaptive-airtime-and-simulator-scaling.md
git commit -m "docs: define adaptive airtime policy and acceptance criteria"
```

### Task 2: Firmware adaptive beacon controller (feature-flagged)

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `main/mesh_task.h`
- Modify: `main/rpc_methods.c`
- Test: `test/` (add focused unit/integration where available)

**Step 1: Write failing tests/check harness notes**
- Add test cases for interval selection by neighbor count/churn.
- If no unit harness exists for this module, add deterministic helper tests in `test/` nearest existing mesh tests.

**Step 2: Run tests to verify failure**
Run: `cd /home/justin/src/bramble && ctest --test-dir build || true`
Expected: new tests fail initially or TODO noted with explicit gap.

**Step 3: Implement minimal adaptive controller**
- Add mode enum + state machine in `mesh_task.c`.
- Inputs: neighbor count delta, recent peer join/leave events, optional retry/congestion counters.
- Output: `beacon_interval_ms` with jitter and hysteresis.
- Keep default behavior equivalent to current 60s in stable small meshes.

**Step 4: Add runtime config/status RPC**
- Add `bramble.setAirtimePolicy` and `bramble.getAirtimePolicy` (or equivalent naming consistent with existing RPC style).
- Persist config in NVS.

**Step 5: Build + flash check**
Run:
```bash
bash scripts/bramble-build.sh heltec
bash scripts/bramble-build.sh tdeck
```
Expected: build and flash complete.

**Step 6: Commit**
```bash
git add main/mesh_task.c main/mesh_task.h main/rpc_methods.c test/
git commit -m "feat(mesh): adaptive beacon interval controller with runtime policy config"
```

### Task 3: OpenAPI + web compatibility sync

**Files:**
- Modify: `api/openapi.yaml`
- Modify: `webapp/src/types/bramble.ts`
- Modify: `webapp/src/store/actions.ts`

**Step 1: Write failing schema checks**
- Add/expect schema entries for new airtime policy RPCs + payloads.

**Step 2: Run OpenAPI validation**
Run: `cd /home/justin/src/bramble && npx @redocly/cli lint api/openapi.yaml`
Expected: no errors.

**Step 3: Implement API schema updates**
- Add request/response objects, enum values, and notification links if any.

**Step 4: Verify webapp compiles with new types**
Run:
```bash
cd /home/justin/src/bramble/webapp
npm run -s test -- --run
npm run -s build
```
Expected: all tests pass and build succeeds.

**Step 5: Commit**
```bash
git add api/openapi.yaml webapp/src/types/bramble.ts webapp/src/store/actions.ts
git commit -m "docs(api): add adaptive airtime policy RPC schemas and web types"
```

### Task 4: Simulator policy modeling (10/50/100/200-node)

**Files:**
- Modify: `simulator/engine/sim_node.c`
- Modify: `simulator/engine/sim_metrics.c`
- Modify: `simulator/engine/sim_metrics.h`
- Modify: `simulator/gosim/sim.go`
- Create: `simulator/scenarios/airtime-adaptive-10.json`
- Create: `simulator/scenarios/airtime-adaptive-50.json`
- Create: `simulator/scenarios/airtime-adaptive-100.json`
- Create: `simulator/scenarios/airtime-adaptive-200.json`

**Step 1: Add failing scenario expectations**
- Define expected idle control airtime trend vs node count.
- Define pass/fail budget thresholds for each scenario.

**Step 2: Run baseline sim to capture pre-change metrics**
Run:
```bash
cd /home/justin/src/bramble/simulator
bash scripts/run-scenario.sh scenarios/ideal-10-node.json
```
Expected: baseline report produced.

**Step 3: Implement adaptive policy model hooks**
- Simulate beacon controller decisions per node.
- Emit per-category airtime metrics and collision/retry counts.

**Step 4: Run new scenarios and collect outputs**
Run each adaptive scenario and persist result artifacts under `reports/`.

**Step 5: Commit**
```bash
git add simulator/engine simulator/gosim simulator/scenarios
git commit -m "feat(sim): model adaptive airtime policy with 10-200 node scenarios"
```

### Task 5: Comparative analysis + rollout recommendation

**Files:**
- Create: `reports/adaptive-airtime-scaling-analysis-2026-02-22.md`
- Modify: `memory/bramble-status.md`

**Step 1: Produce before/after comparison table**
- Control airtime %, delivery latency, retries/collisions by node count.

**Step 2: Validate reproducibility commands**
- Include exact commands to regenerate all reports.

**Step 3: Recommend rollout profile**
- Default mode for small meshes
- Aggressive mode trigger for dense/churned meshes
- Guardrails for regressions

**Step 4: Commit**
```bash
git add reports/adaptive-airtime-scaling-analysis-2026-02-22.md memory/bramble-status.md
git commit -m "docs: adaptive airtime scaling analysis and rollout guidance"
```

## Parallel sub-agent dispatch map (non-overlapping ownership)

- **Agent A (firmware):** Task 2 only. Owns `main/mesh_task.*`, `main/rpc_methods.c`, related tests.
- **Agent B (api/web):** Task 3 only. Owns `api/openapi.yaml`, `webapp/src/types/bramble.ts`, `webapp/src/store/actions.ts`.
- **Agent C (simulator):** Task 4 only. Owns `simulator/engine/*`, `simulator/gosim/*`, `simulator/scenarios/*`.
- **Main session:** Task 1 + Task 5 integration, review, and merge conflict resolution.
