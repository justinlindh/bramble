# 2026-02-22 Channel E2E Evidence (.21/.64)

## Scope
Final evidence for `docs/plans/2026-02-22-channel-e2e-remaining-failures.md` Task 6/7 closure.

## Final E2E checklist results (web + CLI)

| Check | Result | Notes |
|---|---|---|
| Connect `.21` from web client | ✅ PASS | Connection established via host web client (`localhost:3004`) through ws-proxy |
| Create named+PSK channel | ✅ PASS | `e2e-final-chan` + `e2e-final-psk` created |
| Join same channel from `.64` | ✅ PASS | Joined via CLI and verified visible in web channel list |
| Metadata (name/default/lock) | ✅ PASS | Name and PSK state (`hasPsk`) verified via CLI JSON; channel selected in web UI |
| Bidirectional messaging | ✅ PASS | Sent both directions with channel-scoped CLI broadcast (`--channel`), messages visible on both nodes |
| Leave channel + fallback | ✅ PASS | Leave action returns to Broadcast context |

## Screenshots
Stored under:
`docs/plans/evidence/screenshots/2026-02-22-e2e-final/`

- `01-node21-channel-visible.png`
- `02-node21-channel-messages.png`
- `03-node64-channel-messages.png`
- `04-node21-leave-fallback.png`

## Regression/build validation

### Host firmware tests/build
- `cmake --build build -j` ✅
- Regression blocker fixed: `test_dummy_traffic` now links crypto host implementation and OpenSSL in `test/CMakeLists.txt`.

### Webapp
- `npm test` ✅
- `npm run build` ✅
- Docker webapp stack rebuilt/restarted during validation window.

## WebSocket stability investigation + resolution on `.21`
- Root cause chain identified during validation:
  1. Missing explicit WS control-frame handling on node side (ping/pong correctness gap).
  2. A temporary FD-reject strategy caused HTTPD select-loop errors and service instability.
- Final fix state in firmware (`main/ws_server.c`):
  - Explicit WS PING→PONG handling.
  - Safe client-table behavior restored.
  - `.21` WS/HTTP startup recovered and verified reachable.

## Final verdict
Task 6 and Task 7 are complete for this plan. Required channel E2E behavior (create/join/name/PSK/message routing/leave fallback) is verified with current firmware + web client path.
