// Node configuration: the getConfig loader with its normalizer, and every
// config-mutating RPC (radio, name, channels, mailbox, default channel,
// location config and contacts).
import { session, requireClient } from './client';
import { useStore } from '../index';
import { formatAddrHex } from '../../utils/address';
import { parseAddr } from '../../lib/addr';
import type { BrambleConfig, LocationConfig, LocationTier, TimezoneInfo } from '../../types/bramble';
import type { RpcSchemas, WirePartial } from '../../types/rpc';
import { loadPeerLocations, loadStatus } from './telemetry';

// Wire types: the contract schema made deep-optional plus every legacy key
// spelling this normalizer still reads. See types/rpc.ts for the rationale.
type ChannelWire = WirePartial<RpcSchemas['Channel']> & {
  index?: number;
  channel_name?: string;
  channelName?: string;
  has_psk?: boolean;
  psk_enabled?: boolean;
  pskEnabled?: boolean;
  key_epoch?: number;
  keyEpoch?: number;
  isDefault?: boolean;
  default?: boolean;
  default_channel?: boolean;
  defaultChannel?: boolean;
};

type LocationWire = WirePartial<Omit<RpcSchemas['LocationConfig'], 'source'>> & {
  // Legacy 'auto' predates the contract's 'hybrid' and is normalized to it.
  source?: RpcSchemas['LocationConfig']['source'] | 'auto';
  contacts?: Array<{ addr: number; tier: LocationTier; intervalSec?: number; distanceTriggerM?: number }>;
};

type ConfigWire = WirePartial<Omit<RpcSchemas['ConfigResponse'], 'channels' | 'radio' | 'location'>> & {
  channels?: ChannelWire[];
  radio?: WirePartial<RpcSchemas['ConfigResponse']['radio']> & {
    txPowerDbm?: number;
    bwKhz?: number;
    cr?: number;
    freqMhz?: number;
  };
  location?: LocationWire;
  identity?: { address?: number; pubkeyHash?: number; name?: string; pubkeyB64?: string };
};

/**
 * Normalize firmware config response to match BrambleConfig interface.
 * Firmware returns flat structure; webapp expects nested identity/radio objects.
 */
export function normalizeConfig(raw: ConfigWire): BrambleConfig {
  const rawLocation: LocationWire = raw.location ?? {};
  const legacyContacts = (rawLocation.contacts ?? []) as Array<{ addr: number; tier: LocationTier; intervalSec?: number }>;
  const contactRules = (rawLocation.contact_rules ?? legacyContacts.map((c) => ({
    address: formatAddrHex(c.addr),
    enabled: c.tier !== 'off',
    tier: c.tier,
    interval_s: c.intervalSec ?? rawLocation.interval_s ?? 300,
  }))) as LocationConfig['contact_rules'];

  return {
    identity: {
      address: parseAddr(raw.address ?? raw.identity?.address),
      pubkeyHash: typeof raw.pubkey_hash === 'string' ? parseInt(raw.pubkey_hash, 16) : (raw.identity?.pubkeyHash ?? 0),
      name: raw.node_name ?? raw.identity?.name ?? '',
      pubkeyB64: raw.identity?.pubkeyB64 ?? '',
    },
    radio: {
      txPowerDbm: raw.radio?.tx_power_dbm ?? raw.radio?.txPowerDbm ?? 0,
      sf: raw.radio?.sf ?? 9,
      bwKhz: raw.radio?.bw_hz ? Math.round(raw.radio.bw_hz / 1000) : (raw.radio?.bwKhz ?? 125),
      cr: raw.radio?.cr ?? 5,
      freqMhz: raw.radio?.frequency_mhz ?? raw.radio?.freqMhz ?? 915.0,
    },
    channels: (raw.channels ?? []).map((ch) => {
      const candidates = [ch.name, ch.channel_name, ch.channelName];
      const firstNonBlankName = candidates.find((v: unknown) => typeof v === 'string' && v.trim().length > 0) as string | undefined;
      return {
        index: ch.id ?? ch.index ?? 0,
        name: firstNonBlankName ?? '',
        hasPsk: ch.hasPsk ?? ch.has_psk ?? ch.psk_enabled ?? ch.pskEnabled ?? false,
        epoch: ch.epoch ?? ch.key_epoch ?? ch.keyEpoch ?? 0,
        isDefault: ch.is_default ?? ch.isDefault ?? ch.default ?? ch.default_channel ?? ch.defaultChannel ?? false,
      };
    }),
    mailboxEnabled: raw.mailboxEnabled ?? false,
    location: {
      enabled: rawLocation.enabled ?? false,
      tier: rawLocation.tier ?? rawLocation.default_tier ?? 'coarse',
      default_tier: rawLocation.default_tier ?? rawLocation.tier ?? 'coarse',
      interval_s: rawLocation.interval_s ?? 300,
      source: rawLocation.source === 'auto' ? 'hybrid' : (rawLocation.source ?? 'hybrid'),
      lat: rawLocation.lat,
      lon: rawLocation.lon,
      contact_rules: contactRules,
      channel_targets: rawLocation.channel_targets ?? [],
    },
  } as BrambleConfig;
}

