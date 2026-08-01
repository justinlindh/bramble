import { describe, expect, it } from 'vitest';
import { normalizeStatus } from '../actions';

describe('normalizeStatus gpsEnabled', () => {
  it('maps the firmware gps_enabled field', () => {
    const status = normalizeStatus({ gps_available: true, gps_enabled: false });
    expect(status.gpsAvailable).toBe(true);
    expect(status.gpsEnabled).toBe(false);
  });

  it('falls back to the legacy camelCase alias', () => {
    const status = normalizeStatus({ gpsAvailable: true, gpsEnabled: false });
    expect(status.gpsEnabled).toBe(false);
  });

  it('defaults to enabled when the field is absent, matching the firmware default', () => {
    const status = normalizeStatus({});
    expect(status.gpsEnabled).toBe(true);
  });
});

describe('normalizeStatus charging and present', () => {
  it.each(['unknown', 'no', 'yes'] as const)('passes through charging=%s', (charging) => {
    const status = normalizeStatus({ charging });
    expect(status.charging).toBe(charging);
  });

  it('leaves charging undefined when the field is absent, matching older firmware', () => {
    const status = normalizeStatus({});
    expect(status.charging).toBeUndefined();
  });

  it('never fabricates "no" for absent charging', () => {
    const status = normalizeStatus({ battery_mv: 4200, battery_pct: 90 });
    expect(status.charging).not.toBe('no');
    expect(status.charging).toBeUndefined();
  });

  it.each([true, false])('passes through present=%s', (present) => {
    const status = normalizeStatus({ present });
    expect(status.present).toBe(present);
  });

  it('leaves present undefined when the field is absent, matching older firmware', () => {
    const status = normalizeStatus({});
    expect(status.present).toBeUndefined();
  });
});
