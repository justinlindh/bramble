import { useStore } from '../store/index';
import styles from './GnssDot.module.css';

/**
 * Header GNSS indicator: a colored dot plus a short count.
 *
 * A single "has fix" boolean cannot separate a receiver hearing nothing
 * (interference, dead antenna, unpowered module) from a receiver hearing
 * satellites and not yet converging, and those two want opposite responses
 * from an operator. The firmware classifies; this only renders. Colour alone
 * would exclude anyone who cannot see it, so every state also spells its
 * counts into the accessible name.
 */

type GnssUiState = 'unknown' | 'no-signal' | 'acquiring' | 'fix';

const STATE_CLASSES: Record<GnssUiState, string> = {
  unknown: styles.unknown,
  'no-signal': styles.noSignal,
  acquiring: styles.acquiring,
  fix: styles.fix,
};

// Short count for the header text. Distinct per state so the three failure
// classes never collapse into the same glance.
function countText(state: GnssUiState, used?: number, tracked?: number): string {
  switch (state) {
    case 'unknown': return '?';
    case 'no-signal': return '--';
    // A receiver with GSV switched off tracks nothing it can report, and the
    // GGA satellite count is then the only number there is.
    case 'acquiring': return `${tracked || used || 0} sats`;
    case 'fix': return `${used ?? 0} sats`;
  }
}

// Counts go in the accessible name, and a count the firmware did not report is
// omitted rather than printed as zero.
function accessibleName(
  state: GnssUiState,
  inView?: number,
  tracked?: number,
  used?: number,
  snr?: number,
): string {
  switch (state) {
    case 'unknown':
      return 'GNSS state unknown (firmware does not report it)';
    case 'no-signal':
      return inView === undefined
        ? 'GNSS no signal'
        : `GNSS no signal: 0 tracked of ${inView} in view`;
    case 'acquiring': {
      const clauses: string[] = [];
      if (tracked !== undefined && inView !== undefined) clauses.push(`${tracked} tracked of ${inView} in view`);
      else if (tracked !== undefined) clauses.push(`${tracked} tracked`);
      else if (inView !== undefined) clauses.push(`${inView} in view`);
      if (snr !== undefined && snr > 0) clauses.push(`best ${snr} dBHz`);
      return clauses.length > 0 ? `GNSS acquiring: ${clauses.join(', ')}` : 'GNSS acquiring';
    }
    case 'fix': {
      if (used === undefined) return 'GNSS fix';
      return inView === undefined
        ? `GNSS fix: ${used} satellites used`
        : `GNSS fix: ${used} satellites used of ${inView} in view`;
    }
  }
}

export function GnssDot() {
  const connectionState = useStore(s => s.connectionState);
  const status = useStore(s => s.status);

  if (connectionState !== 'connected' || !status) return null;
  // A board with no receiver has nothing to report, and gps_available is the
  // only field that says so: the counts are zero on such a board and must
  // never be read as a failure.
  if (status.gpsAvailable === false) return null;
  if (status.gpsState === 'absent') return null;

  const state: GnssUiState =
    status.gpsState === 'no_signal' ? 'no-signal'
    : status.gpsState === 'acquiring' ? 'acquiring'
    : status.gpsState === 'fix' ? 'fix'
    : 'unknown';

  const label = accessibleName(
    state,
    status.gpsSatsInView,
    status.gpsSatsTracked,
    status.gpsSatsUsed,
    status.gpsSnrMaxDbHz,
  );

  return (
    <span className={styles.group}>
      <span className={styles.divider} aria-hidden="true">&bull;</span>
      <span className={`${styles.dot} ${STATE_CLASSES[state]}`} title={label} aria-label={label} />
      <span className={styles.count} aria-hidden="true">
        {countText(state, status.gpsSatsUsed, status.gpsSatsTracked)}
      </span>
    </span>
  );
}
