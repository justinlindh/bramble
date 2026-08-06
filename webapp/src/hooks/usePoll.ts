import { useEffect, useRef } from 'react';

interface PollOptions {
  /**
   * Gates the poll on some external condition (typically connection state).
   * When false, the interval keeps ticking but `fn` is not invoked, so a
   * disconnected view issues no work without the caller wrapping `fn` in a
   * no-op. Defaults to true.
   */
  enabled?: boolean;
}

/**
 * Calls `fn` immediately, then on every `intervalMs` milliseconds, skipping any
 * tick where `options.enabled` is false. Stops polling when the component
 * unmounts.
 */
export function usePoll(
  fn: () => void | Promise<void>,
  intervalMs: number,
  options?: PollOptions,
): void {
  const fnRef = useRef(fn);
  fnRef.current = fn;
  const enabledRef = useRef(options?.enabled ?? true);
  enabledRef.current = options?.enabled ?? true;

  useEffect(() => {
    let active = true;
    let timerId: ReturnType<typeof setTimeout>;

    async function tick() {
      if (!active) return;
      if (enabledRef.current) {
        try {
          await fnRef.current();
        } catch {
          // silently ignore poll errors
        }
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
