// Epaper.tsx
//
// Renders a virtual GDEY0213B74 panel: a 250x122 backing-store canvas scaled up
// crisply (image-rendering: pixelated) and driven through the modeled refresh
// physics in epaperModel.ts. Each incoming framebuffer (identified by `seq`) is
// turned into a paint schedule; partial refreshes accumulate a faint ghost of
// the prior image, full refreshes play the inversion flash and scrub it.

import { useEffect, useRef } from 'react';
import { applyFrame, initialEpaperState, type CanvasFrame, type EpaperState } from './epaperModel';
import { unpackFramebuffer, FB_WIDTH, FB_HEIGHT } from './framebuffer';

export interface EpaperProps {
  fb: string | null;
  kind: 'partial' | 'full';
  busyMs: number;
  seq: number; // bump to apply a new frame
  // display size in CSS px (the backing store is always FB_WIDTH x FB_HEIGHT)
  displayWidth: number;
  displayHeight: number;
}

const FLASH_FILL: Record<'black' | 'white', string> = {
  black: '#111310',
  white: '#dddcd2',
};

export default function Epaper({ fb, kind, busyMs, seq, displayWidth, displayHeight }: EpaperProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const ghostCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const stateRef = useRef<EpaperState>(initialEpaperState());
  const prevContentRef = useRef<Uint8ClampedArray | null>(null);
  const scratchRef = useRef<Uint8ClampedArray | undefined>(undefined);

  function paint(frame: CanvasFrame) {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    if (frame.paint === 'black' || frame.paint === 'white') {
      ctx.fillStyle = FLASH_FILL[frame.paint];
      ctx.fillRect(0, 0, FB_WIDTH, FB_HEIGHT);
      return;
    }

    // content
    if (!fb) return;
    const rgba = unpackFramebuffer(fb, scratchRef.current);
    if (!rgba) return;
    scratchRef.current = rgba;

    const img = ctx.createImageData(FB_WIDTH, FB_HEIGHT);
    img.data.set(rgba);
    ctx.putImageData(img, 0, 0);

    // Ghost residue: overlay the previous image faintly on top of the new one.
    const prev = prevContentRef.current;
    if (frame.ghost > 0 && prev) {
      let g = ghostCanvasRef.current;
      if (!g) {
        g = document.createElement('canvas');
        g.width = FB_WIDTH;
        g.height = FB_HEIGHT;
        ghostCanvasRef.current = g;
      }
      const gctx = g.getContext('2d');
      if (gctx) {
        const gimg = gctx.createImageData(FB_WIDTH, FB_HEIGHT);
        gimg.data.set(prev);
        gctx.putImageData(gimg, 0, 0);
        ctx.save();
        ctx.globalAlpha = frame.ghost;
        ctx.drawImage(g, 0, 0);
        ctx.restore();
      }
    }
    // Remember this frame as the residue source for the next partial.
    prevContentRef.current = rgba.slice();
  }

  // Apply each new frame (by seq) through the physics model.
  useEffect(() => {
    if (seq <= 0) return;
    const { frames, ghost } = applyFrame(stateRef.current, fb, kind, busyMs);
    stateRef.current = { ghost };
    const timers: ReturnType<typeof setTimeout>[] = [];
    for (const frame of frames) {
      if (frame.at <= 0) {
        paint(frame);
      } else {
        timers.push(setTimeout(() => paint(frame), frame.at));
      }
    }
    return () => {
      for (const t of timers) clearTimeout(t);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [seq]);

  return (
    <canvas
      ref={canvasRef}
      className="epaper-canvas"
      width={FB_WIDTH}
      height={FB_HEIGHT}
      style={{
        width: displayWidth,
        height: displayHeight,
        imageRendering: 'pixelated',
        display: 'block',
      }}
      data-testid="epaper-canvas"
    />
  );
}
