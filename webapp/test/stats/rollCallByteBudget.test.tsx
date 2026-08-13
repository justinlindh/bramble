import { beforeEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import { RollCallPanel } from '../../src/pages/Stats/RollCallPanel';
import { utf8Length, ROLLCALL_TEXT_FALLBACK_BYTES } from '../../src/utils/byteLimit';

const loadRollCall = vi.fn();
const startRollCall = vi.fn();

vi.mock('../../src/store/actions', () => ({
  loadRollCall: (...args: unknown[]) => loadRollCall(...args),
  startRollCall: (...args: unknown[]) => startRollCall(...args),
}));

let state: any;
vi.mock('../../src/store/index', () => ({
  useStore: (selector: any) => selector(state),
}));

describe('roll-call payload byte budget', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    state = {
      connectionState: 'connected',
      peerNames: new Map<number, string>(),
    };
  });

  /*
   * max_text_bytes only arrives with the first getRollCall response. Typing is
   * possible before that resolves, and an unbounded field in that window
   * reproduces the same -32602 the byte clamp exists to prevent, so the
   * firmware constant stands in until the node reports its own.
   */
  it('bounds the payload before the node has reported its cap', () => {
    loadRollCall.mockReturnValue(new Promise(() => {})); // never resolves

    render(<RollCallPanel />);
    const input = screen.getByLabelText('Roll-call message') as HTMLInputElement;

    fireEvent.change(input, { target: { value: '日'.repeat(40) } });

    expect(utf8Length(input.value)).toBeLessThanOrEqual(ROLLCALL_TEXT_FALLBACK_BYTES);
    expect(input.value).not.toContain('�');
  });

  it('counts the payload in bytes rather than characters', () => {
    loadRollCall.mockReturnValue(new Promise(() => {}));

    render(<RollCallPanel />);
    const input = screen.getByLabelText('Roll-call message') as HTMLInputElement;

    fireEvent.change(input, { target: { value: 'héllo' } });

    // Five characters, six bytes.
    expect(screen.getByTestId('rollcall-byte-count').textContent).toContain('6/');
  });
});
