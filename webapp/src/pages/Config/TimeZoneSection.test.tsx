import { describe, expect, it, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { TimeZoneSection } from './TimeZoneSection';
import type { TimezoneInfo } from '../../types/bramble';

const loadTimezone = vi.fn();
const setTimezone = vi.fn();

vi.mock('../../store/actions', () => ({
  loadTimezone: (...args: unknown[]) => loadTimezone(...args),
  setTimezone: (...args: unknown[]) => setTimezone(...args),
}));

const PRESETS = [
  { label: 'UTC', spec: 'UTC0' },
  { label: 'US Pacific', spec: 'PST8PDT,M3.2.0,M11.1.0' },
];

function info(overrides: Partial<TimezoneInfo> = {}): TimezoneInfo {
  return { timezone: 'UTC0', defaultTimezone: 'UTC0', configured: false, presets: PRESETS, ...overrides };
}

describe('TimeZoneSection', () => {
  beforeEach(() => {
    loadTimezone.mockReset();
    setTimezone.mockReset();
  });

  it('selects the preset matching the zone the node reports', async () => {
    loadTimezone.mockResolvedValue(info({ timezone: 'PST8PDT,M3.2.0,M11.1.0', configured: true }));
    render(<TimeZoneSection />);

    const select = (await screen.findByLabelText('Device zone')) as HTMLSelectElement;
    expect(select.value).toBe('PST8PDT,M3.2.0,M11.1.0');
    expect(screen.queryByLabelText('POSIX TZ')).not.toBeInTheDocument();
  });

  it('marks an unconfigured node as showing the default zone', async () => {
    loadTimezone.mockResolvedValue(info());
    render(<TimeZoneSection />);

    expect(await screen.findByText('(default)')).toBeInTheDocument();
  });

  it('persists a preset as soon as it is picked', async () => {
    loadTimezone.mockResolvedValue(info());
    setTimezone.mockResolvedValue(undefined);
    render(<TimeZoneSection />);

    const select = await screen.findByLabelText('Device zone');
    fireEvent.change(select, { target: { value: 'PST8PDT,M3.2.0,M11.1.0' } });

    await waitFor(() => expect(setTimezone).toHaveBeenCalledWith('PST8PDT,M3.2.0,M11.1.0'));
  });

  it('offers a free-text field for a zone that is not a preset', async () => {
    loadTimezone.mockResolvedValue(info({ timezone: 'IST-5:30', configured: true }));
    render(<TimeZoneSection />);

    const custom = (await screen.findByLabelText('POSIX TZ')) as HTMLInputElement;
    expect(custom.value).toBe('IST-5:30');
  });

  it('sends a custom specification only when Apply is pressed', async () => {
    loadTimezone.mockResolvedValue(info());
    setTimezone.mockResolvedValue(undefined);
    render(<TimeZoneSection />);

    const select = await screen.findByLabelText('Device zone');
    fireEvent.change(select, { target: { value: '__custom__' } });
    expect(setTimezone).not.toHaveBeenCalled();

    fireEvent.change(screen.getByLabelText('POSIX TZ'), { target: { value: 'JST-9' } });
    expect(setTimezone).not.toHaveBeenCalled();

    fireEvent.click(screen.getByText('Apply'));
    await waitFor(() => expect(setTimezone).toHaveBeenCalledWith('JST-9'));
  });

  it('surfaces a rejection from the node', async () => {
    loadTimezone.mockResolvedValue(info());
    setTimezone.mockRejectedValue(new Error('not a POSIX TZ specification'));
    render(<TimeZoneSection />);

    const select = await screen.findByLabelText('Device zone');
    fireEvent.change(select, { target: { value: 'PST8PDT,M3.2.0,M11.1.0' } });

    expect(await screen.findByText(/not a POSIX TZ specification/)).toBeInTheDocument();
  });
});
