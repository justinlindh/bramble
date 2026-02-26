# Mesh Task State Refactoring Guide

## Overview

This document guides the migration of static global state from `mesh_task.c` into cohesive sub-modules. The goal is to reduce coupling, improve testability, and make the 2000+ line mesh_task.c more maintainable.

## New Module Structure

| Module | Header | Purpose |
|--------|--------|---------|
| `mesh_neighbor_state` | `mesh_neighbor_state.h` | Neighbor table management |
| `mesh_routing_state` | `mesh_routing_state.h` | Route table, discovery, reverse routes |
| `mesh_dedup_state` | `mesh_dedup_state.h` | Packet deduplication buffer |
| `mesh_msg_state` | `mesh_msg_state.h` | Message queue, pending ACKs, reassembly |

## Global State Mapping

### mesh_neighbor_state.c
Encapsulates neighbor discovery and tracking.

| Original Global | Type | Status |
|----------------|------|--------|
| `s_neighbors` | `neighbor_table_t` | Ready to migrate |

**Functions in mesh_task.c that access this state:**
- `handle_beacon()` — updates neighbors via `neighbor_update()`
- `mesh_periodic_maintenance()` — calls `neighbor_purge()`
- `mesh_get_state()` — copies `s_neighbors` to shared state
- `send_beacon()` — reads `neighbor_count()`
- `mesh_get_peer_name()` — reads `neighbor_lookup()`

**Risk Level:** LOW — well-isolated, minimal cross-dependencies

---

### mesh_routing_state.c
Encapsulates all routing protocol state.

| Original Global | Type | Status |
|----------------|------|--------|
| `s_routes` | `routing_table_t` | Ready to migrate |
| `s_rreq_dedup` | `rreq_dedup_t` | Ready to migrate |
| `s_reverse_routes` | `reverse_route_table_t` | Ready to migrate |
| `s_pending_disc` | `pending_discovery_table_t` | Ready to migrate |
| `s_rreq_rl` | `rreq_rate_limiter_t` | Ready to migrate |

**Functions in mesh_task.c that access this state:**
- `handle_rreq()` — uses `s_rreq_dedup`, `s_reverse_routes`, `s_routes`
- `handle_rrep()` — uses `s_routes`, `s_reverse_routes`, `s_pending_disc`
- `handle_rerr()` — uses `s_routes`
- `forward_ack()` — uses `s_routes` (route_lookup)
- `forward_data_packet()` — uses `s_routes` (route_lookup)
- `mesh_periodic_maintenance()` — uses `s_routes`, `s_reverse_routes`, `s_pending_disc`
- `initiate_discovery()` — uses `s_rreq_rl`, `s_pending_disc`
- `mesh_send_message()` — uses `s_routes` (route_lookup)
- `mesh_get_routes()` — copies `s_routes`
- `flush_queued_messages()` — indirectly via discovery

**Risk Level:** MEDIUM — core routing logic, but uses existing well-tested component APIs

---

### mesh_dedup_state.c
Encapsulates packet deduplication.

| Original Global | Type | Status |
|----------------|------|--------|
| `s_dedup` | `dedup_buffer_t` | Ready to migrate |

**Functions in mesh_task.c that access this state:**
- `mesh_process_rx_packet()` — calls `dedup_check_and_add()`
- `mesh_periodic_maintenance()` — calls `dedup_purge()`

**Risk Level:** LOW — single-purpose, minimal dependencies

---

### mesh_msg_state.c
Encapsulates message handling state.

| Original Global | Type | Status |
|----------------|------|--------|
| `s_queued_msgs` | `queued_msg_t[8]` | Ready to migrate |
| `s_pending_acks` | `pending_ack_table_t` | Ready to migrate |
| `s_reassembly` | `reassembly_ctx_t` | Ready to migrate |

