import { afterEach, describe, expect, it } from 'vitest';
import { cleanup, render, screen } from '@testing-library/react';
import { GnssDot } from './GnssDot';
import { useStore } from '../store/index';

afterEach(cleanup);

function setStatus(status: Record<string, unknown> | null) {
  useStore.setState({ connectionState: 'connected', status } as any);
}

function dot() {
  // The dot is the only element carrying an accessible name.
  return screen.getByLabelText(/GNSS/i);
}

describe('GnssDot', () => {
  it('renders the no-signal state with its satellite counts', () => {
    setStatus({ gpsAvailable: true, gpsState: 'no_signal', gpsSatsInView: 0 });
    render(<GnssDot />);

    expect(dot().className).toMatch(/noSignal/);
    expect(dot()).toHaveAttribute('aria-label', 'GNSS no signal: 0 tracked of 0 in view');
    expect(screen.queryByLabelText(/acquiring/i)).toBeNull();
  });

  it('renders the acquiring state with tracked, in view and best C/N0', () => {
    setStatus({
      gpsAvailable: true,
      gpsState: 'acquiring',
      gpsSatsInView: 12,
      gpsSatsTracked: 7,
      gpsSnrMaxDbHz: 22,
    });
    render(<GnssDot />);

    expect(dot().className).toMatch(/acquiring/);
    const label = dot().getAttribute('aria-label') ?? '';
    expect(label).toContain('7 tracked');
    expect(label).toContain('12 in view');
    expect(label).toContain('22 dBHz');
  });

  it('renders the fix state with the used count', () => {
    setStatus({ gpsAvailable: true, gpsState: 'fix', gpsSatsUsed: 6, gpsSatsInView: 12 });
    render(<GnssDot />);

    expect(dot().className).toMatch(/fix/);
    expect(dot().getAttribute('aria-label')).toContain('6 satellites used');
  });

  it('gives the three failure classes three distinct classes and three distinct names', () => {
    // The whole point of the indicator: collapsing any two of these back
    // together reproduces the single-boolean surface it replaces.
    const seen: { cls: string; name: string; text: string }[] = [];
    const cases = [
      { gpsState: 'no_signal', gpsSatsInView: 0 },
      { gpsState: 'acquiring', gpsSatsInView: 12, gpsSatsTracked: 7, gpsSnrMaxDbHz: 22 },
      { gpsState: 'fix', gpsSatsUsed: 6, gpsSatsInView: 12 },
    ];
    for (const c of cases) {
      setStatus({ gpsAvailable: true, ...c });
      const { container, unmount } = render(<GnssDot />);
      seen.push({
        cls: dot().className,
        name: dot().getAttribute('aria-label') ?? '',
        text: container.textContent ?? '',
      });
      unmount();
    }

    for (let i = 0; i < seen.length; i++) {
      for (let j = i + 1; j < seen.length; j++) {
        expect(seen[i].cls).not.toBe(seen[j].cls);
        expect(seen[i].name).not.toBe(seen[j].name);
        expect(seen[i].text).not.toBe(seen[j].text);
      }
    }
  });

  it('renders the unknown state without inventing counts when the firmware omits them', () => {
    setStatus({ gpsAvailable: true });
    render(<GnssDot />);

    expect(dot().className).toMatch(/unknown/);
    const label = dot().getAttribute('aria-label') ?? '';
    expect(label).toContain('unknown');
    expect(label).not.toMatch(/\d/);
  });

  it('renders nothing on a board without a receiver', () => {
    setStatus({ gpsAvailable: false, gpsState: 'absent', gpsSatsInView: 0 });
    const { container } = render(<GnssDot />);
    expect(container).toBeEmptyDOMElement();
  });

  it('renders nothing when the firmware reports the receiver absent', () => {
    setStatus({ gpsAvailable: true, gpsState: 'absent' });
    const { container } = render(<GnssDot />);
    expect(container).toBeEmptyDOMElement();
  });

  it('renders nothing while disconnected or before the first status arrives', () => {
    useStore.setState({ connectionState: 'disconnected', status: { gpsAvailable: true, gpsState: 'fix' } } as any);
    const disconnected = render(<GnssDot />);
    expect(disconnected.container).toBeEmptyDOMElement();
    disconnected.unmount();

    setStatus(null);
    const { container } = render(<GnssDot />);
    expect(container).toBeEmptyDOMElement();
  });
});
