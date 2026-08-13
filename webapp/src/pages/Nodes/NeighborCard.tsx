import { useState } from 'react';
import type { Neighbor, PeerLocation } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import { NamedAddress } from '../../components/NamedAddress';
import { IconClock, IconEnvelope, IconLocation } from '../../components/Icons';
import { usePeerName } from '../../store/peerName';
import { useAgeTick, formatAge } from '../../hooks/useAgeTick';
import styles from './NeighborCard.module.css';

// ─── Helpers ──────────────────────────────────────────────────────────────────

/** Compute health tier from RSSI, the only link-quality signal a node measures. */
function neighborHealth(n: Neighbor): 'good' | 'fair' | 'poor' {
  if (n.rssi > -90) return 'good';
  if (n.rssi < -110) return 'poor';
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

/**
 * Whether a peer location is recent enough to show as current.
 *
 * The node decides this, not the client: `lastUpdatedMs` is a device UPTIME
 * reading, not a wall-clock instant, so subtracting it from Date.now() is not
 * a duration in any sense. Only the device holds the clock those readings were
 * taken on, and only the device knows whether a stored position predates its
 * current boot (in which case it has no computable age at all). It answers
 * both questions in `online`.
 */
function isLocationFresh(loc: PeerLocation): boolean {
  return loc.online;
}

const STALE_NEIGHBOR_THRESHOLD_MS = 10 * 60 * 1000;

function isNeighborStale(lastHeardMs: number): boolean {
  return lastHeardMs > STALE_NEIGHBOR_THRESHOLD_MS;
}

export function NeighborCard({ neighbor, peerLocation, onOpenDM, onShowOnMap }: NeighborCardProps) {
  const [expanded, setExpanded] = useState(false);
  const peerName = usePeerName(neighbor.addr);
  useAgeTick(); // drives 1s re-renders for live age display
  const health = neighborHealth(neighbor);
  const barPct = rssiBarPct(neighbor.rssi);
  const hasLocation = !!peerLocation;
  const stale = isNeighborStale(neighbor.lastHeardMs);

  return (
    <article
      className={`${styles.card} ${styles[health]} ${stale ? styles.staleCard : ''}`}
      onClick={() => setExpanded((e) => !e)}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          setExpanded((v) => !v);
        }
      }}
      aria-expanded={expanded}
    >
      {/* ── Header row ── */}
      <div className={styles.header}>
        <span className={styles.nameGroup}>
          <NamedAddress addr={neighbor.addr} name={peerName} subClassName={styles.addrSub} />
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
        <span title="Signal-to-Noise Ratio">SNR: {neighbor.snr?.toFixed(1) ?? '-'} dB</span>
        <span title="Last heard"><IconClock size={13} /> {formatAge(neighbor.lastHeardMs)}</span>
        <span
          className={stale ? styles.badgeStale : styles.badgeActive}
          title={stale ? 'Neighbor not heard from in over 10 minutes' : 'Neighbor heard from recently'}
        >
          {stale ? 'Stale' : 'Active'}
        </span>
        {hasLocation ? (
          <button
            className={styles.badgeLocation}
            title={peerLocation && isLocationFresh(peerLocation) ? 'Show location on map' : 'Show last known location on map'}
            onClick={(e) => { e.stopPropagation(); onShowOnMap?.(neighbor.addr); }}
          >
            <IconLocation size={13} /> Show Location: {peerLocation.tier === 'full' ? 'Exact' : peerLocation.tier === 'coarse' ? 'Zone' : 'Present'}
          </button>
        ) : (
          <span className={styles.badgeLocationMuted} title="No location received for this neighbor">
            <IconLocation size={13} /> Location: unavailable
          </span>
        )}
      </div>

      {/* ── Expanded detail ── */}
      {expanded && (
        <div className={styles.detail} onClick={(e) => e.stopPropagation()}>
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
          {peerLocation && isLocationFresh(peerLocation) && peerLocation.position && peerLocation.tier === 'coarse' && (
            <div className={styles.detailRow}>
              <span>Zone:</span>
              {/* Printed to the quantization granularity (a thousandth of a
                  degree), so the row cannot read as a precise fix. */}
              <strong>
                {peerLocation.position.lat.toFixed(3)}, {peerLocation.position.lon.toFixed(3)} (area)
              </strong>
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
