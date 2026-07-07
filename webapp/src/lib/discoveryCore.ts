// Pure logic for LAN node discovery: maps bonjour-service results to
// DiscoveredNode and maintains a deduped snapshot. No sockets, no Electron
// imports; the Electron main process (webapp/electron/discovery.ts) is the
// only runtime consumer.

import type { DiscoveredNode } from '../types/desktop';

/** Subset of a bonjour-service discovery result that we consume. */
export type RawService = {
  host?: string;
  addresses?: string[];
  txt?: Record<string, unknown>;
};

const IPV4 = /^\d{1,3}(\.\d{1,3}){3}$/;
const ADDR_HEX = /^[0-9a-fA-F]{8}$/;

function txtString(txt: Record<string, unknown> | undefined, key: string): string | undefined {
  const v = txt?.[key];
  return typeof v === 'string' && v.length > 0 ? v : undefined;
}

/** Validated, uppercased TXT `addr` value, or undefined when absent/malformed. */
function txtAddrHex(txt: Record<string, unknown> | undefined): string | undefined {
  const addr = txtString(txt, 'addr');
  return addr && ADDR_HEX.test(addr) ? addr.toUpperCase() : undefined;
}

function bareHostname(host: string): string {
  return host.replace(/\.local\.?$/i, '');
}

export function nodeKey(node: DiscoveredNode): string {
  return node.addrHex ?? node.hostname;
}

/** Maps a bonjour service to a DiscoveredNode. Null when unusable (no host or no IPv4). */
export function serviceToNode(svc: RawService): DiscoveredNode | null {
  if (!svc.host) return null;
  const ip = svc.addresses?.find(a => IPV4.test(a));
  if (!ip) return null;
  return {
    addrHex: txtAddrHex(svc.txt),
    name: txtString(svc.txt, 'name'),
    hostname: bareHostname(svc.host),
    ip,
  };
}

/** New snapshot with the node added, replacing any entry with the same key. */
export function upsertNode(snapshot: DiscoveredNode[], node: DiscoveredNode): DiscoveredNode[] {
  return [...snapshot.filter(n => nodeKey(n) !== nodeKey(node)), node];
}

/** New snapshot without the service's entry. Matches by full address (TXT)
 *  or hostname: down events often omit TXT records, so a node stored under
 *  its addrHex must still be removable by hostname alone. */
export function removeService(snapshot: DiscoveredNode[], svc: RawService): DiscoveredNode[] {
  const addrKey = txtAddrHex(svc.txt);
  const host = svc.host ? bareHostname(svc.host) : null;
  if (!addrKey && !host) return snapshot;
  return snapshot.filter(n => !(addrKey && n.addrHex === addrKey) && !(host && n.hostname === host));
}
