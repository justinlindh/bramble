import type { ConnectionCapabilities, RuntimeMode } from '../types/bramble';
import { isElectron } from '../utils/platform';

interface CapabilitiesResponse {
  mode?: RuntimeMode;
  localLanAllowed?: boolean;
  localLanReason?: string;
}

export const DEFAULT_CAPABILITIES: ConnectionCapabilities = {
  mode: 'hosted',
  localLanAllowed: false,
  localLanReason: 'LAN direct connect is unavailable in hosted mode. Use USB or Bluetooth.',
};

// Electron loads the renderer from file:// where /api/capabilities does not
// exist. Desktop is always local mode: the renderer may open ws:// LAN
// sockets directly (no mixed-content or PNA restrictions under file://).
export const ELECTRON_CAPABILITIES: ConnectionCapabilities = {
  mode: 'local',
  localLanAllowed: true,
};

export function normalizeCapabilities(input: unknown): ConnectionCapabilities {
  const body = (input ?? {}) as CapabilitiesResponse;
  const mode: RuntimeMode = body.mode === 'local' ? 'local' : 'hosted';
  const localLanAllowed = mode === 'local' ? body.localLanAllowed !== false : false;
  const localLanReason = localLanAllowed
    ? undefined
    : body.localLanReason || DEFAULT_CAPABILITIES.localLanReason;

  return { mode, localLanAllowed, localLanReason };
}

export async function fetchConnectionCapabilities(fetchImpl: typeof fetch = fetch): Promise<ConnectionCapabilities> {
  if (isElectron()) return ELECTRON_CAPABILITIES;
  try {
    const res = await fetchImpl('/api/capabilities');
    if (!res.ok) return DEFAULT_CAPABILITIES;
    const body = await res.json();
    return normalizeCapabilities(body);
  } catch {
    return DEFAULT_CAPABILITIES;
  }
}
