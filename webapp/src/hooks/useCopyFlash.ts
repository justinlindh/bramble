import { useCallback } from 'react';
import { useTimedFlag } from './useTimedFlag';
import { copyWithFallback } from '../utils/clipboard';

// useCopyFlash pairs copyWithFallback with useTimedFlag: the "copy this text,
// then show a transient 'Copied' on the button" pattern that several config and
// share surfaces repeat. `copy(text)` runs the clipboard write and flashes the
// indicator on success, returning whether the copy succeeded so a caller that
// also wants to signal failure (a failure glyph, an error line) can branch on
// it. copyWithFallback swallows its own failure and returns false, and the
// copied text is always selectable by hand, so a false result simply skips the
// flash. `reset` clears the indicator immediately, e.g. when a freshly
// generated value replaces the one the button last copied.
export function useCopyFlash(
  ms: number,
): readonly [boolean, (text: string) => Promise<boolean>, () => void] {
  const [copied, flash, reset] = useTimedFlag(ms);

  const copy = useCallback(
    async (text: string) => {
      const ok = await copyWithFallback(text);
      if (ok) flash();
      return ok;
    },
    [flash],
  );

  return [copied, copy, reset];
}
