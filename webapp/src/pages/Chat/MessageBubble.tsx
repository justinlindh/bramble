import { useState } from 'react';
import type { Message } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import { DeliveryBadge } from './DeliveryBadge';
import { RelayPathDisplay } from './RelayPathDisplay';
import { IconCritical, IconBroadcast } from '../../components/Icons';
import { useStore } from '../../store/index';
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
  const showRoutesGlobal = useStore(s => s.showRoutes);
  const [expanded, setExpanded] = useState(false);

  const hasRelayPath =
    message.relayPath &&
    message.relayPath.length > 0;

  const showPath = hasRelayPath && (showRoutesGlobal || expanded);

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

      {/* Relay path (shown when global toggle on OR individually expanded) */}
      {showPath && (
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
        {isOut && hasRelayPath && (
          <span
            className={styles.routeToggle}
            onClick={(e) => { e.stopPropagation(); setExpanded(v => !v); }}
            title={expanded ? 'Hide route' : 'Show route'}
            role="button"
            aria-label={expanded ? 'Hide route' : 'Show route'}
          >
            {expanded ? '▾' : '▸'}
          </span>
        )}
        {isOut && (
          <DeliveryBadge status={message.status} tier={message.tier} />
        )}
        {message.tier === 'critical' && (
          <span className={styles.tierTag} title="Critical priority"><IconCritical size={14} /></span>
        )}
        {message.to === 0xFFFFFFFF && (
          <span className={styles.tierTag} title="Broadcast"><IconBroadcast size={14} /></span>
        )}
      </div>
    </div>
  );
}
