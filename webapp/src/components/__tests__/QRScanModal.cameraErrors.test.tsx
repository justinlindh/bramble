import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { cleanup, render, screen, waitFor } from '@testing-library/react';

// hasBarcodeDetector is computed once, at module load, from
// `'BarcodeDetector' in window`. jsdom has no BarcodeDetector, so the
// BarcodeDetector shim below MUST be installed before QRScanModal is
// imported, or the module falls back to text-paste mode and never calls
// getUserMedia at all. A dynamic import inside beforeAll (after the shim is
// installed) is what makes that ordering hold; a static top-level import
// would be hoisted ahead of the shim.
class MockBarcodeDetector {
  static getSupportedFormats() { return Promise.resolve(['qr_code']); }
  detect() { return Promise.resolve([]); }
}

let QRScanModal: typeof import('../QRScanModal').QRScanModal;

beforeAll(async () => {
  (window as unknown as { BarcodeDetector: unknown }).BarcodeDetector = MockBarcodeDetector;
  ({ QRScanModal } = await import('../QRScanModal'));
});

afterEach(cleanup);

function stubGetUserMedia(rejection: unknown) {
  Object.defineProperty(navigator, 'mediaDevices', {
    configurable: true,
    value: { getUserMedia: vi.fn().mockRejectedValue(rejection) },
  });
}

describe('QRScanModal camera-domain error copy', () => {
  it('maps NotAllowedError to camera permission copy, not the connection ERROR_MAP', async () => {
    stubGetUserMedia(new DOMException('Permission denied', 'NotAllowedError'));
    render(<QRScanModal onResult={vi.fn()} onClose={vi.fn()} title="Import Channel" />);

    const error = await screen.findByText(/allow camera access/i);
    expect(error.textContent).toMatch(/camera access was denied/i);
    // Must NOT fall through to the connection-flavored copy.
    expect(error.textContent).not.toMatch(/node is powered on and in range/i);
  });

  it('maps NotFoundError to no-camera copy, not "No device found... node"', async () => {
    stubGetUserMedia(new DOMException('No camera', 'NotFoundError'));
    render(<QRScanModal onResult={vi.fn()} onClose={vi.fn()} title="Import Channel" />);

    const error = await screen.findByText(/no camera found/i);
    expect(error.textContent).not.toMatch(/node is powered on and in range/i);
  });

  it('maps OverconstrainedError to no-camera copy', async () => {
    stubGetUserMedia(new DOMException('Constraints not satisfiable', 'OverconstrainedError'));
    render(<QRScanModal onResult={vi.fn()} onClose={vi.fn()} title="Import Channel" />);

    await waitFor(() => expect(screen.getByText(/no camera found/i)).toBeInTheDocument());
  });

  it('maps NotReadableError to camera-busy copy', async () => {
    stubGetUserMedia(new DOMException('Device in use', 'NotReadableError'));
    render(<QRScanModal onResult={vi.fn()} onClose={vi.fn()} title="Import Channel" />);

    await waitFor(() => expect(screen.getByText(/in use by another app/i)).toBeInTheDocument());
  });

  it('falls through to friendlyErrorFrom for other failures', async () => {
    stubGetUserMedia(new Error('handshake timed out'));
    render(<QRScanModal onResult={vi.fn()} onClose={vi.fn()} title="Import Channel" />);

    await waitFor(() => expect(screen.getByText(/did not respond over Bluetooth/i)).toBeInTheDocument());
  });
});
