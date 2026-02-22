# Bramble Traffic Efficiency Analysis Report

**Generated:** 2026-02-22 00:30:40
**Source:** `/home/justin/src/bramble/reports/idle-capture-2026-02-22/aggregate-idle-300s.jsonl`

## Summary

- **Total Events:** 23
- **TX Events:** 0 (0.0%)
- **RX Events:** 23 (100.0%)
- **Total Airtime:** 6.95s (6,954,500µs)
- **Average Packet Length:** 49.5 bytes
- **Max Packet Length:** 50 bytes

## Airtime by Category

| Category | Airtime (s) | Airtime (%) | Events |
|----------|-------------|-------------|--------|
| beacon | 6.955s | 100.0% | 23 |

## Airtime by Tier (Bucket)

| Tier | Airtime (s) | Airtime (%) | Events |
|------|-------------|-------------|--------|
| broadcast | 6.955s | 100.0% | 23 |

## Top Packet Types by Airtime

| Pkt Type | Airtime (s) | Airtime (%) | Events |
|----------|-------------|-------------|--------|
| 5 | 6.955s | 100.0% | 23 |

## Tuning Recommendations

### 🔴 High Broadcast Bucket Usage (100.0%)

**Impact:** Broadcast budget draining faster than expected.

**Recommendations:**
- **Reduce beacon interval** (currently using 100.0% of total airtime)
  - Current: likely 30s → Suggested: 60s or adaptive
  - Expected savings: ~3.48s per capture period

## Next Steps

1. Implement recommended tuning changes
2. Capture new baseline after changes
3. Compare before/after metrics
4. Monitor long-term trends (24h+ captures)
5. Consider implementing adaptive algorithms based on network density