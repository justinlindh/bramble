import { useEffect, useState } from 'react';
import type { AirtimeStatus, AirtimeTier } from '../../types/bramble';
import { IconClock } from '../../components/Icons';
import styles from './AirtimeCard.module.css';

function formatMs(ms: number): string {
  if (ms >= 10_000) return `${(ms / 1000).toFixed(0)}s`;
  if (ms >= 1_000) return `${(ms / 1000).toFixed(1)}s`;
  return `${ms}ms`;
}

function formatRefill(refillAtMs: number): string {
  const diffMs = refillAtMs - Date.now();
  if (diffMs <= 0) return 'now';
  const s = Math.ceil(diffMs / 1000);
  if (s >= 60) return `in ${Math.floor(s / 60)}m ${s % 60}s`;
  return `in ${s}s`;
}

const TIER_ORDER: AirtimeTier['name'][] = ['critical', 'normal', 'broadcast', 'receipt'];

const TIER_TOOLTIPS: Record<AirtimeTier['name'], string> = {
  critical: 'Critical: reliable delivery for emergency alerts and SOS location traffic',
  normal: 'Normal: acknowledged direct messages and peer data',
  broadcast: 'Broadcast: fire-and-forget channel messages and network announcements',
  receipt: 'Receipt: delivery receipts and acknowledgements for broadcast traffic',
};

export function AirtimeCard({ airtime }: { airtime: AirtimeStatus }) {
  // Tick every second so refill countdowns stay live
  const [, tick] = useState(0);
  useEffect(() => {
    const id = setInterval(() => tick(n => n + 1), 1000);
    return () => clearInterval(id);
  }, []);

  const sorted = TIER_ORDER.flatMap(name =>
    airtime.tiers.filter(t => t.name === name)
  );

  return (
    <section className={styles.card}>
      <h2 className={styles.heading}><IconClock size={18} /> Airtime Budget</h2>
      {sorted.map(tier => {
        const remainPct =
          tier.maxMs > 0
            ? Math.min(100, (tier.remainingMs / tier.maxMs) * 100)
            : 0;
        return (
          <div key={tier.name} className={styles.tier}>
            <div className={styles.tierHeader}>
              <span
                className={`${styles.tierLabel} ${styles[tier.name]}`}
                title={TIER_TOOLTIPS[tier.name]}
              >
                {tier.name.charAt(0).toUpperCase() + tier.name.slice(1)}
              </span>
              <span className={styles.tierMs}>
                {formatMs(tier.remainingMs)} / {formatMs(tier.maxMs)}
              </span>
            </div>

            <div className={styles.track}>
              <div
                className={`${styles.fill} ${styles[tier.name]}`}
                style={{ width: `${remainPct}%` }}
              />
            </div>

            <div className={styles.tierFooter}>
              <span className={styles.pct}>{remainPct.toFixed(0)}% remaining</span>
              <span className={styles.refill}>refills {formatRefill(tier.refillAtMs)}</span>
            </div>
          </div>
        );
      })}
    </section>
  );
}
