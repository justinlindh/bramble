import { AddressLabel } from './AddressLabel';
import { formatAddrShort } from '../utils/address';

interface NamedAddressProps {
  addr: number;
  name?: string;
  subClassName?: string;
}

/**
 * A node's address shown as its name, optionally with the short hex as a
 * subtitle. When no name is known, AddressLabel falls back to the short hex on
 * its own and no subtitle is rendered. Callers give the name once instead of
 * repeating the short={!name} / {name && <sub>} coupling at each site. Pass
 * subClassName to render the short-hex subtitle beneath a known name; omit it
 * for the name (or hex fallback) alone.
 */
export function NamedAddress({ addr, name, subClassName }: NamedAddressProps) {
  return (
    <>
      <AddressLabel addr={addr} name={name} short={!name} />
      {name && subClassName && <span className={subClassName}>{formatAddrShort(addr)}</span>}
    </>
  );
}
