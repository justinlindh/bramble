// Resolution of a node's user-set display name.
//
// Firmware reports a node whose name has not been set with the sentinel
// string "(unnamed)" (see the identity.name field in the RPC contract). The
// UI treats that sentinel, an empty string, and whitespace-only names alike
// as "no user-set name" so it can fall back to the hex address or a
// peer-supplied name. Keeping that rule in one place stops the sentinel from
// drifting between the call sites that display or store node names.

/** The firmware sentinel reported for a node whose name has not been set. */
export const UNNAMED_NODE_NAME = '(unnamed)';

/**
 * The node's real, user-set display name, or undefined when none is set.
 * Trims surrounding whitespace and rejects the "(unnamed)" firmware sentinel.
 */
export function resolveNodeName(name: string | null | undefined): string | undefined {
  const trimmed = name?.trim();
  if (trimmed && trimmed !== UNNAMED_NODE_NAME) {
    return trimmed;
  }
  return undefined;
}
