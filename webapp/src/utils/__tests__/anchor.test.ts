import { describe, it, expect } from 'vitest';
import {
  generateAnchorKeypair,
  anchorPubFromSeed,
  anchorFingerprint,
  endorsementMessage,
  signEndorsement,
  PERMANENT_NOT_AFTER,
} from '../anchor';

// Fixed KAT vectors, byte-identical to the firmware's
// test/test_identity_endorsement.c. The whole point of this file is byte parity
// with identity_endorsement_verify: a cert this module signs MUST be accepted by
// the firmware, so we reproduce the firmware's pinned signature exactly.
const KAT_ANCHOR_SEED =
  '000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f';
const KAT_NODE_PUB =
  '404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f';
// KAT_SIG_PERMANENT: signature over KAT_NODE_PUB with not_after = PERMANENT.
const KAT_SIG_PERMANENT =
  '016e65eae269ec3b1465252b33d526c1d9157d39dfff5f9009c71bb6118a85b3' +
  '7a36afd28bc2f36869f2bba54b601c79cc81213dcc2c41b76ec32ab74740b903';
// The anchor pubkey KAT_ANCHOR_SEED derives to. This is the RFC 8032 expansion
// of the seed and MUST match crypto_ed25519_keypair_from_seed(KAT_ANCHOR_SEED)
// in the firmware (same expansion as libsodium/OpenSSL). Do NOT "fix" these
// bytes: they are pinned to the firmware, transitively guarded by the signature
// KAT above (KAT_SIG_PERMANENT is produced by this same seed expansion), and a
// change here that still passed would mean the webapp and firmware had silently
// diverged on key derivation. If this constant ever mismatches, the seed
// expansion is wrong, not the vector.
const KAT_ANCHOR_PUB =
  '03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8';

describe('anchor crypto parity KAT (load-bearing)', () => {
  it('signs the firmware KAT byte-for-byte', () => {
    const { notAfterHex, sigHex } = signEndorsement(
      KAT_ANCHOR_SEED,
      KAT_NODE_PUB,
      PERMANENT_NOT_AFTER,
    );
    // If this fails, the message construction (context bytes / order /
    // big-endian not_after) or the Ed25519 signing is wrong: fix the builder,
    // NEVER the expected bytes.
    expect(sigHex).toBe(KAT_SIG_PERMANENT);
    expect(notAfterHex).toBe('ffffffffffffffff');
  });

  it('derives the KAT anchor pubkey from the seed (seed-expansion parity)', () => {
    expect(anchorPubFromSeed(KAT_ANCHOR_SEED)).toBe(KAT_ANCHOR_PUB);
  });
});

describe('endorsementMessage layout', () => {
  it('builds the exact 58-byte message with big-endian not_after', () => {
    const msg = endorsementMessage(KAT_NODE_PUB, 0x0102030405060708n);
    expect(msg.length).toBe(58);
    // context(18) = ASCII "bramble-endorse-v1", no NUL
    expect(new TextDecoder().decode(msg.slice(0, 18))).toBe('bramble-endorse-v1');
    // node pubkey(32)
    expect(bytesToHex(msg.slice(18, 50))).toBe(KAT_NODE_PUB);
    // not_after(8) big-endian
    expect(bytesToHex(msg.slice(50, 58))).toBe('0102030405060708');
  });

  it('encodes PERMANENT not_after as all-ones big-endian', () => {
    const msg = endorsementMessage(KAT_NODE_PUB, PERMANENT_NOT_AFTER);
    expect(bytesToHex(msg.slice(50, 58))).toBe('ffffffffffffffff');
  });
});

describe('anchorFingerprint', () => {
  it('is SHA256(pub)[0:4] as 8 lowercase hex', () => {
    // Pinned constant: SHA256 of the 32-byte KAT node pub, first 4 bytes. Must
    // equal the firmware's identity_anchor_fingerprint over the same key.
    expect(anchorFingerprint(KAT_NODE_PUB)).toBe('ca2a4fe7');
    expect(anchorFingerprint(KAT_NODE_PUB)).toMatch(/^[0-9a-f]{8}$/);
  });
});

describe('keypair generation', () => {
  it('round-trips generate then anchorPubFromSeed', () => {
    const { seedHex, pubHex } = generateAnchorKeypair();
    expect(seedHex).toMatch(/^[0-9a-f]{64}$/);
    expect(pubHex).toMatch(/^[0-9a-f]{64}$/);
    expect(anchorPubFromSeed(seedHex)).toBe(pubHex);
  });

  it('draws a fresh seed each call', () => {
    expect(generateAnchorKeypair().seedHex).not.toBe(generateAnchorKeypair().seedHex);
  });
});

describe('PERMANENT_NOT_AFTER', () => {
  it('is UINT64_MAX', () => {
    expect(PERMANENT_NOT_AFTER).toBe(0xffffffffffffffffn);
    expect(PERMANENT_NOT_AFTER.toString(16)).toBe('ffffffffffffffff');
  });
});

// test-local hex helper (independent of the module under test)
function bytesToHex(b: Uint8Array): string {
  return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}
