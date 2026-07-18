import type { BroadcastDeliveryRecipient } from '../../types/bramble';
import { formatAddr0x } from '../../utils/address';
import styles from './BroadcastDeliveryPanel.module.css';

interface BroadcastDeliveryPanelProps {
  recipients?: BroadcastDeliveryRecipient[];
}

export function BroadcastDeliveryPanel({ recipients }: BroadcastDeliveryPanelProps) {
  const items = recipients ?? [];
  const delivered = items.filter((r) => r.status === 'delivered').length;
  const pending = items.filter((r) => r.status === 'pending').length;
  const failed = items.filter((r) => r.status === 'failed').length;

  return (
    <div className={styles.panel} aria-label="Delivery telemetry panel">
      <div className={styles.title}>Delivery telemetry</div>

      {items.length === 0 ? (
        <p className={styles.empty}>No delivery telemetry yet.</p>
      ) : (
        <>
          <div className={styles.summary}>{items.length} recipients</div>
          <div className={styles.chips}>
            <span className={`${styles.chip} ${styles.delivered}`}>Delivered {delivered}</span>
            <span className={`${styles.chip} ${styles.pending}`}>Pending {pending}</span>
            <span className={`${styles.chip} ${styles.failed}`}>Failed {failed}</span>
          </div>
          <ul className={styles.list}>
            {items.map((recipient) => (
              <li key={recipient.addr} className={styles.row}>
                <span>{formatAddr0x(recipient.addr)}</span>
                <span className={
                  recipient.status === 'delivered'
                    ? styles.deliveredText
                    : recipient.status === 'pending'
                      ? styles.pendingText
                      : styles.failedText
                }>
                  {recipient.status}
                </span>
              </li>
            ))}
          </ul>
        </>
      )}
    </div>
  );
}
