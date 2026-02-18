import { useState } from 'react';
import type { Conversation } from '../../types/bramble';
import { IconBroadcast, IconHash, IconUser, IconPlus } from '../../components/Icons';
import { usePeerInfo, STATUS_COLORS } from '../../hooks/usePeer';
import styles from './ConversationList.module.css';

interface ConversationListProps {
  conversations: Map<string, Conversation>;
  activeId: string;
  onSelect: (id: string) => void;
}

// Broadcast address constant
const BROADCAST_ADDR = 0xffffffff;

function DmItem({ conv, active, onSelect }: { conv: Conversation; active: boolean; onSelect: (id: string) => void }) {
  const addr = conv.peerAddr ?? parseInt(conv.id.slice(3), 10);
  const { displayName, fullHex, status } = usePeerInfo(addr);
  return (
    <button
      className={`${styles.item} ${active ? styles.active : ''}`}
      onClick={() => onSelect(conv.id)}
      title={fullHex}
    >
      <span className={styles.icon}><IconUser size={16} /></span>
      <span className={styles.label}>{displayName}</span>
      <span
        className={styles.statusDot}
        style={{ background: STATUS_COLORS[status] }}
        title={status === 'online' ? 'Online' : status === 'reachable' ? 'Reachable' : 'Unknown'}
      />
      {conv.unreadCount > 0 && (
        <span className={styles.badge}>{conv.unreadCount}</span>
      )}
    </button>
  );
}

export function ConversationList({ conversations, activeId, onSelect }: ConversationListProps) {
  const [showDmDialog, setShowDmDialog] = useState(false);
  const [dmAddr, setDmAddr] = useState('');
  const [dmError, setDmError] = useState('');

  // Separate out channels and DMs
  // Broadcasts are filed under 'broadcast' in the map, never as 'dm:0xFFFFFFFF'
  const broadcastUnread = conversations.get('broadcast')?.unreadCount ?? 0;
  const channels = [...conversations.values()].filter(c => c.id.startsWith('ch:'));
  const dms = [...conversations.values()].filter(
    c => c.id.startsWith('dm:') && parseInt(c.id.slice(3), 10) !== BROADCAST_ADDR
  );

  const handleOpenDm = () => {
    setDmError('');
    const raw = dmAddr.trim().replace(/^0x/i, '');
    if (!/^[0-9a-fA-F]{1,8}$/.test(raw)) {
      setDmError('Enter a valid hex address (e.g. 0xABCD1234)');
      return;
    }
    const addr = parseInt(raw, 16);
    onSelect(`dm:${addr}`);
    setShowDmDialog(false);
    setDmAddr('');
    setDmError('');
  };

  return (
    <aside className={styles.sidebar}>
      {/* ── Broadcast ────────────────────────────── */}
      <button
        className={`${styles.item} ${activeId === 'broadcast' ? styles.active : ''}`}
        onClick={() => onSelect('broadcast')}
      >
        <span className={styles.icon}><IconBroadcast size={16} /></span>
        <span className={styles.label}>Broadcast</span>
        {broadcastUnread > 0 && (
          <span className={styles.badge}>{broadcastUnread}</span>
        )}
      </button>

      {/* ── Channels ─────────────────────────────── */}
      {channels.length > 0 && (
        <p className={styles.groupHeader}>Channels</p>
      )}
      {channels.map(conv => (
        <button
          key={conv.id}
          className={`${styles.item} ${activeId === conv.id ? styles.active : ''}`}
          onClick={() => onSelect(conv.id)}
        >
          <span className={styles.icon}><IconHash size={16} /></span>
          <span className={styles.label}>{conv.label}</span>
          {conv.unreadCount > 0 && (
            <span className={styles.badge}>{conv.unreadCount}</span>
          )}
        </button>
      ))}

      {/* ── Direct Messages ───────────────────────── */}
      <div className={styles.dmHeader}>
        <p className={styles.groupHeader}>Direct Messages</p>
        <button
          className={styles.addBtn}
          onClick={() => { setShowDmDialog(true); setDmError(''); setDmAddr(''); }}
          title="Open new DM"
          aria-label="New direct message"
        >
          <IconPlus size={16} />
        </button>
      </div>

      {dms.map(conv => (
        <DmItem key={conv.id} conv={conv} active={activeId === conv.id} onSelect={onSelect} />
      ))}

      {dms.length === 0 && (
        <p className={styles.dmEmpty}>No conversations yet</p>
      )}

      {/* ── New DM Dialog ─────────────────────────── */}
      {showDmDialog && (
        <div className={styles.dialogBackdrop} onClick={() => setShowDmDialog(false)}>
          <div className={styles.dialog} onClick={e => e.stopPropagation()}>
            <h3 className={styles.dialogTitle}>New Direct Message</h3>
            <p className={styles.dialogDesc}>Enter the node's hex address:</p>
            <input
              className={styles.dialogInput}
              value={dmAddr}
              onChange={e => { setDmAddr(e.target.value); setDmError(''); }}
              placeholder="0xABCD1234"
              onKeyDown={e => e.key === 'Enter' && handleOpenDm()}
              autoFocus
            />
            {dmError && <p className={styles.dialogError}>{dmError}</p>}
            <div className={styles.dialogBtns}>
              <button className={styles.dialogOpen} onClick={handleOpenDm}>Open</button>
              <button className={styles.dialogCancel} onClick={() => setShowDmDialog(false)}>
                Cancel
              </button>
            </div>
          </div>
        </div>
      )}
    </aside>
  );
}
