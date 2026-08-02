import { describe, it, expect } from 'vitest';
import { parseConversationId } from '../index';

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
