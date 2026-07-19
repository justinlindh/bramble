import { describe, it, expect, beforeEach } from 'vitest';
import { mergeFirmwareMessages } from '../../src/store/actions';
import { useStore } from '../../src/store/index';
import type { IncomingMessage, Message } from '../../src/types/bramble';

const PEER = 0xa1b2c3d4;
const ME = 0x0badcafe;

/**
 * Shape a row the way `bramble.getMessages` actually emits it: hex address
 * strings, a whole-second `timestamp_s`, and crucially no `msgId`, which the
 * firmware handler never sets, so the synthetic fallback id is the only id
 * these rows ever get.
 */
function fwRow(overrides: Partial<Record<string, unknown>> = {}): IncomingMessage {
  return {
    from: 'A1B2C3D4',
    to: '0BADCAFE',
    direction: 'incoming',
    text: 'hello',
    channel: -1,
    timestamp_s: 1000,
    ...overrides,
  } as unknown as IncomingMessage;
}

function ctx(existing: Message[] = []) {
  return { existing, deviceUptime: 1000, myAddr: ME, now: 1_700_000_000_000 };
}

describe('mergeFirmwareMessages', () => {
  beforeEach(() => {
    useStore.setState({ messages: [], conversations: new Map() });
  });

  it('keeps both messages when one peer sends twice within the same second', () => {
    // The regression: timestamp_s has one-second resolution, so both rows used
    // to collapse onto id `fw-1000-<peer>` and addMessage dropped the second.
    const merged = mergeFirmwareMessages(
      [
        fwRow({ text: 'first', timestamp_s: 1000 }),
        fwRow({ text: 'second', timestamp_s: 1000 }),
      ],
      ctx(),
    );

    expect(merged).toHaveLength(2);
    expect(merged.map(m => m.text)).toEqual(['first', 'second']);
    // Distinct ids are the load-bearing part: addMessage dedupes on id, so
    // colliding ids would lose the second message on the way into the store.
    expect(new Set(merged.map(m => m.id)).size).toBe(2);

    // Drive the real store to prove the message actually survives the write.
    for (const m of merged) useStore.getState().addMessage(m);
    expect(useStore.getState().messages.map(m => m.text)).toEqual(['first', 'second']);
  });

  it('still dedupes a genuine duplicate already present in the store', () => {
    const already: Message = {
      id: 'cached-1',
      direction: 'incoming',
      from: PEER,
      to: ME,
      text: 'hello',
      tier: 'normal',
      timestampMs: 1_700_000_000_000,
      status: 'delivered',
    };

    const merged = mergeFirmwareMessages([fwRow({ text: 'hello' })], ctx([already]));

    expect(merged).toHaveLength(0);
  });

  it('dedupes a duplicate that appears twice inside the same batch', () => {
    // The frozen-snapshot half of the bug: the old loop compared every row
    // against a pre-loop store snapshot, so an intra-batch repeat was invisible.
    const merged = mergeFirmwareMessages(
      [fwRow({ text: 'same text' }), fwRow({ text: 'same text' })],
      ctx(),
    );

    expect(merged).toHaveLength(1);
  });

  it('preserves a cached message rather than re-adding it across two polls', () => {
    const first = mergeFirmwareMessages([fwRow({ text: 'ping' })], ctx());
    expect(first).toHaveLength(1);

    // Second poll returns the same ring contents: nothing new should surface.
    const second = mergeFirmwareMessages([fwRow({ text: 'ping' })], ctx(first));
    expect(second).toHaveLength(0);
  });

  it('honours a firmware-supplied msgId over the synthetic fallback', () => {
    const merged = mergeFirmwareMessages([fwRow({ msgId: 'real-id-7' })], ctx());

    expect(merged).toHaveLength(1);
    expect(merged[0]!.id).toBe('real-id-7');
  });

  it('drops self-addressed rows', () => {
    const merged = mergeFirmwareMessages(
      [fwRow({ from: '0BADCAFE', to: '0BADCAFE', text: 'echo' })],
      ctx(),
    );

    expect(merged).toHaveLength(0);
  });
});
