import { beforeEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import { ChannelManager } from '../../src/pages/Config/ChannelManager';
import { IdentitySection } from '../../src/pages/Config/IdentitySection';
import {
  utf8Length,
  NODE_NAME_MAX_BYTES,
  CHANNEL_NAME_BUDGET_BYTES,
} from '../../src/utils/byteLimit';

vi.mock('../../src/store/actions', () => ({
  addChannel: vi.fn(),
  removeChannel: vi.fn(),
  setDefaultChannel: vi.fn(),
  saveNodeName: vi.fn(),
  setMailbox: vi.fn(),
}));

vi.mock('../../src/components/QRShareModal', () => ({
  QRShareModal: () => null,
}));

vi.mock('../../src/components/QRScanModal', () => ({
  QRScanModal: () => null,
}));

/*
 * The node measures these fields with strlen, so its limits are byte counts.
 * An <input maxLength> counts UTF-16 code units, which matches only for
 * ASCII, so a name of emoji or CJK used to sail past the client and come back
 * as a bare -32602 from bramble.setNodeName or bramble.addChannel.
 */
describe('name inputs hold to the byte budget the node enforces', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('stops a node name at 32 bytes, not 32 characters', () => {
    render(<IdentitySection identity={{ address: 0x11223344, name: '' } as any} />);
    const input = screen.getByLabelText('Node name') as HTMLInputElement;

    fireEvent.change(input, { target: { value: '🌲'.repeat(32) } });

    expect(utf8Length(input.value)).toBeLessThanOrEqual(NODE_NAME_MAX_BYTES);
    expect(Array.from(input.value).length).toBe(8); // 8 trees at 4 bytes each
    expect(input.value).not.toContain('�');
  });

  it('stops a channel name at the byte budget, not the character count', () => {
    render(<ChannelManager channels={[] as any} />);
    const input = screen.getByLabelText('New channel name') as HTMLInputElement;

    fireEvent.change(input, { target: { value: '日本語チャンネル名' } });

    expect(utf8Length(input.value)).toBeLessThanOrEqual(CHANNEL_NAME_BUDGET_BYTES);
    expect(input.value).toBe('日本語チャ');
  });

  it('counts the channel name in bytes so the counter cannot read as room left', () => {
    render(<ChannelManager channels={[] as any} />);
    const input = screen.getByLabelText('New channel name') as HTMLInputElement;

    fireEvent.change(input, { target: { value: 'héllo' } });

    // Five characters, six bytes: the counter has to show the six.
    expect(screen.getByText(`6/${CHANNEL_NAME_BUDGET_BYTES}`)).toBeTruthy();
  });

  it('still accepts a plain ASCII name up to the full budget', () => {
    render(<IdentitySection identity={{ address: 0x11223344, name: '' } as any} />);
    const input = screen.getByLabelText('Node name') as HTMLInputElement;

    const ascii = 'a'.repeat(NODE_NAME_MAX_BYTES);
    fireEvent.change(input, { target: { value: ascii } });

    expect(input.value).toBe(ascii);
  });
});
