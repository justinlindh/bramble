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

describe('normalizeStatus GNSS observability fields', () => {
  it('maps the three-way state and every satellite count', () => {
    const status = normalizeStatus({
      gps_available: true,
      gps_state: 'acquiring',
      gps_sats_in_view: 12,
      gps_sats_tracked: 7,
      gps_sats_used: 0,
      gps_snr_max_dbhz: 22,
      gps_fix_quality: 0,
    });
    expect(status.gpsState).toBe('acquiring');
    expect(status.gpsSatsInView).toBe(12);
    expect(status.gpsSatsTracked).toBe(7);
    expect(status.gpsSatsUsed).toBe(0);
    expect(status.gpsSnrMaxDbHz).toBe(22);
    expect(status.gpsFixQuality).toBe(0);
  });

  it('leaves the fields undefined on firmware that omits them, never zero', () => {
    // A ?? 0 here would report "no satellites" on every older node, which is
    // exactly the misdiagnosis these fields exist to prevent.
    const status = normalizeStatus({ gps_available: true, gps_enabled: true });
    expect(status.gpsState).toBeUndefined();
    expect(status.gpsSatsInView).toBeUndefined();
    expect(status.gpsSatsTracked).toBeUndefined();
    expect(status.gpsSatsUsed).toBeUndefined();
    expect(status.gpsSnrMaxDbHz).toBeUndefined();
    expect(status.gpsFixQuality).toBeUndefined();
  });
});
