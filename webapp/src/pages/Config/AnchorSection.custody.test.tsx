import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';

// The load-bearing custody guard: drive the WHOLE enrollment ceremony through
// the real UI + real store actions, with only the transport mocked, and prove
// the anchor SECRET seed never appears in any RPC payload. This mirrors the
// transport-mock setup in store/__tests__/anchor.test.ts (the client is created
// inside connect() from the mocked transport, so we drive a real connect()).
const rpcMock = vi.fn<(method: string, params?: Record<string, unknown>, timeoutMs?: number) => Promise<any>>();
const transportConnectMock = vi.fn(async () => {});
const transportDisconnectMock = vi.fn(async () => {});

vi.mock('../../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn(() => () => {});
    clearSubscriptions = vi.fn();
    disconnect = vi.fn(async () => {});
    rpc = vi.fn((method: string, params?: Record<string, unknown>, timeoutMs?: number) => rpcMock(method, params, timeoutMs));
    constructor(_transport: unknown) {}
  }
  return {
    createTransport: vi.fn(() => ({
      connect: transportConnectMock,
      disconnect: transportDisconnectMock,
      connected: true,
      onNotification: vi.fn(),
      sendRPC: vi.fn(),
    })),
    BrambleClient: MockBrambleClient,
  };
});

vi.mock('../../store/messageDb', () => ({
  messageDb: {
    open: vi.fn(async () => {}),
    getMessages: vi.fn(async () => []),
    saveMessages: vi.fn(async () => {}),
    clearAll: vi.fn(async () => {}),
  },
}));

vi.mock('../../store/deliveryEventStore', () => ({
  deliveryEventStore: {
    open: vi.fn(async () => {}),
    pruneOldEvents: vi.fn(async () => {}),
    listByMessage: vi.fn(async () => []),
    upsertDeliveryEvents: vi.fn(async () => {}),
  },
}));

const SEED_KEY = 'bramble.anchor.seed';
const KAT_NODE_PUB = '404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f';

// A node that "accepts" whatever anchor we provision: it echoes the fingerprint
// of the last-provisioned anchor pubkey, so the client's mismatch guard passes
// and local enrollment proceeds (exactly what a real just-provisioned node does).
let provisionedFingerprint: string | undefined;

async function fpOf(anchorPubHex: string): Promise<string> {
  const { anchorFingerprint } = await import('../../utils/anchor');
  return anchorFingerprint(anchorPubHex);
}

afterEach(cleanup);

beforeEach(() => {
  localStorage.clear();
  vi.clearAllMocks();
  vi.resetModules();
  provisionedFingerprint = undefined;
  transportConnectMock.mockResolvedValue(undefined);
  transportDisconnectMock.mockResolvedValue(undefined);

  rpcMock.mockImplementation(async (method: string, params?: Record<string, unknown>) => {
    switch (method) {
      case 'bramble.ping':
        return { ok: true };
      case 'bramble.getConfig':
        return { identity: { address: 0x1234 } };
      case 'bramble.getStatus':
      case 'bramble.getAirtime':
        return {};
      case 'bramble.getNeighbors':
        return { neighbors: [] };
      case 'bramble.getRoutes':
        return { routes: [] };
      case 'bramble.getMessages':
        return { messages: [] };
      case 'bramble.getPeerLocations':
        return { peerLocations: [] };
      case 'bramble.getVersion':
        return { supportsDeliveryEventSync: false };
      case 'bramble.setAnchor': {
        // Remember the provisioned pubkey so getAnchorStatus can echo its fp.
        const pub = params?.anchor_pubkey as string;
        provisionedFingerprint = await fpOf(pub);
        return { ok: true };
      }
      case 'bramble.getAnchorStatus':
        return provisionedFingerprint
          ? { anchored: true, anchor_fingerprint: provisionedFingerprint, endorsed: false }
          : { anchored: false, endorsed: false };
      case 'bramble.getIdentity':
        return { address: '00001234', pubkey_hash: 'abcd', ed25519_pub: KAT_NODE_PUB };
      case 'bramble.setEndorsement':
        return { ok: true };
      default:
        return {};
    }
  });
});

