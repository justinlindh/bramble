import { useEffect, useRef } from 'react';
import { useStore } from '../../store/index';
import { useConversation, useMyAddress } from '../../store/selectors';
import { ConversationList } from './ConversationList';
import { MessageBubble } from './MessageBubble';
import { ComposeBar } from './ComposeBar';
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
      <span className={styles.emptyIcon}>💬</span>
      <p className={styles.emptyHint}>{hint}</p>
    </div>
  );
}

// ─── Message list ─────────────────────────────────────────────────────────────

function MessageList({ conversationId }: { conversationId: string }) {
  const { messages } = useConversation(conversationId);
  const myAddr = useMyAddress();
  const bottomRef = useRef<HTMLDivElement>(null);

  // Auto-scroll to bottom when new messages arrive
  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages.length]);

  if (messages.length === 0) {
    return <EmptyMessages convId={conversationId} />;
  }

  return (
    <div className={styles.messageList} aria-live="polite" aria-label="Messages">
      {messages.map(msg => (
        <MessageBubble key={msg.id} message={msg} myAddr={myAddr} />
      ))}
      <div ref={bottomRef} />
    </div>
  );
}

// ─── Chat header ──────────────────────────────────────────────────────────────

function ChatHeader({ conversationId }: { conversationId: string }) {
  const conversations = useStore(s => s.conversations);
  const conv = conversations.get(conversationId);

  let title = conv?.label ?? conversationId;
  let subtitle = '';

  if (conversationId === 'broadcast') {
    title = '📢 Broadcast';
    subtitle = 'All nodes in range';
  } else if (conversationId.startsWith('ch:')) {
    subtitle = `Channel ${conversationId.slice(3)}`;
  } else if (conversationId.startsWith('dm:')) {
    const addr = parseInt(conversationId.slice(3), 10);
    subtitle = `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
  }

  return (
    <div className={styles.chatHeader}>
      <div className={styles.chatHeaderText}>
        <span className={styles.chatTitle}>{title}</span>
        {subtitle && <span className={styles.chatSubtitle}>{subtitle}</span>}
      </div>
    </div>
  );
}

// ─── Main Chat page ───────────────────────────────────────────────────────────

export function Chat() {
  const conversations = useStore(s => s.conversations);
  const activeConversationId = useStore(s => s.activeConversationId);
  const setActiveConversation = useStore(s => s.setActiveConversation);

  return (
    <div className={styles.chat}>
      {/* Sidebar: conversation list */}
      <ConversationList
        conversations={conversations}
        activeId={activeConversationId}
        onSelect={setActiveConversation}
      />

      {/* Main pane: header + messages + compose */}
      <div className={styles.pane}>
        <ChatHeader conversationId={activeConversationId} />

        <MessageList conversationId={activeConversationId} />

        <ComposeBar conversationId={activeConversationId} />
      </div>
    </div>
  );
}
