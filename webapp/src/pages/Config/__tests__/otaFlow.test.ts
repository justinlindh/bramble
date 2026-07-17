import { describe, expect, it } from 'vitest';
import { initialOtaFlow, otaFlowNext } from '../otaFlow';

describe('otaFlow', () => {
  it('walks the happy path, tolerating a missing v prefix', () => {
    let s = initialOtaFlow();
    s = otaFlowNext(s, { kind: 'start', targetVersion: 'v1.4.0', prevVersion: 'v1.3.9' });
    expect(s.phase).toBe('running');
    s = otaFlowNext(s, { kind: 'event', state: 'downloading', percent: 40 });
    expect(s.percent).toBe(40);
    s = otaFlowNext(s, { kind: 'event', state: 'rebooting', percent: 100 });
    expect(s.phase).toBe('rebooting');
    s = otaFlowNext(s, { kind: 'reconnected', version: '1.4.0' });
    expect(s.phase).toBe('done');
  });
  it('fails when the node comes back on the old version after a real reboot', () => {
    let s = otaFlowNext(
      otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v1.4.0', prevVersion: 'v1.3.9' }),
      { kind: 'event', state: 'rebooting', percent: 100 });
    s = otaFlowNext(s, { kind: 'reconnected', version: 'v1.3.9' });
    expect(s.phase).toBe('failed');
    expect(s.error).toMatch(/came back on v1.3.9/);
  });
  it('treats disconnect-while-running as rebooting', () => {
    let s = otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v1.4.0', prevVersion: 'v1.3.9' });
    s = otaFlowNext(s, { kind: 'disconnected' });
    expect(s.phase).toBe('rebooting');
  });
  it('surfaces failed events with their error', () => {
    let s = otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v1.4.0', prevVersion: 'v1.3.9' });
    s = otaFlowNext(s, { kind: 'event', state: 'failed', percent: 30, error: 'signature verification failed' });
    expect(s.phase).toBe('failed');
    expect(s.error).toBe('signature verification failed');
  });
  it('times out the reboot wait', () => {
    let s = otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v1.4.0', prevVersion: 'v1.3.9' });
    s = otaFlowNext(s, { kind: 'event', state: 'rebooting', percent: 100 });
    s = otaFlowNext(s, { kind: 'rebootTimeout' });
    expect(s.phase).toBe('failed');
    expect(s.error).toMatch(/USB/);
  });

  // Finding 4: a transient blip (disconnect with no reboot event) that comes
  // back on the SAME version is not a failure; resume the running phase.
  it('resumes running after a blip that reconnects on the same version', () => {
    let s = otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v0.5.0', prevVersion: '0.4.0' });
    s = otaFlowNext(s, { kind: 'event', state: 'downloading', percent: 55 });
    // A WS/BLE blip mid-download, NOT preceded by a rebooting event.
    s = otaFlowNext(s, { kind: 'disconnected' });
    expect(s.phase).toBe('rebooting');
    expect(s.sawRebootEvent).toBe(false);
    // Node auto-reconnects still on the old version; the OTA is still running.
    s = otaFlowNext(s, { kind: 'reconnected', version: '0.4.0' });
    expect(s.phase).toBe('running');
    expect(s.percent).toBe(55);
  });

  // Finding 9: the index folder version (target) and the firmware PROJECT_VER
  // need not be semver-equal; a version that CHANGED to something else is a
  // success, reported honestly via resultVersion.
  it('treats a changed-but-not-target version as success with resultVersion', () => {
    let s = otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v0.5.0', prevVersion: '0.4.0' });
    s = otaFlowNext(s, { kind: 'event', state: 'rebooting', percent: 100 });
    s = otaFlowNext(s, { kind: 'reconnected', version: '0.5.0-dev' });
    expect(s.phase).toBe('done');
    expect(s.resultVersion).toBe('0.5.0-dev');
  });

  // A genuine did-not-stick: real reboot event, node returns on the SAME
  // version it started on.
  it('fails did-not-stick when a real reboot returns the prior version', () => {
    let s = otaFlowNext(initialOtaFlow(), { kind: 'start', targetVersion: 'v0.5.0', prevVersion: '0.4.0' });
    s = otaFlowNext(s, { kind: 'event', state: 'rebooting', percent: 100 });
    expect(s.sawRebootEvent).toBe(true);
    s = otaFlowNext(s, { kind: 'reconnected', version: '0.4.0' });
    expect(s.phase).toBe('failed');
    expect(s.error).toMatch(/did not stick/);
  });
});
