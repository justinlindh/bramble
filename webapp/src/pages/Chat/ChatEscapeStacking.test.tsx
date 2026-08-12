import { describe, it, expect, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';

// Regression test for overlay stacking: with the mobile sidebar open AND a
// ConversationList dialog open on top, one Escape must dismiss ONLY the
// dialog (topmost overlay); a second Escape closes the sidebar. Uses the
// REAL ConversationList (unlike Chat.test.tsx, which mocks it) so the
// dialog's stopPropagation and the sidebar's window-level listener interact
// exactly as they do in the app.

let state: any;

// Keep the real module (parseConversationId, formatConversationLabel) and only
// stub the store hook.
vi.mock('../../store/index', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../store/index')>()),
  useStore: (selector: any) => selector(state),
}));

vi.mock('../../store/selectors', () => ({
  useConversation: () => ({ messages: [] }),
}));

vi.mock('../../store/actions', () => ({
  loadPeerVerification: vi.fn(),
  setPeerVerified: vi.fn(),
  addChannel: vi.fn(),
}));

vi.mock('./ComposeBar', () => ({
  ComposeBar: () => <div data-testid="compose-bar" />,
}));

import { Chat } from './Chat';

describe('Escape overlay stacking (sidebar + dialog)', () => {
  beforeEach(() => {
    state = {
      conversations: new globalThis.Map(),
      activeConversationId: 'broadcast',
      setActiveConversation: vi.fn(),
      config: null,
      showRoutes: false,
      setShowRoutes: vi.fn(),
      connectionState: 'connected',
      status: { uptimeSec: 10, gpsAvailable: false },
      peerVerifications: new globalThis.Map(),
      neighbors: [],
      routes: [],
      peerNames: new globalThis.Map(),
      peerLocations: [],
    };
  });

  function openSidebar() {
    fireEvent.click(screen.getByLabelText('Open conversations'));
  }

  function sidebarBackdrop(): HTMLElement {
    return document.querySelector('[role="presentation"]') as HTMLElement;
  }

  it('first Escape closes only the DM dialog; second Escape closes the sidebar', () => {
    render(<Chat />);

    openSidebar();
    expect(sidebarBackdrop().className).toMatch(/sidebarBackdropOpen/);

    fireEvent.click(screen.getByLabelText('New direct message'));
    expect(screen.getByRole('dialog', { name: 'New Direct Message' })).toBeInTheDocument();

    // Fire on the autofocused input inside the dialog, like a real keypress:
    // the event must bubble to the dialog handler and STOP there.
    fireEvent.keyDown(screen.getByPlaceholderText('0xABCD1234'), { key: 'Escape' });

    expect(screen.queryByRole('dialog', { name: 'New Direct Message' })).not.toBeInTheDocument();
    expect(sidebarBackdrop().className).toMatch(/sidebarBackdropOpen/);

    fireEvent.keyDown(window, { key: 'Escape' });
    expect(sidebarBackdrop().className).not.toMatch(/sidebarBackdropOpen/);
  });

  it('first Escape closes only the Create Channel dialog; second Escape closes the sidebar', () => {
    render(<Chat />);

    openSidebar();
    expect(sidebarBackdrop().className).toMatch(/sidebarBackdropOpen/);

    fireEvent.click(screen.getByLabelText('New channel'));
    expect(screen.getByRole('dialog', { name: 'Create Channel' })).toBeInTheDocument();

    fireEvent.keyDown(screen.getByPlaceholderText('Channel name'), { key: 'Escape' });

    expect(screen.queryByRole('dialog', { name: 'Create Channel' })).not.toBeInTheDocument();
    expect(sidebarBackdrop().className).toMatch(/sidebarBackdropOpen/);

    fireEvent.keyDown(window, { key: 'Escape' });
    expect(sidebarBackdrop().className).not.toMatch(/sidebarBackdropOpen/);
  });
});
