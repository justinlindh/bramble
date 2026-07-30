// Device management: auth token, allowed origins, and OTA origin/update/status
// (issue #95).
import { session, requireClient } from './client';
import type { RpcSchemas, WirePartial } from '../../types/rpc';

// ─── Device management (auth token, allowed origins, OTA): issue #95 ───────

export interface AuthTokenInfo { token: string; enabled: boolean; }

export async function getAuthToken(): Promise<AuthTokenInfo> {
  const client = requireClient();
  const r = await client.rpc('bramble.getAuthToken');
  return { token: r.token ?? '', enabled: !!r.enabled };
}

export async function setAuthToken(token: string): Promise<void> {
  const client = requireClient();
  await client.rpc('bramble.setAuthToken', { token });
}

export async function getAllowedOrigins(): Promise<string[]> {
  const client = requireClient();
  const r = await client.rpc('bramble.getAllowedOrigins');
  return Array.isArray(r.origins) ? r.origins : [];
}

export async function setAllowedOrigins(origins: string[]): Promise<void> {
  const client = requireClient();
  await client.rpc('bramble.setAllowedOrigins', { origins });
}

export interface OtaOriginInfo {
  origin: string;
  defaultOrigin: string;
  overridden: boolean;
  versionFloor?: string;
  runningVersion?: string;
}

// The two version fields both OTA reads carry (bramble.otaGetOrigin and
// bramble.otaStatus/onOtaEvent), in contract spelling plus the camelCase
// fallbacks in case a bridge normalizes keys before they get here.
interface OtaVersionWire {
  version_floor?: string;
  versionFloor?: string;
  running_version?: string;
  runningVersion?: string;
}

function otaVersionFields(r: OtaVersionWire): { versionFloor?: string; runningVersion?: string } {
  return {
    versionFloor: r.version_floor ?? r.versionFloor,
    runningVersion: r.running_version ?? r.runningVersion,
  };
}

type OtaGetOriginWire = WirePartial<RpcSchemas['OtaGetOriginResponse']> & OtaVersionWire & {
  defaultOrigin?: string;
};

export async function getOtaOrigin(): Promise<OtaOriginInfo> {
  const client = requireClient();
  const r: OtaGetOriginWire = await client.rpc('bramble.otaGetOrigin');
  return {
    origin: r.origin ?? '',
    defaultOrigin: r.default_origin ?? r.defaultOrigin ?? '',
    overridden: !!r.overridden,
    ...otaVersionFields(r),
  };
}

export async function setOtaOrigin(origin: string): Promise<{ ok: boolean; error?: string }> {
  const client = requireClient();
  const r = await client.rpc('bramble.otaSetOrigin', { origin });
  return { ok: !!r.ok, error: r.error };
}

export async function resetOtaOrigin(): Promise<void> {
  const client = requireClient();
  await client.rpc('bramble.otaSetOrigin', { reset: true });
}

export async function startOtaUpdate(path: string, allowDowngrade = false): Promise<{ ok: boolean; note?: string; url?: string; error?: string; lastError?: string }> {
  const client = requireClient();
  const r = await client.rpc('bramble.otaUpdate', { path, allow_downgrade: allowDowngrade });
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

type OtaStatusWire = WirePartial<RpcSchemas['OtaStatusResponse']> & OtaVersionWire & {
  error?: string;
};

function otaStatusFrom(r: OtaStatusWire): OtaStatus {
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
  const client = requireClient();
  const r = await client.rpc('bramble.otaStatus');
  return otaStatusFrom(r);
}

export function subscribeOtaEvents(cb: (e: OtaStatus) => void): () => void {
  if (!session.client) return () => {};
  return session.client.subscribe('bramble.onOtaEvent', (params) => cb(otaStatusFrom((params ?? {}) as OtaStatusWire)));
}
