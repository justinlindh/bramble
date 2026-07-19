// HeltecDevice.tsx
//
// One virtual Heltec WiFi LoRa 32 (V3/V4) rendered as a physical device: the
// SSD1306 128x64 OLED (Oled.tsx) mounted on a simple board face, three face
// buttons (up / down / select) that emit btn edges, and a RESET that is
// hold-to-confirm (the emulator treats reset as a clean process exit, which the
// supervisor restarts). A per-node console sits below the device.
//
// It is the OLED analogue of PagerDevice.tsx (e-paper). The button contract,
// testids (device-card-<node>, btn-<id>), and edge semantics are identical so
// the same e2e helpers (uiActions.ts clickButton) and the same broker btn
// wiring drive both device kinds. The panel geometry is carried per-frame on
// the wire (device.fbWidth / fbHeight), so this component never hardcodes the
// resolution beyond a sane default.

import { useCallback, useEffect, useRef, useState } from 'react';
import type { DeviceState } from '../types';
import Oled from './Oled';
import type { EdgeButtonId } from './PagerDevice';

export interface HeltecDeviceProps {
  device: DeviceState;
  onButton: (id: EdgeButtonId, edge: 'down' | 'up') => void;
  faceWidth?: number; // rendered face width in CSS px (default 300)
}

const RESET_HOLD_MS = 800;

// Board aspect: the Heltec dev board is a tall PCB with the OLED in the top
// third and a header/antenna area below. These proportions are cosmetic, only
// the OLED window needs to match the 2:1 panel aspect (128x64).
const FACE_ASPECT = 1.9; // height / width

const NAV_BUTTONS: { id: 'up' | 'down' | 'select'; label: string }[] = [
  { id: 'up', label: 'UP' },
  { id: 'select', label: 'OK' },
  { id: 'down', label: 'DN' },
];

export default function HeltecDevice({ device, onButton, faceWidth = 300 }: HeltecDeviceProps) {
  const faceHeight = faceWidth * FACE_ASPECT;
  const panelW = device.fbWidth || 128;
  const panelH = device.fbHeight || 64;

  // ---- face buttons: press/release edges, guarded against unpaired edges ----
  const [pressedBtn, setPressedBtn] = useState<EdgeButtonId | null>(null);
  const press = useCallback((id: EdgeButtonId) => {
    setPressedBtn(id);
    onButton(id, 'down');
  }, [onButton]);
  const release = useCallback((id: EdgeButtonId) => {
    setPressedBtn((cur) => {
      if (cur === id) onButton(id, 'up');
      return cur === id ? null : cur;
    });
  }, [onButton]);

  // ---- RESET hold-to-confirm ----
  const [resetProgress, setResetProgress] = useState(0); // 0..1
  const resetTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const resetStart = useRef(0);
  const clearReset = useCallback(() => {
    if (resetTimer.current) { clearInterval(resetTimer.current); resetTimer.current = null; }
    setResetProgress(0);
  }, []);
  const beginReset = useCallback(() => {
    if (resetTimer.current) { clearInterval(resetTimer.current); resetTimer.current = null; }
    resetStart.current = Date.now();
    resetTimer.current = setInterval(() => {
      const p = Math.min(1, (Date.now() - resetStart.current) / RESET_HOLD_MS);
      setResetProgress(p);
      if (p >= 1) {
        if (resetTimer.current) { clearInterval(resetTimer.current); resetTimer.current = null; }
        onButton('reset', 'down');
        onButton('reset', 'up');
        setTimeout(() => setResetProgress(0), 250);
      }
    }, 30);
  }, [onButton]);
  useEffect(() => () => { if (resetTimer.current) clearInterval(resetTimer.current); }, []);
  useEffect(() => {
    const up = () => clearReset();
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', up);
    return () => {
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', up);
    };
  }, [clearReset]);

  // OLED window: centered near the top, 2:1 aspect matching the panel.
  const oledCssW = faceWidth * 0.74;
  const oledCssH = (oledCssW * panelH) / panelW;

  return (
    <div className="heltec-device" data-testid={`device-card-${device.node}`} style={{ width: faceWidth }}>
      <div
        className="heltec-shell"
        style={{
          width: faceWidth,
          height: faceHeight,
          position: 'relative',
          background: 'linear-gradient(180deg, #1b3a2a 0%, #16301f 60%, #122619 100%)',
          borderRadius: 10,
          border: '1px solid #0b1a11',
          boxSizing: 'border-box',
          display: 'flex',
          flexDirection: 'column',
          alignItems: 'center',
          padding: faceWidth * 0.05,
          gap: faceWidth * 0.04,
        }}
      >
        {/* OLED module: black glass carrier with the lit panel inset. */}
        <div
          style={{
            marginTop: faceWidth * 0.04,
            padding: faceWidth * 0.03,
            background: '#05070a',
            borderRadius: 4,
            border: '1px solid #01030a',
          }}
        >
          <Oled
            fb={device.fb}
            seq={device.fbSeq}
            width={panelW}
            height={panelH}
            displayWidth={oledCssW}
            displayHeight={oledCssH}
          />
        </div>

        {/* Nav buttons row */}
        <div style={{ display: 'flex', gap: faceWidth * 0.05, marginTop: faceWidth * 0.03 }}>
          {NAV_BUTTONS.map((b) => (
            <div
              key={b.id}
              data-testid={`btn-${b.id}`}
              className="heltec-face-btn"
              role="button"
              tabIndex={0}
              aria-label={`${b.label} button`}
              style={{
                width: faceWidth * 0.16,
                height: faceWidth * 0.16,
                borderRadius: '50%',
                background: pressedBtn === b.id ? '#2a2e33' : '#3d444c',
                border: '1px solid #10130a',
                color: '#cfd4d9',
                fontSize: faceWidth * 0.05,
                fontFamily: 'sans-serif',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                cursor: 'pointer',
                touchAction: 'none',
                userSelect: 'none',
              }}
              onPointerDown={() => press(b.id)}
              onPointerUp={() => release(b.id)}
              onPointerLeave={() => release(b.id)}
              onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); press(b.id); } }}
              onKeyUp={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); release(b.id); } }}
            >
              {b.label}
            </div>
          ))}
        </div>

        {/* RESET pinhole, hold-to-confirm */}
        <div
          data-testid="btn-reset"
          className="heltec-face-btn"
          role="button"
          tabIndex={0}
          aria-label="reset (hold 0.8s)"
          style={{
            marginTop: faceWidth * 0.02,
            width: faceWidth * 0.12,
            height: faceWidth * 0.12,
            borderRadius: '50%',
            background: resetProgress > 0
              ? `conic-gradient(#ff5a4d ${resetProgress * 360}deg, #2a2d31 0deg)`
              : '#2a2d31',
            border: '1px solid #10130a',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            color: '#8a9099',
            fontSize: faceWidth * 0.035,
            fontFamily: 'sans-serif',
            cursor: 'pointer',
            touchAction: 'none',
            userSelect: 'none',
          }}
          onPointerDown={beginReset}
          onPointerUp={clearReset}
        >
          RST
        </div>
      </div>

      <div className="pager-caption">
        <span className="pager-name">{device.node}</span>
        {device.addr && <span className="pager-addr">{device.addr}</span>}
        <span className={`pager-badge${device.led ? ' on' : ''}`}>led</span>
      </div>

      <pre className="pager-console" data-testid={`console-${device.node}`}>
        {device.console.length === 0 ? '(no console output yet)' : device.console.slice(-40).join('\n')}
      </pre>
    </div>
  );
}
