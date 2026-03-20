import { describe, it, expect, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { LocationSection } from '../../src/pages/Config/LocationSection';
import type { LocationConfig, Neighbor, Channel } from '../../src/types/bramble';

const { setLocationConfigMock } = vi.hoisted(() => ({
  setLocationConfigMock: vi.fn(async () => {}),
}));

vi.mock('../../src/store/actions', async () => {
  const actual = await vi.importActual<typeof import('../../src/store/actions')>('../../src/store/actions');
  return {
    ...actual,
    setLocationConfig: setLocationConfigMock,
  };
});

function makeLocation(overrides: Partial<LocationConfig> = {}): LocationConfig {
  return {
    enabled: false,
    tier: 'coarse',
    default_tier: 'coarse',
    interval_s: 300,
    source: 'hybrid',
    contact_rules: [],
    channel_targets: [],
    ...overrides,
  };
}

describe('LocationSection hybrid policy controls', () => {
  const neighbors: Neighbor[] = [{ addr: 0x1234abcd, rssi: -80, snr: 8, deliveryRate: 255, lastHeardMs: 1000, airtimeRemaining: 100 }];
  const channels: Channel[] = [
    { index: 0, name: 'General', hasPsk: false, epoch: 1, isDefault: true },
    { index: 2, name: 'Ops', hasPsk: true, epoch: 2, isDefault: false },
  ];

  beforeEach(() => {
    setLocationConfigMock.mockClear();
  });

  it('saves edited policy config through setLocationConfig', async () => {
    render(<LocationSection location={makeLocation()} neighbors={neighbors} channels={channels} gpsAvailable />);

    fireEvent.click(screen.getByLabelText('Enable location sharing'));
    fireEvent.change(screen.getByLabelText('Default tier'), { target: { value: 'presence' } });
    fireEvent.change(screen.getByLabelText('Interval (seconds)'), { target: { value: '120' } });
    fireEvent.change(screen.getByLabelText('Source'), { target: { value: 'manual' } });

    fireEvent.click(screen.getByRole('button', { name: 'Save location policy' }));

    await waitFor(() => {
      expect(setLocationConfigMock).toHaveBeenCalledWith(expect.objectContaining({
        enabled: true,
        default_tier: 'presence',
        tier: 'presence',
        interval_s: 120,
        source: 'manual',
      }));
    });
  });

  it('edits both contact rules and channel targets before saving', async () => {
    render(<LocationSection location={makeLocation({ enabled: true })} neighbors={neighbors} channels={channels} gpsAvailable />);

    fireEvent.change(screen.getByLabelText('Contact address (hex)'), { target: { value: '1234ABCD' } });
    fireEvent.click(screen.getByRole('button', { name: 'Add contact target' }));

    fireEvent.change(screen.getByLabelText('Channel target'), { target: { value: '2' } });
    fireEvent.click(screen.getByRole('button', { name: 'Add channel target' }));

    fireEvent.click(screen.getByRole('button', { name: 'Save location policy' }));

    await waitFor(() => {
      expect(setLocationConfigMock).toHaveBeenCalled();
    });

    const calls = setLocationConfigMock.mock.calls as unknown as Array<[LocationConfig]>;
    expect(calls.length).toBeGreaterThan(0);
    const payload = calls[0][0];
    expect(payload.contact_rules).toHaveLength(1);
    expect(payload.contact_rules?.[0]).toEqual(expect.objectContaining({
      address: '1234ABCD',
      enabled: true,
      tier: 'coarse',
    }));

    expect(payload.channel_targets).toHaveLength(1);
    expect(payload.channel_targets?.[0]).toEqual(expect.objectContaining({
      channel: 2,
      enabled: true,
      tier: 'coarse',
    }));
  });

  it('uses primary action styling for add target buttons', () => {
    render(<LocationSection location={makeLocation()} neighbors={[]} channels={channels} gpsAvailable />);

    const addContact = screen.getByRole('button', { name: 'Add contact target' });
    const addChannel = screen.getByRole('button', { name: 'Add channel target' });

    expect(addContact.className).toContain('btnConfirm');
    expect(addChannel.className).toContain('btnConfirm');
    expect(addContact.className).not.toContain('btnCancel');
    expect(addChannel.className).not.toContain('btnCancel');
  });

  it('shows privacy-first defaults as opt-in in preview', () => {
    render(<LocationSection location={makeLocation()} neighbors={[]} channels={channels} gpsAvailable />);

    expect(screen.getByText(/sharing is off/i)).toBeInTheDocument();
    expect((screen.getByLabelText('Enable location sharing') as HTMLInputElement).checked).toBe(false);
    expect((screen.getByLabelText('Default tier') as HTMLSelectElement).value).toBe('coarse');
  });

  it('shows tier description for the selected default tier', () => {
    render(<LocationSection location={makeLocation()} neighbors={[]} channels={channels} gpsAvailable />);

    expect(screen.getByText('Approximate area (grid square ~1km).')).toBeInTheDocument();

    fireEvent.change(screen.getByLabelText('Default tier'), { target: { value: 'presence' } });
    expect(screen.getByText('Online status only; no coordinates.')).toBeInTheDocument();

    fireEvent.change(screen.getByLabelText('Default tier'), { target: { value: 'off' } });
    expect(screen.getByText('Location not shared.')).toBeInTheDocument();

    fireEvent.change(screen.getByLabelText('Default tier'), { target: { value: 'full' } });
    expect(screen.getByText('Precise GPS coordinates.')).toBeInTheDocument();
  });
});
