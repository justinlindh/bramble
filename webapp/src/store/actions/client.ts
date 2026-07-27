// Shared RPC session state for the action modules. `session.client` is the
// single live BrambleClient: connection.ts assigns it on connect/disconnect and
// every other action module reads it through this holder, which keeps the
// module graph cycle-free (everything imports client.ts, client.ts imports
// nothing from its siblings).
import { BrambleClient } from '../../transport';

export const session: { client: BrambleClient | null } = { client: null };

export const LAST_NODE_ADDR_KEY = 'bramble:last-node-addr';
