# Bramble SDK & CLI Implementation Plan

> ✅ **ALL PHASES COMPLETE**

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Build a multi-repo ecosystem: firmware RPC layer, OpenAPI spec, Go SDK, and Go CLI tool for interacting with Bramble mesh nodes over serial and WebSocket.

**Architecture:** Firmware gets a transport-agnostic JSON-RPC 2.0 dispatcher (`components/rpc/`). An OpenAPI spec in `api/openapi.yaml` documents all methods. A Go SDK (`bramble-go`) provides a client library with pluggable transports. A CLI tool (`bramble-cli`) consumes the SDK.

**Tech Stack:** C (ESP-IDF, cJSON), Go 1.25, OpenAPI 3.1, oapi-codegen, cobra, go.bug.st/serial, nhooyr.io/websocket

**Repos:**
- `bramble` (existing) — firmware + API spec + simulator + webapp
- `bramble-go` (new) — Go SDK library
- `bramble-cli` (new) — CLI tool

**Gitea:** `ssh://git@192.168.1.199:2222/dumbot/{repo}.git`

---

## Phase 1: Firmware RPC Foundation

### Task 1.1: Create `components/rpc/` scaffolding

**Files:**
- Create: `components/rpc/CMakeLists.txt`
- Create: `components/rpc/Kconfig`
- Create: `components/rpc/include/rpc_dispatcher.h`
- Create: `components/rpc/rpc_dispatcher.c`

**Step 1: Create CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "rpc_dispatcher.c" "rpc_methods.c"
    INCLUDE_DIRS "include"
    REQUIRES json packet routing dedup identity channel crypto airtime freq_plan
)
```

**Step 2: Create Kconfig**

```kconfig
menu "Bramble RPC"

config BRAMBLE_RPC_ENABLED
    bool "Enable JSON-RPC dispatcher"
    default y

config BRAMBLE_RPC_MAX_REQUEST
    int "Max JSON-RPC request size (bytes)"
    default 1024

config BRAMBLE_RPC_MAX_RESPONSE
    int "Max JSON-RPC response size (bytes)"
    default 2048

config BRAMBLE_RPC_MAX_METHODS
    int "Max registered RPC methods"
    default 32

endmenu
```

**Step 3: Create rpc_dispatcher.h**

```c
#ifndef BRAMBLE_RPC_DISPATCHER_H
#define BRAMBLE_RPC_DISPATCHER_H

#include "cJSON.h"
#include <stddef.h>

/* Error codes (JSON-RPC 2.0 standard + Bramble custom) */
#define RPC_ERR_PARSE       -32700
#define RPC_ERR_INVALID_REQ -32600
#define RPC_ERR_NOT_FOUND   -32601
#define RPC_ERR_INVALID_PARAMS -32602
#define RPC_ERR_INTERNAL    -32603
#define RPC_ERR_RADIO       -1001
#define RPC_ERR_CHANNEL     -1002
#define RPC_ERR_RATE_LIMIT  -1003

/* Method handler signature */
typedef int (*rpc_method_handler_t)(const cJSON *params, cJSON *result);

/* Notification transport callback — called for each registered transport */
typedef void (*rpc_notify_cb_t)(const char *json, size_t len, void *ctx);

/**
 * Initialize the RPC dispatcher. Call once at startup.
 */
void rpc_init(void);

/**
 * Register a method handler.
 * @param method  Full method name (e.g. "bramble.getStatus")
 * @param handler Function to handle the method
 * @return 0 on success, -1 if table full
 */
int rpc_register(const char *method, rpc_method_handler_t handler);

/**
 * Dispatch a JSON-RPC request string. Parses, finds handler, calls it, formats response.
 * @param json_in   Incoming JSON string (null-terminated)
 * @param json_out  Output buffer for response JSON
 * @param out_len   Size of output buffer
 * @return Length of response written, or -1 on error
 */
int rpc_dispatch(const char *json_in, char *json_out, size_t out_len);

/**
 * Send a notification to all registered transports.
 * @param method  Notification method name (e.g. "bramble.onMessage")
 * @param params  cJSON object with notification params (ownership NOT transferred)
 */
void rpc_notify(const char *method, const cJSON *params);

/**
 * Register a transport for receiving notifications.
 * @param cb   Callback that receives serialized JSON notification
 * @param ctx  Opaque context pointer passed to callback
 * @return 0 on success, -1 if max transports reached
 */
int rpc_register_notify_transport(rpc_notify_cb_t cb, void *ctx);

#endif
```

**Step 4: Create rpc_dispatcher.c**

```c
#include "rpc_dispatcher.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "rpc";

#define MAX_METHODS     CONFIG_BRAMBLE_RPC_MAX_METHODS
#define MAX_TRANSPORTS  4

typedef struct {
    char method[64];
    rpc_method_handler_t handler;
} rpc_entry_t;

typedef struct {
    rpc_notify_cb_t cb;
    void *ctx;
} notify_transport_t;

static rpc_entry_t s_methods[MAX_METHODS];
static int s_method_count = 0;

static notify_transport_t s_transports[MAX_TRANSPORTS];
static int s_transport_count = 0;

void rpc_init(void) {
    s_method_count = 0;
    s_transport_count = 0;
    ESP_LOGI(TAG, "RPC dispatcher initialized");
}

int rpc_register(const char *method, rpc_method_handler_t handler) {
    if (s_method_count >= MAX_METHODS) {
        ESP_LOGE(TAG, "Method table full");
        return -1;
    }
    strncpy(s_methods[s_method_count].method, method, sizeof(s_methods[0].method) - 1);
    s_methods[s_method_count].handler = handler;
    s_method_count++;
    ESP_LOGD(TAG, "Registered: %s", method);
    return 0;
}

static rpc_method_handler_t find_handler(const char *method) {
    for (int i = 0; i < s_method_count; i++) {
        if (strcmp(s_methods[i].method, method) == 0) {
            return s_methods[i].handler;
        }
    }
    return NULL;
}

