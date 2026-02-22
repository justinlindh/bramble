import type { DeliveryStatus, MessageTier } from '../../types/bramble';
import styles from './DeliveryBadge.module.css';

interface DeliveryBadgeProps {
  status: DeliveryStatus;
  tier: MessageTier;
}

const STATUS_META: Record<DeliveryStatus, { label: string; cls: string }> = {
  queued:    { label: 'Queued',                 cls: 'pending'   },
  sending:   { label: 'Sending…',               cls: 'sending'   },
  sent:      { label: 'Sent to next hop',       cls: 'pending'   },
  delivered: { label: 'Delivered',              cls: 'delivered' },
  failed:    { label: 'Failed – not delivered', cls: 'failed'    },
  timeout:   { label: 'No receipt (timeout)',   cls: 'warning'   },
};

export function DeliveryBadge({ status, tier: _tier }: DeliveryBadgeProps) {
  const meta = STATUS_META[status];

  return (
    // Clearer sent-status indicator dot shown next to message timestamp.
    <span
      className={`${styles.sentStatusIndicator} ${styles[meta.cls]}`}
      title={meta.label}
      aria-label={meta.label}
      role="img"
    />
  );
}
