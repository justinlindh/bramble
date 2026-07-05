# Task 3 report: provisioning UX

Status: DONE. Branch feat/mandatory-provisioning, 2 commits on top of b158e643.

## Commits
- 87b917d1 feat(firmware): bramble.generateNetworkKey mints + provisions a founder key
- 32849379 feat(webapp): surface UNPROVISIONED prominently; found/join key provisioning

## Firmware: generateNetworkKey RPC + openapi
- `handle_generate_network_key` in main/rpc_methods.c: calls
  network_key_generate_provision (Task 1), which mints an entropy-gated 32-byte
  key, provisions THIS node atomically (memory + NVS), then re-derives the
  beacon subkey live (mirroring setNetworkKey). Returns
  `{"key":"<64 hex>","fingerprint":"<8 hex>"}`. The raw key crosses only the
  operator's local channel (same trust boundary as setNetworkKey, which already
  accepts a raw key); never broadcast, never logged, local stack copy wiped.
  On entropy failure returns RPC_ERR_INTERNAL and provisions nothing (fail
  closed). Registered as `bramble.generateNetworkKey`.
- api/openapi.yaml: additive `/rpc/bramble.generateNetworkKey` path (EmptyParams
  request) + `GenerateNetworkKeyResponse` schema (key + fingerprint, both hex
  patterns). Also corrected stale getNetworkKeyStatus prose (no public-PSK
  fallback anymore: unprovisioned reports all-zero fingerprint and is inert).
- getStatus contract unchanged; the webapp already polls getNetworkKeyStatus for
  {provisioned, fingerprint}, so no getStatus change was needed.
- check-rpc-contract: OK, 54 methods match.

## Webapp: how UNPROVISIONED is surfaced
- New global store field `networkKeyStatus` (+ setter, reset on disconnect),
  loader `loadNetworkKeyStatus`, polled from the app shell (App.tsx) every 10s
  alongside neighbors.
- New `components/UnprovisionedBanner`: a red, role="alert" bar rendered
  directly below the topbar in App.tsx (above the tab body, so visible on every
  tab). Shows only when connected and provisioned === false; text explains the
  node has no key and is not meshing; a "Provision" button jumps to Config. It
  disappears the instant a key is set (next poll / refreshStatus).

## Generate + paste flows (NetworkKeySection)
- Found a new network: the Generate button now calls the firmware
  generateNetworkKey action (was client-side crypto). The device mints +
  provisions itself as founder; the returned key is shown (readonly hex + Copy +
  QR share via existing QRShareModal/networkKeyShare) with its fingerprint to
  copy to the other nodes.
- Join an existing network: unchanged paste/scan path (setNetworkKey via
  QRScanModal + parseNetworkKeyShare).
- Section reads the global networkKeyStatus so it and the banner never disagree.

## Fingerprint display
- Provisioned status line shows the fingerprint prominently ("Every node in this
  network should show this same fingerprint") for fleet-convergence eyeballing;
  the generated-key block shows the new key's fingerprint to verify against each
  node after provisioning.

## Re-key warning
- Clicking Generate on an already-provisioned node opens a warning box (does NOT
  generate): it states the node is already provisioned (with fingerprint) and
  that re-keying cuts it off from nodes on the old key, requiring an explicit
  "Re-key this node" confirmation (or Cancel). The button label also changes to
  "Generate new key (re-key)" when provisioned.

## Webapp tests
- store/__tests__/networkKey.test.ts: added generateNetworkKey (mints, returns
  key+fingerprint) and loadNetworkKeyStatus (pushes status into store) cases;
  extended the not-connected guard to generateNetworkKey.
- components/UnprovisionedBanner.test.tsx (new): renders warning when connected +
  unprovisioned; Provision button switches activeTab to config; renders nothing
  when provisioned; renders nothing while disconnected.

## CI (all green locally)
- Host tests (test/run_all_tests.sh): 101 suites passed.
- Board build (make ci-quality-board-build, heltec-v3): exit 0.
- clang-format v14 (docker runner, --strict): PASS, 379 files.
- cppcheck (docker runner): exit 0.
- check-rpc-contract: OK.
- gosim (go test -count=1 ./...): ok.
- Webapp: typecheck clean, build clean, test suite 273 passed (incl. new tests).
- No em dashes, ASCII only, in code and commit messages.

## Concerns
- Behavior change: Generate now provisions THIS node immediately (founder) via
  the firmware RPC, instead of the old client-side generate-then-paste. This is
  what the brief asked for (on-device entropy gating + atomic persist) and is
  why the re-key confirmation exists. The unused client-side helpers
  (generateNetworkKeyHex, networkKeyFingerprint) remain in utils/networkKeyShare
  with their existing tests; left in place rather than removed to keep the diff
  focused.
