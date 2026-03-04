# Security Audit Fixes — Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Address all 7 findings from the March 2 security audit across firmware, SDK, and CLI.

**Architecture:** Firmware-first fixes (constant-time compare, BLE auth, NVS-stored token, query param removal), then SDK/CLI auth token support, then cleanup of dead code.

**Tech Stack:** C (ESP-IDF), Go (bramble-go, bramble-cli)

**Repos:**
- Firmware: `~/src/bramble` (C, ESP-IDF)
- Go SDK: `~/src/bramble-go`
- CLI: `~/src/bramble-cli`

---

## Finding Summary

| # | Severity | Finding | Fix |
|---|----------|---------|-----|
| 1 | HIGH | BLE GATT has no auth gate — full RPC access to anyone in range | Add BLE pairing requirement or RPC auth layer |
| 2 | HIGH | Auth token uses `strcmp()` — timing side-channel | Replace with constant-time compare |
| 3 | HIGH | Auth token baked into firmware at build time via Kconfig | Move to NVS (runtime-configurable, not in binary) |
| 4 | MEDIUM | SDK/CLI have no auth token support | Add `--token` flag and transport header injection |
| 5 | MEDIUM | NVS private key stored unencrypted | Enable NVS encryption (ESP-IDF feature) |
| 6 | MEDIUM | `key_backup.c` uses OpenSSL APIs that won't link on ESP; dead code | Fix conditional compilation, wire into BLE or remove |
| 7 | LOW | Auth token accepted via URL query parameter `?token=` | Remove query param path, keep only Bearer header |

---

## Task 1: Constant-time token comparison (Finding 2)

**Files:**
- Modify: `main/ws_server.c` (lines 72, 83 — both `strcmp` calls in `auth_check`)
- Create: `test/test_ws_auth.c`

**Step 1: Write a constant-time compare utility**

Add to `main/ws_server.c` (static function, no need for a separate header):

```c
/* Constant-time string comparison to prevent timing side-channels.
 * Returns 0 if strings match, non-zero otherwise. */
static int ct_strcmp(const char *a, const char *b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    /* Always compare len_a bytes to avoid length leak */
    volatile uint8_t result = (len_a != len_b) ? 1 : 0;
    size_t min_len = len_a < len_b ? len_a : len_b;
    for (size_t i = 0; i < min_len; i++) {
        result |= ((volatile uint8_t)a[i]) ^ ((volatile uint8_t)b[i]);
    }
    return result;
}
```

**Step 2: Replace both `strcmp` calls in `auth_check`**

Line 72: `strcmp(val, token) == 0` → `ct_strcmp(val, token) == 0`
Line 83: `strcmp(hdr + 7, token) == 0` → `ct_strcmp(hdr + 7, token) == 0`

**Step 3: Write test**

Create `test/test_ws_auth.c`:
- Test `ct_strcmp("abc", "abc")` returns 0
- Test `ct_strcmp("abc", "abd")` returns non-zero
- Test `ct_strcmp("abc", "ab")` returns non-zero (length mismatch)
- Test `ct_strcmp("", "")` returns 0

**Step 4: Run tests**

```bash
cd test && ./run_all_tests.sh test_ws_auth
```

**Step 5: Commit**

```bash
git add main/ws_server.c test/test_ws_auth.c
git commit -m "security: replace strcmp with constant-time compare for auth token"
```

---

## Task 2: Remove query parameter token path (Finding 7)

**Files:**
- Modify: `main/ws_server.c` — remove the query parameter check block in `auth_check`

**Step 1: Remove the query param block**

Delete lines 63-76 (the `httpd_req_get_url_query_len` block) from `auth_check`. Keep only the `Authorization: Bearer` header path.

**Step 2: Update test from Task 1**

Add test confirming token-in-query is rejected (if applicable at unit level).

**Step 3: Commit**

```bash
git add main/ws_server.c
git commit -m "security: remove auth token from URL query params (prevents log/referer leakage)"
```

---

## Task 3: Move auth token from Kconfig to NVS (Finding 3)

