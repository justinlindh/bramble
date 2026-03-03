# Airtime Budget v2 (Continuous Refill + Receipt Tier + Adaptive Profile)

## Why this exists

v1 behavior used an hourly cliff refill model: once tokens were spent, budgets stayed empty until the next full-hour reset. In practice this caused abrupt receipt suppression during testing and unstable UX in small meshes.

v2 changes:
1. Continuous refill token bucket (proportional refill by elapsed time)
2. Dedicated `receipt` airtime tier
3. Adaptive profile by mesh size

## Defaults

Base budgets (ms/hour):
- critical: 36000
- normal: 18000
- broadcast: 18000
- receipt: 12000

Tier semantics:
- `critical`: urgent/unblockable class (can borrow from normal)
- `normal`: unicast data and routine traffic
- `broadcast`: broadcast data traffic
- `receipt`: broadcast delivery receipts (generated + forwarded)

## Continuous refill model

For each tier `i`:

```
add_i = floor((max_i * elapsed_ms + remainder_i) / REFILL_INTERVAL_MS)
remainder_i = (max_i * elapsed_ms + remainder_i) % REFILL_INTERVAL_MS
tokens_i = min(max_i, tokens_i + add_i)
```

This prevents hour-boundary cliffs and makes suppression behavior gradual/predictable.

## Adaptive profile by peer count

Peer count is used to adjust effective max tokens:

- `<= 15` peers (small mesh)
  - normal: 150%
  - broadcast: 200%
  - receipt: 200%
  - critical: 100%
- `16..40` peers (baseline)
  - all: 100%
- `> 40` peers (large mesh)
  - normal: 75%
  - broadcast: 60%
  - receipt: 50%
  - critical: 100%

## Troubleshooting

### "Delivery receipt suppressed ... receipt airtime budget exhausted"

This means receipt tier tokens are depleted. Typical causes:
- too many back-to-back broadcast tests
- too many retries/receipt waves in dense meshes

Check via RPC (`bramble.getAirtime`) fields:
- `receipt_remaining_ms`
- `receipt_max_ms`
- `next_refill_ms` (continuous model reports 0 because refill is immediate/ongoing)

Mitigations:
- increase spacing between test broadcasts
- reduce test count
- reboot nodes (resets tokens)
- tune receipt/broadcast profile percentages for your deployment

### Broadcast traffic seems fine but receipts are low

Expected if receipt tier is exhausted while broadcast tier still has tokens. This separation is intentional so receipt storms do not starve broadcast data.

## Files

- Core: `components/airtime/airtime_budget.c`
- API/constants: `components/airtime/include/airtime_budget.h`
- Receipt tier wiring: `main/mesh_task.c`
- Airtime RPC fields: `main/rpc_methods.c`
