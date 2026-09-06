// Pure state machine for the firmware-update journey. The component owns
// timers and subscriptions; this owns every transition, so tests cover the
// journey without mounting React.
import { compareSemver } from '../../lib/semver';

type OtaFlowPhase = 'idle' | 'running' | 'rebooting' | 'done' | 'failed';

export interface OtaFlowState {
  phase: OtaFlowPhase;
  targetVersion: string;
  // The running firmware version captured when the install started, so a
  // reconnect can tell "still the old image" (blip or did-not-stick) apart
  // from "moved to a new image" (success, even if not exactly the target).
  prevVersion: string;
  percent: number;
  stage: string;
  // True once the firmware reported a real 'rebooting' event. A bare transport
  // drop (blip) does NOT set this, which is how a blip is told from a reboot.
  sawRebootEvent: boolean;
  error?: string;
  // The version the node actually reports after a successful update, when it
  // differs from the target (finding 9). Shown on success in place of target.
  resultVersion?: string;
}

export type OtaFlowInput =
  | { kind: 'start'; targetVersion: string; prevVersion: string }
  | { kind: 'event'; state: string; percent: number; error?: string }
  | { kind: 'disconnected' }
  | { kind: 'reconnected'; version: string }
  | { kind: 'rebootTimeout' }
  | { kind: 'reset' };

export function initialOtaFlow(): OtaFlowState {
  return { phase: 'idle', targetVersion: '', prevVersion: '', percent: 0, stage: '', sawRebootEvent: false };
}

export function otaFlowNext(s: OtaFlowState, input: OtaFlowInput): OtaFlowState {
  switch (input.kind) {
    case 'start':
      return {
        phase: 'running',
        targetVersion: input.targetVersion,
        prevVersion: input.prevVersion,
        percent: 0,
        stage: 'starting',
        sawRebootEvent: false,
      };
    case 'event': {
      if (s.phase !== 'running') return s;
      if (input.state === 'failed') {
        return { ...s, phase: 'failed', error: input.error ?? 'Update failed.' };
      }
      if (input.state === 'rebooting') {
        return { ...s, phase: 'rebooting', percent: 100, stage: 'rebooting', sawRebootEvent: true };
      }
      return { ...s, percent: input.percent, stage: input.state };
    }
    case 'disconnected':
      // A transport drop while installing folds into the reboot wait. It may be
      // a real reboot or just a blip, so sawRebootEvent is left UNCHANGED; the
      // reconnect decides which happened.
      return s.phase === 'running' ? { ...s, phase: 'rebooting', stage: 'rebooting' } : s;
    case 'reconnected': {
      if (s.phase !== 'rebooting') return s;
      // 1. Exactly the target: unambiguous success.
      if (compareSemver(input.version, s.targetVersion) === 0) {
        return { ...s, phase: 'done' };
      }
      // 2. Moved to some other version: still a success, reported honestly.
      if (compareSemver(input.version, s.prevVersion) !== 0) {
        return { ...s, phase: 'done', resultVersion: input.version };
      }
      // 3. Still on the old version after a real reboot: it did not stick.
      if (s.sawRebootEvent) {
        return {
          ...s, phase: 'failed',
          error: `Node came back on ${input.version}; the update did not stick.`,
        };
      }
      // 4. Still on the old version and no reboot was ever reported: this was
      // just a blip. Resume running; the still-armed event/poll drive progress.
      return { ...s, phase: 'running', stage: 'reconnecting' };
    }
    case 'rebootTimeout':
      return s.phase === 'rebooting'
        ? { ...s, phase: 'failed', error: 'Node did not come back; if it stays offline, recover over USB (see docs/ota-rollout.md).' }
        : s;
    case 'reset':
      return initialOtaFlow();
  }
}
