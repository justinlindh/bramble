# OTA Battery Fix Rollout Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Add a first-class OTA path in `bramble-cli` and use it to deploy the Heltec V4 battery ADC fix without USB flashing.

**Architecture:** Reuse existing firmware RPC `bramble.otaUpdate` (already implemented in firmware) and expose it in `bramble-go` + `bramble-cli` via a new `ota` command. OTA delivery uses HTTP(S) URL to `bramble.bin`; CLI handles RPC trigger and rollout verification (disconnect/reconnect + status checks).

**Tech Stack:** Go (cobra CLI + bramble-go client), ESP-IDF firmware artifact (`build/bramble.bin`), JSON-RPC over WebSocket.

---

### Task 1: Add `bramble-go` client support for OTA RPC

**Files:**
- Modify: `/home/justin/src/bramble-go/client.go`
- Modify: `/home/justin/src/bramble-go/types.go`
- Test: `/home/justin/src/bramble-go/client_test.go`

**Step 1: Write the failing test**

Add test for new client method:
```go
func TestClient_OTAUpdate(t *testing.T) {
  // mock transport returns {"ok":true,"note":"...","partition":"app0"}
  // assert method calls bramble.otaUpdate with URL param
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/justin/src/bramble-go && go test ./... -run OTAUpdate -v`
Expected: FAIL (method/type missing).

**Step 3: Write minimal implementation**

Add:
- request params struct (`URL string`)
- response struct (`OK bool`, `Note string`, `Partition string`)
- `func (c *Client) OTAUpdate(ctx context.Context, url string) (*OTAUpdateResponse, error)`

**Step 4: Run test to verify it passes**

Run: `cd /home/justin/src/bramble-go && go test ./... -run OTAUpdate -v`
Expected: PASS.

**Step 5: Commit**

```bash
cd /home/justin/src/bramble-go
git add client.go types.go client_test.go
git commit -m "feat: add bramble.otaUpdate client API"
```

---

### Task 2: Add CLI command `bramble ota --url <firmware-url>`

**Files:**
- Create: `/home/justin/src/bramble-cli/internal/commands/ota.go`
- Modify: `/home/justin/src/bramble-cli/internal/commands/root.go`
- Test: `/home/justin/src/bramble-cli/internal/commands/ota_test.go`

**Step 1: Write the failing test**

Add command parsing/validation tests:
```go
func TestOTACmd_RequiresURL(t *testing.T) {}
func TestOTACmd_AcceptsURLAndCallsClient(t *testing.T) {}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/justin/src/bramble-cli && go test ./... -run OTA -v`
Expected: FAIL (no command/no handler).

**Step 3: Write minimal implementation**

Implement command:
- `Use: "ota"`
- required `--url`
- call `client.OTAUpdate(ctx, url)`
- print note/partition (JSON and text modes)

**Step 4: Run test to verify it passes**

Run: `cd /home/justin/src/bramble-cli && go test ./... -run OTA -v`
Expected: PASS.

**Step 5: Commit**

```bash
cd /home/justin/src/bramble-cli
git add internal/commands/ota.go internal/commands/ota_test.go internal/commands/root.go
git commit -m "feat(cli): add ota command for URL-based firmware update"
```

---

### Task 3: Add OTA operator docs (single-node first)

**Files:**
- Modify: `/home/justin/src/bramble/README.md`
- Create: `/home/justin/src/bramble/docs/ota-rollout.md`

**Step 1: Write failing doc check (manual gate)**

Create checklist section that currently does not exist:
- Build artifact
- Host artifact
- Trigger OTA
- Verify reconnect/version
- Rollback guidance

**Step 2: Validate missing content**

Run: `rg -n "bramble ota|ota-rollout" /home/justin/src/bramble/README.md /home/justin/src/bramble/docs`
Expected: no/insufficient matches.

**Step 3: Add minimal docs**

Include exact commands:
```bash
cd ~/src/bramble
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build

cd build
python3 -m http.server 8088

cd ~/src/bramble-cli
./bramble --transport ws://192.168.1.179/ws ota --url http://192.168.6.34:8088/bramble.bin
```

**Step 4: Verify docs discoverable**

Run: `rg -n "ota --url|ota-rollout" /home/justin/src/bramble/README.md /home/justin/src/bramble/docs/ota-rollout.md`
Expected: PASS (matches found).

**Step 5: Commit**

```bash
cd /home/justin/src/bramble
git add README.md docs/ota-rollout.md
git commit -m "docs: add OTA rollout guide for WiFi nodes"
```

---

### Task 4: Roll out Heltec V4 battery fix over OTA (no USB)

**Files:**
- Use existing fix in: `/home/justin/src/bramble/components/battery/battery.c`
- Validation notes: `/home/justin/src/bramble/docs/plans/evidence/2026-02-23-heltec-v4-ota-battery-fix-evidence.md`

**Step 1: Write failing validation condition**

Define precondition failure target:
- UI battery shows `0%` on Heltec V4 with battery attached.

**Step 2: Capture baseline**

Run:
```bash
cd /home/justin/src/bramble-cli
./bramble --transport ws://192.168.1.179/ws status --json
```
And capture UI screenshot showing bad battery value.

**Step 3: Build + host firmware**

Run:
```bash
cd /home/justin/src/bramble
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build
cd build
python3 -m http.server 8088
```

**Step 4: Trigger OTA via CLI**

Run:
```bash
cd /home/justin/src/bramble-cli
./bramble --transport ws://192.168.1.179/ws ota --url http://192.168.6.34:8088/bramble.bin
```
Expected: RPC ack (`ok=true`, partition note), device reboots.

**Step 5: Verify post-OTA behavior**

Run:
```bash
./bramble --transport ws://192.168.1.179/ws ping
./bramble --transport ws://192.168.1.179/ws status --json
./bramble --transport ws://192.168.1.179/ws monitor --topic gps --follow --since 2m
```
Expected:
- reconnect works,
- radio/gps healthy,
- UI battery no longer pinned at 0%.

**Step 6: Commit evidence**

```bash
cd /home/justin/src/bramble
git add docs/plans/evidence/2026-02-23-heltec-v4-ota-battery-fix-evidence.md
git commit -m "chore: capture OTA rollout evidence for heltec-v4 battery fix"
```

---

### Task 5: Regression checks + release hygiene

**Files:**
- Modify: `/home/justin/src/bramble-cli/README.md`
- Modify: `/home/justin/.openclaw/workspace/docs/tools/bramble.md`
- Modify: `/home/justin/.openclaw/workspace/memory/bramble-hardware.md`

**Step 1: Write failing check list**

Define release gates:
- `bramble-cli` tests pass
- `bramble-go` tests pass
- OTA command shown in `--help`
- operator docs updated

**Step 2: Run gates before final claim**

```bash
cd /home/justin/src/bramble-go && go test ./...
cd /home/justin/src/bramble-cli && go test ./... && ./bramble --help | rg -n "ota"
```
Expected: PASS.

**Step 3: Update operator references**

Document that OTA is preferred for WiFi-connected nodes and USB is fallback.

**Step 4: Verify docs updated**

```bash
rg -n "ota|OTA" /home/justin/src/bramble-cli/README.md /home/justin/.openclaw/workspace/docs/tools/bramble.md /home/justin/.openclaw/workspace/memory/bramble-hardware.md
```

**Step 5: Commit**

```bash
cd /home/justin/src/bramble-cli
git add README.md
git commit -m "docs(cli): document ota command"

cd /home/justin/.openclaw/workspace
git add docs/tools/bramble.md memory/bramble-hardware.md
git commit -m "docs: record OTA-first bramble workflow"
```
