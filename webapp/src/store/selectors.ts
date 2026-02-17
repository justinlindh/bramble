import { useStore } from './index';
import type { Conversation, Message } from '../types/bramble';

export function useConversation(id: string): {
  conv: Conversation | undefined;
  messages: Message[];
} {
  const conv = useStore(s => s.conversations.get(id));
  const messages = useStore(s =>
    s.messages.filter(m => {
      if (id === 'broadcast') {
        // Show broadcast messages (to 0xFFFFFFFF)
        return m.to === 0xffffffff;
      }
      if (id.startsWith('ch:')) {
        const chIdx = parseInt(id.slice(3), 10);
        return m.channelIndex === chIdx;
      }
      if (id.startsWith('dm:')) {
        const peerAddr = parseInt(id.slice(3), 10);
        return (
          (m.direction === 'outgoing' && m.to === peerAddr) ||
          (m.direction === 'incoming' && m.from === peerAddr)
        );
      }
      return false;
    })
  );
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
