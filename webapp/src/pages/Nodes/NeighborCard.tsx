import { useState } from 'react';
import type { Neighbor } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import { IconClock, IconMailbox, IconEnvelope } from '../../components/Icons';
import styles from './NeighborCard.module.css';

// ─── Helpers ──────────────────────────────────────────────────────────────────

/** Convert delivery rate (0-255) to percentage */
function pdrPercent(deliveryRate: number): number {
  return Math.round((deliveryRate / 255) * 100);
}

/** Human-readable "time ago" string */
function formatAgo(ms: number): string {
  if (ms < 1000) return 'just now';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ago`;
  return `${Math.floor(m / 60)}h ago`;
}

/** Compute health tier based on PDR and RSSI */
function neighborHealth(n: Neighbor): 'good' | 'fair' | 'poor' {
  const pdr = pdrPercent(n.deliveryRate);
  if (pdr > 90 && n.rssi > -90) return 'good';
  if (pdr < 70 || n.rssi < -110) return 'poor';
  return 'fair';
}

/**
 * Map RSSI (-130 .. -50 dBm) to a 0-100% bar width.
 * Clamps outside bounds.
 */
function rssiBarPct(rssi: number): number {
  const min = -130;
  const max = -50;
  return Math.min(100, Math.max(0, ((rssi - min) / (max - min)) * 100));
}

// ─── Component ────────────────────────────────────────────────────────────────

interface NeighborCardProps {
  neighbor: Neighbor;
  onOpenDM?: (addr: number) => void;
}

export function NeighborCard({ neighbor, onOpenDM }: NeighborCardProps) {
  const [expanded, setExpanded] = useState(false);
  const health = neighborHealth(neighbor);
  const pdr = pdrPercent(neighbor.deliveryRate);
  const barPct = rssiBarPct(neighbor.rssi);

  return (
    <article
      className={`${styles.card} ${styles[health]}`}
      onClick={() => setExpanded((e) => !e)}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => e.key === 'Enter' && setExpanded((v) => !v)}
      aria-expanded={expanded}
    >
      {/* ── Header row ── */}
      <div className={styles.header}>
        <AddressLabel addr={neighbor.addr} short />
        <span className={styles.rssi} title="Received Signal Strength Indicator">
          {neighbor.rssi} dBm
        </span>
      </div>

      {/* ── RSSI bar ── */}
      <div className={styles.rssiBar} aria-hidden>
        <div
          className={`${styles.rssiBarFill} ${styles[`bar${health.charAt(0).toUpperCase() + health.slice(1)}` as 'barGood' | 'barFair' | 'barPoor']}`}
          style={{ width: `${barPct}%` }}
        />
      </div>

      {/* ── Stats row ── */}
      <div className={styles.row}>
        <span title="Packet Delivery Rate">PDR: {pdr}%</span>
        <span title="Signal-to-Noise Ratio">SNR: {neighbor.snr} dB</span>
        <span title="Last heard"><IconClock size={13} /> {formatAgo(neighbor.lastHeardMs)}</span>
        {neighbor.isMailbox && (
          <span className={styles.badgeMailbox} title="This node stores messages for offline destinations and delivers them when they come back in range"><IconMailbox size={13} /> Mailbox</span>
        )}
      </div>

      {/* ── Expanded detail ── */}
      {expanded && (
        <div className={styles.detail} onClick={(e) => e.stopPropagation()}>
          <div className={styles.detailRow}>
            <span>Airtime remaining:</span>
            <strong>{neighbor.airtimeRemaining}%</strong>
          </div>
          <div className={styles.detailRow}>
            <span>Full address:</span>
            <AddressLabel addr={neighbor.addr} />
          </div>
          {onOpenDM && (
            <button
              className={styles.dmBtn}
              onClick={() => onOpenDM(neighbor.addr)}
            >
              <IconEnvelope size={14} /> Send DM
            </button>
          )}
        </div>
      )}
    </article>
  );
}
