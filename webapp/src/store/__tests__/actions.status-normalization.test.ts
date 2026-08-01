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
