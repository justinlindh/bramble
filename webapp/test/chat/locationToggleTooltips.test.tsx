import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { ShareLocationToggle } from '../../src/pages/Chat/ShareLocationButton';

describe('ShareLocationToggle privacy tooltips', () => {
  it('uses clear privacy-tier wording for each option', () => {
    render(<ShareLocationToggle value="off" onChange={() => {}} />);

    expect(screen.getByRole('button', { name: 'Off' })).toHaveAttribute('title', 'Off: no location shared');
    expect(screen.getByRole('button', { name: 'Zone' })).toHaveAttribute('title', 'Coarse: grid square (~1km zone)');
    expect(screen.getByRole('button', { name: 'Exact' })).toHaveAttribute('title', 'Exact: precise GPS coordinates');
  });
});
