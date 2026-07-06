# Mandatory-attestation Task 1 report: close the bootstrap-quorum race with a bounded per-boot grace

> Note: `.superpowers/sdd/task-t1-report.md` was already taken by the prior
> mandatory-provisioning campaign's Task 1 report (a committed artifact), so
> this report uses `task-att-t1-report.md` to avoid clobbering it.

## Status: DONE

Replaced the unbounded "zero pins -> trust every established peer" hole in the
timesync corroboration quorum with a bounded per-boot grace. After the grace an
unpinned peer NEVER corroborates (NEW-SEC-4 "1.3c" bootstrap-quorum race closed);
within the grace a fresh mesh's established-unpinned peers still corroborate so
timesync can bootstrap (liveness preserved). Only the timesync quorum eligibility
rule changed; transport/relay/routing/first-contact/DM untouched.

## New quorum_eligible signature + bounded-grace rule

```c
bool identity_store_quorum_eligible(const identity_store_t* s, uint32_t address,
                                    bool established, uint32_t now_ms);
```

Rule (identity_store.c):
- `if (!established) return false;`                              // tenure never relaxed
- `if (identity_store_lookup(s, address) != NULL) return true;`  // pinned: always eligible
- `if ((uint32_t)(now_ms - s->boot_ms) < QUORUM_BOOTSTRAP_GRACE_MS) return true;` // bounded boot grace (liveness)
- `return false;`                                                // after grace: unpinned NEVER corroborates (race closed)

The subtraction uses the uint32 wraparound idiom already used for LRU age in the
same file.

## Boot reference mechanism

`identity_store_init` signature changed to `identity_store_init(identity_store_t* s,
uint32_t now_ms)`; it records `now_ms` into a new `uint32_t boot_ms` field on the
store (RAM-only, resets with the pins on reboot). The grace is measured from that
boot reference. Chosen over a separate setter so boot_ms can never be left
uninitialized. Callers updated to pass the current time:
- main/mesh_task.c boot path: `identity_store_init(&s_identity_pins, now_ms())`
- simulator/gosim/bridge.c node-join init: `identity_store_init(&ext->ident_pins, now_ms)`
- test/test_identity_store.c, test/test_ident_relay_gate.c: pass explicit clock values

The quorum caller at main/mesh_task.c already had the beacon-handling time `t`
in scope and now passes it through.

## QUORUM_BOOTSTRAP_GRACE_MS value + tradeoff

`#define QUORUM_BOOTSTRAP_GRACE_MS 300000u` (5 minutes), defined in
components/identity/include/identity_store.h with the tradeoff comment: a LONGER
grace gives more liveness margin on large/slow meshes where propagating and
verifying the first attestations takes longer, at the cost of a wider window in
which an unattested/Sybil node could skew the clock; a SHORTER grace tightens the
exposure but risks a slow mesh failing to bootstrap timesync. Because every node
attests on boot (immediate) + every 15 min, genuine pins normally arrive within
seconds-to-minutes, so the gate has already tightened to pinned-only well before
the grace expires. The grace is a liveness backstop, not the normal path.

## Early-exit refinement: NOT done (deliberately deferred)

The optional "end the grace early once >= N pins exist" refinement was NOT
implemented. Rationale: liveness is the sacred constraint, and the time bound
alone fully closes the security assertion (after the grace, unpinned peers are
excluded regardless of pin count). Ending the grace early introduces a narrow
edge case (a node holding >= N pins but relying on unpinned-within-grace
corroboration would drop those sources sooner) on the path the brief calls
sacred, for only a marginal shrink of an already-bounded window. Shipping the
time bound alone keeps the liveness path maximally simple. The refinement can be
layered on later as `count(s) >= THRESHOLD` in the unpinned-within-grace branch
without changing the signature.

## Tests (test/test_identity_store.c) - both key assertions non-vacuous

Boot reference `QBOOT = 1000` in all four quorum tests; they share one zero-pin
store so liveness and security are proven against the same state.

- test_quorum_within_grace_unpinned_is_eligible (LIVENESS): zero pins, established
  unpinned peer is eligible at boot AND at `boot + GRACE - 1`. Asserts TRUE. If
  the grace were absent (unpinned always excluded) this fails.
- test_quorum_after_grace_unpinned_excluded_even_with_zero_pins (SECURITY, the
  race closed): zero pins, established unpinned peer is NOT eligible at exactly
  `boot + GRACE` and at `boot + GRACE + 60s`. Asserts FALSE. If the old unbounded
  "zero pins -> trust" behavior remained this fails. This is the security
  assertion.
- test_quorum_pinned_peer_eligible_within_and_after_grace: a pinned+established
  peer is eligible both inside and long after the grace (TRUE), while an unpinned
  peer at a different address is excluded after the grace (FALSE).
- test_quorum_unestablished_never_eligible: an unestablished peer is ineligible
  inside grace, after grace, and even when pinned (FALSE) - tenure never relaxed.

The two prior tests (no_pins_falls_back / with_pins_requires_pinned) were replaced
by the above.

gosim does not model timesync at all (confirmed: no timesync/network_time
references under simulator/gosim), so there is no timesync scenario to extend and
a full gosim liveness scenario would be out-of-scope engineering for this quorum
change. The liveness path is directly and non-vacuously covered by the host
pure-function test above. gosim was updated for the new init signature and stays
green.

## CI (all green)

- Host tests: `bash test/run_all_tests.sh` -> 101 suites, 0 failures (new quorum
  tests pass).
- gosim: `cd simulator/gosim && go test -count=1 ./...` -> ok.
- clang-format v14 (runner image): PASS, 379 files clean.
- cppcheck (runner image): clean, exit 0.
- check-rpc-contract: OK, 54 methods match.
- Board build: `source esp-idf/export.sh && make ci-quality-board-build` ->
  ESP32-S3 image built and signed, 52% app partition free.

## Concerns

None blocking. Note only: the early-exit refinement is deferred (rationale above);
if the campaign later wants the tightest exposure window it is a one-line add in
the unpinned-within-grace branch plus one test.
