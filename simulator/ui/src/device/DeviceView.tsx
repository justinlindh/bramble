// DeviceView.tsx
//
// The device tab: one PagerDevice per firmware node, a global mute toggle for
// the buzzer, and a per-node selector. Additive to the mesh map (App keeps both
// views); a firmware node appears here as soon as it streams its first frame or
// indicator.

import { useEffect, useMemo, useRef, useState } from 'react';
import type { DeviceState } from '../types';
import PagerDevice, { type EdgeButtonId } from './PagerDevice';
import HeltecDevice from './HeltecDevice';

export interface DeviceViewProps {
  devices: Map<string, DeviceState>;
  onButton: (node: string, id: EdgeButtonId, edge: 'down' | 'up') => void;
}

const FACE_MIN = 280; // the old fixed-size feel, kept as the floor (mobile)
const FACE_MAX = 560; // cap so a lone device does not become a billboard
const GRID_GAP = 24; // keep in sync with .device-grid gap in App.css

/* Face width that uses the container: split the available row width across
 * the devices (as many columns as fit at >= FACE_MIN), clamped to
 * [FACE_MIN, FACE_MAX]. Phones land at the old 280-300px feel; desktops
 * grow the pagers to fill the space. */
function computeFaceWidth(containerWidth: number, count: number): number {
  if (containerWidth <= 0 || count === 0) return 300;
  const cols = Math.max(
    1,
    Math.min(count, Math.floor((containerWidth + GRID_GAP) / (FACE_MIN + GRID_GAP))),
  );
  const per = Math.floor((containerWidth - (cols - 1) * GRID_GAP) / cols);
  return Math.max(FACE_MIN, Math.min(FACE_MAX, per));
}

export default function DeviceView({ devices, onButton }: DeviceViewProps) {
  const [muted, setMuted] = useState(true); // buzzer muted by default
  const gridRef = useRef<HTMLDivElement | null>(null);
  const [containerWidth, setContainerWidth] = useState(0);

  useEffect(() => {
    const el = gridRef.current;
    if (!el) return;
    const ro = new ResizeObserver((entries) => {
      for (const en of entries) setContainerWidth(en.contentRect.width);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, [devices.size]);
  const list = useMemo(
    () => Array.from(devices.values()).sort((a, b) => a.node.localeCompare(b.node)),
    [devices],
  );

  if (list.length === 0) {
    return (
      <div className="device-view device-view-empty">
        <p>No firmware devices attached.</p>
        <p className="muted">Load a scenario with firmware nodes (e.g. <code>emulator-3-pagers</code>) to see live pagers here.</p>
      </div>
    );
  }

  return (
    <div className="device-view">
      <div className="device-view-toolbar">
        <span className="device-view-count">{list.length} device{list.length === 1 ? '' : 's'}</span>
        <button
          className={`device-mute${muted ? ' muted' : ''}`}
          onClick={() => setMuted((m) => !m)}
          aria-pressed={!muted}
          title={muted ? 'Buzzer muted' : 'Buzzer audible'}
        >
          {muted ? '🔇 Muted' : '🔊 Audio on'}
        </button>
      </div>

      <div className="device-grid" ref={gridRef}>
        {list.map((d) => (
          // Panel geometry selects the device face: a 128-wide frame is the
          // SSD1306 OLED (Heltec), anything else is the SSD1680 e-paper pager.
          d.fbWidth === 128 ? (
            <HeltecDevice
              key={d.node}
              device={d}
              faceWidth={computeFaceWidth(containerWidth, list.length)}
              onButton={(id, edge) => onButton(d.node, id, edge)}
            />
          ) : (
            <PagerDevice
              key={d.node}
              device={d}
              muted={muted}
              faceWidth={computeFaceWidth(containerWidth, list.length)}
              onButton={(id, edge) => onButton(d.node, id, edge)}
            />
          )
        ))}
      </div>
    </div>
  );
}
