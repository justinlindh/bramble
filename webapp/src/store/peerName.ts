/**
 * Shared peer-name resolution.
 *
 * Precedence (highest first):
 *   1. Location telemetry name (peerLocation.name, non-empty)
 *   2. Contact / neighbor beacon name (peerNames map)
 *   3. undefined (caller chooses its own fallback)
 *
 * This is the same logic the Nodes page uses for its "Known peers" list,
 * extracted here so every surface (chat sidebar, new-DM picker, Config
 * PeerManager, Nodes) resolves names consistently.
 */
import { useStore } from './index';
import type { PeerLocation } from '../types/bramble';

/**
 * Pure function: resolve the assigned name for a peer address given raw store
 * slices.  Returns `undefined` when no name is known (caller picks fallback).
 */
export function resolvePeerName(
  addr: number,
  peerNames: Map<number, string> | undefined,
  peerLocations: PeerLocation[] | undefined,
): string | undefined {
  // 1. Location telemetry name
  const loc = peerLocations?.find((l) => l.addr === addr);
  if (loc?.name?.trim()) return loc.name.trim();

  // 2. Contact / neighbor beacon name
  const contact = peerNames?.get(addr);
  if (contact) return contact;

  // 3. No name known
  return undefined;
}

/**
 * React hook: subscribe to the store and return the assigned name for a peer,
 * or `undefined` when none is known. Call sites use their own hex fallback so
 * they can independently decide short vs full hex format.
 */
export function usePeerName(addr: number): string | undefined {
  return useStore((s) =>
    resolvePeerName(addr, s.peerNames, s.peerLocations),
  );
}
