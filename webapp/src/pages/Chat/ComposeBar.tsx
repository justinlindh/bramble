import { useState, useRef, useEffect, useMemo } from 'react';
import type { ReactNode } from 'react';
import type { MessageTier } from '../../types/bramble';
import { sendMessage } from '../../store/actions';
import { useStore } from '../../store/index';
import { IconCritical, IconBroadcast, IconSend } from '../../components/Icons';
import { ShareLocationButton } from './ShareLocationButton';
import styles from './ComposeBar.module.css';

const MAX_BYTES = 800;
const SINGLE_PACKET = 200;

function byteLength(str: string): number {
  return new TextEncoder().encode(str).length;
}

function packetInfo(bytes: number): { count: number; cls: string; label: string } {
  if (bytes === 0) return { count: 0, cls: '', label: '' };
  if (bytes <= SINGLE_PACKET) return { count: 1, cls: styles.counterOk, label: '' };
  const count = Math.ceil(bytes / SINGLE_PACKET);
  if (count <= 2) return { count, cls: styles.counterWarn, label: `${count} packets` };
  if (count <= 4) return { count, cls: styles.counterHigh, label: `${count} packets` };
  return { count, cls: styles.counterOver, label: 'too long' };
}

interface ComposeBarProps {
  conversationId: string;
}

// Parse a conversationId into a sendMessage call signature
function parseConversation(convId: string): { dest: number; channelIndex?: number } {
  if (convId === 'broadcast') {
    return { dest: 0xffffffff };
  }
  if (convId.startsWith('ch:')) {
    const idx = parseInt(convId.slice(3), 10);
    return { dest: 0xfffffffe, channelIndex: idx };
  }
  if (convId.startsWith('dm:')) {
    const addr = parseInt(convId.slice(3), 10);
    return { dest: addr };
  }
  return { dest: 0xffffffff };
}

const TIER_OPTIONS: Array<{ value: MessageTier; label: ReactNode; title: string }> = [
  { value: 'normal',   label: 'Normal',   title: 'Standard delivery'                        },
  { value: 'critical', label: <><IconCritical size={12} /> Critical</>, title: 'Reliable delivery with relay path tracking' },
];

export function ComposeBar({ conversationId }: ComposeBarProps) {
  const [text, setText] = useState('');
  const [tier, setTier] = useState<MessageTier>('normal');
  const [sending, setSending] = useState(false);
  const [error, setError] = useState('');
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const isConnected = useStore(s => s.connectionState === 'connected');

  // Parse conversation to get dest
  const { dest } = parseConversation(conversationId);

  // For broadcast conversation, force tier to 'broadcast'
  const isBroadcastConv = conversationId === 'broadcast';
  const effectiveTier: MessageTier = isBroadcastConv ? 'broadcast' : tier;

  const bytes = useMemo(() => byteLength(text), [text]);
  const pkt = packetInfo(bytes);
  const overLimit = bytes > MAX_BYTES;
  const canSend = isConnected && text.trim().length > 0 && !sending && !overLimit;

  const handleSend = async () => {
    const trimmed = text.trim();
    if (!trimmed || !isConnected || sending) return;

    setError('');
    setSending(true);
    setText('');

    const { dest, channelIndex } = parseConversation(conversationId);

    try {
      await sendMessage(dest, trimmed, effectiveTier, channelIndex);
    } catch (e) {
      setError((e as Error).message ?? 'Send failed');
      // restore text so user can retry
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
    el.style.height = `${Math.min(el.scrollHeight, 120)}px`;
  }, [text]);

  return (
    <div className={styles.compose}>
      {error && (
        <div className={styles.error} role="alert">
          ⚠ {error}
          <button className={styles.errorDismiss} onClick={() => setError('')}>✕</button>
        </div>
      )}

      <div className={styles.row}>
        <textarea
          ref={inputRef}
          className={styles.input}
          value={text}
          onChange={e => setText(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder={isConnected ? 'Type a message… (Enter to send, Shift+Enter for newline)' : 'Connect to a node to send messages'}
          disabled={!isConnected || sending}
          rows={1}
          aria-label="Message input"
        />

        {bytes > 0 && (
          <div className={`${styles.counter} ${pkt.cls}`}>
            <span>{bytes}/{MAX_BYTES}</span>
            {pkt.label && <span className={styles.packetLabel}>{pkt.label}</span>}
          </div>
        )}

        <div className={styles.controls}>
          {/* Tier selector — hidden for broadcast conv (fixed to broadcast tier) */}
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

          {!isBroadcastConv && <ShareLocationButton dest={dest} />}

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
    </div>
  );
}
