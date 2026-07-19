// Oled.tsx
//
// Renders a virtual SSD1306 128x64 monochrome OLED: a backing-store canvas at
// the exact panel resolution, scaled up crisply (image-rendering: pixelated).
// Unlike the e-paper panel (Epaper.tsx), an OLED has no refresh physics: each
// frame is drawn immediately and in full, with no ghosting, no partial-refresh
// accumulation, and no inversion flash. So this component simply decodes the
// latest firmware framebuffer and paints it once per new frame (keyed by seq).
//
// It renders the ACTUAL firmware framebuffer streamed over emu-link (the same
// bytes the real render_screen() draws into via display_draw_text), not a UI
// reimagining of the screen. A set bit is a lit pixel (foreground); the panel
// glows near-white on a near-black substrate, matching a real Heltec OLED.

import { useEffect, useRef } from 'react';
import { unpackFramebufferSized } from './framebuffer';

export interface OledProps {
  fb: string | null;
  seq: number; // bump to apply a new frame
  width: number; // panel pixels (128 for SSD1306)
  height: number; // panel pixels (64 for SSD1306)
  // rendered size in CSS px (the backing store is always width x height)
  displayWidth: number;
  displayHeight: number;
}

// Lit pixel vs unlit substrate. Real Heltec OLEDs are a cool white/blue on a
// near-black glass; these read as an OLED and never as e-paper.
const LIT: readonly [number, number, number] = [0xe6, 0xf1, 0xff];
const OFF: readonly [number, number, number] = [0x06, 0x0a, 0x10];

export default function Oled({ fb, seq, width, height, displayWidth, displayHeight }: OledProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const scratchRef = useRef<Uint8ClampedArray | undefined>(undefined);

  useEffect(() => {
    if (seq <= 0 || !fb) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const rgba = unpackFramebufferSized(fb, width, height, LIT, OFF, scratchRef.current);
    if (!rgba) return;
    scratchRef.current = rgba;

    const img = ctx.createImageData(width, height);
    img.data.set(rgba);
    ctx.putImageData(img, 0, 0);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [seq]);

  return (
    <canvas
      ref={canvasRef}
      className="oled-canvas"
      width={width}
      height={height}
      style={{
        width: displayWidth,
        height: displayHeight,
        imageRendering: 'pixelated',
        display: 'block',
      }}
      data-testid="oled-canvas"
    />
  );
}
