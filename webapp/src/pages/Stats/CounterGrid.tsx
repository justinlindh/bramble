import { useEffect, useRef, useState } from 'react';
import type { NodeStatus, AirtimeStatus } from '../../types/bramble';
import { IconPackets } from '../../components/Icons';
import styles from './CounterGrid.module.css';

interface CounterDelta {
  tx: number;
  rx: number;
  dropped: number;
}

export function CounterGrid({ status, airtime }: { status: NodeStatus; airtime?: AirtimeStatus | null }) {
  const prevRef = useRef<NodeStatus | null>(null);
  const [delta, setDelta] = useState<CounterDelta>({ tx: 0, rx: 0, dropped: 0 });

  // Compute deltas whenever the status snapshot changes
  useEffect(() => {
    if (prevRef.current) {
      setDelta({
        tx: Math.max(0, status.txCount - prevRef.current.txCount),
        rx: Math.max(0, status.rxCount - prevRef.current.rxCount),
        dropped: Math.max(0, status.droppedCount - prevRef.current.droppedCount),
      });
    }
    prevRef.current = status;
  }, [status]);

  const airtimeUsedMs = status.airtimeUsedMs > 0
    ? status.airtimeUsedMs
    : (airtime?.tiers?.reduce((sum, tier) => sum + Math.max(0, tier.maxMs - tier.remainingMs), 0) ?? 0);

  return (
    <section className={styles.card}>
      <h2 className={styles.heading}><IconPackets size={18} /> Packet Counters</h2>
      <div className={styles.grid}>
        <CounterCell
          label="Sent"
          value={status.txCount}
          delta={delta.tx}
          colorClass={styles.sent}
        />
        <CounterCell
          label="Received"
          value={status.rxCount}
          delta={delta.rx}
          colorClass={styles.received}
        />
        <CounterCell
          label="Dropped"
          value={status.droppedCount}
          delta={delta.dropped}
          colorClass={delta.dropped > 0 ? styles.dropped : undefined}
        />
        <CounterCell
          label="Neighbors"
          value={status.neighborCount}
        />
        <CounterCell
          label="Routes"
          value={status.routeCount}
        />
        <CounterCell
          label="Air Used"
          value={airtimeUsedMs}
          format={ms => ms >= 1000 ? `${(ms / 1000).toFixed(1)}s` : `${ms}ms`}
        />
      </div>
    </section>
  );
}

interface CounterCellProps {
  label: string;
  value: number;
  delta?: number;
  colorClass?: string;
  format?: (v: number) => string;
}

function CounterCell({ label, value, delta, colorClass, format }: CounterCellProps) {
  const display = format ? format(value) : value.toLocaleString();
  return (
    <div className={styles.cell}>
      <div className={`${styles.value} ${colorClass ?? ''}`}>
        {display}
        {delta !== undefined && delta > 0 && (
          <span className={styles.delta} title={`+${delta} since last refresh`}>
            ↑{delta}
          </span>
        )}
      </div>
      <div className={styles.label}>{label}</div>
    </div>
  );
}
