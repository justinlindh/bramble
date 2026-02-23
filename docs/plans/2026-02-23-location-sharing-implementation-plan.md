# Privacy-First Location Sharing Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Implement opt-in, persistent, privacy-first location sharing using dedicated location packets, with T-Deck UI controls and web-client controls for all devices.

**Architecture:** Add a firmware location policy engine that sends `PKT_TYPE_LOCATION` on a periodic schedule when explicitly enabled. Use GPS as preferred source (manual fallback), persist all policy in NVS, and integrate receive/cache paths so `getPeerLocations` returns real peer data. Expose the same policy model in T-Deck UI and web client.

**Tech Stack:** ESP-IDF C firmware, Bramble RPC + packet pipeline, NVS, existing `components/location`, LVGL T-Deck UI, React/TypeScript webapp.

---

### Task 1: Define persistent location policy schema + defaults

**Files:**
- Modify: `main/rpc_methods.c`
- Modify: `components/location/include/location.h`
- Modify: `components/location/location.c`
- Test: `test/test_location.c`

**Step 1: Add failing tests for default policy behavior**
- Add tests for defaults: sharing disabled, default tier coarse, interval floor enforced.

**Step 2: Implement schema helpers**
- Add clear helpers for reading/writing policy fields in `bramble_loc` namespace.
- Keep backward compatibility with existing `lat_e6/lon_e6` keys.

**Step 3: Enforce defaults on fresh boot**
- If key missing: `enabled=0`, `def_tier="coarse"`, sane `interval_s`.

**Step 4: Verify tests pass**
- Run: `bash test/run_all_tests.sh`
- Expected: location tests pass with new defaults.

**Step 5: Commit**
```bash
git add main/rpc_methods.c components/location/include/location.h components/location/location.c test/test_location.c
git commit -m "feat(location): add persistent policy schema with privacy-first defaults"
```

---

### Task 2: Implement policy engine tick + send gating

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `components/location/location.c`
- Modify: `components/location/include/location.h`
- Test: `test/test_location.c` (new policy-decision tests)

**Step 1: Add failing tests for send decisions**
- Cases: disabled/no-source/no-target/interval-not-reached/allowed-send.

**Step 2: Add `location_policy_should_send(...)` style helper(s)**
- Centralize gate logic (enabled/source/interval/targets).

**Step 3: Add periodic tick in mesh task**
- Evaluate policy at interval cadence.
- Do not send when disabled or invalid source.

**Step 4: Verify**
- Run focused location tests + full host suite.

**Step 5: Commit**
```bash
git add main/mesh_task.c components/location/location.c components/location/include/location.h test/test_location.c
git commit -m "feat(location): add periodic policy engine with explicit send gating"
```

---

### Task 3: Add dedicated `PKT_TYPE_LOCATION` transmit path

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `components/packet/include/packet.h` (only if wire constants need extension/clarification)
- Modify: `components/location/location.c`
- Test: `test/test_packet.c`, `test/test_integration.c` (location packet cases)

**Step 1: Add failing tx tests**
- Verify location payload is encoded as `PKT_TYPE_LOCATION`, not chat JSON.

**Step 2: Implement tiered serialization dispatch**
- Use existing `location_serialize_full/coarse/...` based on policy tier.

**Step 3: Wire tx into sender path**
- Send per recipient scope (contacts and selected channels where applicable).

**Step 4: Verify tests**
- Run packet/integration tests.

**Step 5: Commit**
```bash
git add main/mesh_task.c components/location/location.c components/packet/include/packet.h test/test_packet.c test/test_integration.c
git commit -m "feat(location): send dedicated PKT_TYPE_LOCATION payloads"
```

---

### Task 4: Add dedicated location receive handling + cache updates

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `components/location/location.c`
- Modify: `main/rpc_methods.c`
- Test: `test/test_location.c`, `test/test_integration.c`

**Step 1: Add failing rx/cache tests**
- Inbound location packet should update cache and freshness.

**Step 2: Implement packet receive handling**
- Parse by tier, update cache with sender + timestamp.

**Step 3: Finish `getPeerLocations` integration**
- Return cached peer locations + own location where applicable.
- Remove/replace current TODO-only behavior.

**Step 4: Verify**
- Run location + integration tests.

**Step 5: Commit**
```bash
git add main/mesh_task.c components/location/location.c main/rpc_methods.c test/test_location.c test/test_integration.c
git commit -m "feat(location): integrate PKT_TYPE_LOCATION receive and peer cache RPC"
```

---

### Task 5: Upgrade RPC contract for policy controls (hybrid targets)

**Files:**
- Modify: `main/rpc_methods.c`
- Modify: `docs/api/rpc.md`
- Modify: `webapp/src/types/bramble.ts`
- Test: `test/test_rpc_methods.c`, `test/test_rpc_dispatcher.c`

**Step 1: Add failing RPC tests**
- Ensure config supports: enabled, tier, interval, source, contact rules, channel targets.

**Step 2: Implement RPC request/response shape**
- Extend `setLocationConfig` and related methods.
- Keep compatibility with existing fields where feasible.

**Step 3: Update docs/types**
- Reflect finalized payloads and field meanings.

**Step 4: Verify tests**
- Run rpc test binaries + full host suite.

