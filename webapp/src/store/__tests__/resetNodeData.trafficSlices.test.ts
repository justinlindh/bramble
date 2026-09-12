import { beforeEach, describe, expect, it } from 'vitest';
import { useStore } from '../index';
import type { TrafficEvent, TrafficDebugStatus } from '../../types/bramble';

// resetNodeData runs on every connect() to wipe the previous node's per-node
// state. The traffic-debug slices are per-node (their seq/ring counters come
// from the connected node's getTrafficDebug RPC), so they must be cleared too:
// otherwise the Traffic Monitor keeps the old node's events and addTrafficEvents
// merges the new node's low seq numbers into the stale list.
describe('resetNodeData clears per-node traffic slices', () => {
  beforeEach(() => {
    useStore.setState({ trafficEvents: [], trafficDebugStatus: null });
  });

  it('drops trafficEvents and trafficDebugStatus', () => {
    const event: TrafficEvent = {
      seq: 7,
      timestampMs: 1000,
      direction: 'tx',
      category: 'chat',
      packetType: 'DATA',
      tier: 'normal',
      airtimeBucket: 'normal',
      airtimeDebitUs: 1234,
    };
    const status: TrafficDebugStatus = {
      config: { enabled: true, includeTx: true, includeRx: true, sampleRate: 100 },
      ringSize: 512,
      ringUsed: 3,
      droppedCount: 0,
      lastSeq: 7,
    };
    useStore.setState({ trafficEvents: [event], trafficDebugStatus: status });

    useStore.getState().resetNodeData();

    expect(useStore.getState().trafficEvents).toEqual([]);
    expect(useStore.getState().trafficDebugStatus).toBeNull();
  });
});
