// config.ts
//
// Whether this page should run the guided tour.
//
// Two sources, in precedence order:
//   1. a "tour" query parameter, which forces the tour on ("1"/"true"/"") or
//      off ("0"/"false") regardless of the server. This is how a running
//      simulator UI is turned into a playground without restarting gosim, and
//      how the e2e suite drives the tour against the shared stack.
//   2. GET /api/ui-config, which reports what the gosim process was started
//      for: `gosim --playground` (make playground) answers tour:true, an
//      ordinary `make run` answers tour:false.
//
// The fetch failing is not an error worth surfacing: an older broker with no
// /api/ui-config route simply means no tour, which is the same answer it would
// have given.

import { useEffect, useState } from 'react';

export interface UiConfig {
  tour: boolean;
  scenario: string;
}

export const DEFAULT_UI_CONFIG: UiConfig = { tour: false, scenario: '' };

// tourOverrideFromSearch reads the query string's "tour" parameter. Returns
// null when the parameter is absent, so the caller can fall through to the
// server's answer instead of treating "absent" as "off".
export function tourOverrideFromSearch(search: string): boolean | null {
  const params = new URLSearchParams(search);
  if (!params.has('tour')) return null;
  const raw = (params.get('tour') ?? '').toLowerCase();
  if (raw === '0' || raw === 'false' || raw === 'off') return false;
  return true;
}

export async function fetchUiConfig(): Promise<UiConfig> {
  try {
    const res = await fetch('/api/ui-config');
    if (!res.ok) return DEFAULT_UI_CONFIG;
    const body = (await res.json()) as Partial<UiConfig>;
    return {
      tour: body.tour === true,
      scenario: typeof body.scenario === 'string' ? body.scenario : '',
    };
  } catch {
    return DEFAULT_UI_CONFIG;
  }
}

// useTourEnabled resolves the two sources above once on mount. It reports
// false until the answer is known, so the overlay never flashes onto a page
// that turns out not to want it.
export function useTourEnabled(): boolean {
  const [enabled, setEnabled] = useState(false);

  useEffect(() => {
    const override = tourOverrideFromSearch(window.location.search);
    if (override !== null) {
      setEnabled(override);
      return;
    }
    let live = true;
    fetchUiConfig().then((cfg) => {
      if (live) setEnabled(cfg.tour);
    });
    return () => {
      live = false;
    };
  }, []);

  return enabled;
}
