import { describe, expect, it } from 'vitest';
import { STATE_LABELS } from '../components/StatusDot';
import type { ConnectionState } from '../types/bramble';

// STATE_LABELS is the single label map the StatusDot title/aria-label and the
// status pill text in App both read, so the visible text can never contradict
// the dot. The pill used to render the raw enum ('connecting', 'disconnected')
// next to properly cased labels; every state gets real copy, and the dot and
// the pill had already drifted ('Error' next to 'Reconnecting…') back when each
// carried its own map.
describe('STATE_LABELS', () => {
  it('maps every connection state to display copy', () => {
    expect(STATE_LABELS.connected).toBe('Connected');
    expect(STATE_LABELS.error).toBe('Reconnecting…');
    expect(STATE_LABELS.connecting).toBe('Connecting…');
    expect(STATE_LABELS.disconnected).toBe('Disconnected');
  });

  it('covers every connection state with non-empty copy', () => {
    const states: ConnectionState[] = ['connected', 'error', 'connecting', 'disconnected'];
    for (const s of states) {
      expect(STATE_LABELS[s]).toBeTruthy();
    }
  });
});
