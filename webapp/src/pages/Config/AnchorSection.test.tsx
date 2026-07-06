import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';

// Mock the store actions (the only RPC path) so we can assert exactly what the
// component sends. The crypto (utils/anchor) and codecs (utils/anchorShare) are
// the REAL modules: these tests exercise the actual signing/encoding. The
// custody guard at the RPC-payload level lives in AnchorSection.custody.test.tsx
// (real actions + a spied transport); here we additionally assert the seed is
// never handed to any store action.
const setAnchor = vi.fn<(pub: string) => Promise<boolean>>();
const getAnchorStatus = vi.fn();
const getIdentity = vi.fn<() => Promise<{ address: string; pubkey_hash: string; ed25519_pub: string }>>();
const setEndorsement = vi.fn<(na: string, sig: string) => Promise<boolean>>();
const loadAnchorStatus = vi.fn<() => Promise<void>>();

vi.mock('../../store/actions', () => ({
  setAnchor: (pub: string) => setAnchor(pub),
  getAnchorStatus: () => getAnchorStatus(),
  getIdentity: () => getIdentity(),
  setEndorsement: (na: string, sig: string) => setEndorsement(na, sig),
  loadAnchorStatus: () => loadAnchorStatus(),
}));

import { AnchorSection } from './AnchorSection';
import { useStore } from '../../store/index';
import { anchorPubFromSeed, anchorFingerprint, PERMANENT_NOT_AFTER, signEndorsement } from '../../utils/anchor';
import { encodeAnchorBackup, encodeIdentityShare, encodeCertShare } from '../../utils/anchorShare';

const SEED_KEY = 'bramble.anchor.seed';
// KAT vectors (byte-identical to the firmware) so signing assertions are exact.
const KAT_ANCHOR_SEED = '000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f';
const KAT_NODE_PUB = '404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f';
const KAT_SIG_PERMANENT =
  '016e65eae269ec3b1465252b33d526c1d9157d39dfff5f9009c71bb6118a85b3' +
  '7a36afd28bc2f36869f2bba54b601c79cc81213dcc2c41b76ec32ab74740b903';

const KAT_ANCHOR_PUB = anchorPubFromSeed(KAT_ANCHOR_SEED);
const KAT_ANCHOR_FP = anchorFingerprint(KAT_ANCHOR_PUB);

afterEach(cleanup);

beforeEach(() => {
  localStorage.clear();
  vi.clearAllMocks();
  setAnchor.mockResolvedValue(true);
  setEndorsement.mockResolvedValue(true);
  getIdentity.mockResolvedValue({ address: '00001234', pubkey_hash: 'abcd', ed25519_pub: KAT_NODE_PUB });
  loadAnchorStatus.mockResolvedValue(undefined);
  useStore.setState({ anchorStatus: null } as never);
});

/** Seed localStorage so the component mounts with an anchor already held. */
function seedClientAnchor(seedHex = KAT_ANCHOR_SEED) {
  localStorage.setItem(SEED_KEY, seedHex);
}

describe('AnchorSection generate + mandatory backup gate', () => {
  it('does NOT persist the seed until the operator confirms the backup', () => {
    render(<AnchorSection />);
    fireEvent.click(screen.getByRole('button', { name: 'Generate anchor' }));

    // Backup is shown, but nothing is persisted yet: the gate is closed.
    const backup = screen.getByLabelText('Anchor backup string') as HTMLInputElement;
    expect(backup.value).toMatch(/^bramble:\/\/anchor\/v1\?sk=[0-9a-f]{64}$/);
    expect(localStorage.getItem(SEED_KEY)).toBeNull();
    expect(screen.getByRole('button', { name: 'I have saved this backup' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'I have saved this backup' }));

    // Only now is it persisted, and the held-anchor UI appears.
    expect(localStorage.getItem(SEED_KEY)).toMatch(/^[0-9a-f]{64}$/);
    expect(screen.getByText(/Anchor held/)).toBeInTheDocument();
  });

  it('discards the anchor on cancel without persisting', () => {
    render(<AnchorSection />);
    fireEvent.click(screen.getByRole('button', { name: 'Generate anchor' }));
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

    expect(localStorage.getItem(SEED_KEY)).toBeNull();
    expect(screen.getByRole('button', { name: 'Generate anchor' })).toBeInTheDocument();
  });
});

describe('AnchorSection import', () => {
  it('restores an anchor from a backup string and shows its fingerprint', () => {
    render(<AnchorSection />);
    fireEvent.change(screen.getByLabelText('Anchor backup to import'), {
      target: { value: encodeAnchorBackup(KAT_ANCHOR_SEED) },
    });
    fireEvent.click(screen.getByRole('button', { name: 'Import anchor backup' }));

    expect(localStorage.getItem(SEED_KEY)).toBe(KAT_ANCHOR_SEED);
    expect(screen.getByText(/Anchor held/)).toBeInTheDocument();
    expect(screen.getByText(KAT_ANCHOR_FP)).toBeInTheDocument();
  });

  it('rejects a malformed backup string', () => {
    render(<AnchorSection />);
    fireEvent.change(screen.getByLabelText('Anchor backup to import'), { target: { value: 'not-a-backup' } });
    fireEvent.click(screen.getByRole('button', { name: 'Import anchor backup' }));
    expect(localStorage.getItem(SEED_KEY)).toBeNull();
    expect(screen.getByText(/Enter a bramble:\/\/anchor/)).toBeInTheDocument();
  });
});

