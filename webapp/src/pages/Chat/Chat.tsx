import { useEffect, useRef, useState, useCallback } from 'react';
import type { ReactNode } from 'react';
import { useStore } from '../../store/index';
import { useConversation, useMyAddress } from '../../store/selectors';
import { IconChat, IconBroadcast, IconHash } from '../../components/Icons';
import { usePeerInfo, STATUS_COLORS } from '../../hooks/usePeer';
import { ConversationList } from './ConversationList';
import { MessageBubble } from './MessageBubble';
import { ComposeBar } from './ComposeBar';
import { ChannelDetailPanel } from './ChannelDetailPanel';
import { formatDaySeparatorLabel, shouldInsertDaySeparator } from './chatDateFormatting';
import styles from './Chat.module.css';

export function isNearBottom(el: { scrollTop: number; clientHeight: number; scrollHeight: number }, threshold = 100): boolean {
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

// ─── Empty state ──────────────────────────────────────────────────────────────

export function getEmptyHint(convId: string): string {
  return convId === 'broadcast'
    ? 'Broadcast messages will appear here. All nodes in range will receive them.'
    : convId.startsWith('ch:')
    ? 'Channel messages will appear here.'
    : 'Send a message to start this conversation.';
}

function EmptyMessages({ convId }: { convId: string }) {
  const hint = getEmptyHint(convId);

  return (
    <div className={styles.emptyPane}>
      <span className={styles.emptyIcon}><IconChat size={32} /></span>
      <p className={styles.emptyHint}>{hint}</p>
    </div>
  );
}

// ─── Message list ─────────────────────────────────────────────────────────────

function MessageList({ conversationId }: { conversationId: string }) {
  const { messages } = useConversation(conversationId);
  const myAddr = useMyAddress();
  const listRef = useRef<HTMLDivElement>(null);
  const [nearBottom, setNearBottom] = useState(true);
  const [showJump, setShowJump] = useState(false);

  const updateScrollState = useCallback(() => {
    const el = listRef.current;
    if (!el) return;
    const atBottom = isNearBottom(el);
    setNearBottom(atBottom);
    if (atBottom) setShowJump(false);
  }, []);

  const scrollToBottom = useCallback(() => {
    const el = listRef.current;
    if (!el) return;
    el.scrollTop = el.scrollHeight;
    setNearBottom(true);
    setShowJump(false);
  }, []);

  // Auto-scroll only when user is near bottom.
  useEffect(() => {
    const el = listRef.current;
    if (!el) return;

    requestAnimationFrame(() => {
      if (nearBottom) {
        el.scrollTop = el.scrollHeight;
        setShowJump(false);
      } else {
        setShowJump(true);
      }
    });
  }, [messages.length, messages[messages.length - 1]?.status, nearBottom]);

  if (messages.length === 0) {
    return <EmptyMessages convId={conversationId} />;
  }

  return (
    <div className={styles.messageListWrap}>
      <div
        ref={listRef}
        className={styles.messageList}
        aria-live="polite"
        aria-label="Messages"
        onScroll={updateScrollState}
      >
        {messages.map((msg, index) => {
          const previousTs = index > 0 ? messages[index - 1].timestampMs : undefined;
          const showDaySeparator = shouldInsertDaySeparator(previousTs, msg.timestampMs);

          return (
            <div key={msg.id}>
              {showDaySeparator && (
                <div className={styles.daySeparator} role="separator" aria-label={`Messages from ${formatDaySeparatorLabel(msg.timestampMs)}`}>
                  <span className={styles.daySeparatorText}>{formatDaySeparatorLabel(msg.timestampMs)}</span>
                </div>
              )}
              <MessageBubble message={msg} myAddr={myAddr} />
            </div>
          );
        })}
      </div>

      {showJump && (
        <button className={styles.jumpToLatestBtn} onClick={scrollToBottom} aria-label="Scroll to latest messages">
          ↓ New messages
        </button>
      )}
    </div>
  );
}

// ─── Chat header ──────────────────────────────────────────────────────────────

function DmHeaderInfo({ addr }: { addr: number }) {
  const { displayName, fullHex, status, lastSeen } = usePeerInfo(addr);
  const statusLabel = status === 'online' ? 'Online'
    : status === 'reachable' ? 'Reachable'
    : 'Unknown';
  const statusText = status === 'online' ? 'Online'
    : lastSeen ? `Last seen ${lastSeen}`
    : 'Unknown';
  return (
    <>
      <span className={styles.chatTitle}>
        <span
          className={styles.statusDot}
          style={{ background: STATUS_COLORS[status] }}
          title={statusLabel}
        />
        {displayName}
      </span>
      <span className={styles.chatSubtitle}>{statusText} · {fullHex}</span>
    </>
  );
}

function ChatHeader({ conversationId, onToggleDetail, onToggleSidebar }: { conversationId: string; onToggleDetail?: () => void; onToggleSidebar?: () => void }) {
  const conversations = useStore(s => s.conversations);
  const config = useStore(s => s.config);
  const conv = conversations.get(conversationId);
  const showRoutes = useStore(s => s.showRoutes);
  const setShowRoutes = useStore(s => s.setShowRoutes);

  const isChannel = conversationId.startsWith('ch:');
  const isDm = conversationId.startsWith('dm:');
  const dmAddr = isDm ? parseInt(conversationId.slice(3), 10) : 0;

  return (
    <div
      className={`${styles.chatHeader} ${isChannel ? styles.chatHeaderClickable : ''}`}
      onClick={isChannel ? onToggleDetail : undefined}
      role={isChannel ? 'button' : undefined}
      title={isChannel ? 'Click for channel details' : undefined}
    >
      <button
        className={styles.conversationsBtn}
        onClick={(e) => { e.stopPropagation(); onToggleSidebar?.(); }}
        aria-label="Open conversations"
        title="Conversations"
      >
        ☰
      </button>
      <div className={styles.chatHeaderText}>
        {isDm ? (
          <DmHeaderInfo addr={dmAddr} />
        ) : conversationId === 'broadcast' ? (
          <>
            <span className={styles.chatTitle}><IconBroadcast size={16} /> Broadcast</span>
            <span className={styles.chatSubtitle}>All nodes in range</span>
          </>
        ) : isChannel ? (
          <>
            <span className={styles.chatTitle}><IconHash size={14} /> {conv?.label ?? config?.channels?.find(c => c.index === Number(conversationId.slice(3)))?.name ?? `ch-${conversationId.slice(3)}`}</span>
            <span className={styles.chatSubtitle}>Channel {conversationId.slice(3)}</span>
          </>
        ) : (
          <span className={styles.chatTitle}>{conv?.label ?? conversationId}</span>
        )}
      </div>
      <button
        className={`${styles.routeBtn} ${showRoutes ? styles.routeBtnActive : ''}`}
        onClick={(e) => { e.stopPropagation(); setShowRoutes(!showRoutes); }}
        title={showRoutes ? 'Hide all routes' : 'Show all routes'}
        aria-label={showRoutes ? 'Hide all routes' : 'Show all routes'}
      >
        ⇆
      </button>
      {isChannel && <span className={styles.chevron}>▸</span>}
    </div>
  );
}

// ─── Main Chat page ───────────────────────────────────────────────────────────

export function Chat() {
  const conversations = useStore(s => s.conversations);
  const activeConversationId = useStore(s => s.activeConversationId);
  const setActiveConversation = useStore(s => s.setActiveConversation);
  const [showDetail, setShowDetail] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(false);

  const toggleDetail = useCallback(() => setShowDetail(v => !v), []);

  // Hide detail panel when switching conversations
  useEffect(() => {
    setShowDetail(false);
  }, [activeConversationId]);

  const isChannel = activeConversationId.startsWith('ch:');

  return (
    <div className={styles.chat}>
      <div className={`${styles.sidebarBackdrop} ${sidebarOpen ? styles.sidebarBackdropOpen : ''}`} onClick={() => setSidebarOpen(false)} />
      <div className={`${styles.sidebarWrap} ${sidebarOpen ? styles.sidebarOpen : ''}`}>
        <ConversationList
          conversations={conversations}
          activeId={activeConversationId}
          onSelect={(id) => {
            setActiveConversation(id);
            setSidebarOpen(false);
          }}
        />
      </div>
      <div className={styles.pane}>
        <ChatHeader conversationId={activeConversationId} onToggleDetail={toggleDetail} onToggleSidebar={() => setSidebarOpen(v => !v)} />
        {showDetail && isChannel ? (
          <ChannelDetailPanel
            channelIndex={parseInt(activeConversationId.slice(3), 10)}
            onClose={() => setShowDetail(false)}
          />
        ) : (
          <>
            <MessageList conversationId={activeConversationId} />
            <ComposeBar conversationId={activeConversationId} />
          </>
        )}
      </div>
    </div>
  );
}
