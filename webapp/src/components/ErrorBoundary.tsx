import { Component, type ErrorInfo, type ReactNode } from 'react';

interface Props {
  children: ReactNode;
  /**
   * Renders instead of the default recovery panel. A custom fallback opts out
   * of the built-in "Try again" action, so only pass one that offers its own
   * way out.
   */
  fallback?: ReactNode;
  /**
   * Changing this value clears a tripped boundary. App passes the active tab,
   * so one crash in Chat no longer leaves the boundary stuck for the rest of
   * the session: navigating away and back remounts the subtree.
   */
  resetKey?: unknown;
  /** Labels the boundary in console output so a crash says which one tripped. */
  name?: string;
}

interface State {
  hasError: boolean;
  error?: Error;
}

export class ErrorBoundary extends Component<Props, State> {
  constructor(props: Props) {
    super(props);
    this.state = { hasError: false };
  }

  static getDerivedStateFromError(error: Error): State {
    return { hasError: true, error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    const label = this.props.name ? `[ErrorBoundary:${this.props.name}]` : '[ErrorBoundary]';
    console.error(label, error, info);
  }

  componentDidUpdate(prevProps: Props): void {
    if (this.state.hasError && prevProps.resetKey !== this.props.resetKey) {
      this.reset();
    }
  }

  reset = (): void => {
    this.setState({ hasError: false, error: undefined });
  };

  handleReload = (): void => {
    window.location.reload();
  };

  render(): ReactNode {
    if (this.state.hasError) {
      if (this.props.fallback) return this.props.fallback;
      return (
        <div style={{ padding: '2rem', color: 'var(--danger)' }} role="alert">
          <h2>Something went wrong</h2>
          <pre style={{ marginTop: '0.5rem', fontSize: '0.8rem', opacity: 0.7, whiteSpace: 'pre-wrap' }}>
            {this.state.error?.message}
          </pre>
          <div style={{ marginTop: '1rem', display: 'flex', gap: '0.5rem', flexWrap: 'wrap' }}>
            <button type="button" onClick={this.reset}>Try again</button>
            <button type="button" onClick={this.handleReload}>Reload app</button>
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}