**Step 5: Commit**
```bash
git add main/rpc_methods.c docs/api/rpc.md webapp/src/types/bramble.ts test/test_rpc_methods.c test/test_rpc_dispatcher.c
git commit -m "feat(rpc): add hybrid privacy-first location policy controls"
```

---

### Task 6: T-Deck UI controls for location sharing

**Files:**
- Modify: `components/ui_graphics/screens/scr_settings.c`
- Modify: `components/ui_graphics/include/*.h` (as needed)
- Modify: `main/cli.c` (only if existing settings plumbing requires)
- Test: `test/test_ui.c`

**Step 1: Add failing UI-state tests**
- Verify toggle/tier/interval/source/targets map to store actions.

**Step 2: Implement settings UI elements**
- Sharing toggle (default off), tier selector (default coarse), interval selector.
- Contact/channel target editors (minimal but usable).
- Panic-off action.

**Step 3: Add state indicators**
- last shared timestamp, active source.

**Step 4: Verify**
- Run UI tests + T-Deck build.

**Step 5: Commit**
```bash
git add components/ui_graphics/screens/scr_settings.c components/ui_graphics/include test/test_ui.c
git commit -m "feat(tdeck-ui): add opt-in location sharing controls"
```

---

### Task 7: Web client location control surface (for simple-screen nodes)

**Files:**
- Modify: `webapp/src/store/actions.ts`
- Modify: `webapp/src/types/bramble.ts`
- Modify: `webapp/src/pages/Map/Map.tsx`
- Modify/Create: settings panel components under `webapp/src/pages` or `webapp/src/components`
- Test: `webapp/test/*location*`, plus new tests

**Step 1: Add failing web tests**
- Config save flow, hybrid targets editing, opt-in defaults.

**Step 2: Implement controls**
- Toggle, tier, interval, source selector.
- Contacts/channels target editing.
- User-readable policy preview.

**Step 3: Wire RPC integration**
- `setLocationConfig`, contact/channel updates, refresh peer locations.

**Step 4: Verify**
- Run: `cd webapp && npm test`

**Step 5: Commit**
```bash
git add webapp/src webapp/test
git commit -m "feat(webapp): add hybrid location sharing controls and policy preview"
```

---

### Task 8: Make `shareLocationOnce` use dedicated location packet path

**Files:**
- Modify: `main/rpc_methods.c`
- Modify: `main/mesh_task.c` (or shared send helper)
- Test: `test/test_rpc_methods.c`, `test/test_packet.c`

**Step 1: Add failing tests**
- `shareLocationOnce` should emit `PKT_TYPE_LOCATION` and honor tier.

**Step 2: Replace legacy JSON message send**
- Route through location packet helper.

**Step 3: Verify**
- Run targeted tests and full host suite.

**Step 4: Commit**
```bash
git add main/rpc_methods.c main/mesh_task.c test/test_rpc_methods.c test/test_packet.c
git commit -m "refactor(location): route shareLocationOnce through location packet transport"
```

---

### Task 9: Verification matrix + hardware acceptance

**Files:**
- Create: `docs/plans/evidence/2026-02-23-location-sharing-evidence.md`
- Modify: `docs/testing/network-reach-e2e-checklist.md`

**Step 1: Build matrix**
```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" build
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build
bash test/run_all_tests.sh
cd webapp && npm test
```

**Step 2: Hardware matrix**
- T-Deck: GPS fix + periodic location sends when enabled.
- Heltec (non-GPS/manual): manual source works.
- Reboot persistence checks across both classes.

**Step 3: Privacy checks**
- Fresh device default sharing OFF.
- Coarse tier is default when first enabled.
- Disabled => no outbound location packets.

**Step 4: Commit**
```bash
git add docs/plans/evidence/2026-02-23-location-sharing-evidence.md docs/testing/network-reach-e2e-checklist.md
git commit -m "chore(verify): add location sharing verification evidence and checklist"
```

---

### Task 10: Bump semantic version for firmware/core release

**Files:**
- Modify: `main/version.h` or canonical firmware version source used by `bramble.getVersion`
- Modify: any mirrored version metadata used by build/release tooling (if present)
- Test: `main/rpc_methods.c` / runtime `bramble.getVersion` output validation

**Step 1: Determine current semantic version source of truth**
- Locate authoritative version field consumed in firmware build and RPC output.

**Step 2: Bump semver for this feature set**
- Increment version according to release policy (recommended: minor bump for new capabilities).

**Step 3: Verify version surfaces**
- Build firmware target(s).
- Confirm `bramble.getVersion` reports bumped value.

**Step 4: Commit**
```bash
git add <version-files>
git commit -m "chore(release): bump firmware semantic version for location sharing"
```

---

## Definition of Done

- Dedicated `PKT_TYPE_LOCATION` tx/rx path is production path.
- Hybrid recipient model works (contacts + channels).
- Sharing is opt-in and persisted across reboot.
- Default behavior remains privacy-first (OFF + coarse).
- T-Deck has practical on-device controls.
- Web client has complete controls for simple-screen nodes.
- `getPeerLocations` returns real cached peers (not placeholder TODO behavior).
- Build/test matrix passes; hardware validation captured.
