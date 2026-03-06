import { useState } from 'react';
import type { Conversation } from '../../types/bramble';
import { IconBroadcast, IconHash, IconUser, IconPlus, IconLock } from '../../components/Icons';
import { usePeerInfo, STATUS_COLORS } from '../../hooks/usePeer';
import { addChannel } from '../../store/actions';
import { useStore } from '../../store/index';
import styles from './ConversationList.module.css';

interface ConversationListProps {
  conversations: Map<string, Conversation>;
  activeId: string;
  onSelect: (id: string) => void;
}

// Broadcast address constant
const BROADCAST_ADDR = 0xffffffff;

type ChannelItem = {
  id: string;
  label: string;
  unreadCount: number;
  hasPsk: boolean;
};

export function buildChannelItems(config: any, conversations: Map<string, Conversation>): ChannelItem[] {
  return (config?.channels ?? [])
    .filter((ch: any) => ch.index > 0)
    .map((ch: any): ChannelItem => {
      const id = `ch:${ch.index}`;
      const existing = conversations.get(id);
      const rawName = typeof ch.name === 'string' ? ch.name : '';
      const trimmedName = rawName.trim();
      return {
        id,
        label: trimmedName.length > 0 ? trimmedName : `ch-${ch.index}`,
        unreadCount: existing?.unreadCount ?? 0,
        hasPsk: Boolean(ch.hasPsk),
      };
    });
}

export function filterDmConversations(conversations: Map<string, Conversation>, knownPeerAddrs?: Set<number>) {
  const hasKnownPeers = (knownPeerAddrs?.size ?? 0) > 0;
  return [...conversations.values()].filter(c => {
    if (!c.id.startsWith('dm:')) return false;
    const addr = c.peerAddr ?? parseInt(c.id.slice(3), 10);
    if (addr === BROADCAST_ADDR) return false;
    if (!hasKnownPeers) return true;
    return knownPeerAddrs!.has(addr);
  });
}

export function parseDmHexAddress(input: string): number | null {
  const raw = input.trim().replace(/^0x/i, '');
  if (!/^[0-9a-fA-F]{1,8}$/.test(raw)) return null;
  return parseInt(raw, 16);
}

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

  const [showChannelDialog, setShowChannelDialog] = useState(false);
  const [channelName, setChannelName] = useState('');
  const [channelPsk, setChannelPsk] = useState('');
  const [channelError, setChannelError] = useState('');
  const [isCreating, setIsCreating] = useState(false);

  const config = useStore(s => s.config);
  const neighbors = useStore(s => s.neighbors);
  const routes = useStore(s => s.routes);
  const peerLocations = useStore(s => s.peerLocations);

  const knownPeerAddrs = new Set<number>();
  for (const n of neighbors) knownPeerAddrs.add(n.addr);
  for (const r of routes) {
    if (r.dest !== BROADCAST_ADDR) knownPeerAddrs.add(r.dest);
    if (r.nextHop !== BROADCAST_ADDR) knownPeerAddrs.add(r.nextHop);
  }
  for (const p of peerLocations) knownPeerAddrs.add(p.addr);

  // Separate out channels and DMs
  // Broadcasts are filed under 'broadcast' in the map, never as 'dm:0xFFFFFFFF'
  const broadcastUnread = conversations.get('broadcast')?.unreadCount ?? 0;

  // Show channels from config even before first message arrives.
  const channels = buildChannelItems(config, conversations);

  const dms = filterDmConversations(conversations, knownPeerAddrs);

  const handleOpenDm = () => {
    setDmError('');
    const addr = parseDmHexAddress(dmAddr);
    if (addr === null) {
      setDmError('Enter a valid hex address (e.g. 0xABCD1234)');
      return;
    }
    onSelect(`dm:${addr}`);
    setShowDmDialog(false);
    setDmAddr('');
    setDmError('');
  };

  const handleCreateChannel = async () => {
    setChannelError('');
    const name = channelName.trim();
    if (!name) {
      setChannelError('Channel name is required');
      return;
    }
    if (name.length > 16) {
      setChannelError('Channel name must be 16 characters or less');
      return;
    }
    setIsCreating(true);
    try {
      const newChannelIndex = await addChannel(name, channelPsk.trim() || undefined);
      // Success! Auto-switch to the new channel
      onSelect(`ch:${newChannelIndex}`);
      setShowChannelDialog(false);
      setChannelName('');
      setChannelPsk('');
      setChannelError('');
    } catch (err) {
      setChannelError((err as Error).message);
    } finally {
      setIsCreating(false);
    }
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
      <div className={styles.dmHeader}>
        <p className={styles.groupHeader}>Channels</p>
        <button
          className={styles.addBtn}
          onClick={() => { setShowChannelDialog(true); setChannelError(''); setChannelName(''); setChannelPsk(''); }}
          title="Create new channel"
          aria-label="New channel"
        >
          <IconPlus size={16} />
        </button>
      </div>

      {channels.map(conv => (
        <button
          key={conv.id}
          className={`${styles.item} ${activeId === conv.id ? styles.active : ''}`}
          onClick={() => onSelect(conv.id)}
        >
          <span className={styles.icon}><IconHash size={16} /></span>
          <span className={styles.label}>{conv.label}</span>
          {conv.hasPsk && (
            <span title="PSK protected" aria-label="PSK protected">
              <IconLock size={12} />
            </span>
          )}
          {conv.unreadCount > 0 && (
            <span className={styles.badge}>{conv.unreadCount}</span>
          )}
        </button>
      ))}

      {channels.length === 0 && (
        <p className={styles.dmEmpty}>No channels yet</p>
      )}

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

      {/* ── New Channel Dialog ────────────────────── */}
      {showChannelDialog && (
        <div className={styles.dialogBackdrop} onClick={() => setShowChannelDialog(false)}>
          <div className={styles.dialog} onClick={e => e.stopPropagation()}>
            <h3 className={styles.dialogTitle}>Create Channel</h3>
            <p className={styles.dialogDesc}>Enter channel details:</p>
            <input
              className={styles.dialogInput}
              value={channelName}
              onChange={e => { setChannelName(e.target.value); setChannelError(''); }}
              placeholder="Channel name"
              maxLength={16}
              onKeyDown={e => e.key === 'Enter' && !e.shiftKey && handleCreateChannel()}
              autoFocus
            />
            <input
              className={styles.dialogInput}
              type="password"
              value={channelPsk}
              onChange={e => { setChannelPsk(e.target.value); setChannelError(''); }}
              placeholder="PSK (optional)"
              onKeyDown={e => e.key === 'Enter' && handleCreateChannel()}
              autoComplete="new-password"
            />
            {channelError && <p className={styles.dialogError}>{channelError}</p>}
            <div className={styles.dialogBtns}>
              <button
                className={styles.dialogOpen}
                onClick={handleCreateChannel}
                disabled={isCreating || !channelName.trim()}
              >
                {isCreating ? 'Creating…' : 'Create'}
              </button>
              <button className={styles.dialogCancel} onClick={() => setShowChannelDialog(false)}>
                Cancel
              </button>
            </div>
          </div>
        </div>
      )}
    </aside>
  );
}
