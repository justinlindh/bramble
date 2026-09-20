/** The user's contact names in localStorage. The store is their only reader and writer. */
import { loadAddrMap, saveAddrMap } from '../utils/persistedAddrMap';

const CONTACT_NAMES_KEY = 'bramble:peerNames';

export const loadContactNames = (): Map<number, string> => loadAddrMap(CONTACT_NAMES_KEY);

export const saveContactNames = (m: Map<number, string>): void => saveAddrMap(CONTACT_NAMES_KEY, m);
