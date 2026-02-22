import { describe, it, expect } from 'vitest';
import { tabFromShortcut } from '../../src/App';

describe('keyboard shortcut tab mapping', () => {
  it('maps Ctrl+1..5 to tabs', () => {
    expect(tabFromShortcut('1')).toBe('chat');
    expect(tabFromShortcut('2')).toBe('nodes');
    expect(tabFromShortcut('3')).toBe('map');
    expect(tabFromShortcut('4')).toBe('config');
    expect(tabFromShortcut('5')).toBe('stats');
  });

  it('returns null for unsupported keys', () => {
    expect(tabFromShortcut('0')).toBeNull();
    expect(tabFromShortcut('x')).toBeNull();
  });
});
