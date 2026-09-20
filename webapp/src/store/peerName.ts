/**
 * Shared peer-name resolution.
 *
 * Precedence (highest first):
 *   1. Contact name the user assigned (contactNames)
 *   2. Location telemetry name (peerLocation.name, non-empty)
 *   3. Name the firmware reported (peerNames)
 *   4. undefined (caller chooses its own fallback)
 */
import { useStore } from './index';
import type { PeerLocation } from '../types/bramble';

/** Resolve a peer's display name from raw store slices; `undefined` when none is known. */
export function resolvePeerName(
  addr: number,
  peerNames: Map<number, string> | undefined,
  peerLocations: PeerLocation[] | undefined,
  contactNames?: Map<number, string>,
): string | undefined {
  const contact = contactNames?.get(addr);
  if (contact) return contact;

  const loc = peerLocations?.find((l) => l.addr === addr);
  if (loc?.name?.trim()) return loc.name.trim();

  return peerNames?.get(addr) || undefined;
}

/**
 * React hook: subscribe to the store and return the resolved name for a peer,
 * or `undefined` when none is known. Call sites use their own hex fallback so
 * they can independently decide short vs full hex format.
 */
export function usePeerName(addr: number): string | undefined {
  return useStore((s) =>
    resolvePeerName(addr, s.peerNames, s.peerLocations, s.contactNames),
  );
}
