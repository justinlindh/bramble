// Device management: auth token, allowed origins, and OTA origin/update/status
// (issue #95).
import { session } from './client';

// ─── Device management (auth token, allowed origins, OTA): issue #95 ───────

export interface AuthTokenInfo { token: string; enabled: boolean; }

export async function getAuthToken(): Promise<AuthTokenInfo> {
  if (!session.client) throw new Error('Not connected');
  const r = await session.client.rpc<any>('bramble.getAuthToken');
  return { token: r.token ?? '', enabled: !!r.enabled };
}

export async function setAuthToken(token: string): Promise<void> {
  if (!session.client) throw new Error('Not connected');
  await session.client.rpc('bramble.setAuthToken', { token });
}

export async function getAllowedOrigins(): Promise<string[]> {
  if (!session.client) throw new Error('Not connected');
  const r = await session.client.rpc<any>('bramble.getAllowedOrigins');
  return Array.isArray(r.origins) ? r.origins : [];
}

export async function setAllowedOrigins(origins: string[]): Promise<void> {
  if (!session.client) throw new Error('Not connected');
  await session.client.rpc('bramble.setAllowedOrigins', { origins });
}

export interface OtaOriginInfo {
  origin: string;
  defaultOrigin: string;
  overridden: boolean;
  versionFloor?: string;
  runningVersion?: string;
}

// Shared snake/camel mapping for the two RPC-response fields both OTA reads
// carry (bramble.otaGetOrigin and bramble.otaStatus/onOtaEvent). Defensive
// camelCase fallbacks in case a bridge normalizes keys before they get here.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
function otaVersionFields(r: any): { versionFloor?: string; runningVersion?: string } {
  return {
    versionFloor: r.version_floor ?? r.versionFloor,
    runningVersion: r.running_version ?? r.runningVersion,
  };
}

export async function getOtaOrigin(): Promise<OtaOriginInfo> {
  if (!session.client) throw new Error('Not connected');
  const r = await session.client.rpc<any>('bramble.otaGetOrigin');
  return {
    origin: r.origin ?? '',
    defaultOrigin: r.default_origin ?? r.defaultOrigin ?? '',
    overridden: !!r.overridden,
    ...otaVersionFields(r),
  };
}

export async function setOtaOrigin(origin: string): Promise<{ ok: boolean; error?: string }> {
  if (!session.client) throw new Error('Not connected');
  const r = await session.client.rpc<any>('bramble.otaSetOrigin', { origin });
  return { ok: !!r.ok, error: r.error };
}

export async function resetOtaOrigin(): Promise<void> {
  if (!session.client) throw new Error('Not connected');
  await session.client.rpc('bramble.otaSetOrigin', { reset: true });
}

export async function startOtaUpdate(path: string, allowDowngrade = false): Promise<{ ok: boolean; note?: string; url?: string; error?: string; lastError?: string }> {
  if (!session.client) throw new Error('Not connected');
  const r = await session.client.rpc<any>('bramble.otaUpdate', { path, allow_downgrade: allowDowngrade });
  return { ok: !!r.ok, note: r.note, url: r.url, error: r.error, lastError: r.last_error };
}

export interface OtaStatus {
  state: 'idle' | 'downloading' | 'verifying' | 'rebooting' | 'failed';
  bytes: number;
  total: number;
  percent: number;
  lastError?: string;
  runningVersion?: string;
  versionFloor?: string;
}

function otaStatusFrom(r: any): OtaStatus {
  return {
    state: r.state ?? 'idle',
    bytes: r.bytes ?? 0,
    total: r.total ?? 0,
    percent: r.percent ?? 0,
    lastError: r.last_error ?? r.error,
    ...otaVersionFields(r),
  };
}

export async function getOtaStatus(): Promise<OtaStatus> {
  if (!session.client) throw new Error('Not connected');
  const r = await session.client.rpc<any>('bramble.otaStatus');
  return otaStatusFrom(r);
}

export function subscribeOtaEvents(cb: (e: OtaStatus) => void): () => void {
  if (!session.client) return () => {};
  return session.client.subscribe('bramble.onOtaEvent', (params: any) => cb(otaStatusFrom(params ?? {})));
}
