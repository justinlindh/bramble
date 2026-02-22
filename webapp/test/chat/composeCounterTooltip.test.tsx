import { describe, it, expect, beforeEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { ComposeBar } from '../../src/pages/Chat/ComposeBar';
import { useStore } from '../../src/store/index';

describe('ComposeBar counter tooltip', () => {
  beforeEach(() => {
    useStore.setState({ connectionState: 'connected', config: { location: { enabled: false } } as any });
  });

  it('shows fragmentation tooltip on byte counter', () => {
    render(<ComposeBar conversationId="broadcast" />);

    const input = screen.getByLabelText('Message input');
    fireEvent.change(input, { target: { value: 'hello world' } });

    const counter = screen.getByLabelText('Message size and fragmentation info');
    expect(counter).toBeInTheDocument();
    expect(counter).toHaveAttribute('title');
    expect(counter.getAttribute('title')).toContain('split into multiple LoRa packets');
  });
});
