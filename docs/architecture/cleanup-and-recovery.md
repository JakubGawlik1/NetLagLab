# Cleanup and recovery

## Status

The helper prepares the production Network Environment before Workload launch
and retains its prepared owner through Workload reaping and cleanup. That owner
tracks the proven namespace/veth roots, routes, DNS view, source-scoped NAT,
and any approved firewall change. It retains the root-owned host lock until
cleanup is complete. Identity-unconfirmed state never regains deletion
authority, and owner destruction makes one bounded no-throw pass.

UFW's persistent rule is additionally recorded in a durable recovery journal.
Reconciliation runs under the host lock before fixed-name preflight. The
journal protects persistent firewall/NAT ownership across helper interruption;
the in-memory owner continues to track namespace and veth cleanup.

## Ownership rule

NetLagLab removes only resources that the active Session created and can prove
it owns. Matching a familiar name is not proof of ownership.

The helper owns, when created for the Session:

- the global host lock while held;
- the named network namespace reference;
- `nll-host` and `nll-app`;
- Session addresses and routes;
- qdiscs and the applied Network Profile state;
- the Session mount namespace and DNS snapshot/mount;
- the uniquely named `netlaglab_<token>` nftables table and its contained NAT
  objects;
- an explicitly approved firewall exception;
- the directly managed Workload process;
- helper-side protocol descriptors and temporary privileged files.

The Supervisor owns its per-user lock, runtime-directory descriptors,
`control.sock`, the `helper.sock` client endpoint, Controller connection, and
the `sudo` launcher process it started.

The Controller owns only its descriptors and local presentation state.

## Transactional rollback

Startup records which resources were actually created. If a later step fails,
the helper rolls back only that recorded set, normally in dependency-reversing
order. Cleanup operations are idempotent so retrying a partially completed
cleanup does not remove unrelated state or turn an already-absent owned object
into a new failure.

A setup failure never discards ownership evidence for a resource that may
remain. Complete rollback returns only the setup failure. Incomplete rollback
also returns an opaque cleanup owner containing the remaining in-memory ledger;
the helper retains it and can retry exact cleanup, but it does not describe the
partial setup as an active Network Environment. This owner is not durable
recovery evidence for namespace or veth state.
Both a prepared environment and a residual cleanup owner own their cleanup
capability; neither borrows a production adapter whose lifetime must be managed
separately by the helper.

The production preparation operation acquires the root-owned global host lock
before preflight and transfers it with the adapter and clock into whichever
owner survives. That owner retains the lock through explicit cleanup, residual
retry, and any destructor safety pass. A clean preparation failure or complete
cleanup releases it. The helper enters the retained network namespace only in
the forked Workload child and does not reacquire the host lock while this owner
holds it.

Explicit cleanup consumes the prepared environment or residual cleanup owner
and performs exactly one dependency-aware pass. Complete cleanup returns no new
owner. Incomplete cleanup returns the ordered failures plus a new residual owner
containing only unattempted, still-proven, or identity-unconfirmed state. A
caller may explicitly consume that residual owner for another one-pass attempt
with a fresh 30-second cleanup budget. The module does not hide an automatic
retry loop inside preparation or cleanup, and a retry never repeats a target
already proven absent or removed.

Identity-unconfirmed state is carried forward for diagnostics and future
verified recovery but cannot regain deletion authority in this checkpoint.
Idempotence describes the underlying cleanup operations and retry through the
returned residual owner; it does not make a consumed C++ owner reusable.

Destruction is a no-throw safety net, not the normal cleanup interface. If a
prepared environment or residual cleanup owner is destroyed while it still
contains proven resources, its destructor makes one bounded best-effort cleanup
pass. It never deletes identity-unconfirmed state. A destructor result cannot
establish a successful Session, erase an already reported cleanup failure, or
substitute for the explicit consuming cleanup path; its only purpose is to
reduce residue after an omitted or abandoned explicit cleanup.

The ledger is typed and dependency-aware rather than a stack of arbitrary
inverse callbacks. It distinguishes independently removable resource roots,
such as the named namespace and veth pair, from completed configuration
milestones on those resources, such as moving the peer, assigning addresses,
bringing links up, and adding the namespace route. Cleanup is explicit module
logic: deleting an owned link or namespace also removes the configuration it
contains, while future independent resources require their own ledger entries
and cleanup operations.

Names locate resources but do not prove identity. Immediately after creation,
the ledger records the named namespace device/inode identity and the veth
interface indices and peer relationship in their respective namespaces.
Cleanup resolves each expected name again and compares those identifiers before
deletion. An already-absent owned resource is an idempotent success; a different
resource under the same name is left untouched and makes cleanup fail.

The internal port represents those facts as opaque, move-only ownership proofs.
It has distinct proof types for the exact namespace, the host-side veth pair,
and the pair after its Session endpoint has been verified in that exact
namespace. An identity-unconfirmed residual is a separate type rather than a
flag on a proven resource. Cleanup accepts only proven ownership; no operation
can delete an unconfirmed residual by name. The namespace proof retains an
opaque exact handle but exposes no raw descriptor, namespace path, or `setns()`
mechanism to the transaction coordinator.

