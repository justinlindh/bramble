// DeviceView.tsx
//
// The device tab: one PagerDevice per firmware node, a global mute toggle for
// the buzzer, and a per-node selector. Additive to the mesh map (App keeps both
// views); a firmware node appears here as soon as it streams its first frame or
// indicator.

import { useMemo, useState } from 'react';
import type { DeviceState } from '../types';
import PagerDevice, { type EdgeButtonId } from './PagerDevice';

export interface DeviceViewProps {
  devices: Map<string, DeviceState>;
  onButton: (node: string, id: EdgeButtonId, edge: 'down' | 'up') => void;
}

export default function DeviceView({ devices, onButton }: DeviceViewProps) {
  const [muted, setMuted] = useState(true); // buzzer muted by default
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

      <div className="device-grid">
        {list.map((d) => (
          <PagerDevice
            key={d.node}
            device={d}
            muted={muted}
            onButton={(id, edge) => onButton(d.node, id, edge)}
          />
        ))}
      </div>
    </div>
  );
}
