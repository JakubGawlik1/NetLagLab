# Cleanup and recovery

## Status

The implemented helper lifecycle reaps the directly managed Workload, releases
the root-owned host lock before its final result, and uses RAII for descriptors
and Unix-socket paths. The Supervisor then reaps its launcher and removes the
user control socket.

The standalone Network Environment library now implements and tests the local
transaction ownership contract through a scripted semantic adapter. Preparation
rolls back only its proven prefix; explicit cleanup consumes a prepared or
residual owner, continues across independent roots, aggregates failures, and
returns only unresolved state. Identity-unconfirmed state never regains
deletion authority, and owner destruction performs one bounded no-throw pass.
Its production entry point acquires the root-owned host lock before preflight,
and the runtime retains that lock in either a prepared or residual owner until
cleanup work is gone. The standalone production path can now create and clean
the proven namespace/veth roots together with their contained addresses, link
state, loopback state, and namespace route. The active helper prepares this
local topology before Workload launch and explicitly cleans it after the
Workload is reaped. Session-owned firewall rule creation, NAT, DNS-mount, and
qdisc ownership remain unimplemented. The durable UFW recovery journal and
startup reconciliation described below are implemented.

The remaining production ownership and cleanup rules below are accepted target
design unless a section marks them implemented.

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
- the `netlaglab` nftables table and its contained NAT objects;
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
recovery evidence and does not resolve interrupted-helper recovery or Q47.
Both a prepared environment and a residual cleanup owner own their cleanup
capability; neither borrows a production adapter whose lifetime must be managed
separately by the helper.

The production preparation operation acquires the root-owned global host lock
before preflight and transfers it with the adapter and clock into whichever
owner survives. That owner retains the lock through explicit cleanup, residual
retry, and any destructor safety pass. A clean preparation failure or complete
cleanup releases it. The helper relies on this transaction-owned lock and does
not acquire a duplicate lock.

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

## Persistent firewall recovery (Q47)

A UFW CLI rule can outlive the helper process, so normal in-memory RAII cannot
make it safely Session-scoped. The implementation stores
`/var/lib/netlaglab/recovery.state` in a root-owned directory that is not
group- or world-writable. The regular journal file is root-owned and mode
`0600`; readers reject symlinks, unexpected ownership or mode, malformed
content, and records larger than 4 KiB. Its versioned ASCII record has one
fixed UFW route-rule shape and a random 128-bit transaction ID. That ID is
also the UFW rule comment (`netlaglab-<id>`), so an otherwise identical
administrator rule does not grant deletion authority.

Journal replacement uses an exclusive temporary file in the same directory,
`fsync` on the file, atomic rename, and `fsync` on the containing directory.
The typed phases are `intent`, `applied`, `removing`, and `removed`; updates
must follow the allowed transition graph and keep the same transaction ID.
Intent must be durable before a caller applies the persistent rule. The rule
creation path is not yet implemented.

Startup acquires the existing root host lock and reconciles the journal before
preflight or creation of namespace/veth state. With no journal it continues.
For a live record it inspects `ufw status numbered` and requires exactly one
active route rule whose source, destination, action, interface, and
transaction comment match the fixed record. It persists `removing` before
deletion, deletes by the complete UFW rule including its comment, inspects
again, then persists `removed` before unlinking the journal and syncing the
directory. An absent matching rule is an idempotent cleanup success. Inactive
UFW, missing or untrusted UFW, duplicate markers, a same-source mismatch,
corrupt evidence, or any failed inspection refuses startup and preserves the
journal for diagnosis.

Controlled tests exercise durable phase transitions, interrupted removal,
retry, corruption, symlink refusal, live identity mismatch, duplicate UFW
matches, and lock-before-preflight ordering. No test mutates the host firewall.

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
