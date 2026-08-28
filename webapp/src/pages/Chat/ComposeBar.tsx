import { useState, useRef, useEffect, useMemo } from 'react';
import type { ReactNode } from 'react';
import type { MessageTier } from '../../types/bramble';
import { sendMessage, shareLocationOnce } from '../../store/actions';
import { useStore, parseConversationId } from '../../store/index';
import { IconCritical, IconBroadcast, IconSend } from '../../components/Icons';
import { showToast } from '../../components/Toast';
import { friendlyErrorFrom } from '../../lib/errors';
import { BROADCAST_ADDR, CHANNEL_BROADCAST_ADDR } from '../../lib/addr';
import {
  utf8Length,
  SINGLE_PACKET_MAX_BYTES,
  FRAGMENT_PAYLOAD_BYTES,
  MAX_FRAGMENTS,
  FRAGMENTED_MAX_BYTES,
} from '../../utils/byteLimit';
import { ShareLocationToggle } from './ShareLocationButton';
import type { LocationAttach } from './ShareLocationButton';
import styles from './ComposeBar.module.css';

function packetInfo(bytes: number): { count: number; cls: string; label: string } {
  if (bytes === 0) return { count: 0, cls: '', label: '' };
  if (bytes <= SINGLE_PACKET_MAX_BYTES) return { count: 1, cls: styles.counterOk, label: '' };
  const count = Math.ceil(bytes / FRAGMENT_PAYLOAD_BYTES);
  if (count <= 2) return { count, cls: styles.counterWarn, label: `${count} fragments` };
  if (count <= MAX_FRAGMENTS) return { count, cls: styles.counterHigh, label: `${count} fragments` };
  return { count, cls: styles.counterOver, label: 'too long' };
}

interface ComposeBarProps {
  conversationId: string;
}

// Map a conversationId to a sendMessage call signature.
function parseConversation(convId: string): { dest: number; channelIndex?: number } {
  const parsed = parseConversationId(convId);
  switch (parsed.kind) {
    case 'channel':
      return { dest: CHANNEL_BROADCAST_ADDR, channelIndex: parsed.index };
    case 'dm':
      return { dest: parsed.addr };
    case 'broadcast':
    case 'unknown':
      return { dest: BROADCAST_ADDR };
  }
}

const TIER_OPTIONS: Array<{ value: MessageTier; label: ReactNode; title: string }> = [
  { value: 'normal',   label: 'Normal',   title: 'Standard delivery'                        },
  { value: 'critical', label: <><IconCritical size={12} /> Critical</>, title: 'Reliable delivery with relay path tracking' },
];

