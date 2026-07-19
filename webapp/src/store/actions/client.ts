// Shared RPC session state for the action modules. `session.client` is the
// single live BrambleClient: connection.ts assigns it on connect/disconnect and
// every other action module reads it through this holder, which keeps the
// module graph cycle-free (everything imports client.ts, client.ts imports
// nothing from its siblings).
import { BrambleClient } from '../../transport';

export const session: { client: BrambleClient | null } = { client: null };

export const LAST_NODE_ADDR_KEY = 'bramble:last-node-addr';

export function parseHexAddr(addr: string | number | undefined): number {
  if (typeof addr === 'number') return addr;
  if (!addr) return 0;
  const raw = addr.trim();
  if (!raw) return 0;
  const stripped = raw.replace(/^0x/i, '');
  // If the string is plain decimal digits, treat as decimal.
  if (/^[0-9]+$/.test(stripped) && !/[A-Fa-f]/.test(stripped)) {
    return parseInt(stripped, 10);
  }
  return parseInt(stripped, 16);
}

export function getClient(): BrambleClient | null {
  return session.client;
}
