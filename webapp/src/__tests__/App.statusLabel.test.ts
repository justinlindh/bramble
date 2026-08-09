import { describe, expect, it } from 'vitest';
import { statusLabelFor } from '../App';
import { STATE_LABELS } from '../components/StatusDot';
import type { ConnectionState } from '../types/bramble';

// The status pill used to render the raw enum ('connecting', 'disconnected')
// next to properly cased labels; every state gets real copy.
describe('statusLabelFor', () => {
  it('maps every connection state to display copy', () => {
    expect(statusLabelFor('connected')).toBe('Connected');
    expect(statusLabelFor('error')).toBe('Reconnecting…');
    expect(statusLabelFor('connecting')).toBe('Connecting…');
    expect(statusLabelFor('disconnected')).toBe('Disconnected');
  });

  it('agrees with the StatusDot labels sitting beside it in the pill', () => {
    // The dot's title/aria-label and the visible text render side by side;
    // two parallel maps had already drifted (the dot said 'Error' while the
    // text said 'Reconnecting…'), so both surfaces consume one map.
    const states: ConnectionState[] = ['connected', 'error', 'connecting', 'disconnected'];
    for (const s of states) {
      expect(statusLabelFor(s)).toBe(STATE_LABELS[s]);
    }
  });
});
