import type { Message } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import { DeliveryBadge } from './DeliveryBadge';
import { RelayPathDisplay } from './RelayPathDisplay';
import { IconCritical, IconBroadcast } from '../../components/Icons';
import styles from './MessageBubble.module.css';

interface MessageBubbleProps {
  message: Message;
  myAddr: number;
}

function formatTime(ms: number): string {
  const d = new Date(ms);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

const TIER_CLASS: Record<string, string> = {
  broadcast: '',
  normal:    '',
  critical:  'tierCritical',
};

export function MessageBubble({ message, myAddr }: MessageBubbleProps) {
  const isOut = message.direction === 'outgoing';
  const tierCls = TIER_CLASS[message.tier] ?? '';

  const hasRelayPath =
    message.tier === 'critical' &&
    message.relayPath &&
    message.relayPath.length > 0;

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
        <AddressLabel addr={message.from} short className={styles.sender} />
      )}

      {/* Message text */}
      <p className={styles.text}>{message.text}</p>

      {/* Relay path (Critical tier with ack path) */}
      {hasRelayPath && (
        <RelayPathDisplay
          path={message.relayPath!}
          myAddr={myAddr}
        />
      )}

      {/* Timestamp + delivery status */}
      <div className={styles.meta}>
        <time className={styles.time} dateTime={new Date(message.timestampMs).toISOString()}>
          {formatTime(message.timestampMs)}
        </time>
        {isOut && (
          <DeliveryBadge status={message.status} tier={message.tier} />
        )}
        {message.tier === 'critical' && (
          <span className={styles.tierTag} title="Critical priority"><IconCritical size={14} /></span>
        )}
        {message.tier === 'broadcast' && (
          <span className={styles.tierTag} title="Broadcast"><IconBroadcast size={14} /></span>
        )}
      </div>
    </div>
  );
}
