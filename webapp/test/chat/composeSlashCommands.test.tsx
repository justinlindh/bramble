import { describe, it, expect, beforeEach, vi } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';

const { sendMessageMock, shareLocationOnceMock } = vi.hoisted(() => ({
  sendMessageMock: vi.fn().mockResolvedValue(undefined),
  shareLocationOnceMock: vi.fn().mockResolvedValue(undefined),
}));

vi.mock('../../src/store/actions', () => ({
  sendMessage: sendMessageMock,
  shareLocationOnce: shareLocationOnceMock,
}));

import { ComposeBar } from '../../src/pages/Chat/ComposeBar';
import { useStore } from '../../src/store/index';

describe('ComposeBar slash command handling', () => {
  beforeEach(() => {
    sendMessageMock.mockClear();
    useStore.setState({ connectionState: 'connected', config: { location: { enabled: false } } as any });
  });

  it('encodes /me as CTCP ACTION', async () => {
    render(<ComposeBar conversationId="broadcast" />);

    fireEvent.change(screen.getByLabelText('Message input'), {
      target: { value: '/me waves' },
    });
    fireEvent.click(screen.getByLabelText('Send message'));

    await waitFor(() => expect(sendMessageMock).toHaveBeenCalledTimes(1));
    expect(sendMessageMock).toHaveBeenCalledWith(
      0xffffffff,
      '\x01ACTION waves\x01',
      'broadcast',
      undefined,
    );
  });

  it('encodes /slap <nick> as trout CTCP ACTION', async () => {
    render(<ComposeBar conversationId="broadcast" />);

    fireEvent.change(screen.getByLabelText('Message input'), {
      target: { value: '/slap NodeName' },
    });
    fireEvent.click(screen.getByLabelText('Send message'));

    await waitFor(() => expect(sendMessageMock).toHaveBeenCalledTimes(1));
    expect(sendMessageMock).toHaveBeenCalledWith(
      0xffffffff,
      '\x01ACTION slaps NodeName around a bit with a large trout\x01',
      'broadcast',
      undefined,
    );
  });
});