**Files:**
- Modify: `main/ws_server.c` — read token from NVS instead of `CONFIG_BRAMBLE_WS_AUTH_TOKEN`
- Modify: `main/Kconfig.projbuild` — remove `BRAMBLE_WS_AUTH_TOKEN` config entry
- Modify: `main/rpc_methods.c` — add RPC method `bramble.setAuthToken` for runtime config
- Create: `test/test_auth_token_nvs.c`

**Step 1: Add NVS read for auth token**

In `ws_server.c`, replace the compile-time `CONFIG_BRAMBLE_WS_AUTH_TOKEN` with an NVS read at server start:

```c
#define AUTH_TOKEN_MAX 128
static char s_auth_token[AUTH_TOKEN_MAX] = {0};

void ws_server_load_token(void)
{
    nvs_handle_t h;
    if (nvs_open("bramble", NVS_READONLY, &h) == ESP_OK) {
        size_t len = AUTH_TOKEN_MAX;
        if (nvs_get_str(h, "auth_token", s_auth_token, &len) != ESP_OK) {
            s_auth_token[0] = '\0'; /* no token = open access */
        }
        nvs_close(h);
    }
}
```

Call `ws_server_load_token()` from `ws_server_start()`.

**Step 2: Update `auth_check` to use `s_auth_token` instead of `CONFIG_BRAMBLE_WS_AUTH_TOKEN`**

Remove the `#ifdef CONFIG_BRAMBLE_WS_AUTH_TOKEN` guard. Always check `s_auth_token`. If empty, allow all connections (same behavior as before).

**Step 3: Add `bramble.setAuthToken` RPC method**

In `rpc_methods.c`:

```c
static int rpc_set_auth_token(const cJSON *params, cJSON *result)
{
    const cJSON *token = cJSON_GetObjectItem(params, "token");
    if (!token || !cJSON_IsString(token)) {
        return RPC_ERR_INVALID_PARAMS;
    }
    const char *val = token->valuestring;
    if (strlen(val) > 127) {
        return RPC_ERR_INVALID_PARAMS;
    }
    nvs_handle_t h;
    if (nvs_open("bramble", NVS_READWRITE, &h) != ESP_OK) {
        return RPC_ERR_INTERNAL;
    }
    if (val[0] == '\0') {
        nvs_erase_key(h, "auth_token");
    } else {
        nvs_set_str(h, "auth_token", val);
    }
    nvs_commit(h);
    nvs_close(h);
    ws_server_load_token(); /* reload immediately */
    cJSON_AddBoolToObject(result, "ok", true);
    return 0;
}
```

Register: `rpc_register("bramble.setAuthToken", rpc_set_auth_token);`

**Step 4: Remove Kconfig entry**

Delete the `BRAMBLE_WS_AUTH_TOKEN` block from `main/Kconfig.projbuild`.

**Step 5: Write test, run, commit**

```bash
git commit -m "security: move auth token from Kconfig to NVS (runtime-configurable)"
```

---

## Task 4: BLE auth gate (Finding 1)

**Files:**
- Modify: `components/ble/ble_server.c` — add auth token check before RPC dispatch

**Step 1: Add auth check to BLE RPC handler**

The BLE server already has `rpc_dispatch` called directly on received data. Add a pre-dispatch auth check:

Option A (simplest, recommended for pre-alpha): Require the first BLE message after connection to be an auth JSON-RPC call `bramble.auth` with the token. Until authenticated, reject all other RPC calls.

Option B: Use ESP-IDF BLE security (bonding/pairing). More complex, deferred.

**For now, implement Option A:**

In `ble_server.c`, add a per-connection auth state. If `s_auth_token` is set (non-empty), require `bramble.auth {"token": "..."}` as the first RPC call. All other calls return `{"error": "unauthorized"}` until auth succeeds.

**Step 2: Write test, run, commit**

```bash
git commit -m "security: add auth gate to BLE RPC (requires token if configured)"
```

---

## Task 5: SDK auth token support (Finding 4)

**Files:**
- Modify: `~/src/bramble-go/transport/websocket.go` — add token to connection headers
- Modify: `~/src/bramble-go/transport/ble.go` — add auth handshake
- Modify: `~/src/bramble-go/transport/serial.go` — add auth handshake
- Modify: `~/src/bramble-go/transport/transport.go` — add `AuthToken` to config
- Create: `~/src/bramble-go/transport/websocket_test.go` (auth header test)

**Step 1: Add AuthToken field**

