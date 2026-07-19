import { describe, it, expect } from 'vitest';
import { decodePacketType } from '../../src/store/actions';

describe('decodePacketType: numeric mappings (regression T37)', () => {
  it('0x01 → ack', () => expect(decodePacketType(0x01)).toBe('ack'));
  it('0x02 → rreq', () => expect(decodePacketType(0x02)).toBe('rreq'));
  it('0x03 → rrep', () => expect(decodePacketType(0x03)).toBe('rrep'));
  it('0x04 → rerr', () => expect(decodePacketType(0x04)).toBe('rerr'));
  it('0x05 → beacon', () => expect(decodePacketType(0x05)).toBe('beacon'));
  it('0x06 → key_exchange', () => expect(decodePacketType(0x06)).toBe('key_exchange'));
  it('0x07 → delivery_receipt', () => expect(decodePacketType(0x07)).toBe('delivery_receipt'));
  it('0x08 → congestion', () => expect(decodePacketType(0x08)).toBe('congestion'));
  it('0x09 → time_sync', () => expect(decodePacketType(0x09)).toBe('time_sync'));
  it('0x0A → data', () => expect(decodePacketType(0x0A)).toBe('data'));
  it('0x0B → store_request', () => expect(decodePacketType(0x0B)).toBe('store_request'));
  it('0x0C → store_ack', () => expect(decodePacketType(0x0C)).toBe('store_ack'));
  it('0x0D → mailbox_delivery', () => expect(decodePacketType(0x0D)).toBe('mailbox_delivery'));
  it('0x0E → mailbox_query', () => expect(decodePacketType(0x0E)).toBe('mailbox_query'));
  it('0x0F → emergency', () => expect(decodePacketType(0x0F)).toBe('emergency'));
  it('0x10 → emergency_cancel', () => expect(decodePacketType(0x10)).toBe('emergency_cancel'));
  it('0x11 → coded', () => expect(decodePacketType(0x11)).toBe('coded'));
  it('0x12 → probe', () => expect(decodePacketType(0x12)).toBe('probe'));
  it('0x13 → probe_ack', () => expect(decodePacketType(0x13)).toBe('probe_ack'));
  it('0x14 → location', () => expect(decodePacketType(0x14)).toBe('location'));
});

describe('decodePacketType: edge cases', () => {
  it('undefined → unknown', () => expect(decodePacketType(undefined)).toBe('unknown'));
  it('unknown number → unknown', () => expect(decodePacketType(0xFF)).toBe('unknown'));
  it('string passthrough: returns the string unchanged', () => {
    expect(decodePacketType('data')).toBe('data');
    expect(decodePacketType('beacon')).toBe('beacon');
    expect(decodePacketType('custom_type')).toBe('custom_type');
  });
});