static int format_error(int id, int code, const char *message, char *out, size_t out_len) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id >= 0) {
        cJSON_AddNumberToObject(resp, "id", id);
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);

    int ret = -1;
    char *json = cJSON_PrintUnformatted(resp);
    if (json) {
        size_t len = strlen(json);
        if (len < out_len) {
            memcpy(out, json, len + 1);
            ret = (int)len;
        }
        cJSON_free(json);
    }
    cJSON_Delete(resp);
    return ret;
}

int rpc_dispatch(const char *json_in, char *json_out, size_t out_len) {
    cJSON *req = cJSON_Parse(json_in);
    if (!req) {
        return format_error(-1, RPC_ERR_PARSE, "Parse error", json_out, out_len);
    }

    /* Validate JSON-RPC 2.0 */
    cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
    cJSON *method_item = cJSON_GetObjectItem(req, "method");
    cJSON *id_item = cJSON_GetObjectItem(req, "id");
    cJSON *params_item = cJSON_GetObjectItem(req, "params");

    if (!jsonrpc || !cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        cJSON_Delete(req);
        return format_error(-1, RPC_ERR_INVALID_REQ, "Missing jsonrpc: 2.0", json_out, out_len);
    }

    if (!method_item || !cJSON_IsString(method_item)) {
        cJSON_Delete(req);
        return format_error(-1, RPC_ERR_INVALID_REQ, "Missing method", json_out, out_len);
    }

    int id = -1;
    if (id_item && cJSON_IsNumber(id_item)) {
        id = id_item->valueint;
    }

    const char *method = method_item->valuestring;
    rpc_method_handler_t handler = find_handler(method);
    if (!handler) {
        cJSON_Delete(req);
        return format_error(id, RPC_ERR_NOT_FOUND, "Method not found", json_out, out_len);
    }

    /* Create params object if not provided */
    cJSON *params = params_item ? params_item : cJSON_CreateObject();
    bool params_created = (params_item == NULL);

    /* Call handler */
    cJSON *result = cJSON_CreateObject();
    int handler_ret = handler(params, result);

    if (params_created) cJSON_Delete(params);

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id >= 0) {
        cJSON_AddNumberToObject(resp, "id", id);
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    if (handler_ret == 0) {
        cJSON_AddItemToObject(resp, "result", result);
    } else {
        cJSON_Delete(result);
        cJSON *err = cJSON_AddObjectToObject(resp, "error");
        cJSON_AddNumberToObject(err, "code", handler_ret);
        cJSON_AddStringToObject(err, "message", "Method error");
    }

    int ret = -1;
    char *json = cJSON_PrintUnformatted(resp);
    if (json) {
        size_t len = strlen(json);
        if (len < out_len) {
            memcpy(out, json, len + 1);
            ret = (int)len;
        }
        cJSON_free(json);
    }
    cJSON_Delete(resp);
    cJSON_Delete(req);
    return ret;
}

void rpc_notify(const char *method, const cJSON *params) {
    cJSON *notif = cJSON_CreateObject();
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", method);
    if (params) {
        cJSON_AddItemToObject(notif, "params", cJSON_Duplicate(params, 1));
    }

    char *json = cJSON_PrintUnformatted(notif);
    if (json) {
        size_t len = strlen(json);
        for (int i = 0; i < s_transport_count; i++) {
            s_transports[i].cb(json, len, s_transports[i].ctx);
        }
        cJSON_free(json);
    }
    cJSON_Delete(notif);
}

int rpc_register_notify_transport(rpc_notify_cb_t cb, void *ctx) {
    if (s_transport_count >= MAX_TRANSPORTS) return -1;
    s_transports[s_transport_count].cb = cb;
    s_transports[s_transport_count].ctx = ctx;
    s_transport_count++;
    return 0;
}
```

**Step 5: Commit**

```bash
git add components/rpc/
git commit -m "feat(rpc): add JSON-RPC 2.0 dispatcher component"
```

---

### Task 1.2: Implement RPC method handlers (query methods)

**Files:**
- Create: `components/rpc/include/rpc_methods.h`
- Create: `components/rpc/rpc_methods.c`

**Step 1: Create rpc_methods.h**

```c
#ifndef BRAMBLE_RPC_METHODS_H
#define BRAMBLE_RPC_METHODS_H

#include "identity.h"
#include "mesh_task.h"

/**
 * Register all bramble.* RPC methods.
 * Must be called after rpc_init() and mesh_task_start().
 */
void rpc_methods_init(bramble_identity_t *identity);

