import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';

// Mock the store actions so we can assert exactly what the card sends,
// matching the idiom in AnchorSection.test.tsx.
const getBleSecurity = vi.fn<() => Promise<{ mode: string; staticPasskeySet: boolean }>>();
const setBlePasskey = vi.fn<(passkey: string | null) => Promise<void>>();

vi.mock('../../../store/actions', () => ({
  getBleSecurity: () => getBleSecurity(),
  setBlePasskey: (passkey: string | null) => setBlePasskey(passkey),
}));

import { BleSecurityCard } from '../DeviceManagementSection';

afterEach(cleanup);

beforeEach(() => {
  vi.clearAllMocks();
  setBlePasskey.mockResolvedValue(undefined);
});

describe('BleSecurityCard', () => {
  it('renders the mode text with no passkey form for passkey-display boards', async () => {
    getBleSecurity.mockResolvedValue({ mode: 'passkey-display', staticPasskeySet: false });
    render(<BleSecurityCard />);

    expect(await screen.findByText(/shows a random pairing code on its screen/)).toBeInTheDocument();
    expect(screen.queryByPlaceholderText('6 digits')).not.toBeInTheDocument();
  });

  it('sets a passkey in just-works mode and shows the re-pair warning', async () => {
    getBleSecurity.mockResolvedValue({ mode: 'just-works', staticPasskeySet: false });
    render(<BleSecurityCard />);

    const input = await screen.findByPlaceholderText('6 digits');
    fireEvent.change(input, { target: { value: '123456' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save passkey' }));

    await waitFor(() => expect(setBlePasskey).toHaveBeenCalledWith('123456'));
    expect(await screen.findByText(/paired devices were unpaired and must pair again/)).toBeInTheDocument();
  });

  it('shows a passkey-set state with a Clear button in static-passkey mode, and clears it', async () => {
    getBleSecurity.mockResolvedValue({ mode: 'static-passkey', staticPasskeySet: true });
    render(<BleSecurityCard />);

    expect(await screen.findByRole('button', { name: 'Clear' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Clear' }));

    await waitFor(() => expect(setBlePasskey).toHaveBeenCalledWith(null));
    expect(await screen.findByText(/paired devices were unpaired and must pair again/)).toBeInTheDocument();
  });

  it('disables save when the passkey is not exactly 6 digits', async () => {
    getBleSecurity.mockResolvedValue({ mode: 'just-works', staticPasskeySet: false });
    render(<BleSecurityCard />);

    const input = await screen.findByPlaceholderText('6 digits');
    fireEvent.change(input, { target: { value: '12345' } });

    expect(screen.getByRole('button', { name: 'Save passkey' })).toBeDisabled();
  });
});
