import type { ConnectionCapabilities, RuntimeMode } from '../types/bramble';
import { isEmbeddedShell } from '../utils/platform';

interface CapabilitiesResponse {
  mode?: RuntimeMode;
  localLanAllowed?: boolean;
  localLanReason?: string;
}

// Shown to the user when direct LAN connect is unavailable (hosted mode).
// Single source of truth: the default capabilities below and the connection
// overlay's fallback both reference it so the copy cannot drift.
export const LOCAL_LAN_UNAVAILABLE_REASON =
  'LAN direct connect is unavailable in hosted mode. Use USB or Bluetooth.';

export const DEFAULT_CAPABILITIES: ConnectionCapabilities = {
  mode: 'hosted',
  localLanAllowed: false,
  localLanReason: LOCAL_LAN_UNAVAILABLE_REASON,
};

// Embedded shells (Electron file://, Android WebView asset origin) load the
// app from a local origin where /api/capabilities does not exist. They are
// always local mode: the shell may open ws:// LAN sockets directly.
export const EMBEDDED_CAPABILITIES: ConnectionCapabilities = {
  mode: 'local',
  localLanAllowed: true,
};

function normalizeCapabilities(input: unknown): ConnectionCapabilities {
  const body = (input ?? {}) as CapabilitiesResponse;
  const mode: RuntimeMode = body.mode === 'local' ? 'local' : 'hosted';
  const localLanAllowed = mode === 'local' ? body.localLanAllowed !== false : false;
  const localLanReason = localLanAllowed
    ? undefined
    : body.localLanReason || DEFAULT_CAPABILITIES.localLanReason;

  return { mode, localLanAllowed, localLanReason };
}

export async function fetchConnectionCapabilities(fetchImpl: typeof fetch = fetch): Promise<ConnectionCapabilities> {
  if (isEmbeddedShell()) return EMBEDDED_CAPABILITIES;
  try {
    const res = await fetchImpl('/api/capabilities');
    if (!res.ok) return DEFAULT_CAPABILITIES;
    const body = await res.json();
    return normalizeCapabilities(body);
  } catch {
    return DEFAULT_CAPABILITIES;
  }
}
