/** The user's contact names: the key PeerManager writes and the store seeds peerNames from. */
import { loadAddrMap, saveAddrMap } from '../utils/persistedAddrMap';

const CONTACT_NAMES_KEY = 'bramble:peerNames';

export const loadContactNames = (): Map<number, string> => loadAddrMap(CONTACT_NAMES_KEY);

export const saveContactNames = (m: Map<number, string>): void => saveAddrMap(CONTACT_NAMES_KEY, m);
