import { describe, it, expect, beforeEach } from 'vitest';
import { saveUnreadCounts, loadUnreadCounts, clearUnreadCounts } from '../../src/store/unreadStore';

describe('unreadStore', () => {
  const testNodeAddr = '12345678';
  
  beforeEach(() => {
    // Clear localStorage before each test
    localStorage.clear();
  });

  it('should save and load unread counts', () => {
    const counts = {
      'dm:100': 5,
      'ch:1': 3,
      'broadcast': 2,
    };

    saveUnreadCounts(testNodeAddr, counts);
    const loaded = loadUnreadCounts(testNodeAddr);

    expect(loaded).toEqual(counts);
  });

  it('should return empty object when no data exists', () => {
    const loaded = loadUnreadCounts('nonexistent');
    expect(loaded).toEqual({});
  });

  it('should isolate counts by node address', () => {
    const node1Counts = { 'dm:100': 5 };
    const node2Counts = { 'dm:200': 3 };

    saveUnreadCounts('NODE1', node1Counts);
    saveUnreadCounts('NODE2', node2Counts);

    expect(loadUnreadCounts('NODE1')).toEqual(node1Counts);
    expect(loadUnreadCounts('NODE2')).toEqual(node2Counts);
  });

  it('should clear counts for specific node', () => {
    const counts = { 'dm:100': 5 };
    saveUnreadCounts(testNodeAddr, counts);
    
    clearUnreadCounts(testNodeAddr);
    const loaded = loadUnreadCounts(testNodeAddr);
    
    expect(loaded).toEqual({});
  });

  it('should handle undefined node address gracefully', () => {
    expect(() => saveUnreadCounts(undefined, { 'dm:100': 5 })).not.toThrow();
    expect(loadUnreadCounts(undefined)).toEqual({});
    expect(() => clearUnreadCounts(undefined)).not.toThrow();
  });
});
