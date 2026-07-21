// Security surfaces: peer verification (SAS), network key provisioning, and
// the trust anchor. All of these are RPC pass-throughs: key material handling
// stays on the firmware side.
import { session } from './client';
import { useStore } from '../index';
import { formatAddrHex } from '../../utils/address';

// ─── Peer verification (SAS) ──────────────────────────────────────────────

/**
 * Fetch a peer's SAS/verification state and cache it in the store. An
 * unpinned peer (never DM'd, or DM'd before the identity handshake landed)
 * is not an error: the firmware returns an empty SAS and verified:false.
 */
export async function loadPeerVerification(
  addr: number,
): Promise<import('../../types/bramble').PeerVerification> {
  if (!session.client) throw new Error('Not connected');
  const result = await session.client.rpc('bramble.getPeerVerification', { address: formatAddrHex(addr) });
  const v: import('../../types/bramble').PeerVerification = {
    sas: result.sas ?? '',
    verified: !!result.verified,
    keyChanged: !!result.keyChanged,
  };
  useStore.getState().setPeerVerification(addr, v);
  return v;
}

/** Mark (or unmark) a peer verified. Refreshes the cached state on success. */
export async function setPeerVerified(addr: number, verified: boolean): Promise<boolean> {
  if (!session.client) throw new Error('Not connected');
  const result = await session.client.rpc('bramble.setPeerVerified', {
    address: formatAddrHex(addr),
    verified,
  });
  if (result?.ok) {
    await loadPeerVerification(addr).catch(() => {});
  }
  return !!result?.ok;
}

// ─── Network key provisioning ──────────────────────────────────────────────

/**
 * Push a freshly-generated network key to the device out-of-band (QR / paste).
 * The key is write-only: this call never returns the key, only whether the
 * device accepted it. Returns false rather than throwing on rejection so
 * callers can show an inline error instead of an unhandled promise rejection.
 */
export async function setNetworkKey(keyHex: string): Promise<boolean> {
  if (!session.client) throw new Error('Not connected');
  const result = await session.client.rpc('bramble.setNetworkKey', { key: keyHex });
  return !!result?.ok;
}

/**
 * Mint a fresh network key on the device and provision THIS node as the fleet
 * founder. The device generates the key from its entropy-gated source and
 * persists it; the raw key is returned once so the operator can copy it to the
 * other nodes. On an already-provisioned node this RE-KEYS it, so callers must
 * confirm first. The returned key is a secret: never log it.
 */
export async function generateNetworkKey(): Promise<{ key: string; fingerprint: string }> {
  if (!session.client) throw new Error('Not connected');
  return await session.client.rpc('bramble.generateNetworkKey');
}

/**
 * Refresh the global network-key provisioning status in the store. Polled from
 * the app shell so the UNPROVISIONED (inert) banner stays live on every tab.
 */
export async function loadNetworkKeyStatus(): Promise<void> {
  if (!session.client) return;
  const s = await session.client.rpc('bramble.getNetworkKeyStatus');
  useStore.getState().setNetworkKeyStatus(s);
}

// --- Trust anchor ----------------------------------------------------------

/**
 * Pin THIS node to a fleet trust anchor by its PUBLIC key. The anchor private
 * seed stays offline on the operator's client and is NEVER sent: setAnchor
 * carries the public key only. Returns false rather than throwing on rejection
 * so callers can show an inline error.
 */
export async function setAnchor(anchorPubHex: string): Promise<boolean> {
  if (!session.client) throw new Error('Not connected');
  const result = await session.client.rpc('bramble.setAnchor', { anchor_pubkey: anchorPubHex });
  return !!result?.ok;
}

export async function getIdentity(): Promise<import('../../types/bramble').NodeIdentityWire> {
  if (!session.client) throw new Error('Not connected');
  return await session.client.rpc('bramble.getIdentity');
}

/**
 * Install the endorsement cert the operator signed for this node. The cert is
 * public (the signature over the node's own identity key); it travels back to
 * the node so an anchored fleet will pin this identity. Returns false rather
 * than throwing on rejection.
 */
export async function setEndorsement(notAfterHex: string, sigHex: string): Promise<boolean> {
  if (!session.client) throw new Error('Not connected');
  const result = await session.client.rpc('bramble.setEndorsement', {
    not_after: notAfterHex,
    endorsement_sig: sigHex,
  });
  return !!result?.ok;
}

/**
 * Refresh the node's anchor provisioning status in the store, mirroring
 * loadNetworkKeyStatus so the enrollment UI can reflect anchored/endorsed live.
 */
export async function loadAnchorStatus(): Promise<void> {
  if (!session.client) return;
  const s = await session.client.rpc('bramble.getAnchorStatus');
  useStore.getState().setAnchorStatus(s);
}
