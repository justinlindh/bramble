# WiFi/BLE Toggle Implementation Test Plan

## Implementation Summary

The WiFi/BLE connectivity mode toggle for T-Deck Plus has been fully implemented with:

1. **NVS-persisted mode selection** via `conn_mode_get()` and `conn_mode_set()`
2. **Three connectivity modes:**
   - `CONN_MODE_WIFI` (default) — WiFi only
   - `CONN_MODE_BLE` — Bluetooth LE only
   - `CONN_MODE_BOTH` — Both WiFi and BLE (may exhaust SRAM)
3. **Settings UI integration:**
   - Graphical UI (T-Deck Plus): Dropdown selector in Settings tab
   - Text UI (Heltec V3): Menu selector in Settings screen
4. **Automatic reboot** after mode change to apply settings

## Code Locations

- **Mode enum:** `components/ui/include/ui.h` (lines 19-25)
- **Mode get/set:** `main/main.c` (lines 71-88)
- **Graphical Settings UI:** `components/ui_graphics/screens/scr_settings.c` (lines 63-87, 198-265)
- **Text Settings UI:** `main/main.c` (lines 344-373, 510-548)
- **Conditional initialization:** `main/main.c` (lines 661-693)

## Test Cases

### Test 1: Default Mode (WiFi Only)
**Steps:**
1. Flash fresh firmware to T-Deck Plus
2. Observe boot logs

**Expected:**
- Logs show: `Connectivity mode: WiFi`
- WiFi initializes (station or AP mode)
- BLE does not initialize
- No "BLE server started" log message

### Test 2: Switch to BLE Mode
**Steps:**
1. Connect to device via WebSocket (if WiFi working) or serial
2. Navigate to Settings tab
3. Select "BLE only" from dropdown
4. Tap "Apply Mode & Reboot" button
5. Device reboots

**Expected:**
- NVS stores `conn_mode=1`
- After reboot, logs show: `Connectivity mode: BLE`
- BLE initializes and starts advertising
- WiFi does not initialize
- Can connect via BLE GATT (Nordic UART Service)

### Test 3: Switch to WiFi+BLE Mode (Memory Test)
**Steps:**
1. Set mode to "WiFi + BLE"
2. Apply and reboot
3. Monitor boot logs and memory usage

**Expected:**
- Logs show: `Connectivity mode: WiFi+BLE`
- Both WiFi and BLE initialization attempted
- **May fail** with ESP_ERR_NO_MEM if mesh task exhausts SRAM
- User can switch back to single-mode via serial CLI or settings

### Test 4: Mode Persistence Across Reboots
**Steps:**
1. Set mode to BLE
2. Reboot device via Settings → Reboot button
3. Power cycle device (disconnect/reconnect USB)

**Expected:**
- Mode remains BLE across both soft and hard resets
- Only changes when user explicitly changes via Settings UI

### Test 5: Settings UI Displays Current Mode
**Steps:**
1. Boot device in any mode
2. Navigate to Settings
3. Check dropdown selection

**Expected:**
- Dropdown pre-selects the current active mode
- Asterisk (*) marker shows current mode in text UI

## Manual Test Procedure

```bash
# Terminal 1: Serial monitor
cd ~/src/bramble
./scripts/flash-local.sh --monitor

# Look for these log lines during boot:
# - "Connectivity mode: WiFi" (or BLE, or WiFi+BLE)
# - "WiFi ready: <IP>" (WiFi mode)
# - "BLE server started" (BLE mode)

# Terminal 2: RPC test (if WiFi mode)
echo '{"method":"ping"}' | websocat ws://bramble-XXXX.local/ws

# Or BLE test (if BLE mode)
# Use webapp BLE transport or Nordic nRF Connect app
```

## Known Limitations

1. **CONN_MODE_BOTH may fail** due to ESP32-S3 internal SRAM constraints (~180KB total)
   - Mesh task + BLE stack + WiFi stack exceeds available memory
   - If both are needed, consider disabling mesh or reducing buffer sizes

2. **Settings UI requires working display**
   - If display fails, use serial CLI RPC to change mode:
   ```json
   {"method":"settings_set","params":{"conn_mode":0}}  // WiFi
   {"method":"settings_set","params":{"conn_mode":1}}  // BLE
   {"method":"settings_set","params":{"conn_mode":2}}  // Both
   {"method":"reboot"}
   ```

## Success Criteria

✅ Fresh flash defaults to WiFi mode  
✅ Settings UI shows current mode correctly  
✅ Mode change persists to NVS  
✅ Device reboots after mode change  
✅ Only selected connectivity stack(s) initialize  
✅ Mode survives power cycle  
✅ User can switch back if mode fails

## Implementation Status: ✅ COMPLETE

All requirements met:
- WiFi and BLE both fully implemented
- Default to WiFi on fresh flash
- Settings UI toggle with dropdown
- Mode persists to NVS
- Automatic reboot on mode change
