# Task 2 report: gate the control plane on provisioned (inert unprovisioned node) + boot + sim/test harness

Status: DONE. Branch feat/mandatory-provisioning, base 9d0ba53c (Task 1).

After this task an UNPROVISIONED node emits NO network-key-authenticated frame
and ACCEPTS none (every verify rejects before the compare), while a PROVISIONED
mesh behaves byte-identically to before. Proven with host + gosim tests.

## Commits (5 logical pieces)

- `9276ed8b` feat(routing-auth): harden the 12 network-key MAC call sites (fail closed)
- `c3a745bd` feat(mesh): make an unprovisioned node inert; kill beacon PSK fallback
- `5eebf45f` test(host): provision a fixed network key in the control-plane auth suites
- `b2164bf5` test(gosim): provision sim fleet by default; add unprovisioned-inert scenario
- (this report)

## Part A - the 12 MAC call sites hardened (routing_auth.c + discovery.c)

Threaded `network_key_mac()`'s int return through every sign+verify helper.

SIGN helpers changed `void -> int` and now `return network_key_mac(...)`:
rerr_sign, ack_sign, receipt_sign, data_auth_sign, ident_relay_sign
(routing_auth.c) and rrep_sign (discovery.c). 0 on success; nonzero when
unprovisioned (it wrote the all-zero sentinel, not a fallback HMAC). Callers
treat nonzero as "do not transmit".

VERIFY helpers (rerr/ack/receipt/data_auth/ident_relay/rrep) now:

```c
if (network_key_mac(label, buf, sizeof(buf), expect) != 0)
    return 0;                 /* reject BEFORE the ct_eq compare */
return ct_eq(expect, received, 8);
```

This is THE correctness point: unprovisioned `network_key_mac` writes the
all-zero sentinel into `expect[]`. A verify that ignored the return and did
`ct_eq(expect, received)` would ACCEPT a received all-zero MAC (a forgery).
Checking the return first makes an unprovisioned verifier reject everything,
including an all-zero frame. Header doc comments updated (they claimed these
helpers were "forgeable under the public-PSK fallback"; that fallback is gone).

## Part B - beacon PSK fallback removed (mesh_task.c mesh_rederive_beacon_key)

The unprovisioned branch no longer does
`channel_derive_key(BRAMBLE_PUBLIC_CHANNEL_PSK, ...)`. It `memset`s
`s_beacon_key` to zero and logs "unprovisioned: no beacon key". The provisioned
HKDF-from-network-key path is unchanged. public_channel.c untouched.

## Part C - the SEND gating list (all gated on network_key_is_provisioned)

Early-return when unprovisioned (log at debug "unprovisioned: inert"):
- `send_beacon`
- `send_ack`
- `send_rerr`
- `send_rrep`
- `send_identity_attestation`
- `queue_broadcast_delivery_receipt` (delivery-receipt origination)

DATA origination gated by checking `data_auth_sign()`'s new int return and
aborting the send (return 0) when nonzero, at all four sites:
- `send_data_packet` (channel/broadcast DATA + flood origination)
- `send_dm_packet`
- `mesh_send_location_packet` session branch
- `mesh_send_location_packet` channel branch

RX: `handle_beacon` drops when unprovisioned (its key would be all-zero, so
accepting would trust a forgeable MAC). Every other inbound
network-key-authenticated frame (DATA reverse-route learn, RREP, RERR, ACK,
receipt, attestation relay) is already dropped by its Part A verify helper.
The LOCAL provisioning channel (setNetworkKey RPC / websocket) is NOT
network-key-gated and stays functional while inert - confirmed it does not
depend on provisioned state.

## Part D - boot + NVS consolidation

- `mesh_load_network_key()` now calls the component's
  `network_key_load_from_nvs()` (single source of truth for NVS namespace/key
  and in-memory provisioning) and logs the boot state clearly: provisioned, or
  "node is INERT until provisioned". No more hand-rolled nvs_open/get_blob.
- `rpc_set_network_key` dropped its hand-rolled nvs_set_blob/commit and relies
  on `network_key_set_provisioned()`'s persist-on-set (removes the double
  write, removes RPC/component disagreement risk). Result contract unchanged.

## Part E - harness

HOST (test/test_net_key.h): a shared fixed 32-byte key +
`bramble_test_provision_net_key()`, included via same-directory lookup (no
CMake change). 11 suites' setUp now provision that key instead of running
unprovisioned/clear: test_ack_auth, test_data_auth, test_discovery,
test_ident_relay_auth, test_ident_relay_gate, test_rerr_auth, test_rrep_auth,
test_unicast_flood, test_flooded_ack, test_flood_origination,
test_flood_suppression_auth. The "wrong key forgery" tests now re-provision the
fixed fleet key (provisioned verifier rejecting a differently-keyed MAC) rather
than clearing. New explicit inert tests: `test_ack_unprovisioned_is_inert` and
`test_unprovisioned_is_inert` (sign fails + emits the all-zero sentinel; verify
rejects that sentinel before the compare; no breadcrumb learned).

GOSIM: `bridge_init` provisions a shared default key (BRIDGE_DEFAULT_NET_KEY)
for the whole fleet - the real network_key.c is a process-global, so this is
the sim analog of a provisioned boot and every existing scenario meshes exactly
as before. Per-node inertness modeled by a new `bridge_node_ext_t.provisioned`
flag (default true), set via `bridge_node_set_provisioned`. A scenario opts a
node out with a per-node `"unprovisioned": true` field, read Go-side
(loadUnprovisionedNodeIDs, provisioning.go) and applied after join. Unprovisioned
nodes originate nothing (generate_message / generate_attestation emit
"unprovisioned_inert") and drop all inbound frames (receive_packet). New
`TestUnprovisionedNodeIsInert`: provisioned pair meshes while the unprovisioned
node originates nothing and receives nothing.

## No authenticated frame emitted/accepted while unprovisioned

- Emit: every send path is gated (Part C) AND the underlying sign returns
  nonzero without a real MAC (Part A). Belt and suspenders.
- Accept: every verify rejects before the compare (Part A) and handle_beacon
  drops (Part C). An all-zero sentinel frame is explicitly rejected, proven by
  the new host inert tests.
- Provisioned mesh: gates are pure short-circuits that never fire when
  provisioned; sign/verify math unchanged -> byte-identical.

## CI - all green

- Host tests: `bash test/run_all_tests.sh` -> 101 suites, 101 passed, 0 failed.
- Board build: `flash.sh local heltec-v3 build` -> Project build complete, 0
  errors, signed bramble.bin generated.
- clang-format v14 (runner image bramble/runner-full:22.04-go126): PASS.
- cppcheck (--enable=warning,performance,portability --std=c11): 0 findings.
- check-rpc-contract: OK (53 methods match openapi.yaml).
- gosim: `go build ./...` clean; `go test -count=1 ./...` -> ok.

## Concerns / notes

- The gosim flood MODEL path (routing:"flood", Go-only benchmark in flood.go)
  is not gated; the inert test uses the real firmware reactive path through
  bridge.c, which is what mirrors firmware. Left the Go-only model alone.
- rpc_set_network_key no longer surfaces an NVS write failure as RPC_ERR_INTERNAL
  (network_key_set_provisioned's persist is best-effort by design: a write
  failure does not un-provision the running node). Acceptable per the
  component's documented contract.
