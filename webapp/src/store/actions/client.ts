// Shared RPC session state for the action modules. `session.client` is the
// single live BrambleClient: connection.ts assigns it on connect/disconnect and
// every other action module reads it through this holder, which keeps the
// module graph cycle-free (everything imports client.ts, client.ts imports
// nothing from its siblings).
import { BrambleClient } from '../../transport';

export const session: { client: BrambleClient | null } = { client: null };

export const LAST_NODE_ADDR_KEY = 'bramble:last-node-addr';

// Assert a live client and return it non-null. Action functions that must talk
// to the device call this instead of hand-rolling the `if (!session.client)
// throw new Error('Not connected')` guard, so the guard message lives in one
// place and callers get a typed BrambleClient without a null assertion.
export function requireClient(): BrambleClient {
  if (!session.client) throw new Error('Not connected');
  return session.client;
}