export function ComposeBar({ conversationId }: ComposeBarProps) {
  const [text, setText] = useState('');
  const [tier, setTier] = useState<MessageTier>('normal');
  const [locAttach, setLocAttach] = useState<LocationAttach>('off');
  const [sending, setSending] = useState(false);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const isConnected = useStore(s => s.connectionState === 'connected');
  const gpsEnabled = useStore(s => s.config?.location?.enabled ?? false);

  // Parse conversation to get dest
  const { dest } = parseConversation(conversationId);

  // For broadcast conversation, force tier to 'broadcast'
  const isBroadcastConv = parseConversationId(conversationId).kind === 'broadcast';
  const effectiveTier: MessageTier = isBroadcastConv ? 'broadcast' : tier;

  const bytes = useMemo(() => utf8Length(text), [text]);
  const pkt = packetInfo(bytes);
  const overLimit = bytes > FRAGMENTED_MAX_BYTES;
  const canSend = isConnected && text.trim().length > 0 && !sending && !overLimit;

  const handleSend = async () => {
    const trimmed = text.trim();
    if (!trimmed || !isConnected || sending) return;

    setSending(true);
    setText('');

    const { dest, channelIndex } = parseConversation(conversationId);

    // Wrap slash-action commands as CTCP ACTION
    const payload = trimmed.startsWith('/me ')
      ? `\x01ACTION ${trimmed.slice(4)}\x01`
      : trimmed.startsWith('/slap ')
        ? `\x01ACTION slaps ${trimmed.slice(6).trim()} around a bit with a large trout\x01`
        : trimmed;

    try {
      await sendMessage(dest, payload, effectiveTier, channelIndex);
      // Attach location if enabled (fire-and-forget)
      if (locAttach !== 'off' && dest !== BROADCAST_ADDR) {
        const locTier = locAttach === 'exact' ? 'full' : 'coarse';
        shareLocationOnce(dest, locTier as import('../../types/bramble').LocationTier).catch((err) => {
          showToast(`Location attach failed: ${friendlyErrorFrom(err)}`, 'error', 4000);
        });
      }
    } catch (e) {
      showToast(friendlyErrorFrom(e), 'error', 5000);
      // restore original text so user can retry (preserve /me prefix, not encoded form)
      setText(trimmed);
    } finally {
      setSending(false);
      // Double rAF to ensure React re-render (re-enabling textarea) has flushed
      requestAnimationFrame(() =>
        requestAnimationFrame(() => inputRef.current?.focus())
      );
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  // Auto-grow textarea
  useEffect(() => {
    const el = inputRef.current;
    if (!el) return;
    el.style.height = 'auto';
    el.style.height = `${Math.min(el.scrollHeight, 160)}px`;
  }, [text]);

  return (
    <div className={styles.compose}>
      {/* ── Options row (tier, location, counter) ── */}
      <div className={styles.controls}>
        {!isBroadcastConv && (
          <div className={styles.tierSelector} role="group" aria-label="Message priority">
            {TIER_OPTIONS.map(opt => (
              <button
                key={opt.value}
                className={[
                  styles.tierBtn,
                  tier === opt.value ? styles.tierActive : '',
                  opt.value === 'critical' && tier === opt.value ? styles.tierCriticalActive : '',
                ].join(' ')}
                onClick={() => setTier(opt.value)}
                disabled={!isConnected}
                title={opt.title}
                aria-pressed={tier === opt.value}
              >
                {opt.label}
              </button>
            ))}
          </div>
        )}

        {isBroadcastConv && (
          <span className={styles.broadcastTag} title="Messages here go to all nodes in range">
            <IconBroadcast size={14} /> Broadcast
          </span>
        )}

        {!isBroadcastConv && gpsEnabled && (
          <ShareLocationToggle value={locAttach} onChange={setLocAttach} />
        )}

        {bytes > 0 && (
          <div
            className={`${styles.counter} ${pkt.cls}`}
            title={`Messages over ${SINGLE_PACKET_MAX_BYTES} bytes are automatically split into fragments (max ${MAX_FRAGMENTS} fragments, ${FRAGMENT_PAYLOAD_BYTES} bytes each). Fragments are reassembled on the receiving node.`}
            aria-label="Message size and fragmentation info"
          >
            <span>{bytes}/{FRAGMENTED_MAX_BYTES}</span>
            {pkt.label && <span className={styles.packetLabel}>{pkt.label}</span>}
          </div>
        )}
      </div>

      {/* ── Input row (textarea + send button) ── */}
      <div className={styles.row}>
        <textarea
          ref={inputRef}
          className={styles.input}
          value={text}
          onChange={e => setText(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder={isConnected ? 'Type a message…' : 'Connect to a node to send messages'}
          disabled={!isConnected || sending}
          rows={1}
          aria-label="Message input"
        />

        <button
          className={styles.sendBtn}
          onClick={handleSend}
          onMouseDown={(e) => e.preventDefault()}
          disabled={!canSend}
          aria-label="Send message"
          title="Send (Enter)"
        >
          {sending ? '…' : <IconSend size={16} />}
        </button>
      </div>
    </div>
  );
}
