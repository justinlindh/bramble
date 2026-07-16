import { useEffect, useRef, useState, useCallback } from 'react';
import { isBrambleShare, parseChannelShare, parseNodeShare } from '../utils/channelShare';
import type { ChannelShareData, NodeShareData } from '../utils/channelShare';
import { parseNetworkKeyShare } from '../utils/networkKeyShare';
import { friendlyErrorFrom } from '../lib/errors';
import { EscapeDialog } from './EscapeDialog';
import styles from './QRScanModal.module.css';

export type ScanResult =
  | { kind: 'channel'; data: ChannelShareData }
  | { kind: 'node'; data: NodeShareData }
  | { kind: 'network'; data: { key: string } };

interface QRScanModalProps {
  onResult: (result: ScanResult) => void;
  onClose: () => void;
  title?: string;
}

// BarcodeDetector is a Web API available in Chrome 88+ / Android Chrome
declare class BarcodeDetector {
  static getSupportedFormats(): Promise<string[]>;
  constructor(opts?: { formats: string[] });
  detect(source: HTMLVideoElement | ImageBitmap | HTMLCanvasElement): Promise<Array<{ rawValue: string }>>;
}

const hasBarcodeDetector =
  typeof window !== 'undefined' && 'BarcodeDetector' in window;

// getUserMedia failures are DOMExceptions whose `name` identifies the
// failure mode. friendlyErrorFrom's ERROR_MAP is connection-flavored (BLE /
// serial / WiFi transport errors), so a camera NotFoundError would render as
// "No device found. Make sure your node is powered on and in range." which
// is the wrong domain entirely. Map the camera-specific names here, locally,
// instead of polluting the shared connection ERROR_MAP; anything else falls
// through to the shared copy.
function friendlyCameraError(e: unknown): string {
  const name = e instanceof DOMException ? e.name : (e as { name?: unknown } | null)?.name;
  switch (name) {
    case 'NotAllowedError':
      return 'Camera access was denied. Allow camera access for this site in your browser settings, then try again.';
    case 'NotFoundError':
    case 'OverconstrainedError':
      return 'No camera found on this device.';
    case 'NotReadableError':
      return 'Could not access the camera. It may be in use by another app.';
    default:
      return friendlyErrorFrom(e);
  }
}