#endif
```

**Step 2: Create rpc_methods.c with query method handlers**

This file implements handlers for: `bramble.getStatus`, `bramble.getIdentity`, `bramble.getVersion`, `bramble.getNeighbors`, `bramble.getRoutes`, `bramble.getAirtime`, `bramble.ping`.

Each handler follows the pattern:
```c
static int handle_get_status(const cJSON *params, cJSON *result) {
    (void)params;
    mesh_shared_state_t state;
    mesh_get_state(&state);

    cJSON_AddStringToObject(result, "address",
                            addr_to_hex(s_identity->address));
    cJSON_AddStringToObject(result, "firmware_version", BRAMBLE_VERSION_STR);
    cJSON_AddStringToObject(result, "protocol_version", BRAMBLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "hardware", "heltec_v3");
    cJSON_AddBoolToObject(result, "radio_ok", state.radio_ok);
    cJSON_AddNumberToObject(result, "peers", neighbor_count(&state.neighbors));
    cJSON_AddNumberToObject(result, "beacon_tx", state.beacon_tx_count);
    cJSON_AddNumberToObject(result, "beacon_rx", state.beacon_rx_count);
    cJSON_AddNumberToObject(result, "packets_tx", state.packets_tx);
    cJSON_AddNumberToObject(result, "packets_rx", state.packets_rx);
    cJSON_AddNumberToObject(result, "uptime_s", uptime_seconds());
    return 0;
}
```

Implement all 7 query methods following this pattern. Each reads from `mesh_get_state()` or identity and populates the result cJSON object.

Define version constants:
```c
#define BRAMBLE_VERSION_STR      "0.1.0-dev"
#define BRAMBLE_PROTOCOL_VERSION "0.1.0"
```

Register all methods in `rpc_methods_init()`:
```c
void rpc_methods_init(bramble_identity_t *identity) {
    s_identity = identity;
    rpc_register("bramble.getStatus", handle_get_status);
    rpc_register("bramble.getIdentity", handle_get_identity);
    rpc_register("bramble.getVersion", handle_get_version);
    rpc_register("bramble.getNeighbors", handle_get_neighbors);
    rpc_register("bramble.getRoutes", handle_get_routes);
    rpc_register("bramble.getAirtime", handle_get_airtime);
    rpc_register("bramble.ping", handle_ping);
}
```

**Step 3: Commit**

```bash
git add components/rpc/
git commit -m "feat(rpc): implement query method handlers"
```

---

### Task 1.3: Wire RPC into CLI with UART auto-detection

**Files:**
- Modify: `main/cli.c`

**Step 1: Add JSON-RPC auto-detection to CLI task**

Modify the CLI task's line processing to detect JSON-RPC requests (lines starting with `{`) and route them to `rpc_dispatch()`, while keeping existing console commands for everything else.

```c
#include "rpc_dispatcher.h"
#include "rpc_methods.h"

// In cli_init(), after esp_console_init():
rpc_init();
rpc_methods_init(identity);

// Add UART notification transport:
static void uart_notify_cb(const char *json, size_t len, void *ctx) {
    (void)ctx;
    printf("%s\n", json);
    fflush(stdout);
}
rpc_register_notify_transport(uart_notify_cb, NULL);

// In the cli_task loop, replace the esp_console_run block:
if (strlen(line) > 0) {
    linenoiseHistoryAdd(line);

    if (line[0] == '{') {
        /* JSON-RPC mode */
        char response[CONFIG_BRAMBLE_RPC_MAX_RESPONSE];
        int ret = rpc_dispatch(line, response, sizeof(response));
        if (ret > 0) {
            printf("%s\n", response);
            fflush(stdout);
        }
    } else {
        /* Human console mode */
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unknown command. Type 'help'.\n");
        }
    }
}
```

**Step 2: Verify on hardware**

Flash to a board. Over serial, type:
```
{"jsonrpc":"2.0","id":1,"method":"bramble.getStatus"}
```
Should get a JSON response. Then type `peers` — should still work as before.

**Step 3: Commit**

```bash
git add main/cli.c
git commit -m "feat(cli): add JSON-RPC auto-detection on UART"
```

---

### Task 1.4: Unit tests for RPC dispatcher

**Files:**
- Create: `test/test_rpc_dispatcher.c`
- Modify: `test/CMakeLists.txt` (add test executable)

**Step 1: Write tests**

Test cases:
- Valid request dispatches to handler and returns result
- Unknown method returns error -32601
- Malformed JSON returns parse error -32700
- Missing `jsonrpc: "2.0"` returns invalid request error
- Missing method field returns invalid request error
- Request without params works (params default to empty object)
- Notification (no id) still dispatches
- Multiple methods can be registered and dispatched independently

**Step 2: Run tests**

```bash
cd test && mkdir -p build && cd build && cmake .. && make && ctest -V
```
Expected: All tests PASS

**Step 3: Commit**

```bash
git add test/
git commit -m "test(rpc): unit tests for JSON-RPC dispatcher"
```

---

## Phase 2: OpenAPI Spec + Versioning Docs

### Task 2.1: Write the OpenAPI 3.1 specification

**Files:**
- Create: `api/openapi.yaml`

**Step 1: Write the spec**

Create `api/openapi.yaml` with:

```yaml
openapi: 3.1.0
info:
  title: Bramble Mesh Node API
  description: |
    JSON-RPC 2.0 API for interacting with Bramble mesh nodes.
    All methods are invoked via JSON-RPC over serial (UART), WebSocket, or BLE.
    This OpenAPI spec documents the method schemas for codegen and documentation.
  version: 0.1.0
  license:
    name: TBD
  contact:
    name: Bramble Project
```

Document each method as a path (`/rpc/bramble.methodName`) with full request/response schemas. Include:
- All 9 query methods with response schemas
- All 15 action methods with request params + response schemas
- All 5 notification schemas under `x-notifications` extension
- Shared `components/schemas` for types: `Neighbor`, `Route`, `Channel`, `Message`, `AirtimeStats`, `LocationPeer`, `NodeIdentity`, `ProbeResult`

**Step 2: Validate**

```bash
npx @redocly/cli lint api/openapi.yaml
```
Expected: No errors.

**Step 3: Commit**

```bash
git add api/
git commit -m "docs(api): add OpenAPI 3.1 specification for Bramble JSON-RPC"
```

---

### Task 2.2: Write VERSIONING.md

**Files:**
- Create: `VERSIONING.md`

**Step 1: Write versioning documentation**

Cover:
- **Repository structure**: 3 repos, what lives where, how they relate
- **Protocol version lifecycle**: What triggers patch/minor/major bumps
- **Firmware versioning**: Independent from protocol version
- **SDK versioning**: Independent, documents supported protocol version range
- **CLI versioning**: Independent, documents bramble-go dependency version
- **Compatibility matrix**: Table showing which SDK versions work with which protocol versions
- **Version negotiation**: How SDK checks firmware compatibility on connect
- **Adding a new RPC method**: Checklist (update firmware → update openapi.yaml → bump protocol minor → update SDK → update CLI)
- **Breaking changes**: What constitutes a breaking change, migration guide template

**Step 2: Commit**

```bash
git add VERSIONING.md
git commit -m "docs: add VERSIONING.md — repo structure, semver rules, compatibility"
```

---

### Task 2.3: Set up Go type codegen from OpenAPI

**Files:**
- Create: `api/generate.sh`
- Create: `api/go/types.go` (generated)

**Step 1: Create generation script**

```bash
#!/bin/bash
# Generate Go types from Bramble OpenAPI spec
# Requires: go install github.com/oapi-codegen/oapi-codegen/v2/cmd/oapi-codegen@latest
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