describe('AnchorSection provision', () => {
  it('provisions the anchor PUBLIC key (never the seed) to the node', async () => {
    seedClientAnchor();
    render(<AnchorSection />);
    fireEvent.click(screen.getByRole('button', { name: 'Provision anchor to this node' }));

    await waitFor(() => expect(setAnchor).toHaveBeenCalledWith(KAT_ANCHOR_PUB));
    expect(loadAnchorStatus).toHaveBeenCalled();
    expect(await screen.findByText('Anchor provisioned to this node.')).toBeInTheDocument();
  });
});

describe('AnchorSection local enrollment', () => {
  it('reads identity, signs a permanent cert, and applies it', async () => {
    seedClientAnchor();
    // Node is anchored to the SAME fingerprint, so enrollment is not blocked.
    useStore.setState({ anchorStatus: { anchored: true, anchor_fingerprint: KAT_ANCHOR_FP, endorsed: false } } as never);
    // Reflect endorsed=true once the cert is applied.
    loadAnchorStatus.mockImplementation(async () => {
      useStore.setState({ anchorStatus: { anchored: true, anchor_fingerprint: KAT_ANCHOR_FP, endorsed: true } } as never);
    });

    render(<AnchorSection />);
    fireEvent.click(screen.getByRole('button', { name: 'Enroll this node' }));

    await waitFor(() => expect(getIdentity).toHaveBeenCalled());
    // The cert sent is exactly the KAT signature: permanent not_after + sig.
    expect(setEndorsement).toHaveBeenCalledWith('ffffffffffffffff', KAT_SIG_PERMANENT);
    expect(await screen.findByText(/This node is enrolled/)).toBeInTheDocument();
    expect(await screen.findByText(/Endorsed \(enrolled\)/)).toBeInTheDocument();
  });

  it('warns and disables enroll when the node is anchored to a different fingerprint', () => {
    seedClientAnchor();
    useStore.setState({ anchorStatus: { anchored: true, anchor_fingerprint: 'deadbeef', endorsed: false } } as never);
    render(<AnchorSection />);

    expect(screen.getByText(/MISMATCH/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Enroll this node' })).toBeDisabled();
  });
});

describe('AnchorSection remote enrollment', () => {
  it('signs a cert for a pasted identity share without any RPC', () => {
    seedClientAnchor();
    render(<AnchorSection />);
    fireEvent.change(screen.getByLabelText('Remote node identity share'), {
      target: { value: encodeIdentityShare(KAT_NODE_PUB) },
    });
    fireEvent.click(screen.getByRole('button', { name: 'Sign cert' }));

    const { sigHex } = signEndorsement(KAT_ANCHOR_SEED, KAT_NODE_PUB, PERMANENT_NOT_AFTER);
    const cert = screen.getByLabelText('Signed endorsement cert') as HTMLInputElement;
    expect(cert.value).toBe(encodeCertShare('ffffffffffffffff', sigHex));
    // Remote signing must not talk to any node.
    expect(setAnchor).not.toHaveBeenCalled();
    expect(setEndorsement).not.toHaveBeenCalled();
  });
});

describe('AnchorSection node-side affordances', () => {
  it('shows this node identity as a share string', async () => {
    render(<AnchorSection />);
    fireEvent.click(screen.getByRole('button', { name: 'Show my identity' }));
    await waitFor(() => expect(getIdentity).toHaveBeenCalled());
    const share = (await screen.findByLabelText('This node identity share')) as HTMLInputElement;
    expect(share.value).toBe(encodeIdentityShare(KAT_NODE_PUB));
  });

  it('applies a pasted endorsement cert', async () => {
    render(<AnchorSection />);
    fireEvent.change(screen.getByLabelText('Endorsement cert to apply'), {
      target: { value: encodeCertShare('ffffffffffffffff', KAT_SIG_PERMANENT) },
    });
    fireEvent.click(screen.getByRole('button', { name: 'Apply endorsement' }));

    await waitFor(() => expect(setEndorsement).toHaveBeenCalledWith('ffffffffffffffff', KAT_SIG_PERMANENT));
    expect(await screen.findByText(/Endorsement applied/)).toBeInTheDocument();
  });
});

describe('AnchorSection custody (component level)', () => {
  it('never hands the anchor seed to any store action across generate/provision/enroll', async () => {
    useStore.setState({ anchorStatus: null } as never);
    render(<AnchorSection />);

    // Generate + confirm a fresh anchor.
    fireEvent.click(screen.getByRole('button', { name: 'Generate anchor' }));
    fireEvent.click(screen.getByRole('button', { name: 'I have saved this backup' }));
    const seed = localStorage.getItem(SEED_KEY)!;
    expect(seed).toMatch(/^[0-9a-f]{64}$/);

    // Provision, then enroll (node reports the matching fingerprint).
    const fp = anchorFingerprint(anchorPubFromSeed(seed));
    useStore.setState({ anchorStatus: { anchored: true, anchor_fingerprint: fp, endorsed: false } } as never);
    fireEvent.click(screen.getByRole('button', { name: 'Provision anchor to this node' }));
    await waitFor(() => expect(setAnchor).toHaveBeenCalled());
    fireEvent.click(screen.getByRole('button', { name: 'Enroll this node' }));
    await waitFor(() => expect(setEndorsement).toHaveBeenCalled());

    // The seed must appear in NO argument of ANY store action.
    const allArgs = JSON.stringify([
      ...setAnchor.mock.calls,
      ...setEndorsement.mock.calls,
      ...getIdentity.mock.calls,
      ...getAnchorStatus.mock.calls,
      ...loadAnchorStatus.mock.calls,
    ]);
    expect(allArgs).not.toContain(seed);
    // But the public key WAS sent, confirming the flow really ran.
    expect(setAnchor).toHaveBeenCalledWith(anchorPubFromSeed(seed));
  });
});
