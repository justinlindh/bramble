# T-Deck UI: Map Tab Implementation

## Summary

Added a new Map tab to the T-Deck Plus LVGL UI showing node positions using GPS/grid square data. The implementation follows the existing screen patterns and integrates with the location manager component.

## Changes Made

### 1. Location State Access (mesh_task.c/h)

Added a thread-safe getter function for location manager state:

```c
void mesh_get_location_state(location_manager_t *out);
```

This follows the same pattern as `mesh_get_state()` and `mesh_get_routes()`, using mutex protection to safely copy the location manager state for UI consumption.

**Files Modified:**
- `main/mesh_task.c` - Added getter implementation
- `main/mesh_task.h` - Added function declaration

### 2. UI Layout Updates (scr_layout.h/c)

Extended the tab system to include a Map tab:

- Added `TAB_MAP` to the `bramble_tab_t` enum (between TAB_NODES and TAB_STATS)
- Updated tab button width from 75px to 60px to accommodate 5 tabs (320px / 5 = 64px each)
- Added GPS symbol (`LV_SYMBOL_GPS`) and "Map" label for the tab button
- Added case handler in `layout_set_tab()` to call `scr_map_create()`
- Included `scr_map.h` header

**Files Modified:**
- `components/ui_graphics/screens/scr_layout.h` - Updated enum
- `components/ui_graphics/screens/scr_layout.c` - Added tab configuration and handler

### 3. Map Screen Implementation (scr_map.c/h)

Created a new map screen that displays node positions on a simple coordinate grid:

**Features:**
- **Self Position Display**: Shows device's current GPS position as a blue marker labeled "You"
- **Peer Position Display**: Shows cached peer locations as green markers with node names/addresses
- **Coordinate Projection**: Uses simple equirectangular projection for lat/lon to pixel conversion
- **Grid Reference**: Displays crosshair grid lines centered on self position
- **Zoom Level**: Fixed 5km radius view (±5km from center)
- **Status Info**: Shows current coordinates and GPS accuracy
- **Peer Count**: Displays number of visible peers on the map
- **Graceful Degradation**: Shows "No GPS data" message when position is unavailable

**Map Canvas:**
- Canvas size: 280×140 pixels
- Container size: 312×148 pixels (with 4px padding)
- Uses LVGL v9 draw buffer API (`LV_DRAW_BUF_DEFINE_STATIC`)
- Background grid with horizontal/vertical crosshairs
- RGB565 color format for efficiency

