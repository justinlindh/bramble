import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { useState } from 'react';
import { render, screen, fireEvent } from '@testing-library/react';
import { ErrorBoundary } from '../../src/components/ErrorBoundary';

/**
 * Issue #100: the boundary used to be a dead end. It rendered a heading and a
 * raw error message with no way out, and it never reset on tab change, so a
 * single crash in one tab poisoned the rest of the session.
 */

let shouldThrow = true;

function Boom({ label = 'kaboom' }: { label?: string }) {
  if (shouldThrow) throw new Error(label);
  return <div>recovered content</div>;
}

describe('ErrorBoundary recovery', () => {
  let errorSpy: ReturnType<typeof vi.spyOn>;

  beforeEach(() => {
    shouldThrow = true;
    // React logs the caught error itself; keep the test output readable.
    errorSpy = vi.spyOn(console, 'error').mockImplementation(() => {});
  });

  afterEach(() => {
    errorSpy.mockRestore();
  });

  it('renders the error message plus Try again and Reload actions', () => {
    render(<ErrorBoundary><Boom label="radio exploded" /></ErrorBoundary>);

    expect(screen.getByText('Something went wrong')).toBeInTheDocument();
    expect(screen.getByText('radio exploded')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Reload app' })).toBeInTheDocument();
  });

  it('re-renders the children when Try again is clicked and the cause is gone', () => {
    render(<ErrorBoundary><Boom /></ErrorBoundary>);
    expect(screen.getByText('Something went wrong')).toBeInTheDocument();

    shouldThrow = false;
    fireEvent.click(screen.getByRole('button', { name: 'Try again' }));

    expect(screen.getByText('recovered content')).toBeInTheDocument();
    expect(screen.queryByText('Something went wrong')).not.toBeInTheDocument();
  });

  it('stays tripped when Try again is clicked but the child still throws', () => {
    render(<ErrorBoundary><Boom label="still broken" /></ErrorBoundary>);

    fireEvent.click(screen.getByRole('button', { name: 'Try again' }));

    expect(screen.getByText('Something went wrong')).toBeInTheDocument();
    expect(screen.getByText('still broken')).toBeInTheDocument();
  });

  it('resets when resetKey changes, so a crash in one tab does not stick', () => {
    function Harness() {
      const [tab, setTab] = useState('chat');
      return (
        <>
          <button type="button" onClick={() => setTab('nodes')}>go to nodes</button>
          <ErrorBoundary resetKey={tab}>
            {tab === 'chat' ? <Boom /> : <div>nodes content</div>}
          </ErrorBoundary>
        </>
      );
    }

    render(<Harness />);
    expect(screen.getByText('Something went wrong')).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'go to nodes' }));

    expect(screen.getByText('nodes content')).toBeInTheDocument();
    expect(screen.queryByText('Something went wrong')).not.toBeInTheDocument();
  });

  it('does not reset while resetKey is unchanged', () => {
    const { rerender } = render(
      <ErrorBoundary resetKey="chat"><Boom /></ErrorBoundary>,
    );
    expect(screen.getByText('Something went wrong')).toBeInTheDocument();

    shouldThrow = false;
    rerender(<ErrorBoundary resetKey="chat"><Boom /></ErrorBoundary>);

    expect(screen.getByText('Something went wrong')).toBeInTheDocument();
  });

  it('renders a custom fallback instead of the recovery panel', () => {
    render(
      <ErrorBoundary fallback={<div>custom fallback</div>}><Boom /></ErrorBoundary>,
    );

    expect(screen.getByText('custom fallback')).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Try again' })).not.toBeInTheDocument();
  });

  it('renders children untouched when nothing throws', () => {
    shouldThrow = false;
    render(<ErrorBoundary><Boom /></ErrorBoundary>);
    expect(screen.getByText('recovered content')).toBeInTheDocument();
  });

  it('names the boundary in the console log so crash reports say which tripped', () => {
    render(<ErrorBoundary name="connection-overlay"><Boom /></ErrorBoundary>);

    const logged = errorSpy.mock.calls.some(
      (args: unknown[]) => args[0] === '[ErrorBoundary:connection-overlay]',
    );
    expect(logged).toBe(true);
  });
});