**Functions in mesh_task.c that access this state:**
- `queue_message()` — writes to `s_queued_msgs`
- `flush_queued_messages()` — reads/clears `s_queued_msgs`
- `mesh_periodic_maintenance()` — expires `s_queued_msgs`, ticks `s_pending_acks`, purges `s_reassembly`
- `send_data_packet()` — adds to `s_pending_acks`
- `handle_ack()` — removes from `s_pending_acks`
- `handle_data()` — uses `s_reassembly`

**Risk Level:** MEDIUM — touches reliability and fragmentation paths

---

## Migration Order (Recommended)

### Phase 1: Low-Risk Extractions ✓ (Scaffolding Complete)
1. **mesh_dedup_state** — simplest, no cross-module deps
2. **mesh_neighbor_state** — well-isolated, single type

### Phase 2: Core Routing
3. **mesh_routing_state** — larger scope but clean boundaries

### Phase 3: Message Handling
4. **mesh_msg_state** — depends on routing state for queue flush

---

## Migration Steps Per Module

For each module:

1. **Add include to mesh_task.c:**
   ```c
   #include "mesh_neighbor_state.h"  // (example)
   ```

2. **Remove the static global from mesh_task.c:**
   ```c
   // DELETE: static neighbor_table_t s_neighbors;
   ```

3. **Update initialization in mesh_task_start():**
   ```c
   // BEFORE: neighbor_init(&s_neighbors);
   // AFTER:  mesh_neighbor_state_init();
   ```

4. **Update all access sites to use new APIs:**
   ```c
   // BEFORE: neighbor_update(&s_neighbors, ...);
   // AFTER:  mesh_neighbor_update(...);
   
   // BEFORE: neighbor_count(&s_neighbors)
   // AFTER:  mesh_neighbor_count()
   ```

5. **Compile and test:**
   ```bash
   cd ~/src/bramble
   ./scripts/bramble-build.sh tdeck-plus
   ```

---

## State NOT Being Migrated (This Phase)

These globals remain in mesh_task.c for now:

| Global | Reason |
|--------|--------|
| `s_identity` | Pointer to external identity, not owned by mesh_task |
| `s_state_mutex`, `s_delivery_event_mutex` | FreeRTOS handles, task-local |
| `s_rx_queue` | FreeRTOS handle, task-local |
| `s_shared` | Shared state struct, specific to mesh_task API |
| `s_delivery_event_ring` | Delivery tracking, specific to mesh_task |
| `s_node_name` | Simple config, low coupling |
| `s_channels`, `s_channel_names`, `s_channel_has_psk`, etc. | Channel state — candidate for future `mesh_channel_state` module |
| `s_mailbox_*` | Mailbox state — candidate for future `mesh_mailbox_state` module |
| `s_location_*` | Location state — candidate for future `mesh_location_state` module |
| `s_beacon_policy`, `s_beacon_status`, `s_churn_*` | Beacon policy — candidate for future `mesh_beacon_state` module |
| `s_traffic_*` | Traffic debug — already well-contained via traffic_debug component |
| `s_probe_*` | Probe state — candidate for future `mesh_probe_state` module |
| `s_airtime` | Airtime budget — could merge with reliability or stay |

---

## Testing Strategy

1. **Unit tests** — Each new module should have corresponding tests
2. **Integration** — Full mesh_task behavior unchanged after migration
3. **Regression** — Existing tests pass (esp. multi-hop routing tests)

---

## Files Changed

- `main/CMakeLists.txt` — Added new source files
- `main/mesh_neighbor_state.{h,c}` — NEW
- `main/mesh_routing_state.{h,c}` — NEW
- `main/mesh_dedup_state.{h,c}` — NEW
- `main/mesh_msg_state.{h,c}` — NEW

---

## Next Steps

1. Build to verify scaffolding compiles
2. Pick first module (recommend `mesh_dedup_state`)
3. Follow migration steps above
4. Test, commit, repeat for next module

---

## Related Work

- Future: `mesh_channel_state` module for channel management
- Future: `mesh_mailbox_state` module for store-and-forward
- Future: `mesh_probe_state` module for network diagnostics
