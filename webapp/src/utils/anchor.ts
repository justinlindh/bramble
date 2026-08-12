/**
 * Trust-anchor crypto primitives for the operator's enrollment tool.
 *
 * The operator's client holds the fleet anchor PRIVATE seed (offline, never on a
 * node) and signs an endorsement cert over each node's Ed25519 identity key. The
 * single property that matters: a cert signed here MUST be byte-accepted by the
 * firmware's identity_endorsement_verify. The parity KAT in anchor.test.ts pins
 * that: it reproduces the firmware's exact signature bytes from a fixed seed.
 *
 * Endorsement signed message (LOCKED, matches the firmware byte-for-byte):
 *   ASCII "bramble-endorse-v1" (18) || node_ed25519_pub (32) || not_after (8, BIG-ENDIAN)
 *   = 58 bytes. Ed25519-signed by the anchor key -> 64-byte endorsement_sig.
 *
 * not_after is a uint64: v1 always issues PERMANENT (0xFFFFFFFFFFFFFFFF). It is
 * carried as a bigint because UINT64_MAX does not fit a JS number losslessly.
 *
 * Pure and framework-free: the UI (P4b) and tests import it directly. The anchor
 * private seed never leaves this client and is never sent over any RPC.
 */
import * as ed from '@noble/ed25519';
import { sha256, sha512 } from '@noble/hashes/sha2.js';
import { bytesToHex, hexToBytes } from '@noble/hashes/utils.js';

// @noble/ed25519 v3 needs a synchronous sha512 hook for sync sign/getPublicKey.
// Wire it once at module load so signing is deterministic and offline.
ed.hashes.sha512 = sha512;

const ENDORSE_CONTEXT = new TextEncoder().encode('bramble-endorse-v1'); // 18 bytes
const HEX64 = /^[0-9a-fA-F]{64}$/;

/** v1 always issues a permanent cert. On the wire this is "ffffffffffffffff". */
export const PERMANENT_NOT_AFTER = 0xffffffffffffffffn;

function requireHex64(hex: string, label: string): string {
  const s = hex.trim().toLowerCase();
  if (!HEX64.test(s)) throw new Error(`${label} must be 64 hex chars`);
  return s;
}

/**
 * Draw a fresh 32-byte anchor seed and derive its Ed25519 public key. The SEED
 * is the backup secret: those 32 bytes reconstruct the whole key. Returns hex.
 */
export function generateAnchorKeypair(): { seedHex: string; pubHex: string } {
  const seed = new Uint8Array(32);
  crypto.getRandomValues(seed);
  return { seedHex: bytesToHex(seed), pubHex: bytesToHex(ed.getPublicKey(seed)) };
}

/** Deterministic anchor public key from a seed (import / restore). */
export function anchorPubFromSeed(seedHex: string): string {
  return bytesToHex(ed.getPublicKey(hexToBytes(requireHex64(seedHex, 'anchor seed'))));
}

/**
 * Anchor address / fingerprint = SHA256(anchor_pub)[0:4], 8 lowercase hex.
 * Matches the firmware's identity_anchor_fingerprint.
 */
export function anchorFingerprint(pubHex: string): string {
  return bytesToHex(sha256(hexToBytes(requireHex64(pubHex, 'anchor pub'))).slice(0, 4));
}

/**
 * Build the exact 58-byte endorsement message:
 *   context(18) || node_ed25519_pub(32) || not_after(8, big-endian).
 * not_after is a bigint so UINT64_MAX survives.
 */
export function endorsementMessage(nodeEd25519PubHex: string, notAfter: bigint): Uint8Array {
  const nodePub = hexToBytes(requireHex64(nodeEd25519PubHex, 'node pub'));
  const msg = new Uint8Array(ENDORSE_CONTEXT.length + 32 + 8); // 58
  msg.set(ENDORSE_CONTEXT, 0);
  msg.set(nodePub, ENDORSE_CONTEXT.length);
  const naOffset = ENDORSE_CONTEXT.length + 32;
  for (let i = 0; i < 8; i += 1) {
    // Big-endian: most-significant byte first.
    msg[naOffset + i] = Number((notAfter >> BigInt(8 * (7 - i))) & 0xffn);
  }
  return msg;
}

/**
 * Sign the 58-byte endorsement message with the seed-derived anchor key.
 * Returns not_after as 16 hex big-endian and the 64-byte signature as 128 hex.
 */
export function signEndorsement(
  anchorSeedHex: string,
  nodeEd25519PubHex: string,
  notAfter: bigint,
): { notAfterHex: string; sigHex: string } {
  const seed = hexToBytes(requireHex64(anchorSeedHex, 'anchor seed'));
  const msg = endorsementMessage(nodeEd25519PubHex, notAfter);
  const sig = ed.sign(msg, seed);
  return {
    notAfterHex: notAfter.toString(16).padStart(16, '0'),
    sigHex: bytesToHex(sig),
  };
}
