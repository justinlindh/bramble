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
  it('renders the heading, body, and alternatives in that order', () => {
    const { container } = render(<TransportUnavailableNotice info={BASE} />);
    // Checked by element and position, not just presence: the body and the
    // alternatives sentence are both paragraphs, and swapping them would
    // still satisfy a bare text lookup.
    expect(screen.getByRole('heading')).toHaveTextContent('WiFi needs a direct connection to your node');
    const paragraphs = container.querySelectorAll('p');
    expect(paragraphs).toHaveLength(2);
    expect(paragraphs[0]).toHaveTextContent('Browsers block this.');
    expect(paragraphs[1]).toHaveTextContent('USB and Bluetooth work here.');
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
