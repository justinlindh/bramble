// webapp/src/store/__tests__/deviceBook.actions.test.ts
import { beforeEach, describe, expect, it } from 'vitest';
import { useStore } from '../index';
import { refreshDevices, forgetSavedDevice, renameSavedDevice } from '../actions';
import { upsertDevice } from '../../lib/deviceBook';

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
  it('renameSavedDevice updates the name in the store', () => {
    upsertDevice({ address: 'AAAA0001', transport: 'wifi', remember: false, nowMs: 1 });
    refreshDevices();
    renameSavedDevice('AAAA0001', 'Shed');
    expect(useStore.getState().devices[0].name).toBe('Shed');
  });
});
