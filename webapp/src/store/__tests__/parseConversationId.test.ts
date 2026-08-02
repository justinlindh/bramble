import { describe, it, expect } from 'vitest';
import { parseConversationId, formatConversationLabel } from '../index';
import type { BrambleConfig, Channel } from '../../types/bramble';

describe('parseConversationId', () => {
  it('decodes the broadcast bucket', () => {
    expect(parseConversationId('broadcast')).toEqual({ kind: 'broadcast' });
  });

  it('decodes a channel bucket to its numeric index', () => {
    expect(parseConversationId('ch:0')).toEqual({ kind: 'channel', index: 0 });
    expect(parseConversationId('ch:7')).toEqual({ kind: 'channel', index: 7 });
  });

  it('decodes a dm bucket to its numeric address', () => {
    expect(parseConversationId('dm:3735928559')).toEqual({ kind: 'dm', addr: 3735928559 });
  });

  it('reports an unrecognized id as unknown', () => {
    expect(parseConversationId('')).toEqual({ kind: 'unknown' });
    expect(parseConversationId('nonsense')).toEqual({ kind: 'unknown' });
  });
});

// The label policy the store applies to conversations, also rendered directly
// by the Chat header for channels that have no conversation entry yet.
describe('formatConversationLabel', () => {
  const channel = (index: number, name: string): Channel =>
    ({ index, name, hasPsk: false, epoch: 0, isDefault: false });
  const config = (...channels: Channel[]): BrambleConfig =>
    ({ channels } as BrambleConfig);

  it('labels the broadcast bucket', () => {
    expect(formatConversationLabel('broadcast')).toBe('Broadcast');
  });

  it('labels a channel by its configured name', () => {
    expect(formatConversationLabel('ch:2', undefined, config(channel(2, 'ops')))).toBe('ops');
  });

  it('falls back to ch-{index} when the channel name is missing or blank', () => {
    expect(formatConversationLabel('ch:2', undefined, config(channel(2, '  ')))).toBe('ch-2');
    expect(formatConversationLabel('ch:3', undefined, config())).toBe('ch-3');
    expect(formatConversationLabel('ch:3', undefined, null)).toBe('ch-3');
  });

  it('labels a dm by peer name, else the 0x hex address', () => {
    expect(formatConversationLabel('dm:3735928559', new Map([[3735928559, 'alice']]))).toBe('alice');
    expect(formatConversationLabel('dm:3735928559')).toBe('0xDEADBEEF');
  });

  it('passes an unknown id through unchanged', () => {
    expect(formatConversationLabel('nonsense')).toBe('nonsense');
  });
});
