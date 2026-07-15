// Map technical error messages to human-friendly text
const ERROR_MAP: Array<[RegExp, string]> = [
  // Must precede the /auth/i rule: a handshake TIMEOUT means the node never
  // answered (usually because another device, like the phone, holds its one
  // BLE connection), not that the token is wrong.
  [/handshake timed out/i, 'The node did not respond over Bluetooth. If it is connected to another device (like your phone), disconnect there first, then retry.'],
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
  const raw = e instanceof Error ? e.message : typeof e === 'string' ? e : '';
  return raw ? friendlyError(raw) : 'Something went wrong. Check the connection and retry.';
}
