// Map technical error messages to human-friendly text
const ERROR_MAP: Array<[RegExp, string]> = [
  // Must precede the /auth/i rule: a handshake TIMEOUT means the node never
  // answered (usually because another device, like the phone, holds its one
  // BLE connection), not that the token is wrong.
  [/handshake timed out/i, 'The node did not respond over Bluetooth. If it is connected to another device (like your phone), disconnect there first, then retry.'],
  // First-time BLE pairing outcomes from the transport. Both rules sit
  // before the /1008|unauthorized|auth/i rule because fail-fast stacks
  // append security reasons like "insufficient authentication" to the raw
  // message, and a pairing failure must map to pairing copy, not token copy.
  // (The overlay's token-field highlight reads the store's structured
  // connectionErrorIsAuth flag, not this display text.)
  [/pairing did not complete/i, 'Bluetooth pairing did not finish. Click Connect again, and type the code shown on the node when the browser asks for it.'],
  [/pairing was cancelled/i, 'Pairing was cancelled. Click Connect to try again.'],
  // A raw 'RPC timeout: bramble.getVersion' used to leak to the UI verbatim.
  [/RPC timeout/i, 'Connected, but the node did not answer. Retry, and power-cycle the node if it keeps happening.'],
  [/write timed out/i, 'The Bluetooth link stalled while sending. Move closer to the node and retry.'],
  [/cancelled.*requestDevice/i, 'Bluetooth pairing was cancelled.'],
  [/cancelled.*requestPort/i, 'Serial port selection was cancelled.'],
  [/user cancel/i, 'Connection was cancelled.'],
  [/no compatible device/i, 'No Bramble device found nearby.'],
  [/NetworkError/i, 'Could not reach the node. Check the IP address and that it\'s on the same network.'],
  [/WebSocket.*failed/i, 'Could not connect. Check the IP address and that the node is powered on.'],
  [/GATT.*disconnect/i, 'Bluetooth connection was lost.'],
  [/SecurityError/i, 'Browser blocked the connection. Try using HTTPS or localhost.'],
  [/AbortError/i, 'Connection timed out.'],
  [/NotFoundError/i, 'No device found. Make sure your node is powered on and in range.'],
  [/already.*connect/i, 'Already connected to a device.'],
  [/serial rpc handshake failed/i, 'Serial link is up, but RPC is still starting. Please retry in a moment.'],
  [/1008|unauthorized|auth/i, 'Authentication required. Enter this node\'s auth token, then reconnect.'],
  [/not a bramble node/i, 'Connected, but the endpoint did not respond as a Bramble node. Check the address and port.'],
];

export function friendlyError(raw: string): string {
  for (const [pattern, friendly] of ERROR_MAP) {
    if (pattern.test(raw)) return friendly;
  }
  if (raw.length > 100) return 'Connection failed. Please try again.';
  return raw;
}

// Convenience wrapper for catch (err: unknown) sites: extracts a message
// from an Error-shaped value (or a plain thrown string) and maps it to
// friendly text in one call. Anything without a usable message (thrown
// undefined/null, message-less objects, non-string primitives) gets a
// generic fallback instead of rendering "undefined" or "[object Object]".
export function friendlyErrorFrom(e: unknown): string {
  const raw = messageOf(e);
  return raw ? friendlyError(raw) : 'Something went wrong. Check the connection and retry.';
}

// Pull a usable message string out of an unknown thrown value (an Error, a
// plain string like a stored connectionError, or something message-less), so
// the classifiers below accept whatever a call site happens to hold.
function messageOf(e: unknown): string {
  if (typeof e === 'string') return e;
  if (e && typeof e === 'object' && typeof (e as { message?: unknown }).message === 'string') {
    return (e as { message: string }).message;
  }
  return '';
}

// The node rejected the call for missing or failed authentication: a 1008
// WebSocket close, an "unauthorized" body, or any auth-tagged message. This is
// the firmware's auth-error text contract in one place; the app decides whether
// to prompt for a token based on it, so update the pattern here when the
// wording changes rather than in each caller.
// A TIMEOUT is excluded even when the message mentions auth: the transport's
// 'Authentication handshake timed out' means the node never answered (usually
// another device holds its one BLE connection), not that the token is wrong,
// so it must not paint the token field red.
export function isAuthError(e: unknown): boolean {
  const msg = messageOf(e);
  return /1008|unauthorized|auth/i.test(msg) && !/timed out/i.test(msg);
}

// The RPC method is not implemented by this firmware build (an older node that
// predates a newer method). Callers use this to fall back to a legacy call
// shape or to skip an optional feature.
export function isUnknownMethodError(e: unknown): boolean {
  return /not\s+found|unknown\s+method|method\s+not\s+found/i.test(messageOf(e));
}
