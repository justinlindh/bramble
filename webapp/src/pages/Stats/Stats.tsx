import { useStore } from '../../store/index';
import styles from './Stats.module.css';

function formatUptime(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return `${h}h ${m}m`;
}

function formatBytes(bytes: number): string {
  if (bytes > 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes > 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  return `${bytes} B`;
}

export function Stats() {
  const status = useStore(s => s.status);
  const airtime = useStore(s => s.airtime);

  if (!status) {
    return (
      <div className={styles.stats}>
        <p className={styles.empty}>Connect to a node to view statistics.</p>
      </div>
    );
  }

  return (
    <div className={styles.stats}>
      <div className={styles.grid}>
        <StatCard value={status.txCount.toLocaleString()} label="TX" />
        <StatCard value={status.rxCount.toLocaleString()} label="RX" />
        <StatCard value={String(status.neighborCount)} label="Neighbors" />
        <StatCard value={String(status.routeCount)} label="Routes" />
        <StatCard value={formatUptime(status.uptimeSec)} label="Uptime" />
        <StatCard value={formatBytes(status.freeHeapBytes)} label="Free Heap" />
      </div>

      {airtime && (
        <section className={styles.airtimeSection}>
          <h2>⏱ Airtime Budget</h2>
          {airtime.tiers.map(tier => {
            const remaining = 100 - tier.usedPct;
            return (
              <div key={tier.name} className={styles.tierRow}>
                <span className={styles.tierName}>{tier.name.charAt(0).toUpperCase() + tier.name.slice(1)}</span>
                <div className={styles.track}>
                  <div
                    className={`${styles.fill} ${styles[tier.name]}`}
                    style={{ width: `${remaining}%` }}
                  />
                </div>
                <span className={styles.pct}>{remaining.toFixed(0)}%</span>
              </div>
            );
          })}
        </section>
      )}
    </div>
  );
}

function StatCard({ value, label }: { value: string; label: string }) {
  return (
    <div className={styles.card}>
      <div className={styles.cardValue}>{value}</div>
      <div className={styles.cardLabel}>{label}</div>
    </div>
  );
}
