// PagerDevice.tsx
//
// One virtual pager rendered as the physical device: an SVG face whose geometry
// is derived from the case model (pagerFace.ts), the modeled e-paper panel over
// the display window (Epaper.tsx), clickable/keyboard face buttons that emit btn
// edges, a corner RESET that is hold-to-confirm, an LED glow on the aperture, a
// vibra shake, and a buzzer tone (muted by default). A per-node console sits
// below the device.

import { useEffect, useRef, useState, useCallback } from 'react';
import type { DeviceState } from '../types';
import Epaper from './Epaper';
import {
  EXT_W, EXT_H, R_OUT, BODY, WINDOW, ACTIVE, BUTTONS, LED, RESET, BUZZER, USB,
  type ButtonId,
} from './pagerFace';

export type EdgeButtonId = ButtonId | 'reset';

export interface PagerDeviceProps {
  device: DeviceState;
  muted: boolean;
  onButton: (id: EdgeButtonId, edge: 'down' | 'up') => void;
  faceWidth?: number; // rendered face width in CSS px (default 300)
}

const RESET_HOLD_MS = 800;

export default function PagerDevice({ device, muted, onButton, faceWidth = 300 }: PagerDeviceProps) {
  const faceHeight = (faceWidth * EXT_H) / EXT_W;

  // ---- vibra shake, retriggered on each vibra pulse ----
  const [shaking, setShaking] = useState(false);
  useEffect(() => {
    if (device.vibraSeq <= 0) return;
    setShaking(true);
    const t = setTimeout(() => setShaking(false), 450);
    return () => clearTimeout(t);
  }, [device.vibraSeq]);

  // ---- buzzer (Web Audio), muted by default ----
  const audioRef = useRef<{ ctx: AudioContext; osc: OscillatorNode; gain: GainNode } | null>(null);
  useEffect(() => {
    const active = !muted && device.buzzerHz > 0;
    if (!active) {
      if (audioRef.current) {
        try { audioRef.current.osc.stop(); audioRef.current.ctx.close(); } catch { /* noop */ }
        audioRef.current = null;
      }
      return;
    }
    const Ctor = window.AudioContext || (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
    if (!Ctor) return;
    if (!audioRef.current) {
      const ctx = new Ctor();
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.type = 'square';
      gain.gain.value = 0.05;
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.start();
      audioRef.current = { ctx, osc, gain };
    }
    audioRef.current.osc.frequency.value = device.buzzerHz;
    return () => { /* stop handled on next run / unmount */ };
  }, [muted, device.buzzerHz]);
  useEffect(() => () => {
    if (audioRef.current) {
      try { audioRef.current.osc.stop(); audioRef.current.ctx.close(); } catch { /* noop */ }
      audioRef.current = null;
    }
  }, []);

  // ---- face buttons ----
  const [pressedBtn, setPressedBtn] = useState<EdgeButtonId | null>(null);
  const press = useCallback((id: EdgeButtonId) => {
    setPressedBtn(id);
    onButton(id, 'down');
  }, [onButton]);
  const release = useCallback((id: EdgeButtonId) => {
    // Guarded: only emit the up edge for a button that is actually down, so a
    // stray mouseleave or key auto-repeat cannot send unpaired edges.
    setPressedBtn((cur) => {
      if (cur === id) onButton(id, 'up');
      return cur === id ? null : cur;
    });
  }, [onButton]);

  // ---- RESET hold-to-confirm ----
  const [resetProgress, setResetProgress] = useState(0); // 0..1
  const resetTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const resetStart = useRef(0);
  const [resetHint, setResetHint] = useState(false);
  const clearReset = useCallback(() => {
    if (resetTimer.current) { clearInterval(resetTimer.current); resetTimer.current = null; }
    // A released tap that never completed shows a brief "hold" hint so the
    // pinhole does not read as dead.
    setResetProgress((p) => {
      if (p > 0 && p < 1) {
        setResetHint(true);
        setTimeout(() => setResetHint(false), 1200);
      }
      return 0;
    });
  }, []);
  const beginReset = useCallback(() => {
    setResetHint(false);
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
    // Release anywhere ends a reset hold (replaces the old mouseleave cancel,
    // which aborted the hold on a one-pixel drift off the tiny pinhole).
    const up = () => clearReset();
    window.addEventListener('mouseup', up);
    return () => window.removeEventListener('mouseup', up);
  }, [clearReset]);

  return (
    <div className="pager-device" data-testid={`device-card-${device.node}`} style={{ width: faceWidth }}>
      <div className={`pager-shell${shaking ? ' pager-shake' : ''}`} style={{ width: faceWidth, height: faceHeight, position: 'relative' }}>
        <svg
          viewBox={`0 0 ${EXT_W} ${EXT_H}`}
          width={faceWidth}
          height={faceHeight}
          style={{ display: 'block' }}
          role="img"
          aria-label={`pager ${device.node}`}
        >
          <defs>
            <linearGradient id={`body-${device.node}`} x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stopColor="#3a3f45" />
              <stop offset="0.5" stopColor="#2b2f34" />
              <stop offset="1" stopColor="#212429" />
            </linearGradient>
            <radialGradient id={`led-${device.node}`} cx="0.5" cy="0.5" r="0.5">
              <stop offset="0" stopColor="#7CFC66" stopOpacity="0.95" />
              <stop offset="1" stopColor="#7CFC66" stopOpacity="0" />
            </radialGradient>
          </defs>

          {/* USB-C notch on the bottom edge */}
          <rect x={USB.x} y={USB.y} width={USB.w} height={USB.h} rx={0.8} fill="#15171a" />

          {/* Body shell */}
          <rect
            x={BODY.x} y={BODY.y} width={BODY.w} height={BODY.h}
            rx={R_OUT} ry={R_OUT}
            fill={`url(#body-${device.node})`}
            stroke="#0c0d0f" strokeWidth={0.6}
          />

          {/* Display window recess + glass reveal */}
          <rect
            x={WINDOW.x - 0.6} y={WINDOW.y - 0.6} width={WINDOW.w + 1.2} height={WINDOW.h + 1.2}
            rx={1.2} fill="#0a0b0c"
          />
          <rect x={WINDOW.x} y={WINDOW.y} width={WINDOW.w} height={WINDOW.h} rx={0.8} fill="#c9c8be" />

          {/* LED aperture */}
          {device.led && (
            <circle cx={LED.x} cy={LED.y} r={LED.r * 3.5} fill={`url(#led-${device.node})`} />
          )}
          <circle cx={LED.x} cy={LED.y} r={LED.r}
            fill={device.led ? '#8CFF74' : '#243026'} stroke="#0c0d0f" strokeWidth={0.2} />

          {/* Buzzer sound port (3x3 grille) */}
          {[-1, 0, 1].map((gx) => [-1, 0, 1].map((gy) => (
            <circle key={`bz-${gx}-${gy}`} cx={BUZZER.x + gx * 1.1} cy={BUZZER.y + gy * 1.1} r={BUZZER.r} fill="#15171a" />
          )))}

          {/* Face buttons */}
          {BUTTONS.map((b) => (
            <g
              key={b.id}
              data-testid={`btn-${b.id}`}
              className="pager-face-btn"
              role="button"
              tabIndex={0}
              aria-label={`${b.label} button`}
              style={{ cursor: 'pointer' }}
              onMouseDown={() => press(b.id)}
              onMouseUp={() => release(b.id)}
              onMouseLeave={() => release(b.id)}
              onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); press(b.id); } }}
              onKeyUp={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); release(b.id); } }}
            >
              <circle cx={b.center.x} cy={b.center.y} r={b.r + 1.4} fill="#191b1e" stroke="#0c0d0f" strokeWidth={0.3} />
              {/* Cap: sinks and darkens while pressed (the depressed look). */}
              <circle
                cx={b.center.x}
                cy={b.center.y + (pressedBtn === b.id ? 0.35 : 0)}
                r={pressedBtn === b.id ? b.r - 0.25 : b.r}
                fill={pressedBtn === b.id ? '#33383e' : '#4a5058'}
                stroke={pressedBtn === b.id ? '#464c54' : '#5c636c'}
                strokeWidth={0.3}
              />
              <text x={b.center.x} y={b.center.y + b.r + 3.2} textAnchor="middle" fontSize={2.6} fill="#c9ccd1" fontFamily="sans-serif">{b.label}</text>
            </g>
          ))}

          {/* RESET pinhole, hold-to-confirm */}
          <g
            data-testid="btn-reset"
            className="pager-face-btn"
            role="button"
            tabIndex={0}
            aria-label="reset (hold 0.8s)"
            style={{ cursor: 'pointer' }}
            onMouseDown={beginReset}
            onMouseUp={clearReset}
          >
            {/* Generous invisible hit area; the visible pinhole is tiny and a
                one-pixel drift must not cancel the hold (no mouseleave cancel:
                release anywhere ends it via the window listener below). */}
            <circle cx={RESET.x} cy={RESET.y} r={5.5} fill="transparent" />
            <circle cx={RESET.x} cy={RESET.y} r={2.4} fill="#151719" stroke="#0c0d0f" strokeWidth={0.3} />
            <circle cx={RESET.x} cy={RESET.y} r={RESET.r} fill="#2a2d31" />
            {resetProgress > 0 && (
              <circle
                cx={RESET.x} cy={RESET.y} r={2.0}
                fill="none" stroke="#ff5a4d" strokeWidth={0.7}
                strokeDasharray={`${resetProgress * 2 * Math.PI * 2.0} ${2 * Math.PI * 2.0}`}
                transform={`rotate(-90 ${RESET.x} ${RESET.y})`}
              />
            )}
            <text x={RESET.x} y={RESET.y - 3.4} textAnchor="middle" fontSize={2.2} fill="#8a9099" fontFamily="sans-serif">RST</text>
            {resetHint && (
              <text x={RESET.x} y={RESET.y + 6.2} textAnchor="middle" fontSize={2.2} fill="#e0a030" fontFamily="sans-serif">hold to reset</text>
            )}
          </g>
        </svg>

        {/* E-paper panel, positioned over the active-pixel area */}
        <div
          className="epaper-mount"
          style={{
            position: 'absolute',
            left: `${(ACTIVE.x / EXT_W) * 100}%`,
            top: `${(ACTIVE.y / EXT_H) * 100}%`,
            width: `${(ACTIVE.w / EXT_W) * 100}%`,
            height: `${(ACTIVE.h / EXT_H) * 100}%`,
          }}
        >
          <Epaper
            fb={device.fb}
            kind={device.fbKind}
            busyMs={device.fbBusyMs}
            seq={device.fbSeq}
            displayWidth={(ACTIVE.w / EXT_W) * faceWidth}
            displayHeight={(ACTIVE.h / EXT_H) * faceHeight}
          />
        </div>
      </div>

      <div className="pager-caption">
        <span className="pager-name">{device.node}</span>
        {device.addr && <span className="pager-addr">{device.addr}</span>}
        <span className={`pager-badge${device.buzzerHz > 0 ? ' on' : ''}`}>{device.buzzerHz > 0 ? `♪ ${device.buzzerHz}Hz` : '♪'}</span>
        <span className={`pager-badge${device.vibra ? ' on' : ''}`}>vibra</span>
        <span className={`pager-badge${device.led ? ' on' : ''}`}>led</span>
      </div>

      <pre className="pager-console" data-testid={`console-${device.node}`}>
        {device.console.length === 0 ? '(no console output yet)' : device.console.slice(-40).join('\n')}
      </pre>
    </div>
  );
}
