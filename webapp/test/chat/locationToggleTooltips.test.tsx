import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { ShareLocationToggle } from '../../src/pages/Chat/ShareLocationButton';

describe('ShareLocationToggle privacy tooltips', () => {
  it('uses clear privacy-tier wording for each option', () => {
    render(<ShareLocationToggle value="off" onChange={() => {}} />);

    expect(screen.getByRole('button', { name: 'Off' })).toHaveAttribute('title', 'Off: no location shared');
    expect(screen.getByRole('button', { name: 'Exact' })).toHaveAttribute('title', 'Exact: precise GPS coordinates');

    // The zone tooltip is the one number a user weighs before sharing, so
    // assert it names the real cell rather than the "~1km" the tier carried
    // for a while, which overstated the fuzzing by about three times.
    const zoneTitle = screen.getByRole('button', { name: 'Zone' }).getAttribute('title') ?? '';
    expect(zoneTitle).toMatch(/^Coarse:/);
    expect(zoneTitle).toContain('330 m');
    expect(zoneTitle).toContain('670 m');
    expect(zoneTitle).not.toMatch(/1 ?km/i);
  });
});
