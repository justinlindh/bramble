/*
 * Shadow of main/mesh_task.h for the ws_server host suite only.
 *
 * The real header pulls in routing, dedup, freq_plan, channel_key,
 * channel_msg, public_channel, airtime_budget, traffic_debug, location and
 * delivery_event_ring. main/ws_server.c uses exactly one symbol from it, so
 * the suite compiles against this instead of dragging the whole mesh stack
 * onto the host.
 *
 * It claims the real header's include guard on purpose. A quoted include
 * resolves against the including file's own directory first, so ws_server.c
 * would always find main/mesh_task.h no matter how the -I paths are
 * ordered. test_ws_server.c includes this file first, which then renders
 * the real header inert. This directory is on the include path of the
 * ws_server test target only.
 */

#ifndef BRAMBLE_MESH_TASK_H
#define BRAMBLE_MESH_TASK_H

void mesh_reboot_delayed(int delay_ms);

#endif /* BRAMBLE_MESH_TASK_H */
