import { render, screen, fireEvent } from '@testing-library/react';
import { describe, it, expect, vi } from 'vitest';
import { VerifySafetyNumber } from '../VerifySafetyNumber';

describe('VerifySafetyNumber', () => {
  it('shows the grouped SAS and marks verified', () => {
    const setVerified = vi.fn().mockResolvedValue({ ok: true });
    render(
      <VerifySafetyNumber
        peerAddress="aabbccdd"
        sas="1234567"
        verified={false}
        keyChanged={false}
        onSetVerified={setVerified}
      />,
    );
    expect(screen.getByText(/123\s?4567/)).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /mark verified/i }));
    expect(setVerified).toHaveBeenCalledWith('aabbccdd', true);
  });

  it('renders the key-changed warning banner', () => {
    render(
      <VerifySafetyNumber
        peerAddress="aabbccdd"
        sas="1234567"
        verified={false}
        keyChanged={true}
        onSetVerified={vi.fn()}
      />,
    );
    expect(screen.getByText(/safety number changed/i)).toBeInTheDocument();
  });
});
