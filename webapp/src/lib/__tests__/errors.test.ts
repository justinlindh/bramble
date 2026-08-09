import { describe, it, expect } from 'vitest';
import { friendlyError, isAuthError, isUnknownMethodError, friendlyErrorFrom } from '../errors';

describe('isAuthError', () => {
  it('matches 1008, unauthorized, and auth-tagged messages', () => {
    expect(isAuthError(new Error('WebSocket closed 1008'))).toBe(true);
    expect(isAuthError('Unauthorized')).toBe(true);
    expect(isAuthError(new Error('auth token required'))).toBe(true);
  });

  it('accepts a raw string as well as an Error', () => {
    expect(isAuthError('1008 policy violation')).toBe(true);
  });

  it('is false for unrelated messages and message-less values', () => {
    expect(isAuthError(new Error('connection timed out'))).toBe(false);
    expect(isAuthError(null)).toBe(false);
    expect(isAuthError(undefined)).toBe(false);
    expect(isAuthError('')).toBe(false);
  });
});

describe('isUnknownMethodError', () => {
  it('matches the firmware not-implemented phrasings', () => {
    expect(isUnknownMethodError(new Error('method not found'))).toBe(true);
    expect(isUnknownMethodError('unknown method: bramble.foo')).toBe(true);
    expect(isUnknownMethodError(new Error('not found'))).toBe(true);
  });

  it('is false for other errors and message-less values', () => {
    expect(isUnknownMethodError(new Error('invalid params'))).toBe(false);
    expect(isUnknownMethodError(null)).toBe(false);
  });
});

describe('pairing and link failure mappings', () => {
  it('maps pairing-did-not-complete to a retry-with-code hint that never trips isAuthError', () => {
    const friendly = friendlyError('Bluetooth pairing did not complete');
    expect(friendly).toMatch(/connect again/i);
    expect(friendly).toMatch(/code shown on the node/i);
  });

  it('maps pairing-was-cancelled and wins over the auth rule', () => {
    expect(friendlyError('Bluetooth pairing was cancelled')).toBe('Pairing was cancelled. Click Connect to try again.');
    // BlueZ appends a security reason to the raw message; the cancel mapping
    // must still win over /1008|unauthorized|auth/i so the RAW classification
    // at the connect() boundary maps to the cancel copy, not the token copy.
    const friendly = friendlyError('Bluetooth pairing was cancelled: insufficient authentication');
    expect(friendly).toBe('Pairing was cancelled. Click Connect to try again.');
  });

  it('maps a raw RPC timeout instead of leaking it verbatim', () => {
    const friendly = friendlyError('RPC timeout: bramble.getVersion');
    expect(friendly).not.toMatch(/RPC timeout/);
    expect(friendly).toMatch(/did not answer/i);
  });

  it('maps a GATT write timeout to a link-stall explanation', () => {
    const friendly = friendlyError('GATT write timed out (chunk 3)');
    expect(friendly).not.toMatch(/chunk/);
    expect(friendly).toMatch(/bluetooth link/i);
  });

  it('keeps the handshake-timed-out mapping for the post-encryption case', () => {
    expect(friendlyError('handshake timed out')).toMatch(/did not respond/i);
  });
});

describe('friendlyErrorFrom', () => {
  it('still maps Error and string inputs through the friendly map', () => {
    expect(friendlyErrorFrom(new Error('Unauthorized'))).toMatch(/auth token/i);
    expect(friendlyErrorFrom('handshake timed out')).toMatch(/did not respond/i);
  });

  it('falls back for message-less values', () => {
    expect(friendlyErrorFrom(undefined)).toMatch(/something went wrong/i);
  });
});
