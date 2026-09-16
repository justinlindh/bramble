import { describe, it, expect } from 'vitest';
import { resolveNodeName, UNNAMED_NODE_NAME } from '../nodeName';

describe('resolveNodeName', () => {
  it('returns a trimmed user-set name', () => {
    expect(resolveNodeName('Base Camp')).toBe('Base Camp');
    expect(resolveNodeName('  Base Camp  ')).toBe('Base Camp');
  });

  it('rejects the firmware "(unnamed)" sentinel, trimmed or not', () => {
    expect(resolveNodeName(UNNAMED_NODE_NAME)).toBeUndefined();
    expect(resolveNodeName('  (unnamed)  ')).toBeUndefined();
  });

  it('treats empty, whitespace-only, null, and undefined as no name', () => {
    expect(resolveNodeName('')).toBeUndefined();
    expect(resolveNodeName('   ')).toBeUndefined();
    expect(resolveNodeName(null)).toBeUndefined();
    expect(resolveNodeName(undefined)).toBeUndefined();
  });
});