oapi-codegen \
  -generate types \
  -package api \
  -o "$SCRIPT_DIR/go/types.go" \
  "$SCRIPT_DIR/openapi.yaml"

echo "Generated: api/go/types.go"
```

**Step 2: Run generation**

```bash
chmod +x api/generate.sh
./api/generate.sh
```

**Step 3: Verify generated types look correct**

**Step 4: Commit**

```bash
git add api/
git commit -m "build(api): add Go type codegen from OpenAPI spec"
```

---

## Phase 3: bramble-go SDK Core

### Task 3.1: Initialize bramble-go repository

**Step 1: Create repo on Gitea**

```bash
TOKEN=$(cat ~/.config/gitea/token)
curl -s -X POST "https://git.idiotica.org/api/v1/user/repos" \
  -H "Authorization: token $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"bramble-go","description":"Go SDK for Bramble mesh nodes","private":false}'
```

**Step 2: Initialize local repo**

```bash
mkdir -p ~/src/bramble-go
cd ~/src/bramble-go
git init
go mod init git.idiotica.org/dumbot/bramble-go
```

**Step 3: Create directory structure**

```
bramble-go/
├── transport/
│   └── transport.go
├── client.go
├── protocol.go
├── version.go
├── go.mod
└── README.md
```

**Step 4: Write README.md**

Cover: what it is, installation, quick start example, transport options, API reference link.

**Step 5: Commit and push**

```bash
git add .
git commit -m "feat: initialize bramble-go SDK"
git remote add origin ssh://git@192.168.1.199:2222/dumbot/bramble-go.git
git push -u origin master
```

---

### Task 3.2: Implement Transport interface

**Files:**
- Create: `transport/transport.go`

**Step 1: Define the Transport interface**

```go
package transport

import (
    "context"
    "io"
)

// Transport abstracts communication with a Bramble node.
type Transport interface {
    // Connect establishes the connection. Context controls timeout.
    Connect(ctx context.Context) error

    // Send writes a JSON-RPC message (newline-delimited JSON).
    Send(data []byte) error

    // Receive reads the next complete JSON-RPC message.
    // Blocks until a message is available or context is cancelled.
    Receive(ctx context.Context) ([]byte, error)

    // Close terminates the connection.
    Close() error

    // Info returns human-readable transport description (e.g., "/dev/ttyUSB0 @ 115200").
    Info() string
}
```

**Step 2: Commit**

```bash
git add transport/
git commit -m "feat(transport): define Transport interface"
```

---

### Task 3.3: Implement serial transport

**Files:**
- Create: `transport/serial.go`
- Modify: `go.mod` (add go.bug.st/serial)

**Step 1: Implement SerialTransport**

```go
package transport

// SerialTransport communicates with a Bramble node over UART.
type SerialTransport struct {
    port     string
    baudRate int
    // ...
}

type SerialOption func(*SerialTransport)

func WithBaudRate(baud int) SerialOption { ... }