**Marker System:**
- Self marker: Blue (#0066FF) circle, 10px diameter, white border
- Peer markers: Green (#00CC00) circles, 10px diameter, white border
- Labels: Node name or shortened hex address (last 4 digits)
- Labels positioned to the right of markers with semi-transparent background

**Data Flow:**
1. Calls `mesh_get_location_state()` to get location manager snapshot
2. Calls `mesh_get_state()` to get neighbor table for peer names
3. Checks if self position is valid (`my_position.valid`)
4. Projects self position to canvas center (140, 70)
5. Iterates through location cache entries, projects and displays peer positions
6. Matches peer addresses to neighbor table to resolve names

**Files Created:**
- `components/ui_graphics/screens/scr_map.h` - Header with public interface
- `components/ui_graphics/screens/scr_map.c` - Implementation

### 4. Build Configuration

Updated CMakeLists.txt to include the new map screen source file:

**Files Modified:**
- `components/ui_graphics/CMakeLists.txt` - Added `"screens/scr_map.c"` to build sources

## Technical Details

### Coordinate Projection

The implementation uses a simple equirectangular projection (plate carrée):

```
x_km = (lon - center_lon) × 111 × cos(center_lat)
y_km = (lat - center_lat) × 111
```

Where:
- 1° latitude ≈ 111 km (constant)
- 1° longitude ≈ 111 × cos(latitude) km (varies by latitude)

This projection is accurate enough for small areas (±5km) and avoids the complexity of true Mercator or other projections.

### Location Data Sources

The map screen pulls location data from two sources:

1. **Self Position**: `location_manager_t.my_position` (bramble_position_t)
   - Updated by GPS component via `location_set_position()`
   - Contains lat/lon in 1e7 format (e.g., 37.7749 → 377749000)
   - Includes accuracy, speed, heading, altitude

2. **Peer Positions**: `location_manager_t.cache[]` (location_cache_entry_t array)
   - Updated when receiving location packets from peers
   - Filtered by `active` flag and `pos.valid` flag
   - Matched against neighbor table for name resolution

### Memory Considerations

- Canvas draw buffer: ~78KB static allocation (280×140×2 bytes for RGB565)
- Location manager state: ~1.5KB stack allocation (copied via mutex)
- Mesh state: ~1.8KB stack allocation (shared neighbor table)
- All static/stack allocations avoid heap fragmentation

### Performance

- Map screen regenerates on every tab switch (not real-time updating)
- No background refresh timer (reduces CPU/battery usage)
- User must switch away and back to refresh map data
- Future enhancement: Add refresh button or periodic update timer

## References

### Existing Patterns

The implementation follows these existing patterns:

- **Screen Structure**: Modeled after `scr_nodes.c` for mesh data access
- **Data Access**: Uses `mesh_get_*()` pattern like `scr_stats.c`
- **UI Styling**: Follows Bramble theme constants (`BR_COLOR_*`, `BR_RADIUS`, etc.)
- **Canvas Usage**: Based on LVGL v9 examples (`lv_example_canvas_1.c`)

### Webapp Map Reference

The webapp Map implementation (`webapp/src/pages/Map/Map.tsx`) provides:

- Grid square conversion logic (Maidenhead locator system)
- Route line rendering (not implemented in T-Deck version)
- Node label formatting (name + hex address)
- Map focus and zoom controls (simplified for T-Deck)

### Component Dependencies

- `components/location/` - Location manager and position data structures
- `components/routing/` - Neighbor table for peer name lookup
- `components/ui_graphics/theme/` - Bramble UI theme constants
- `main/mesh_task.c` - Mesh state and location state access

## Testing

### Prerequisites

1. T-Deck Plus device with GPS module
2. Bramble firmware with location sharing enabled
3. At least one peer node broadcasting location data

### Test Scenarios

**Scenario 1: No GPS Fix**
- Expected: "No GPS data available. Waiting for position fix..." message
- Verify: GPS indicator in status bar shows no fix

**Scenario 2: Self Position Only**
- Expected: Blue marker at center, lat/lon/accuracy displayed, "0 peers visible"
- Verify: GPS coordinates match `bramble location get` CLI output

**Scenario 3: Multiple Peers**
- Expected: Blue self marker + green peer markers, peer count displayed
- Verify: Peer positions roughly match webapp Map view
- Verify: Peer labels show names or hex addresses

**Scenario 4: Off-Screen Peers**
- Expected: Peers beyond ±5km range are not displayed (off-canvas)
- Verify: Peer count may be less than total cached locations

**Scenario 5: Tab Switching**
- Expected: Map refreshes data on each tab switch
- Verify: Switching to other tabs and back updates positions

### Known Limitations

1. **Static View**: Map does not auto-refresh; requires tab switch to update
2. **No Panning/Zoom**: Fixed 5km zoom level, always centered on self
3. **No Route Lines**: Peer connections not visualized (unlike webapp)
4. **Grid Squares**: Coarse location (grid square) rendering not implemented
5. **No Interaction**: Markers are not clickable/selectable

### Future Enhancements

- [ ] Add refresh button or 30-second auto-refresh timer
- [ ] Render route lines between connected peers (from routing table)
- [ ] Add zoom controls (+/- buttons for 1km, 5km, 10km views)
- [ ] Support coarse location (grid square) rendering as rectangles
- [ ] Make markers selectable to show peer details
- [ ] Add compass/heading indicator
- [ ] Show GPS satellite count/signal strength
- [ ] Pan map using trackball (recenter on peer positions)

## Build Instructions

```bash
cd /home/user/src/bramble

# Set up ESP-IDF environment (if not already done)
. $HOME/esp/esp-idf/export.sh

# Build for T-Deck Plus
idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tdeck_plus" build

# Flash to device
idf.py -p /dev/ttyUSB0 flash monitor
```

Or use the CI build script:

```bash
cd /home/user/src/bramble
./scripts/ci-build-firmware.sh
```

## Files Changed/Created

### Modified Files
- `main/mesh_task.c` - Added location state getter
- `main/mesh_task.h` - Added function declaration
- `components/ui_graphics/screens/scr_layout.h` - Added TAB_MAP enum
- `components/ui_graphics/screens/scr_layout.c` - Added map tab configuration
- `components/ui_graphics/CMakeLists.txt` - Added scr_map.c to build

### Created Files
- `components/ui_graphics/screens/scr_map.h` - Map screen header
- `components/ui_graphics/screens/scr_map.c` - Map screen implementation
- `docs/tdeck-map-tab-implementation.md` - This documentation

## Integration Notes

### Location Sharing Configuration

For the map to show peer positions, location sharing must be enabled:

```bash
# Enable location sharing with full precision
bramble location set --enabled --tier full --interval 300 --source gps

# Verify configuration
bramble location get
```

### GPS Module

The T-Deck Plus has a built-in GPS module. Ensure it has:
- Clear view of the sky (outdoor or near window)
- Adequate time to acquire satellite lock (1-5 minutes cold start)
- GPS antenna properly connected (if external)

### Peer Discovery

Peers must:
1. Be within radio range (mesh connectivity)
2. Have location sharing enabled
3. Have valid GPS position (or be sending location data)

Check peer locations via CLI:

```bash
bramble location peers
```

## Related Components

- **Location Manager** (`components/location/`) - Position tracking and sharing
- **GPS Component** (`components/gps/`) - GPS hardware interface (if present)
- **Routing** (`components/routing/`) - Neighbor table for peer discovery
- **Mesh Task** (`main/mesh_task.c`) - Central mesh state and message handling
- **UI Graphics** (`components/ui_graphics/`) - LVGL UI framework

## Webapp Parity

The T-Deck map implementation is a simplified version of the webapp Map:

| Feature | Webapp | T-Deck |
|---------|--------|--------|
| Self marker | ✓ Blue circle | ✓ Blue circle |
| Peer markers (exact) | ✓ Green circle | ✓ Green circle |
| Peer markers (coarse) | ✓ Yellow rectangle | ✗ Not implemented |
| Node labels | ✓ Name + hex | ✓ Name or short hex |
| Route lines | ✓ Colored by state | ✗ Not implemented |
| Grid squares overlay | ✓ Optional | ✗ Not implemented |
| Zoom controls | ✓ Interactive | ✗ Fixed zoom |
| Pan controls | ✓ Drag | ✗ Static |
| Marker popup | ✓ Click details | ✗ Not implemented |
| Real-time updates | ✓ WebSocket | ✗ Manual refresh |
| Legend | ✓ Color key | ✗ Not implemented |

## Performance Impact

- **Memory**: +78KB static (canvas buffer), minimal runtime overhead
- **CPU**: Negligible (map only renders on tab switch, not continuously)
- **Battery**: No impact (no background timers or GPS polling)
- **Flash**: +~4KB code size (map screen implementation)

## Conclusion

The Map tab successfully integrates GPS/location data into the T-Deck UI, providing a visual representation of mesh node positions. While simplified compared to the webapp version, it offers essential location awareness for field use without overwhelming the embedded display constraints.
