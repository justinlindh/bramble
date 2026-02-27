import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import { RadioForm } from './RadioForm';
import type { RadioConfig } from '../../types/bramble';

vi.mock('../../store/actions', () => ({
  saveRadio: vi.fn(),
}));

const baseRadio: RadioConfig = {
  txPowerDbm: 10,
  sf: 9,
  bwKhz: 125,
  cr: 5,
  freqMhz: 915,
};

describe('RadioForm frequency guidance', () => {
  it('shows regional guidance without warning for in-band frequencies', () => {
    render(<RadioForm radio={baseRadio} />);

    expect(screen.getByText('Regional band: US915 902–928 MHz')).toBeInTheDocument();
    expect(screen.queryByText(/outside common regional ISM bands/i)).not.toBeInTheDocument();

    const input = screen.getByLabelText('Frequency in MHz');
    expect(input).not.toHaveAttribute('aria-invalid', 'true');
  });

  it('shows out-of-band warning state for frequencies outside known regional ISM bands', () => {
    render(<RadioForm radio={baseRadio} />);

    const input = screen.getByLabelText('Frequency in MHz');
    fireEvent.change(input, { target: { value: '500' } });

    expect(screen.getByText('Regional band: Out of known regional ISM ranges')).toBeInTheDocument();
    expect(screen.getByText(/outside common regional ISM bands/i)).toBeInTheDocument();
    expect(input).toHaveAttribute('aria-invalid', 'true');
  });
});
