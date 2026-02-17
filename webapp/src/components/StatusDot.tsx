import type { ConnectionState } from '../types/bramble';
import styles from './StatusDot.module.css';

interface StatusDotProps {
  state: ConnectionState;
}

const STATE_LABELS: Record<ConnectionState, string> = {
  disconnected: 'Disconnected',
  connecting: 'Connecting…',
  connected: 'Connected',
  error: 'Error',
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
