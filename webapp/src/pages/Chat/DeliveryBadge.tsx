import type { DeliveryStatus, MessageTier } from '../../types/bramble';
import styles from './DeliveryBadge.module.css';

interface DeliveryBadgeProps {
  status: DeliveryStatus;
  tier: MessageTier;
}

const STATUS_META: Record<DeliveryStatus, { icon: string; label: string; cls: string }> = {
  queued:    { icon: '●',  label: 'Queued',               cls: 'muted'    },
  sending:   { icon: '●',  label: 'Sending…',             cls: 'sending'  },
  sent:      { icon: '✓',  label: 'Sent to node',         cls: 'muted'    },
  delivered: { icon: '✓✓', label: 'Delivered',            cls: 'delivered' },
  failed:    { icon: '✗',  label: 'Failed – no delivery', cls: 'failed'   },
  timeout:   { icon: '!',  label: 'No receipt (timeout)', cls: 'timeout'  },
};

export function DeliveryBadge({ status, tier }: DeliveryBadgeProps) {
  const meta = STATUS_META[status];
  const isCritical = tier === 'critical';

  return (
    <span
      className={[
        styles.badge,
        styles[meta.cls],
        isCritical && status === 'delivered' ? styles.critical : '',
      ].join(' ')}
      title={meta.label}
      aria-label={meta.label}
    >
      {meta.icon}
    </span>
  );
}
