// TourOverlay.tsx
//
// The guided tour: a dockable panel that walks a first-time visitor through
// the playground fleet, plus a ring drawn around whichever real UI element the
// current step is about.
//
// Self-contained by design. Nothing it points at knows it exists: targets are
// found by their existing data-testid and ringed from a fixed-position overlay,
// so the mesh canvas, the device cards and the consoles render exactly as they
// do without the tour. The only thing App has to do is mount it and hand it
// the socket and the view toggle.
//
// Every action button sends a real broker command, which the emulator control
// path (emulator/node/emu_control.c) turns into a real firmware call. Nothing
// in the tour fakes an outcome: a step completes when the firmware's own
// console says the thing happened.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { DeviceState, SimNode } from '../types';
import {
  CHANNEL_TEXT,
  DM_TEXT,
  FLEET_ROLES,
  PLAYGROUND_NETWORK_KEY,
  RECEIPT_TEXT,
  ROLE_NAMES,
  resolveFleet,
  type Fleet,
  type FleetRole,
} from './fleet';
import {
  EMPTY_MILESTONES,
  latchMilestones,
  scanConsoles,
  type Milestones,
} from './milestones';
import { useTour } from './useTour';
import { TOUR_STEPS, type TourAction } from './steps';
import './tour.css';

// Step ids in order, for the progress rail's per-dot completion mark.
const TOUR_STEP_IDS = TOUR_STEPS.map((s) => s.id);

export interface TourOverlayProps {
  devices: Map<string, DeviceState>;
  nodes: Map<string, SimNode>;
  ws: WebSocket | null;
  view: 'mesh' | 'devices';
  onView(view: 'mesh' | 'devices'): void;
}

interface Box {
  top: number;
  left: number;
  width: number;
  height: number;
}

// Milestone lists only ever grow (latchMilestones unions them), so comparing
// lengths is a complete equality test and keeps the scan effect from looping.
function sameMilestones(a: Milestones, b: Milestones): boolean {
  return (
    a.inert.length === b.inert.length &&
    a.controlReady.length === b.controlReady.length &&
    a.provisioned.length === b.provisioned.length &&
    a.channelHeardBy.length === b.channelHeardBy.length &&
    a.dmHeardBy.length === b.dmHeardBy.length &&
    a.verified.length === b.verified.length &&
    a.receiptTextHeardBy.length === b.receiptTextHeardBy.length &&
    a.receipts.length === b.receipts.length
  );
}

function sameBox(a: Box | null, b: Box | null): boolean {
  if (a === null || b === null) return a === b;
  return a.top === b.top && a.left === b.left && a.width === b.width && a.height === b.height;
}

// useTargetBox tracks where the ringed element currently is. Polled rather
// than observed because the target changes identity between steps and can be
// unmounted entirely (a device card only exists in the devices view); a poll
// handles appearance, disappearance, scrolling and resizing with one path.
function useTargetBox(testid: string | null): Box | null {
  const [box, setBox] = useState<Box | null>(null);
  const boxRef = useRef<Box | null>(null);

  useEffect(() => {
    if (!testid) {
      boxRef.current = null;
      setBox(null);
      return;
    }
    const measure = () => {
      const el = document.querySelector(`[data-testid="${testid}"]`);
      let next: Box | null = null;
      if (el) {
        const r = el.getBoundingClientRect();
        if (r.width > 0 && r.height > 0) {
          next = { top: r.top, left: r.left, width: r.width, height: r.height };
        }
      }
      if (!sameBox(boxRef.current, next)) {
        boxRef.current = next;
        setBox(next);
      }
    };
    measure();
    const id = window.setInterval(measure, 400);
    window.addEventListener('resize', measure);
    return () => {
      window.clearInterval(id);
      window.removeEventListener('resize', measure);
    };
  }, [testid]);

  return box;
}

