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
