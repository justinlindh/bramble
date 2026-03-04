# Default Public Channel ("Bramble Common")

> ✅ **SIMULATOR IMPLEMENTATION COMPLETE** (2026-02-17) — Checkboxes below not updated but all tasks were implemented in the simulator branch.

**Date:** 2026-02-17
**Status:** Draft

## Summary

Add a default public channel (Channel 0, "Bramble Common") that all nodes join automatically. Uses a well-known PSK so the existing encrypted channel code path handles it — no special plaintext mode needed.

## Design

### Key Derivation

```
PSK = SHA-256("bramble-default")
```

This produces the AES-256-GCM key used for Channel 0. The string and derivation are documented publicly. Anyone can compute the key — that's the point. It's a town square, not a vault.

### Why Encrypt with a Known Key?

1. **Code path consistency** — no special plaintext branch
2. **Easy upgrade** — a group forks by changing the PSK; same mechanism
3. **Protocol uniformity** — relay nodes treat all channel messages identically

## Channel Definition

- **Index:** 0 (reserved)
- **Name:** `"Bramble Common"`
- **PSK:** derived from `"bramble-default"` as above
- **Cannot be deleted.** User can mute but not leave.
- Stored in NVS like any other channel, but re-created if missing on boot.

## Auto-Join Behavior

- On first boot (or if Channel 0 is absent from NVS), the node creates it with the well-known PSK.
- Channel persists across reboots via normal NVS channel storage.
- UI exposes a **mute** toggle — muted means no notifications and messages aren't displayed, but the node still relays.

## Hop Limit

- **Default: 3 hops.** Balances reach vs. airtime on typical meshes.
- Configurable via Kconfig (`CONFIG_BRAMBLE_PUBLIC_CHANNEL_HOP_LIMIT`, default 3, range 1–7).
- Can also be changed at runtime via CLI/settings.

## Rate Limiting

Public channel gets tighter limits than private channels to prevent spam:

| Parameter | Default | Notes |
|-----------|---------|-------|
| Max messages per node | 1 msg / 30s | Enforced locally before TX |
| Burst allowance | 3 messages | Token bucket, refills at steady-state rate |
| Airtime interaction | Shares the global airtime budget | Public channel TX deducted from same duty-cycle tracker |

- Receiving nodes **also** rate-limit per source address — drop if a single source exceeds 1 msg/10s (protects against spoofed source, though encryption makes this hard).
- Configurable via Kconfig: `CONFIG_BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS` (default 30000).

## Message Format

No changes. Public channel messages use the existing channel message format:

```
[ Epoch | Nonce | AES-256-GCM( Channel-ID | Source-Addr | Payload ) ]
```

The only difference is the key is publicly known. Trial decryption against Channel 0's key works exactly like any other channel.

## Discovery & Presence

- Beacons are already broadcast; nodes on the public channel see each other. No change needed.
- **Optional future use:** public channel as initial contact surface — nodes can announce themselves and negotiate DM key exchange over it. Not in this scope, but the design supports it naturally.

## Interaction with Other Features

- **Emergency beacons:** broadcast on Channel 0 so all nearby nodes see them regardless of shared private channels.
- **Store-and-forward mailbox:** nodes can announce mailbox availability on Channel 0.
- **New node introduction:** a node joining the mesh is immediately visible on Channel 0.

These are future features that benefit from Channel 0 existing. No implementation needed now beyond the channel itself.

## Configuration (Kconfig)

```kconfig
config BRAMBLE_PUBLIC_CHANNEL_ENABLED
    bool "Enable default public channel"
    default y

config BRAMBLE_PUBLIC_CHANNEL_HOP_LIMIT
    int "Public channel hop limit"
    default 3
    range 1 7

config BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS
    int "Public channel rate limit (ms between messages)"
    default 30000
    range 5000 300000
```

## Implementation

### Changes Required

1. **`components/identity/` — channel init**
   - On boot, check NVS for Channel 0. If missing, create with well-known PSK.
   - Add `bramble_public_channel_init()` called from identity init.

2. **`components/crypto/` — key constant**
   - Add `BRAMBLE_PUBLIC_CHANNEL_PSK` as a compile-time constant (SHA-256 of `"bramble-default"`).
   - No changes to key derivation logic — it's just a fixed input.

3. **`components/routing/` — rate limiting**
   - Add per-source rate tracking for Channel 0 TX and RX.
   - Hook into existing channel flood TX path with a channel-index check.

4. **`components/shell/` (CLI)**
   - `channel list` shows Channel 0 as `[public]`.
   - `channel mute 0` / `channel unmute 0` toggles mute.
   - `channel delete 0` returns error.

5. **Kconfig**
   - Add the three config options above.

### What Doesn't Change

- Channel flood routing — Channel 0 is just another channel.
- Trial decryption — Channel 0's key is in the key list like any other.
- Beacon format — no changes.
- Message format — no changes.

## Tasks

- [ ] Add `BRAMBLE_PUBLIC_CHANNEL_PSK` constant to crypto component
- [ ] Add `bramble_public_channel_init()` to identity/channel init
- [ ] Add Kconfig options
- [ ] Add TX rate limiting for Channel 0 in routing
- [ ] Add RX per-source rate limiting for Channel 0
- [ ] Add CLI guards (no delete, mute/unmute)
- [ ] Add `[public]` label to `channel list` output
- [ ] Test: channel auto-created on fresh boot
- [ ] Test: channel survives reboot
- [ ] Test: rate limiting enforced
- [ ] Test: messages encrypt/decrypt with well-known key
