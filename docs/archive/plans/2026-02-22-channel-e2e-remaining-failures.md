# Bramble Channel E2E Remaining Failures Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Resolve the remaining failures from the corrected two-node web-client E2E retest: channel name/PSK metadata mismatch, cross-node delivery mismatch, missing Leave Channel action behavior, and lock-state visibility.

**Architecture:** Fix data correctness first (firmware RPC + runtime channel metadata), then enforce deterministic web UI behavior for create/join/leave paths. Add targeted tests at each layer and finish with scripted two-node E2E verification against real nodes (`192.168.1.21`, `192.168.1.64`).

**Tech Stack:** ESP-IDF C firmware (`main/*`), React/TypeScript webapp (`webapp/src/*`), Docker compose web deployment.

---

### Task 1: Reproduce and pin root causes with deterministic evidence

**Execution constraints for all subagents (mandatory):**
- Use `agent-browser` CLI for web E2E interactions (not ad-hoc Playwright scripts).
- If `tmp-e2e/repro-channel-failures.js` is missing, create it first, then execute it.
- Rebuild/redeploy web client Docker stack before E2E and prove served bundle hash changed.

**Files:**
- Create: `docs/archive/plans/evidence/2026-02-22-channel-e2e-failures.md`
- Modify: `webapp/src/api/client.ts` (only if temporary debug logging is needed)

**Step 1: Capture failing baseline flow (no code changes)**

Run:
```bash
node /home/justin/.openclaw/workspace/tmp-e2e/repro-channel-failures.js
```

Script must capture:
- create channel with explicit name + PSK on `192.168.1.21`
- connect `192.168.1.64`, join equivalent channel context
- send both directions
- attempt leave action

**Step 2: Save raw artifacts**
- Save screenshots + text dumps under `tmp-e2e/` with stable names.
- Summarize exact observed failures in `docs/archive/plans/evidence/2026-02-22-channel-e2e-failures.md`.

**Step 3: Commit evidence only**
```bash
git add docs/archive/plans/evidence/2026-02-22-channel-e2e-failures.md
git commit -m "test(e2e): capture reproducible channel failures on nodes .21/.64"
```

---

### Task 2: Fix channel create metadata application (name + PSK + lock state)

**Files:**
- Modify: `main/rpc_methods.c`
- Modify: `main/mesh_task.c`
- Modify: `main/mesh_task.h`
- Modify: `webapp/src/pages/Config/ChannelManager.tsx`
- Test: `test/*` (firmware test target covering channel metadata export)

**Step 1: Write failing firmware/API test**
Add test asserting `bramble.getConfig` channel entry preserves:
- `name` set by create action
- `hasPsk` true when PSK provided
- `is_default` only for configured default channel

