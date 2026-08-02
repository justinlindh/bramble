import { describe, it, expect, vi, afterEach } from 'vitest';
import { copyWithFallback } from '../clipboard';

const originalExec = (document as { execCommand?: unknown }).execCommand;

afterEach(() => {
  vi.unstubAllGlobals();
  (document as { execCommand?: unknown }).execCommand = originalExec;
});

function stubExecCommand(result: boolean): ReturnType<typeof vi.fn> {
  const execCommand = vi.fn().mockReturnValue(result);
  (document as { execCommand?: unknown }).execCommand = execCommand;
  return execCommand;
}

describe('copyWithFallback', () => {
  it('uses the async Clipboard API when available', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const ok = await copyWithFallback('hello');
    expect(ok).toBe(true);
    expect(writeText).toHaveBeenCalledWith('hello');
  });

  it('falls back to execCommand when the Clipboard API rejects', async () => {
    const writeText = vi.fn().mockRejectedValue(new Error('denied'));
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const execCommand = stubExecCommand(true);

    const ok = await copyWithFallback('hello');
    expect(ok).toBe(true);
    expect(execCommand).toHaveBeenCalledWith('copy');
  });

  it('falls back to execCommand when the Clipboard API is absent', async () => {
    vi.stubGlobal('navigator', {});
    const execCommand = stubExecCommand(true);

    const ok = await copyWithFallback('hello');
    expect(ok).toBe(true);
    expect(execCommand).toHaveBeenCalledWith('copy');
  });

  it('returns false when both paths fail', async () => {
    vi.stubGlobal('navigator', {});
    stubExecCommand(false);
    const ok = await copyWithFallback('hello');
    expect(ok).toBe(false);
  });
});
