import { useCallback, useEffect, useRef, useState } from 'react';

// useTimedFlag backs the transient "Copied ✓" indicator on a copy button: the
// returned trigger sets the flag true and schedules it back to false after ms.
// A second trigger restarts the window; reset clears it immediately (e.g. when
// a new value is generated); and the pending timer is cancelled on unmount, so
// a component torn down mid-flash (a closing modal, a list row that scrolls out)
// never runs its timeout callback against a dead component. The hand-rolled
// `setTimeout(() => setX(false), ms)` copies this replaces omitted that cleanup.
export function useTimedFlag(ms: number): readonly [boolean, () => void, () => void] {
  const [on, setOn] = useState(false);
  const timer = useRef<number | null>(null);

  const clear = useCallback(() => {
    if (timer.current !== null) {
      window.clearTimeout(timer.current);
      timer.current = null;
    }
  }, []);

  useEffect(() => clear, [clear]);

  const trigger = useCallback(() => {
    setOn(true);
    clear();
    timer.current = window.setTimeout(() => {
      setOn(false);
      timer.current = null;
    }, ms);
  }, [clear, ms]);

  const reset = useCallback(() => {
    clear();
    setOn(false);
  }, [clear]);

  return [on, trigger, reset];
}
