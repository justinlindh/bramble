# Bramble Traffic Efficiency Analysis Report

**Generated:** 2026-02-21 22:42:01
**Source:** `reports/traffic-capture-simulated.jsonl`

## Summary

- **Total Events:** 74
- **TX Events:** 37 (50.0%)
- **RX Events:** 37 (50.0%)
- **Total Airtime:** 31.40s (31,401,000µs)
- **Average Packet Length:** 71.2 bytes
- **Max Packet Length:** 177 bytes

## Airtime by Category

| Category | Airtime (s) | Airtime (%) | Events |
|----------|-------------|-------------|--------|
| chat | 10.325s | 32.9% | 14 |
| beacon | 9.806s | 31.2% | 25 |
| routing | 5.508s | 17.5% | 12 |
| ack | 3.237s | 10.3% | 15 |
| timesync | 2.095s | 6.7% | 7 |
| maintenance | 0.431s | 1.4% | 1 |

## Airtime by Tier (Bucket)

| Tier | Airtime (s) | Airtime (%) | Events |
|------|-------------|-------------|--------|
| broadcast | 16.823s | 53.6% | 43 |
| normal | 11.341s | 36.1% | 16 |
| critical | 3.237s | 10.3% | 15 |

## Top Packet Types by Airtime

| Pkt Type | Airtime (s) | Airtime (%) | Events |
|----------|-------------|-------------|--------|
| 5 | 9.806s | 31.2% | 25 |
| 20 | 9.767s | 31.1% | 13 |
| 30 | 4.492s | 14.3% | 10 |
| 40 | 3.237s | 10.3% | 15 |
| 10 | 2.095s | 6.7% | 7 |
| 31 | 1.016s | 3.2% | 2 |
| 21 | 0.557s | 1.8% | 1 |
| 51 | 0.431s | 1.4% | 1 |

## Tuning Recommendations

### 🔴 High Broadcast Bucket Usage (53.6%)

**Impact:** Broadcast budget draining faster than expected.

**Recommendations:**
- **Reduce beacon interval** (currently using 31.2% of total airtime)
  - Current: likely 30s → Suggested: 60s or adaptive
  - Expected savings: ~4.90s per capture period

### TX/RX Ratio: 1.00


## Next Steps

1. Implement recommended tuning changes
2. Capture new baseline after changes
3. Compare before/after metrics
4. Monitor long-term trends (24h+ captures)
5. Consider implementing adaptive algorithms based on network density