# Cleanup and recovery

## Status

The implemented helper lifecycle reaps the directly managed Workload, releases
the root-owned host lock before its final result, and uses RAII for descriptors
and Unix-socket paths. The Supervisor then reaps its launcher and removes the
user control socket. There are no privileged namespace, veth, route, NAT,
firewall, DNS-mount, or qdisc resources to clean up yet.

The ownership and cleanup rules below are accepted target design. A durable
recovery journal is accepted in principle for persistent firewall changes, but
its exact design remains deliberately open.

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
make it safely Session-scoped. The accepted direction is a durable,
root-owned recovery journal used under the same global host lock as normal
setup and cleanup.

The intended safety properties are:

- record intent before performing a persistent mutation;
- record that the exact mutation was successfully applied;
- remove only the exact recorded resource during normal cleanup;
- record successful removal before discarding recovery evidence;
- reconcile an interrupted prior Session before creating new host resources.

The following details are not decided and must not be invented during
implementation:

- final path (a location under `/var/lib/netlaglab/` was only an example);
- file/schema format and versioning;
- atomic write, flush, ownership, permission, and corruption behavior;
- exact intent/applied/removing/removed state transitions;
- reconciliation rules for host state that differs from the journal;
- retention and user-facing diagnostics after failed recovery.

Because this is persistent privileged state, its design requires a separate
high-risk review and may merit an ADR once the trade-off is settled.

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
