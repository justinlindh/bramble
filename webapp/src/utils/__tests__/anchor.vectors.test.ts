import { describe, it, expect } from 'vitest';
import { anchorPubFromSeed, anchorFingerprint, signEndorsement } from '../anchor';

/**
 * Cross-version signing vectors (issue #191).
 *
 * These vectors were generated with the PRE-migration dependency set,
 * @noble/hashes 1.8.0 + @noble/ed25519 2.3.0, by running the exact anchor.ts
 * logic as it existed on main before the v2/v3 bump (including the old
 * ed.etc.sha512Sync variadic hook, which v3 removed). They pin the property
 * that actually matters for a crypto dependency bump: for the same seed and
 * inputs, certificates signed AFTER a migration are byte-identical to
 * certificates signed BEFORE it. A sign-then-verify round trip with the new
 * library proves nothing about certs already in the wild; exact hex equality
 * against pre-migration output does.
 *
 * The first vector doubles as a cross-check: it uses the firmware KAT seed
 * and node pub, and its signature and pubkey match anchor.test.ts and the
 * firmware's test_identity_endorsement.c, so the generator provably ran the
 * same construction. If any assertion here fails after a future noble bump,
 * the new library changed hashing, key expansion, or signing behavior: fix
 * the migration, NEVER these bytes.
 */
const VECTORS = [
  {
    name: 'firmware KAT seed, permanent cert',
    seedHex: '000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
    nodePubHex: '404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f',
    notAfter: 0xffffffffffffffffn,
    notAfterHex: 'ffffffffffffffff',
    anchorPubHex: '03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8',
    fingerprint: '56475aa7',
    sigHex:
      '016e65eae269ec3b1465252b33d526c1d9157d39dfff5f9009c71bb6118a85b3' +
      '7a36afd28bc2f36869f2bba54b601c79cc81213dcc2c41b76ec32ab74740b903',
  },
  {
    name: 'repeated-byte seed, finite not_after',
    seedHex: '2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a',
    nodePubHex: 'b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf',
    notAfter: 0x0102030405060708n,
    notAfterHex: '0102030405060708',
    anchorPubHex: '197f6b23e16c8532c6abc838facd5ea789be0c76b2920334039bfa8b3d368d61',
    fingerprint: 'b600306c',
    sigHex:
      '64776b4d710c960987d515eb534ebdc4564af8945ef72773b3b095f14349bf21' +
      'e244cc7b871f5904ee915d1af4e240af9cdd343a4febe3051e5edee981aa240b',
  },
  {
    name: 'descending-byte seed, timestamp not_after',
    seedHex: 'fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0',
    nodePubHex: '101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f',
    notAfter: 0x00000000699a0a00n,
    notAfterHex: '00000000699a0a00',
    anchorPubHex: 'bafc71bead3ac5e4b63e9c8216ee71a34aaec65722eedbca728b4e9b3ccce396',
    fingerprint: '2642177f',
    sigHex:
      'a8c526f8313f2993b9eec11ec20f419340dcdb56ba6217c95b67b8bb31567abe' +
      '23ef8a6f98fa5adeeff3a769aa0a54d540ebd0d49f6f4cec54460a18dbb1bb04',
  },
] as const;

describe('cross-version signing vectors (pre-migration noble v1/v2 output)', () => {
  for (const v of VECTORS) {
    describe(v.name, () => {
      it('derives the identical anchor pubkey from the seed', () => {
        expect(anchorPubFromSeed(v.seedHex)).toBe(v.anchorPubHex);
      });

      it('derives the identical anchor fingerprint', () => {
        expect(anchorFingerprint(v.anchorPubHex)).toBe(v.fingerprint);
      });

      it('signs the identical certificate bytes', () => {
        const { notAfterHex, sigHex } = signEndorsement(v.seedHex, v.nodePubHex, v.notAfter);
        expect(notAfterHex).toBe(v.notAfterHex);
        expect(sigHex).toBe(v.sigHex);
      });
    });
  }
});
