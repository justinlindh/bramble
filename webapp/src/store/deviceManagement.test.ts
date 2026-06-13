import { describe, it, expect } from 'vitest';
import * as actions from './actions';

// The device-management actions (issue #95) guard on an active client. Without
// a connection they must reject rather than silently no-op, so the UI surfaces
// the error. Exercising the live RPC mapping is covered end to end against the
// mock node in the e2e smoke and the mock handler tests.
describe('device management actions (issue #95)', () => {
  it('exposes the auth, origins, and OTA actions', () => {
    for (const fn of [
      'getAuthToken', 'setAuthToken',
      'getAllowedOrigins', 'setAllowedOrigins',
      'getOtaOrigin', 'setOtaOrigin', 'resetOtaOrigin', 'startOtaUpdate',
    ] as const) {
      expect(typeof actions[fn]).toBe('function');
    }
  });

  it('rejects when not connected', async () => {
    await expect(actions.getAuthToken()).rejects.toThrow('Not connected');
    await expect(actions.setAuthToken('x')).rejects.toThrow('Not connected');
    await expect(actions.getAllowedOrigins()).rejects.toThrow('Not connected');
    await expect(actions.setAllowedOrigins([])).rejects.toThrow('Not connected');
    await expect(actions.getOtaOrigin()).rejects.toThrow('Not connected');
    await expect(actions.setOtaOrigin('x')).rejects.toThrow('Not connected');
    await expect(actions.resetOtaOrigin()).rejects.toThrow('Not connected');
    await expect(actions.startOtaUpdate('x')).rejects.toThrow('Not connected');
  });
});
