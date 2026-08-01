import { describe, expect, it } from 'vitest';
import { render, screen } from '@testing-library/react';
import { SystemInfo } from '../../src/pages/Stats/SystemInfo';

const config: any = {
  identity: {
    name: 'Node Alpha',
    address: 0x1a2b,
    pubkeyHash: 0x3c4d,
  },
};

const baseStatus = {
  uptimeSec: 123,
  freeHeapBytes: 50000,
  fwVersion: 'v1.2.3',
};

describe('SystemInfo battery row', () => {
  it('shows the charging indicator with the rail mV and hides the percentage while charging', () => {
    const status: any = {
      ...baseStatus,
      batteryPct: 80,
      batteryMv: 4300,
      present: true,
      charging: 'yes',
    };

    render(<SystemInfo status={status} config={config} />);

    expect(screen.getByText('⚡ Charging (4300 mV rail)')).toBeInTheDocument();
    expect(screen.queryByText(/80%/)).not.toBeInTheDocument();
  });

  it('treats present: false as no battery even when the voltage looks plausible', () => {
    const status: any = {
      ...baseStatus,
      batteryPct: 80,
      batteryMv: 4000,
      present: false,
      charging: 'no',
    };

    render(<SystemInfo status={status} config={config} />);

    expect(screen.getByText('N/A')).toBeInTheDocument();
    expect(screen.queryByText(/4000 mV/)).not.toBeInTheDocument();
  });

  it('falls back to the mv heuristic when present is absent, matching older firmware', () => {
    const status: any = {
      ...baseStatus,
      batteryPct: 80,
      batteryMv: 4000,
      // present and charging both omitted, as older firmware does.
    };

    render(<SystemInfo status={status} config={config} />);

    expect(screen.getByText('80% (4000 mV)')).toBeInTheDocument();
  });
});
