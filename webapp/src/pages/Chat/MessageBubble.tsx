import { useState } from 'react';
import type { Message } from '../../types/bramble';
import { DeliveryBadge } from './DeliveryBadge';
import { RelayPathDisplay } from './RelayPathDisplay';
import { BroadcastDeliveryPanel } from './BroadcastDeliveryPanel';
import { IconCritical, IconBroadcast } from '../../components/Icons';
import { useStore } from '../../store/index';
import { usePeerInfo } from '../../hooks/usePeer';
import { parseAction } from '../../utils/parseAction';
import { formatMessageTimestamp } from './chatDateFormatting';
import styles from './MessageBubble.module.css';

interface MessageBubbleProps {
  message: Message;
}

const TIER_CLASS: Record<string, string> = {
  broadcast: '',
  normal:    '',
  critical:  'tierCritical',
};

export function MessageBubble({ message }: MessageBubbleProps) {
  const isOut = message.direction === 'outgoing';
  const tierCls = TIER_CLASS[message.tier] ?? '';
  const showRoutesGlobal = useStore(s => s.showRoutes);
  const [routeExpanded, setRouteExpanded] = useState(false);
  const [deliveryExpanded, setDeliveryExpanded] = useState(false);

  const hasRelayPath =
    message.relayPath &&
    message.relayPath.length > 0;
  const isOutgoingBroadcast = isOut && message.to === 0xFFFFFFFF;
  const recipientCount = message.broadcastRecipients?.length ?? 0;

  const { displayName, fullHex } = usePeerInfo(message.from);
  const showPath = hasRelayPath && (showRoutesGlobal || routeExpanded);

  const { isAction, actionText } = parseAction(message.text);

  // Action messages render as a full-width IRC-style line (* Nick text)
  if (isAction) {
    const actorName = isOut ? 'me' : displayName;
    return (
      <div className={styles.actionLine}>
        <span className={styles.actionText}>
          <span className={styles.actionAsterisk}>*</span>{' '}
          <span className={styles.actionActor}>{actorName}</span>{' '}
          {actionText}
        </span>
        <time className={styles.actionTime} dateTime={new Date(message.timestampMs).toISOString()}>
          {formatMessageTimestamp(message.timestampMs)}
        </time>
      </div>
    );
  }

  return (
    <div
      className={[
        styles.bubble,
        isOut ? styles.outgoing : styles.incoming,
        tierCls ? styles[tierCls as keyof typeof styles] : '',
      ].join(' ')}
    >
      {/* Sender label for incoming messages */}
      {!isOut && (
        <span className={styles.sender} title={fullHex}>{displayName}</span>
      )}

      {/* Message text */}
      <p className={styles.text}>{message.text}</p>

      {/* Relay path (shown when global toggle on OR individually expanded) */}
      {showPath && (
        <RelayPathDisplay path={message.relayPath!} />
      )}

      {/* Timestamp + delivery status */}
      <div className={styles.meta}>
        <time className={styles.time} dateTime={new Date(message.timestampMs).toISOString()}>
          {formatMessageTimestamp(message.timestampMs)}
        </time>
        {isOut && (
          <DeliveryBadge
            status={message.status}
            broadcastRecipientCount={isOutgoingBroadcast ? recipientCount : undefined}
          />
        )}
        {isOutgoingBroadcast && (
          <button
            className={`${styles.deliveryToggle} ${deliveryExpanded ? styles.deliveryToggleActive : ''}`}
            onClick={(e) => { e.stopPropagation(); setDeliveryExpanded(v => !v); }}
            title={deliveryExpanded ? 'Hide delivery details' : 'Show delivery details'}
            aria-label={deliveryExpanded ? 'Hide delivery details' : 'Show delivery details'}
          >
            Delivery {recipientCount > 0 ? `(${recipientCount})` : ''}
          </button>
        )}
        {isOut && hasRelayPath && (
          <button
            className={`${styles.routeToggle} ${routeExpanded ? styles.routeToggleActive : ''}`}
            onClick={(e) => { e.stopPropagation(); setRouteExpanded(v => !v); }}
            title={routeExpanded ? 'Hide route' : 'Show route'}
            aria-label={routeExpanded ? 'Hide route' : 'Show route'}
          >
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <circle cx="5" cy="6" r="2" /><circle cx="19" cy="18" r="2" /><path d="M7 6h6a4 4 0 0 1 4 4v2a4 4 0 0 1-4 4H5" />
            </svg>
          </button>
        )}
        {message.tier === 'critical' && (
          <span className={styles.tierTag} title="Critical priority"><IconCritical size={14} /></span>
        )}
        {message.to === 0xFFFFFFFF && (
          <span
            data-testid="broadcast-meta-icon"
            className={`${styles.tierTag} ${styles.broadcastTierTag}`}
            title="Broadcast"
          >
            <IconBroadcast size={14} />
          </span>
        )}
      </div>
      {isOutgoingBroadcast && deliveryExpanded && (
        <BroadcastDeliveryPanel recipients={message.broadcastRecipients} />
      )}
    </div>
  );
}
