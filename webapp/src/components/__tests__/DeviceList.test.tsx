import { render, screen, fireEvent } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { DeviceList } from '../DeviceList';
import { useStore } from '../../store/index';
import { upsertDevice, setDeviceToken } from '../../lib/deviceBook';
import * as actions from '../../store/actions';

beforeEach(() => {
  localStorage.clear(); sessionStorage.clear();
  useStore.getState().setDevices([]);
});

describe('DeviceList', () => {
  it('renders saved devices and one-click connect passes the stored token + expectAddressHex', () => {
    upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '198.51.100.146', transport: 'wifi', remember: true, nowMs: 1 });
    setDeviceToken('DEADBEEF', 'tok', true);
    actions.refreshDevices();
    const spy = vi.spyOn(actions, 'connect').mockResolvedValue();
    render(<DeviceList />);
    expect(screen.getByText('V4')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /connect to V4/i }));
    expect(spy).toHaveBeenCalledWith('wifi', expect.objectContaining({
      token: 'tok', ip: '198.51.100.146', expectAddressHex: 'DEADBEEF', remember: true,
    }));
  });
  it('forget removes the device from the list', () => {
    upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '192.0.2.1', transport: 'wifi', remember: false, nowMs: 1 });
    actions.refreshDevices();
    render(<DeviceList />);
    fireEvent.click(screen.getByRole('button', { name: /forget V4/i }));
    expect(screen.queryByText('V4')).not.toBeInTheDocument();
  });
});
