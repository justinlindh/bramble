import { describe, it, expect } from 'vitest';
import { connectingLabelFor } from '../../src/components/ConnectionOverlay';

describe('ConnectionOverlay connecting labels', () => {
  it('returns transport-specific connecting states', () => {
    expect(connectingLabelFor('ble')).toContain('Scanning');
    expect(connectingLabelFor('serial')).toContain('Opening serial');
    expect(connectingLabelFor('wifi')).toContain('Handshaking');
    expect(connectingLabelFor('mock')).toContain('Connecting');
  });
});
