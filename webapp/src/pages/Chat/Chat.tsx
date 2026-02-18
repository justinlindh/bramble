import { useEffect, useRef, useState, useCallback } from 'react';
import type { ReactNode } from 'react';
import { useStore } from '../../store/index';
import { useConversation, useMyAddress } from '../../store/selectors';
import { IconChat, IconBroadcast, IconHash } from '../../components/Icons';
import { ConversationList } from './ConversationList';
import { MessageBubble } from './MessageBubble';
import { ComposeBar } from './ComposeBar';
import { ChannelDetailPanel } from './ChannelDetailPanel';
import styles from './Chat.module.css';

// ─── Empty state ──────────────────────────────────────────────────────────────

function EmptyMessages({ convId }: { convId: string }) {
  const hint =
    convId === 'broadcast'
      ? 'Broadcast messages will appear here. All nodes in range will receive them.'
      : convId.startsWith('ch:')
      ? 'Channel messages will appear here.'
      : 'Send a message to start this conversation.';

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

  // Auto-scroll to bottom when messages change (new message, status update, etc.)
  useEffect(() => {
    const el = listRef.current;
    if (!el) return;
    // Use requestAnimationFrame to ensure DOM has updated
    requestAnimationFrame(() => {
      el.scrollTop = el.scrollHeight;
    });
  }, [messages.length, messages[messages.length - 1]?.status]);

  if (messages.length === 0) {
    return <EmptyMessages convId={conversationId} />;
  }

  return (
    <div ref={listRef} className={styles.messageList} aria-live="polite" aria-label="Messages">
      {messages.map(msg => (
        <MessageBubble key={msg.id} message={msg} myAddr={myAddr} />
      ))}
    </div>
  );
}

// ─── Chat header ──────────────────────────────────────────────────────────────

function ChatHeader({ conversationId, onToggleDetail }: { conversationId: string; onToggleDetail?: () => void }) {
  const conversations = useStore(s => s.conversations);
  const conv = conversations.get(conversationId);

  let title: ReactNode = conv?.label ?? conversationId;
  let subtitle = '';
  const isChannel = conversationId.startsWith('ch:');

  if (conversationId === 'broadcast') {
    title = <><IconBroadcast size={16} /> Broadcast</>;
    subtitle = 'All nodes in range';
  } else if (isChannel) {
    title = <><IconHash size={14} /> {conv?.label ?? `ch-${conversationId.slice(3)}`}</>;
    subtitle = `Channel ${conversationId.slice(3)}`;
  } else if (conversationId.startsWith('dm:')) {
    const addr = parseInt(conversationId.slice(3), 10);
    subtitle = `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
  }

  return (
    <div
      className={`${styles.chatHeader} ${isChannel ? styles.chatHeaderClickable : ''}`}
      onClick={isChannel ? onToggleDetail : undefined}
      role={isChannel ? 'button' : undefined}
      title={isChannel ? 'Click for channel details' : undefined}
    >
      <div className={styles.chatHeaderText}>
        <span className={styles.chatTitle}>{title}</span>
        {subtitle && <span className={styles.chatSubtitle}>{subtitle}</span>}
      </div>
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

  const toggleDetail = useCallback(() => setShowDetail(v => !v), []);

  // Hide detail panel when switching conversations
  useEffect(() => {
    setShowDetail(false);
  }, [activeConversationId]);

  const isChannel = activeConversationId.startsWith('ch:');

  return (
    <div className={styles.chat}>
      <ConversationList
        conversations={conversations}
        activeId={activeConversationId}
        onSelect={setActiveConversation}
      />
      <div className={styles.pane}>
        <ChatHeader conversationId={activeConversationId} onToggleDetail={toggleDetail} />
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
