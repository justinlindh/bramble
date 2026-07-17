import { afterEach, describe, expect, it, vi } from 'vitest';
import { cleanup, render, screen } from '@testing-library/react';

// The flasher link must not render inside Electron/Android shells: the flasher
// ships as a hosted sibling path and needs the browser's Web Serial prompt.
const platformMocks = vi.hoisted(() => ({ isEmbeddedShell: vi.fn(() => false) }));
vi.mock('../../utils/platform', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../utils/platform')>();
  return { ...actual, isEmbeddedShell: platformMocks.isEmbeddedShell };
});

import { ConnectionOverlay } from '../ConnectionOverlay';

afterEach(() => {
  cleanup();
  platformMocks.isEmbeddedShell.mockReset();
  platformMocks.isEmbeddedShell.mockReturnValue(false);
});

describe('ConnectionOverlay web flasher link', () => {
  it('links brand-new-device users to the web flasher', () => {
    render(<ConnectionOverlay />);
    const link = screen.getByRole('link', { name: /flash/i });
    expect(link).toHaveAttribute('href', './web-flasher/');
  });

  it('hides the flasher link in embedded shells', () => {
    platformMocks.isEmbeddedShell.mockReturnValue(true);
    render(<ConnectionOverlay />);
    expect(screen.queryByRole('link', { name: /flash/i })).toBeNull();
  });
});
