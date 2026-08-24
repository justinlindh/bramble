import { beforeEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { PeerManager } from '../../src/pages/Config/PeerManager';
import type { Neighbor, PeerLocation, Route } from '../../src/types/bramble';

describe('PeerManager contact import/export', () => {
  const neighbors: Neighbor[] = [{ addr: 0x1234abcd, rssi: -70, snr: 10, lastHeardMs: 1200 }];
  const routes: Route[] = [];
  const peerLocations: PeerLocation[] = [];

  beforeEach(() => {
    localStorage.clear();
    vi.restoreAllMocks();
  });

  it('exports contacts from localStorage as JSON download', async () => {
    localStorage.setItem('bramble:peerNames', JSON.stringify({ [0x1234abcd]: 'Alice' }));

    const createObjectURL = vi.fn(() => 'blob:test');
    const revokeObjectURL = vi.fn();
    Object.defineProperty(URL, 'createObjectURL', { value: createObjectURL, writable: true });
    Object.defineProperty(URL, 'revokeObjectURL', { value: revokeObjectURL, writable: true });
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});

    render(<PeerManager neighbors={neighbors} routes={routes} peerLocations={peerLocations} />);

    fireEvent.click(screen.getByRole('button', { name: 'Export Contacts' }));

    expect(createObjectURL).toHaveBeenCalledTimes(1);
    const createdBlob = (createObjectURL.mock.calls as unknown as Array<[Blob]>)[0][0];
    expect(createdBlob).toBeInstanceOf(Blob);
    expect(revokeObjectURL).toHaveBeenCalledWith('blob:test');
  });

  it('imports contacts and keeps existing names when conflicts are declined', async () => {
    localStorage.setItem('bramble:peerNames', JSON.stringify({ [0x1234abcd]: 'Existing' }));
    vi.spyOn(window, 'confirm').mockReturnValue(false);

    render(<PeerManager neighbors={neighbors} routes={routes} peerLocations={peerLocations} />);

    const input = screen.getByLabelText('Import contacts file') as HTMLInputElement;
    const file = new File([
      JSON.stringify({
        version: 1,
        contacts: {
          '1234ABCD': { name: 'Imported' },
          '89ABCDEF': { name: 'New Contact', note: 'added from backup' },
        },
      }),
    ], 'contacts.json', { type: 'application/json' });

    fireEvent.change(input, { target: { files: [file] } });

    await waitFor(() => {
      const stored = JSON.parse(localStorage.getItem('bramble:peerNames') || '{}');
      expect(stored[String(0x1234abcd)]).toBe('Existing');
      expect(stored[String(0x89abcdef)]).toBe('New Contact');
    });
  });

  it('allows editing and persisting notes for a peer', async () => {
    localStorage.setItem('bramble:peerNames', JSON.stringify({ [0x1234abcd]: 'Alice' }));

    render(<PeerManager neighbors={neighbors} routes={routes} peerLocations={peerLocations} />);

    fireEvent.click(screen.getByRole('button', { name: /edit/i }));
    fireEvent.change(screen.getByLabelText('Peer notes'), { target: { value: 'Met at meetup' } });
    fireEvent.click(screen.getByRole('button', { name: '✓' }));

    await waitFor(() => {
      const storedNotes = JSON.parse(localStorage.getItem('bramble:peerNotes') || '{}');
      expect(storedNotes[String(0x1234abcd)]).toBe('Met at meetup');
    });
  });

  it('lists a peer known only through location telemetry', () => {
    localStorage.setItem('bramble:peerNames', JSON.stringify({ [0x89abcdef]: 'LocOnly' }));
    const locationOnly: PeerLocation[] = [
      { addr: 0x89abcdef, name: '', tier: 'presence', position: null, online: true, lastUpdatedMs: 500 },
    ];

    render(<PeerManager neighbors={[]} routes={[]} peerLocations={locationOnly} />);

    // The peer appears in the list even though it is neither a neighbor nor a
    // route destination, matching the Nodes tab's known-peer union.
    expect(screen.getByText('LocOnly')).toBeInTheDocument();
  });
});
