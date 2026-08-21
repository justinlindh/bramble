// Location sharing publishes only to configured targets, so whether a node is
// actually sending location is decided by "has at least one enabled target",
// never by the policy switch alone. Both the map summary and the config preview
// derive their affirmative copy from this one predicate, so they cannot
// disagree about whether the node is publishing.

/** A share target (contact rule or channel target) as far as the enabled count
 * is concerned. `enabled` is optional so a wire row that omits it still counts,
 * matching the rest of the UI: only an explicit `false` opts a target out. */
type ShareTarget = { enabled?: boolean };

const isEnabled = (t: ShareTarget): boolean => t.enabled !== false;

/** Number of enabled location share targets across contacts and channels. */
export function countEnabledShareTargets(
  contactRules: readonly ShareTarget[],
  channelTargets: readonly ShareTarget[],
): number {
  return contactRules.filter(isEnabled).length + channelTargets.filter(isEnabled).length;
}
