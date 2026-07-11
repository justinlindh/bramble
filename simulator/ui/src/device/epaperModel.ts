// epaperModel.ts
//
// Modeled e-paper physics for the virtual pager display (Bramble emulator,
// DESIGN.md section 9). The firmware node streams packed 1bpp framebuffers over
// emu-link tagged "partial" or "full"; this module turns each frame into a
// schedule of canvas paints that mimics how a real GDEY0213B74 SSD1680 panel
// updates:
//
//   - partial refresh: the new image latches after the panel's busy window with
//     NO inversion flash. Each partial leaves a faint residue of the prior image
//     (ghosting) that accumulates until a full refresh clears it.
//   - full refresh: the controller drives the panel through an inversion flash
//     (black then white) before settling on the new content, which scrubs the
//     accumulated ghost back to zero.
//
// The tunables below are seeded from the datasheet and are meant to be corrected
// against real panels later; they all live in the single EPD_MODEL table so a
// calibration pass touches one place.

export type FlashColor = 'black' | 'white';

// What a scheduled canvas paint draws: a solid inversion flash, or the decoded
// framebuffer content (optionally overlaid with a ghost residue).
export type FramePaint = FlashColor | 'content';

export interface EpdModel {
  // Ghost residue added by each partial refresh (alpha of the residual image).
  ghostPerFrame: number;
  // Hard cap on accumulated ghost before a full refresh is really needed.
  ghostMax: number;
  // Inversion-flash colors played, in order, at the start of a full refresh.
  // Content is drawn as the final frame after these.
  flashColors: FlashColor[];
  // Typical panel busy windows (ms). The live busy_ms from the engine overrides
  // these per frame; they are here so callers have a sane default and tests /
  // previews have representative timings.
  defaultPartialBusyMs: number;
  defaultFullBusyMs: number;
}

// GDEY0213B74 (2.13" SSD1680) seed values. Ghost accumulation and the flash
// timing are approximations to be tuned against physical panels.
export const EPD_MODEL: EpdModel = {
  ghostPerFrame: 0.06,
  ghostMax: 0.3,
  flashColors: ['black', 'white'],
  defaultPartialBusyMs: 300,
  defaultFullBusyMs: 2600,
};

// One scheduled paint. `at` is a millisecond offset from the moment the frame
// was applied; the renderer arms a timer per frame.
export interface CanvasFrame {
  at: number;
  paint: FramePaint;
  // Residual-image alpha to overlay when painting content (0 for flashes and
  // for the settled image right after a full refresh).
  ghost: number;
}

export interface EpaperState {
  ghost: number;
}

export interface ApplyResult {
  frames: CanvasFrame[];
  ghost: number;
}

export function initialEpaperState(): EpaperState {
  return { ghost: 0 };
}

// applyFrame turns an incoming framebuffer update into a paint schedule and the
// resulting ghost state. `fb` (the base64 payload) is opaque to the model; only
// `kind` and `busyMs` drive the schedule. See DESIGN.md section 9.
export function applyFrame(
  state: EpaperState,
  _fb: string | null,
  kind: 'partial' | 'full',
  busyMs: number,
  model: EpdModel = EPD_MODEL,
): ApplyResult {
  const busy = Math.max(0, busyMs);

  if (kind === 'full') {
    // Inversion flash spread evenly across the busy window, content at the end.
    const flashes = model.flashColors;
    const steps = flashes.length; // flashes occupy [0, busy); content lands at busy
    const frames: CanvasFrame[] = flashes.map((color, i) => ({
      at: steps === 0 ? 0 : (busy * i) / steps,
      paint: color,
      ghost: 0,
    }));
    frames.push({ at: busy, paint: 'content', ghost: 0 });
    return { frames, ghost: 0 };
  }

  // partial: no flash, single content latch after the busy window, with an
  // incremented (capped) ghost residue.
  const ghost = Math.min(state.ghost + model.ghostPerFrame, model.ghostMax);
  return {
    frames: [{ at: busy, paint: 'content', ghost }],
    ghost,
  };
}
