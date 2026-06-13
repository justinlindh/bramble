/**
 * Live-ticking "time ago" utilities (Issue #98).
 *
 * useAgeTick() returns a tick counter that increments every second so
 * components that call it re-render and display fresh age strings. A single
 * interval drives all consumers; the interval pauses when the tab is hidden.
 */
import { useEffect, useState } from 'react';

/**
 * Pure function: convert an elapsed milliseconds value to a human-readable
 * age string. The caller supplies the elapsed time; this function has no
 * dependency on the current clock and is trivially testable.
 *
 * Examples: 500 -> "just now", 5000 -> "5s ago", 90000 -> "1m ago"
 */
export function formatAge(elapsedMs: number): string {
  if (elapsedMs < 1000) return 'just now';
  const s = Math.floor(elapsedMs / 1000);
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ago`;
  return `${Math.floor(m / 60)}h ago`;
}

/**
 * React hook: returns a monotonically-increasing tick counter (starting at 0)
 * that increments every second while the tab is visible. All components that
 * call this hook share the re-render cadence; there are no per-row timers.
 *
 * Usage:
 *   const tick = useAgeTick();
 *   // tick value itself is unused; it just forces a re-render every second.
 *   const age = formatAge(Date.now() - peer.lastHeardAt);
 */
export function useAgeTick(): number {
  const [tick, setTick] = useState(0);

  useEffect(() => {
    let intervalId: ReturnType<typeof setInterval> | null = null;

    function start() {
      if (intervalId !== null) return;
      intervalId = setInterval(() => setTick((t) => t + 1), 1000);
    }

    function stop() {
      if (intervalId === null) return;
      clearInterval(intervalId);
      intervalId = null;
    }

    function onVisibilityChange() {
      if (document.hidden) {
        stop();
      } else {
        setTick((t) => t + 1); // immediate update on tab reveal
        start();
      }
    }

    // Start immediately unless the tab is already hidden.
    if (typeof document === 'undefined' || !document.hidden) {
      start();
    }

    if (typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', onVisibilityChange);
    }

    return () => {
      stop();
      if (typeof document !== 'undefined') {
        document.removeEventListener('visibilitychange', onVisibilityChange);
      }
    };
  }, []);

  return tick;
}
