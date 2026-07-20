import { describe, it, expect, beforeEach } from 'vitest';
import { mergeFirmwareMessages } from '../../src/store/actions';
import { useStore } from '../../src/store/index';
import type { Message } from '../../src/types/bramble';

const PEER = 0xa1b2c3d4;
const ME = 0x0badcafe;

// The wire row type mergeFirmwareMessages accepts, derived from its signature
// so this test tracks the real contract instead of restating it.
type FwRow = Parameters<typeof mergeFirmwareMessages>[0][number];

/**
 * Shape a row the way `bramble.getMessages` actually emits it: hex address
 * strings, a whole-second `timestamp_s`, and crucially no `msgId`, which the
 * firmware handler never sets, so the synthetic fallback id is the only id
 * these rows ever get.
 */
function fwRow(overrides: Partial<Record<string, unknown>> = {}): FwRow {
  return {
    from: 'A1B2C3D4',
    to: '0BADCAFE',
    direction: 'incoming',
    text: 'hello',
    channel: -1,
    timestamp_s: 1000,
    ...overrides,
  } as unknown as FwRow;
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

  it('keeps two identical-text rows in one batch, since each is a real message', () => {
    // A user really can send "ok" twice a few seconds apart. Both rows occupy
    // distinct ring slots and both come back in one response, so content-based
    // dedup must NOT be applied within a batch or the repeat is swallowed.
    const merged = mergeFirmwareMessages(
      [fwRow({ text: 'ok', timestamp_s: 1000 }), fwRow({ text: 'ok', timestamp_s: 1003 })],
      ctx(),
    );

    expect(merged).toHaveLength(2);
    expect(merged.map(m => m.text)).toEqual(['ok', 'ok']);
    expect(new Set(merged.map(m => m.id)).size).toBe(2);

    for (const m of merged) useStore.getState().addMessage(m);
    expect(useStore.getState().messages).toHaveLength(2);
  });

  it('does not re-add duplicate-text rows when the same ring is polled twice', () => {
    // Removing the intra-batch check must not reintroduce cross-poll growth:
    // the ctx.existing content match is what catches re-fetched rows, whose
    // ring indices (and therefore synthetic ids) shift as the ring rotates.
    const ring = [
      fwRow({ text: 'ok', timestamp_s: 1000 }),
      fwRow({ text: 'ok', timestamp_s: 1003 }),
    ];

    const first = mergeFirmwareMessages(ring, ctx());
    expect(first).toHaveLength(2);

    const second = mergeFirmwareMessages(ring, ctx(first));
    expect(second).toHaveLength(0);
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
