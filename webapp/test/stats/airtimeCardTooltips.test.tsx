import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { AirtimeCard } from '../../src/pages/Stats/AirtimeCard';
import type { AirtimeStatus } from '../../src/types/bramble';

const airtime: AirtimeStatus = {
  tiers: [
    { name: 'critical', remainingMs: 36000, maxMs: 36000, usedPct: 0, refillAtMs: Date.now() + 60_000 },
    { name: 'normal', remainingMs: 18000, maxMs: 18000, usedPct: 0, refillAtMs: Date.now() + 60_000 },
    { name: 'broadcast', remainingMs: 18000, maxMs: 18000, usedPct: 0, refillAtMs: Date.now() + 60_000 },
  ],
};

describe('AirtimeCard tier tooltips', () => {
  it('shows concise traffic descriptions for each tier name', () => {
    render(<AirtimeCard airtime={airtime} />);

    expect(screen.getByText('Critical')).toHaveAttribute(
      'title',
      'Critical: reliable delivery for emergency alerts and SOS location traffic'
    );
    expect(screen.getByText('Normal')).toHaveAttribute(
      'title',
      'Normal: acknowledged direct messages and peer data'
    );
    expect(screen.getByText('Broadcast')).toHaveAttribute(
      'title',
      'Broadcast: fire-and-forget channel messages and network announcements'
    );
  });
});
