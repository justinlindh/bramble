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

describe('useSimulation btn wiring', () => {
  beforeEach(() => {
    MockWebSocket.last = null;
    vi.stubGlobal('WebSocket', MockWebSocket as unknown as typeof WebSocket);
  });
  afterEach(() => {
    vi.unstubAllGlobals();
  });

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

  beforeEach(() => {
    MockWebSocket.last = null;
    vi.stubGlobal('WebSocket', MockWebSocket as unknown as typeof WebSocket);
  });
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  function decode(type: string, raw: Record<string, unknown>) {
    const { result } = renderHook(() => useSimulation());
    act(() => {
      MockWebSocket.last!.emit({ type, ...raw });
    });
    return result.current.state.metrics;
  }

  it('maps every counter and derives the delivery rate', () => {
    expect(decode('metrics', frame)).toEqual({
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

  it('decodes final_metrics the same way as metrics', () => {
    expect(decode('final_metrics', frame)).toEqual(decode('metrics', frame));
  });

  it('defaults absent counters and avoids dividing by zero messages sent', () => {
    expect(decode('metrics', { timestamp_us: 1_000 })).toEqual({
      timestamp_us: 1_000,
      activeNodes: 0,
      totalPackets: 0,
      messagesSent: 0,
      delivered: 0,
      dropped: 0,
      avgLatencyMs: 0,
      deliveryRate: 0,
      retried: undefined,
      deliveredOnRetry: undefined,
      dedupDropped: undefined,
      airtimeDeferred: undefined,
      fragmentsSent: undefined,
      fragmentsReassembled: undefined,
      cryptoEncrypted: undefined,
      cryptoDecrypted: undefined,
    });
  });
});
