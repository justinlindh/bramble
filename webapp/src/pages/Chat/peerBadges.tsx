import { IconLock, IconWarning } from '../../components/Icons';
import { STATUS_COLORS, STATUS_LABELS, type PeerStatus } from '../../hooks/usePeer';
import type { PeerVerification } from '../../types/bramble';

// The presence dot binds a status to its color and its label, so a caller
// cannot paint the color while hand-writing a title that drifts from it: the
// same hazard StatusDot.tsx guards for the connection-state dot. Layout differs
// per call site (inline in the DM header, flex-pushed in the DM list), so the
// class name is the caller's.
export function PeerStatusDot({ status, className }: { status: PeerStatus; className: string }) {
  return (
    <span className={className} style={{ background: STATUS_COLORS[status] }} title={STATUS_LABELS[status]} />
  );
}

// One owner for the SAS-verification glyph: the key-changed / verified branch
// and its titles were hand-duplicated in the DM header and the DM list, which
// differ only in icon size and CSS class.
export function PeerVerificationBadge({
  verification,
  okClassName,
  warnClassName,
  size,
}: {
  verification: PeerVerification | undefined;
  okClassName: string;
  warnClassName: string;
  size: number;
}) {
  if (verification?.keyChanged) {
    return (
      <span className={warnClassName} title="Safety number changed">
        <IconWarning size={size} />
      </span>
    );
  }
  if (verification?.verified) {
    return (
      <span className={okClassName} title="Verified">
        <IconLock size={size} />
      </span>
    );
  }
  return null;
}
