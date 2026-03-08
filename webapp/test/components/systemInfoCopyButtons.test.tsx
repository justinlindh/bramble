import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { SystemInfo } from '../../src/pages/Stats/SystemInfo';

const status: any = {
  uptimeSec: 123,
  freeHeapBytes: 50000,
  fwVersion: 'v1.2.3',
  batteryPct: 80,
  batteryMv: 4000,
};

const config: any = {
  identity: {
    name: 'Node Alpha',
    address: 0x1a2b,
    pubkeyHash: 0x3c4d,
  },
};

describe('SystemInfo copy controls', () => {
  const originalClipboard = navigator.clipboard;

  beforeEach(() => {
    vi.restoreAllMocks();
  });

  afterEach(() => {
    Object.defineProperty(navigator, 'clipboard', {
      configurable: true,
      value: originalClipboard,
    });
  });

  it('shows copy buttons for Address and Pubkey Hash and copies via Clipboard API', async () => {
    const writeText = vi.fn(async () => {});
    Object.defineProperty(navigator, 'clipboard', {
      configurable: true,
      value: { writeText },
    });

    render(<SystemInfo status={status} config={config} />);

    const copyAddress = screen.getByRole('button', { name: /copy address/i });
    const copyPubkey = screen.getByRole('button', { name: /copy pubkey hash/i });

    fireEvent.click(copyAddress);
    expect(writeText).toHaveBeenCalledWith('0x00001A2B');

    await waitFor(() => {
      expect(copyAddress).toHaveAttribute('title', 'Copied!');
    });

    fireEvent.click(copyPubkey);
    await waitFor(() => {
      expect(writeText).toHaveBeenCalledWith('0x00003C4D');
      expect(copyPubkey).toHaveAttribute('title', 'Copied!');
    });
  });

  it('falls back to execCommand copy when Clipboard API is unavailable', async () => {
    Object.defineProperty(navigator, 'clipboard', {
      configurable: true,
      value: undefined,
    });

    const execSpy = vi.fn(() => true);
    Object.defineProperty(document, 'execCommand', {
      configurable: true,
      value: execSpy,
    });

    render(<SystemInfo status={status} config={config} />);

    const copyAddress = screen.getByRole('button', { name: /copy address/i });
    fireEvent.click(copyAddress);

    await waitFor(() => {
      expect(execSpy).toHaveBeenCalledWith('copy');
      expect(copyAddress).toHaveAttribute('title', 'Copied!');
    });
  });
});
