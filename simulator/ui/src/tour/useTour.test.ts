import { describe, expect, it, beforeEach } from 'vitest';
import { act, renderHook } from '@testing-library/react';
import { EMPTY_MILESTONES, latchMilestones, scanConsoles } from './milestones';
import { resolveFleet } from './fleet';
import { TOUR_STORAGE_KEY, readProgress, useTour } from './useTour';
import { TOUR_STEPS } from './steps';

const FLEET = resolveFleet([
  { id: 'alpha-0', x: 0, kind: 'firmware' },
  { id: 'bravo-0', x: 100, kind: 'firmware' },
  { id: 'charlie-0', x: 200, kind: 'firmware' },
]);
const NO_FLEET = resolveFleet([]);

// The fleet's control paths are up, which is what the orientation step waits
// for (a node that has attached but not opened its control path cannot be
// driven yet).
const READY = latchMilestones(
  EMPTY_MILESTONES,
  scanConsoles(
    ['alpha-0', 'bravo-0', 'charlie-0'].map(
      (id) => [id, ['I (1) emu_control: emu-link control path ready (prov, send)']] as const,
    ),
  ),
);

describe('readProgress', () => {
  beforeEach(() => window.localStorage.clear());

  it('starts at the first step with nothing stored', () => {
    expect(readProgress(null)).toEqual({ index: 0, done: [], dismissed: false });
  });

  it('restores a saved position, which is what makes the tour resumable', () => {
    expect(readProgress(JSON.stringify({ index: 3, done: ['orientation'], dismissed: true }))).toEqual({
      index: 3,
      done: ['orientation'],
      dismissed: true,
    });
  });

  it('falls back to the start on corrupt or out-of-range storage', () => {
    expect(readProgress('{not json')).toEqual({ index: 0, done: [], dismissed: false });
    expect(readProgress(JSON.stringify({ index: 99 })).index).toBe(0);
    expect(readProgress(JSON.stringify({ index: 1, done: [1, 'orientation'] })).done).toEqual([
      'orientation',
    ]);
  });
});

describe('useTour', () => {
  beforeEach(() => window.localStorage.clear());

  it('advances by itself when the firmware completes the current step', () => {
    const { result, rerender } = renderHook(
      ({ m, f }) => useTour(m, f),
      { initialProps: { m: EMPTY_MILESTONES, f: NO_FLEET } },
    );
    expect(result.current.step.id).toBe('orientation');

    // The fleet attaches and opens its control paths: the orientation step is
    // observably complete.
    rerender({ m: READY, f: FLEET });
    expect(result.current.done).toContain('orientation');
    expect(result.current.step.id).toBe('provision');
  });

  it('skips a step without waiting for the mesh', () => {
    const { result } = renderHook(() => useTour(EMPTY_MILESTONES, NO_FLEET));
    act(() => result.current.skip());
    expect(result.current.done).toEqual(['orientation']);
    expect(result.current.step.id).toBe('provision');
  });

  it('moves back and forth manually and clamps at both ends', () => {
    const { result } = renderHook(() => useTour(EMPTY_MILESTONES, NO_FLEET));
    act(() => result.current.back());
    expect(result.current.index).toBe(0);
    for (let i = 0; i < TOUR_STEPS.length + 2; i++) act(() => result.current.next());
    expect(result.current.index).toBe(TOUR_STEPS.length - 1);
  });

  it('dismisses and resumes, and persists both across a remount', () => {
    const first = renderHook(() => useTour(EMPTY_MILESTONES, NO_FLEET));
    act(() => first.result.current.goTo(2));
    act(() => first.result.current.dismiss());
    first.unmount();

    const second = renderHook(() => useTour(EMPTY_MILESTONES, NO_FLEET));
    expect(second.result.current.dismissed).toBe(true);
    expect(second.result.current.index).toBe(2);
    act(() => second.result.current.resume());
    expect(second.result.current.dismissed).toBe(false);
    expect(window.localStorage.getItem(TOUR_STORAGE_KEY)).toContain('"dismissed":false');
  });

  it('keeps a finished step finished after its console line scrolls away', () => {
    const provisioned = latchMilestones(
      READY,
      scanConsoles([
        ['alpha-0', ['I (1) emu_control: network key provisioned over emu-link (fingerprint AA)']],
        ['bravo-0', ['I (1) emu_control: network key provisioned over emu-link (fingerprint BB)']],
        ['charlie-0', ['I (1) emu_control: network key provisioned over emu-link (fingerprint CC)']],
      ]),
    );
    const { result, rerender } = renderHook(({ m }) => useTour(m, FLEET), {
      initialProps: { m: provisioned },
    });
    expect(result.current.done).toEqual(expect.arrayContaining(['orientation', 'provision']));

    // A later scan whose buffers no longer hold the provisioning lines.
    rerender({ m: latchMilestones(provisioned, scanConsoles([['alpha-0', ['I (9) mesh: beacon']]])) });
    expect(result.current.done).toEqual(expect.arrayContaining(['provision']));
  });

  it('reports finished only when every step is done or skipped', () => {
    const { result } = renderHook(() => useTour(EMPTY_MILESTONES, NO_FLEET));
    expect(result.current.finished).toBe(false);
    for (let i = 0; i < TOUR_STEPS.length; i++) act(() => result.current.skip());
    expect(result.current.finished).toBe(true);
    act(() => result.current.restart());
    expect(result.current.finished).toBe(false);
    expect(result.current.index).toBe(0);
  });
});
