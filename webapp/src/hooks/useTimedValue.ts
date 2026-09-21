import { useCallback, useEffect, useRef, useState } from 'react';

// useTimedValue backs a transient, self-clearing UI value: an inline "saved"
// message, a "Copied" row label, any signal that should show for a moment and
// then disappear on its own. `show(value)` sets the value and schedules it back
// to `cleared` after ms; a second show restarts the window; reset clears it
// immediately (e.g. before starting a fresh action); and the pending timer is
// cancelled on unmount, so a component torn down mid-window (a closing modal, a
// tab the user leaves) never runs its timeout callback against a dead component.
// The hand-rolled `setX(v); setTimeout(() => setX(cleared), ms)` copies this
// replaces omitted that cleanup. useTimedFlag is this hook specialized to a
// boolean flag.
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
