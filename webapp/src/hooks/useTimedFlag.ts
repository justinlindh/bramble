import { useCallback } from 'react';
import { useTimedValue } from './useTimedValue';

// useTimedFlag backs the transient "Copied ✓" indicator on a copy button: the
// returned trigger sets the flag true and schedules it back to false after ms.
// A second trigger restarts the window; reset clears it immediately (e.g. when
// a new value is generated); and the pending timer is cancelled on unmount, so
// a component torn down mid-flash (a closing modal, a list row that scrolls out)
// never runs its timeout callback against a dead component. It is useTimedValue
// specialized to a boolean flag.
export function useTimedFlag(ms: number): readonly [boolean, () => void, () => void] {
  const [on, show, reset] = useTimedValue(false, ms);
  const trigger = useCallback(() => show(true), [show]);
  return [on, trigger, reset];
}
