---
status: accepted
---

# Prepare the Network Environment through one transactional boundary

The helper requests Network Environment preparation through one operation
rather than driving public preflight, creation, configuration, commit, and
rollback phases. Success returns a move-only prepared environment that owns its
resource ledger, exact namespace handle, and explicit cleanup operation;
failure identifies the setup stage and carries an opaque residual cleanup owner
only when rollback could not prove that every created resource was removed. We
chose this over helper-driven phases or loose utility functions so callers
cannot publish a partial environment, skip rollback, or split ownership across
the helper and the environment module.

The boundary reports failure using orthogonal typed stage and cause values.
Stable presentation is derived from those values, while bounded iproute2
standard error remains local diagnostic context and never becomes control flow
or a wire-level contract.

A failed preparation preserves the primary setup failure, the ordered rollback
failures, and an optional residual cleanup owner as separate facts. Incomplete
cleanup is a summary of that combined result rather than a replacement for the
original failure. Rollback has its own deadline; it continues across independent
failures while budget remains and retains unattempted state after expiry.

The prepared environment and any residual cleanup owner also own the
capability used for later cleanup rather than borrowing an adapter from the
caller. Their exact namespace handle and ownership proofs are move-only and
opaque, so the helper cannot bypass the transaction with a raw descriptor or
name-based deletion.

Concrete move-only owners hide their implementation. A private runtime holding
the semantic adapter, monotonic clock, and acquired global host lock moves from
preparation into the prepared or residual owner and across retries. The public
interface exposes neither those dependencies nor the ledger. Later helper
integration replaces its current manual lock acquisition rather than nesting a
second acquisition; this standalone checkpoint does not modify the helper.

Explicit cleanup consumes its owner and makes exactly one pass. Success returns
no owner; failure returns the ordered cleanup failures and a new residual owner
containing only unresolved state. Each explicit residual retry is another
single pass with a fresh bounded budget. There is no hidden retry loop, and
identity-unconfirmed state cannot acquire deletion authority in this
checkpoint.

As a final RAII safety net, destruction of either owner performs one bounded,
no-throw best-effort pass over still-proven resources. It never touches
identity-unconfirmed state and cannot make the Session successful or hide a
previously reported failure. Correct control flow therefore still requires the
explicit consuming cleanup operation.
