import { describe, it, expect } from 'vitest';
import { resolveDeviceId } from './useSimulation';
import type { DeviceState } from '../types';

function dev(node: string): DeviceState {
  return {
    node, fb: null, fbKind: 'full', fbBusyMs: 0, fbSeq: 0,
    led: false, buzzerHz: 0, vibra: false, vibraSeq: 0, console: [],
  };
}

describe('resolveDeviceId', () => {
  const order = ['0D668E03', 'C7D35266', 'EAC769D1']; // firmware hello ids in attach order

  it('returns an id that is already a device key unchanged', () => {
    const devices = new Map([['0D668E03', dev('0D668E03')]]);
    expect(resolveDeviceId('0D668E03', devices, order)).toBe('0D668E03');
  });

  it('routes a process label "<label>-<i>" to the i-th firmware hello id', () => {
    const devices = new Map<string, DeviceState>();
    expect(resolveDeviceId('pager-0', devices, order)).toBe('0D668E03');
    expect(resolveDeviceId('pager-2', devices, order)).toBe('EAC769D1');
  });

  it('leaves an unmappable label as-is (out-of-range or no suffix)', () => {
    const devices = new Map<string, DeviceState>();
    expect(resolveDeviceId('pager-9', devices, order)).toBe('pager-9');
    expect(resolveDeviceId('weird', devices, order)).toBe('weird');
  });
});