**Step 2: Run to confirm fail**
```bash
cd /home/justin/src/bramble/test
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Step 3: Implement minimal fix**
- Ensure create/join path writes channel metadata to runtime state used by `getConfig`.
- Ensure `getConfig` reads from runtime canonical channel source (not stale NVS-only values).

**Step 4: Re-run tests**
Expected: new metadata tests pass.

**Step 5: Commit**
```bash
git add main/rpc_methods.c main/mesh_task.c main/mesh_task.h test
git commit -m "fix(channels): preserve name and PSK metadata in runtime/exported config"
```

---

### Task 3: Fix cross-node channel delivery mismatch

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `main/radio_task.c` (if channel lookup/pathing happens here)
- Modify: `webapp/src/pages/Chat/*` (only if wrong target/channel id is being sent)
- Test: `test/*` channel routing tests

**Step 1: Write failing test(s)**
Create/extend tests that verify:
- outbound message from channel `N` uses channel `N`
- inbound message tagged with channel `N` is rendered only in channel `N`
- no cross-channel leakage

**Step 2: Verify fail**
Run targeted routing tests.

**Step 3: Implement minimal fix**
- Normalize channel id mapping between web payload, RPC, mesh send, and stored message channel index.
- Ensure both nodes use identical channel-id semantics for send/receive.

**Step 4: Verify pass**
Run routing tests and smoke hardware send/receive between `.21` and `.64`.

**Step 5: Commit**
```bash
git add main/mesh_task.c main/radio_task.c webapp/src/pages/Chat test
git commit -m "fix(channels): enforce consistent channel-id routing across send/receive"
```

---

### Task 4: Restore/verify Leave Channel UX and behavior

**Files:**
- Modify: `webapp/src/pages/Chat/ChannelDetailPanel.tsx`
- Modify: `webapp/src/store/index.ts`
- Modify: `webapp/src/api/client.ts`
- Test: `webapp/src/pages/Chat/__tests__/ChannelDetailPanel.test.tsx`

**Step 1: Write failing UI test**
Add tests asserting:
- non-public channel shows **Leave Channel** action
- clicking leave calls remove-channel API and navigates to broadcast
- public/default channel does not show leave action

**Step 2: Verify fail**
```bash
cd /home/justin/src/bramble/webapp
npm test -- ChannelDetailPanel
```

**Step 3: Implement minimal fix**
- Render leave action deterministically for eligible channel rows.
- Wire to existing remove-channel state transition and fallback conversation.

**Step 4: Verify pass**
Run UI tests + build.

**Step 5: Commit**
```bash
git add webapp/src/pages/Chat/ChannelDetailPanel.tsx webapp/src/store/index.ts webapp/src/api/client.ts webapp/src/pages/Chat/__tests__/ChannelDetailPanel.test.tsx
git commit -m "fix(webapp): restore leave-channel action and fallback behavior"
```

---

### Task 5: Lock icon/PSK indicator consistency in all touched surfaces

**Files:**
- Modify: `webapp/src/components/Icons.tsx`
- Modify: `webapp/src/pages/Config/ChannelManager.tsx`
- Modify: `webapp/src/pages/Chat/ChannelDetailPanel.tsx`
- Modify: `webapp/src/pages/Chat/ConversationList.tsx` (if lock state shown there)

**Step 1: Add failing component tests/snapshots**
Verify lock icon appears when `hasPsk=true` and hides when false in all three surfaces.

**Step 2: Implement minimal consistency fix**
Use single icon component + shared condition helper.

**Step 3: Verify**
```bash
cd /home/justin/src/bramble/webapp
npm test
npm run build
```

**Step 4: Commit**
```bash
git add webapp/src/components/Icons.tsx webapp/src/pages/Config/ChannelManager.tsx webapp/src/pages/Chat/ChannelDetailPanel.tsx webapp/src/pages/Chat/ConversationList.tsx
git commit -m "fix(webapp): unify PSK lock indicator rendering across channel surfaces"
```

---

### Task 6: Full two-node E2E verification and deployment ✅ COMPLETED (2026-02-22)

**Files:**
- Modify: `docs/archive/plans/evidence/2026-02-22-channel-e2e-failures.md`
- Modify: `memory/bramble-status.md`

**Step 1: Build firmware + webapp**
```bash
cd /home/justin/src/bramble
/home/justin/.openclaw/workspace/scripts/bramble-build.sh tdeck

cd /home/justin/src/bramble/webapp
npm run build
docker compose -f docker-compose.yml build bramble-webapp
docker compose -f docker-compose.yml up -d bramble-webapp caddy
```

**Step 2: Execute required E2E checklist**
Against `https://192.168.6.34:3443`:
1. connect `.21`
2. create named+PSK channel
3. connect `.64` and join same channel
4. verify metadata (name/default/lock)
5. send both directions and verify visibility both sides
6. leave channel from detail panel and verify fallback

**Step 3: Record evidence**
- Save screenshots + concise pass/fail table in evidence file.

**Step 4: Update status memory**
- Add final notes + caveats in `memory/bramble-status.md`.

**Step 5: Commit**
```bash
git add docs/archive/plans/evidence/2026-02-22-channel-e2e-failures.md memory/bramble-status.md
git commit -m "docs(bramble): finalize channel E2E verification evidence on nodes .21/.64"
```

---

### Task 7: Final regression pass ✅ COMPLETED (2026-02-22)

**Files:**
- Modify: `docs/archive/plans/2026-02-22-channel-e2e-remaining-failures.md` (checklist completion)

**Step 1: Regression commands**
```bash
cd /home/justin/src/bramble/test
cmake --build build -j
ctest --test-dir build --output-on-failure

cd /home/justin/src/bramble/webapp
npm test
npm run build
```

**Step 2: Confirm no regressions**
- DM chat behavior unchanged
- channel list still renders correctly on T-Deck and web
- dispatch/connect flows unaffected

**Step 3: Commit final checklist**
```bash
git add docs/archive/plans/2026-02-22-channel-e2e-remaining-failures.md
git commit -m "chore(plan): mark remaining channel failure remediation complete"
```

---

Plan complete and saved to `docs/archive/plans/2026-02-22-channel-e2e-remaining-failures.md`.

## Final completion status (2026-02-22)
- Task 6 executed on nodes `.21` and `.64` with evidence captured in `docs/archive/plans/evidence/2026-02-22-channel-e2e-failures.md` and screenshots under `docs/archive/plans/evidence/screenshots/2026-02-22-e2e-final/`.
- Task 7 regression/build pass executed; host test build blocker (`test_dummy_traffic` crypto linkage) fixed in `test/CMakeLists.txt`.
- WebSocket stability root causes investigated and corrected in firmware (`main/ws_server.c`) with validated `.21` WS/HTTP startup recovery.
