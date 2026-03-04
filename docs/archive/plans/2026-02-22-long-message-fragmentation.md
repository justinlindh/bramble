# Long Message Fragmentation Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Implement true multi-packet long-message support (broadcast + DM) with fragmentation/reassembly in core firmware and aligned RPC/web behavior.

**Architecture:** Reuse existing `components/fragment` primitives and integrate them into the active message send/receive path in `main/mesh_task.c`. Long messages are automatically split into fragments and reassembled before message delivery/notifications. RPC and web layers report clear limits and fragment metadata while preserving existing method names.

**Tech Stack:** ESP-IDF C firmware, Bramble RPC (`cJSON`), OpenAPI YAML, TypeScript webapp store/actions, existing shell build/flash scripts.

---

### Task 1: Define exact payload ceilings and remove misleading UX copy

**Files:**
- Modify: `webapp/src/pages/Chat/ComposeBar.tsx`
- Modify: `main/rpc_methods.c`

**Step 1: Add explicit firmware constants for single-packet and fragmented max**
- Add named constants near send handlers documenting overhead math and true max bytes.

**Step 2: Update ComposeBar helper text to accurate runtime behavior**
- Replace stale tooltip text with “auto-split + max total bytes” wording.

**Step 3: Build webapp to verify TS/UI integrity**
Run: `cd /home/justin/src/bramble/webapp && npm run -s build`
Expected: build succeeds.

**Step 4: Commit**
```bash
git add webapp/src/pages/Chat/ComposeBar.tsx main/rpc_methods.c
git commit -m "docs(ui): align long-message copy with fragmentation implementation limits"
```

### Task 2: Integrate fragmentation into firmware send path

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `main/mesh_task.h`
- Read/Use: `components/fragment/include/fragment.h`, `components/fragment/fragment.c`

**Step 1: Add failing test notes (or deterministic harness note if no unit harness)**
- Record expected behavior for >single-packet payload send path.

**Step 2: Implement `mesh_send_fragmented_*` path**
- Use `fragment_split(...)` when plaintext > single packet.
- Send each fragment as data packet with pacing and airtime debit.
- Return a logical message id for the whole message.

**Step 3: Keep short messages on current fast path**
- Preserve current behavior for <= single-packet payload.

**Step 4: Build firmware (tdeck + heltec)**
Run:
```bash
cd /home/justin/src/bramble
bash /home/justin/.openclaw/workspace/scripts/bramble-build.sh tdeck --no-flash
bash /home/justin/.openclaw/workspace/scripts/bramble-build.sh heltec --no-flash
```
Expected: both builds succeed.

**Step 5: Commit**
```bash
git add main/mesh_task.c main/mesh_task.h
git commit -m "feat(mesh): split long messages into multi-packet fragments on send"
```

### Task 3: Integrate reassembly into firmware receive path

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `main/mesh_task.h`
- Read/Use: `components/fragment/include/fragment.h`, `components/fragment/fragment.c`

**Step 1: Add reassembly context in mesh runtime state**
- Initialize context on startup.

**Step 2: Detect fragment payloads in `handle_data`**
- Route fragment packets through `reassembly_add(...)`.

**Step 3: Emit complete message only after `reassembly_collect(...)` success**
- Preserve message store + RPC notify behavior for final reconstructed text.

**Step 4: Add timeout purge in periodic maintenance**
- Call `reassembly_purge(...)` regularly.

**Step 5: Build firmware (tdeck + heltec)**
Run same build commands as Task 2.
Expected: both builds succeed.

**Step 6: Commit**
```bash
git add main/mesh_task.c main/mesh_task.h
git commit -m "feat(mesh): reassemble multi-packet message fragments on receive"
```

### Task 4: RPC + API + webapp behavior alignment

**Files:**
- Modify: `main/rpc_methods.c`
- Modify: `api/openapi.yaml`
- Modify: `webapp/src/types/bramble.ts`
- Modify: `webapp/src/store/actions.ts`

**Step 1: Update RPC send handlers**
- Remove hard 203-byte rejection.
- Enforce true fragmentation max.
- Return metadata: `fragmented`, `fragments_total`, `max_bytes`, `actual_bytes` when relevant.

**Step 2: Update OpenAPI schemas for send results/errors**
- Reflect new response fields and max behavior.

**Step 3: Update webapp send flow**
- Validate against true max bytes.
- Keep optimistic UX and clear error text.

**Step 4: Validate API + webapp**
Run:
```bash
cd /home/justin/src/bramble && npx @redocly/cli lint api/openapi.yaml
cd /home/justin/src/bramble/webapp && npm run -s test -- --run && npm run -s build
```
Expected: passes (acknowledging any known pre-existing lint warnings/errors only).

**Step 5: Commit**
```bash
git add main/rpc_methods.c api/openapi.yaml webapp/src/types/bramble.ts webapp/src/store/actions.ts
git commit -m "feat(rpc/web): expose long-message fragmentation metadata and limits"
```

### Task 5: End-to-end verification on real nodes

**Files:**
- Create: `reports/long-message-fragmentation-validation-2026-02-22.md`

**Step 1: Flash all active nodes with new firmware**
Run local + GPU box flash sequence.

**Step 2: Execute test matrix**
- Broadcast payload sizes: 180, 203, 204, 400, 700 bytes
- DM payload sizes: same matrix
- Observe message reconstruction correctness, duplicates, truncation, and latency.

**Step 3: Capture telemetry evidence**
- Use traffic capture to verify fragment TX/RX counts align with expected fragment totals.

**Step 4: UI validation**
- Verify long message displays as one logical message in webapp (not fragment spam).
- Capture screenshot.

**Step 5: Write report + commit**
```bash
git add reports/long-message-fragmentation-validation-2026-02-22.md
git commit -m "test(mesh): validate long-message fragmentation and reassembly on hardware"
```

## Parallel sub-agent ownership plan

- **Agent A (firmware send/reassembly):** Tasks 2 + 3 only. Owns `main/mesh_task.c` and `main/mesh_task.h`.
- **Agent B (rpc/api/web):** Task 4 only. Owns `main/rpc_methods.c`, `api/openapi.yaml`, `webapp/src/types/bramble.ts`, `webapp/src/store/actions.ts`.
- **Main session:** Task 1 copy alignment + Task 5 flashing/validation/report.

## Non-goals
- No feature flags.
- No protocol-side optional toggles.
- No unrelated refactors.
