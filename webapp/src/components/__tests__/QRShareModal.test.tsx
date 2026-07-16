import { describe, it, expect, vi, afterEach } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { QRShareModal } from '../QRShareModal';

afterEach(cleanup);

describe('QRShareModal dialog dismissal', () => {
  it('exposes dialog role/aria-modal and closes on Escape', () => {
    const onClose = vi.fn();
    render(<QRShareModal title="Share channel" shareString="bramble://ch/v1?x" onClose={onClose} />);

    const dialog = screen.getByRole('dialog', { name: 'Share channel' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(dialog, { key: 'Escape' });
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  it('still closes on backdrop click and via the close button', () => {
    const onCloseBackdrop = vi.fn();
    render(<QRShareModal title="Share channel" shareString="bramble://ch/v1?x" onClose={onCloseBackdrop} />);
    fireEvent.click(screen.getByRole('dialog', { name: 'Share channel' }));
    expect(onCloseBackdrop).toHaveBeenCalledTimes(1);

    cleanup();

    const onCloseBtn = vi.fn();
    render(<QRShareModal title="Share channel" shareString="bramble://ch/v1?x" onClose={onCloseBtn} />);
    fireEvent.click(screen.getByLabelText('Close'));
    expect(onCloseBtn).toHaveBeenCalledTimes(1);
  });

  it('does not close when clicking inside the dialog panel', () => {
    const onClose = vi.fn();
    render(<QRShareModal title="Share channel" shareString="bramble://ch/v1?x" onClose={onClose} />);
    fireEvent.click(screen.getByText('Share channel'));
    expect(onClose).not.toHaveBeenCalled();
  });
});
