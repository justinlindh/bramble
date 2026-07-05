import { afterEach, describe, expect, it } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { UnprovisionedBanner } from './UnprovisionedBanner';
import { useStore } from '../store/index';

afterEach(cleanup);

describe('UnprovisionedBanner', () => {
  it('shows a prominent inert warning when connected and unprovisioned', () => {
    useStore.setState({
      connectionState: 'connected',
      networkKeyStatus: { provisioned: false, fingerprint: '00000000' },
    } as any);

    render(<UnprovisionedBanner />);

    expect(screen.getByRole('alert')).toBeInTheDocument();
    expect(screen.getByText(/UNPROVISIONED and inert/i)).toBeInTheDocument();
    expect(screen.getByText(/not meshing/i)).toBeInTheDocument();
  });

  it('jumps to the Config tab when the action is clicked', () => {
    useStore.setState({
      connectionState: 'connected',
      activeTab: 'chat',
      networkKeyStatus: { provisioned: false, fingerprint: '00000000' },
    } as any);

    render(<UnprovisionedBanner />);
    fireEvent.click(screen.getByRole('button', { name: /provision/i }));

    expect(useStore.getState().activeTab).toBe('config');
  });

  it('renders nothing once a key is provisioned', () => {
    useStore.setState({
      connectionState: 'connected',
      networkKeyStatus: { provisioned: true, fingerprint: 'deadbeef' },
    } as any);

    const { container } = render(<UnprovisionedBanner />);
    expect(container).toBeEmptyDOMElement();
  });

  it('renders nothing while disconnected even if status is stale-unprovisioned', () => {
    useStore.setState({
      connectionState: 'disconnected',
      networkKeyStatus: { provisioned: false, fingerprint: '00000000' },
    } as any);

    const { container } = render(<UnprovisionedBanner />);
    expect(container).toBeEmptyDOMElement();
  });
});
