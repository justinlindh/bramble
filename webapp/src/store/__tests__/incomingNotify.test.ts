import { beforeEach, afterEach, describe, expect, it, vi, type Mock } from 'vitest';
import { useStore } from '../index';
import { normalizeIncomingRealtimeMessage } from '../actions';

// The native message-notification bridge fires for incoming messages in the
// Android shell, but not for self-messages and not for the conversation the
// user is currently looking at. Test the observable effect:
// window.brambleAndroidNotify.onMessage is called with the right payload.

// handleIncomingMessage is the exported subscription handler wired into
// connect()'s onMessage subscription; to test the notify branch in isolation
// we re-import actions with the Android shell stubbed and call it directly.

vi.mock('../messageDb', () => ({
  messageDb: { saveMessage: vi.fn(async () => {}), open: vi.fn(async () => {}), getMessages: vi.fn(async () => []), saveMessages: vi.fn(async () => {}), updateMessageStatus: vi.fn(async () => {}) },
}));

let onMessage: Mock<(payloadJson: string) => void>;
let clearConversation: Mock<(conversationId: string) => void>;

beforeEach(() => {
  onMessage = vi.fn<(payloadJson: string) => void>();
  clearConversation = vi.fn<(conversationId: string) => void>();
  vi.stubGlobal('brambleAndroid', true);
  window.brambleAndroidNotify = { onMessage, clearConversation };
  useStore.setState({
    config: { identity: { address: 0xaa11 } } as any,
    peerNames: new Map([[0xdeadbeef, 'Node A']]),
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
  const { handleIncomingMessage } = await import('../actions');
  handleIncomingMessage(params);
}

describe('incoming message native notification', () => {
  it('notifies for an incoming DM with the peer name as title and sender', async () => {
    await deliver({ from: 'DEADBEEF', to: 'AA11', text: 'ping', msgId: 'm1' });
    expect(onMessage).toHaveBeenCalledTimes(1);
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({ conversationId: 'dm:3735928559', sender: 'Node A', conversationTitle: 'Node A', text: 'ping' });
  });

  it('does not notify for a message from this node', async () => {
    await deliver({ from: 'AA11', to: 'DEADBEEF', text: 'my echo', msgId: 'm2' });
    expect(onMessage).not.toHaveBeenCalled();
  });

  it('does not notify for the conversation currently open and visible', async () => {
    useStore.setState({ activeConversationId: 'dm:3735928559' } as any);
    await deliver({ from: 'DEADBEEF', to: 'AA11', text: 'looking at this', msgId: 'm3' });
    expect(onMessage).not.toHaveBeenCalled();
  });

  it('notifies for the open conversation when the app is not visible', async () => {
    useStore.setState({ activeConversationId: 'dm:3735928559' } as any);
    Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true });
    await deliver({ from: 'DEADBEEF', to: 'AA11', text: 'backgrounded', msgId: 'm4' });
    expect(onMessage).toHaveBeenCalledTimes(1);
  });

  it('titles a channel message with the channel name', async () => {
    useStore.setState({ config: { identity: { address: 0xaa11 }, channels: [{ index: 0, name: 'Mesh Net' }] } as any } as any);
    await deliver({ from: 'DEADBEEF', text: 'net check', channel: 0, msgId: 'm5' });
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({ conversationId: 'ch:0', conversationTitle: 'Mesh Net', sender: 'Node A' });
  });

  it('titles an unnamed channel with the same ch- fallback the chat header uses', async () => {
    useStore.setState({ config: { identity: { address: 0xaa11 } } as any } as any);
    await deliver({ from: 'DEADBEEF', text: 'net check', channel: 2, msgId: 'm9' });
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({ conversationId: 'ch:2', conversationTitle: 'ch-2', sender: 'Node A' });
  });

  it('learns the sender name from fromName so an unknown peer never titles as hex', async () => {
    useStore.setState({ peerNames: new Map() } as any);
    await deliver({ from: 'AABBCC04', to: 'AA11', text: 'hi', msgId: 'm7', fromName: 'Northside' });
    expect(onMessage).toHaveBeenCalledTimes(1);
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({ sender: 'Northside', conversationTitle: 'Northside' });
    expect(useStore.getState().peerNames.get(0xaabbcc04)).toBe('Northside');
  });

  // The only user-visible surface of the DM/sender label consolidation: with
  // no stored name and no fromName, the sender falls back to formatAddr0x,
  // which zero-pads to eight hex digits like every other address in the UI.
  // 0xBEEF is chosen because the unpadded form ("0xBEEF") differs from the
  // padded one, so this fails if the fallback regresses to a raw toString(16).
  it('falls back to the zero-padded hex address when the sender has no name', async () => {
    useStore.setState({ peerNames: new Map() } as any);
    await deliver({ from: '0000BEEF', to: 'AA11', text: 'who dis', msgId: 'm8' });
    expect(onMessage).toHaveBeenCalledTimes(1);
    const payload = JSON.parse(onMessage.mock.calls[0][0]);
    expect(payload).toMatchObject({
      conversationId: 'dm:48879',
      sender: '0x0000BEEF',
      conversationTitle: '0x0000BEEF',
      text: 'who dis',
    });
  });

  it('is a no-op outside the Android shell', async () => {
    vi.stubGlobal('brambleAndroid', undefined);
    await deliver({ from: 'DEADBEEF', to: 'AA11', text: 'web', msgId: 'm6' });
    expect(onMessage).not.toHaveBeenCalled();
  });
});

// Silence unused-import lint: normalizeIncomingRealtimeMessage is part of the
// tested module surface.
void normalizeIncomingRealtimeMessage;