export async function loadConfig(): Promise<void> {
  if (!session.client) return;
  const result = await session.client.rpc('bramble.getConfig');
  useStore.getState().setConfig(normalizeConfig(result));
}

// ─── Config mutations ────────────────────────────────────────────────────

/** Throw if an RPC result has ok:false with an error message */
function assertOk(result: unknown, fallback: string): void {
  const r = result as Record<string, unknown> | null;
  if (r && r.ok === false) {
    throw new Error((r.error as string) || fallback);
  }
}

export async function saveRadio(radio: import('../../types/bramble').RadioConfig): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.setRadio', radio as unknown as Record<string, unknown>);
  assertOk(result, 'Radio config failed');
  await loadConfig();
}

export async function saveNodeName(name: string): Promise<void> {
  const client = requireClient();
  await client.rpc('bramble.setNodeName', { name });
  await loadConfig();
}

export async function addChannel(name: string, psk?: string): Promise<number> {
  const client = requireClient();
  const result = await client.rpc<{ ok: boolean; index: number; error?: string }>('bramble.addChannel', {
    name,
    ...(psk ? { psk } : {}),
  });
  assertOk(result, 'Failed to add channel');
  await loadConfig();
  return result.index;
}

export async function removeChannel(index: number): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.removeChannel', { index });
  assertOk(result, 'Failed to remove channel');
  await loadConfig();
}

export async function setMailbox(enabled: boolean): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.setMailbox', { enabled });
  assertOk(result, 'Failed to set mailbox');
  await loadConfig();
}

export async function setDefaultChannel(index: number): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.setDefaultChannel', { index });
  assertOk(result, 'Failed to set default channel');
  await loadConfig();
}

export async function setLocationConfig(config: Partial<LocationConfig>): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.setLocationConfig', config as unknown as Record<string, unknown>);
  assertOk(result, 'Failed to save location config');
  await loadConfig();
  await loadPeerLocations().catch(() => {});
}

export async function setGpsEnabled(enabled: boolean): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.setGpsEnabled', { enabled });
  assertOk(result, 'Failed to set GPS power');
  await loadStatus(); // re-pull so gpsEnabled reflects the device's answer
}

// The node's clock zone. UTC stays the device's internal source of truth, so
// this only affects what the device itself renders: the webapp keeps showing
// times in the browser's own zone.
export async function loadTimezone(): Promise<TimezoneInfo> {
  const client = requireClient();
  const result = (await client.rpc('bramble.getTimezone', {})) as Record<string, unknown> | null;
  assertOk(result, 'Failed to read timezone');
  const presets = Array.isArray(result?.presets) ? (result!.presets as Record<string, unknown>[]) : [];
  return {
    timezone: (result?.timezone as string) ?? 'UTC0',
    defaultTimezone: (result?.default_timezone as string) ?? 'UTC0',
    configured: result?.configured === true,
    presets: presets
      .filter((p) => typeof p.label === 'string' && typeof p.spec === 'string')
      .map((p) => ({ label: p.label as string, spec: p.spec as string })),
  };
}

export async function setTimezone(timezone: string): Promise<void> {
  const client = requireClient();
  const result = await client.rpc('bramble.setTimezone', { timezone });
  assertOk(result, 'Failed to set timezone');
}

export async function shareLocationOnce(addr: number, tier?: LocationTier): Promise<void> {
  const client = requireClient();
  const params: Record<string, unknown> = { address: formatAddrHex(addr) };
  if (tier !== undefined) params.tier = tier;
  const result = await client.rpc('bramble.shareLocationOnce', params);
  assertOk(result, 'Failed to share location');
}
