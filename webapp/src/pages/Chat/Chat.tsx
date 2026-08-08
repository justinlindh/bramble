import { useEffect, useRef, useState, useCallback } from 'react';
import type { ReactNode } from 'react';
import { useStore, parseConversationId, formatConversationLabel } from '../../store/index';
import { useConversation } from '../../store/selectors';
import { IconChat, IconBroadcast, IconHash, IconRoutes, IconLock, IconWarning } from '../../components/Icons';
import { usePeerInfo, usePeerVerification, STATUS_COLORS } from '../../hooks/usePeer';
import { ConversationList } from './ConversationList';
import { MessageBubble } from './MessageBubble';
import { ComposeBar } from './ComposeBar';
import { ChannelDetailPanel } from './ChannelDetailPanel';
import { VerifySafetyNumber } from './VerifySafetyNumber';
import { loadPeerVerification, setPeerVerified } from '../../store/actions';
import { formatDaySeparatorLabel, shouldInsertDaySeparator } from './chatDateFormatting';
import styles from './Chat.module.css';

export function isNearBottom(el: { scrollTop: number; clientHeight: number; scrollHeight: number }, threshold = 100): boolean {
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

// ─── Empty state ──────────────────────────────────────────────────────────────

export function getEmptyHint(convId: string): string {
  switch (parseConversationId(convId).kind) {
    case 'broadcast':
      return 'Broadcast messages will appear here. All nodes in range will receive them.';
    case 'channel':
      return 'Channel messages will appear here.';
    default:
      return 'Send a message to start this conversation.';
  }
}

function EmptyMessages({ convId, loading }: { convId: string; loading?: boolean }) {
  const hint = loading ? 'Loading messages…' : getEmptyHint(convId);

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
  // `status` loads in the same initial connect-time batch as message history;
  // while it is still null we can't yet tell "no messages" from "haven't
  // fetched yet" apart, so show a loading affordance instead of the
  // per-conversation empty hint.
  const connectionState = useStore(s => s.connectionState);
  const status = useStore(s => s.status);
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

  useEffect(() => {
    setNearBottom(true);
    setShowJump(false);
  }, [conversationId]);

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
    const loading = connectionState === 'connected' && status === null;
    return <EmptyMessages convId={conversationId} loading={loading} />;
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
              <MessageBubble message={msg} />
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

// ─── Verify safety number (SAS) ────────────────────────────────────────────

function VerifySafetyNumberPanel({ addr, onClose }: { addr: number; onClose: () => void }) {
  const { displayName, fullHex } = usePeerInfo(addr);
  const verification = useStore(s => s.peerVerifications.get(addr));

  useEffect(() => {
    loadPeerVerification(addr).catch(() => {});
  }, [addr]);

  return (
    <VerifySafetyNumber
      peerAddress={fullHex.slice(2)}
      peerName={displayName}
      sas={verification?.sas ?? ''}
      verified={verification?.verified ?? false}
      keyChanged={verification?.keyChanged ?? false}
      onSetVerified={(peerAddr, v) => setPeerVerified(parseInt(peerAddr, 16), v)}
      onClose={onClose}
    />
  );
}

function KeyChangedBanner({ onOpenVerify }: { onOpenVerify: () => void }) {
  return (
    <div className={styles.keyChangedBar} role="alert">
      <IconWarning size={14} />
      <span>Safety number changed for this contact.</span>
      <button className={styles.keyChangedBarBtn} onClick={onOpenVerify}>Verify now</button>
    </div>
  );
}

// ─── Chat header ──────────────────────────────────────────────────────────────

function DmHeaderInfo({ addr }: { addr: number }) {
  const { displayName, fullHex, status, lastSeen } = usePeerInfo(addr);
  const verification = usePeerVerification(addr);
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
        {verification?.keyChanged ? (
          <span className={styles.verifyGlyphWarn} title="Safety number changed">
            <IconWarning size={13} />
          </span>
        ) : verification?.verified ? (
          <span className={styles.verifyGlyphOk} title="Verified">
            <IconLock size={13} />
          </span>
        ) : null}
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

  const parsed = parseConversationId(conversationId);
  const isChannel = parsed.kind === 'channel';
  const isDm = parsed.kind === 'dm';
  const dmAddr = parsed.kind === 'dm' ? parsed.addr : 0;

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
        ) : parsed.kind === 'broadcast' ? (
          <>
            <span className={styles.chatTitle}><IconBroadcast size={16} /> Broadcast</span>
            <span className={styles.chatSubtitle}>All nodes in range</span>
          </>
        ) : parsed.kind === 'channel' ? (
          <>
            <span className={styles.chatTitle}><IconHash size={14} /> {conv?.label ?? formatConversationLabel(conversationId, undefined, config)}</span>
            <span className={styles.chatSubtitle}>Channel {parsed.index}</span>
          </>
        ) : (
          <span className={styles.chatTitle}>{conv?.label ?? conversationId}</span>
        )}
      </div>
      {isDm && (
        <button
          className={styles.verifyBtn}
          onClick={(e) => { e.stopPropagation(); onToggleDetail?.(); }}
          title="Verify safety number"
          aria-label="Verify safety number"
        >
          <IconLock size={14} />
        </button>
      )}
      <button
        className={`${styles.routeBtn} ${showRoutes ? styles.routeBtnActive : ''}`}
        onClick={(e) => { e.stopPropagation(); setShowRoutes(!showRoutes); }}
        title={showRoutes ? 'Hide all routes' : 'Show all routes'}
        aria-label={showRoutes ? 'Hide all routes' : 'Show all routes'}
      >
        <IconRoutes size={14} />
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

  // Escape closes the mobile conversation sidebar, matching the backdrop
  // click and the hamburger toggle button as alternate dismissal paths.
  useEffect(() => {
    if (!sidebarOpen) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setSidebarOpen(false);
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [sidebarOpen]);

  const parsed = parseConversationId(activeConversationId);
  const isDm = parsed.kind === 'dm';
  const dmAddr = parsed.kind === 'dm' ? parsed.addr : 0;
  const dmVerification = useStore(s => (isDm ? s.peerVerifications.get(dmAddr) : undefined));

  return (
    <div className={styles.chat}>
      <div
        className={`${styles.sidebarBackdrop} ${sidebarOpen ? styles.sidebarBackdropOpen : ''}`}
        onClick={() => setSidebarOpen(false)}
        role="presentation"
      />
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
        {showDetail && parsed.kind === 'channel' ? (
          <ChannelDetailPanel
            channelIndex={parsed.index}
            onClose={() => setShowDetail(false)}
          />
        ) : showDetail && isDm ? (
          <VerifySafetyNumberPanel addr={dmAddr} onClose={() => setShowDetail(false)} />
        ) : (
          <>
            {isDm && dmVerification?.keyChanged && (
              <KeyChangedBanner onOpenVerify={() => setShowDetail(true)} />
            )}
            <MessageList conversationId={activeConversationId} />
            <ComposeBar conversationId={activeConversationId} />
          </>
        )}
      </div>
    </div>
  );
}