A resource-creating or peer-placement mutation is not committed until the
adapter captures its new ownership identity. If identity capture fails after
the mutation may have succeeded, the adapter records provisional ownership and
immediately reconciles or rolls back the operation. If it cannot prove the old
state, new state, or absence, the residual cleanup owner marks the resource
identity as unconfirmed. Address, link-state, loopback, and route operations do
not create new identity; positive completion records only their configuration
milestone. Later code must not delete an object based only on a matching name;
the Session fails with infrastructure result `125` and leaves unconfirmed state
for a future verified recovery mechanism.

Cleanup does not stop merely because its first operation failed. While its
budget remains, it attempts every independent target whose identity is still
proven, aggregates the failures, and retains unattempted or unresolved resources
in the residual cleanup owner. After the budget expires it starts no new
mutation. A child already started must still be terminated and reaped after its
deadline, even if that finalization exceeds the nominal cleanup budget.

The local environment cleanup first attempts the owned veth pair, then the
verified named namespace, and closes its namespace descriptor last. An identity
mismatch is a failure and blocks deletion of that object, but it does not block
cleanup of an independent object whose identity still matches. A removal command
whose status is ambiguous is reconciled semantically: proven absence is an
idempotent success, a matching remaining identity stays eligible for retry, and
a different identity is left untouched. After successful namespace removal,
cleanup rechecks a previously unresolved veth once because destroying the
namespace may also have removed its peer and therefore the pair.
The descriptor close is the last namespace-ownership action. When named
namespace removal succeeds with a veth proof still unresolved, consuming the
namespace proof closes that descriptor before the one read-only veth recheck;
dropping the final owned namespace reference is what allows kernel namespace
destruction to remove the peer. The recheck grants no new deletion authority.

The same principle applies to a live Network Profile mutation: restore the
last confirmed state after a partial failure. If the helper cannot establish
that rollback succeeded, the applied state is unknown and the Session fails.

Preflight never adopts or removes a colliding pre-existing resource. A later
recovery run may touch stale state only when durable evidence identifies it as
NetLagLab-owned.

## Normal completion

Session completion requires:

1. stop or observe and reap the directly managed Workload;
2. stop further profile and Controller operations;
3. remove shaping and privileged network resources;
4. remove the DNS mount/snapshot and namespace-owned references;
5. remove the exact owned firewall and NAT resources;
6. release the global host lock only after cleanup has completed;
7. send the final helper result and exit;
8. let the Supervisor reap its `sudo` launcher;
9. remove user-runtime sockets and release the per-user lock.

The exact internal cleanup order may be refined to respect real dependencies,
but the externally visible contract is fixed: a Session is successful only
when every owned resource has been cleaned up.

Failure to remove any actually owned resource changes the Session result to
infrastructure result `125`, even if the Workload had already produced a normal
result. Diagnostics preserve both facts.

## Supervisor and helper failure

- Controller loss has no cleanup authority and does not end the Session.
- Supervisor loss makes the helper stop and reap the directly managed
  Workload, clean owned resources, and exit.
- Unexpected helper death kills the directly managed Workload through
  `PR_SET_PDEATHSIG(SIGKILL)`, but the dead helper cannot finish cleanup. Any
  persistent or host-visible remainder must be handled by a later verified
  recovery path.
- Helper loss, incomplete terminal exchange, failed reaping, and unknown
  resource state are infrastructure failures.

## Descendants and namespace references

NetLagLab does not own Workload descendants. A descendant can retain a
reference to the Session's network namespace after NetLagLab has removed the
namespace name and veth pair. The kernel namespace may then remain alive as an
anonymous object until that descendant exits.

Cleanup still succeeds when all NetLagLab-owned names, mounts, files, devices,
rules, and references are gone. This is not a NetLagLab resource leak. The
project must not claim that successful `ip netns del` proves the kernel object
has ceased to exist.

Strictly discovering and terminating every process that retains the namespace
would require a different ownership model, likely including cgroups. That
alternative was rejected for the MVP because descendants are explicitly not
owned.

## Persistent UFW recovery

The helper stores `/var/lib/netlaglab/recovery.state` in a root-owned `0700`
directory. The bounded version-1 record stores the backend, `intent`, `applied`,
or `removing` phase, and a random token. Replacement writes a mode-`0600`
temporary file, synchronizes it, atomically renames it, and synchronizes the
directory. Removing the record also synchronizes the directory.

Intent is durable before a UFW mutation. Reconciliation reads the record under
the host lock before new network resources are created and removes only state
that matches the journal token and live identity checks. Corrupt or mismatched
evidence fails closed. Controlled journal tests cover replacement, phases,
corruption, and identity mismatch; real privileged host mutation remains
outside the routine test suite.

## Verification obligations

Future implementation must exercise at least:

- failure after each privileged setup step and cleanup of the already-created
  prefix;
- natural exit, requested stop, Supervisor loss, and helper/protocol failure;
- idempotent retry after partial cleanup;
- refusal to remove an unowned name or rule collision;
- launcher and Workload reaping;
- cleanup failure overriding a known Workload result;
- separately authorized recovery of an interrupted persistent firewall rule.
