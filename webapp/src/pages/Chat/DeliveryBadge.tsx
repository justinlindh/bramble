import type { DeliveryStatus, MessageTier } from '../../types/bramble';
import styles from './DeliveryBadge.module.css';

interface DeliveryBadgeProps {
  status: DeliveryStatus;
  tier: MessageTier;
}

const STATUS_META: Record<DeliveryStatus, { icon: string; label: string; cls: string }> = {
  queued:    { icon: '●', label: 'Queued',               cls: 'pending'   },
  sending:   { icon: '●', label: 'Sending…',             cls: 'sending'   },
  sent:      { icon: '●', label: 'Sent to next hop',     cls: 'pending'   },
  delivered: { icon: '●', label: 'Delivered',             cls: 'delivered' },
  failed:    { icon: '●', label: 'Failed – not delivered', cls: 'failed'  },
  timeout:   { icon: '●', label: 'No receipt (timeout)',  cls: 'warning'  },
};

export function DeliveryBadge({ status, tier }: DeliveryBadgeProps) {
  const meta = STATUS_META[status];

  return (
    <span
      className={`${styles.badge} ${styles[meta.cls]}`}
      title={meta.label}
      aria-label={meta.label}
    >
      {meta.icon}
    </span>
  );
}
