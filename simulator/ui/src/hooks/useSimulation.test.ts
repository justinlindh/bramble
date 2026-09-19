import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { renderHook, act } from '@testing-library/react';
import { useSimulation } from './useSimulation';

// A minimal WebSocket stand-in: reports OPEN, records every frame sent, and
// lets a test push an inbound message through the registered listener. This
// pins the exact wire contract the device view relies on (btn out, device_fb
// in) without a live broker.
class MockWebSocket {
  static OPEN = 1;
  static last: MockWebSocket | null = null;

  readyState = MockWebSocket.OPEN;
  sent: string[] = [];
  private listeners: Record<string, ((ev: unknown) => void)[]> = {};

  constructor(public url: string) {
    MockWebSocket.last = this;
  }
  addEventListener(type: string, cb: (ev: unknown) => void) {
    (this.listeners[type] ||= []).push(cb);
    if (type === 'open') cb(new Event('open'));
  }
  send(data: string) {
    this.sent.push(data);
  }
  close() {}
  // Test helper: deliver an inbound broker frame.
  emit(obj: unknown) {
    for (const cb of this.listeners['message'] || []) {
      cb({ data: JSON.stringify(obj) } as MessageEvent<string>);
    }
  }
}

beforeEach(() => {
  MockWebSocket.last = null;
  vi.stubGlobal('WebSocket', MockWebSocket as unknown as typeof WebSocket);
});
afterEach(() => {
  vi.unstubAllGlobals();
});

describe('useSimulation btn wiring', () => {
  it('sendButton emits the exact { type:"btn", node, id, edge } frame', () => {
    const { result } = renderHook(() => useSimulation());
    act(() => {
      result.current.sendButton('pager-0', 'select', 'down');
    });
    const sock = MockWebSocket.last!;
    // The button frame is the only thing this test sends.
    const frame = sock.sent.find((s) => s.includes('"btn"'));
    expect(frame).toBeDefined();
    expect(JSON.parse(frame!)).toEqual({
      type: 'btn',
      node: 'pager-0',
      id: 'select',
      edge: 'down',
    });
  });

  it('carries the release edge and every button id verbatim', () => {
    const { result } = renderHook(() => useSimulation());
    act(() => {
      result.current.sendButton('pager-1', 'up', 'up');
      result.current.sendButton('pager-1', 'down', 'down');
      result.current.sendButton('pager-1', 'reset', 'down');
    });
    const frames = MockWebSocket.last!.sent
      .filter((s) => s.includes('"btn"'))
      .map((s) => JSON.parse(s));
    expect(frames).toEqual([
      { type: 'btn', node: 'pager-1', id: 'up', edge: 'up' },
      { type: 'btn', node: 'pager-1', id: 'down', edge: 'down' },
      { type: 'btn', node: 'pager-1', id: 'reset', edge: 'down' },
    ]);
  });

  it('populates a device from an inbound device_fb frame', () => {
    const { result } = renderHook(() => useSimulation());
    act(() => {
      MockWebSocket.last!.emit({
        type: 'device_fb',
        node: 'pager-0',
        addr: '0x0000AB12',
        seq: 1,
        kind: 'full',
        fb: 'AAAA',
        busy_ms: 2600,
      });
    });
    const dev = result.current.state.devices.get('pager-0');
    expect(dev).toBeDefined();
    expect(dev!.fb).toBe('AAAA');
    expect(dev!.fbKind).toBe('full');
    expect(dev!.fbBusyMs).toBe(2600);
    expect(dev!.fbSeq).toBe(1);
    expect(dev!.addr).toBe('0x0000AB12');
  });
});

describe('useSimulation metrics decoding', () => {
  // A frame carrying both the required counters and every optional counter, so
  // a dropped field shows up as a missing key rather than a coincidental zero.
  const frame = {
    timestamp_us: 4_000_000,
    active_nodes: 6,
    total_packets: 240,
    messages_sent: 50,
    delivered: 40,
    dropped: 10,
    avg_latency_ms: 312.5,
    retried: 7,
    delivered_on_retry: 5,
    dedup_dropped: 3,
    airtime_deferred: 2,
    fragments_sent: 9,
    fragments_reassembled: 8,
    crypto_encrypted: 11,
    crypto_decrypted: 12,
  };

  // The counters final_metrics carries. It has no timestamp_us and no
  // active_nodes, matching what the simulator emits at the end of a run.
  const { timestamp_us: _ts, active_nodes: _active, ...finalFrame } = {
    ...frame,
    delivered: 45,
    dropped: 5,
  };

  function decode(...frames: Record<string, unknown>[]) {
    const { result } = renderHook(() => useSimulation());
    act(() => {
      for (const f of frames) MockWebSocket.last!.emit(f);
    });
    return result.current.state.metrics;
  }

  it('maps every counter and derives the delivery rate', () => {
    expect(decode({ type: 'metrics', ...frame })).toEqual({
      timestamp_us: 4_000_000,
      activeNodes: 6,
      totalPackets: 240,
      messagesSent: 50,
      delivered: 40,
      dropped: 10,
      avgLatencyMs: 312.5,
      deliveryRate: 80,
      retried: 7,
      deliveredOnRetry: 5,
      dedupDropped: 3,
      airtimeDeferred: 2,
      fragmentsSent: 9,
      fragmentsReassembled: 8,
      cryptoEncrypted: 11,
      cryptoDecrypted: 12,
    });
  });

  it('takes the counters from final_metrics and keeps the clock and node count', () => {
    const metrics = decode(
      { type: 'metrics', ...frame },
      { type: 'final_metrics', ...finalFrame },
    );
    expect(metrics).toMatchObject({
      timestamp_us: 4_000_000,
      activeNodes: 6,
      delivered: 45,
      dropped: 5,
      deliveryRate: 90,
      cryptoDecrypted: 12,
    });
  });

  it('decodes a final_metrics that arrives with no periodic tick before it', () => {
    expect(decode({ type: 'final_metrics', ...finalFrame })).toMatchObject({
      timestamp_us: 0,
      activeNodes: 0,
      delivered: 45,
      totalPackets: 240,
    });
  });

  it('defaults absent counters and avoids dividing by zero messages sent', () => {
    expect(decode({ type: 'metrics', timestamp_us: 1_000 })).toEqual({
      timestamp_us: 1_000,
      activeNodes: 0,
      totalPackets: 0,
      messagesSent: 0,
      delivered: 0,
      dropped: 0,
      avgLatencyMs: 0,
      deliveryRate: 0,
    });
  });
});
