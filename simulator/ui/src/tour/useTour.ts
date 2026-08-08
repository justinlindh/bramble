// useTour.ts
//
// The tour's position: which step is showing, which steps are finished, and
// whether the panel has been dismissed. Persisted to localStorage so the tour
// is resumable across a reload (the emulator page is reloaded often, and being
// thrown back to step one every time would make the tour worse than no tour).
//
// A step is finished when its own predicate observes the firmware doing the
// thing (steps.ts), or when the user skips it. Finished steps stay finished:
// the console buffers they read are bounded rings, and un-completing a step
// because a line scrolled away would be a lie about what happened.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { Fleet } from './fleet';
import type { Milestones } from './milestones';
import { TOUR_STEPS, type TourStep } from './steps';

export const TOUR_STORAGE_KEY = 'bramble.playground.tour.v1';

export interface TourProgress {
  // Index into TOUR_STEPS of the step on screen.
  index: number;
  // Step ids the user has finished or skipped.
  done: string[];
  // Panel collapsed to the resume pill.
  dismissed: boolean;
}

const INITIAL: TourProgress = { index: 0, done: [], dismissed: false };

// readProgress is tolerant of anything in storage: a truncated write, a value
// from an older shape, or a browser that refuses storage entirely all fall
// back to starting the tour from the beginning.
export function readProgress(raw: string | null): TourProgress {
  if (!raw) return INITIAL;
  try {
    const parsed = JSON.parse(raw) as Partial<TourProgress>;
    const done = Array.isArray(parsed.done)
      ? parsed.done.filter((d): d is string => typeof d === 'string')
      : [];
    const index =
      typeof parsed.index === 'number' && parsed.index >= 0 && parsed.index < TOUR_STEPS.length
        ? Math.floor(parsed.index)
        : 0;
    return { index, done, dismissed: parsed.dismissed === true };
  } catch {
    return INITIAL;
  }
}

function load(): TourProgress {
  try {
    return readProgress(window.localStorage.getItem(TOUR_STORAGE_KEY));
  } catch {
    return INITIAL;
  }
}

function save(p: TourProgress): void {
  try {
    window.localStorage.setItem(TOUR_STORAGE_KEY, JSON.stringify(p));
  } catch {
    /* storage unavailable (private mode, quota): the tour still works, it
     * just will not survive a reload. Not worth an error surface. */
  }
}

export interface TourController {
  step: TourStep;
  index: number;
  total: number;
  done: string[];
  dismissed: boolean;
  // True when every step has been finished or skipped.
  finished: boolean;
  next(): void;
  back(): void;
  skip(): void;
  goTo(index: number): void;
  dismiss(): void;
  resume(): void;
  restart(): void;
}

export function useTour(milestones: Milestones, fleet: Fleet): TourController {
  const [progress, setProgress] = useState<TourProgress>(load);
  const progressRef = useRef(progress);
  progressRef.current = progress;

  useEffect(() => {
    save(progress);
  }, [progress]);

  // Observe the current step's completion. Marking it done also advances to
  // the first step that is still open, which is what "steps advance on
  // completion where detectable" means: the user does not have to click Next
  // for something the mesh already proved.
  const step = TOUR_STEPS[progress.index] ?? TOUR_STEPS[0];
  const stepDone = step.done(milestones, fleet);

  useEffect(() => {
    if (!stepDone) return;
    setProgress((p) => {
      const cur = TOUR_STEPS[p.index];
      if (!cur || p.done.includes(cur.id)) return p;
      const done = [...p.done, cur.id];
      let index = p.index;
      while (index < TOUR_STEPS.length - 1 && done.includes(TOUR_STEPS[index].id)) {
        index += 1;
      }
      return { ...p, done, index };
    });
  }, [stepDone, progress.index]);

  const next = useCallback(() => {
    setProgress((p) => ({ ...p, index: Math.min(p.index + 1, TOUR_STEPS.length - 1) }));
  }, []);

  const back = useCallback(() => {
    setProgress((p) => ({ ...p, index: Math.max(p.index - 1, 0) }));
  }, []);

  const skip = useCallback(() => {
    setProgress((p) => {
      const cur = TOUR_STEPS[p.index];
      const done = cur && !p.done.includes(cur.id) ? [...p.done, cur.id] : p.done;
      return { ...p, done, index: Math.min(p.index + 1, TOUR_STEPS.length - 1) };
    });
  }, []);

  const goTo = useCallback((index: number) => {
    setProgress((p) => ({
      ...p,
      index: Math.max(0, Math.min(index, TOUR_STEPS.length - 1)),
    }));
  }, []);

  const dismiss = useCallback(() => setProgress((p) => ({ ...p, dismissed: true })), []);
  const resume = useCallback(() => setProgress((p) => ({ ...p, dismissed: false })), []);
  const restart = useCallback(() => setProgress({ ...INITIAL }), []);

  const finished = useMemo(
    () => TOUR_STEPS.every((s) => progress.done.includes(s.id)),
    [progress.done],
  );

  return {
    step,
    index: progress.index,
    total: TOUR_STEPS.length,
    done: progress.done,
    dismissed: progress.dismissed,
    finished,
    next,
    back,
    skip,
    goTo,
    dismiss,
    resume,
    restart,
  };
}
