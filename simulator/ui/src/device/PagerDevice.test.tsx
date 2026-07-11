import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, fireEvent, cleanup } from '@testing-library/react';
import PagerDevice from './PagerDevice';
import { FB_BYTES, FB_WIDTH, FB_HEIGHT } from './framebuffer';
import type { DeviceState } from '../types';

// jsdom has no canvas backend; install a fake 2d context that records paints.
let fakeCtx: {
  fillRect: ReturnType<typeof vi.fn>;
  putImageData: ReturnType<typeof vi.fn>;
  drawImage: ReturnType<typeof vi.fn>;
  createImageData: (w: number, h: number) => { data: Uint8ClampedArray; width: number; height: number };
  save: ReturnType<typeof vi.fn>;
  restore: ReturnType<typeof vi.fn>;
  fillStyle: string;
  globalAlpha: number;
};

beforeEach(() => {
  fakeCtx = {
    fillRect: vi.fn(),
    putImageData: vi.fn(),
    drawImage: vi.fn(),
    createImageData: (w: number, h: number) => ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }),
    save: vi.fn(),
    restore: vi.fn(),
    fillStyle: '',
    globalAlpha: 1,
  };
  // @ts-expect-error test stub
  HTMLCanvasElement.prototype.getContext = vi.fn(() => fakeCtx);
});

afterEach(() => cleanup());

function makeDevice(over: Partial<DeviceState> = {}): DeviceState {
  return {
    node: 'AB12',
    addr: '0x0000AB12',
    fb: null,
    fbKind: 'full',
    fbBusyMs: 0,
    fbSeq: 0,
    led: false,
    buzzerHz: 0,
    vibra: false,
    vibraSeq: 0,
    console: [],
    ...over,
  };
}

function zeroFrameBase64(): string {
  const bytes = new Uint8Array(FB_BYTES);
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}

describe('PagerDevice buttons', () => {
  it('emits a down edge on press and an up edge on release for SELECT', () => {
    const onButton = vi.fn();
    const { getByTestId } = render(
      <PagerDevice device={makeDevice()} muted onButton={onButton} />,
    );
    const sel = getByTestId('btn-select');
    fireEvent.mouseDown(sel);
    expect(onButton).toHaveBeenCalledWith('select', 'down');
    fireEvent.mouseUp(sel);
    expect(onButton).toHaveBeenCalledWith('select', 'up');
  });

  it('emits the correct id for UP and DOWN buttons', () => {
    const onButton = vi.fn();
    const { getByTestId } = render(
      <PagerDevice device={makeDevice()} muted onButton={onButton} />,
    );
    fireEvent.mouseDown(getByTestId('btn-up'));
    fireEvent.mouseDown(getByTestId('btn-down'));
    expect(onButton).toHaveBeenCalledWith('up', 'down');
    expect(onButton).toHaveBeenCalledWith('down', 'down');
  });
});

describe('PagerDevice e-paper', () => {
  it('paints the framebuffer onto the canvas when a frame arrives', () => {
    const onButton = vi.fn();
    // seq 0 -> no paint yet
    const { rerender } = render(
      <PagerDevice device={makeDevice({ fb: null, fbSeq: 0 })} muted onButton={onButton} />,
    );
    expect(fakeCtx.putImageData).not.toHaveBeenCalled();

    // an fb event bumps fbSeq with content -> canvas updates. A partial frame
    // with busy 0 latches content immediately; a full frame plays the
    // inversion flicker first and lands content a few hundred ms later (that
    // timing is covered by epaperModel.test.ts), so a partial is the right
    // case for "content paints on arrival".
    rerender(
      <PagerDevice
        device={makeDevice({ fb: zeroFrameBase64(), fbKind: 'partial', fbBusyMs: 0, fbSeq: 1 })}
        muted
        onButton={onButton}
      />,
    );
    expect(fakeCtx.putImageData).toHaveBeenCalled();
    const img = fakeCtx.putImageData.mock.calls[0][0] as { data: Uint8ClampedArray };
    expect(img.data.length).toBe(FB_WIDTH * FB_HEIGHT * 4);
  });
});
