import { useMemo, useRef } from 'react';
import { useStore, conversationIdForMessage } from './index';
import type { Message } from '../types/bramble';

// A message belongs to exactly one conversation, and conversationIdForMessage
// (store/index.ts) is the single classifier that decides which. Filtering the
// message list through it keeps the per-conversation view consistent with the
// conversation list and unread counts, which bucket the same way. Re-deriving
// the broadcast/channel/DM rules here used to let a channel message addressed
// to the broadcast address (channelIndex >= 0 and to === 0xffffffff) match both
// the 'broadcast' filter and its own 'ch:' filter, so it showed in two views;
// routing through the classifier files it under its channel alone.
function filterMessages(messages: Message[], id: string): Message[] {
  return messages.filter(m => conversationIdForMessage(m) === id);
}

// Exported for tests that pin the single-bucket filtering behavior.
export const __filterMessages = filterMessages;

export function useConversation(id: string): { messages: Message[] } {
  const allMessages = useStore(s => s.messages);
  const prevRef = useRef<Message[]>([]);

  const messages = useMemo(() => {
    const next = filterMessages(allMessages, id);
    // Return previous reference if contents haven't changed (avoids re-render)
    // Include broadcast recipient telemetry so delivery count/panel updates render.
    const prev = prevRef.current;
    const recSig = (m: Message) =>
      (m.broadcastRecipients ?? [])
        .map(r => `${r.addr}:${r.status}:${r.hopCount}:${r.deliveredAtMs}`)
        .join('|');
    if (
      prev.length === next.length &&
      next.every((m, i) =>
        m.id === prev[i].id &&
        m.status === prev[i].status &&
        recSig(m) === recSig(prev[i])
      )
    ) {
      return prev;
    }
    prevRef.current = next;
    return next;
  }, [allMessages, id]);

  return { messages };
}
