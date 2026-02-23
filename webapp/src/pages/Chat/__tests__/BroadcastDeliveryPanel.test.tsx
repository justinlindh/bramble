import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import type { Message } from '../../../types/bramble';
import { MessageBubble } from '../MessageBubble';

vi.mock('../../../store/index', () => ({
  useStore: (selector: any) => selector({ showRoutes: false }),
}));

vi.mock('../../../hooks/usePeer', () => ({
  usePeerInfo: () => ({
    displayName: 'peer',
    fullHex: '0xPEER',
  }),
}));

function makeMessage(overrides: Partial<Message> = {}): Message {
  return {
    id: 'msg-1',
    direction: 'outgoing',
    from: 0x01,
    to: 0xFFFFFFFF,
    text: 'Broadcast hello',
    timestampMs: Date.now(),
    tier: 'broadcast',
    status: 'sent',
    ...overrides,
  };
}

describe('BroadcastDeliveryPanel', () => {
  it('toggles from message bubble', () => {
    render(<MessageBubble message={makeMessage()} myAddr={0x01} />);

    expect(screen.queryByText(/delivery telemetry/i)).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /show delivery details/i }));

    expect(screen.getByLabelText(/delivery telemetry panel/i)).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /hide delivery details/i }));

    expect(screen.queryByText(/delivery telemetry/i)).not.toBeInTheDocument();
  });

  it('shows recipient count and status chips', () => {
    render(
      <MessageBubble
        message={makeMessage({
          broadcastRecipients: [
            { addr: 0x1001, status: 'delivered', hopCount: 1, deliveredAtMs: Date.now() },
            { addr: 0x1002, status: 'failed', hopCount: 2, deliveredAtMs: Date.now() },
            { addr: 0x1003, status: 'delivered', hopCount: 1, deliveredAtMs: Date.now() },
          ],
        })}
        myAddr={0x01}
      />
    );

    fireEvent.click(screen.getByRole('button', { name: /show delivery details/i }));

    expect(screen.getByText(/3 recipients/i)).toBeInTheDocument();
    expect(screen.getByText(/delivered 2/i)).toBeInTheDocument();
    expect(screen.getByText(/failed 1/i)).toBeInTheDocument();
  });

  it('degrades gracefully with no telemetry', () => {
    render(<MessageBubble message={makeMessage({ broadcastRecipients: undefined })} myAddr={0x01} />);

    fireEvent.click(screen.getByRole('button', { name: /show delivery details/i }));

    expect(screen.getByText(/no delivery telemetry yet/i)).toBeInTheDocument();
  });
});
