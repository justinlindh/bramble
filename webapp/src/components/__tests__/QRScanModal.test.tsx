import { describe, it, expect, vi, afterEach } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { QRScanModal } from '../QRScanModal';

// jsdom has no BarcodeDetector, so the modal always mounts in 'text' mode
// here: these tests exercise the shared EscapeDialog wiring, not the camera
// scanning loop (that needs the BarcodeDetector shim, see the "camera error
// mapping" describe block added for the QRScanModal error-copy fix).

afterEach(cleanup);

describe('QRScanModal dialog dismissal', () => {
  it('exposes dialog role/aria-modal and closes on Escape', () => {
    const onClose = vi.fn();
    render(<QRScanModal onResult={vi.fn()} onClose={onClose} title="Import Channel" />);

    const dialog = screen.getByRole('dialog', { name: 'Import Channel' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(dialog, { key: 'Escape' });
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  it('still closes on backdrop click and via the close button', () => {
    const onCloseBackdrop = vi.fn();
    render(<QRScanModal onResult={vi.fn()} onClose={onCloseBackdrop} title="Import Channel" />);
    fireEvent.click(screen.getByRole('dialog', { name: 'Import Channel' }));
    expect(onCloseBackdrop).toHaveBeenCalledTimes(1);

    cleanup();

    const onCloseBtn = vi.fn();
    render(<QRScanModal onResult={vi.fn()} onClose={onCloseBtn} title="Import Channel" />);
    fireEvent.click(screen.getByLabelText('Close'));
    expect(onCloseBtn).toHaveBeenCalledTimes(1);
  });

  it('does not close when clicking inside the dialog panel', () => {
    const onClose = vi.fn();
    render(<QRScanModal onResult={vi.fn()} onClose={onClose} title="Import Channel" />);
    fireEvent.click(screen.getByText('Import Channel'));
    expect(onClose).not.toHaveBeenCalled();
  });
});
