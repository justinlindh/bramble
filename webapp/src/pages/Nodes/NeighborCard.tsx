import { useState } from 'react';
import type { Neighbor, PeerLocation } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import { IconClock, IconEnvelope, IconLocation } from '../../components/Icons';
import { useStore } from '../../store';
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
  peerLocation?: PeerLocation;
  onOpenDM?: (addr: number) => void;
  onShowOnMap?: (addr: number) => void;
}

/** Check if a peer location is still fresh (not older than 30 minutes) */
function isLocationFresh(loc: PeerLocation): boolean {
  return Date.now() - loc.lastUpdatedMs < 30 * 60 * 1000;
}

const STALE_NEIGHBOR_THRESHOLD_MS = 10 * 60 * 1000;

function isNeighborStale(lastHeardMs: number): boolean {
  return lastHeardMs > STALE_NEIGHBOR_THRESHOLD_MS;
}

export function NeighborCard({ neighbor, peerLocation, onOpenDM, onShowOnMap }: NeighborCardProps) {
  const [expanded, setExpanded] = useState(false);
  const peerName = useStore(s => s.peerNames.get(neighbor.addr));
  const health = neighborHealth(neighbor);
  const pdr = pdrPercent(neighbor.deliveryRate);
  const barPct = rssiBarPct(neighbor.rssi);
  const hasFreshLocation = !!peerLocation && isLocationFresh(peerLocation);
  const stale = isNeighborStale(neighbor.lastHeardMs);

  return (
    <article
      className={`${styles.card} ${styles[health]} ${stale ? styles.staleCard : ''}`}
      onClick={() => setExpanded((e) => !e)}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => e.key === 'Enter' && setExpanded((v) => !v)}
      aria-expanded={expanded}
    >
      {/* ── Header row ── */}
      <div className={styles.header}>
        <span className={styles.nameGroup}>
          <AddressLabel addr={neighbor.addr} name={peerName} short={!peerName} />
          {peerName && (
            <span className={styles.addrSub}>
              0x{neighbor.addr.toString(16).toUpperCase().padStart(8, '0').slice(-4)}
            </span>
          )}
        </span>
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
        <span title="Signal-to-Noise Ratio">SNR: {neighbor.snr?.toFixed(1)} dB</span>
        <span title="Last heard"><IconClock size={13} /> {formatAgo(neighbor.lastHeardMs)}</span>
        <span
          className={stale ? styles.badgeStale : styles.badgeActive}
          title={stale ? 'Neighbor not heard from in over 10 minutes' : 'Neighbor heard from recently'}
        >
          {stale ? 'Stale' : 'Active'}
        </span>
        {hasFreshLocation ? (
          <button
            className={styles.badgeLocation}
            title="Show location on map"
            onClick={(e) => { e.stopPropagation(); onShowOnMap?.(neighbor.addr); }}
          >
            <IconLocation size={13} /> Show Location: {peerLocation.tier === 'full' ? 'Exact' : peerLocation.tier === 'coarse' ? 'Zone' : 'Present'}
          </button>
        ) : (
          <span className={styles.badgeLocationMuted} title="No recent location received for this neighbor">
            <IconLocation size={13} /> Location: unavailable
          </span>
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
          {peerLocation && isLocationFresh(peerLocation) && peerLocation.position && peerLocation.tier === 'full' && (
            <div className={styles.detailRow}>
              <span>Coordinates:</span>
              <strong>{peerLocation.position.lat.toFixed(6)}, {peerLocation.position.lon.toFixed(6)}</strong>
            </div>
          )}
          {peerLocation && isLocationFresh(peerLocation) && peerLocation.gridSquare && (
            <div className={styles.detailRow}>
              <span>Grid square:</span>
              <strong>{peerLocation.gridSquare}</strong>
            </div>
          )}
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
