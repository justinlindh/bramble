import type { ConnectionState } from '../types/bramble';
import styles from './StatusDot.module.css';

interface StatusDotProps {
  state: ConnectionState;
}

// The single connection-state label map: the dot's title/aria-label and the
// status pill text in App render side by side, and two parallel maps drifted
// ('Error' next to 'Reconnecting…'). 'error' means the app is
// auto-reconnecting, so the label says that.
export const STATE_LABELS: Record<ConnectionState, string> = {
  disconnected: 'Disconnected',
  connecting: 'Connecting…',
  connected: 'Connected',
  error: 'Reconnecting…',
};

export function StatusDot({ state }: StatusDotProps) {
  return (
    <span
      className={`${styles.dot} ${styles[state]}`}
      title={STATE_LABELS[state]}
      aria-label={STATE_LABELS[state]}
    />
  );
}