// Generous per-waitFor ceiling for a suite that drives real async ceremonies
// (connect + multiple RPC round trips) instead of pre-resolved mocks: under
// CI load a bit of slack here is cheaper than a spurious failure, and every
// wait below is on a real condition, never a fixed delay.
const WAIT_OPTS = { timeout: 5000 };

describe('AnchorSection RPC custody guard', () => {
  it(
    'never sends the anchor seed over RPC through the full enroll ceremony',
    async () => {
      const { useStore } = await import('../../store/index');
      const { connect } = await import('../../store/actions');
      const { AnchorSection } = await import('./AnchorSection');

      useStore.setState({
        connectionState: 'disconnected',
        config: null,
        status: null,
        anchorStatus: null,
      } as never);

      await connect('serial');
      render(<AnchorSection />);

      // 1. Generate + confirm a fresh anchor (seed lands in localStorage only).
      fireEvent.click(screen.getByRole('button', { name: 'Generate anchor' }));
      fireEvent.click(await screen.findByRole('button', { name: 'I have saved this backup' }, WAIT_OPTS));
      const seed = localStorage.getItem(SEED_KEY)!;
      expect(seed).toMatch(/^[0-9a-f]{64}$/);

      // 2. Provision the anchor PUBLIC key to the node.
      fireEvent.click(screen.getByRole('button', { name: 'Provision anchor to this node' }));
      await waitFor(
        () => expect(rpcMock).toHaveBeenCalledWith('bramble.setAnchor', expect.any(Object), undefined),
        WAIT_OPTS,
      );
      // onProvision keeps running past that rpc call (setProvisionSuccess, then
      // refreshStatus's own rpc round trip, then setProvisioning(false) in its
      // finally). Wait for all of it to settle, not just the first rpc call,
      // so every trailing setState lands inside this waitFor's act() window
      // instead of firing after the test moves on (the source of the
      // "not wrapped in act(...)" warnings and the flake under load).
      await waitFor(
        () => expect(screen.getByText('Anchor provisioned to this node.')).toBeInTheDocument(),
        WAIT_OPTS,
      );
      await waitFor(
        () => expect(screen.getByRole('button', { name: 'Provision anchor to this node' })).toBeEnabled(),
        WAIT_OPTS,
      );
      // The node now reports the matching fingerprint; wait for the enroll button
      // to become enabled (mismatch guard cleared).
      await waitFor(
        () => expect(screen.getByRole('button', { name: 'Enroll this node' })).toBeEnabled(),
        WAIT_OPTS,
      );

      // 3. Enroll: sign locally, send only the cert.
      fireEvent.click(screen.getByRole('button', { name: 'Enroll this node' }));
      await waitFor(
        () => expect(rpcMock).toHaveBeenCalledWith('bramble.setEndorsement', expect.any(Object), undefined),
        WAIT_OPTS,
      );
      // Same reasoning as the provision step: let onEnrollLocal's trailing
      // refreshStatus + setEnrolling(false) settle before the plain
      // synchronous assertions below run.
      await waitFor(
        () => expect(screen.getByText('This node is enrolled (permanent endorsement applied).')).toBeInTheDocument(),
        WAIT_OPTS,
      );
      await waitFor(
        () => expect(screen.getByRole('button', { name: 'Enroll this node' })).toBeEnabled(),
        WAIT_OPTS,
      );

      // The seed must appear in NO argument of ANY rpc call.
      const allRpcArgs = JSON.stringify(rpcMock.mock.calls);
      expect(allRpcArgs).not.toContain(seed);

      // Sanity: the ceremony really ran over RPC (public key + cert were sent).
      const { anchorPubFromSeed } = await import('../../utils/anchor');
      expect(rpcMock).toHaveBeenCalledWith(
        'bramble.setAnchor',
        { anchor_pubkey: anchorPubFromSeed(seed) },
        undefined,
      );
      const endorseCall = rpcMock.mock.calls.find((c) => c[0] === 'bramble.setEndorsement');
      expect(endorseCall?.[1]).toMatchObject({ not_after: 'ffffffffffffffff' });
      expect((endorseCall?.[1] as Record<string, string>).endorsement_sig).toMatch(/^[0-9a-f]{128}$/);
    },
    15000,
  );
});
