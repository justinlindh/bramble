import { afterEach, describe, expect, it } from 'vitest';
import { cleanup, render, screen } from '@testing-library/react';
import { SystemInfo } from '../../src/pages/Stats/SystemInfo';

/**
 * The Stats page spells out used, tracked and in view separately so a node with
 * satellites listed but none heard reads differently from a cold start. These
 * assertions pin that the three GNSS classes stay three readings.
 */

afterEach(cleanup);

const config: any = {
  identity: { name: 'Node Alpha', address: 0x1a2b, pubkeyHash: 0x3c4d },
};

function baseStatus(extra: Record<string, unknown>): any {
  return { uptimeSec: 123, freeHeapBytes: 50_000, fwVersion: 'v1.2.3', ...extra };
}

function valueOf(label: string): string {
  const dt = screen.getByText(label);
  return dt.parentElement?.querySelector('dd')?.textContent ?? '';
}

describe('SystemInfo GNSS rows', () => {
  it('reports no signal with the counts that distinguish it from acquiring', () => {
    render(
      <SystemInfo
        status={baseStatus({
          gpsAvailable: true,
          gpsState: 'no_signal',
          gpsSatsInView: 8,
          gpsSatsTracked: 0,
          gpsSatsUsed: 0,
          gpsSnrMaxDbHz: 0,
        })}
        config={config}
      />,
    );

    expect(valueOf('GNSS')).toBe('no signal');
    expect(valueOf('Satellites')).toBe('0 used / 0 tracked / 8 in view');
    expect(screen.queryByText('Best C/N0')).toBeNull();
  });

  it('reports acquiring with the best carrier-to-noise ratio', () => {
    render(
      <SystemInfo
        status={baseStatus({
          gpsAvailable: true,
          gpsState: 'acquiring',
          gpsSatsInView: 12,
          gpsSatsTracked: 7,
          gpsSatsUsed: 0,
          gpsSnrMaxDbHz: 22,
        })}
        config={config}
      />,
    );

    expect(valueOf('GNSS')).toBe('acquiring');
    expect(valueOf('Satellites')).toBe('0 used / 7 tracked / 12 in view');
    expect(valueOf('Best C/N0')).toBe('22 dBHz');
  });

  it('reports a fix', () => {
    render(
      <SystemInfo
        status={baseStatus({
          gpsAvailable: true,
          gpsState: 'fix',
          gpsSatsInView: 12,
          gpsSatsTracked: 6,
          gpsSatsUsed: 6,
          gpsSnrMaxDbHz: 45,
        })}
        config={config}
      />,
    );

    expect(valueOf('GNSS')).toBe('fix');
    expect(valueOf('Satellites')).toBe('6 used / 6 tracked / 12 in view');
  });

  it('says unknown rather than zero on firmware that omits the fields', () => {
    render(<SystemInfo status={baseStatus({ gpsAvailable: true })} config={config} />);

    expect(valueOf('GNSS')).toBe('unknown');
    expect(valueOf('Satellites')).toBe('unknown');
  });

  it('omits the block entirely on a board with no receiver', () => {
    render(<SystemInfo status={baseStatus({ gpsAvailable: false })} config={config} />);

    expect(screen.queryByText('GNSS')).toBeNull();
    expect(screen.queryByText('Satellites')).toBeNull();
  });
});
