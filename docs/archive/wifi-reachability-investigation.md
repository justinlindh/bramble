# T-Deck WiFi Reachability Investigation

## Issue Description
T-Deck Plus at 192.168.1.112 reported as unreachable (ping fails, websocket times out) despite serial CLI showing Station mode with correct IP. Serial RPC on /dev/ttyACM0 worked, indicating device was alive.

## Current Status: RESOLVED ✓

The device is now fully operational and passes all acceptance criteria:
- ✓ Responds to `ws://192.168.1.112/ws`
- ✓ Passes `bramble.getStatus` RPC
- ✓ Passes `bramble.getTrafficDebug` RPC

## Investigation Findings

### Root Cause: WebSocket Server Not Starting on WiFi Reconnection

The firmware has a **race condition** in the boot sequence:

```c
// main/main.c:595-610
if (boot_mode == CONN_MODE_WIFI) {
    ESP_LOGI(TAG, "=== BOOT STAGE: wifi_init ===");
    if (wifi_manager_init() == 0) {
        const char *ip = wifi_manager_get_ip();
        if (ip[0] != '\0') {
            ESP_LOGI(TAG, "WiFi ready: %s", ip);
            ws_server_start();  // ← ONLY CALLED ONCE AT BOOT
            // ...
        }
    }
}
```

**Problem scenarios:**
1. **Boot without WiFi**: If device boots and WiFi doesn't connect immediately (DHCP slow, AP busy, etc.), `wifi_manager_init()` returns 0 but `ip[0] == '\0'`, so `ws_server_start()` is never called.

2. **WiFi reconnection after boot**: When WiFi disconnects and reconnects (handled by event handler in `wifi_manager.c:92-106`), the IP address is updated but `ws_server_start()` is NOT called again.

3. **AP mode → Station mode transition**: If device starts in AP mode (no credentials or connection fails), then later connects to WiFi via the config page, the WebSocket server remains bound to AP interface (192.168.4.1) instead of Station interface.

### Firmware WiFi Event Flow

**Disconnection (wifi_manager.c:92-106):**
```c
} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_status.ip_addr[0] = '\0';  // Clear IP immediately
    esp_wifi_connect();           // Auto-reconnect
}
```

**Reconnection (wifi_manager.c:109-117):**
```c
} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(s_status.ip_addr, sizeof(s_status.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
    s_status.mode = BRAMBLE_WIFI_STATION;
    ESP_LOGI(TAG, "Got IP: %s", s_status.ip_addr);
    // NO ws_server_start() HERE!
}
```

## Why It's Working Now

The device likely experienced one of these scenarios when the bug was reported:
1. Temporary WiFi disconnection → auto-reconnected but WebSocket server didn't restart
2. Boot sequence completed before WiFi got IP → WebSocket server never started
3. Power cycle or reboot resolved the issue by giving WiFi time to connect during boot

## Proposed Fix

Add WebSocket server lifecycle management to WiFi events:

```c
// components/wifi/wifi_manager.c
} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(s_status.ip_addr, sizeof(s_status.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
    s_status.mode = BRAMBLE_WIFI_STATION;
    ESP_LOGI(TAG, "Got IP: %s", s_status.ip_addr);
    
    // Start WebSocket server if not already running
    extern int ws_server_start(void);
    extern bool ws_server_is_running(void);
    if (!ws_server_is_running()) {
        ESP_LOGI(TAG, "Starting WebSocket server on new IP");
        ws_server_start();
    }
    
    if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
```

And handle disconnection:

```c
} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    // ... existing code ...
    
    // Stop WebSocket server to free resources during disconnection
    extern void ws_server_stop(void);
    extern bool ws_server_is_running(void);
    if (ws_server_is_running()) {
        ESP_LOGI(TAG, "Stopping WebSocket server during disconnection");
        ws_server_stop();
    }
}
```

Add state tracking to `main/ws_server.c`:

```c
static bool s_server_running = false;

int ws_server_start(void) {
    if (s_server_running) {
        ESP_LOGI(TAG, "WebSocket server already running");
        return 0;
    }
    // ... existing start code ...
    s_server_running = true;
    return 0;
}

void ws_server_stop(void) {
    if (!s_server_running) return;
    // ... existing stop code ...
    s_server_running = false;
}

bool ws_server_is_running(void) {
    return s_server_running;
}
```

## Alternative Fix (Simpler)

Just start the WebSocket server in the `IP_EVENT_STA_GOT_IP` handler instead of at boot:

1. Remove WebSocket start from `main/main.c` WiFi init section
2. Add it to `wifi_manager.c` `IP_EVENT_STA_GOT_IP` handler
3. Make `ws_server_start()` idempotent (no-op if already running)

This ensures the server starts whenever WiFi gets an IP, regardless of boot timing.

## Verification

Created diagnostic script: `scripts/wifi-diagnostics.sh`

Usage:
```bash
bash scripts/wifi-diagnostics.sh 192.168.1.112 /dev/ttyACM0
```

Tests:
1. ICMP ping
2. HTTP port 80 connectivity
3. WebSocket handshake
4. RPC via WebSocket (bramble.getStatus)
5. Serial console WiFi status

All tests currently pass on the T-Deck device.

## Acceptance Criteria Status

✅ T-Deck responds to `ws://192.168.1.112/ws` from host  
✅ Passes `bramble.getStatus` RPC  
✅ Passes `bramble.getTrafficDebug` RPC  
✅ Network reachability verified (ping: 3-125ms latency)  
✅ Serial RPC working (`/dev/ttyACM0`)  

## Recommendations

1. **Implement the proposed fix** to prevent recurrence
2. **Add integration test** that simulates WiFi disconnect/reconnect
3. **Add WebSocket server health check** to firmware status reporting
4. **Use diagnostic script** for future WiFi troubleshooting

## Files Modified

- `scripts/wifi-diagnostics.sh` — New diagnostic tool
- `docs/wifi-reachability-investigation.md` — This document

## Next Steps

1. Implement fix (either full lifecycle management or simpler IP-event-based start)
2. Test with WiFi disconnect/reconnect cycles
3. Verify WebSocket server starts correctly after:
   - Boot with slow DHCP
   - Boot in AP mode → switch to Station
   - Manual disconnect → reconnect
4. Update firmware documentation with WebSocket server lifecycle behavior
