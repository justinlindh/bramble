import type { ReactNode } from 'react';

interface EscapeDialogProps {
  /** Accessible name for the dialog (role="dialog" aria-label). */
  ariaLabel: string;
  onClose: () => void;
  children: ReactNode;
  /** Caller's own backdrop CSS module class (positioning, overlay color, z-index). */
  backdropClassName: string;
  /** Caller's own dialog-panel CSS module class. */
  dialogClassName: string;
}

/**
 * Shared backdrop wrapper for every modal dialog in the app: dialog role and
 * aria-modal, backdrop-click closes, Escape closes and stops there (see the
 * propagation note inside the handler) so a stack of overlays only ever
 * dismisses the topmost one. Callers keep their own CSS module for exact
 * positioning/sizing; this component only owns the shared interaction
 * contract, not the styling.
 *
 * Originally file-local to ConversationList.tsx (PR #229); promoted here so
 * every dialog surface in the app dismisses the same way.
 */
export function EscapeDialog({ ariaLabel, onClose, children, backdropClassName, dialogClassName }: EscapeDialogProps) {
  return (
    <div
      className={backdropClassName}
      onClick={onClose}
      onKeyDown={e => {
        if (e.key === 'Escape') {
          // Stop the NATIVE event too (React's synthetic stopPropagation
          // does both): a topmost overlay (e.g. the mobile sidebar) may have
          // its own Escape listener on window, and Escape must dismiss only
          // this dialog, not also whatever is stacked underneath it.
          e.stopPropagation();
          onClose();
        }
      }}
      role="dialog"
      aria-modal="true"
      aria-label={ariaLabel}
    >
      <div className={dialogClassName} onClick={e => e.stopPropagation()}>
        {children}
      </div>
    </div>
  );
}
