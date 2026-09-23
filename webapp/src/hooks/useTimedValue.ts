import { useCallback, useEffect, useRef, useState } from 'react';

// useTimedValue holds a transient UI value that clears itself. `show(value)`
// sets it and schedules a revert to `cleared` after ms; a second show restarts
// the window; reset clears it immediately; the pending timer is cancelled on
// unmount. Pass a primitive or stable `cleared`, or show/reset change identity
// every render.
export function useTimedValue<T>(
  cleared: T,
  ms: number,
): readonly [T, (value: T) => void, () => void] {
  const [value, setValue] = useState<T>(cleared);
  const timer = useRef<number | null>(null);

  const clear = useCallback(() => {
    if (timer.current !== null) {
      window.clearTimeout(timer.current);
      timer.current = null;
    }
  }, []);

  useEffect(() => clear, [clear]);

  const show = useCallback(
    (next: T) => {
      setValue(next);
      clear();
      timer.current = window.setTimeout(() => {
        setValue(cleared);
        timer.current = null;
      }, ms);
    },
    [clear, cleared, ms],
  );

  const reset = useCallback(() => {
    clear();
    setValue(cleared);
  }, [clear, cleared]);

  return [value, show, reset];
}
