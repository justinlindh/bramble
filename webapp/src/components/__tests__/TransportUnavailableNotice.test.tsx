import { describe, it, expect, afterEach } from 'vitest';
import { cleanup, render, screen } from '@testing-library/react';
import { TransportUnavailableNotice } from '../TransportUnavailableNotice';
import type { TransportUnavailable } from '../../lib/transportAvailability';

const BASE: TransportUnavailable = {
  available: false,
  caption: 'Unavailable here',
  heading: 'WiFi needs a direct connection to your node',
  body: 'Browsers block this.',
  alternatives: 'USB and Bluetooth work here.',
};

afterEach(cleanup);

describe('TransportUnavailableNotice', () => {
  it('renders the heading, body, and alternatives', () => {
    render(<TransportUnavailableNotice info={BASE} />);
    expect(screen.getByText('WiFi needs a direct connection to your node')).toBeInTheDocument();
    expect(screen.getByText('Browsers block this.')).toBeInTheDocument();
    expect(screen.getByText('USB and Bluetooth work here.')).toBeInTheDocument();
  });

  it('renders no link when there is no cta', () => {
    render(<TransportUnavailableNotice info={BASE} />);
    expect(screen.queryByRole('link')).toBeNull();
  });

  it('renders the cta as a safe external link', () => {
    render(
      <TransportUnavailableNotice
        info={{ ...BASE, cta: { label: 'Get the desktop app', href: 'https://example.invalid/releases' } }}
      />,
    );
    const link = screen.getByRole('link', { name: 'Get the desktop app' });
    expect(link).toHaveAttribute('href', 'https://example.invalid/releases');
    expect(link).toHaveAttribute('target', '_blank');
    expect(link.getAttribute('rel')).toContain('noopener');
  });
});
