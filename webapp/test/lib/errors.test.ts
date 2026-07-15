import { describe, it, expect } from 'vitest';
import { friendlyError, friendlyErrorFrom } from '../../src/lib/errors';

describe('friendlyError', () => {
  it('maps a known pattern to friendly text', () => {
    const s = friendlyError('handshake timed out');
    expect(s).toContain('did not respond');
  });

  it('passes through an unknown short string unchanged', () => {
    const s = friendlyError('some unrecognized error');
    expect(s).toBe('some unrecognized error');
  });
});

describe('friendlyErrorFrom', () => {
  it('handles a non-Error-shaped input', () => {
    const s = friendlyErrorFrom('handshake timed out');
    expect(s).toContain('did not respond');
  });

  it('extracts message from an Error-shaped input', () => {
    const s = friendlyErrorFrom(new Error('handshake timed out'));
    expect(s).toContain('did not respond');
  });

  it('falls back to generic text for thrown undefined', () => {
    const s = friendlyErrorFrom(undefined);
    expect(s).not.toContain('undefined');
    expect(s.length).toBeGreaterThan(0);
  });

  it('falls back to generic text for thrown null', () => {
    const s = friendlyErrorFrom(null);
    expect(s).not.toContain('null');
    expect(s.length).toBeGreaterThan(0);
  });

  it('falls back to generic text for a plain object without message', () => {
    const s = friendlyErrorFrom({ code: -1005 });
    expect(s).not.toContain('[object Object]');
    expect(s.length).toBeGreaterThan(0);
  });

  it('falls back to generic text for a non-string primitive', () => {
    const s = friendlyErrorFrom(42);
    expect(s).not.toBe('42');
    expect(s.length).toBeGreaterThan(0);
  });
});