export function QRScanModal({ onResult, onClose, title = 'Import Channel' }: QRScanModalProps) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const rafRef = useRef<number>(0);
  const detectorRef = useRef<BarcodeDetector | null>(null);

  const [mode, setMode] = useState<'camera' | 'text'>(hasBarcodeDetector ? 'camera' : 'text');
  const [cameraError, setCameraError] = useState('');
  const [textInput, setTextInput] = useState('');
  const [parseError, setParseError] = useState('');
  const [scanning, setScanning] = useState(false);

  // ── Parse and emit a share string ────────────────────────────────────────
  const handleString = useCallback((s: string) => {
    const trimmed = s.trim();
    if (!isBrambleShare(trimmed)) {
      setParseError('Not a valid Bramble share string.');
      return;
    }
    if (trimmed.startsWith('bramble://ch/')) {
      const result = parseChannelShare(trimmed);
      if (!result.ok) { setParseError(result.error); return; }
      onResult({ kind: 'channel', data: result.data });
    } else if (trimmed.startsWith('bramble://net/')) {
      const result = parseNetworkKeyShare(trimmed);
      if (!result.ok) { setParseError(result.error); return; }
      onResult({ kind: 'network', data: result.data });
    } else {
      const result = parseNodeShare(trimmed);
      if (!result.ok) { setParseError(result.error); return; }
      onResult({ kind: 'node', data: result.data });
    }
  }, [onResult]);

  // ── Camera scanning loop ─────────────────────────────────────────────────
  useEffect(() => {
    if (mode !== 'camera') return;

    let stopped = false;

    const start = async () => {
      try {
        const stream = await navigator.mediaDevices.getUserMedia({
          video: { facingMode: 'environment' },
        });
        if (stopped) { stream.getTracks().forEach(t => t.stop()); return; }
        streamRef.current = stream;
        if (videoRef.current) {
          videoRef.current.srcObject = stream;
          await videoRef.current.play();
        }
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        detectorRef.current = new (window as any).BarcodeDetector({ formats: ['qr_code'] });
        setScanning(true);

        const scan = async () => {
          if (stopped || !videoRef.current || !detectorRef.current) return;
          try {
            const results = await detectorRef.current.detect(videoRef.current);
            for (const r of results) {
              if (isBrambleShare(r.rawValue)) {
                stopped = true;
                handleString(r.rawValue);
                return;
              }
            }
          } catch { /* ignore decode errors */ }
          rafRef.current = requestAnimationFrame(scan);
        };
        rafRef.current = requestAnimationFrame(scan);
      } catch (e) {
        // Stay in camera mode so the error actually renders (it's shown by
        // the mode === 'camera' branch below, in place of the video); the
        // user can still tap "Paste String" to fall back manually. Forcing
        // mode to 'text' here used to hide the error the same render it was
        // set, since that branch is gated on mode === 'camera'.
        setCameraError(friendlyCameraError(e));
      }
    };

    start();

    return () => {
      stopped = true;
      cancelAnimationFrame(rafRef.current);
      streamRef.current?.getTracks().forEach(t => t.stop());
      streamRef.current = null;
    };
  }, [mode, handleString]);

  const handleTextSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    setParseError('');
    handleString(textInput);
  };

  return (
    <EscapeDialog
      ariaLabel={title}
      onClose={onClose}
      backdropClassName={styles.backdrop}
      dialogClassName={styles.modal}
    >
      <button className={styles.closeBtn} onClick={onClose} aria-label="Close">✕</button>
      <h3 className={styles.title}>{title}</h3>

      {/* Tab switcher */}
      <div className={styles.tabs}>
        {hasBarcodeDetector && (
          <button
            className={`${styles.tab} ${mode === 'camera' ? styles.tabActive : ''}`}
            onClick={() => { setParseError(''); setMode('camera'); }}
          >
            📷 Scan QR
          </button>
        )}
        <button
          className={`${styles.tab} ${mode === 'text' ? styles.tabActive : ''}`}
          onClick={() => { setParseError(''); setMode('text'); }}
        >
          ✏️ Paste String
        </button>
      </div>

      {/* Camera view */}
      {mode === 'camera' && (
        <div className={styles.cameraWrap}>
          {cameraError ? (
            <p className={styles.cameraError}>{cameraError}</p>
          ) : (
            <>
              <video
                ref={videoRef}
                className={styles.video}
                playsInline
                muted
                aria-label="Camera preview"
              />
              {scanning && (
                <div className={styles.scanOverlay} aria-hidden>
                  <div className={styles.scanCorner} />
                </div>
              )}
              <p className={styles.scanHint}>Point at a Bramble QR code</p>
            </>
          )}
        </div>
      )}

      {/* Text paste view */}
      {mode === 'text' && (
        <form className={styles.textForm} onSubmit={handleTextSubmit}>
          <textarea
            className={styles.textArea}
            placeholder="Paste a Bramble share string here…&#10;(bramble://ch/v1?...)"
            value={textInput}
            onChange={(e) => { setTextInput(e.target.value); setParseError(''); }}
            rows={4}
            autoFocus
            aria-label="Share string input"
            spellCheck={false}
          />
          {parseError && <span className={styles.error}>{parseError}</span>}
          <button
            className={styles.importBtn}
            type="submit"
            disabled={!textInput.trim()}
          >
            Import
          </button>
        </form>
      )}

      {mode === 'camera' && parseError && (
        <span className={styles.error}>{parseError}</span>
      )}
    </EscapeDialog>
  );
}
