/**
 * Trust-anchor share-string codecs, mirroring networkKeyShare.ts. Three schemes:
 *
 *   bramble://anchor/v1?sk={seed_hex}          the operator's SECRET anchor seed
 *   bramble://ident/v1?pk={ed25519_pub_hex}    a node's PUBLIC identity key
 *   bramble://endorse/v1?na={na_hex}&sig={sig_hex}   an endorsement cert
 *
 * The anchor backup carries the private seed (offline backup, never handed to a
 * node). The identity share and the cert share are both public: the node's key
 * travels out for remote enrollment, and the cert travels back to the node.
 *
 * Each parse is strict (exact hex lengths) and returns null on any malformed
 * input, so callers can branch on a single nullish check.
 */
import { parseShareParams } from './channelShare';

const ANCHOR_PREFIX = 'bramble://anchor/v1?';
const IDENT_PREFIX = 'bramble://ident/v1?';
const ENDORSE_PREFIX = 'bramble://endorse/v1?';
const HEX64 = /^[0-9a-fA-F]{64}$/;
const HEX16 = /^[0-9a-fA-F]{16}$/;
const HEX128 = /^[0-9a-fA-F]{128}$/;

// These codecs report malformed input as null rather than the ParseResult
// error strings the channel/network-key codecs surface, so adapt the shared
// prefix-check + query decode down to a nullish result.
function paramsFor(input: string, prefix: string): URLSearchParams | null {
  const parsed = parseShareParams(input, prefix, '');
  return parsed.ok ? parsed.data : null;
}

function hexParam(params: URLSearchParams, key: string, re: RegExp): string | null {
  const raw = params.get(key);
  if (!raw) return null;
  const v = raw.trim();
  return re.test(v) ? v.toLowerCase() : null;
}

/** Encode the operator's SECRET anchor seed for offline backup. */
export function encodeAnchorBackup(seedHex: string): string {
  const params = new URLSearchParams();
  params.set('sk', seedHex.trim().toLowerCase());
  return `${ANCHOR_PREFIX}${params.toString()}`;
}

export function parseAnchorBackup(input: string): string | null {
  const params = paramsFor(input, ANCHOR_PREFIX);
  return params ? hexParam(params, 'sk', HEX64) : null;
}

/** Encode a node's PUBLIC Ed25519 identity key (not secret). */
export function encodeIdentityShare(ed25519PubHex: string): string {
  const params = new URLSearchParams();
  params.set('pk', ed25519PubHex.trim().toLowerCase());
  return `${IDENT_PREFIX}${params.toString()}`;
}

export function parseIdentityShare(input: string): string | null {
  const params = paramsFor(input, IDENT_PREFIX);
  return params ? hexParam(params, 'pk', HEX64) : null;
}

/** Encode an endorsement cert (not secret) to travel back to the node. */
export function encodeCertShare(notAfterHex: string, sigHex: string): string {
  const params = new URLSearchParams();
  params.set('na', notAfterHex.trim().toLowerCase());
  params.set('sig', sigHex.trim().toLowerCase());
  return `${ENDORSE_PREFIX}${params.toString()}`;
}

export function parseCertShare(input: string): { notAfterHex: string; sigHex: string } | null {
  const params = paramsFor(input, ENDORSE_PREFIX);
  if (!params) return null;
  const notAfterHex = hexParam(params, 'na', HEX16);
  const sigHex = hexParam(params, 'sig', HEX128);
  if (!notAfterHex || !sigHex) return null;
  return { notAfterHex, sigHex };
}