// dmDestination returns the address a DM to this node must be addressed to.
//
// That is the node's own FIRMWARE address, which is exactly its emu-link hello
// id: main.c formats the id as "%08X" of the identity address when it connects
// to the broker. The "addr" the broker publishes alongside it is a different
// number, the address the simulation assigned that node's slot for its own
// bookkeeping, and sending to that would address nobody.
function dmDestination(id: string | null): string | null {
  if (!id) return null;
  return /^[0-9A-F]{8}$/.test(id) ? id : null;
}

export default function TourOverlay({ devices, nodes, ws, view, onView }: TourOverlayProps) {
  const [milestones, setMilestones] = useState<Milestones>(EMPTY_MILESTONES);

  const fleet = useMemo<Fleet>(() => resolveFleet(nodes.values()), [nodes]);

  useEffect(() => {
    const scan = scanConsoles(
      [...devices.entries()].map(([id, d]) => [id, d.console] as const),
    );
    setMilestones((prev) => {
      const merged = latchMilestones(prev, scan);
      return sameMilestones(prev, merged) ? prev : merged;
    });
  }, [devices]);

  const tour = useTour(milestones, fleet);
  const step = tour.step;
  const targetId = step.target(fleet);
  const box = useTargetBox(tour.dismissed ? null : targetId);

  const send = useCallback(
    (cmd: Record<string, unknown>) => {
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(cmd));
    },
    [ws],
  );

  const provision = useCallback(
    (roles: FleetRole[]) => {
      for (const role of roles) {
        const id = fleet[role];
        if (id) send({ type: 'prov', node: id, key: PLAYGROUND_NETWORK_KEY });
      }
    },
    [fleet, send],
  );

  const runAction = useCallback(
    (action: TourAction) => {
      switch (action.kind) {
        case 'provision-pair':
          provision(['alpha', 'bravo']);
          break;
        case 'provision-rest':
          provision(['charlie']);
          break;
        case 'send-channel':
          if (fleet.alpha) send({ type: 'send', node: fleet.alpha, text: CHANNEL_TEXT });
          break;
        case 'announce-identity':
          if (fleet.alpha) send({ type: 'attest', node: fleet.alpha });
          break;
        case 'send-dm': {
          const to = dmDestination(fleet.bravo);
          if (fleet.alpha && to) send({ type: 'send', node: fleet.alpha, text: DM_TEXT, to });
          break;
        }
        case 'send-receipt':
          if (fleet.alpha) send({ type: 'send', node: fleet.alpha, text: RECEIPT_TEXT });
          break;
      }
    },
    [fleet, provision, send],
  );

  // Every action goes through a node's emu-link control path, and a node
  // registers those handlers only once its mesh is up (emu_control.c), some
  // seconds after it attaches to the broker. A message sent before then is
  // dropped by the node with no reply, so a button that could fire early
  // would look like a broken tour. Gate on the node's own ready line.
  const actionReady = useCallback(
    (action: TourAction): boolean => {
      const ready = action.needs.every((role) => {
        const id = fleet[role];
        return id !== null && milestones.controlReady.includes(id);
      });
      if (!ready) return false;
      if (action.kind === 'send-dm') return dmDestination(fleet.bravo) !== null;
      return true;
    },
    [fleet, milestones],
  );

  if (tour.dismissed) {
    return (
      <button
        type="button"
        className="tour-resume"
        data-testid="tour-resume"
        onClick={tour.resume}
      >
        Resume the guided tour
      </button>
    );
  }

  const stepDone = tour.done.includes(step.id);

  return (
    <>
      {box && (
        <div
          className="tour-ring"
          data-testid="tour-ring"
          data-tour-target={targetId ?? ''}
          style={{ top: box.top - 6, left: box.left - 6, width: box.width + 12, height: box.height + 12 }}
        />
      )}

      <aside className="tour-panel" data-testid="tour-panel" data-tour-step={step.id}>
        <header className="tour-head">
          <span className="tour-kicker">Guided tour</span>
          <span className="tour-count" data-testid="tour-count">
            Step {tour.index + 1} of {tour.total}
          </span>
          <button
            type="button"
            className="tour-x"
            data-testid="tour-dismiss"
            title="Hide the tour (it resumes where you left it)"
            onClick={tour.dismiss}
          >
            &times;
          </button>
        </header>

        <ol className="tour-rail" data-testid="tour-rail">
          {Array.from({ length: tour.total }, (_, i) => i).map((i) => (
            <li key={i}>
              <button
                type="button"
                className={`tour-dot ${i === tour.index ? 'tour-dot-active' : ''} ${
                  tour.done.includes(TOUR_STEP_IDS[i]) ? 'tour-dot-done' : ''
                }`}
                data-testid={`tour-dot-${i}`}
                title={`Step ${i + 1}`}
                onClick={() => tour.goTo(i)}
              />
            </li>
          ))}
        </ol>

        <h2 className="tour-title" data-testid="tour-title">
          {step.title}
        </h2>
        {step.body.map((para, i) => (
          <p className="tour-body" key={i}>
            {para}
          </p>
        ))}

        <div className="tour-fleet" data-testid="tour-fleet-status">
          {FLEET_ROLES.map((role) => {
            const id = fleet[role];
            // "booting" covers both not-yet-attached and attached-but-not-
            // yet-drivable; "inert" is the fail-closed state the tour is
            // about, so it is only claimed once the node can be driven.
            const state = !id
              ? 'booting'
              : milestones.provisioned.includes(id)
                ? 'provisioned'
                : milestones.controlReady.includes(id)
                  ? 'inert'
                  : 'booting';
            return (
              <span
                className={`tour-chip tour-chip-${state}`}
                key={role}
                data-testid={`tour-node-${role}`}
                data-node-id={id ?? ''}
              >
                {ROLE_NAMES[role]}: {state}
              </span>
            );
          })}
        </div>

        {view !== step.view && (
          <button
            type="button"
            className="tour-secondary"
            data-testid="tour-switch-view"
            onClick={() => onView(step.view)}
          >
            {step.view === 'mesh' ? 'Show me the mesh map' : 'Show me the devices'}
          </button>
        )}

        {step.actions.length > 0 && (
          <div className="tour-actions">
            {step.actions.map((action) => (
              <button
                type="button"
                key={action.kind}
                className="tour-primary"
                data-testid={`tour-action-${action.kind}`}
                disabled={!actionReady(action)}
                onClick={() => runAction(action)}
              >
                {action.label}
              </button>
            ))}
          </div>
        )}

        <p className={`tour-status ${stepDone ? 'tour-status-done' : ''}`} data-testid="tour-detail">
          {stepDone ? 'Done: ' : `Waiting for ${step.waitingFor}. `}
          {step.detail(milestones, fleet)}
        </p>

        {tour.finished && (
          <p className="tour-finished" data-testid="tour-finished">
            That is the whole tour. This was a browser fleet on a simulated radio, not a field
            deployment: real range, interference and battery life are not modelled here. See
            docs/playground.md for what carries over to hardware and what does not.
          </p>
        )}

        <footer className="tour-foot">
          <button
            type="button"
            className="tour-secondary"
            data-testid="tour-back"
            disabled={tour.index === 0}
            onClick={tour.back}
          >
            Back
          </button>
          <button type="button" className="tour-secondary" data-testid="tour-skip" onClick={tour.skip}>
            Skip step
          </button>
          <button
            type="button"
            className="tour-secondary"
            data-testid="tour-next"
            disabled={tour.index >= tour.total - 1}
            onClick={tour.next}
          >
            Next
          </button>
          <button type="button" className="tour-secondary" data-testid="tour-restart" onClick={tour.restart}>
            Restart
          </button>
        </footer>
      </aside>
    </>
  );
}
