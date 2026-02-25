import type { ConnectionCapabilities, RuntimeMode } from '../types/bramble';

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
  try {
    const res = await fetchImpl('/api/capabilities');
    if (!res.ok) return DEFAULT_CAPABILITIES;
    const body = await res.json();
    return normalizeCapabilities(body);
  } catch {
    return DEFAULT_CAPABILITIES;
  }
}
