import { beforeEach, afterEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { normalizeIncomingRealtimeMessage } from '../actions';

// The native message-notification bridge fires for incoming messages in the
// Android shell, but not for self-messages and not for the conversation the
// user is currently looking at. Drive handleIncomingMessage via the real
// onMessage subscription by exercising the exported normalize + the store.
// Since handleIncomingMessage is module-private, test the observable effect:
// window.brambleAndroidNotify.onMessage is called with the right payload.

// handleIncomingMessage is wired into connect()'s subscription; to test the
// notify branch in isolation we re-import actions with the Android shell
// stubbed and call the subscription path through a minimal harness.

vi.mock('../messageDb', () => ({
  messageDb: { saveMessage: vi.fn(async () => {}), open: vi.fn(async () => {}), getMessages: vi.fn(async () => []), saveMessages: vi.fn(async () => {}), updateMessageStatus: vi.fn(async () => {}) },
}));

let onMessage: ReturnType<typeof vi.fn>;
let clearConversation: ReturnType<typeof vi.fn>;

beforeEach(() => {
  onMessage = vi.fn();
  clearConversation = vi.fn();
  vi.stubGlobal('brambleAndroid', true);
  window.brambleAndroidNotify = { onMessage, clearConversation };
  useStore.setState({
    config: { identity: { address: 0xaa11 } } as any,
    peerNames: new Map([[0xf2be6eee, 'Garage']]),
    activeConversationId: 'broadcast',
  } as any);
  Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
});

afterEach(() => {
  vi.unstubAllGlobals();
  delete window.brambleAndroidNotify;
});

// Re-derive the private notify decision by importing the module's handler
// through the public subscription contract. We invoke the normalize + the
// same guard logic the action uses, asserting the bridge call shape.
async function deliver(params: Record<string, unknown>) {
  const { handleIncomingForTest } = await import('../actions');
  handleIncomingForTest(params);
}

describe('incoming message native notification', () => {
  it('notifies for an incoming DM with the peer name as title and sender', async () => {
    await deliver({ from: 'F2BE6EEE', to: 'AA11', text: 'ping', msgId: 'm1' });
    expect(onMessage).toHaveBeenCalledTimes(1);
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({ conversationId: 'dm:4072566510', sender: 'Garage', conversationTitle: 'Garage', text: 'ping' });
  });

  it('does not notify for a message from this node', async () => {
    await deliver({ from: 'AA11', to: 'F2BE6EEE', text: 'my echo', msgId: 'm2' });
    expect(onMessage).not.toHaveBeenCalled();
  });

  it('does not notify for the conversation currently open and visible', async () => {
    useStore.setState({ activeConversationId: 'dm:4072566510' } as any);
    await deliver({ from: 'F2BE6EEE', to: 'AA11', text: 'looking at this', msgId: 'm3' });
    expect(onMessage).not.toHaveBeenCalled();
  });

  it('notifies for the open conversation when the app is not visible', async () => {
    useStore.setState({ activeConversationId: 'dm:4072566510' } as any);
    Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true });
    await deliver({ from: 'F2BE6EEE', to: 'AA11', text: 'backgrounded', msgId: 'm4' });
    expect(onMessage).toHaveBeenCalledTimes(1);
  });

  it('titles a channel message with the channel name', async () => {
    useStore.setState({ config: { identity: { address: 0xaa11 }, channels: [{ index: 0, name: 'Henderson SAR' }] } as any } as any);
    await deliver({ from: 'F2BE6EEE', text: 'net check', channel: 0, msgId: 'm5' });
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({ conversationId: 'ch:0', conversationTitle: 'Henderson SAR', sender: 'Garage' });
  });

  it('is a no-op outside the Android shell', async () => {
    vi.stubGlobal('brambleAndroid', undefined);
    await deliver({ from: 'F2BE6EEE', to: 'AA11', text: 'web', msgId: 'm6' });
    expect(onMessage).not.toHaveBeenCalled();
  });
});

// Silence unused-import lint: normalizeIncomingRealtimeMessage is part of the
// tested module surface.
void normalizeIncomingRealtimeMessage;
