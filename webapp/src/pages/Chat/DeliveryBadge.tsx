import type { DeliveryStatus } from '../../types/bramble';
import styles from './DeliveryBadge.module.css';

interface DeliveryBadgeProps {
  status: DeliveryStatus;
  broadcastRecipientCount?: number;
}

const STATUS_META: Record<DeliveryStatus, { label: string; cls: string }> = {
  queued:    { label: 'Queued',                 cls: 'pending'   },
  sending:   { label: 'Sending…',               cls: 'sending'   },
  sent:      { label: 'Sent to next hop',       cls: 'pending'   },
  delivered: { label: 'Delivered',              cls: 'delivered' },
  failed:    { label: 'Failed – not delivered', cls: 'failed'    },
  timeout:   { label: 'No confirmation yet',    cls: 'warning'   },
  parked:    { label: 'Parked, peer offline',   cls: 'pending'   },
};

export function DeliveryBadge({ status, broadcastRecipientCount }: DeliveryBadgeProps) {
  const meta = STATUS_META[status];

  return (
    <span className={styles.badgeWrap}>
      {/* Clearer sent-status indicator dot shown next to message timestamp. */}
      <span
        className={`${styles.sentStatusIndicator} ${styles[meta.cls]}`}
        title={meta.label}
        aria-label={meta.label}
        role="img"
      />
      {typeof broadcastRecipientCount === 'number' && (
        <span className={styles.recipientCount} title="Recipients with telemetry">
          {broadcastRecipientCount}
        </span>
      )}
    </span>
  );
}
