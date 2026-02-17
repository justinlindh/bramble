import { useEffect, useRef } from 'react';

/**
 * Calls `fn` immediately, then on every `intervalMs` milliseconds.
 * Stops polling when the component unmounts.
 */
export function usePoll(fn: () => void | Promise<void>, intervalMs: number): void {
  const fnRef = useRef(fn);
  fnRef.current = fn;

  useEffect(() => {
    let active = true;
    let timerId: ReturnType<typeof setTimeout>;

    async function tick() {
      if (!active) return;
      try {
        await fnRef.current();
      } catch {
        // silently ignore poll errors
      }
      if (active) {
        timerId = setTimeout(tick, intervalMs);
      }
    }

    tick();

    return () => {
      active = false;
      clearTimeout(timerId);
    };
  }, [intervalMs]);
}
