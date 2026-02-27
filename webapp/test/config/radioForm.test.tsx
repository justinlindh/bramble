import { describe, it, expect, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { RadioForm } from '../../src/pages/Config/RadioForm';
import type { RadioConfig } from '../../src/types/bramble';

const { saveRadioMock } = vi.hoisted(() => ({
  saveRadioMock: vi.fn(async () => {}),
}));

vi.mock('../../src/store/actions', async () => {
  const actual = await vi.importActual<typeof import('../../src/store/actions')>('../../src/store/actions');
  return {
    ...actual,
    saveRadio: saveRadioMock,
  };
});

const baseRadio: RadioConfig = {
  txPowerDbm: 10,
  sf: 9,
  bwKhz: 125,
  cr: 5,
  freqMhz: 915.0,
};

describe('RadioForm dirty state and revert behavior', () => {
  beforeEach(() => {
    saveRadioMock.mockClear();
  });

  it('shows unsaved indicator and risky-field change markers when edited', () => {
    render(<RadioForm radio={baseRadio} />);

    expect(screen.queryByText(/unsaved changes/i)).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: /save radio settings/i })).toBeDisabled();

    fireEvent.change(screen.getByLabelText(/spreading factor/i), { target: { value: '10' } });
    fireEvent.change(screen.getByLabelText(/bandwidth/i), { target: { value: '250' } });
    fireEvent.change(screen.getByLabelText(/frequency in mhz/i), { target: { value: '916.1' } });

    expect(screen.getByText(/unsaved changes/i)).toBeInTheDocument();
    expect(screen.getAllByText('changed')).toHaveLength(3);
    expect(screen.getByRole('button', { name: /save radio settings/i })).not.toBeDisabled();
    expect(screen.getByRole('button', { name: /revert/i })).not.toBeDisabled();
  });

  it('reverts edited values back to persisted config', () => {
    render(<RadioForm radio={baseRadio} />);

    const sf = screen.getByLabelText(/spreading factor/i) as HTMLSelectElement;
    const bw = screen.getByLabelText(/bandwidth/i) as HTMLSelectElement;
    const freq = screen.getByLabelText(/frequency in mhz/i) as HTMLInputElement;

    fireEvent.change(sf, { target: { value: '12' } });
    fireEvent.change(bw, { target: { value: '500' } });
    fireEvent.change(freq, { target: { value: '920.5' } });

    fireEvent.click(screen.getByRole('button', { name: /revert/i }));

    expect(sf.value).toBe(String(baseRadio.sf));
    expect(bw.value).toBe(String(baseRadio.bwKhz));
    expect(freq.value).toBe(String(baseRadio.freqMhz));
    expect(screen.queryByText(/unsaved changes/i)).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: /save radio settings/i })).toBeDisabled();
  });

  it('saves dirty form and clears dirty state against new persisted values', async () => {
    render(<RadioForm radio={baseRadio} />);

    fireEvent.change(screen.getByLabelText(/frequency in mhz/i), { target: { value: '917.2' } });
    fireEvent.click(screen.getByRole('button', { name: /save radio settings/i }));

    await waitFor(() => {
      expect(saveRadioMock).toHaveBeenCalledWith(expect.objectContaining({
        ...baseRadio,
        freqMhz: 917.2,
      }));
    });

    await waitFor(() => {
      expect(screen.queryByText(/unsaved changes/i)).not.toBeInTheDocument();
      expect(screen.getByRole('button', { name: /save radio settings/i })).toBeDisabled();
      expect(screen.getByRole('button', { name: /revert/i })).toBeDisabled();
    });
  });
});