In `transport.go` or a new `auth.go`, add token field to transport configs.

**Step 2: WebSocket — inject `Authorization: Bearer <token>` header on connect**

In `websocket.go`, add the header to the HTTP upgrade request:

```go
type WebSocket struct {
    url       string
    AuthToken string  // if set, sent as Authorization: Bearer header
    // ...
}
```

**Step 3: BLE/Serial — send `bramble.auth` RPC as first message after connect**

After transport connects, if `AuthToken` is set, send:
```json
{"jsonrpc":"2.0","method":"bramble.auth","params":{"token":"..."},"id":0}
```

**Step 4: Tests, commit**

```bash
cd ~/src/bramble-go && go test ./...
git commit -m "feat(transport): add auth token support to all transports"
```

---

## Task 6: CLI `--token` flag (Finding 4, continued)

**Files:**
- Modify: `~/src/bramble-cli/cmd/bramble/root.go` — add `--token` global flag
- Modify: transport initialization to pass token through

**Step 1: Add `--token` flag**

```go
rootCmd.PersistentFlags().StringVar(&authToken, "token", "", "Auth token for node connection")
```

**Step 2: Pass to transport**

When creating the transport in the connect flow, set `AuthToken` from the flag value.

**Step 3: Test, commit**

```bash
cd ~/src/bramble-cli && go build -o bramble ./cmd/bramble && ./bramble --help | grep token
git commit -m "feat(cli): add --token global flag for node authentication"
```

---

## Task 7: Fix key_backup conditional compilation (Finding 6)

**Files:**
- Modify: `components/ble/key_backup.c` — fix `#ifdef ESP_PLATFORM` to properly guard OpenSSL usage

**Step 1: Audit the file**

The file already has `#ifdef ESP_PLATFORM` / `#else` guards for includes. Check if the function bodies that use OpenSSL APIs (`EVP_*`) are also guarded.

**Step 2: Fix any unguarded OpenSSL calls**

Wrap `key_backup_export` and `key_backup_import` implementations in proper `#ifdef` guards. The ESP path should use `mbedtls` or the existing `crypto` component, the host path can use OpenSSL.

**Step 3: Either wire into BLE server or mark as experimental**

If wiring in is too much scope, add a `// TODO: wire into ble_server.c` comment and document in code that it's unconnected.

**Step 4: Run tests, commit**

```bash
cd test && ./run_all_tests.sh test_key_backup
git commit -m "fix: guard key_backup OpenSSL usage with platform ifdefs"
```

---

## Task 8: NVS encryption for identity keys (Finding 5)

**Deferred / low priority.** ESP-IDF supports NVS encryption via flash encryption, but enabling it requires:
1. `CONFIG_NVS_ENCRYPTION=y`
2. Flash encryption enabled (eFuse-based, one-way on production)
3. Partition table changes

This is a deployment-time security hardening step, not a code change. Document it as a security hardening recommendation in `docs/bramble-security-audit.md`.

**Step 1: Add documentation**

Update `docs/bramble-security-audit.md` with:
- Current state: private keys stored in plaintext NVS
- Mitigation: enable ESP-IDF flash encryption + NVS encryption for production deployments
- Risk assessment: requires physical flash access to exploit

**Step 2: Commit**

```bash
git commit -m "docs: document NVS encryption recommendation for identity keys"
```

---

## Execution Order

Tasks 1-2 are quick and independent. Task 3 is the most impactful firmware change. Task 4 depends on Task 3 (shared token). Tasks 5-6 are cross-repo and depend on 3-4 for the firmware-side auth protocol. Task 7 is independent. Task 8 is docs-only.

**Recommended dispatch order:**
1. Tasks 1+2 together (firmware, quick)
2. Task 3 (firmware, medium)
3. Task 4 (firmware, depends on 3)
4. Tasks 5+6 together (SDK + CLI, depends on 3+4)
5. Task 7 (firmware, independent)
6. Task 8 (docs, independent)

**Parallel groups:**
- Group A: Tasks 1+2 (firmware auth hardening)
- Group B: Task 7 (key_backup fix) — can run parallel with Group A
- Group C: Task 8 (docs) — can run parallel with anything
- Sequential: Task 3 → Task 4 → Tasks 5+6
