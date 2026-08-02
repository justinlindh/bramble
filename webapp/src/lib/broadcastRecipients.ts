// Merge rule for per-recipient broadcast delivery telemetry.
//
// A broadcast message accumulates one BroadcastDeliveryRecipient per node that
// acknowledges it. The same recipient can be reported more than once and out of
// order: live telemetry (bramble.onBroadcastDelivery) and the persisted
// delivery-event replay both feed the same list, so the merge must be
// idempotent and newest-wins. Both callers previously hand-rolled this
// invariant, which is exactly the kind of thing that silently diverges between
// the live and replay paths; this is the single implementation.

import type { BroadcastDeliveryRecipient } from '../types/bramble';

// mergeBroadcastRecipient folds one incoming recipient into recipients,
// deduplicating by addr with the newest deliveredAtMs winning. It returns the
// SAME array reference when the incoming report is stale (older than what is
// already recorded), so callers can cheaply detect a no-op and preserve their
// own object identity.
export function mergeBroadcastRecipient(
  recipients: BroadcastDeliveryRecipient[],
  incoming: BroadcastDeliveryRecipient,
): BroadcastDeliveryRecipient[] {
  const idx = recipients.findIndex(r => r.addr === incoming.addr);
  if (idx < 0) {
    return [...recipients, incoming];
  }
  if (recipients[idx].deliveredAtMs > incoming.deliveredAtMs) {
    return recipients;
  }
  const next = [...recipients];
  next[idx] = { ...recipients[idx], ...incoming };
  return next;
}
