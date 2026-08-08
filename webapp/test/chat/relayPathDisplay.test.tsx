import { describe, expect, it } from 'vitest';
import { render, screen } from '@testing-library/react';
import { RelayPathDisplay } from '../../src/pages/Chat/RelayPathDisplay';
import styles from '../../src/pages/Chat/RelayPathDisplay.module.css';

describe('RelayPathDisplay', () => {

  it('shows RSSI values in dBm with quality color classes', () => {
    const path = [
      { addr: 0x1111, rssi: -65 },
      { addr: 0x2222, rssi: -80 },
      { addr: 0x3333, rssi: -95 },
    ];

    render(<RelayPathDisplay path={path} />);

    const strong = screen.getByText('-65 dBm');
    const fair = screen.getByText('-80 dBm');
    const weak = screen.getByText('-95 dBm');

    expect(strong).toHaveClass(styles.rssi, styles.rssiGood);
    expect(fair).toHaveClass(styles.rssi, styles.rssiFair);
    expect(weak).toHaveClass(styles.rssi, styles.rssiPoor);
  });
});
