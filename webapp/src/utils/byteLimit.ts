/**
 * UTF-8 byte budgets for text the node validates by byte length.
 *
 * The RPC layer measures these fields with strlen, so its limits are byte
 * counts: ROLLCALL_TEXT_MAX for a roll-call payload, BRAMBLE_NODE_NAME_MAX
 * for a node name, and the channel-name bound in handle_add_channel. An
 * <input maxLength> counts UTF-16 code units instead, which agrees with the
 * node only for ASCII. One emoji is a single visible character, two UTF-16
 * units, and four UTF-8 bytes, so a field capped at 32 "characters" can
 * carry 128 bytes and the node rejects the whole request as malformed.
 */

/** bramble.setNodeName rejects over BRAMBLE_NODE_NAME_MAX (main/mesh_task.h). */
export const NODE_NAME_MAX_BYTES = 32;

/**
 * Budget for a channel name. handle_add_channel accepts up to 19 bytes, into
 * a 20-byte slot in channel_storage. The client holds itself to a tighter
 * figure so names stay short in the channel list; what this constant fixes is
 * the unit, not the number, which the UI has always shown as 16.
 */
export const CHANNEL_NAME_BUDGET_BYTES = 16;

/**
 * Fallback cap for the roll-call payload, used only until the node reports
 * its own max_text_bytes. It mirrors ROLLCALL_TEXT_MAX in
 * components/rollcall/include/rollcall.h. The reported value always wins once
 * the ledger loads: this exists so the window before that first response does
 * not leave the field unbounded.
 */
export const ROLLCALL_TEXT_FALLBACK_BYTES = 48;

/**
 * Fragmentation budgets for a message body, aligned with the firmware's
 * components/fragment. A body up to SINGLE_PACKET_MAX_BYTES rides in one
 * packet; anything longer is split into at most MAX_FRAGMENTS packets of
 * FRAGMENT_PAYLOAD_BYTES, so FRAGMENTED_MAX_BYTES is the largest body the
 * receiving node can reassemble.
 */
export const SINGLE_PACKET_MAX_BYTES = 203;
export const FRAGMENT_PAYLOAD_BYTES = 154;
export const MAX_FRAGMENTS = 4;
export const FRAGMENTED_MAX_BYTES = FRAGMENT_PAYLOAD_BYTES * MAX_FRAGMENTS;

const encoder = new TextEncoder();

/** Length of `s` in UTF-8 bytes, the unit the node's limits are written in. */
export function utf8Length(s: string): number {
  return encoder.encode(s).length;
}

/**
 * Longest prefix of `s` that fits in `maxBytes` UTF-8 bytes, cut on a
 * character boundary.
 *
 * Iterating with Array.from walks code points rather than UTF-16 units, so a
 * surrogate pair is kept or dropped whole and the result is never truncated
 * into an invalid sequence. Combining marks and ZWJ sequences are separate
 * code points and can still be split, which drops an accent or breaks a
 * composed emoji apart; that is a visible edit rather than a corrupt string,
 * and it only happens at the boundary of a field the node would otherwise
 * refuse outright.
 */
export function clampToUtf8Bytes(s: string, maxBytes: number): string {
  if (maxBytes <= 0) return '';
  if (utf8Length(s) <= maxBytes) return s;

  let used = 0;
  let out = '';
  for (const ch of s) {
    const size = utf8Length(ch);
    if (used + size > maxBytes) break;
    used += size;
    out += ch;
  }
  return out;
}
