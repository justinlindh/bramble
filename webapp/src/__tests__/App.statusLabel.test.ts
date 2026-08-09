import { describe, expect, it } from 'vitest';
import { statusLabelFor } from '../App';

// The status pill used to render the raw enum ('connecting', 'disconnected')
// next to properly cased labels; every state gets real copy.
describe('statusLabelFor', () => {
  it('maps every connection state to display copy', () => {
    expect(statusLabelFor('connected')).toBe('Connected');
    expect(statusLabelFor('error')).toBe('Reconnecting…');
    expect(statusLabelFor('connecting')).toBe('Connecting…');
    expect(statusLabelFor('disconnected')).toBe('Disconnected');
  });
});
