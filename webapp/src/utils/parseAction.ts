/**
 * Parse a CTCP ACTION message (IRC-style: \x01ACTION text\x01).
 * The firmware passes these through unchanged.
 */
export function parseAction(text: string): { isAction: boolean; actionText: string } {
  if (text.startsWith('\x01ACTION ') && text.endsWith('\x01')) {
    return { isAction: true, actionText: text.slice(8, -1) };
  }
  return { isAction: false, actionText: '' };
}
