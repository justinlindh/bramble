import { describe, it, expect } from 'vitest';
import {
  encodeAnchorBackup,
  parseAnchorBackup,
  encodeIdentityShare,
  parseIdentityShare,
  encodeCertShare,
  parseCertShare,
} from '../anchorShare';

const SEED = 'ab'.repeat(32); // 64 hex
const PUB = 'cd'.repeat(32); // 64 hex
const NA = 'ffffffffffffffff'; // 16 hex
const SIG = 'ef'.repeat(64); // 128 hex

describe('anchor backup codec (carries the private seed)', () => {
  it('round-trips a 32-byte seed', () => {
    const uri = encodeAnchorBackup(SEED);
    expect(uri).toBe(`bramble://anchor/v1?sk=${SEED}`);
    expect(parseAnchorBackup(uri)).toBe(SEED);
  });

  it('normalizes uppercase hex to lowercase on parse', () => {
    expect(parseAnchorBackup(`bramble://anchor/v1?sk=${'AB'.repeat(32)}`)).toBe(SEED);
  });

  it('rejects a non-anchor share string', () => {
    expect(parseAnchorBackup(`bramble://net/v1?k=${SEED}`)).toBeNull();
  });

  it('rejects wrong-length or non-hex seed', () => {
    expect(parseAnchorBackup('bramble://anchor/v1?sk=abcd')).toBeNull();
    expect(parseAnchorBackup(`bramble://anchor/v1?sk=${'zz'.repeat(32)}`)).toBeNull();
  });
});

describe('identity share codec (node public key)', () => {
  it('round-trips a 32-byte public key', () => {
    const uri = encodeIdentityShare(PUB);
    expect(uri).toBe(`bramble://ident/v1?pk=${PUB}`);
    expect(parseIdentityShare(uri)).toBe(PUB);
  });

  it('normalizes uppercase hex to lowercase on parse', () => {
    expect(parseIdentityShare(`bramble://ident/v1?pk=${'CD'.repeat(32)}`)).toBe(PUB);
  });

  it('rejects a non-identity share string', () => {
    expect(parseIdentityShare(`bramble://anchor/v1?sk=${PUB}`)).toBeNull();
  });

  it('rejects wrong-length or non-hex key', () => {
    expect(parseIdentityShare('bramble://ident/v1?pk=abcd')).toBeNull();
    expect(parseIdentityShare(`bramble://ident/v1?pk=${'zz'.repeat(32)}`)).toBeNull();
  });
});

describe('cert share codec (endorsement travelling back to the node)', () => {
  it('round-trips not_after + signature', () => {
    const uri = encodeCertShare(NA, SIG);
    expect(uri).toBe(`bramble://endorse/v1?na=${NA}&sig=${SIG}`);
    expect(parseCertShare(uri)).toEqual({ notAfterHex: NA, sigHex: SIG });
  });

  it('normalizes uppercase hex to lowercase on parse', () => {
    const r = parseCertShare(`bramble://endorse/v1?na=${'FF'.repeat(8)}&sig=${'EF'.repeat(64)}`);
    expect(r).toEqual({ notAfterHex: NA, sigHex: SIG });
  });

  it('rejects a non-cert share string', () => {
    expect(parseCertShare(`bramble://ident/v1?pk=${PUB}`)).toBeNull();
  });

  it('rejects a missing field', () => {
    expect(parseCertShare(`bramble://endorse/v1?na=${NA}`)).toBeNull();
    expect(parseCertShare(`bramble://endorse/v1?sig=${SIG}`)).toBeNull();
  });

  it('rejects wrong-length not_after or signature', () => {
    expect(parseCertShare(`bramble://endorse/v1?na=ff&sig=${SIG}`)).toBeNull();
    expect(parseCertShare(`bramble://endorse/v1?na=${NA}&sig=abcd`)).toBeNull();
  });

  it('rejects non-hex fields', () => {
    expect(parseCertShare(`bramble://endorse/v1?na=${'zz'.repeat(8)}&sig=${SIG}`)).toBeNull();
    expect(parseCertShare(`bramble://endorse/v1?na=${NA}&sig=${'zz'.repeat(64)}`)).toBeNull();
  });
});
