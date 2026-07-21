import { describe, it, expect, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';

let state: any;
let conversationState: { messages: any[] };

vi.mock('../../store/index', () => ({
  useStore: (selector: any) => selector(state),
}));

vi.mock('../../store/selectors', () => ({
  useConversation: () => ({ messages: conversationState.messages }),
  useMyAddress: () => 0,
}));

vi.mock('../../store/actions', () => ({
  loadPeerVerification: vi.fn(),
  setPeerVerified: vi.fn(),
}));

vi.mock('./ConversationList', () => ({
  ConversationList: () => <div data-testid="conversation-list" />,
}));

vi.mock('./ComposeBar', () => ({
  ComposeBar: () => <div data-testid="compose-bar" />,
}));

import { Chat } from './Chat';

describe('Chat message-list loading vs empty state', () => {
  beforeEach(() => {
    conversationState = { messages: [] };
    state = {
      conversations: new globalThis.Map(),
      activeConversationId: 'broadcast',
      setActiveConversation: vi.fn(),
      config: null,
      showRoutes: false,
      setShowRoutes: vi.fn(),
      connectionState: 'connected',
      status: null,
      peerVerifications: new globalThis.Map(),
      neighbors: [],
      routes: [],
      peerNames: new globalThis.Map(),
      peerLocations: [],
    };
  });

  it('renders a loading affordance when connected but initial status has not arrived yet', () => {
    render(<Chat />);
    expect(screen.getByText('Loading messages…')).toBeInTheDocument();
  });

  it('renders the normal empty-conversation hint once status has loaded and there truly are no messages', () => {
    state.status = { uptimeSec: 10, gpsAvailable: false };
    render(<Chat />);
    expect(screen.queryByText('Loading messages…')).not.toBeInTheDocument();
    expect(screen.getByText(/Broadcast messages will appear here/)).toBeInTheDocument();
  });

  it('renders neither loading nor empty hint when messages already exist', () => {
    conversationState.messages = [
      { id: '1', direction: 'incoming', from: 0x1234, to: 0xffffffff, text: 'hi', timestampMs: Date.now(), status: 'received' },
    ];
    render(<Chat />);
    expect(screen.queryByText('Loading messages…')).not.toBeInTheDocument();
    expect(screen.queryByText(/Broadcast messages will appear here/)).not.toBeInTheDocument();
  });
});

describe('Chat sidebar backdrop a11y', () => {
  beforeEach(() => {
    conversationState = { messages: [] };
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
    };
  });

  it('marks the sidebar backdrop as presentational, not an interactive control', () => {
    const { container } = render(<Chat />);
    const backdrop = container.querySelector('[role="presentation"]');
    expect(backdrop).toBeTruthy();
    expect(backdrop).not.toHaveAttribute('role', 'button');
  });

  it('closes the sidebar on Escape while it is open', () => {
    render(<Chat />);
    const openBtn = screen.getByLabelText('Open conversations');
    fireEvent.click(openBtn);

    const backdrop = document.querySelector('[role="presentation"]') as HTMLElement;
    expect(backdrop.className).toMatch(/sidebarBackdropOpen/);

    fireEvent.keyDown(window, { key: 'Escape' });

    expect(backdrop.className).not.toMatch(/sidebarBackdropOpen/);
  });
});
