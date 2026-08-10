import { beforeEach, describe, expect, it } from 'vitest';
import {
  listDevices, upsertDevice, renameDevice, forgetDevice,
  getDeviceToken, setDeviceToken, clearDeviceToken, type SavedDevice,
} from '../deviceBook';

beforeEach(() => { localStorage.clear(); sessionStorage.clear(); });

describe('deviceBook', () => {
  it('upserts and lists by most-recent first', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false, lastIp: '192.0.2.1', nowMs: 100 });
    upsertDevice({ address: 'BBBB0002', transport: 'wifi', remember: false, lastIp: '192.0.2.2', nowMs: 200 });
    const list = listDevices();
    expect(list.map(d => d.address)).toEqual(['BBBB0002', 'AAAA0001']);
    expect(list[0].name).toBe('Node BBBB0002'); // default name
  });

  it('upsert merges by address and does not clobber a user-set name with an auto name', () => {
    upsertDevice({ address: 'AAAA0001', name: 'Node A', transport: 'wifi', remember: false, nowMs: 1 });
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: true, lastIp: '192.0.2.9', nowMs: 2 });
    const d = listDevices()[0];
    expect(d.name).toBe('Node A');       // preserved
    expect(d.lastIp).toBe('192.0.2.9');  // updated
    expect(d.remember).toBe(true);       // updated
    expect(listDevices()).toHaveLength(1); // merged, not duplicated
  });

  it('rename updates the name', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false, nowMs: 1 });
    renameDevice('AAAA0001', 'Kitchen');
    expect(listDevices()[0].name).toBe('Kitchen');
  });

  it('token routes to localStorage when remembered, sessionStorage when not', () => {
    setDeviceToken('AAAA0001', 'tok-remember', true);
    expect(localStorage.getItem('bramble.deviceToken.AAAA0001')).toBe('tok-remember');
    expect(sessionStorage.getItem('bramble.deviceToken.AAAA0001')).toBeNull();
    setDeviceToken('BBBB0002', 'tok-session', false);
    expect(sessionStorage.getItem('bramble.deviceToken.BBBB0002')).toBe('tok-session');
    expect(localStorage.getItem('bramble.deviceToken.BBBB0002')).toBeNull();
    expect(getDeviceToken('AAAA0001')).toBe('tok-remember');
    expect(getDeviceToken('BBBB0002')).toBe('tok-session');
  });

  it('flipping remember off clears the persisted localStorage copy', () => {
    setDeviceToken('AAAA0001', 'tok', true);
    setDeviceToken('AAAA0001', 'tok', false);
    expect(localStorage.getItem('bramble.deviceToken.AAAA0001')).toBeNull();
    expect(sessionStorage.getItem('bramble.deviceToken.AAAA0001')).toBe('tok');
  });

  it('forget removes the entry and both token stores', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: true, nowMs: 1 });
    setDeviceToken('AAAA0001', 'tok', true);
    sessionStorage.setItem('bramble.deviceToken.AAAA0001', 'stale');
    forgetDevice('AAAA0001');
    expect(listDevices()).toHaveLength(0);
    expect(localStorage.getItem('bramble.deviceToken.AAAA0001')).toBeNull();
    expect(sessionStorage.getItem('bramble.deviceToken.AAAA0001')).toBeNull();
  });

  it('clearDeviceToken removes the token from both storages and keeps the book entry', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'ble', remember: true, nowMs: 1 });
    setDeviceToken('AAAA0001', 'tok', true);
    sessionStorage.setItem('bramble.deviceToken.AAAA0001', 'stale');
    clearDeviceToken('AAAA0001');
    expect(localStorage.getItem('bramble.deviceToken.AAAA0001')).toBeNull();
    expect(sessionStorage.getItem('bramble.deviceToken.AAAA0001')).toBeNull();
    expect(getDeviceToken('AAAA0001')).toBe('');
    expect(listDevices()).toHaveLength(1); // only the token goes; Forget removes the entry
  });

  it('degrades gracefully when storage throws', () => {
    const orig = Storage.prototype.setItem;
    Storage.prototype.setItem = () => { throw new Error('quota'); };
    expect(() => upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false })).not.toThrow();
    Storage.prototype.setItem = orig;
  });
});