func NewSerial(port string, opts ...SerialOption) *SerialTransport { ... }
```

Implement `Connect`, `Send`, `Receive`, `Close`, `Info`.

Key details:
- Default baud rate: 115200
- Newline-delimited JSON (each message is one line)
- `Receive` reads lines, skips non-JSON lines (human console output from the device)
- Skip lines not starting with `{` (those are ESP_LOG output or console prompts)

**Step 2: Commit**

```bash
git add transport/ go.mod go.sum
git commit -m "feat(transport): implement serial transport"
```

---

### Task 3.4: Implement WebSocket transport

**Files:**
- Create: `transport/websocket.go`
- Modify: `go.mod` (add nhooyr.io/websocket)

**Step 1: Implement WSTransport**

```go
package transport

type WSTransport struct {
    url string
    // ...
}

func NewWebSocket(url string) *WSTransport { ... }
```

Each WebSocket message frame = one JSON-RPC message. No newline delimiting needed.

**Step 2: Commit**

```bash
git add transport/ go.mod go.sum
git commit -m "feat(transport): implement WebSocket transport"
```

---

### Task 3.5: Implement BLE transport stub

**Files:**
- Create: `transport/ble.go`

**Step 1: Create stub**

```go
package transport

import (
    "context"
    "errors"
)

var ErrBLENotImplemented = errors.New("BLE transport not yet implemented")

type BLETransport struct{}

func NewBLE() *BLETransport { return &BLETransport{} }

func (b *BLETransport) Connect(ctx context.Context) error { return ErrBLENotImplemented }
func (b *BLETransport) Send(data []byte) error            { return ErrBLENotImplemented }
func (b *BLETransport) Receive(ctx context.Context) ([]byte, error) { return nil, ErrBLENotImplemented }
func (b *BLETransport) Close() error                      { return nil }
func (b *BLETransport) Info() string                      { return "BLE (not implemented)" }
```

**Step 2: Commit**

```bash
git add transport/
git commit -m "feat(transport): add BLE transport stub"
```

---

### Task 3.6: Implement JSON-RPC 2.0 protocol layer

**Files:**
- Create: `protocol.go`

**Step 1: Implement protocol layer**

```go
package bramble

// Protocol handles JSON-RPC 2.0 framing, request/response ID matching, and timeouts.
type Protocol struct {
    transport transport.Transport
    nextID    atomic.Int64
    pending   sync.Map  // id -> chan *Response
    notifyCh  chan Notification
}

type Request struct {
    JSONRPC string      `json:"jsonrpc"`
    ID      int64       `json:"id"`
    Method  string      `json:"method"`
    Params  interface{} `json:"params,omitempty"`
}

type Response struct {
    JSONRPC string          `json:"jsonrpc"`
    ID      *int64          `json:"id"`
    Result  json.RawMessage `json:"result,omitempty"`
    Error   *RPCError       `json:"error,omitempty"`
}

type RPCError struct {
    Code    int    `json:"code"`
    Message string `json:"message"`
}

type Notification struct {
    Method string          `json:"method"`
    Params json.RawMessage `json:"params"`
}
```

Implement:
- `Call(ctx, method, params) (json.RawMessage, error)` — send request, wait for matching response by ID
- Background goroutine reads from transport, routes responses to pending callers, routes notifications to channel
- Timeout handling via context

**Step 2: Commit**

```bash
git add protocol.go
git commit -m "feat: implement JSON-RPC 2.0 protocol layer"
```

---

### Task 3.7: Implement Client with query methods

**Files:**
- Create: `client.go`
- Create: `client_options.go`
- Create: `version.go`
- Create: `types.go` (copy generated types from api/go/types.go, or define manually for now)

**Step 1: Implement Client**

```go
package bramble

type Client struct {
    proto    *Protocol
    transport transport.Transport
}

type Option func(*Client)

func NewClient(t transport.Transport, opts ...Option) *Client { ... }

// Connect establishes transport and verifies protocol version.
func (c *Client) Connect(ctx context.Context) error { ... }

// Query methods
func (c *Client) Status(ctx context.Context) (*StatusResponse, error) { ... }
func (c *Client) Identity(ctx context.Context) (*IdentityResponse, error) { ... }
func (c *Client) Version(ctx context.Context) (*VersionResponse, error) { ... }
func (c *Client) Neighbors(ctx context.Context) ([]Neighbor, error) { ... }
func (c *Client) Routes(ctx context.Context) ([]Route, error) { ... }
func (c *Client) Airtime(ctx context.Context) (*AirtimeStats, error) { ... }
func (c *Client) Ping(ctx context.Context) error { ... }

// Notification subscription
func (c *Client) OnMessage(fn func(Message)) { ... }
func (c *Client) OnNeighborChange(fn func([]Neighbor)) { ... }
func (c *Client) Messages() <-chan Message { ... }

func (c *Client) Close() error { ... }
```

**Step 2: Implement version negotiation in Connect()**

```go
const (
    MinProtocolVersion = "0.1.0"
    MaxProtocolVersion = "0.1.0"
)

func (c *Client) Connect(ctx context.Context) error {
    if err := c.transport.Connect(ctx); err != nil {
        return err
    }
    // Start protocol reader
    c.proto.Start()

    // Check version
    ver, err := c.Version(ctx)
    if err != nil {
        return fmt.Errorf("version check failed: %w", err)
    }
    if !isCompatible(ver.ProtocolVersion, MinProtocolVersion, MaxProtocolVersion) {
        return fmt.Errorf("firmware protocol %s not supported (SDK supports %s-%s)",
            ver.ProtocolVersion, MinProtocolVersion, MaxProtocolVersion)
    }
    return nil
}
```

**Step 3: Commit**

```bash
git add client.go client_options.go version.go types.go
git commit -m "feat: implement Client with query methods and version negotiation"
```

---

### Task 3.8: Unit tests with mock transport

**Files:**
- Create: `transport/mock.go`
- Create: `client_test.go`
- Create: `protocol_test.go`

**Step 1: Create mock transport**

```go
package transport

type MockTransport struct {
    responses [][]byte
    sent      [][]byte
    // ...
}

func NewMock() *MockTransport { ... }
func (m *MockTransport) QueueResponse(json string) { ... }
func (m *MockTransport) SentMessages() []string { ... }
```

**Step 2: Write tests**

Test cases:
- Client.Connect succeeds with compatible protocol version
- Client.Connect fails with incompatible protocol version
- Client.Status returns parsed status
- Client.Neighbors returns parsed neighbor list
- Client.Ping succeeds
- Protocol correctly matches response IDs to callers
- Protocol routes notifications to subscribers
- Protocol handles timeout (context deadline)
- Each transport implements the interface (compile check)

**Step 3: Run tests**

```bash
go test ./... -v
```
Expected: All PASS

**Step 4: Commit**

```bash
git add transport/mock.go client_test.go protocol_test.go
git commit -m "test: unit tests for protocol layer and client"
```

---

## Phase 4: bramble-cli MVP

### Task 4.1: Initialize bramble-cli repository

**Step 1: Create repo on Gitea**

```bash
TOKEN=$(cat ~/.config/gitea/token)
curl -s -X POST "https://git.idiotica.org/api/v1/user/repos" \
  -H "Authorization: token $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"bramble-cli","description":"CLI tool for Bramble mesh nodes","private":false}'
```

**Step 2: Initialize**

```bash
mkdir -p ~/src/bramble-cli
cd ~/src/bramble-cli
git init
go mod init git.idiotica.org/dumbot/bramble-cli
```

**Step 3: Create directory structure**

```
bramble-cli/
├── cmd/
│   └── bramble/
│       └── main.go
├── internal/
│   ├── commands/
│   ├── output/
│   └── discovery/
├── go.mod
└── README.md
```

**Step 4: Add bramble-go dependency**

```bash
go get git.idiotica.org/dumbot/bramble-go@latest
```

**Step 5: Commit and push**

```bash
git add .
git commit -m "feat: initialize bramble-cli"
git remote add origin ssh://git@192.168.1.199:2222/dumbot/bramble-cli.git
git push -u origin master
```

---

### Task 4.2: Implement root command + transport flags

**Files:**
- Create: `cmd/bramble/main.go`
- Create: `internal/commands/root.go`

**Step 1: Implement root command**

```go
// root.go
var (
    flagPort      string
    flagTransport string
    flagJSON      bool
)

var rootCmd = &cobra.Command{
    Use:   "bramble",
    Short: "CLI for Bramble mesh nodes",
}

func init() {
    rootCmd.PersistentFlags().StringVarP(&flagPort, "port", "p", "", "Serial port (e.g. /dev/ttyUSB0)")
    rootCmd.PersistentFlags().StringVarP(&flagTransport, "transport", "t", "", "Transport URL (e.g. ws://192.168.4.1/ws)")
    rootCmd.PersistentFlags().BoolVar(&flagJSON, "json", false, "Output as JSON")
}

// getClient creates a connected bramble.Client from flags
func getClient(ctx context.Context) (*bramble.Client, error) { ... }
```

**Step 2: Commit**

```bash
git add cmd/ internal/
git commit -m "feat: root command with transport flags"
```

---

### Task 4.3: Implement USB auto-detection

**Files:**
- Create: `internal/discovery/discover.go`

**Step 1: Implement discovery**

Scan `/dev/ttyUSB*` (Linux) and `/dev/tty.usbserial*` (macOS). If exactly one found, use it. If multiple, list them and error with "use --port to specify". If none, error with helpful message.

**Step 2: Commit**

```bash
git add internal/discovery/
git commit -m "feat(discovery): auto-detect USB serial ports"
```

---

### Task 4.4: Implement output formatters

**Files:**
- Create: `internal/output/table.go`
- Create: `internal/output/json.go`
- Create: `internal/output/format.go`

**Step 1: Implement table formatter**

Simple aligned text columns. No external dependency — just `fmt.Fprintf` with column widths.

**Step 2: Implement JSON formatter**

`json.MarshalIndent` for human-readable, `json.Marshal` for piping.

**Step 3: `format.go` selects based on `--json` flag**

**Step 4: Commit**

```bash
git add internal/output/
git commit -m "feat(output): table and JSON formatters"
```

---

### Task 4.5: Implement status command

**Files:**
- Create: `internal/commands/status.go`

**Step 1: Implement**

```go
var statusCmd = &cobra.Command{
    Use:   "status",
    Short: "Show node status",
    RunE: func(cmd *cobra.Command, args []string) error {
        ctx := cmd.Context()
        client, err := getClient(ctx)
        if err != nil { return err }
        defer client.Close()

        status, err := client.Status(ctx)
        if err != nil { return err }

        if flagJSON {
            return output.JSON(os.Stdout, status)
        }
        // Table output
        fmt.Printf("Node:      %s\n", status.Address)
        fmt.Printf("Firmware:  %s\n", status.FirmwareVersion)
        fmt.Printf("Protocol:  %s\n", status.ProtocolVersion)
        // ...
        return nil
    },
}
```

**Step 2: Test with real hardware**

```bash
go run ./cmd/bramble status
go run ./cmd/bramble status --json
```

**Step 3: Commit**

```bash
git add internal/commands/
git commit -m "feat: add status command"
```

---

### Task 4.6: Implement peers command

**Files:**
- Create: `internal/commands/peers.go`

**Step 1: Implement**

Table output:
```
ADDRESS     RSSI  SNR  LAST SEEN
6EEA8967    -40   10   3s ago
1191C6E0    -44   10   12s ago
```

JSON output: raw array of Neighbor objects.

**Step 2: Commit**

```bash
git add internal/commands/
git commit -m "feat: add peers command"
```

---

### Task 4.7: Implement routes and ping commands

**Files:**
- Create: `internal/commands/routes.go`
- Create: `internal/commands/ping.go`

**Step 1: Implement routes**

Table output:
```
DEST        NEXT HOP    HOPS  EXPIRES
6EEA8967    6EEA8967    1     58s
```

**Step 2: Implement ping**

```bash
bramble ping
# Pong from 1191C6E0 (protocol: 0.1.0, firmware: 0.1.0-dev)
```

**Step 3: Commit**

```bash
git add internal/commands/
git commit -m "feat: add routes and ping commands"
```

---

### Task 4.8: Build, test on hardware, tag v0.1.0

**Step 1: Build binary**

```bash
go build -o bramble ./cmd/bramble
```

**Step 2: Test full workflow on hardware**

```bash
./bramble status
./bramble peers
./bramble routes
./bramble ping
./bramble status --json
./bramble --port /dev/ttyUSB1 peers
```

**Step 3: Tag releases**

```bash
# bramble-go
cd ~/src/bramble-go
git tag v0.1.0
git push origin v0.1.0

# bramble-cli
cd ~/src/bramble-cli
git tag v0.1.0
git push origin v0.1.0

# bramble firmware (in bramble repo)
cd ~/src/bramble
git tag v0.1.0-rpc
git push origin v0.1.0-rpc
```

**Step 4: Commit**

```bash
git commit --allow-empty -m "release: bramble-cli v0.1.0"
```

---

## Phase 5: Firmware Action Methods + Notifications

### Task 5.1: Implement action method handlers

**Files:**
- Modify: `components/rpc/rpc_methods.c`

Add handlers for:
- `bramble.sendMessage` — calls `mesh_send_message()` or `mesh_send_broadcast()`
- `bramble.sendProbe` — calls probe component
- `bramble.setRadio` — update radio config (requires mesh_task API extension)
- `bramble.setNodeName` — store in NVS
- `bramble.addChannel` / `removeChannel` / `setDefaultChannel` — manage channel list
- `bramble.setMailbox` — toggle mailbox flag
- `bramble.setLocationConfig` / `setLocationContact` / `removeLocationContact` / `shareLocationOnce` — location management
- `bramble.reboot` — `esp_restart()` after 500ms delay

Each handler validates params, calls the appropriate component API, and returns success/error.

Some of these require new `mesh_task` APIs:
- `mesh_set_radio_config(...)` — update radio settings and reinit
- `mesh_set_node_name(const char *name)` — store in NVS
- `mesh_get_node_name(char *buf, size_t len)` — read from NVS

**Step 1: Add mesh_task API extensions**

**Files:**
- Modify: `main/mesh_task.h` (add new function declarations)
- Modify: `main/mesh_task.c` (implement new functions)

**Step 2: Implement all action handlers in rpc_methods.c**

**Step 3: Register in rpc_methods_init()**

**Step 4: Commit**

```bash
git add components/rpc/ main/mesh_task.*
git commit -m "feat(rpc): implement action method handlers"
```

---

### Task 5.2: Implement notification emission

**Files:**
- Modify: `main/mesh_task.c` — emit notifications on events
- Modify: `components/rpc/rpc_dispatcher.c` — if needed

Add notification emission calls to mesh_task:

```c
// In handle_data(), after successful decrypt:
cJSON *params = cJSON_CreateObject();
cJSON_AddStringToObject(params, "from", addr_to_hex(info.src_addr));
cJSON_AddStringToObject(params, "text", text);
cJSON_AddNumberToObject(params, "rssi", rssi);
cJSON_AddNumberToObject(params, "snr", snr);
cJSON_AddNumberToObject(params, "channel_id", info.channel_id);
rpc_notify("bramble.onMessage", params);
cJSON_Delete(params);

// In handle_beacon(), after neighbor update:
rpc_notify("bramble.onNeighborChange", NULL);

// In handle_ack (when implemented):
rpc_notify("bramble.onAck", params);
```

**Step 1: Add notification calls to mesh_task.c**

**Step 2: Test over serial** — connect, watch for notifications when the other board sends beacons

**Step 3: Commit**

```bash
git add main/mesh_task.c
git commit -m "feat(rpc): emit notifications on message/neighbor/ack events"
```

---

### Task 5.3: Update OpenAPI spec with action methods + notifications

**Files:**
- Modify: `api/openapi.yaml`

Add all action method schemas and notification schemas. Bump spec version to include everything.

**Step 1: Update spec**

**Step 2: Regenerate Go types**

```bash
./api/generate.sh
```

**Step 3: Validate**

```bash
npx @redocly/cli lint api/openapi.yaml
```

**Step 4: Commit**

```bash
git add api/
git commit -m "docs(api): add action methods and notifications to OpenAPI spec"
```

---

### Task 5.4: Unit tests for action methods

**Files:**
- Modify: `test/test_rpc_dispatcher.c` (add test cases)

Test:
- sendMessage returns success
- sendMessage with missing params returns invalid params error
- ping returns pong
- reboot schedules restart
- Notification emission produces valid JSON-RPC notification format

**Step 1: Write tests**

**Step 2: Run and verify**

**Step 3: Commit**

```bash
git add test/
git commit -m "test(rpc): unit tests for action methods and notifications"
```

---

## Phase 6: SDK + CLI Action Methods

### Task 6.1: Add action methods to bramble-go

**Files:**
- Modify: `client.go`
- Create: `types.go` (or update with new types)

Add methods:
```go
func (c *Client) Send(ctx context.Context, dest string, text string) (*SendResult, error)
func (c *Client) Broadcast(ctx context.Context, text string) (*SendResult, error)
func (c *Client) SendProbe(ctx context.Context) error
func (c *Client) SetRadio(ctx context.Context, config RadioConfig) error
func (c *Client) SetNodeName(ctx context.Context, name string) error
func (c *Client) AddChannel(ctx context.Context, ch ChannelConfig) error
func (c *Client) RemoveChannel(ctx context.Context, id int) error
func (c *Client) SetDefaultChannel(ctx context.Context, id int) error
func (c *Client) SetMailbox(ctx context.Context, enabled bool) error
func (c *Client) SetLocationConfig(ctx context.Context, config LocationConfig) error
func (c *Client) SetLocationContact(ctx context.Context, addr string, tier string) error
func (c *Client) RemoveLocationContact(ctx context.Context, addr string) error
func (c *Client) ShareLocationOnce(ctx context.Context, addr string) error
func (c *Client) Reboot(ctx context.Context) error
```

**Step 1: Implement all methods**

**Step 2: Add notification subscription**

```go
func (c *Client) OnMessage(fn func(Message))
func (c *Client) OnAck(fn func(Ack))
func (c *Client) OnNeighborChange(fn func())
func (c *Client) OnRouteChange(fn func())
func (c *Client) OnProbeResult(fn func(ProbeResult))

// Channel-based alternative
func (c *Client) Messages() <-chan Message
```

**Step 3: Unit tests**

**Step 4: Commit and tag v0.2.0**

```bash
git add .
git commit -m "feat: add action methods and notification subscription"
git tag v0.2.0
git push origin master v0.2.0
```

---

### Task 6.2: Add CLI commands for actions

**Files:**
- Create: `internal/commands/send.go`
- Create: `internal/commands/broadcast.go` (or subcommand of send)
- Create: `internal/commands/config.go`
- Create: `internal/commands/channels.go`
- Create: `internal/commands/location.go`
- Create: `internal/commands/probe.go`
- Create: `internal/commands/monitor.go`
- Create: `internal/commands/reboot.go`

**Step 1: Implement send/broadcast**

```bash
bramble send 6EEA8967 "hello"
bramble broadcast "anyone out there?"
```

**Step 2: Implement config**

```bash
bramble config get
bramble config set-name "HomeBase"
bramble config set-radio --sf 10 --power 20
```

**Step 3: Implement channels**

```bash
bramble channels list
bramble channels add --name "family" --psk "..."
bramble channels remove --id 1
bramble channels set-default --id 0
```

**Step 4: Implement location**

```bash
bramble location status
bramble location set-contact 6EEA8967 --tier zone
bramble location remove-contact 6EEA8967
bramble location share-once 6EEA8967
```

**Step 5: Implement probe**

```bash
bramble probe
# Probing network...
# Reachable: 3/5 nodes
# 6EEA8967  1 hop   -40 dBm
# ...
```

**Step 6: Implement monitor**

```bash
bramble monitor
# Listening for events... (Ctrl+C to stop)
# [19:42:01] MESSAGE from 6EEA8967: hello
# [19:42:15] NEIGHBOR 6EEA8967 RSSI:-40 SNR:10
# [19:42:30] ACK for msg_id=12345 via 6EEA8967
```

With filters:
```bash
bramble monitor --messages
bramble monitor --neighbors
```

**Step 7: Implement reboot**

```bash
bramble reboot
# Node 1191C6E0 rebooting...
```

**Step 8: Commit and tag v0.2.0**

```bash
git add internal/commands/
git commit -m "feat: add send, config, channels, location, probe, monitor, reboot commands"
git tag v0.2.0
git push origin master v0.2.0
```

---

## Phase 7: WiFi WebSocket Transport

### Task 7.1: Firmware WiFi station mode ✅

**Files:**
- Create: `components/wifi/` (or `main/wifi_task.c`)
- Modify: `main/main.c`
- Modify: `main/Kconfig` (WiFi SSID/password config)

**Step 1: Implement WiFi station connection**

Use ESP-IDF `esp_wifi` to connect to a configured network. Fallback to AP mode if station fails.

Kconfig entries:
```
BRAMBLE_WIFI_SSID
BRAMBLE_WIFI_PASSWORD
BRAMBLE_WIFI_AP_SSID (default: "Bramble-XXXX" where XXXX is last 4 of address)
BRAMBLE_WIFI_AP_PASSWORD (default: "bramble123")
```

**Step 2: Commit**

```bash
git add components/wifi/ main/
git commit -m "feat(wifi): WiFi station mode with AP fallback"
```

---

### Task 7.2: Firmware WebSocket server ✅

**Files:**
- Create: `main/ws_server.c`
- Create: `main/ws_server.h`
- Modify: `main/main.c`

**Step 1: Implement WebSocket server using esp_http_server**

```c
// Start HTTP server with WebSocket upgrade on /ws
// Each WS frame = one JSON-RPC message
// Route incoming frames to rpc_dispatch()
// Register as notification transport to push to all WS clients
```

**Step 2: Register as notification transport**

```c
rpc_register_notify_transport(ws_notify_cb, NULL);
```

**Step 3: Commit**

```bash
git add main/ws_server.* main/main.c
git commit -m "feat: WebSocket server for JSON-RPC over WiFi"
```

---

### Task 7.3: SDK WebSocket transport + CLI flag ✅

**Files:**
- Modify: `bramble-go/transport/websocket.go` (if not already complete)
- Modify: `bramble-cli/internal/commands/root.go`

**Step 1: Verify WebSocket transport works with firmware**

**Step 2: Add `--transport` flag handling in CLI**

When `--transport ws://...` is provided, use WebSocket instead of serial.

**Step 3: Test over WiFi**

```bash
bramble --transport ws://192.168.4.1/ws status
bramble --transport ws://192.168.4.1/ws peers
bramble --transport ws://192.168.4.1/ws send 6EEA8967 "hello over wifi"
bramble --transport ws://192.168.4.1/ws monitor
```

**Step 4: Commit both repos**

```bash
# bramble-go
git commit -m "feat(transport): WebSocket transport verified with firmware"
git tag v0.3.0

# bramble-cli
git commit -m "feat: WiFi transport via --transport flag"
git tag v0.3.0
```

---

### Task 7.4: mDNS discovery ✅

**Files:**
- Firmware: `main/main.c` (add mDNS service advertisement)
- CLI: `internal/discovery/discover.go` (add mDNS scanning)

**Step 1: Firmware advertises `_bramble._tcp` via mDNS**

**Step 2: CLI `bramble discover` command scans for nodes**

```bash
bramble discover
# Found 2 Bramble nodes:
#   HomeBase (1191C6E0) at 192.168.1.42:80/ws
#   GarageNode (6EEA8967) at 192.168.1.43:80/ws
```

**Step 3: Commit both repos**

---

## Phase 8: Polish + Docs

### Task 8.1: Auto-reconnect in transports ✅

**Files:**
- Modify: `bramble-go/transport/serial.go`
- Modify: `bramble-go/transport/websocket.go`

Implement exponential backoff reconnect. On disconnect, attempt reconnect: 1s, 2s, 4s, 8s, max 30s. Client methods return error during reconnect. Optional `OnDisconnect` / `OnReconnect` callbacks.

**Step 1: Implement**

**Step 2: Test by unplugging/replugging USB**

**Step 3: Commit**

---

### Task 8.2: Shell completion ✅

**Files:**
- Modify: `bramble-cli/cmd/bramble/main.go`

Cobra has built-in completion generation:

```bash
bramble completion bash > /etc/bash_completion.d/bramble
bramble completion zsh > ~/.zsh/completions/_bramble
bramble completion fish > ~/.config/fish/completions/bramble.fish
```

**Step 1: Add completion command**

**Step 2: Commit**

---

### Task 8.3: README + examples for each repo ✅

**Files:**
- Modify: `bramble/README.md` — add API spec section, link to VERSIONING.md
- Modify: `bramble-go/README.md` — installation, quick start, full API
- Modify: `bramble-cli/README.md` — installation, all commands, examples
- Create: `bramble-go/examples/basic/main.go` — simple connect + status
- Create: `bramble-go/examples/monitor/main.go` — notification listener

**Step 1: Write all docs**

**Step 2: Commit each repo**

---

### Task 8.4: Final tags ⏳ (ready when you want to cut a release)

```bash
# bramble firmware
cd ~/src/bramble
git tag v0.2.0
git push origin v0.2.0

# bramble-go
cd ~/src/bramble-go
git tag v0.4.0
git push origin v0.4.0

# bramble-cli
cd ~/src/bramble-cli
git tag v0.4.0
git push origin v0.4.0
```

Update VERSIONING.md compatibility matrix with all versions.

---

## Summary

| Phase | Deliverable | Tasks |
|-------|-------------|-------|
| 1 | Firmware RPC dispatcher + query methods + UART auto-detect | 4 |
| 2 | OpenAPI spec + versioning docs + Go codegen | 3 |
| 3 | bramble-go SDK with serial + WS transports, query methods | 8 |
| 4 | bramble-cli MVP (status, peers, routes, ping) | 8 |
| 5 | Firmware action methods + notifications | 4 |
| 6 | SDK + CLI full method coverage | 2 |
| 7 | WiFi WebSocket transport end-to-end | 4 |
| 8 | Polish, auto-reconnect, docs | 4 |
| **Total** | | **37** |
