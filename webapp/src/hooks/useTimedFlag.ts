import { useCallback } from 'react';
import { useTimedValue } from './useTimedValue';

// useTimedFlag is useTimedValue specialized to a boolean: trigger() shows true
// and reverts to false after ms.
export function useTimedFlag(ms: number): readonly [boolean, () => void, () => void] {
  const [on, show, reset] = useTimedValue(false, ms);
  const trigger = useCallback(() => show(true), [show]);
  return [on, trigger, reset];
}
