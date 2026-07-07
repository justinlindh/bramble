// Merges the mDNS discovery snapshot with the device book into the list
// model rendered by the desktop-only NearbyNodes component.

import type { DiscoveredNode } from '../types/desktop';
import type { SavedDevice } from './deviceBook';
import { nodeKey } from './discoveryCore';

export type NearbyNode = {
  key: string;
  displayName: string;
  ip: string;
  hostname: string;
  addrHex?: string;
  /** Raw TXT-advertised name; prefills the add-device form for unknown nodes. */
  txtName?: string;
  /** Exact device-book match by full address. */
  saved?: SavedDevice;
  /** Heuristic match by 16-bit hostname suffix (firmware without TXT records). */
  probableSaved?: SavedDevice;
};

const HOSTNAME_SUFFIX = /^bramble-([0-9a-fA-F]{4})$/;

export function mergeNearby(discovered: DiscoveredNode[], devices: SavedDevice[]): NearbyNode[] {
  const nodes = discovered.map((d): NearbyNode => {
    const saved = d.addrHex ? devices.find(x => x.address === d.addrHex) : undefined;

    // Old firmware advertises no TXT addr; the hostname carries the low 16
    // bits of the address. Only claim a match when it is unambiguous, and
    // never when TXT gave us the full address (exact info wins).
    let probableSaved: SavedDevice | undefined;
    if (!d.addrHex) {
      const m = d.hostname.match(HOSTNAME_SUFFIX);
      if (m) {
        const suffix = m[1].toUpperCase();
        const matches = devices.filter(x => x.address.slice(4) === suffix);
        if (matches.length === 1) probableSaved = matches[0];
      }
    }

    return {
      key: nodeKey(d),
      displayName: saved?.name ?? probableSaved?.name ?? d.name ?? d.hostname,
      ip: d.ip,
      hostname: d.hostname,
      addrHex: d.addrHex,
      txtName: d.name,
      saved,
      probableSaved,
    };
  });

  const rank = (n: NearbyNode) => (n.saved ? 0 : n.probableSaved ? 1 : 2);
  return nodes.sort((a, b) => rank(a) - rank(b) || a.displayName.localeCompare(b.displayName));
}
