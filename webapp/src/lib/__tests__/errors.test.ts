import { describe, it, expect } from 'vitest';
import { isAuthError, isUnknownMethodError, friendlyErrorFrom } from '../errors';

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

describe('friendlyErrorFrom', () => {
  it('still maps Error and string inputs through the friendly map', () => {
    expect(friendlyErrorFrom(new Error('Unauthorized'))).toMatch(/auth token/i);
    expect(friendlyErrorFrom('handshake timed out')).toMatch(/did not respond/i);
  });

  it('falls back for message-less values', () => {
    expect(friendlyErrorFrom(undefined)).toMatch(/something went wrong/i);
  });
});
