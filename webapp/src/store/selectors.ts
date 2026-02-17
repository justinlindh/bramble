import { useMemo, useRef } from 'react';
import { useStore } from './index';
import type { Conversation, Message } from '../types/bramble';

function filterMessages(messages: Message[], id: string): Message[] {
  return messages.filter(m => {
    if (id === 'broadcast') return m.to === 0xffffffff;
    if (id.startsWith('ch:')) {
      const chIdx = parseInt(id.slice(3), 10);
      return m.channelIndex === chIdx;
    }
    if (id.startsWith('dm:')) {
      const peerAddr = parseInt(id.slice(3), 10);
      if (m.to === 0xffffffff) return false;
      return (
        (m.direction === 'outgoing' && m.to === peerAddr) ||
        (m.direction === 'incoming' && m.from === peerAddr)
      );
    }
    return false;
  });
}

export function useConversation(id: string): {
  conv: Conversation | undefined;
  messages: Message[];
} {
  const allMessages = useStore(s => s.messages);
  const conv = useStore(s => s.conversations.get(id));
  const prevRef = useRef<Message[]>([]);

  const messages = useMemo(() => {
    const next = filterMessages(allMessages, id);
    // Return previous reference if contents haven't changed (avoids re-render)
    const prev = prevRef.current;
    if (
      prev.length === next.length &&
      next.every((m, i) => m.id === prev[i].id && m.status === prev[i].status)
    ) {
      return prev;
    }
    prevRef.current = next;
    return next;
  }, [allMessages, id]);

  return { conv, messages };
}

export function useTotalUnread(): number {
  return useStore(s => {
    let total = 0;
    for (const conv of s.conversations.values()) {
      total += conv.unreadCount;
    }
    return total;
  });
}

export function useIsConnected(): boolean {
  return useStore(s => s.connectionState === 'connected');
}

export function useMyAddress(): number {
  return useStore(s => s.config?.identity.address ?? 0);
}
