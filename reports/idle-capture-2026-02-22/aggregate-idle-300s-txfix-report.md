# Bramble Traffic Efficiency Analysis Report

**Generated:** 2026-02-22 01:21:09
**Source:** `/home/user/src/bramble/reports/idle-capture-2026-02-22/aggregate-idle-300s-txfix.jsonl`

## Summary

- **Total Events:** 61
- **TX Events:** 22 (36.1%)
- **RX Events:** 39 (63.9%)
- **Total Airtime:** 19.22s (19,221,000µs)
- **Average Packet Length:** 51.5 bytes
- **Max Packet Length:** 64 bytes

## Airtime by Category

| Category | Airtime (s) | Airtime (%) | Events |
|----------|-------------|-------------|--------|
| beacon | 13.717s | 71.4% | 45 |
| chat | 4.991s | 26.0% | 13 |
| routing | 0.329s | 1.7% | 2 |
| ack | 0.183s | 1.0% | 1 |

## Airtime by Tier (Bucket)

| Tier | Airtime (s) | Airtime (%) | Events |
|------|-------------|-------------|--------|
| broadcast | 13.717s | 71.4% | 45 |
| normal | 5.503s | 28.6% | 16 |

## Top Packet Types by Airtime

| Pkt Type | Airtime (s) | Airtime (%) | Events |
|----------|-------------|-------------|--------|
| 5 | 13.717s | 71.4% | 45 |
| 10 | 4.991s | 26.0% | 13 |
| 4 | 0.329s | 1.7% | 2 |
| 1 | 0.183s | 1.0% | 1 |

## Tuning Recommendations

### 🔴 High Broadcast Bucket Usage (71.4%)

**Impact:** Broadcast budget draining faster than expected.

**Recommendations:**
- **Reduce beacon interval** (currently using 71.4% of total airtime)
  - Current: likely 30s → Suggested: 60s or adaptive
  - Expected savings: ~6.86s per capture period

### TX/RX Ratio: 0.56


## Next Steps

1. Implement recommended tuning changes
2. Capture new baseline after changes
3. Compare before/after metrics
4. Monitor long-term trends (24h+ captures)
5. Consider implementing adaptive algorithms based on network density