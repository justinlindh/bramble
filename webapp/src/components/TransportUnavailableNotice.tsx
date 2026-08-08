import type { TransportUnavailable } from '../lib/transportAvailability';
import styles from './TransportUnavailableNotice.module.css';

/**
 * Occupies the space the selected transport's connection form would have
 * taken. A disabled button with a title attribute says nothing on a touch
 * device, which is why the unavailable options are selectable and explain
 * themselves here instead.
 */
export function TransportUnavailableNotice({ info }: { info: TransportUnavailable }) {
  return (
    <div className={styles.notice} role="note">
      <h4 className={styles.heading}>{info.heading}</h4>
      <p className={styles.body}>{info.body}</p>
      <p className={styles.alternatives}>{info.alternatives}</p>
      {info.cta && (
        <a className={styles.cta} href={info.cta.href} target="_blank" rel="noopener noreferrer">
          {info.cta.label}
        </a>
      )}
    </div>
  );
}
