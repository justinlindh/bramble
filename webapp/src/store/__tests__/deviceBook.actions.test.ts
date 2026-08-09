// webapp/src/store/__tests__/deviceBook.actions.test.ts
import { beforeEach, describe, expect, it } from 'vitest';
import { useStore } from '../index';
import { refreshDevices, forgetSavedDevice, renameSavedDevice, saveConnectedDevice } from '../actions';
import { upsertDevice, setDeviceToken, getDeviceToken } from '../../lib/deviceBook';

beforeEach(() => { localStorage.clear(); sessionStorage.clear(); useStore.getState().setDevices([]); });

describe('device actions', () => {
  it('refreshDevices loads the book into the store', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false, nowMs: 1 });
    refreshDevices();
    expect(useStore.getState().devices.map(d => d.address)).toEqual(['AAAA0001']);
  });
  it('forgetSavedDevice removes it and refreshes the store', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false, nowMs: 1 });
    refreshDevices();
    forgetSavedDevice('AAAA0001');
    expect(useStore.getState().devices).toHaveLength(0);
  });
  it('saveConnectedDevice with a blank token and remember off clears a stale stored token', () => {
    // Reconnecting with the token field empty and Remember off must not leave
    // the previous session's localStorage token behind on a shared computer.
    setDeviceToken('0000A001', 'stale-tok', true);
    saveConnectedDevice({ addr: 0xA001, ip: '192.0.2.9', token: '', remember: false, transport: 'wifi' });
    expect(getDeviceToken('0000A001')).toBe('');
    expect(localStorage.getItem('bramble.deviceToken.0000A001')).toBeNull();
    expect(sessionStorage.getItem('bramble.deviceToken.0000A001')).toBeNull();
  });

  it('a serial connect never wipes a token the user remembered via wifi or BLE', () => {
    // Tokens are keyed by node address, not transport. Serial saves hard-code
    // token '' and remember false because the serial form has no token or
    // Remember control, so that combination expresses no revocation intent:
    // treating it as one deleted the node's remembered token on every USB
    // connect and broke the next one-click wifi/BLE row.
    upsertDevice({ address: '0000A001', transport: 'wifi', remember: true, lastIp: '192.0.2.9', nowMs: 1 });
    setDeviceToken('0000A001', 'kept-tok', true);
    saveConnectedDevice({ addr: 0xA001, ip: '', token: '', remember: false, transport: 'serial' });
    expect(getDeviceToken('0000A001')).toBe('kept-tok');
    expect(localStorage.getItem('bramble.deviceToken.0000A001')).toBe('kept-tok');
  });

  it('a serial connect preserves the entry\'s remember flag instead of forcing it off', () => {
    // The remember flag drives the one-click row's post-connect token save:
    // forcing it false here made the next BLE row connect demote the stored
    // token to sessionStorage, silently un-remembering it.
    upsertDevice({ address: '0000A001', transport: 'wifi', remember: true, lastIp: '192.0.2.9', nowMs: 1 });
    saveConnectedDevice({ addr: 0xA001, ip: '', token: '', remember: false, transport: 'serial' });
    refreshDevices();
    expect(useStore.getState().devices[0].remember).toBe(true);
  });

  it('renameSavedDevice updates the name in the store', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false, nowMs: 1 });
    refreshDevices();
    renameSavedDevice('AAAA0001', 'Shed');
    expect(useStore.getState().devices[0].name).toBe('Shed');
  });
});
