import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import { IdentitySection } from './IdentitySection';
import { UNNAMED_NODE_NAME } from '../../utils/nodeName';
import type { NodeIdentity } from '../../types/bramble';

vi.mock('../../store/actions', () => ({
  saveNodeName: vi.fn(),
  setMailbox: vi.fn(),
}));

// Surface the two props under test instead of rendering a QR code.
vi.mock('../../components/QRShareModal', () => ({
  QRShareModal: ({ title, shareString }: { title: string; shareString: string }) => (
    <div>
      <span data-testid="share-title">{title}</span>
      <span data-testid="share-string">{shareString}</span>
    </div>
  ),
}));

function identity(name: string): NodeIdentity {
  return { address: 0xab12cd34, pubkeyHash: 0x01020304, name, pubkeyB64: 'AAAA' };
}

function openShare() {
  fireEvent.click(screen.getByRole('button', { name: /Share Node/ }));
  return {
    title: screen.getByTestId('share-title').textContent,
    shareString: screen.getByTestId('share-string').textContent ?? '',
  };
}

describe('IdentitySection node name', () => {
  it('starts the name field empty for an unnamed node, so Save cannot persist the sentinel', () => {
    render(<IdentitySection identity={identity(UNNAMED_NODE_NAME)} />);
    expect((screen.getByLabelText('Node name') as HTMLInputElement).value).toBe('');
  });

  it('keeps the sentinel out of the share title and the QR payload', () => {
    render(<IdentitySection identity={identity(UNNAMED_NODE_NAME)} />);
    const { title, shareString } = openShare();
    expect(title).toBe('Share node "0xAB12CD34"');
    expect(new URLSearchParams(shareString.split('?')[1]).get('n')).toBe('');
    expect(shareString).not.toContain('unnamed');
  });

  it('shows and shares a real name', () => {
    render(<IdentitySection identity={identity('Basecamp')} />);
    expect((screen.getByLabelText('Node name') as HTMLInputElement).value).toBe('Basecamp');
    const { title, shareString } = openShare();
    expect(title).toBe('Share node "Basecamp"');
    expect(new URLSearchParams(shareString.split('?')[1]).get('n')).toBe('Basecamp');
  });
});
